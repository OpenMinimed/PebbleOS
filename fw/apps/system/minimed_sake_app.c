/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_app.h"

#ifdef CONFIG_MINIMED_SAKE

#include "applib/app.h"
#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/recognizer/swipe.h"
#include "applib/ui/ui.h"
#include "kernel/pbl_malloc.h"
#include "popups/minimed_sake_ui.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "system/version.h"

#include <stdio.h>
#include <string.h>

#define SAKE_APP_REFRESH_MS 400

typedef struct {
  Window window;
  TextLayer text;
  AppTimer *timer;
  Recognizer *swipe_recognizer;
  char buf[640];
} MinimedSakeAppData;

static void prv_refresh(MinimedSakeAppData *data) {
  const char *mode = (minimed_sake_get_mode() == MinimedSakeModeDual)
                         ? (minimed_sake_pump_paired() ? "MODE: DUAL (FE81)" : "MODE: DUAL (FE82)")
                         : "MODE: NORMAL";

  // Commit hash of the running build, so a flashed image can be identified on the watch itself.
  const char *ver = TINTIN_METADATA.version_short;
  const bool has_ver = (ver[0] != '\0');

  // Forecast score since boot, pinned here because the session's "rmse ..." ring line scrolls out
  // of the 14-line log on the next reconnect. rmse = forecast RMSE, cf = carry-forward baseline
  // RMSE, both in tenths of mg/dL; n = scored forecasts; "rmse n0" before the first score.
  uint32_t rmse_x10, cf_x10, n;
  minimed_sake_get_pred_score(&rmse_x10, &cf_x10, &n);
  char score[48];
  if (n == 0) {
    snprintf(score, sizeof(score), "rmse n0");
  } else {
    snprintf(score, sizeof(score), "rmse%lu.%lu cf%lu.%lu n%lu", (unsigned long)(rmse_x10 / 10),
             (unsigned long)(rmse_x10 % 10), (unsigned long)(cf_x10 / 10),
             (unsigned long)(cf_x10 % 10), (unsigned long)n);
  }

  snprintf(data->buf, sizeof(data->buf), "%s%s%s\n%s\n%s", has_ver ? ver : "", has_ver ? " " : "",
           mode, score, minimed_sake_get_log());
  text_layer_set_text(&data->text, data->buf);
}

static void prv_timer_cb(void *context) {
  MinimedSakeAppData *data = app_state_get_user_data();
  prv_refresh(data);
  data->timer = app_timer_register(SAKE_APP_REFRESH_MS, prv_timer_cb, NULL);
}

// SELECT toggles Normal<->Dual. Back is intentionally left unhandled so it exits the app to the
// launcher -- never trap the user out of the system menus.
static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  minimed_sake_toggle_mode();
  prv_refresh(app_state_get_user_data());
}

// DOWN forgets the pump pairing (back to FE82 first-pair advertising) -- for when the "Mobile PB"
// device has been removed on the pump and reconnect can never succeed.
static void prv_down_click(ClickRecognizerRef recognizer, void *context) {
  minimed_sake_forget_pump();
  prv_refresh(app_state_get_user_data());
}

// Right swipe = swipe-back: acts like the physical Back button and exits the app.
static void prv_swipe_handler(const Recognizer *recognizer, RecognizerEvent event) {
  if ((event == RecognizerEvent_Completed) &&
      (swipe_recognizer_get_direction(recognizer) == SwipeDirection_Right)) {
    app_window_stack_pop(true);
  }
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click);
}

static void prv_window_load(Window *window) {
  MinimedSakeAppData *data = app_state_get_user_data();
  Layer *root = &window->layer;
  text_layer_init(&data->text, &root->bounds);
  text_layer_set_font(&data->text, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(root, &data->text.layer);
  // The window owns the recognizer and destroys it on unload.
  data->swipe_recognizer = swipe_recognizer_create(prv_swipe_handler, NULL, SwipeDirection_Right);
  window_attach_recognizer(window, data->swipe_recognizer);
  prv_refresh(data);
}

static void prv_handle_init(void) {
  MinimedSakeAppData *data = app_malloc_check(sizeof(MinimedSakeAppData));
  memset(data, 0, sizeof(*data));
  app_state_set_user_data(data);

  window_init(&data->window, "MiniMed App");
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
  data->swipe_recognizer = NULL;
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
    .name = "MiniMed App",
  };
  return (const PebbleProcessMd *)&s_info;
}

#endif
