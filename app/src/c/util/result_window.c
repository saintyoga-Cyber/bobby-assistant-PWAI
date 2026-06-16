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

#include "result_window.h"
#include "fonts.h"
#include "style.h"
#include "memory/malloc.h"
#include "memory/sdk.h"

#include <pebble.h>

typedef struct {
  TextLayer *title_layer;
  TextLayer *text_layer;
  StatusBarLayer *status_bar;
  char *title_text;
  char *text_text;
  AppTimer *timer;
} ResultWindowData;

static void prv_window_load(Window *window);
static void prv_window_unload(Window *window);
static void prv_window_appear(Window *window);
static void prv_timer_expired(void *context);

void result_window_push(const char *title, const char *text) {
  Window *window = bwindow_create();
  ResultWindowData *data = bmalloc(sizeof(ResultWindowData));
  memset(data, 0, sizeof(ResultWindowData));
  data->title_text = bmalloc(strlen(title) + 1);
  strncpy(data->title_text, title, strlen(title) + 1);
  data->text_text = bmalloc(strlen(text) + 1);
  strncpy(data->text_text, text, strlen(text) + 1);
  window_set_user_data(window, data);
  window_set_background_color(window, BRANDED_BACKGROUND_COLOUR);
  window_set_window_handlers(window, (WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload,
    .appear = prv_window_appear,
  });
  window_stack_push(window, true);
}

static void prv_window_load(Window *window) {
  ResultWindowData *data = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  const FontsConfig *fonts = fonts_get_config();

  int16_t side = PBL_IF_ROUND_ELSE(30, 4);
  int16_t text_w = bounds.size.w - side * 2;

  int16_t content_y;
#if defined(PBL_ROUND)
  // No status bar on round — use full height and vertically centre the block.
  int16_t block_h = (int16_t)(fonts->title_font_cap + 8 + fonts->text_font_cap * 2 + 4);
  content_y = (int16_t)((bounds.size.h - block_h) / 2);
  if (content_y < 4) content_y = 4;
#else
  data->status_bar = bstatus_bar_layer_create();
  bobby_status_bar_result_pane_config(data->status_bar);
  layer_add_child(root, status_bar_layer_get_layer(data->status_bar));
  content_y = STATUS_BAR_LAYER_HEIGHT + 8;
#endif

  data->title_layer = btext_layer_create(
      GRect(side, content_y, text_w, fonts->title_font_cap + 4));
  text_layer_set_background_color(data->title_layer, GColorClear);
  text_layer_set_font(data->title_layer, fonts->title_font);
  text_layer_set_text_alignment(data->title_layer, GTextAlignmentCenter);
  text_layer_set_text(data->title_layer, data->title_text);
  layer_add_child(root, text_layer_get_layer(data->title_layer));

  int16_t text_y = content_y + fonts->title_font_cap + 8;
  data->text_layer = btext_layer_create(
      GRect(side, text_y, text_w, bounds.size.h - text_y - side / 2));
  text_layer_set_background_color(data->text_layer, GColorClear);
  text_layer_set_font(data->text_layer, fonts->text_font);
  text_layer_set_text_alignment(data->text_layer, GTextAlignmentCenter);
  text_layer_set_text(data->text_layer, data->text_text);
  layer_add_child(root, text_layer_get_layer(data->text_layer));
}

static void prv_window_unload(Window *window) {
  ResultWindowData *data = window_get_user_data(window);
  if (data->timer) {
    app_timer_cancel(data->timer);
  }
  text_layer_destroy(data->title_layer);
  text_layer_destroy(data->text_layer);
  if (data->status_bar) {
    status_bar_layer_destroy(data->status_bar);
  }
  free(data->title_text);
  free(data->text_text);
  free(data);
  window_destroy(window);
}

static void prv_window_appear(Window *window) {
  ResultWindowData *data = window_get_user_data(window);
  if (data->timer) {
    app_timer_cancel(data->timer);
  }
  data->timer = app_timer_register(4000, prv_timer_expired, window);
}

static void prv_timer_expired(void *context) {
  window_stack_remove(context, true);
}
