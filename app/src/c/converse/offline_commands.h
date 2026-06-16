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

#ifndef OFFLINE_COMMANDS_H
#define OFFLINE_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  OC_NONE     = 0,
  OC_TYPE_TIMER,
  OC_TYPE_ALARM,
  OC_TYPE_REMINDER,
  OC_TYPE_INFO,
} OfflineCommandType;

// Attempts to handle a transcribed prompt entirely on the watch (timers,
// alarms, reminders, time/date/battery queries), with no round trip to the
// service.  Returns true and writes a human-readable response into result_buf
// if the input was a recognised command; returns false if the caller should
// fall through (e.g. save as a note).  *out_type is set to the matched command
// category (or OC_NONE on no match); pass NULL to ignore.
bool offline_commands_try(const char *input, char *result_buf, size_t result_size,
                          OfflineCommandType *out_type);

#endif
