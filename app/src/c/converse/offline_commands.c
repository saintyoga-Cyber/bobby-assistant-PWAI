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
    } else if (c == '.' && i + 1 < len && input[i + 1] >= '0' && input[i + 1] <= '9' &&
               w > 0 && tl->buffer[w - 1] >= '0' && tl->buffer[w - 1] <= '9') {
      tl->buffer[w++] = c;  // keep decimal point inside numeric literals like "1.5"
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
  // %l pads with a leading space; remove it.
  char *p = buf;
  while (*p == ' ') {
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

// ---------------------------------------------------------------------------
// C2: Reminder matching helper
// Called with j pointing to the token after "me" (or after "reminder" + opt
// "to"/"for"). Attempts to parse "remind me [to] <text> at/in <time>" or
// "remind me at/in <time> to <text>" forms.
// Returns true and calls alarm_manager_add_reminder on success.
// ---------------------------------------------------------------------------
#define REMINDER_TEXT_SIZE 32

static bool prv_match_reminder(ConversationManager *manager,
                               const TokenList *tl, int j) {
  // Skip optional "to" / "that" / "about"
  if (j < tl->count &&
      (prv_eq(tl->tokens[j], "to") || prv_eq(tl->tokens[j], "that") ||
       prv_eq(tl->tokens[j], "about"))) {
    j++;
  }

  time_t when = 0;
  char text_buf[REMINDER_TEXT_SIZE] = {0};

  // Check whether time preposition comes FIRST ("remind me at 5pm to buy milk")
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
    // Optional "to" / "that" before the text
    if (k < tl->count &&
        (prv_eq(tl->tokens[k], "to") || prv_eq(tl->tokens[k], "that"))) {
      k++;
    }
    if (k >= tl->count) return false;
    prv_build_name(tl, k, tl->count, text_buf, REMINDER_TEXT_SIZE);
  } else {
    // Text comes first: collect tokens until we hit "at"/"in" + parseable time.
    int text_end = -1;
    int text_start = j;

    for (int k = text_start; k < tl->count; ++k) {
      if (prv_eq(tl->tokens[k], "at") || prv_eq(tl->tokens[k], "in")) {
        bool use_duration = prv_eq(tl->tokens[k], "in");
        int try_idx = k + 1;
        if (use_duration) {
          int secs = prv_parse_duration(tl, &try_idx);
          if (secs >= 0) {
            when = time(NULL) + secs;
            text_end = k;
            break;
          }
        } else {
          ClockTime ct;
          if (prv_parse_clock(tl, &try_idx, &ct)) {
            when = prv_next_occurrence(&ct);
            text_end = k;
            break;
          }
        }
      }
    }

    if (text_end < 0 || when == 0) return false;
    if (text_end <= text_start) return false;
    prv_build_name(tl, text_start, text_end, text_buf, REMINDER_TEXT_SIZE);
  }

  if (when == 0 || text_buf[0] == '\0') return false;

  int result = alarm_manager_add_reminder(when, text_buf);
  if (result != 0) {
    conversation_manager_add_response(manager, "Sorry, I couldn't set that reminder.");
    return true;
  }
  char timestr[16];
  prv_format_clock(when, timestr, sizeof(timestr));
  char msg[80];
  snprintf(msg, sizeof(msg), "Reminder set for %s: %s.", timestr, text_buf);
  conversation_manager_add_response(manager, msg);
  BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline reminder.");
  return true;
}

// ---------------------------------------------------------------------------
// Unit Converter (offline)
// ---------------------------------------------------------------------------

#define UC_MASS  0
#define UC_DIST  1
#define UC_VOL   2
#define UC_TEMP  3
#define UC_SPEED 4

typedef struct {
  const char *tok;
  int         cat;
  double      to_base;  // multiply value by this → base unit
                        // UC_TEMP: 1.0=Celsius(base), 0.0=Fahrenheit, -1.0=Kelvin
  const char *disp;
} UCUnit;

static const UCUnit s_uc_units[] = {
  // Mass (base: grams)
  {"mg", UC_MASS, 0.001, "mg"},
  {"milligram", UC_MASS, 0.001, "mg"}, {"milligrams", UC_MASS, 0.001, "mg"},
  {"g", UC_MASS, 1.0, "g"},
  {"gram", UC_MASS, 1.0, "g"}, {"grams", UC_MASS, 1.0, "g"},
  {"kg", UC_MASS, 1000.0, "kg"},
  {"kilogram", UC_MASS, 1000.0, "kg"}, {"kilograms", UC_MASS, 1000.0, "kg"},
  {"oz", UC_MASS, 28.3495, "oz"},
  {"ounce", UC_MASS, 28.3495, "oz"}, {"ounces", UC_MASS, 28.3495, "oz"},
  {"lb", UC_MASS, 453.592, "lb"}, {"lbs", UC_MASS, 453.592, "lb"},
  {"pound", UC_MASS, 453.592, "lb"}, {"pounds", UC_MASS, 453.592, "lb"},
  // Distance (base: meters)
  {"mm", UC_DIST, 0.001, "mm"},
  {"millimeter", UC_DIST, 0.001, "mm"}, {"millimeters", UC_DIST, 0.001, "mm"},
  {"cm", UC_DIST, 0.01, "cm"},
  {"centimeter", UC_DIST, 0.01, "cm"}, {"centimeters", UC_DIST, 0.01, "cm"},
  {"m", UC_DIST, 1.0, "m"},
  {"meter", UC_DIST, 1.0, "m"}, {"meters", UC_DIST, 1.0, "m"},
  {"km", UC_DIST, 1000.0, "km"},
  {"kilometer", UC_DIST, 1000.0, "km"}, {"kilometers", UC_DIST, 1000.0, "km"},
  {"inch", UC_DIST, 0.0254, "in"}, {"inches", UC_DIST, 0.0254, "in"},
  {"ft", UC_DIST, 0.3048, "ft"},
  {"foot", UC_DIST, 0.3048, "ft"}, {"feet", UC_DIST, 0.3048, "ft"},
  {"yd", UC_DIST, 0.9144, "yd"},
  {"yard", UC_DIST, 0.9144, "yd"}, {"yards", UC_DIST, 0.9144, "yd"},
  {"mile", UC_DIST, 1609.34, "mi"}, {"miles", UC_DIST, 1609.34, "mi"},
  // Volume (base: milliliters)
  {"ml", UC_VOL, 1.0, "ml"},
  {"milliliter", UC_VOL, 1.0, "ml"}, {"milliliters", UC_VOL, 1.0, "ml"},
  {"l", UC_VOL, 1000.0, "L"},
  {"liter", UC_VOL, 1000.0, "L"}, {"liters", UC_VOL, 1000.0, "L"},
  {"tsp", UC_VOL, 4.92892, "tsp"},
  {"teaspoon", UC_VOL, 4.92892, "tsp"}, {"teaspoons", UC_VOL, 4.92892, "tsp"},
  {"tbsp", UC_VOL, 14.7868, "tbsp"},
  {"tablespoon", UC_VOL, 14.7868, "tbsp"}, {"tablespoons", UC_VOL, 14.7868, "tbsp"},
  {"fluid", UC_VOL, 29.5735, "fl oz"},  // "fluid ounce(s)"
  {"cup", UC_VOL, 236.588, "cup"}, {"cups", UC_VOL, 236.588, "cups"},
  {"pint", UC_VOL, 473.176, "pt"}, {"pints", UC_VOL, 473.176, "pt"},
  {"quart", UC_VOL, 946.353, "qt"}, {"quarts", UC_VOL, 946.353, "qt"},
  {"gallon", UC_VOL, 3785.41, "gal"}, {"gallons", UC_VOL, 3785.41, "gal"},
  // Temperature (special sentinel: 1.0=C, 0.0=F, -1.0=K)
  {"celsius", UC_TEMP, 1.0, "\xc2\xb0""C"}, {"c", UC_TEMP, 1.0, "\xc2\xb0""C"},
  {"fahrenheit", UC_TEMP, 0.0, "\xc2\xb0""F"}, {"f", UC_TEMP, 0.0, "\xc2\xb0""F"},
  {"kelvin", UC_TEMP, -1.0, "K"}, {"k", UC_TEMP, -1.0, "K"},
  // Speed (base: m/s)
  {"mph", UC_SPEED, 0.44704, "mph"},
  {"kph", UC_SPEED, 0.27778, "km/h"}, {"kmh", UC_SPEED, 0.27778, "km/h"},
  {"knot", UC_SPEED, 0.514444, "kn"}, {"knots", UC_SPEED, 0.514444, "kn"},
};

// Format a double to up to 4 decimal places, no trailing zeros.
// Uses only integer snprintf (safe on all Pebble platforms).
static void prv_format_float_uc(double v, char *buf, size_t size) {
  bool neg = (v < 0.0);
  if (neg) v = -v;
  int int_part = (int)v;
  int frac4 = (int)((v - (double)int_part) * 10000.0 + 0.5);
  if (frac4 >= 10000) { int_part++; frac4 = 0; }
  char tmp[48];
  if (neg) {
    snprintf(tmp, sizeof(tmp), "-%d.%04d", int_part, frac4);
  } else {
    snprintf(tmp, sizeof(tmp), "%d.%04d", int_part, frac4);
  }
  // Trim trailing zeros after decimal point
  size_t len = strlen(tmp);
  while (len > 0 && tmp[len - 1] == '0') { tmp[--len] = '\0'; }
  if (len > 0 && tmp[len - 1] == '.') { tmp[--len] = '\0'; }
  strncpy(buf, tmp, size - 1);
  buf[size - 1] = '\0';
}

// Parse a floating-point value: decimal token ("1.5"), integer, word number,
// or compound fraction ("one and a half" → 1.5).
static double prv_parse_float(const TokenList *tl, int *idx) {
  if (*idx >= tl->count) return -1.0;
  const char *t = tl->tokens[*idx];
  // Decimal token (after tokenizer fix keeps '.' between digits)
  if (strchr(t, '.')) {
    bool valid = true;
    int dots = 0;
    for (const char *p = t; *p; p++) {
      if (*p == '.') { if (++dots > 1) { valid = false; break; } }
      else if (*p < '0' || *p > '9') { valid = false; break; }
    }
    if (valid && dots == 1 && t[0] != '.') {
      double v = 0.0;
      const char *p = t;
      while (*p && *p != '.') { v = v * 10.0 + (*p++ - '0'); }
      if (*p == '.') {
        p++;
        double frac = 0.1;
        while (*p) { v += (*p++ - '0') * frac; frac *= 0.1; }
      }
      (*idx)++;
      return v;
    }
  }
  // Integer / word number
  int save = *idx;
  int n = prv_parse_number(tl, idx);
  if (n < 0) return -1.0;
  double val = (double)n;
  // Optional "and a half" / "and a quarter" / "and half"
  int j = *idx;
  if (j + 2 < tl->count && prv_eq(tl->tokens[j], "and") && prv_eq(tl->tokens[j + 1], "a")) {
    if (prv_eq(tl->tokens[j + 2], "half")) { val += 0.5; *idx = j + 3; }
    else if (prv_eq(tl->tokens[j + 2], "quarter")) { val += 0.25; *idx = j + 3; }
  } else if (j + 1 < tl->count && prv_eq(tl->tokens[j], "and") &&
             prv_eq(tl->tokens[j + 1], "half")) {
    val += 0.5; *idx = j + 2;
  }
  (void)save;
  return val;
}

// Find unit entry index at tl->tokens[*idx]. Advances *idx past the unit
// (2 tokens for "fluid ounce/ounces/oz"). Returns -1 if not found.
static int prv_find_uc_unit(const TokenList *tl, int *idx) {
  if (*idx >= tl->count) return -1;
  // Skip "degrees"/"degree" prefix for temperature
  int i = *idx;
  if (prv_eq(tl->tokens[i], "degrees") || prv_eq(tl->tokens[i], "degree")) {
    i++;
    if (i >= tl->count) return -1;
  }
  // Special compound: "fluid ounce(s)"
  if (prv_eq(tl->tokens[i], "fluid") && i + 1 < tl->count) {
    const char *nxt = tl->tokens[i + 1];
    if (prv_eq(nxt, "ounce") || prv_eq(nxt, "ounces") || prv_eq(nxt, "oz")) {
      *idx = i + 2;
      // Return index of "fluid" entry in s_uc_units
      int n = (int)(sizeof(s_uc_units) / sizeof(s_uc_units[0]));
      for (int k = 0; k < n; k++) {
        if (prv_eq(s_uc_units[k].tok, "fluid")) return k;
      }
    }
  }
  int n = (int)(sizeof(s_uc_units) / sizeof(s_uc_units[0]));
  for (int k = 0; k < n; k++) {
    if (prv_eq(tl->tokens[i], s_uc_units[k].tok)) {
      *idx = i + 1;
      return k;
    }
  }
  return -1;
}

static double prv_uc_convert(double val, const UCUnit *from, const UCUnit *to) {
  if (from->cat == UC_TEMP) {
    double c;
    if (from->to_base == 0.0) c = (val - 32.0) * 5.0 / 9.0;      // F → C
    else if (from->to_base < 0.0) c = val - 273.15;                // K → C
    else c = val;                                                    // C → C
    if (to->to_base == 0.0) return c * 9.0 / 5.0 + 32.0;          // C → F
    else if (to->to_base < 0.0) return c + 273.15;                 // C → K
    return c;
  }
  return val * from->to_base / to->to_base;
}

static bool prv_match_unit_convert(char *result_buf, size_t result_size,
                                   const TokenList *tl, int i) {
  int idx = i;
  bool how_many = false;

  if (idx < tl->count && prv_eq(tl->tokens[idx], "convert")) {
    idx++;
  } else if (idx < tl->count && prv_eq(tl->tokens[idx], "how") &&
             idx + 1 < tl->count && prv_eq(tl->tokens[idx + 1], "many")) {
    idx += 2;
    how_many = true;
  }

  int ua = -1, ub = -1;
  double val = 0.0;

  if (how_many) {
    // Pattern: "how many UNIT_B in VALUE UNIT_A"
    ub = prv_find_uc_unit(tl, &idx);
    if (ub < 0) return false;
    // Skip "are"/"is" if present, then require "in"
    if (idx < tl->count && (prv_eq(tl->tokens[idx], "are") || prv_eq(tl->tokens[idx], "is")))
      idx++;
    if (idx >= tl->count || !prv_eq(tl->tokens[idx], "in")) return false;
    idx++;
    val = prv_parse_float(tl, &idx);
    if (val < 0.0) return false;
    ua = prv_find_uc_unit(tl, &idx);
    if (ua < 0) return false;
  } else {
    // Pattern: "convert VALUE UNIT_A to UNIT_B" or "VALUE UNIT_A to/in UNIT_B"
    val = prv_parse_float(tl, &idx);
    if (val < 0.0) return false;
    ua = prv_find_uc_unit(tl, &idx);
    if (ua < 0) return false;
    if (idx >= tl->count) return false;
    if (prv_eq(tl->tokens[idx], "to") || prv_eq(tl->tokens[idx], "into") ||
        prv_eq(tl->tokens[idx], "in") || prv_eq(tl->tokens[idx], "as")) {
      idx++;
    } else {
      return false;
    }
    ub = prv_find_uc_unit(tl, &idx);
    if (ub < 0) return false;
  }

  if (s_uc_units[ua].cat != s_uc_units[ub].cat) return false;

  double result = prv_uc_convert(val, &s_uc_units[ua], &s_uc_units[ub]);
  char val_str[24], res_str[24];
  prv_format_float_uc(val, val_str, sizeof(val_str));
  prv_format_float_uc(result, res_str, sizeof(res_str));
  snprintf(result_buf, result_size, "%s %s = %s %s.",
           val_str, s_uc_units[ua].disp, res_str, s_uc_units[ub].disp);
  return true;
}

// ---------------------------------------------------------------------------
// Weather query — encodes day offset (0=today, 1=tomorrow) in result_buf;
// voice_window reads this and sends a WEATHER_REQUEST AppMessage to the phone.
// ---------------------------------------------------------------------------

static bool prv_match_weather(char *result_buf, size_t result_size,
                              const TokenList *tl, int i) {
  int day = 0;
  for (int k = i; k < tl->count; k++) {
    if (prv_eq(tl->tokens[k], "tomorrow")) { day = 1; break; }
  }
  snprintf(result_buf, result_size, "%d", day);
  return true;
}

// ---------------------------------------------------------------------------
// Timezone query — extracts location string after "in" into result_buf;
// voice_window sends this as a TZ_QUERY AppMessage to the phone.
// ---------------------------------------------------------------------------

static bool prv_match_timezone(char *result_buf, size_t result_size,
                               const TokenList *tl, int i) {
  int in_idx = -1;
  for (int k = i; k < tl->count; k++) {
    if (prv_eq(tl->tokens[k], "in")) { in_idx = k; break; }
  }
  if (in_idx < 0 || in_idx + 1 >= tl->count) return false;
  int pos = 0;
  for (int k = in_idx + 1; k < tl->count && pos < (int)result_size - 1; k++) {
    if (k > in_idx + 1 && pos < (int)result_size - 2) result_buf[pos++] = ' ';
    const char *tok = tl->tokens[k];
    while (*tok && pos < (int)result_size - 1) result_buf[pos++] = *tok++;
  }
  result_buf[pos] = '\0';
  return pos > 0;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

bool offline_commands_try(ConversationManager *manager, const char *input) {
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
      conversation_manager_add_response(manager, msg);
      BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline cancel-all.");
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
      conversation_manager_add_response(manager,
          is_timer ? "Timer cancelled." : "Alarm cancelled.");
    } else {
      conversation_manager_add_response(manager,
          is_timer ? "You have no timer to cancel."
                   : "You have no alarm to cancel.");
    }
    BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline cancel command.");
    return true;
  }

  // --- Set timer / alarm (B1 looser phrasing) / set reminder (C2) ----------
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

    // C2 variant: "set a reminder ..."
    if (prv_eq(tl.tokens[j], "reminder")) {
      j++;
      // Skip optional "to" / "for" / "about"
      if (j < tl.count &&
          (prv_eq(tl.tokens[j], "to") || prv_eq(tl.tokens[j], "for") ||
           prv_eq(tl.tokens[j], "about"))) {
        j++;
      }
      return prv_match_reminder(manager, &tl, j);
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
    char name_buf[REMINDER_TEXT_SIZE] = {0};
    if (name_end > name_start) {
      prv_build_name(&tl, name_start, name_end, name_buf, REMINDER_TEXT_SIZE);
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
      int result = alarm_manager_add_alarm(when, true, name, true);
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
      int result = alarm_manager_add_alarm(when, false, name, true);
      if (result != 0) {
        conversation_manager_add_response(manager, "Sorry, I couldn't set that alarm.");
        return true;
      }
      char timestr[16];
      prv_format_clock(when, timestr, sizeof(timestr));
      char msg[64];
      snprintf(msg, sizeof(msg), "Alarm set for %s.", timestr);
      conversation_manager_add_response(manager, msg);
      BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline alarm for %d.", (int)when);
      return true;
    }
  }

  // --- C2: "remind me ..." -------------------------------------------------
  if (prv_eq(tl.tokens[i], "remind")) {
    int j = i + 1;
    if (j >= tl.count) return false;
    if (!prv_eq(tl.tokens[j], "me")) return false;
    j++;
    return prv_match_reminder(manager, &tl, j);
  }

  // --- Unit Converter (offline) -------------------------------------------
  if (i < tl.count && prv_eq(tl.tokens[i], "convert")) {
    if (prv_match_unit_convert(result_buf, result_size, &tl, i)) {
      if (out_type) *out_type = OC_TYPE_UNIT_CONVERT;
      return true;
    }
  }
  // "how many UNIT in VALUE UNIT" — unit convert wins over B2 for "how many"
  if (i < tl.count && prv_eq(tl.tokens[i], "how") && i + 1 < tl.count &&
      prv_eq(tl.tokens[i + 1], "many")) {
    if (prv_match_unit_convert(result_buf, result_size, &tl, i)) {
      if (out_type) *out_type = OC_TYPE_UNIT_CONVERT;
      return true;
    }
    // fall through to B2 for "how many timers/alarms"
  }
  // "VALUE UNIT to UNIT" / "VALUE UNIT in UNIT" direct form
  {
    int uc_i = i;
    if (prv_match_unit_convert(result_buf, result_size, &tl, uc_i)) {
      if (out_type) *out_type = OC_TYPE_UNIT_CONVERT;
      return true;
    }
  }

  // --- Timezone Query (must come before B4 which also catches "time") ------
  if ((prv_contains_token(&tl, i, "time") || prv_contains_token(&tl, i, "clock")) &&
      prv_contains_token(&tl, i, "in")) {
    if (prv_match_timezone(result_buf, result_size, &tl, i)) {
      if (out_type) *out_type = OC_TYPE_TIMEZONE;
      return true;
    }
  }

  // --- Weather Query -------------------------------------------------------
  if (prv_contains_token(&tl, i, "weather") || prv_contains_token(&tl, i, "forecast") ||
      prv_contains_token(&tl, i, "temperature") || prv_contains_token(&tl, i, "rain") ||
      prv_contains_token(&tl, i, "sunny") || prv_contains_token(&tl, i, "raining")) {
    if (prv_match_weather(result_buf, result_size, &tl, i)) {
      if (out_type) *out_type = OC_TYPE_WEATHER;
      return true;
    }
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
        conversation_manager_add_response(manager, msg);
        BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline list command.");
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
        conversation_manager_add_response(manager, msg);
        BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline battery query.");
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
        conversation_manager_add_response(manager, msg);
        BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline date query.");
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
        conversation_manager_add_response(manager, msg);
        BOBBY_LOG(APP_LOG_LEVEL_INFO, "Handled offline time query.");
        return true;
      }
    }
  }

  return false;
}
