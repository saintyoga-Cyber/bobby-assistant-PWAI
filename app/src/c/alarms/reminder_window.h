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

#ifndef ALARMS_REMINDER_WINDOW_H
#define ALARMS_REMINDER_WINDOW_H

#include <pebble.h>

// Push a notification-style window for a wakeup reminder.
// text is the reminder message (e.g. "buy milk"). Copied internally.
void reminder_window_push(const char *text);

#endif
