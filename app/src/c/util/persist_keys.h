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

#ifndef APP_PERSIST_KEYS_H
#define APP_PERSIST_KEYS_H

// Centralised persist key registry.  Keys can NEVER be renumbered.

// Alarm state (used by alarms/manager.c)
#define PERSIST_KEY_ALARM_COUNT_ONE   1
#define PERSIST_KEY_ALARM_COUNT_TWO   2
#define PERSIST_KEY_ALARM_TIMES       3
#define PERSIST_KEY_ALARM_WAKEUP_IDS  4
#define PERSIST_KEY_ALARM_IS_TIMERS   5
#define PERSIST_KEY_ALARM_NAMES       8

// next key: 15

#endif
