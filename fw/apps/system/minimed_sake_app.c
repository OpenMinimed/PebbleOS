/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_app.h"

#ifdef CONFIG_MINIMED_SAKE_SPIKE

#include "applib/app.h"
#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/ui.h"
#include "kernel/pbl_malloc.h"
#include "popups/minimed_sake_spike_ui.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"

#include <stdio.h>
#include <string.h>

#define SAKE_APP_REFRESH_MS 400

typedef struct {
  Window window;
  TextLayer text;
  AppTimer *timer;
  char buf[384];
} MinimedSakeAppData;

static void prv_refresh(MinimedSakeAppData *data) {
  const char *mode =
      (minimed_sake_get_mode() == MinimedSakeModeSpike) ? "MODE: SPIKE" : "MODE: NORMAL";
  snprintf(data->buf, sizeof(data->buf), "%s\n%s", mode, minimed_sake_get_log());
  text_layer_set_text(&data->text, data->buf);
}

static void prv_timer_cb(void *context) {
  MinimedSakeAppData *data = app_state_get_user_data();
  prv_refresh(data);
  data->timer = app_timer_register(SAKE_APP_REFRESH_MS, prv_timer_cb, NULL);
}

// SELECT toggles Normal<->Spike. Back is intentionally left unhandled so it exits the app to the
// launcher -- never trap the user out of the system menus.
static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  minimed_sake_toggle_mode();
  prv_refresh(app_state_get_user_data());
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
}

static void prv_window_load(Window *window) {
  MinimedSakeAppData *data = app_state_get_user_data();
  Layer *root = &window->layer;
  text_layer_init(&data->text, &root->bounds);
  text_layer_set_font(&data->text, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(root, &data->text.layer);
  prv_refresh(data);
}

static void prv_handle_init(void) {
  MinimedSakeAppData *data = app_malloc_check(sizeof(MinimedSakeAppData));
  memset(data, 0, sizeof(*data));
  app_state_set_user_data(data);

  window_init(&data->window, "SAKE Spike");
  window_set_window_handlers(&data->window, &(WindowHandlers){
                                                .load = prv_window_load,
                                            });
  window_set_click_config_provider(&data->window, prv_click_config);
  app_window_stack_push(&data->window, true /* animated */);

  data->timer = app_timer_register(SAKE_APP_REFRESH_MS, prv_timer_cb, NULL);
}

static void prv_handle_deinit(void) {
  MinimedSakeAppData *data = app_state_get_user_data();
  if (data->timer) {
    app_timer_cancel(data->timer);
  }
  app_free(data);
}

static void s_main(void) {
  prv_handle_init();
  app_event_loop();
  prv_handle_deinit();
}

const PebbleProcessMd *minimed_sake_app_get_info(void) {
  static const PebbleProcessMdSystem s_info = {
      .common.main_func = s_main,
      .name = "SAKE Spike",
  };
  return (const PebbleProcessMd *)&s_info;
}

#endif
