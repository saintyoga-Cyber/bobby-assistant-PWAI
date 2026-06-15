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

#include "reminder_window.h"
#include "../util/fonts.h"
#include "../util/style.h"
#include "../util/memory/malloc.h"
#include "../util/memory/sdk.h"

#include <pebble.h>

static uint32_t s_vibe_segments[] = {200, 100, 200, 100, 400, 100, 200};
static VibePattern s_vibe_pattern = {
  .durations = s_vibe_segments,
  .num_segments = ARRAY_LENGTH(s_vibe_segments),
};

typedef struct {
  char *text;
  TextLayer *label_layer;
  TextLayer *text_layer;
  StatusBarLayer *status_bar;
  AppTimer *repeat_timer;
} ReminderWindowData;

static void prv_repeat_vibe(void *context);

static void prv_window_load(Window *window) {
  ReminderWindowData *data = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  const FontsConfig *fonts = fonts_get_config();

  window_set_background_color(window, COLOR_FALLBACK(GColorCyan, GColorWhite));

  data->status_bar = bstatus_bar_layer_create();
  status_bar_layer_set_colors(data->status_bar, GColorClear, GColorBlack);
  layer_add_child(root, status_bar_layer_get_layer(data->status_bar));

  data->label_layer = btext_layer_create(
      GRect(4, STATUS_BAR_LAYER_HEIGHT + 4, bounds.size.w - 8, 24));
  text_layer_set_text(data->label_layer, "Reminder");
  text_layer_set_font(data->label_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(data->label_layer, GTextAlignmentCenter);
  text_layer_set_background_color(data->label_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(data->label_layer));

  int16_t text_y = STATUS_BAR_LAYER_HEIGHT + 32;
  data->text_layer = btext_layer_create(
      GRect(4, text_y, bounds.size.w - 8, bounds.size.h - text_y - 4));
  text_layer_set_text(data->text_layer, data->text ? data->text : "");
  text_layer_set_font(data->text_layer, fonts->content_font);
  text_layer_set_text_alignment(data->text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(data->text_layer, GColorClear);
  text_layer_set_overflow_mode(data->text_layer, GTextOverflowModeWordWrap);
  layer_add_child(root, text_layer_get_layer(data->text_layer));
}

static void prv_handle_dismiss(ClickRecognizerRef recognizer, void *context) {
  window_stack_pop(true);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_handle_dismiss);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_handle_dismiss);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_handle_dismiss);
}

static void prv_repeat_vibe(void *context) {
  Window *window = context;
  ReminderWindowData *data = window_get_user_data(window);
  vibes_enqueue_custom_pattern(s_vibe_pattern);
  data->repeat_timer = app_timer_register(5000, prv_repeat_vibe, window);
}

static void prv_window_appear(Window *window) {
  ReminderWindowData *data = window_get_user_data(window);
  light_enable_interaction();
  vibes_enqueue_custom_pattern(s_vibe_pattern);
  data->repeat_timer = app_timer_register(5000, prv_repeat_vibe, window);
}

static void prv_window_disappear(Window *window) {
  ReminderWindowData *data = window_get_user_data(window);
  vibes_cancel();
  if (data->repeat_timer) {
    app_timer_cancel(data->repeat_timer);
    data->repeat_timer = NULL;
  }
}

static void prv_window_unload(Window *window) {
  ReminderWindowData *data = window_get_user_data(window);
  text_layer_destroy(data->label_layer);
  text_layer_destroy(data->text_layer);
  status_bar_layer_destroy(data->status_bar);
  if (data->text) {
    free(data->text);
  }
  free(data);
  window_destroy(window);
}

void reminder_window_push(const char *text) {
  Window *window = bwindow_create();
  ReminderWindowData *data = bmalloc(sizeof(ReminderWindowData));
  data->text = NULL;
  data->repeat_timer = NULL;
  if (text && text[0] != '\0') {
    size_t len = strlen(text);
    data->text = bmalloc(len + 1);
    strncpy(data->text, text, len + 1);
  }
  window_set_user_data(window, data);
  window_set_window_handlers(window, (WindowHandlers){
    .load = prv_window_load,
    .appear = prv_window_appear,
    .disappear = prv_window_disappear,
    .unload = prv_window_unload,
  });
  window_set_click_config_provider(window, prv_click_config_provider);
  window_stack_push(window, true);
}
