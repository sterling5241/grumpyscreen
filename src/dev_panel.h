#ifndef __DEV_PANEL_H__
#define __DEV_PANEL_H__

#include "button_container.h"
#include "websocket_client.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <mutex>
#include <string>

// The hidden Developer screen, opened by tapping the version on Settings >
// About seven times. Read-only diagnostics for now: build, host, network and
// the Moonraker / Klipper connection, refreshed once a second while open.
class DevPanel {
 public:
  DevPanel(KWebSocketClient &c, std::mutex &l);
  ~DevPanel();

  void foreground();
  void background();

 private:
  static void _handle_back(lv_event_t *event);
  static void _handle_reconnect(lv_event_t *event);
  static void _refresh_cb(lv_timer_t *timer);
  static void _monitor_cb(lv_disp_drv_t *drv, uint32_t time, uint32_t px);

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
  lv_obj_t *system_tab;
  lv_obj_t *system_list;
  lv_obj_t *perf_label;
  lv_obj_t *host_label;
  lv_obj_t *network_label;
  lv_obj_t *connection_label;
  lv_obj_t *reconnect_btn;
  lv_obj_t *build_label;
  ButtonContainer back_btn;

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
