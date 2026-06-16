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
} VoiceWindow;

static VoiceWindow *s_vw = NULL;

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
  s_vw = vw;
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

  window_set_click_config_provider_with_context(window, prv_click_config_provider, vw);
}

static void prv_window_appear(Window *window) {
  VoiceWindow *vw = window_get_user_data(window);
  // Release dictation memory budget.
  free(bmalloc(2048));
  prv_start_dictation(vw);
}

static void prv_window_unload(Window *window) {
  VoiceWindow *vw = window_get_user_data(window);
  dictation_session_destroy(vw->dictation);
  events_app_message_unsubscribe(vw->app_message_handle);
  text_layer_destroy(vw->status_layer);
  free(vw);
  window_destroy(window);
  s_vw = NULL;
}

static void prv_start_dictation(VoiceWindow *vw) {
  vw->waiting_for_phone = false;
  prv_set_status(vw, "Listening…");
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

  if (offline_commands_try(transcription, result, sizeof(result))) {
    vibe_haptic_feedback();
    result_window_push("PWAI", result);
    window_stack_remove(vw->window, false);
    return;
  }

  // Not an offline command — send to phone as a note.
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    result_window_push("PWAI", "Couldn't save note");
    window_stack_remove(vw->window, false);
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_NOTE_TEXT, transcription);
  if (app_message_outbox_send() != APP_MSG_OK) {
    result_window_push("PWAI", "Couldn't save note");
    window_stack_remove(vw->window, false);
    return;
  }
  vw->waiting_for_phone = true;
  prv_set_status(vw, "Saving…");
}

static void prv_app_message_received(DictionaryIterator *iter, void *context) {
  VoiceWindow *vw = context;
  if (!vw->waiting_for_phone) {
    return;
  }
  Tuple *t = dict_find(iter, MESSAGE_KEY_NOTE_SAVED);
  if (t == NULL) {
    return;
  }
  bool ok = (t->value->int8 != 0);
  vw->waiting_for_phone = false;
  result_window_push("PWAI", ok ? "Note saved" : "Couldn't save note");
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
