/*
 * Copyright 2025 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "offline_commands.h"

#include "../alarms/manager.h"
#include "../util/logging.h"

#include <pebble.h>
#include <stdlib.h>
#include <string.h>

// Tunable limits. Quick commands are short; anything longer is assumed to be
// a real assistant query and falls through to the service.
#define OC_MAX_INPUT 160
#define OC_MAX_TOKENS 32

// ---------------------------------------------------------------------------
// Tokenisation
// ---------------------------------------------------------------------------

typedef struct {
  const char *tokens[OC_MAX_TOKENS];
  int count;
  char buffer[OC_MAX_INPUT];
} TokenList;

// Lowercases the input into tl->buffer and splits it into alphanumeric tokens.
// A token containing a ':' (e.g. "7:30") is kept intact so it can be split
// later. Returns false if the input is too long to be a quick command.
static bool prv_tokenize(const char *input, TokenList *tl) {
  size_t len = strlen(input);
  if (len == 0 || len >= OC_MAX_INPUT) {
    return false;
  }
  // Lowercase, dropping characters that never appear in our grammar (keeping
  // letters, digits, ':' and the apostrophe for "o'clock").
  int w = 0;
  for (size_t i = 0; i < len; ++i) {
    char c = input[i];
    if (c >= 'A' && c <= 'Z') {
      c = c - 'A' + 'a';
    }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '\'' || c == ' ') {
      tl->buffer[w++] = c;
    } else {
      tl->buffer[w++] = ' ';
    }
  }
  tl->buffer[w] = '\0';

  tl->count = 0;
  char *p = tl->buffer;
  while (*p) {
    while (*p == ' ') {
      *p++ = '\0';
    }
    if (!*p) {
      break;
    }
    if (tl->count >= OC_MAX_TOKENS) {
      return false;  // too many words for a quick command
    }
    tl->tokens[tl->count++] = p;
    while (*p && *p != ' ') {
      ++p;
    }
  }
  return tl->count > 0;
}

static bool prv_eq(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

// ---------------------------------------------------------------------------
// Number-word parsing
// ---------------------------------------------------------------------------

// Returns the value of a single number word, or -1 if not a number word.
static int prv_word_value(const char *t) {
  static const char *ones[] = {"zero", "one", "two", "three", "four", "five",
                               "six", "seven", "eight", "nine", "ten",
                               "eleven", "twelve", "thirteen", "fourteen",
                               "fifteen", "sixteen", "seventeen", "eighteen",
                               "nineteen"};
  for (int i = 0; i < 20; ++i) {
    if (prv_eq(t, ones[i])) {
      return i;
    }
  }
  if (prv_eq(t, "twenty")) return 20;
  if (prv_eq(t, "thirty")) return 30;
  if (prv_eq(t, "forty")) return 40;
  if (prv_eq(t, "fifty")) return 50;
  if (prv_eq(t, "sixty")) return 60;
  return -1;
}

static bool prv_is_digits(const char *t) {
  if (!*t) {
    return false;
  }
  for (const char *p = t; *p; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
  }
  return true;
}

// Parses a (possibly multi-word) cardinal number starting at *idx, advancing
// *idx past the consumed tokens. Handles digits ("15"), single words ("five"),
// and tens+units ("twenty five"). Returns the value, or -1 if no number is
// present at *idx.
static int prv_parse_number(const TokenList *tl, int *idx) {
  if (*idx >= tl->count) {
    return -1;
  }
  const char *t = tl->tokens[*idx];
  if (prv_is_digits(t)) {
    (*idx)++;
    return atoi(t);
  }
  // "a"/"an" as in "a timer for an hour" means one.
  if (prv_eq(t, "a") || prv_eq(t, "an")) {
    (*idx)++;
    return 1;
  }
  int v = prv_word_value(t);
  if (v < 0) {
    return -1;
  }
  (*idx)++;
  // tens + units, e.g. "twenty five".
  if (v >= 20 && v % 10 == 0 && *idx < tl->count) {
    int units = prv_word_value(tl->tokens[*idx]);
    if (units > 0 && units < 10) {
      v += units;
      (*idx)++;
    }
  }
  return v;
}

static bool prv_is_unit_second(const char *t) {
  return prv_eq(t, "second") || prv_eq(t, "seconds") || prv_eq(t, "sec") || prv_eq(t, "secs");
}
static bool prv_is_unit_minute(const char *t) {
  return prv_eq(t, "minute") || prv_eq(t, "minutes") || prv_eq(t, "min") || prv_eq(t, "mins");
}
static bool prv_is_unit_hour(const char *t) {
  return prv_eq(t, "hour") || prv_eq(t, "hours") || prv_eq(t, "hr") || prv_eq(t, "hrs");
}

// ---------------------------------------------------------------------------
// Duration parsing (for timers)
// ---------------------------------------------------------------------------

// Parses a duration like "5 minutes", "1 hour 30 minutes", "an hour and a
// half", "half an hour" starting at *idx. Returns total seconds, or -1 if no
// valid duration is found. On success *idx points just past the duration.
static int prv_parse_duration(const TokenList *tl, int *idx) {
  long total = 0;
  bool found = false;
  int i = *idx;

  while (i < tl->count) {
    // Skip connective words.
    if (prv_eq(tl->tokens[i], "and")) {
      i++;
      continue;
    }
    // "half a/an <unit>" -> 0.5 unit.
    if (prv_eq(tl->tokens[i], "half")) {
      int j = i + 1;
      if (j < tl->count && (prv_eq(tl->tokens[j], "a") || prv_eq(tl->tokens[j], "an"))) {
        j++;
      }
      if (j < tl->count) {
        if (prv_is_unit_hour(tl->tokens[j])) {
          total += 1800;
          found = true;
          i = j + 1;
          continue;
        }
        if (prv_is_unit_minute(tl->tokens[j])) {
          total += 30;
          found = true;
          i = j + 1;
          continue;
        }
      }
      break;
    }
    int save = i;
    int n = prv_parse_number(tl, &i);
    if (n < 0) {
      break;
    }
    if (i >= tl->count) {
      i = save;  // a number with no unit isn't a duration
      break;
    }
    // "<n> and a half <unit>".
    bool and_a_half = false;
    if (i + 2 < tl->count && prv_eq(tl->tokens[i], "and") &&
        prv_eq(tl->tokens[i + 1], "a") && prv_eq(tl->tokens[i + 2], "half")) {
      and_a_half = true;
    }
    int unit_idx = and_a_half ? i + 3 : i;
    if (unit_idx >= tl->count) {
      i = save;
      break;
    }
    const char *unit = tl->tokens[unit_idx];
    long unit_secs;
    if (prv_is_unit_second(unit)) {
      unit_secs = 1;
    } else if (prv_is_unit_minute(unit)) {
      unit_secs = 60;
    } else if (prv_is_unit_hour(unit)) {
      unit_secs = 3600;
    } else {
      i = save;  // number followed by something that isn't a time unit
      break;
    }
    total += (long)n * unit_secs + (and_a_half ? unit_secs / 2 : 0);
    found = true;
    i = unit_idx + 1;
    // Trailing "and a half", e.g. "an hour and a half" (unit before the half).
    if (!and_a_half && i + 2 < tl->count && prv_eq(tl->tokens[i], "and") &&
        prv_eq(tl->tokens[i + 1], "a") && prv_eq(tl->tokens[i + 2], "half")) {
      total += unit_secs / 2;
      i += 3;
    }
  }

  if (!found || total <= 0) {
    return -1;
  }
  *idx = i;
  return (int)total;
}

// ---------------------------------------------------------------------------
// Clock-time parsing (for alarms)
// ---------------------------------------------------------------------------

typedef struct {
  int hour;     // 0-23 once am/pm applied, else 0-12 with has_meridiem false
  int minute;
  bool has_meridiem;  // true if am/pm was explicitly stated
} ClockTime;

// Parses an absolute wall-clock time starting at *idx. Handles "noon",
// "midnight", "7", "7:30", "seven thirty", "seven o'clock", "seven oh five",
// each with optional "am"/"pm". Returns true on success.
static bool prv_parse_clock(const TokenList *tl, int *idx, ClockTime *out) {
  int i = *idx;
  if (i >= tl->count) {
    return false;
  }
  out->minute = 0;
  out->has_meridiem = false;

  if (prv_eq(tl->tokens[i], "noon")) {
    out->hour = 12;
    out->has_meridiem = true;  // unambiguous
    *idx = i + 1;
    return true;
  }
  if (prv_eq(tl->tokens[i], "midnight")) {
    out->hour = 0;
    out->has_meridiem = true;  // unambiguous
    *idx = i + 1;
    return true;
  }

  // Hour, possibly "7:30" as one token.
  const char *t = tl->tokens[i];
  const char *colon = strchr(t, ':');
  if (colon) {
    char hbuf[4] = {0};
    int hl = (int)(colon - t);
    if (hl <= 0 || hl > 2) {
      return false;
    }
    strncpy(hbuf, t, hl);
    if (!prv_is_digits(hbuf) || !prv_is_digits(colon + 1)) {
      return false;
    }
    out->hour = atoi(hbuf);
    out->minute = atoi(colon + 1);
    i++;
  } else {
    int h = prv_parse_number(tl, &i);
    if (h < 0 || h > 23) {
      return false;
    }
    out->hour = h;
    // Optional minutes.
    if (i < tl->count && prv_eq(tl->tokens[i], "o'clock")) {
      i++;
    } else if (i < tl->count && prv_eq(tl->tokens[i], "oh") && i + 1 < tl->count) {
      // "seven oh five" -> 7:05
      int j = i + 1;
      int m = prv_parse_number(tl, &j);
      if (m >= 0 && m < 60) {
        out->minute = m;
        i = j;
      } else {
        i++;
      }
    } else {
      int j = i;
      int m = prv_parse_number(tl, &j);
      if (m >= 0 && m < 60 && j > i) {
        out->minute = m;
        i = j;
      }
    }
  }

  if (out->hour > 23 || out->minute > 59) {
    return false;
  }

  // Optional am/pm.
  if (i < tl->count) {
    const char *mer = tl->tokens[i];
    if (prv_eq(mer, "am")) {
      out->has_meridiem = true;
      if (out->hour == 12) {
        out->hour = 0;
      }
      i++;
    } else if (prv_eq(mer, "pm")) {
      out->has_meridiem = true;
      if (out->hour < 12) {
        out->hour += 12;
      }
      i++;
    }
  }

  *idx = i;
  return true;
}

// Converts a parsed clock time into the next future absolute timestamp.
static time_t prv_next_occurrence(const ClockTime *ct) {
  time_t now = time(NULL);
  struct tm lt = *localtime(&now);
  lt.tm_sec = 0;
  lt.tm_min = ct->minute;

  if (ct->has_meridiem || ct->hour == 0 || ct->hour > 12) {
    // Unambiguous hour.
    lt.tm_hour = ct->hour;
    time_t when = mktime(&lt);
    if (when <= now) {
      lt.tm_mday += 1;
      when = mktime(&lt);
    }
    return when;
  }

  // Ambiguous 12-hour time (1-12, no am/pm): pick the soonest future of the
  // two candidates (h and h+12).
  int h = ct->hour % 12;  // 12 -> 0
  time_t best = 0;
  for (int k = 0; k < 2; ++k) {
    struct tm cand = lt;
    cand.tm_hour = h + 12 * k;
    time_t when = mktime(&cand);
    if (when <= now) {
      cand.tm_mday += 1;
      when = mktime(&cand);
    }
    if (best == 0 || when < best) {
      best = when;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// Confirmation formatting
// ---------------------------------------------------------------------------

static void prv_format_duration(int seconds, char *out, size_t out_len) {
  int h = seconds / 3600;
  int m = (seconds % 3600) / 60;
  int s = seconds % 60;
  if (h > 0 && m > 0) {
    snprintf(out, out_len, "%d hour%s %d minute%s", h, h == 1 ? "" : "s", m, m == 1 ? "" : "s");
  } else if (h > 0) {
    snprintf(out, out_len, "%d hour%s", h, h == 1 ? "" : "s");
  } else if (m > 0 && s > 0) {
    snprintf(out, out_len, "%d minute%s %d second%s", m, m == 1 ? "" : "s", s, s == 1 ? "" : "s");
  } else if (m > 0) {
    snprintf(out, out_len, "%d minute%s", m, m == 1 ? "" : "s");
  } else {
    snprintf(out, out_len, "%d second%s", s, s == 1 ? "" : "s");
  }
}

// ---------------------------------------------------------------------------
// Command matchers
// ---------------------------------------------------------------------------

static bool prv_is_set_verb(const char *t) {
  return prv_eq(t, "set") || prv_eq(t, "start") || prv_eq(t, "create") || prv_eq(t, "make");
}

static bool prv_is_cancel_verb(const char *t) {
  return prv_eq(t, "cancel") || prv_eq(t, "delete") || prv_eq(t, "stop") ||
         prv_eq(t, "clear") || prv_eq(t, "remove");
}

// Advances past an optional leading "please".
static int prv_skip_pleasantries(const TokenList *tl, int idx) {
  if (idx < tl->count && prv_eq(tl->tokens[idx], "please")) {
    idx++;
  }
  return idx;
}

// Advances past optional articles "a"/"an"/"the"/"my".
static int prv_skip_article(const TokenList *tl, int idx) {
  if (idx < tl->count && (prv_eq(tl->tokens[idx], "a") || prv_eq(tl->tokens[idx], "an") ||
                          prv_eq(tl->tokens[idx], "the") || prv_eq(tl->tokens[idx], "my"))) {
    idx++;
  }
  return idx;
}

bool offline_commands_try(ConversationManager *manager, const char *input) {
  TokenList tl;
  if (!prv_tokenize(input, &tl)) {
    return false;
  }

  int i = prv_skip_pleasantries(&tl, 0);
  if (i >= tl.count) {
    return false;
  }

  // --- Cancel timer / alarm ------------------------------------------------
  if (prv_is_cancel_verb(tl.tokens[i])) {
    int j = prv_skip_article(&tl, i + 1);
    if (j >= tl.count) {
      return false;
    }
    bool is_timer;
    if (prv_eq(tl.tokens[j], "timer") || prv_eq(tl.tokens[j], "timers")) {
      is_timer = true;
    } else if (prv_eq(tl.tokens[j], "alarm") || prv_eq(tl.tokens[j], "alarms")) {
      is_timer = false;
    } else {
      return false;
    }
    // Only match if that's essentially the whole command.
    if (j + 1 < tl.count) {
      return false;
    }
    if (alarm_manager_cancel_first(is_timer)) {
      conversation_manager_add_response(manager, is_timer ? "Timer cancelled." : "Alarm cancelled.");
    } else {
      conversation_manager_add_response(manager, is_timer ? "You have no timer to cancel." : "You have no alarm to cancel.");
    }
    BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline cancel command.");
    return true;
  }

  // --- Set timer / alarm ---------------------------------------------------
  if (prv_is_set_verb(tl.tokens[i])) {
    int j = i + 1;
    // Optional "me" as in "set me a timer".
    if (j < tl.count && prv_eq(tl.tokens[j], "me")) {
      j++;
    }
    j = prv_skip_article(&tl, j);
    if (j >= tl.count) {
      return false;
    }

    bool is_timer;
    if (prv_eq(tl.tokens[j], "timer")) {
      is_timer = true;
    } else if (prv_eq(tl.tokens[j], "alarm")) {
      is_timer = false;
    } else {
      return false;
    }
    j++;
    // Optional "for" / "at".
    if (j < tl.count && (prv_eq(tl.tokens[j], "for") || prv_eq(tl.tokens[j], "at"))) {
      j++;
    }
    if (j >= tl.count) {
      return false;
    }

    if (is_timer) {
      int idx = j;
      int seconds = prv_parse_duration(&tl, &idx);
      // Must consume the rest of the utterance (modulo a trailing "please").
      int tail = prv_skip_pleasantries(&tl, idx);
      if (seconds < 0 || tail != tl.count) {
        return false;
      }
      time_t when = time(NULL) + seconds;
      int result = alarm_manager_add_alarm(when, true, NULL, true);
      if (result != 0) {
        conversation_manager_add_response(manager, "Sorry, I couldn't set that timer.");
        return true;
      }
      char dur[48];
      prv_format_duration(seconds, dur, sizeof(dur));
      char msg[80];
      snprintf(msg, sizeof(msg), "Timer set for %s.", dur);
      conversation_manager_add_response(manager, msg);
      BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline timer for %d seconds.", seconds);
      return true;
    } else {
      ClockTime ct;
      int idx = j;
      if (!prv_parse_clock(&tl, &idx, &ct)) {
        return false;
      }
      int tail = prv_skip_pleasantries(&tl, idx);
      if (tail != tl.count) {
        return false;
      }
      time_t when = prv_next_occurrence(&ct);
      int result = alarm_manager_add_alarm(when, false, NULL, true);
      if (result != 0) {
        conversation_manager_add_response(manager, "Sorry, I couldn't set that alarm.");
        return true;
      }
      struct tm lt = *localtime(&when);
      char timestr[16];
      strftime(timestr, sizeof(timestr), "%l:%M %p", &lt);
      // %l pads with a space; trim it.
      char *ts = timestr;
      while (*ts == ' ') {
        ts++;
      }
      char msg[64];
      snprintf(msg, sizeof(msg), "Alarm set for %s.", ts);
      conversation_manager_add_response(manager, msg);
      BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline alarm for %d.", (int)when);
      return true;
    }
  }

  return false;
}
