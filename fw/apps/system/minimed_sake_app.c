/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_app.h"

#ifdef CONFIG_MINIMED_SAKE_SPIKE

#include "applib/app.h"
#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/recognizer/swipe.h"
#include "applib/ui/ui.h"
#include "kernel/pbl_malloc.h"
#include "pbl/services/bluetooth/bluetooth_persistent_storage.h"
#include "popups/minimed_sake_spike_ui.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"

#include <stdio.h>
#include <string.h>

#define SAKE_APP_REFRESH_MS 400

//! Re-read the bond inventory every Nth refresh. Reading it opens the bonding settings file, so
//! doing it at the full 400 ms rate would mean flash reads 2.5x a second for the whole time this
//! app is foreground. Bond state changes only on a pair/forget, so a few seconds of lag on a
//! diagnostic line costs nothing.
#define SAKE_APP_BOND_REFRESH_EVERY 10

typedef struct {
  Window window;
  TextLayer text;
  AppTimer *timer;
  Recognizer *swipe_recognizer;
  char buf[384];
  uint8_t bond_refresh_countdown;
  uint8_t bond_gateway;
  uint8_t bond_non_gateway;
  uint8_t bond_deleted;
} MinimedSakeAppData;

static void prv_refresh(MinimedSakeAppData *data) {
  const char *mode = (minimed_sake_get_mode() == MinimedSakeModeDual)
                         ? (minimed_sake_pump_paired() ? "MODE: DUAL (FE81)" : "MODE: DUAL (FE82)")
                         : "MODE: NORMAL";

  // Bond inventory: gw = phone bonds, pmp = pump (non-gateway) bonds, del = non-gateway bonds
  // deleted since boot. "FE81" above with pmp0 is the FE81/FE82 mismatch; pmp0 with del1 means
  // something pruned the pump bond; pmp0 with del0 means it was never stored.
  if (data->bond_refresh_countdown == 0) {
    bt_persistent_storage_get_ble_bonding_counts(&data->bond_gateway, &data->bond_non_gateway,
                                                 &data->bond_deleted);
    data->bond_refresh_countdown = SAKE_APP_BOND_REFRESH_EVERY;
  }
  data->bond_refresh_countdown--;

  snprintf(data->buf, sizeof(data->buf), "%s\nbond gw%u pmp%u del%u\n%s", mode, data->bond_gateway,
           data->bond_non_gateway, data->bond_deleted, minimed_sake_get_log());
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
  data->swipe_recognizer =
      swipe_recognizer_create(prv_swipe_handler, NULL, SwipeDirection_Right);
  window_attach_recognizer(window, data->swipe_recognizer);
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
      .name = "SAKE Spike",
  };
  return (const PebbleProcessMd *)&s_info;
}

#endif
