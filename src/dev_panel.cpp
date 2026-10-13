#include "dev_panel.h"
#include "config.h"
#include "icons.h"
#include "logger.h"
#include "theme.h"
#include "utils.h"

#include <algorithm>
#include <cstdlib>
#include <experimental/filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/statvfs.h>

namespace fs = std::experimental::filesystem;
using namespace Theme;

namespace {

constexpr uint32_t REFRESH_MS = 1000;
// Moonraker is asked again every few refreshes rather than every second
constexpr uint32_t CONNECTION_EVERY_TICKS = 5;

// the display's monitor hook counts every redraw between two refreshes
uint32_t render_frames = 0;
uint32_t render_time_ms = 0;
uint32_t render_max_ms = 0;

std::string read_first_line(const std::string &path) {
  std::ifstream f(path);
  std::string line;
  if (f.is_open()) std::getline(f, line);
  return line;
}

// recoloured labels treat '#' as a colour command, so text from outside the
// screen (an interface name, a Klipper message) must not carry one
std::string plain(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '#'), s.end());
  return s;
}

// one "caption  value" line of a section, caption in the dim text colour
std::string line(const char *caption, const std::string &value) {
  return recolor(TEXT_DIM) + caption + "#  " + plain(value) + "\n";
}

void set_lines(lv_obj_t *label, std::string text) {
  if (!text.empty() && text.back() == '\n') text.pop_back();
  if (text != lv_label_get_text(label)) lv_label_set_text(label, text.c_str());
}

std::string mb(uint64_t kb) {
  return fmt::format("{} MB", kb / 1024);
}

std::string duration(uint64_t seconds) {
  const uint64_t days = seconds / 86400;
  const uint64_t hours = seconds % 86400 / 3600;
  const uint64_t minutes = seconds % 3600 / 60;
  if (days > 0) return fmt::format("{}d {}h {}m", days, hours, minutes);
  if (hours > 0) return fmt::format("{}h {}m", hours, minutes);
  return fmt::format("{}m {}s", minutes, seconds % 60);
}

// "MemTotal:  123456 kB" style files: the number after the key, in kB. Read a
// line at a time, /proc/self/status has text values ahead of the numbers.
uint64_t proc_kb(const std::string &path, const std::string &key) {
  std::ifstream f(path);
  std::string row;
  while (std::getline(f, row)) {
    std::istringstream fields(row);
    std::string name;
    uint64_t value;
    if (fields >> name && name == key && fields >> value) return value;
  }
  return 0;
}

// the UI fonts are not known to carry a degree sign, so spell the unit out
const char *rotation_name(lv_disp_rot_t rot) {
  switch (rot) {
    case LV_DISP_ROT_90:  return "90 deg";
    case LV_DISP_ROT_180: return "180 deg";
    case LV_DISP_ROT_270: return "270 deg";
    default:              return "0 deg";
  }
}

const char *log_level_name(LogLevel lvl) {
  switch (lvl) {
    case LogLevel::TRACE: return "trace";
    case LogLevel::DEBUG: return "debug";
    case LogLevel::ERROR: return "error";
    default:              return "info";
  }
}

}  // namespace

DevPanel::DevPanel(KWebSocketClient &c, std::mutex &l)
  : ws(c)
  , lv_lock(l)
  , panel_cont(create_screen(NULL))
  , tabview(lv_tabview_create(panel_cont, LV_DIR_TOP, scale_r(36)))
  , system_tab(lv_tabview_add_tab(tabview, "System"))
  , system_list(create_row(system_tab))
  , perf_label(nullptr)
  , host_label(nullptr)
  , network_label(nullptr)
  , connection_label(nullptr)
  , reconnect_btn(nullptr)
  , build_label(nullptr)
  , back_btn(panel_cont, Icons::BACK, "Back", &DevPanel::_handle_back, this)
  , refresh_timer(nullptr)
  , ticks(0)
  , last_cpu_total(0)
  , last_cpu_idle(0)
  , saved_monitor_cb(nullptr)
{
  lv_obj_move_background(panel_cont);
  lv_obj_set_style_pad_all(panel_cont, 0, 0);
  lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(system_tab, 0, 0);
  style_embedded_tabview(tabview);

  // one scrolling column of sections; the right edge leaves room for the
  // floating Back tile so it never sits on a value
  lv_obj_set_size(system_list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(system_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(system_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(system_list, gap(), 0);
  lv_obj_set_style_pad_right(system_list, ButtonContainer::float_w() + gap() * 2, 0);
  lv_obj_set_style_pad_row(system_list, gap(), 0);
  lv_obj_add_flag(system_list, LV_OBJ_FLAG_SCROLLABLE);
  manage_scroll(system_list);

  perf_label = add_section("Performance");
  host_label = add_section("Host");
  network_label = add_section("Network");
  connection_label = add_section("Moonraker / Klipper");
  reconnect_btn = create_text_btn(system_list, "Reconnect", &DevPanel::_handle_reconnect, this);
  lv_obj_set_height(reconnect_btn, touch_h());
  build_label = add_section("Build");

  back_btn.float_bottom_right();
}

DevPanel::~DevPanel() {
  background();
  if (panel_cont != NULL) {
    lv_obj_del(panel_cont);
    panel_cont = NULL;
  }
}

lv_obj_t *DevPanel::add_section(const char *title) {
  lv_obj_t *title_label = lv_label_create(system_list);
  lv_label_set_text(title_label, title);
  lv_obj_add_style(title_label, &styles().dim_label, 0);
  lv_obj_set_style_pad_top(title_label, gap(), 0);

  lv_obj_t *value_label = lv_label_create(system_list);
  lv_label_set_recolor(value_label, true);
  lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(value_label, LV_PCT(100));
  lv_label_set_text(value_label, "");
  return value_label;
}

void DevPanel::foreground() {
  lv_disp_t *disp = lv_disp_get_default();
  if (disp != NULL && refresh_timer == nullptr) {
    saved_monitor_cb = disp->driver->monitor_cb;
    disp->driver->monitor_cb = &DevPanel::_monitor_cb;
  }
  render_frames = render_time_ms = render_max_ms = 0;
  last_cpu_total = last_cpu_idle = 0;
  ticks = 0;

  refresh_build();
  refresh();
  request_connection_info();
  if (refresh_timer == nullptr) {
    refresh_timer = lv_timer_create(&DevPanel::_refresh_cb, REFRESH_MS, this);
  }

  lv_obj_scroll_to_y(system_list, 0, LV_ANIM_OFF);
  lv_obj_move_foreground(panel_cont);
}

void DevPanel::background() {
  if (refresh_timer != nullptr) {
    lv_timer_del(refresh_timer);
    refresh_timer = nullptr;
    lv_disp_t *disp = lv_disp_get_default();
    if (disp != NULL) disp->driver->monitor_cb = saved_monitor_cb;
    saved_monitor_cb = nullptr;
  }
  if (panel_cont != NULL) lv_obj_move_background(panel_cont);
}

void DevPanel::_handle_back(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    static_cast<DevPanel *>(lv_event_get_user_data(event))->background();
  }
}

void DevPanel::_handle_reconnect(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    DevPanel *panel = static_cast<DevPanel *>(lv_event_get_user_data(event));
    LOG_INFO("developer panel: reconnecting to Moonraker");
    panel->ws.reconnect();
  }
}

void DevPanel::_refresh_cb(lv_timer_t *timer) {
  DevPanel *panel = static_cast<DevPanel *>(timer->user_data);
  panel->ticks++;
  panel->refresh();
  if (panel->ticks % CONNECTION_EVERY_TICKS == 0) panel->request_connection_info();
}

void DevPanel::_monitor_cb(lv_disp_drv_t *drv, uint32_t time, uint32_t px) {
  (void)drv;
  (void)px;
  render_frames++;
  render_time_ms += time;
  if (time > render_max_ms) render_max_ms = time;
}

void DevPanel::refresh() {
  refresh_performance();
  refresh_host();
  refresh_network();
  refresh_connection();
  refresh_scroll(system_list);
}

void DevPanel::refresh_performance() {
  std::string text;

  // aggregate cpu line: user nice system idle iowait irq softirq steal
  std::istringstream stat(read_first_line("/proc/stat"));
  std::string cpu;
  uint64_t v, total = 0, idle = 0;
  stat >> cpu;
  for (int i = 0; i < 8 && stat >> v; i++) {
    total += v;
    if (i == 3 || i == 4) idle += v;
  }
  if (last_cpu_total != 0 && total > last_cpu_total) {
    const uint64_t dt = total - last_cpu_total;
    const uint64_t di = idle - last_cpu_idle;
    text += line("CPU", fmt::format("{}%", 100 * (dt - di) / dt));
  } else {
    text += line("CPU", "measuring...");
  }
  last_cpu_total = total;
  last_cpu_idle = idle;

  const uint64_t rss = proc_kb("/proc/self/status", "VmRSS:");
  text += line("Screen RAM", rss > 0 ? mb(rss) : "n/a");

  // redraws are counted while this screen is up, so they include its own
  // once a second refresh
  if (render_frames > 0) {
    text += line("Redraws", fmt::format("{}/s, avg {} ms, max {} ms",
                                        render_frames, render_time_ms / render_frames, render_max_ms));
  } else {
    text += line("Redraws", "none in the last second");
  }
  render_frames = render_time_ms = render_max_ms = 0;

  set_lines(perf_label, text);
}

void DevPanel::refresh_host() {
  std::string text;

  const double up = std::atof(read_first_line("/proc/uptime").c_str());
  text += line("Uptime", duration(static_cast<uint64_t>(up)));

  std::istringstream load(read_first_line("/proc/loadavg"));
  std::string l1, l5, l15;
  load >> l1 >> l5 >> l15;
  text += line("Load", l1.empty() ? "n/a" : fmt::format("{}  {}  {}", l1, l5, l15));

  const uint64_t total = proc_kb("/proc/meminfo", "MemTotal:");
  const uint64_t avail = proc_kb("/proc/meminfo", "MemAvailable:");
  if (total > 0) {
    text += line("RAM", fmt::format("{} free of {}", mb(avail), mb(total)));
  }

  // the first thermal zone that reads is the SoC on every board seen so far
  std::string temp = "n/a";
  for (int zone = 0; zone < 8; zone++) {
    const std::string raw = read_first_line(fmt::format("/sys/class/thermal/thermal_zone{}/temp", zone));
    if (!raw.empty()) {
      temp = fmt::format("{:.1f} C", std::atof(raw.c_str()) / 1000.0);
      break;
    }
  }
  text += line("CPU temp", temp);

  // free space where the config lives, which is where screenshots and logs go
  std::string dir = fs::path(Config::get_instance()->get_path()).parent_path().string();
  if (dir.empty()) dir = "/";
  struct statvfs vfs;
  if (statvfs(dir.c_str(), &vfs) == 0) {
    const uint64_t free_kb = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize / 1024;
    const uint64_t size_kb = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize / 1024;
    text += line("Disk", fmt::format("{} free of {} ({})", mb(free_kb), mb(size_kb), dir));
  }

  set_lines(host_label, text);
}

void DevPanel::refresh_network() {
  std::string text;

  // /proc/net/wireless: "wlan0: 0000   70.  -40.  -256 ..." -- level is dBm
  std::map<std::string, std::string> signal;
  {
    std::ifstream f("/proc/net/wireless");
    std::string row;
    while (std::getline(f, row)) {
      const size_t colon = row.find(':');
      if (colon == std::string::npos) continue;
      std::string iface = row.substr(0, colon);
      iface.erase(0, iface.find_first_not_of(" \t"));
      std::istringstream rest(row.substr(colon + 1));
      std::string status, quality, level;
      if (rest >> status >> quality >> level) {
        if (!level.empty() && level.back() == '.') level.pop_back();
        signal[iface] = level + " dBm";
      }
    }
  }

  const std::vector<std::string> ifaces = KUtils::get_interfaces();
  for (const std::string &iface : ifaces) {
    const std::string state = read_first_line("/sys/class/net/" + iface + "/operstate");
    std::string ip = KUtils::interface_ip(iface);
    if (ip == "0.0.0.0") ip = "no address";
    std::string value = ip + " (" + (state.empty() ? "unknown" : state) + ")";
    const auto s = signal.find(iface);
    if (s != signal.end()) value += ", signal " + s->second;
    const std::string mac = read_first_line("/sys/class/net/" + iface + "/address");
    if (!mac.empty()) value += "\n" + mac;
    text += line(plain(iface).c_str(), value);
  }
  if (ifaces.empty()) text += line("Interfaces", "none found");

  set_lines(network_label, text);
}

void DevPanel::request_connection_info() {
  if (!ws.isConnected()) return;

  ws.send_jsonrpc("server.info", [this](json &j) {
    std::lock_guard<std::mutex> lock(lv_lock);
    moonraker_version = j.value("/result/moonraker_version"_json_pointer, std::string());
  });
  ws.send_jsonrpc("printer.info", [this](json &j) {
    std::lock_guard<std::mutex> lock(lv_lock);
    klippy_state = j.value("/result/state"_json_pointer, std::string());
    klippy_message = j.value("/result/state_message"_json_pointer, std::string());
  });
}

void DevPanel::refresh_connection() {
  Config *conf = Config::get_instance();
  std::string text;

  text += line("Moonraker", fmt::format("{}:{}",
                                        conf->get<std::string>("/moonraker/host"),
                                        conf->get<int32_t>("/moonraker/port")));
  const bool connected = ws.isConnected();
  text += line("Websocket", connected ? "connected" : "disconnected");
  if (connected) {
    text += line("Version", moonraker_version.empty() ? "waiting..." : moonraker_version);
    text += line("Klipper", klippy_state.empty() ? "waiting..." : klippy_state);
    // Klipper's message is a paragraph when something is wrong; keep the
    // first line, the rest is in the log
    std::string message = klippy_message.substr(0, klippy_message.find('\n'));
    if (!message.empty()) text += line("Message", message);
  }

  set_lines(connection_label, text);
}

void DevPanel::refresh_build() {
  Config *conf = Config::get_instance();
  std::string text;

  text += line("Version", GUPPYSCREEN_VERSION);
  text += line("Branch", GUPPYSCREEN_BRANCH);

  std::string flags;
#ifdef GUPPY_SDL
  flags += "sdl ";
#endif
#ifdef GUPPY_SMALL_SCREEN
  flags += "small-screen ";
#endif
#ifdef GUPPY_CALIBRATE
  flags += "calibrate ";
#endif
#ifdef COSMOS
  flags += "cosmos ";
#endif
  text += line("Build", flags.empty() ? "default" : flags);

  lv_disp_t *disp = lv_disp_get_default();
  if (disp != NULL) {
    text += line("Display", fmt::format("{}x{} ({}x{} panel), rotated {}",
                                        lv_disp_get_hor_res(disp), lv_disp_get_ver_res(disp),
                                        lv_disp_get_physical_hor_res(disp), lv_disp_get_physical_ver_res(disp),
                                        rotation_name(lv_disp_get_rotation(disp))));
  }
  std::string rotate = std::to_string(conf->get<uint32_t>("/ui/display_rotate"));
  const char *rotate_env = std::getenv("DISPLAY_ROTATE");
  if (rotate_env != nullptr && rotate_env[0] != '\0') {
    rotate += fmt::format(", overridden by DISPLAY_ROTATE={}", rotate_env);
  }
  text += line("display_rotate", rotate);

#ifdef GUPPY_CALIBRATE
  std::error_code ec;
  const fs::path calibration = fs::canonical("/proc/self/exe", ec).parent_path() / "calibration.json";
  text += line("Touch calibration", !ec && fs::exists(calibration, ec) ? "saved" : "not saved");
#else
  text += line("Touch calibration", "not in this build");
#endif

  text += line("Config", conf->get_path());
  text += line("Override", conf->has_override_path() ? conf->get_override_path() : "none");
  text += line("Log level", log_level_name(get_log_level()));

  set_lines(build_label, text);
}
