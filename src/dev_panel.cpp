#include "dev_panel.h"
#include "config.h"
#include "icons.h"
#include "logger.h"
#include "simple_dialog.h"
#include "subprocess.hpp"
#include "theme.h"
#include "utils.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <experimental/filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/statvfs.h>
#include <vector>

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

// LVGL's built-in Montserrat carries the degree sign (U+00B0)
const char *ROTATE_MAP[] = {"0°", "90°", "180°", "270°", ""};
constexpr uint32_t ROTATE_COUNT = 4;

// what Reset UI settings falls back to when the main config has no value
constexpr int32_t DEFAULT_DISPLAY_SLEEP_SEC = 600;
constexpr bool DEFAULT_PROMPT_EMERGENCY_STOP = true;
constexpr uint32_t DEFAULT_DISPLAY_ROTATE = 0;

const char *RESET_BUTTONS[] = {"Back", "Reset", ""};
const char *RESTART_BUTTONS[] = {"Later", "Restart GUI", ""};

const char *rotation_name(lv_disp_rot_t rot) {
  return ROTATE_MAP[rot < ROTATE_COUNT ? rot : 0];
}

bool rotate_env_set() {
  const char *env = std::getenv("DISPLAY_ROTATE");
  return env != nullptr && env[0] != '\0';
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
  , display_tab(lv_tabview_add_tab(tabview, "Display"))
  , display_list(create_row(display_tab))
  , rotate_selector(nullptr)
  , rotate_note(nullptr)
  , recalibrate_switch(nullptr)
  , tests_tab(lv_tabview_add_tab(tabview, "Tests"))
  , tests_list(create_row(tests_tab))
  , screenshot_switch(nullptr)
  , system_tab(lv_tabview_add_tab(tabview, "System"))
  , system_list(create_row(system_tab))
  , perf_label(nullptr)
  , host_label(nullptr)
  , network_label(nullptr)
  , connection_label(nullptr)
  , reconnect_btn(nullptr)
  , build_label(nullptr)
  , back_btn(panel_cont, Icons::BACK, "Back", &DevPanel::_handle_back, this)
  , touch_layer(nullptr)
  , touch_label(nullptr)
  , touch_hline(nullptr)
  , touch_vline(nullptr)
  , last_dot{0, 0}
  , color_layer(nullptr)
  , color_hint(nullptr)
  , color_hint_timer(nullptr)
  , color_index(0)
  , screenshot_btn(nullptr)
  , screenshot_dragged(false)
  , toast(nullptr)
  , toast_timer(nullptr)
  , refresh_timer(nullptr)
  , ticks(0)
  , last_cpu_total(0)
  , last_cpu_idle(0)
  , saved_monitor_cb(nullptr)
{
  lv_obj_move_background(panel_cont);
  lv_obj_set_style_pad_all(panel_cont, 0, 0);
  lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(display_tab, 0, 0);
  lv_obj_set_style_pad_all(tests_tab, 0, 0);
  lv_obj_set_style_pad_all(system_tab, 0, 0);
  style_embedded_tabview(tabview);

  build_display_tab();
  build_tests_tab();

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
  close_touch_test();
  close_color_test();
  show_screenshot_btn(false);
  if (toast_timer != nullptr) lv_timer_del(toast_timer);
  if (toast != nullptr) lv_obj_del(toast);
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

void DevPanel::set_settings_changed_cb(std::function<void()> cb) {
  settings_changed_cb = std::move(cb);
}

namespace {

// red, green, blue, white, black: each primary on its own shows a dead or
// stuck subpixel, white shows dark spots and black shows backlight bleed
const uint32_t TEST_COLORS[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000};
const char *TEST_COLOR_NAMES[] = {"red", "green", "blue", "white", "black"};
constexpr uint32_t TEST_COLOR_COUNT = 5;
constexpr uint32_t COLOR_HINT_MS = 1500;

// enough trail to see a line wobble, few enough objects to stay cheap
constexpr size_t MAX_TOUCH_DOTS = 300;
constexpr uint32_t TOAST_MS = 2500;

// 24-bit bottom-up BMP: no encoder needed, every image viewer opens it
bool write_bmp(const std::string &path, const lv_img_dsc_t &img) {
  const uint32_t w = img.header.w;
  const uint32_t h = img.header.h;
  const uint32_t row = (w * 3 + 3) & ~3u;
  const uint32_t data_size = row * h;
  uint8_t header[54] = {'B', 'M'};
  auto le32 = [&header](int at, uint32_t v) {
    for (int i = 0; i < 4; i++) header[at + i] = static_cast<uint8_t>(v >> (8 * i));
  };
  le32(2, 54 + data_size);
  le32(10, 54);
  le32(14, 40);
  le32(18, w);
  le32(22, h);
  header[26] = 1;   // planes
  header[28] = 24;  // bits per pixel
  le32(34, data_size);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char *>(header), sizeof(header));
  const lv_color_t *pixels = reinterpret_cast<const lv_color_t *>(img.data);
  std::vector<uint8_t> line(row, 0);
  for (uint32_t y = h; y-- > 0;) {
    for (uint32_t x = 0; x < w; x++) {
      const uint32_t argb = lv_color_to32(pixels[y * w + x]);
      line[x * 3] = argb & 0xFF;
      line[x * 3 + 1] = (argb >> 8) & 0xFF;
      line[x * 3 + 2] = (argb >> 16) & 0xFF;
    }
    f.write(reinterpret_cast<const char *>(line.data()), row);
  }
  return static_cast<bool>(f);
}

// a full-screen object on the top layer, above every screen and panel
lv_obj_t *create_overlay(lv_color_t bg) {
  lv_obj_t *layer = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(layer);
  lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(layer, bg, 0);
  lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
  lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
  return layer;
}

// readable on any of the test colours: light text on a dark plate
lv_obj_t *create_plate_label(lv_obj_t *parent) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_bg_color(label, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_pad_all(label, gap(), 0);
  lv_obj_set_style_radius(label, radius_sm(), 0);
  lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
  return label;
}

void add_test_button(lv_obj_t *parent, const char *text, const char *note, lv_event_cb_t cb, void *user_data) {
  lv_obj_t *btn = create_text_btn(parent, text, cb, user_data);
  lv_obj_set_height(btn, touch_h());
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, note);
  lv_obj_add_style(label, &styles().dim_label, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, LV_PCT(100));
}

}  // namespace

void DevPanel::build_tests_tab() {
  lv_obj_set_size(tests_list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(tests_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tests_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(tests_list, gap(), 0);
  lv_obj_set_style_pad_right(tests_list, ButtonContainer::float_w() + gap() * 2, 0);
  lv_obj_set_style_pad_row(tests_list, gap(), 0);
  lv_obj_add_flag(tests_list, LV_OBJ_FLAG_SCROLLABLE);
  manage_scroll(tests_list);

  add_test_button(tests_list, "Touch test",
                  "Draws a dot wherever you touch, with the coordinates. Shows if touch is off and which way.",
                  &DevPanel::_handle_touch_test, this);
  add_test_button(tests_list, "Colour test",
                  "Fills the screen red, green, blue, white, then black; tap for the next. Shows dead pixels and bleed.",
                  &DevPanel::_handle_color_test, this);

  lv_obj_t *shot_row = create_row(tests_list);
  lv_obj_set_size(shot_row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(shot_row, touch_h(), 0);
  lv_obj_set_flex_flow(shot_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(shot_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *shot_label = lv_label_create(shot_row);
  lv_label_set_text(shot_label, "Screenshot button");
  lv_obj_set_flex_grow(shot_label, 1);
  screenshot_switch = lv_switch_create(shot_row);
  lv_obj_add_event_cb(screenshot_switch, &DevPanel::_handle_screenshot_switch, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_t *shot_note = lv_label_create(tests_list);
  lv_label_set_text(shot_note,
                    "Floats over every screen: go to the problem, tap it, and a .bmp is saved next to the config. "
                    "Drag it out of the way.");
  lv_obj_add_style(shot_note, &styles().dim_label, 0);
  lv_label_set_long_mode(shot_note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(shot_note, LV_PCT(100));
}

// --- touch test -------------------------------------------------------------

void DevPanel::_handle_touch_test(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    static_cast<DevPanel *>(lv_event_get_user_data(event))->open_touch_test();
  }
}

void DevPanel::open_touch_test() {
  if (touch_layer != nullptr) return;
  touch_layer = create_overlay(col(BG));
  lv_obj_add_event_cb(touch_layer, &DevPanel::_handle_touch_event, LV_EVENT_PRESSED, this);
  lv_obj_add_event_cb(touch_layer, &DevPanel::_handle_touch_event, LV_EVENT_PRESSING, this);

  // the crosshair spans the screen, so a touch near an edge shows how close
  // to the edge it really landed
  for (lv_obj_t **line : {&touch_hline, &touch_vline}) {
    *line = lv_obj_create(touch_layer);
    lv_obj_remove_style_all(*line);
    lv_obj_set_style_bg_color(*line, theme_primary(), 0);
    lv_obj_set_style_bg_opa(*line, LV_OPA_50, 0);
    lv_obj_clear_flag(*line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(*line, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_set_size(touch_hline, LV_PCT(100), 1);
  lv_obj_set_size(touch_vline, 1, LV_PCT(100));

  touch_label = create_plate_label(touch_layer);
  lv_label_set_text(touch_label, "Touch anywhere");
  lv_obj_align(touch_label, LV_ALIGN_TOP_LEFT, gap(), gap());

  lv_obj_t *close_btn = create_text_btn(touch_layer, LV_SYMBOL_CLOSE, &DevPanel::_handle_touch_close, this);
  lv_obj_set_size(close_btn, touch_h(), touch_h());
  lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -gap(), gap());

  lv_obj_t *clear_btn = create_text_btn(touch_layer, "Clear", &DevPanel::_handle_touch_clear, this);
  lv_obj_set_height(clear_btn, touch_h());
  lv_obj_align_to(clear_btn, close_btn, LV_ALIGN_OUT_LEFT_MID, -gap(), 0);
}

void DevPanel::_handle_touch_event(lv_event_t *event) {
  lv_indev_t *indev = lv_indev_get_act();
  if (indev == NULL) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  static_cast<DevPanel *>(lv_event_get_user_data(event))->touch_at(p);
}

void DevPanel::touch_at(lv_point_t p) {
  lv_label_set_text_fmt(touch_label, "x %d   y %d   (%d x %d)", (int)p.x, (int)p.y,
                        (int)lv_obj_get_width(touch_layer), (int)lv_obj_get_height(touch_layer));
  lv_obj_set_y(touch_hline, p.y);
  lv_obj_set_x(touch_vline, p.x);
  lv_obj_clear_flag(touch_hline, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(touch_vline, LV_OBJ_FLAG_HIDDEN);

  // a dot every few pixels draws a line without thousands of objects
  const lv_coord_t step = scale_r(3);
  if (!touch_dots.empty() && LV_ABS(p.x - last_dot.x) < step && LV_ABS(p.y - last_dot.y) < step) return;
  last_dot = p;

  const lv_coord_t size = scale_r(6);
  lv_obj_t *dot = lv_obj_create(touch_layer);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, size, size);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, theme_primary(), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(dot, p.x - size / 2, p.y - size / 2);
  // keep the label and buttons above the trail
  lv_obj_move_to_index(dot, 2);
  touch_dots.push_back(dot);

  if (touch_dots.size() > MAX_TOUCH_DOTS) {
    lv_obj_del(touch_dots.front());
    touch_dots.pop_front();
  }
}

void DevPanel::_handle_touch_clear(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    static_cast<DevPanel *>(lv_event_get_user_data(event))->clear_touch_dots();
  }
}

void DevPanel::clear_touch_dots() {
  for (lv_obj_t *dot : touch_dots) lv_obj_del(dot);
  touch_dots.clear();
}

void DevPanel::_handle_touch_close(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    static_cast<DevPanel *>(lv_event_get_user_data(event))->close_touch_test();
  }
}

void DevPanel::close_touch_test() {
  if (touch_layer == nullptr) return;
  touch_dots.clear();  // children of the layer, deleted with it
  // deleted later: this runs from a click on one of the layer's own buttons
  lv_obj_del_async(touch_layer);
  touch_layer = touch_label = touch_hline = touch_vline = nullptr;
}

// --- colour test ------------------------------------------------------------

void DevPanel::_handle_color_test(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    static_cast<DevPanel *>(lv_event_get_user_data(event))->open_color_test();
  }
}

void DevPanel::open_color_test() {
  if (color_layer != nullptr) return;
  color_layer = create_overlay(lv_color_black());
  lv_obj_add_event_cb(color_layer, &DevPanel::_handle_color_tap, LV_EVENT_CLICKED, this);
  color_hint = create_plate_label(color_layer);
  lv_obj_center(color_hint);
  show_color(0);
}

void DevPanel::show_color(uint32_t index) {
  color_index = index;
  lv_obj_set_style_bg_color(color_layer, lv_color_hex(TEST_COLORS[index]), 0);

  // the hint names the colour and then gets out of the way of the pixels
  lv_label_set_text_fmt(color_hint, "%s  %u/%u  -  tap for next",
                        TEST_COLOR_NAMES[index], (unsigned)(index + 1), (unsigned)TEST_COLOR_COUNT);
  lv_obj_clear_flag(color_hint, LV_OBJ_FLAG_HIDDEN);
  if (color_hint_timer != nullptr) {
    lv_timer_reset(color_hint_timer);
  } else {
    color_hint_timer = lv_timer_create(&DevPanel::_hide_color_hint_cb, COLOR_HINT_MS, this);
    lv_timer_set_repeat_count(color_hint_timer, 1);
  }
}

void DevPanel::_hide_color_hint_cb(lv_timer_t *timer) {
  DevPanel *panel = static_cast<DevPanel *>(timer->user_data);
  panel->color_hint_timer = nullptr;  // a one shot timer deletes itself
  if (panel->color_hint != nullptr) lv_obj_add_flag(panel->color_hint, LV_OBJ_FLAG_HIDDEN);
}

void DevPanel::_handle_color_tap(lv_event_t *event) {
  DevPanel *panel = static_cast<DevPanel *>(lv_event_get_user_data(event));
  if (panel->color_index + 1 < TEST_COLOR_COUNT) {
    panel->show_color(panel->color_index + 1);
  } else {
    panel->close_color_test();
  }
}

void DevPanel::close_color_test() {
  if (color_layer == nullptr) return;
  if (color_hint_timer != nullptr) {
    lv_timer_del(color_hint_timer);
    color_hint_timer = nullptr;
  }
  // deleted later: this runs from a click on the layer itself
  lv_obj_del_async(color_layer);
  color_layer = color_hint = nullptr;
}

// --- screenshot -------------------------------------------------------------

void DevPanel::_handle_screenshot_switch(lv_event_t *event) {
  DevPanel *panel = static_cast<DevPanel *>(lv_event_get_user_data(event));
  panel->show_screenshot_btn(lv_obj_has_state(panel->screenshot_switch, LV_STATE_CHECKED));
}

void DevPanel::show_screenshot_btn(bool show) {
  if (!show) {
    if (screenshot_btn != nullptr) {
      lv_obj_del(screenshot_btn);
      screenshot_btn = nullptr;
    }
    return;
  }
  if (screenshot_btn != nullptr) return;

  const lv_coord_t size = touch_h();
  screenshot_btn = lv_btn_create(lv_layer_top());
  lv_obj_set_size(screenshot_btn, size, size);
  lv_obj_set_style_radius(screenshot_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(screenshot_btn, theme_primary(), 0);
  lv_obj_set_style_bg_opa(screenshot_btn, LV_OPA_80, 0);
  lv_obj_set_style_shadow_width(screenshot_btn, 0, 0);
  lv_obj_t *icon = lv_label_create(screenshot_btn);
  lv_label_set_text(icon, LV_SYMBOL_IMAGE);
  lv_obj_set_style_text_color(icon, col(ON_PRIMARY), 0);
  lv_obj_center(icon);
  // bottom left, clear of the Back tiles that float bottom right
  lv_obj_set_pos(screenshot_btn, gap() * 2, lv_obj_get_height(lv_layer_top()) - size - gap() * 2);

  for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED}) {
    lv_obj_add_event_cb(screenshot_btn, &DevPanel::_handle_screenshot_btn, code, this);
  }
}

void DevPanel::_handle_screenshot_btn(lv_event_t *event) {
  DevPanel *panel = static_cast<DevPanel *>(lv_event_get_user_data(event));
  lv_obj_t *btn = panel->screenshot_btn;
  const lv_event_code_t code = lv_event_get_code(event);

  if (code == LV_EVENT_PRESSED) {
    panel->screenshot_dragged = false;
  } else if (code == LV_EVENT_PRESSING) {
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;
    // a finger never holds perfectly still: only a real move is a drag
    if (!panel->screenshot_dragged && LV_ABS(v.x) + LV_ABS(v.y) < scale_r(2)) return;
    panel->screenshot_dragged = true;
    lv_obj_t *top = lv_layer_top();
    const lv_coord_t x = LV_CLAMP(0, lv_obj_get_x(btn) + v.x, lv_obj_get_width(top) - lv_obj_get_width(btn));
    const lv_coord_t y = LV_CLAMP(0, lv_obj_get_y(btn) + v.y, lv_obj_get_height(top) - lv_obj_get_height(btn));
    lv_obj_set_pos(btn, x, y);
  } else if (code == LV_EVENT_RELEASED && !panel->screenshot_dragged) {
    panel->take_screenshot();
  }
}

void DevPanel::take_screenshot() {
  // the active screen holds every panel and dialog; the top layer with this
  // button, the tests and the toast is not part of it, so none of them show
  lv_obj_t *scr = lv_scr_act();
  const uint32_t size = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
  std::vector<uint8_t> buf(size);
  lv_img_dsc_t img;
  if (size == 0 || lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &img, buf.data(), size) != LV_RES_OK) {
    LOG_ERROR("screenshot: snapshot failed");
    show_toast("Screenshot failed");
    return;
  }

  std::string dir = fs::path(Config::get_instance()->get_path()).parent_path().string();
  if (dir.empty()) dir = ".";
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char name[64];
  std::strftime(name, sizeof(name), "screenshot-%Y%m%d-%H%M%S.bmp", &tm);
  const std::string path = dir + "/" + name;

  if (!write_bmp(path, img)) {
    LOG_ERROR("screenshot: could not write {}", path);
    show_toast("Screenshot failed: could not write the file");
    return;
  }
  LOG_INFO("screenshot saved to {}", path);
  show_toast(std::string("Saved ") + name);
}

void DevPanel::show_toast(const std::string &text) {
  if (toast == nullptr) {
    toast = create_plate_label(lv_layer_top());
  }
  lv_label_set_text(toast, text.c_str());
  lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -gap() * 2);
  lv_obj_move_foreground(toast);
  if (toast_timer != nullptr) {
    lv_timer_reset(toast_timer);
  } else {
    toast_timer = lv_timer_create(&DevPanel::_hide_toast_cb, TOAST_MS, this);
    lv_timer_set_repeat_count(toast_timer, 1);
  }
}

void DevPanel::_hide_toast_cb(lv_timer_t *timer) {
  DevPanel *panel = static_cast<DevPanel *>(timer->user_data);
  panel->toast_timer = nullptr;  // a one shot timer deletes itself
  if (panel->toast != nullptr) {
    lv_obj_del(panel->toast);
    panel->toast = nullptr;
  }
}

void DevPanel::build_display_tab() {
  lv_obj_set_size(display_list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(display_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(display_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(display_list, gap(), 0);
  lv_obj_set_style_pad_right(display_list, ButtonContainer::float_w() + gap() * 2, 0);
  lv_obj_set_style_pad_row(display_list, gap(), 0);
  lv_obj_add_flag(display_list, LV_OBJ_FLAG_SCROLLABLE);
  manage_scroll(display_list);

  lv_obj_t *rotate_title = lv_label_create(display_list);
  lv_label_set_text(rotate_title, "Screen rotation");
  lv_obj_add_style(rotate_title, &styles().dim_label, 0);

  rotate_selector = lv_btnmatrix_create(display_list);
  lv_obj_set_size(rotate_selector, LV_PCT(100), touch_h());
  lv_obj_add_style(rotate_selector, &styles().key_tray, 0);
  lv_btnmatrix_set_map(rotate_selector, ROTATE_MAP);
  lv_btnmatrix_set_btn_ctrl_all(rotate_selector, LV_BTNMATRIX_CTRL_CHECKABLE);
  lv_btnmatrix_set_one_checked(rotate_selector, true);
  lv_obj_add_event_cb(rotate_selector, &DevPanel::_handle_rotate, LV_EVENT_VALUE_CHANGED, this);

  rotate_note = lv_label_create(display_list);
  lv_obj_add_style(rotate_note, &styles().dim_label, 0);
  lv_label_set_long_mode(rotate_note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(rotate_note, LV_PCT(100));
  lv_obj_add_flag(rotate_note, LV_OBJ_FLAG_HIDDEN);

#ifdef GUPPY_CALIBRATE
  lv_obj_t *recalibrate_row = create_row(display_list);
  lv_obj_set_size(recalibrate_row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(recalibrate_row, touch_h(), 0);
  lv_obj_set_flex_flow(recalibrate_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(recalibrate_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *recalibrate_label = lv_label_create(recalibrate_row);
  lv_label_set_text(recalibrate_label, "Recalibrate touch after rotating");
  lv_obj_add_style(recalibrate_label, &styles().dim_label, 0);
  lv_obj_set_flex_grow(recalibrate_label, 1);
  recalibrate_switch = lv_switch_create(recalibrate_row);
  lv_obj_add_event_cb(recalibrate_switch, &DevPanel::_handle_recalibrate, LV_EVENT_VALUE_CHANGED, this);
#endif

  lv_obj_t *reset_title = lv_label_create(display_list);
  lv_label_set_text(reset_title, "Reset");
  lv_obj_add_style(reset_title, &styles().dim_label, 0);
  lv_obj_set_style_pad_top(reset_title, gap(), 0);

  lv_obj_t *reset_btn = create_text_btn(display_list, "Reset UI settings", &DevPanel::_handle_reset, this);
  lv_obj_set_height(reset_btn, touch_h());

  lv_obj_t *reset_note = lv_label_create(display_list);
  lv_label_set_text(reset_note, "Screen timeout, emergency stop prompt and rotation go back to their defaults.");
  lv_obj_add_style(reset_note, &styles().dim_label, 0);
  lv_label_set_long_mode(reset_note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(reset_note, LV_PCT(100));
}

void DevPanel::refresh_display_controls() {
  Config *conf = Config::get_instance();

  const uint32_t rotate = conf->get<uint32_t>("/ui/display_rotate");
  lv_btnmatrix_clear_btn_ctrl_all(rotate_selector, LV_BTNMATRIX_CTRL_CHECKED);
  if (rotate < ROTATE_COUNT) lv_btnmatrix_set_btn_ctrl(rotate_selector, rotate, LV_BTNMATRIX_CTRL_CHECKED);

  // the environment wins over the config at startup, so a saved value would
  // never show; say so rather than offer a control that does nothing
  std::string note;
  if (rotate_env_set()) {
    lv_obj_add_state(rotate_selector, LV_STATE_DISABLED);
    note = fmt::format("Locked by the DISPLAY_ROTATE={} environment variable.", std::getenv("DISPLAY_ROTATE"));
  } else {
    lv_obj_clear_state(rotate_selector, LV_STATE_DISABLED);
  }
  if (note.empty()) {
    lv_obj_add_flag(rotate_note, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(rotate_note, note.c_str());
    lv_obj_clear_flag(rotate_note, LV_OBJ_FLAG_HIDDEN);
  }

  if (recalibrate_switch != nullptr) {
    if (conf->get<bool>("/ui/recalibrate_on_rotate", true)) {
      lv_obj_add_state(recalibrate_switch, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(recalibrate_switch, LV_STATE_CHECKED);
    }
  }
  refresh_scroll(display_list);
}

void DevPanel::_handle_rotate(lv_event_t *event) {
  static_cast<DevPanel *>(lv_event_get_user_data(event))->rotate_changed();
}

void DevPanel::rotate_changed() {
  Config *conf = Config::get_instance();
  const uint32_t selected = lv_btnmatrix_get_selected_btn(rotate_selector);
  if (selected >= ROTATE_COUNT || selected == conf->get<uint32_t>("/ui/display_rotate")) return;

  if (!conf->set_display_rotate(selected)) {
    refresh_display_controls();
    save_failed();
    return;
  }
  LOG_INFO("developer panel: display_rotate set to {}", selected);
  refresh_build();
  prompt_restart(fmt::format("Rotation {} is applied when the screen restarts.", ROTATE_MAP[selected]).c_str());
}

void DevPanel::_handle_recalibrate(lv_event_t *event) {
  DevPanel *panel = static_cast<DevPanel *>(lv_event_get_user_data(event));
  const bool enabled = lv_obj_has_state(panel->recalibrate_switch, LV_STATE_CHECKED);
  if (!Config::get_instance()->set_recalibrate_on_rotate(enabled)) {
    panel->refresh_display_controls();
    panel->save_failed();
  }
}

void DevPanel::_handle_reset(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  SimpleDialogOptions opts;
  opts.buttons = RESET_BUTTONS;
  opts.highlighted_button_idx = 1;
  opts.result_cb = &DevPanel::_reset_confirmed;
  opts.user_data = lv_event_get_user_data(event);
  create_configurable_dialog(lv_scr_act(), "Reset UI settings?",
      "Screen timeout, emergency stop prompt and rotation go back to their defaults.", opts);
}

void DevPanel::_reset_confirmed(lv_obj_t *mbox, uint32_t button, void *user_data) {
  (void)mbox;
  if (button == 1) static_cast<DevPanel *>(user_data)->reset_ui_settings();
}

void DevPanel::reset_ui_settings() {
  Config *conf = Config::get_instance();
  const uint32_t old_rotate = conf->get<uint32_t>("/ui/display_rotate");
  const int32_t sleep = conf->get_default<int32_t>("/ui/display_sleep_sec", DEFAULT_DISPLAY_SLEEP_SEC);
  const bool prompt = conf->get_default<bool>("/ui/prompt_emergency_stop", DEFAULT_PROMPT_EMERGENCY_STOP);
  uint32_t rotate = conf->get_default<uint32_t>("/ui/display_rotate", DEFAULT_DISPLAY_ROTATE);
  if (rotate >= ROTATE_COUNT) rotate = DEFAULT_DISPLAY_ROTATE;

  // a locked rotation is left alone: the environment decides it anyway
  const bool ok = conf->set_display_sleep_sec(sleep)
      && conf->set_prompt_emergency_stop(prompt)
      && (rotate_env_set() || conf->set_display_rotate(rotate));
  LOG_INFO("developer panel: UI settings reset (sleep {}, prompt {}, rotate {}){}",
           sleep, prompt, rotate, ok ? "" : " -- saving failed");

  refresh_display_controls();
  refresh_build();
  if (settings_changed_cb) settings_changed_cb();

  if (!ok) {
    save_failed();
  } else if (!rotate_env_set() && rotate != old_rotate) {
    prompt_restart(fmt::format("Rotation is back to {}, which is applied when the screen restarts.",
                               ROTATE_MAP[rotate]).c_str());
  }
}

void DevPanel::prompt_restart(const char *message) {
  SimpleDialogOptions opts;
  opts.buttons = RESTART_BUTTONS;
  opts.result_cb = &DevPanel::_restart_confirmed;
  opts.user_data = this;
  create_configurable_dialog(lv_scr_act(), "Restart GUI?", message, opts);
}

void DevPanel::_restart_confirmed(lv_obj_t *mbox, uint32_t button, void *user_data) {
  (void)mbox;
  (void)user_data;
  if (button != 1) return;

  const std::string cmd = Config::get_instance()->get<std::string>("/commands/gui_restart_cmd");
  int ret = -1;
  if (!cmd.empty()) {
    try {
      ret = subprocess::call(cmd);
    } catch (const std::exception &e) {
      LOG_ERROR("Failed to execute command '{}': {}", cmd, e.what());
    }
  }
  if (ret != 0) {
    create_simple_dialog(lv_scr_act(), "Restart GUI Failed", "Failed to restart GUI!", true, true);
  }
}

void DevPanel::save_failed() {
  create_simple_dialog(lv_scr_act(), "Unable to Save Setting",
                       "The config file could not be updated.", true, true);
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

  refresh_display_controls();
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
