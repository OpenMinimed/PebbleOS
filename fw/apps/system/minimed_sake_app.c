/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_app.h"

#ifdef CONFIG_MINIMED_SAKE

#include "applib/app.h"
#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/dialogs/confirmation_dialog.h"
#include "applib/ui/menu_layer.h"
#include "applib/ui/recognizer/swipe.h"
#include "applib/ui/ui.h"
#include "kernel/pbl_malloc.h"
#include "minimed_settings.h"
#include "pbl/services/bluetooth/bluetooth_persistent_storage.h"
#include "pbl/util/size.h"
#include "popups/minimed_sake_ui.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "shell/prefs.h"
#include "system/version.h"

#include <stdio.h>
#include <string.h>

#define SAKE_APP_REFRESH_MS 400

//! Re-read the bond inventory every Nth refresh. Reading it opens the bonding settings file, so
//! doing it at the full 400 ms rate would mean flash reads 2.5x a second for the whole time the
//! log is open. Bond state changes only on a pair/forget, so a few seconds of lag on a
//! diagnostic line costs nothing.
#define SAKE_APP_BOND_REFRESH_EVERY 10

//! One on/off row in a submenu: a bit in the alerts or the features settings mask.
typedef struct {
  const char *title;
  bool is_feature;
  uint8_t bit;
} ToggleItem;

typedef struct {
  const ToggleItem *items;
  uint16_t num_items;
} ToggleList;

static const ToggleItem s_pump_alert_items[] = {
  {"Low", false, MINIMED_ALERT_LOW},
  {"Other", false, MINIMED_ALERT_OTHER},
};
static const ToggleList s_pump_alerts = {s_pump_alert_items, ARRAY_LENGTH(s_pump_alert_items)};

typedef struct {
  Window window;
  MenuLayer menu;
  const ToggleList *list;
} ToggleWindow;

typedef struct {
  Window window;
  TextLayer text;
  AppTimer *timer;
  Recognizer *swipe_recognizer;
  char buf[640];
  uint8_t bond_refresh_countdown;
  uint8_t bond_gateway;
  uint8_t bond_non_gateway;
  uint8_t bond_deleted;
} LogWindow;

typedef struct {
  Window window;
  MenuLayer menu;
  char pump_alerts_summary[32];
  ToggleWindow toggle;
  LogWindow log;
} MinimedSakeAppData;

static bool prv_item_on(const ToggleItem *item) {
  const uint8_t mask =
      item->is_feature ? minimed_settings_get_features() : minimed_settings_get_alerts();
  return (mask & item->bit) != 0;
}

static void prv_item_toggle(const ToggleItem *item) {
  if (item->is_feature) {
    minimed_settings_set_features(minimed_settings_get_features() ^ item->bit);
  } else {
    minimed_settings_set_alerts(minimed_settings_get_alerts() ^ item->bit);
  }
}

static void prv_init_menu(MenuLayer *menu, Window *window, void *context,
                          const MenuLayerCallbacks *callbacks) {
  menu_layer_init(menu, &window->layer.bounds);
  menu_layer_set_callbacks(menu, context, callbacks);
  const GColor highlight_bg = shell_prefs_get_theme_highlight_color();
  menu_layer_set_highlight_colors(menu, highlight_bg, gcolor_legible_over(highlight_bg));
  menu_layer_set_click_config_onto_window(menu, window);
  // Follow the system's menu settings, like the launcher and Settings do.
  menu_layer_set_scroll_wrap_around(menu, shell_prefs_get_menu_scroll_wrap_around_enable());
  menu_layer_set_scroll_vibe_on_wrap(
      menu, shell_prefs_get_menu_scroll_vibe_behavior() == MenuScrollVibeOnWrapAround);
  menu_layer_set_scroll_vibe_on_blocked(
      menu, shell_prefs_get_menu_scroll_vibe_behavior() == MenuScrollVibeOnLocked);
  layer_add_child(&window->layer, menu_layer_get_layer(menu));
}

static void prv_window_init(Window *window, const char *name, void *data, WindowHandlers handlers) {
  window_init(window, name);
  window_set_user_data(window, data);
  window_set_window_handlers(window, &handlers);
}

// Log window
//////////////

static void prv_log_refresh(LogWindow *log) {
  const char *mode = (minimed_sake_get_mode() == MinimedSakeModeDual)
                         ? (minimed_sake_pump_paired() ? "MODE: DUAL (FE81)" : "MODE: DUAL (FE82)")
                         : "MODE: NORMAL";

  // Commit hash of the running build, so a flashed image can be identified on the watch itself.
  const char *ver = TINTIN_METADATA.version_short;
  const bool has_ver = (ver[0] != '\0');

  // Bond inventory: gw = phone bonds, pmp = pump (non-gateway) bonds, del = non-gateway bonds
  // deleted since boot. "FE81" above with pmp0 is the FE81/FE82 mismatch; pmp0 with del1 means
  // something pruned the pump bond; pmp0 with del0 means it was never stored.
  if (log->bond_refresh_countdown == 0) {
    bt_persistent_storage_get_ble_bonding_counts(&log->bond_gateway, &log->bond_non_gateway,
                                                 &log->bond_deleted);
    log->bond_refresh_countdown = SAKE_APP_BOND_REFRESH_EVERY;
  }
  log->bond_refresh_countdown--;

  snprintf(log->buf, sizeof(log->buf), "%s%s%s\nbond gw%u pmp%u del%u\n%s", has_ver ? ver : "",
           has_ver ? " " : "", mode, log->bond_gateway, log->bond_non_gateway, log->bond_deleted,
           minimed_sake_get_log());
  text_layer_set_text(&log->text, log->buf);
}

static void prv_log_timer_cb(void *context) {
  LogWindow *log = context;
  prv_log_refresh(log);
  log->timer = app_timer_register(SAKE_APP_REFRESH_MS, prv_log_timer_cb, log);
}

// Right swipe = swipe-back: acts like the physical Back button. The menus get this from MenuLayer.
static void prv_log_swipe_handler(const Recognizer *recognizer, RecognizerEvent event) {
  if ((event == RecognizerEvent_Completed) &&
      (swipe_recognizer_get_direction(recognizer) == SwipeDirection_Right)) {
    app_window_stack_pop(true);
  }
}

static void prv_log_load(Window *window) {
  LogWindow *log = window_get_user_data(window);
  text_layer_init(&log->text, &window->layer.bounds);
  text_layer_set_font(&log->text, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(&window->layer, &log->text.layer);
  // The window owns the recognizer and destroys it on unload.
  log->swipe_recognizer =
      swipe_recognizer_create(prv_log_swipe_handler, NULL, SwipeDirection_Right);
  window_attach_recognizer(window, log->swipe_recognizer);
  log->bond_refresh_countdown = 0;
  prv_log_refresh(log);
  log->timer = app_timer_register(SAKE_APP_REFRESH_MS, prv_log_timer_cb, log);
}

static void prv_log_unload(Window *window) {
  LogWindow *log = window_get_user_data(window);
  app_timer_cancel(log->timer);
  log->timer = NULL;
  log->swipe_recognizer = NULL;
  text_layer_deinit(&log->text);
}

// Toggle submenu
//////////////////

static uint16_t prv_toggle_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  const ToggleWindow *toggle = context;
  return toggle->list->num_items;
}

static void prv_toggle_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  const ToggleWindow *toggle = context;
  const ToggleItem *item = &toggle->list->items[index->row];
  menu_cell_basic_draw(ctx, cell, item->title, prv_item_on(item) ? "On" : "Off", NULL);
}

static void prv_toggle_select(MenuLayer *menu, MenuIndex *index, void *context) {
  const ToggleWindow *toggle = context;
  prv_item_toggle(&toggle->list->items[index->row]);
  menu_layer_reload_data(menu);
}

static void prv_toggle_load(Window *window) {
  ToggleWindow *toggle = window_get_user_data(window);
  prv_init_menu(&toggle->menu, window, toggle,
                &(MenuLayerCallbacks){
                  .get_num_rows = prv_toggle_num_rows,
                  .draw_row = prv_toggle_draw_row,
                  .select_click = prv_toggle_select,
                });
}

static void prv_toggle_unload(Window *window) {
  ToggleWindow *toggle = window_get_user_data(window);
  menu_layer_deinit(&toggle->menu);
}

static void prv_toggle_push(ToggleWindow *toggle, const ToggleList *list) {
  toggle->list = list;
  app_window_stack_push(&toggle->window, true);
}

// Forget pump confirmation
////////////////////////////

static void prv_forget_confirm(ClickRecognizerRef recognizer, void *context) {
  minimed_sake_forget_pump();
  confirmation_dialog_pop(context);
}

static void prv_forget_decline(ClickRecognizerRef recognizer, void *context) {
  confirmation_dialog_pop(context);
}

static void prv_forget_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_forget_confirm);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_forget_decline);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_forget_decline);
}

// For when the "Mobile PB" device has been removed on the pump and reconnect can never succeed.
static void prv_forget_push(void) {
  ConfirmationDialog *confirmation = confirmation_dialog_create("Forget pump");
  Dialog *dialog = confirmation_dialog_get_dialog(confirmation);
  dialog_set_text(dialog, "Forget pump? Re-pair needed");
  dialog_set_icon(dialog, RESOURCE_ID_GENERIC_WARNING_LARGE);
  confirmation_dialog_set_click_config_provider(confirmation, prv_forget_click_config);
  app_confirmation_dialog_push(confirmation);
}

// Root menu
/////////////

//! A root menu row. A feature that can be compiled out keeps its row and handlers in one #ifdef.
typedef struct {
  const char *title;
  const char *(*subtitle)(MinimedSakeAppData *data); //!< NULL: no subtitle
  void (*select)(MinimedSakeAppData *data);
} RootItem;

static void prv_log_select(MinimedSakeAppData *data) {
  app_window_stack_push(&data->log.window, true);
}

static const char *prv_mode_subtitle(MinimedSakeAppData *data) {
  return (minimed_sake_get_mode() == MinimedSakeModeDual) ? "Dual" : "Normal";
}

static void prv_mode_select(MinimedSakeAppData *data) {
  minimed_sake_toggle_mode();
}

static const char *prv_pump_alerts_subtitle(MinimedSakeAppData *data) {
  char *buf = data->pump_alerts_summary;
  buf[0] = '\0';
  for (uint16_t i = 0; i < s_pump_alerts.num_items; i++) {
    const ToggleItem *item = &s_pump_alerts.items[i];
    if (prv_item_on(item)) {
      const size_t len = strlen(buf);
      snprintf(buf + len, sizeof(data->pump_alerts_summary) - len, "%s%s", len ? ", " : "",
               item->title);
    }
  }
  return buf[0] ? buf : "Off";
}

static void prv_pump_alerts_select(MinimedSakeAppData *data) {
  prv_toggle_push(&data->toggle, &s_pump_alerts);
}

#ifdef CONFIG_MINIMED_HYPO
static const ToggleItem s_hypo_items[] = {
  {"Enabled", true, MINIMED_FEATURE_HYPO},
  {"Alert", false, MINIMED_ALERT_HYPO_MODEL},
  {"Alert in DND", false, MINIMED_ALERT_HYPO_DND_BYPASS},
};
static const ToggleList s_hypo = {s_hypo_items, ARRAY_LENGTH(s_hypo_items)};

static const char *prv_hypo_subtitle(MinimedSakeAppData *data) {
  return prv_item_on(&s_hypo_items[0]) ? "On" : "Off";
}

static void prv_hypo_select(MinimedSakeAppData *data) {
  prv_toggle_push(&data->toggle, &s_hypo);
}
#endif

static void prv_forget_select(MinimedSakeAppData *data) {
  prv_forget_push();
}

static const RootItem s_root_items[] = {
  {"Log", NULL, prv_log_select},
  {"Mode", prv_mode_subtitle, prv_mode_select},
  {"Pump alerts", prv_pump_alerts_subtitle, prv_pump_alerts_select},
#ifdef CONFIG_MINIMED_HYPO
  {"Hypo model", prv_hypo_subtitle, prv_hypo_select},
#endif
  {"Forget pump", NULL, prv_forget_select},
};

static uint16_t prv_root_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  return ARRAY_LENGTH(s_root_items);
}

static void prv_root_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  const RootItem *item = &s_root_items[index->row];
  menu_cell_basic_draw(ctx, cell, item->title, item->subtitle ? item->subtitle(context) : NULL,
                       NULL);
}

static void prv_root_select(MenuLayer *menu, MenuIndex *index, void *context) {
  s_root_items[index->row].select(context);
  menu_layer_reload_data(menu);
}

static void prv_root_load(Window *window) {
  MinimedSakeAppData *data = window_get_user_data(window);
  prv_init_menu(&data->menu, window, data,
                &(MenuLayerCallbacks){
                  .get_num_rows = prv_root_num_rows,
                  .draw_row = prv_root_draw_row,
                  .select_click = prv_root_select,
                });
}

// Back from a submenu: its subtitle may have changed.
static void prv_root_appear(Window *window) {
  MinimedSakeAppData *data = window_get_user_data(window);
  menu_layer_reload_data(&data->menu);
}

static void prv_root_unload(Window *window) {
  MinimedSakeAppData *data = window_get_user_data(window);
  menu_layer_deinit(&data->menu);
}

static void prv_handle_init(void) {
  MinimedSakeAppData *data = app_malloc_check(sizeof(MinimedSakeAppData));
  memset(data, 0, sizeof(*data));
  app_state_set_user_data(data);

  prv_window_init(&data->log.window, WINDOW_NAME("MiniMed Log"), &data->log,
                  (WindowHandlers){.load = prv_log_load, .unload = prv_log_unload});
  prv_window_init(&data->toggle.window, WINDOW_NAME("MiniMed Toggles"), &data->toggle,
                  (WindowHandlers){.load = prv_toggle_load, .unload = prv_toggle_unload});
  prv_window_init(&data->window, WINDOW_NAME("MiniMed App"), data,
                  (WindowHandlers){
                    .load = prv_root_load,
                    .appear = prv_root_appear,
                    .unload = prv_root_unload,
                  });
  app_window_stack_push(&data->window, true /* animated */);
}

static void prv_handle_deinit(void) {
  app_free(app_state_get_user_data());
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
