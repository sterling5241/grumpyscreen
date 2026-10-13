#ifndef __DEV_PANEL_H__
#define __DEV_PANEL_H__

#include "button_container.h"
#include "websocket_client.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

// The hidden Developer screen, opened by tapping the version on Settings >
// About seven times. Display holds the settings too rarely changed for the
// Settings tab (rotation, a reset); Tests holds full-screen touch and colour
// checks and a floating screenshot button; System is read-only diagnostics --
// build, host, network and the Moonraker / Klipper connection, refreshed once
// a second while open.
class DevPanel {
 public:
  DevPanel(KWebSocketClient &c, std::mutex &l);
  ~DevPanel();

  void foreground();
  void background();
  // called after a reset changes settings the Settings tab shows
  void set_settings_changed_cb(std::function<void()> cb);

 private:
  static void _handle_back(lv_event_t *event);
  static void _handle_reconnect(lv_event_t *event);
  static void _handle_rotate(lv_event_t *event);
  static void _handle_recalibrate(lv_event_t *event);
  static void _handle_reset(lv_event_t *event);
  static void _reset_confirmed(lv_obj_t *mbox, uint32_t button, void *user_data);
  static void _restart_confirmed(lv_obj_t *mbox, uint32_t button, void *user_data);
  static void _refresh_cb(lv_timer_t *timer);
  static void _monitor_cb(lv_disp_drv_t *drv, uint32_t time, uint32_t px);

  static void _handle_touch_test(lv_event_t *event);
  static void _handle_touch_event(lv_event_t *event);
  static void _handle_touch_clear(lv_event_t *event);
  static void _handle_touch_close(lv_event_t *event);
  static void _handle_color_test(lv_event_t *event);
  static void _handle_color_tap(lv_event_t *event);
  static void _hide_color_hint_cb(lv_timer_t *timer);
  static void _handle_screenshot_switch(lv_event_t *event);
  static void _handle_screenshot_btn(lv_event_t *event);
  static void _hide_toast_cb(lv_timer_t *timer);

  void build_tests_tab();
  void open_touch_test();
  void touch_at(lv_point_t p);
  void clear_touch_dots();
  void close_touch_test();
  void open_color_test();
  void show_color(uint32_t index);
  void close_color_test();
  void show_screenshot_btn(bool show);
  void take_screenshot();
  void show_toast(const std::string &text);

  void build_display_tab();
  void refresh_display_controls();
  void rotate_changed();
  void reset_ui_settings();
  void prompt_restart(const char *message);
  void save_failed();

  lv_obj_t *add_section(const char *title);
  void refresh();
  void refresh_performance();
  void refresh_host();
  void refresh_network();
  void refresh_connection();
  void request_connection_info();
  void refresh_build();

  KWebSocketClient &ws;
  std::mutex &lv_lock;

  lv_obj_t *panel_cont;
  lv_obj_t *tabview;
  lv_obj_t *display_tab;
  lv_obj_t *display_list;
  lv_obj_t *rotate_selector;
  lv_obj_t *rotate_note;
  lv_obj_t *recalibrate_switch;
  lv_obj_t *tests_tab;
  lv_obj_t *tests_list;
  lv_obj_t *screenshot_switch;
  lv_obj_t *system_tab;
  lv_obj_t *system_list;
  lv_obj_t *perf_label;
  lv_obj_t *host_label;
  lv_obj_t *network_label;
  lv_obj_t *connection_label;
  lv_obj_t *reconnect_btn;
  lv_obj_t *build_label;
  ButtonContainer back_btn;

  std::function<void()> settings_changed_cb;

  // the touch test: a full-screen layer, the last touch's crosshair and
  // coordinates, and a trail of dots (oldest dropped past a limit)
  lv_obj_t *touch_layer;
  lv_obj_t *touch_label;
  lv_obj_t *touch_hline;
  lv_obj_t *touch_vline;
  std::deque<lv_obj_t *> touch_dots;
  lv_point_t last_dot;

  // the colour test: one full-screen fill per tap
  lv_obj_t *color_layer;
  lv_obj_t *color_hint;
  lv_timer_t *color_hint_timer;
  uint32_t color_index;

  // the floating screenshot button lives on the top layer, over every screen,
  // and can be dragged out of the way
  lv_obj_t *screenshot_btn;
  bool screenshot_dragged;
  lv_obj_t *toast;
  lv_timer_t *toast_timer;

  lv_timer_t *refresh_timer;
  uint32_t ticks;

  // CPU usage is the change between two /proc/stat samples
  uint64_t last_cpu_total;
  uint64_t last_cpu_idle;

  // the display's own monitor hook, restored when the panel closes
  void (*saved_monitor_cb)(lv_disp_drv_t *, uint32_t, uint32_t);

  // filled from Moonraker replies on the websocket thread, read under lv_lock
  std::string moonraker_version;
  std::string klippy_state;
  std::string klippy_message;
};

#endif // __DEV_PANEL_H__
