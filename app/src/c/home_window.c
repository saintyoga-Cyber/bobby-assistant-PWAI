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

#include "home_window.h"
#include "alarms/manager.h"
#include "converse/voice_window.h"
#include "util/result_window.h"
#include "util/style.h"
#include "util/memory/malloc.h"
#include "util/memory/sdk.h"

#include <pebble.h>

#define MAX_HOME_ITEMS 8
#define LABEL_LEN 48

// Three sections: Speak (0), Timers (1), Alarms (2).
// All data is on-watch — no Bluetooth round-trip on open.

typedef struct {
  Window *window;
  MenuLayer *menu_layer;
  int timer_count;
  int alarm_count;
  char timer_labels[MAX_HOME_ITEMS][LABEL_LEN];
  char alarm_labels[MAX_HOME_ITEMS][LABEL_LEN];
} HomeWindowData;

static void prv_window_load(Window *window);
static void prv_window_unload(Window *window);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void home_window_push(void) {
  Window *window = bwindow_create();
  HomeWindowData *hw = bmalloc(sizeof(HomeWindowData));
  memset(hw, 0, sizeof(HomeWindowData));
  hw->window = window;
  window_set_user_data(window, hw);
  window_set_background_color(window, BRANDED_BACKGROUND_COLOUR);
  window_set_window_handlers(window, (WindowHandlers){
    .load   = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(window, true);
}

// ---------------------------------------------------------------------------
// MenuLayer callbacks
// ---------------------------------------------------------------------------

static uint16_t prv_num_sections(MenuLayer *layer, void *context) {
  return 3;
}

static uint16_t prv_num_rows(MenuLayer *layer, uint16_t section, void *context) {
  HomeWindowData *hw = context;
  switch (section) {
    case 0: return 1;
    case 1: return hw->timer_count > 0 ? (uint16_t)hw->timer_count : 1;
    case 2: return hw->alarm_count > 0 ? (uint16_t)hw->alarm_count : 1;
    default: return 0;
  }
}

static int16_t prv_header_height(MenuLayer *layer, uint16_t section, void *context) {
  return section == 0 ? 0 : MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void prv_draw_header(GContext *ctx, const Layer *cell_layer,
                            uint16_t section, void *context) {
  static const char *titles[] = {NULL, "Timers", "Alarms"};
  if (section == 0) return;
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_fill_color(ctx, ACCENT_COLOUR);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, titles[section],
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(5, 1, bounds.size.w - 5, bounds.size.h - 1),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void prv_draw_row(GContext *ctx, const Layer *cell_layer,
                         MenuIndex *index, void *context) {
  HomeWindowData *hw = context;
  const char *text = "";

  switch (index->section) {
    case 0:
      text = "Speak";
      break;
    case 1:
      text = hw->timer_count > 0 ? hw->timer_labels[index->row] : "No timers";
      break;
    case 2:
      text = hw->alarm_count > 0 ? hw->alarm_labels[index->row] : "No alarms";
      break;
    default:
      break;
  }

  menu_cell_basic_draw(ctx, cell_layer, text, NULL, NULL);
}

static void prv_select_click(MenuLayer *layer, MenuIndex *index, void *context) {
  HomeWindowData *hw = context;
  switch (index->section) {
    case 0:
      voice_window_push();
      break;
    case 1:
      if (hw->timer_count > 0 && index->row < (uint16_t)hw->timer_count) {
        result_window_push("Timer", hw->timer_labels[index->row], 0);
      }
      break;
    case 2:
      if (hw->alarm_count > 0 && index->row < (uint16_t)hw->alarm_count) {
        result_window_push("Alarm", hw->alarm_labels[index->row], 0);
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

static void prv_window_load(Window *window) {
  HomeWindowData *hw = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  // Enumerate timers and alarms from the on-watch alarm_manager.
  int total = alarm_manager_get_alarm_count();
  time_t now = time(NULL);
  for (int i = 0; i < total; i++) {
    Alarm *a = alarm_manager_get_alarm(i);
    time_t when = alarm_get_time(a);
    char *name = alarm_get_name(a);
    if (alarm_is_timer(a)) {
      if (hw->timer_count >= MAX_HOME_ITEMS) continue;
      int idx = hw->timer_count;
      if (name && name[0]) {
        snprintf(hw->timer_labels[idx], LABEL_LEN, "%s", name);
      } else {
        int remaining = (int)(when - now);
        if (remaining < 0) remaining = 0;
        int m = remaining / 60;
        int s = remaining % 60;
        if (m > 0) {
          snprintf(hw->timer_labels[idx], LABEL_LEN, "%d min remaining", m);
        } else {
          snprintf(hw->timer_labels[idx], LABEL_LEN, "%d sec remaining", s);
        }
      }
      hw->timer_count++;
    } else {
      if (hw->alarm_count >= MAX_HOME_ITEMS) continue;
      int idx = hw->alarm_count;
      struct tm lt = *localtime(&when);
      char time_buf[12];
      strftime(time_buf, sizeof(time_buf), "%l:%M %p", &lt);
      if (name && name[0]) {
        snprintf(hw->alarm_labels[idx], LABEL_LEN, "%s", name);
      } else {
        snprintf(hw->alarm_labels[idx], LABEL_LEN, "At %s", time_buf);
      }
      hw->alarm_count++;
    }
  }

  hw->menu_layer = bmenu_layer_create(bounds);
  menu_layer_set_callbacks(hw->menu_layer, hw, (MenuLayerCallbacks){
    .get_num_sections  = prv_num_sections,
    .get_num_rows      = prv_num_rows,
    .get_header_height = prv_header_height,
    .draw_header       = prv_draw_header,
    .draw_row          = prv_draw_row,
    .select_click      = prv_select_click,
  });
  menu_layer_set_normal_colors(hw->menu_layer, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(hw->menu_layer, ACCENT_COLOUR, GColorWhite);
#if defined(PBL_ROUND)
  menu_layer_set_center_focused(hw->menu_layer, true);
#endif
  menu_layer_set_click_config_onto_window(hw->menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(hw->menu_layer));
}

static void prv_window_unload(Window *window) {
  HomeWindowData *hw = window_get_user_data(window);
  menu_layer_destroy(hw->menu_layer);
  free(hw);
  window_destroy(window);
}
