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

// Deferred reminder send — schedules the AppMessage via a timer so it fires
// after the dictation callback unwinds (avoids outbox state issues).
typedef struct {
  time_t when;
  char text[32];
} PendingReminder;

static PendingReminder s_pending_reminder;
static bool s_has_pending_reminder = false;

static void prv_deferred_reminder_send(void *context) {
  if (!s_has_pending_reminder) return;
  s_has_pending_reminder = false;
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    BOBBY_LOG(APP_LOG_LEVEL_WARNING, "Deferred reminder: outbox begin failed");
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_OFFLINE_REMINDER_TEXT, s_pending_reminder.text);
  dict_write_int32(iter, MESSAGE_KEY_OFFLINE_REMINDER_TIME, (int32_t)s_pending_reminder.when);
  app_message_outbox_send();
  BOBBY_LOG(APP_LOG_LEVEL_INFO, "Deferred reminder sent to phone at %d", (int)s_pending_reminder.when);
}

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

static bool prv_is_timer_word(const char *t) {
  return prv_eq(t, "timer") || prv_eq(t, "timers");
}

static bool prv_is_alarm_word(const char *t) {
  return prv_eq(t, "alarm") || prv_eq(t, "alarms");
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

// Build a name string from tl->tokens[start..end) into buf (size buf_len).
static void prv_build_name(const TokenList *tl, int start, int end,
                           char *buf, int buf_len) {
  int pos = 0;
  for (int k = start; k < end && pos < buf_len - 1; ++k) {
    if (k > start && pos < buf_len - 2) {
      buf[pos++] = ' ';
    }
    for (const char *p = tl->tokens[k]; *p && pos < buf_len - 1; ++p) {
      buf[pos++] = *p;
    }
  }
  buf[pos] = '\0';
}

// Format a future alarm time as "H:MM AM/PM" and write to buf.
static void prv_format_clock(time_t when, char *buf, size_t buf_len) {
  struct tm lt = *localtime(&when);
  strftime(buf, buf_len, "%l:%M %p", &lt);
  // %l pads with a single leading space for single-digit hours; remove it.
  if (buf[0] == ' ') {
    memmove(buf, buf + 1, buf_len - 1);
  }
}

// ---------------------------------------------------------------------------
// B4: Time / date / battery helpers
// ---------------------------------------------------------------------------

static bool prv_contains_token(const TokenList *tl, int from, const char *word) {
  for (int k = from; k < tl->count; ++k) {
    if (prv_eq(tl->tokens[k], word)) {
      return true;
    }
  }
  return false;
}

#define NAME_BUF_SIZE 32

// ---------------------------------------------------------------------------
// Reminder helper — parses time+text and sends to phone via AppMessage.
// The phone creates the Rebble timeline pin (same as Note to Self approach).
// No wakeup API, no new windows, no alarm manager involvement.
// ---------------------------------------------------------------------------

static bool prv_match_reminder(char *result_buf, size_t result_size,
                               const TokenList *tl, int j) {
  if (j < tl->count &&
      (prv_eq(tl->tokens[j], "to") || prv_eq(tl->tokens[j], "that") ||
       prv_eq(tl->tokens[j], "about"))) {
    j++;
  }

  time_t when = 0;
  char text_buf[NAME_BUF_SIZE] = {0};

  if (j < tl->count &&
      (prv_eq(tl->tokens[j], "at") || prv_eq(tl->tokens[j], "in"))) {
    bool use_duration = prv_eq(tl->tokens[j], "in");
    int k = j + 1;
    if (use_duration) {
      int secs = prv_parse_duration(tl, &k);
      if (secs < 0) return false;
      when = time(NULL) + secs;
    } else {
      ClockTime ct;
      if (!prv_parse_clock(tl, &k, &ct)) return false;
      when = prv_next_occurrence(&ct);
    }
    if (k < tl->count &&
        (prv_eq(tl->tokens[k], "to") || prv_eq(tl->tokens[k], "that"))) {
      k++;
    }
    if (k >= tl->count) return false;
    prv_build_name(tl, k, tl->count, text_buf, NAME_BUF_SIZE);
  } else {
    int text_start = j, text_end = -1;
    for (int k = text_start; k < tl->count; ++k) {
      if (prv_eq(tl->tokens[k], "at") || prv_eq(tl->tokens[k], "in")) {
        bool use_duration = prv_eq(tl->tokens[k], "in");
        int try_idx = k + 1;
        if (use_duration) {
          int secs = prv_parse_duration(tl, &try_idx);
          if (secs >= 0) { when = time(NULL) + secs; text_end = k; break; }
        } else {
          ClockTime ct;
          if (prv_parse_clock(tl, &try_idx, &ct)) {
            when = prv_next_occurrence(&ct); text_end = k; break;
          }
        }
      }
    }
    if (text_end < 0 || when == 0 || text_end <= text_start) return false;
    prv_build_name(tl, text_start, text_end, text_buf, NAME_BUF_SIZE);
  }

  if (when == 0 || text_buf[0] == '\0') return false;

  // Queue the AppMessage for deferred delivery so it fires after the
  // conversation handler chain unwinds (avoids outbox state corruption).
  s_pending_reminder.when = when;
  strncpy(s_pending_reminder.text, text_buf, sizeof(s_pending_reminder.text) - 1);
  s_pending_reminder.text[sizeof(s_pending_reminder.text) - 1] = '\0';
  s_has_pending_reminder = true;
  app_timer_register(100, prv_deferred_reminder_send, NULL);

  char timestr[16];
  prv_format_clock(when, timestr, sizeof(timestr));
  snprintf(result_buf, result_size, "Reminder for %s: %s.", timestr, text_buf);
  BOBBY_LOG(APP_LOG_LEVEL_INFO, "Queued deferred reminder to phone at %d", (int)when);
  return true;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

bool offline_commands_try(const char *input, char *result_buf, size_t result_size,
                          OfflineCommandType *out_type) {
  if (out_type) *out_type = OC_NONE;
  TokenList tl;
  if (!prv_tokenize(input, &tl)) {
    return false;
  }

  int i = prv_skip_pleasantries(&tl, 0);
  if (i >= tl.count) {
    return false;
  }

  // --- Cancel timer / alarm (single or all) ---------------------------------
  if (prv_is_cancel_verb(tl.tokens[i])) {
    int j = i + 1;
    j = prv_skip_article(&tl, j);
    if (j >= tl.count) {
      return false;
    }

    // B3: cancel all
    if (prv_eq(tl.tokens[j], "all")) {
      j++;
      j = prv_skip_article(&tl, j);
      if (j >= tl.count) return false;
      bool is_timer;
      if (prv_is_timer_word(tl.tokens[j])) {
        is_timer = true;
      } else if (prv_is_alarm_word(tl.tokens[j])) {
        is_timer = false;
      } else {
        return false;
      }
      int count = 0;
      while (alarm_manager_cancel_first(is_timer)) {
        count++;
      }
      char msg[64];
      if (count == 0) {
        snprintf(msg, sizeof(msg), "No %s to cancel.",
                 is_timer ? "timers" : "alarms");
      } else {
        snprintf(msg, sizeof(msg), "Cancelled %d %s.", count,
                 count == 1 ? (is_timer ? "timer" : "alarm")
                             : (is_timer ? "timers" : "alarms"));
      }
      snprintf(result_buf, result_size, "%s", msg);
      BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline cancel-all.");
      if (out_type) *out_type = is_timer ? OC_TYPE_TIMER : OC_TYPE_ALARM;
      return true;
    }

    // Single cancel
    bool is_timer;
    if (prv_is_timer_word(tl.tokens[j])) {
      is_timer = true;
    } else if (prv_is_alarm_word(tl.tokens[j])) {
      is_timer = false;
    } else {
      return false;
    }
    if (alarm_manager_cancel_first(is_timer)) {
      snprintf(result_buf, result_size, "%s",
               is_timer ? "Timer cancelled." : "Alarm cancelled.");
    } else {
      snprintf(result_buf, result_size, "%s",
               is_timer ? "You have no timer to cancel."
                        : "You have no alarm to cancel.");
    }
    BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline cancel command.");
    if (out_type) *out_type = is_timer ? OC_TYPE_TIMER : OC_TYPE_ALARM;
    return true;
  }

  // --- Set timer / alarm (B1 looser phrasing) ------------------------------------
  if (prv_is_set_verb(tl.tokens[i])) {
    int j = i + 1;
    // Optional "me" as in "set me a timer".
    if (j < tl.count && prv_eq(tl.tokens[j], "me")) {
      j++;
    }
    j = prv_skip_article(&tl, j);
    if (j < tl.count && prv_eq(tl.tokens[j], "reminder")) {
      j++;
      if (j < tl.count &&
          (prv_eq(tl.tokens[j], "to") || prv_eq(tl.tokens[j], "for") ||
           prv_eq(tl.tokens[j], "about"))) {
        j++;
      }
      if (prv_match_reminder(result_buf, result_size, &tl, j)) {
        if (out_type) *out_type = OC_TYPE_REMINDER;
        return true;
      }
      return false;
    }
    if (j >= tl.count) {
      return false;
    }

    // B1: scan up to 2 adjective/label tokens before "timer"/"alarm"
    bool is_timer = false;
    bool found_type = false;
    int name_start = j;
    int name_end = j;

    for (int scan = 0; scan <= 2 && j < tl.count; ++scan) {
      if (prv_is_timer_word(tl.tokens[j])) {
        is_timer = true;
        found_type = true;
        name_end = j;
        j++;
        break;
      }
      if (prv_is_alarm_word(tl.tokens[j])) {
        is_timer = false;
        found_type = true;
        name_end = j;
        j++;
        break;
      }
      if (scan < 2) {
        j++;
      }
    }
    if (!found_type) {
      return false;
    }

    // Build name from adjective tokens (e.g. "baking" from "set a baking timer")
    char name_buf[NAME_BUF_SIZE] = {0};
    if (name_end > name_start) {
      prv_build_name(&tl, name_start, name_end, name_buf, NAME_BUF_SIZE);
    }
    const char *name = (name_buf[0] != '\0') ? name_buf : NULL;

    // Optional "for" / "at".
    if (j < tl.count && (prv_eq(tl.tokens[j], "for") || prv_eq(tl.tokens[j], "at") ||
                         prv_eq(tl.tokens[j], "in"))) {
      j++;
    }
    if (j >= tl.count) {
      return false;
    }

    if (is_timer) {
      int idx = j;
      int seconds = prv_parse_duration(&tl, &idx);
      int tail = prv_skip_pleasantries(&tl, idx);
      if (seconds < 0 || tail != tl.count) {
        return false;
      }
      time_t when = time(NULL) + seconds;
      int result = alarm_manager_add_alarm(when, true, name);
      if (result != 0) {
        snprintf(result_buf, result_size, "%s", "Sorry, I couldn't set that timer.");
        if (out_type) *out_type = OC_TYPE_TIMER;
        return true;
      }
      char dur[48];
      prv_format_duration(seconds, dur, sizeof(dur));
      snprintf(result_buf, result_size, "Timer set for %s.", dur);
      BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline timer for %d seconds.", seconds);
      if (out_type) *out_type = OC_TYPE_TIMER;
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
      int result = alarm_manager_add_alarm(when, false, name);
      if (result != 0) {
        snprintf(result_buf, result_size, "%s", "Sorry, I couldn't set that alarm.");
        if (out_type) *out_type = OC_TYPE_ALARM;
        return true;
      }
      char timestr[16];
      prv_format_clock(when, timestr, sizeof(timestr));
      snprintf(result_buf, result_size, "Alarm set for %s.", timestr);
      BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline alarm for %d.", (int)when);
      if (out_type) *out_type = OC_TYPE_ALARM;
      return true;
    }
  }

  // --- Reminder: "remind me ..." -------------------------------------------
  if (prv_eq(tl.tokens[i], "remind")) {
    int j = i + 1;
    if (j >= tl.count || !prv_eq(tl.tokens[j], "me")) return false;
    j++;
    if (prv_match_reminder(result_buf, result_size, &tl, j)) {
      if (out_type) *out_type = OC_TYPE_REMINDER;
      return true;
    }
    return false;
  }

  // --- B2: List / query timers & alarms ------------------------------------
  // Trigger words: list, show, tell + query content; or what/how/do/are + ...
  {
    bool is_query = (prv_eq(tl.tokens[i], "list") || prv_eq(tl.tokens[i], "show") ||
                     prv_eq(tl.tokens[i], "what") || prv_eq(tl.tokens[i], "what's") ||
                     prv_eq(tl.tokens[i], "whats") || prv_eq(tl.tokens[i], "how") ||
                     prv_eq(tl.tokens[i], "do") || prv_eq(tl.tokens[i], "are") ||
                     prv_eq(tl.tokens[i], "tell"));
    if (is_query) {
      // Look for "timer(s)" or "alarm(s)" anywhere in the remaining tokens
      bool found_timer = prv_contains_token(&tl, i + 1, "timer") ||
                         prv_contains_token(&tl, i + 1, "timers");
      bool found_alarm = prv_contains_token(&tl, i + 1, "alarm") ||
                         prv_contains_token(&tl, i + 1, "alarms");
      // Must find exactly one type (timers or alarms)
      if (found_timer != found_alarm) {
        bool is_timer = found_timer;
        int total = alarm_manager_get_alarm_count();
        int count = 0;
        int first_idx = -1;
        for (int k = 0; k < total; ++k) {
          Alarm *a = alarm_manager_get_alarm(k);
          if (alarm_is_timer(a) == is_timer) {
            if (first_idx < 0) first_idx = k;
            count++;
          }
        }
        char msg[128];
        if (count == 0) {
          snprintf(msg, sizeof(msg), "No %s set.", is_timer ? "timers" : "alarms");
        } else {
          Alarm *first = alarm_manager_get_alarm(first_idx);
          char timestr[32];
          if (is_timer) {
            int remaining = (int)(alarm_get_time(first) - time(NULL));
            if (remaining < 0) remaining = 0;
            prv_format_duration(remaining, timestr, sizeof(timestr));
          } else {
            prv_format_clock(alarm_get_time(first), timestr, sizeof(timestr));
          }
          const char *name = alarm_get_name(first);
          if (count == 1) {
            if (name) {
              snprintf(msg, sizeof(msg), "1 %s \"%s\" (%s%s).",
                       is_timer ? "timer" : "alarm", name,
                       is_timer ? "" : "at ", timestr);
            } else {
              snprintf(msg, sizeof(msg), "1 %s (%s%s).",
                       is_timer ? "timer" : "alarm",
                       is_timer ? "" : "at ", timestr);
            }
          } else {
            if (name) {
              snprintf(msg, sizeof(msg), "%d %s. Next: \"%s\" (%s%s).",
                       count, is_timer ? "timers" : "alarms", name,
                       is_timer ? "" : "at ", timestr);
            } else {
              snprintf(msg, sizeof(msg), "%d %s. Next: %s%s.",
                       count, is_timer ? "timers" : "alarms",
                       is_timer ? "" : "at ", timestr);
            }
          }
        }
        snprintf(result_buf, result_size, "%s", msg);
        BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline list command.");
        if (out_type) *out_type = is_timer ? OC_TYPE_TIMER : OC_TYPE_ALARM;
        return true;
      }
    }
  }

  // --- B4: Time / date / battery -------------------------------------------
  {
    // Only match if the first token signals a query or is the keyword itself.
    bool starts_query = (prv_eq(tl.tokens[i], "what") || prv_eq(tl.tokens[i], "what's") ||
                         prv_eq(tl.tokens[i], "whats") || prv_eq(tl.tokens[i], "how") ||
                         prv_eq(tl.tokens[i], "time") || prv_eq(tl.tokens[i], "date") ||
                         prv_eq(tl.tokens[i], "day") || prv_eq(tl.tokens[i], "battery") ||
                         prv_eq(tl.tokens[i], "tell") || prv_eq(tl.tokens[i], "check"));
    if (starts_query) {
      if (prv_contains_token(&tl, i, "battery")) {
        BatteryChargeState state = battery_state_service_peek();
        char msg[64];
        if (state.is_charging) {
          snprintf(msg, sizeof(msg), "Battery is at %d%% (charging).",
                   state.charge_percent);
        } else {
          snprintf(msg, sizeof(msg), "Battery is at %d%%.", state.charge_percent);
        }
        snprintf(result_buf, result_size, "%s", msg);
        BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline battery query.");
        if (out_type) *out_type = OC_TYPE_INFO;
        return true;
      }
      if (prv_contains_token(&tl, i, "date") || prv_contains_token(&tl, i, "day") ||
          prv_contains_token(&tl, i, "today") || prv_contains_token(&tl, i, "month") ||
          prv_contains_token(&tl, i, "year")) {
        time_t now = time(NULL);
        char datestr[48];
        strftime(datestr, sizeof(datestr), "%A, %B %e %Y", localtime(&now));
        // %e pads with a space for single-digit days; remove it.
        for (char *p = datestr; *p; ++p) {
          if (*p == ' ' && *(p+1) == ' ') {
            memmove(p, p+1, strlen(p));
          }
        }
        char msg[80];
        snprintf(msg, sizeof(msg), "Today is %s.", datestr);
        snprintf(result_buf, result_size, "%s", msg);
        BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline date query.");
        if (out_type) *out_type = OC_TYPE_INFO;
        return true;
      }
      if (prv_contains_token(&tl, i, "time")) {
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        char timestr[16];
        if (clock_is_24h_style()) {
          strftime(timestr, sizeof(timestr), "%H:%M", lt);
        } else {
          strftime(timestr, sizeof(timestr), "%l:%M %p", lt);
          // Trim leading space from %l
          char *p = timestr;
          while (*p == ' ') {
            memmove(timestr, timestr + 1, sizeof(timestr) - 1);
          }
        }
        char msg[48];
        snprintf(msg, sizeof(msg), "It's %s.", timestr);
        snprintf(result_buf, result_size, "%s", msg);
        BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline time query.");
        if (out_type) *out_type = OC_TYPE_INFO;
        return true;
      }
    }
  }

  return false;
}
