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

#include "voice_window.h"
#include "offline_commands.h"
#include "../util/result_window.h"
#include "../util/fonts.h"
#include "../util/style.h"
#include "../util/logging.h"
#include "../util/memory/malloc.h"
#include "../util/memory/sdk.h"
#include "../vibes/haptic_feedback.h"

#include <pebble.h>
#include <pebble-events/pebble-events.h>

#define RESULT_BUF_SIZE 160

typedef struct {
  Window *window;
  DictationSession *dictation;
  TextLayer *status_layer;
  EventHandle app_message_handle;
  bool waiting_for_phone;
  bool dictation_pending;
  bool waiting_for_weather;
  bool waiting_for_tz;
} VoiceWindow;

static void prv_start_dictation(VoiceWindow *vw);
static void prv_dictation_callback(DictationSession *session,
                                   DictationSessionStatus status,
                                   char *transcription, void *context);
static void prv_app_message_received(DictionaryIterator *iter, void *context);
static void prv_set_status(VoiceWindow *vw, const char *text);
static void prv_window_load(Window *window);
static void prv_window_unload(Window *window);
static void prv_window_appear(Window *window);
static void prv_click_config_provider(void *context);
static void prv_select_clicked(ClickRecognizerRef recognizer, void *context);

void voice_window_push(void) {
  Window *window = bwindow_create();
  VoiceWindow *vw = bmalloc(sizeof(VoiceWindow));
  memset(vw, 0, sizeof(VoiceWindow));
  vw->window = window;
  window_set_user_data(window, vw);
  window_set_background_color(window, BRANDED_BACKGROUND_COLOUR);
  window_set_window_handlers(window, (WindowHandlers){
    .load   = prv_window_load,
    .appear = prv_window_appear,
    .unload = prv_window_unload,
  });
  window_stack_push(window, true);
}

static void prv_window_load(Window *window) {
  VoiceWindow *vw = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  const FontsConfig *fonts = fonts_get_config();

  // Centred status text (shows "Listening…", "Saving…", etc.)
  vw->status_layer = btext_layer_create(
      GRect(4, bounds.size.h / 2 - fonts->title_font_cap,
            bounds.size.w - 8, fonts->title_font_cap * 2 + 4));
  text_layer_set_background_color(vw->status_layer, GColorClear);
  text_layer_set_font(vw->status_layer, fonts->title_font);
  text_layer_set_text_alignment(vw->status_layer, GTextAlignmentCenter);
  text_layer_set_text(vw->status_layer, "Listening…");
  layer_add_child(root, text_layer_get_layer(vw->status_layer));

  vw->dictation = dictation_session_create(0, prv_dictation_callback, vw);
  dictation_session_enable_confirmation(vw->dictation, false);

  vw->app_message_handle = events_app_message_register_inbox_received(
      prv_app_message_received, vw);

  // Auto-start dictation exactly once, when the window first appears.
  // Subsequent appears (e.g. when the dictation modal closes) must NOT
  // restart it, or we'd loop forever on the mic screen. Re-dictation
  // after that happens only via the SELECT button.
  vw->dictation_pending = true;

  window_set_click_config_provider_with_context(window, prv_click_config_provider, vw);
}

static void prv_window_appear(Window *window) {
  VoiceWindow *vw = window_get_user_data(window);
  if (vw->dictation_pending) {
    vw->dictation_pending = false;
    prv_start_dictation(vw);
  }
}

static void prv_window_unload(Window *window) {
  VoiceWindow *vw = window_get_user_data(window);
  dictation_session_destroy(vw->dictation);
  events_app_message_unsubscribe(vw->app_message_handle);
  text_layer_destroy(vw->status_layer);
  free(vw);
  window_destroy(window);
}

static void prv_start_dictation(VoiceWindow *vw) {
  vw->waiting_for_phone = false;
  prv_set_status(vw, "Listening…");
  // Dictation needs a large transient allocation; nudge the heap first.
  free(bmalloc(2048));
  dictation_session_start(vw->dictation);
}

static void prv_set_status(VoiceWindow *vw, const char *text) {
  text_layer_set_text(vw->status_layer, text);
}

static void prv_dictation_callback(DictationSession *session,
                                   DictationSessionStatus status,
                                   char *transcription, void *context) {
  VoiceWindow *vw = context;

  if (status != DictationSessionStatusSuccess) {
    if (status == DictationSessionStatusFailureTranscriptionRejected) {
      // User backed out of the dictation UI — exit the app.
      window_stack_pop(true);
    } else {
      // Mic / network / no-speech error — offer retry via SELECT.
      prv_set_status(vw, "Tap to retry");
    }
    return;
  }

  BOBBY_LOG(APP_LOG_LEVEL_INFO, "Dictation: \"%s\"", transcription);

  char result[RESULT_BUF_SIZE];
  result[0] = '\0';

  OfflineCommandType oc_type;
  if (offline_commands_try(transcription, result, sizeof(result), &oc_type)) {
    // Weather and timezone require a phone round-trip; result holds the query.
    if (oc_type == OC_TYPE_WEATHER) {
      DictionaryIterator *iter;
      if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        result_window_push("PWAI", "Weather unavailable.", 0);
        window_stack_remove(vw->window, false);
        return;
      }
      // result[0] is '0' (today) or '1' (tomorrow)
      int8_t day_offset = (result[0] == '1') ? 1 : 0;
      dict_write_int(iter, MESSAGE_KEY_WEATHER_REQUEST, &day_offset, sizeof(int8_t), true);
      if (app_message_outbox_send() != APP_MSG_OK) {
        result_window_push("PWAI", "Weather unavailable.", 0);
        window_stack_remove(vw->window, false);
        return;
      }
      vw->waiting_for_phone = true;
      vw->waiting_for_weather = true;
      prv_set_status(vw, "Fetching weather\xe2\x80\xa6");
      return;
    }
    if (oc_type == OC_TYPE_TIMEZONE) {
      DictionaryIterator *iter;
      if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        result_window_push("PWAI", "Couldn't look up time.", 0);
        window_stack_remove(vw->window, false);
        return;
      }
      dict_write_cstring(iter, MESSAGE_KEY_TZ_QUERY, result);
      if (app_message_outbox_send() != APP_MSG_OK) {
        result_window_push("PWAI", "Couldn't look up time.", 0);
        window_stack_remove(vw->window, false);
        return;
      }
      vw->waiting_for_phone = true;
      vw->waiting_for_tz = true;
      prv_set_status(vw, "Looking up time\xe2\x80\xa6");
      return;
    }
    // Pure offline result.
    uint32_t icon = 0;
    if (oc_type == OC_TYPE_TIMER || oc_type == OC_TYPE_ALARM) {
      icon = RESOURCE_ID_ICON_TIMER;
    } else if (oc_type == OC_TYPE_REMINDER) {
      icon = RESOURCE_ID_ICON_REMINDER;
    }
    vibe_haptic_feedback();
    result_window_push("PWAI", result, icon);
    window_stack_remove(vw->window, false);
    return;
  }

  // Not an offline command — send to phone as a note.
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    result_window_push("PWAI", "Couldn't save note", RESOURCE_ID_ICON_NOTE);
    window_stack_remove(vw->window, false);
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_NOTE_TEXT, transcription);
  if (app_message_outbox_send() != APP_MSG_OK) {
    result_window_push("PWAI", "Couldn't save note", RESOURCE_ID_ICON_NOTE);
    window_stack_remove(vw->window, false);
    return;
  }
  vw->waiting_for_phone = true;
  prv_set_status(vw, "Saving\xe2\x80\xa6");
}

static void prv_app_message_received(DictionaryIterator *iter, void *context) {
  VoiceWindow *vw = context;
  if (!vw->waiting_for_phone) {
    return;
  }

  Tuple *t;

  t = dict_find(iter, MESSAGE_KEY_WEATHER_RESPONSE);
  if (t && vw->waiting_for_weather) {
    vw->waiting_for_phone = false;
    vw->waiting_for_weather = false;
    result_window_push("PWAI", t->value->cstring, 0);
    window_stack_remove(vw->window, false);
    return;
  }

  t = dict_find(iter, MESSAGE_KEY_TZ_RESPONSE);
  if (t && vw->waiting_for_tz) {
    vw->waiting_for_phone = false;
    vw->waiting_for_tz = false;
    result_window_push("PWAI", t->value->cstring, 0);
    window_stack_remove(vw->window, false);
    return;
  }

  t = dict_find(iter, MESSAGE_KEY_NOTE_SAVED);
  if (t == NULL) {
    return;
  }
  bool ok = (t->value->int8 != 0);
  vw->waiting_for_phone = false;
  result_window_push("PWAI", ok ? "Note saved" : "Couldn't save note", RESOURCE_ID_ICON_NOTE);
  window_stack_remove(vw->window, false);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_clicked);
}

static void prv_select_clicked(ClickRecognizerRef recognizer, void *context) {
  VoiceWindow *vw = context;
  if (!vw->waiting_for_phone) {
    prv_start_dictation(vw);
  }
}
