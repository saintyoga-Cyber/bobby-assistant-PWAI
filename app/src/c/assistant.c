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

#include "converse/voice_window.h"
#include "home_window.h"
#include "alarms/manager.h"
#include "util/fonts.h"
#include "util/memory/pressure.h"

#include <pebble.h>
#include <pebble-events/pebble-events.h>

static void prv_init(void) {
  memory_pressure_init();
  fonts_load();
  // Must request buffer sizes before opening AppMessage, otherwise the
  // default buffers are too small for note/alarm/reminder payloads.
  events_app_message_request_inbox_size(1024);
  events_app_message_request_outbox_size(1024);
  events_app_message_open();
  alarm_manager_init();
}

static void prv_deinit(void) {
  fonts_unload();
}

int main(void) {
  prv_init();
  if (!alarm_manager_maybe_alarm()) {
    if (launch_reason() == APP_LAUNCH_USER) {
      home_window_push();
    } else {
      voice_window_push();
    }
  }
  app_event_loop();
  prv_deinit();
}
