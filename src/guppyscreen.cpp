#include "guppyscreen.h"

#include "config.h"
#include "theme.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#ifdef GUPPY_SDL
#include "lv_drivers/sdl/sdl_common.h"
#endif
#include "vnc/lv_vnc.h"
#include "logger.h"
#include "state.h"
#include "utils.h"
#ifdef GUPPY_CALIBRATE
#include <fstream>
#endif
#ifdef GUPPY_SDL
#include <cstdlib>
#endif
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;

namespace {
constexpr double calibration_version = 2.0;
}

GuppyScreen *GuppyScreen::instance = NULL;
lv_style_t GuppyScreen::style_container;
lv_theme_t GuppyScreen::th_new;

lv_obj_t *GuppyScreen::screen_saver = NULL;

KWebSocketClient GuppyScreen::ws(NULL);

std::mutex GuppyScreen::lv_lock;

GuppyScreen::GuppyScreen()
  : spoolman_panel(ws, lv_lock)
  , mmu_panel(MmuPanel::enabled() ? std::make_unique<MmuPanel>(ws, lv_lock) : nullptr)
#ifdef MMU_BACKEND_AFC
  , afc_backend(mmu_panel != nullptr ? std::make_unique<AfcBackend>(ws) : nullptr)
#endif
#ifdef MMU_BACKEND_HH
  , hh_backend(mmu_panel != nullptr ? std::make_unique<HhBackend>(ws) : nullptr)
#endif
  , main_panel(ws, lv_lock, spoolman_panel, mmu_panel.get())
  , init_panel(main_panel, lv_lock)
{
  // the ids here are the values accepted by /mmu/backend
  if (mmu_panel == nullptr) {
    main_panel.create_panel();
    return;
  }
#ifdef MMU_BACKEND_AFC
  if (afc_backend != nullptr) mmu_panel->add_backend("afc", afc_backend.get());
#endif
#ifdef MMU_BACKEND_HH
  if (hh_backend != nullptr) mmu_panel->add_backend("hh", hh_backend.get());
#endif
  main_panel.create_panel();
}

GuppyScreen *GuppyScreen::get() {
  if (instance == NULL) {
    instance = new GuppyScreen();
  }

  return instance;
}

/*--------------------------------------------------------------------------*/
/* GuppyScreen::init:                                                       */
/*                                                                          */
/* Bring the UI up: logging from the configuration, LVGL, the display (the   */
/* host's hal_init callback registers the display driver and input), the     */
/* theme layer every widget is styled through, the one GuppyScreen          */
/* instance with all its panels, the websocket to Moonraker (a configured   */
/* host of 0.0.0.0 meaning this machine, 127.0.0.1), the screen             */
/* saver used for display sleep, and -- in calibrated builds -- the touch    */
/* calibration screen or the saved coefficients.                            */
/*                                                                          */
/* On the local panel the framebuffer device is opened and unblanked before  */
/* hal_init asks it for its size.  In VNC mode (the server was started by    */
/* main, so LVNC_IsStarted is true) there is no framebuffer to open -- a Pi  */
/* serving VNC usually has none worth drawing on -- so that step is skipped. */
/*                                                                          */
/* Arguments:                                                               */
/*     hal_init : the host's display / input registration, called with the  */
/*                theme's primary and secondary colours once LVGL is up.    */
/*                                                                          */
/* Returns:                                                                 */
/*     GuppyScreen * : the single GuppyScreen instance.                     */
/*--------------------------------------------------------------------------*/

GuppyScreen *GuppyScreen::init(std::function<void(lv_color_t, lv_color_t)> hal_init) {
  /* libhv's own log is off; GrumpyScreen logs through its own logger. */
  hlog_disable();

  /* Log level and theme colours from the configuration. */
  Config *conf = Config::get_instance();
  auto ll = conf->get<std::string>("/ui/log_level");
  set_log_level(ll);

  auto primary_color = Theme::cfg_col("primary_colour", lv_color_hex(0x2196F3));
  auto secondary_color = Theme::cfg_col("secondary_colour", lv_color_hex(0xF44336));

  LOG_INFO("GrumpyScreen Version: {}-{}", GUPPYSCREEN_BRANCH, GUPPYSCREEN_VERSION);

  /* LVGL itself, before anything is registered with it. */
  lv_init();

#ifndef GUPPY_SDL
  if (!LVNC_IsStarted()) {  /* The local panel: open the framebuffer device and make sure it is lit. */
    fbdev_init();
    fbdev_unblank();
  }
#endif

  /* Display and input, then the PNG decoder the image assets need. */
  hal_init(primary_color, secondary_color);
  lv_png_init();

  /* The base container style: no border, square corners. */
  lv_style_init(&style_container);
  lv_style_set_border_width(&style_container, 0);
  lv_style_set_radius(&style_container, 0);

  /* A theme layered over LVGL's default one: it inherits everything and   */
  /* adds GrumpyScreen's per-class styling through new_theme_apply_cb, and */
  /* becomes the display's theme so every widget created from here on is  */
  /* styled by it.                                                        */
  lv_theme_t *th_act = lv_disp_get_theme(NULL);
  th_new = *th_act;
  lv_theme_set_parent(&th_new, th_act);
  lv_theme_set_apply_cb(&th_new, &GuppyScreen::new_theme_apply_cb);
  lv_disp_set_theme(NULL, &th_new);

  /* Printer state follows every websocket update. */
  ws.register_notify_update(State::get_instance());

  /* The panels, then the connection to Moonraker, at the host           */
  /* KUTIL_MoonrakerHost gives: the configured one, with 0.0.0.0 read as  */
  /* this machine (127.0.0.1), so the log line below shows the real one.  */
  GuppyScreen *gs = GuppyScreen::get();
  std::string ws_url = fmt::format("ws://{}:{}/websocket",
                                   KUtils::KUTIL_MoonrakerHost(),
                                   conf->get<uint32_t>("/moonraker/port"));

  LOG_INFO("connecting to printer at {}", ws_url);
  gs->connect_ws(ws_url);

  /* The screen saver: a full-screen opaque cover kept behind everything, */
  /* brought to the front when the display sleeps.                        */
  screen_saver = lv_obj_create(lv_scr_act());

  lv_obj_set_size(screen_saver, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(screen_saver, LV_OPA_100, 0);
#ifdef GUPPY_SDL
  /* The simulator has no panel to blank, so the cover itself is black. */
  lv_obj_set_style_bg_color(screen_saver, lv_color_black(), 0);
#endif
  lv_obj_move_background(screen_saver);

#ifdef GUPPY_CALIBRATE
  /* Calibrated builds: run the calibration screen once, or load its result. */
  lv_obj_t *main_screen = lv_disp_get_scr_act(NULL);
  std::vector<float> c = GuppyScreen::load_calibration_coeff();
  if (c.empty()) {  /* Never calibrated: show the calibration screen, then return to the main one. */
    lv_tc_register_coeff_save_cb(&GuppyScreen::save_calibration_coeff);
    lv_obj_t *touch_calibrate_scr = lv_tc_screen_create();
    lv_disp_load_scr(touch_calibrate_scr);
    lv_tc_screen_start(touch_calibrate_scr);
    lv_obj_add_event_cb(touch_calibrate_scr, &GuppyScreen::handle_calibrated, LV_EVENT_READY, main_screen);
    LOG_INFO("running touch calibration");
  } else {  /* Calibrated before: apply the saved coefficients. */
    lv_tc_coeff_t coeff = {true, c[0], c[1], c[2], c[3], c[4], c[5]};
    lv_tc_set_coeff(coeff, false);
    LOG_INFO("loaded calibration coefficients");
  }
#endif

  /* The instance, for the caller that wants it. */
  return gs;
}

/*--------------------------------------------------------------------------*/
/* GuppyScreen::loop:                                                       */
/*                                                                          */
/* The UI's main loop, for the life of the program: every 5 ms, under the   */
/* LVGL lock, run LVGL's timers (which render, flush and read input), then  */
/* decide display sleep.                                                    */
/*                                                                          */
/* Sleep: after display_sleep_sec of no touch the display is blanked and     */
/* the screen saver brought to the front; the first touch after that wakes  */
/* it.  LVGL's inactivity clock only ever grows until a touch resets it, so */
/* a drop in it is taken as a fresh baseline (a touch LVGL itself saw).  On  */
/* the local panel blanking is the framebuffer's own blank; in VNC mode it   */
/* is the server's -- clients are served black, so a client that darkens    */
/* its backlight after a spell of black (as PrimeVNC can) sleeps with this   */
/* one, and wakes on the first frame that is not black.                     */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : never returns (the SDL simulator exits when its window         */
/*            closes).                                                      */
/*--------------------------------------------------------------------------*/

void GuppyScreen::loop() {
  /* Sleep state and its settings; -1 seconds means never sleep. */
  std::atomic_bool is_sleeping(false);
  Config *conf = Config::get_instance();
  int32_t display_sleep = conf->get<int32_t>("/ui/display_sleep_sec") * 1000;
  uint32_t inactive_baseline = lv_disp_get_inactive_time(NULL);
  uint32_t last_inactive = inactive_baseline;

  LOG_DEBUG("Display sleep timeout: {} ms, starting inactivity baseline: {} ms",
           display_sleep,
           inactive_baseline);

  while (1) {  /* One pass of the UI per iteration, every 5 ms. */
#ifdef GUPPY_SDL
    if (sdl_quit_qry) {  /* The simulator window was closed. */
      std::exit(0);
    }
#endif

    /* LVGL's timers: rendering, flushing and input all happen in here. */
    lv_lock.lock();
    lv_timer_handler();

#ifdef GUPPY_SDL
    if (sdl_quit_qry) {  /* Closed during that pass: release the lock and exit. */
      lv_lock.unlock();
      std::exit(0);
    }
#endif

    if (display_sleep != -1) {  /* Display sleep is enabled: decide it from inactivity. */
      const uint32_t current_inactive = lv_disp_get_inactive_time(NULL);
      if (current_inactive < last_inactive) {  /* The clock went back: a touch, so start counting again from here. */
        inactive_baseline = current_inactive;
      }
      last_inactive = current_inactive;

      const uint32_t effective_inactive = current_inactive - inactive_baseline;
      if (effective_inactive > static_cast<uint32_t>(display_sleep)) {  /* Idle long enough to sleep. */
        if (!is_sleeping.load()) {  /* Not asleep yet: blank the display and raise the screen saver. */
          LOG_DEBUG("putting display to sleeping after {} ms effective inactivity", effective_inactive);
#ifndef GUPPY_SDL
          if (LVNC_IsStarted()) {  /* VNC: serve black to the clients. */
            LVNC_SetBlank(1);
          } else {  /* The local panel: blank the framebuffer. */
            fbdev_blank();
          }
#endif
          lv_obj_move_foreground(screen_saver);
          is_sleeping = true;
        }
      } else {  /* Active (recently touched). */
        if (is_sleeping.load()) {  /* Asleep: unblank and drop the screen saver behind everything again. */
          LOG_DEBUG("waking up display");
#ifndef GUPPY_SDL
          if (LVNC_IsStarted()) {  /* VNC: serve the live frame again. */
            LVNC_SetBlank(0);
          } else {  /* The local panel: unblank the framebuffer. */
            fbdev_unblank();
          }
#endif
          lv_obj_move_background(screen_saver);
          is_sleeping = false;
        }
      }
    }

    /* Let other threads (the websocket's) take the lock, then pause. */
    lv_lock.unlock();
    usleep(5000);
  }
}

std::mutex &GuppyScreen::get_lock() {
  return lv_lock;
}

void GuppyScreen::connect_ws(const std::string &url) {
  init_panel.set_message(LV_SYMBOL_WARNING " Waiting for Klipper to start...");
  ws.connect(url.c_str(),
   [this]() { init_panel.connected(ws); },
   [this]() { init_panel.disconnected(ws); });
}

void GuppyScreen::new_theme_apply_cb(lv_theme_t *th, lv_obj_t *obj) {
  LV_UNUSED(th);

  if (lv_obj_check_type(obj, &lv_obj_class)) {
    lv_obj_add_style(obj, &style_container, 0);
  }

  // Widget looks by class: a panel never styles a keyboard, entry or table
  // itself, so one edit to Theme reaches all of them. Panels may still add a
  // style on top for a genuinely different role (the console log is a panel).
  // Screens are skipped: the first one is created before there is an active
  // screen to read the primary colour from, and styles() would hang on it.
  if (lv_obj_get_parent(obj) == NULL) return;
  Theme::Styles &s = Theme::styles();
  // every text button is the stock grey button; LVGL's own theme, this one's
  // parent, darkens it under a finger and greys it out when disabled
  if (lv_obj_check_type(obj, &lv_btn_class)) {
    lv_obj_add_style(obj, &s.btn, LV_PART_MAIN);
  }
  // nothing draws a scrollbar: every list in the UI is swiped
  if (!lv_obj_check_type(obj, &lv_label_class) && !lv_obj_check_type(obj, &lv_img_class)) {
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
  }
  // button matrices (selectors, dialog button rows) and keyboards share the
  // key look; the one difference is what CHECKED means
  if (lv_obj_check_type(obj, &lv_btnmatrix_class) || lv_obj_check_type(obj, &lv_keyboard_class)) {
    lv_obj_add_style(obj, &s.key_body, LV_PART_MAIN);
    lv_obj_add_style(obj, &s.key, LV_PART_ITEMS);
    // a matrix's checked key is the chosen value: accent. A keyboard's checked
    // keys are its control keys: plain.
    if (!lv_obj_check_type(obj, &lv_keyboard_class)) {
      lv_obj_add_style(obj, &s.key_checked, LV_PART_ITEMS | LV_STATE_CHECKED);
      lv_obj_add_style(obj, &s.key_checked, LV_PART_ITEMS | LV_STATE_PRESSED);
    }
  }
  if (lv_obj_check_type(obj, &lv_textarea_class)) {
    lv_obj_add_style(obj, &s.input, LV_PART_MAIN);
    lv_obj_add_style(obj, &s.input_placeholder, LV_PART_TEXTAREA_PLACEHOLDER);
  }
  if (lv_obj_check_type(obj, &lv_table_class)) {
    lv_obj_add_style(obj, &s.table, LV_PART_MAIN);
    lv_obj_add_style(obj, &s.table_cell, LV_PART_ITEMS);
  }
  // sliders, bars, switches, arcs, spinners: groove in the raised grey, the
  // filled part in the accent, knobs in the text colour
  if (lv_obj_check_type(obj, &lv_arc_class) || lv_obj_check_type(obj, &lv_spinner_class)) {
    lv_obj_add_style(obj, &s.arc_track, LV_PART_MAIN);
    lv_obj_add_style(obj, &s.arc_fill, LV_PART_INDICATOR);
  } else if (lv_obj_check_type(obj, &lv_switch_class)) {
    // a switch is a grey channel that shows the accent only once it is on, and
    // keeps LVGL's own pale knob
    lv_obj_add_style(obj, &s.track, LV_PART_MAIN);
    lv_obj_add_style(obj, &s.track, LV_PART_INDICATOR);
    lv_obj_add_style(obj, &s.fill, LV_PART_INDICATOR | LV_STATE_CHECKED);
  } else if (lv_obj_check_type(obj, &lv_slider_class) || lv_obj_check_type(obj, &lv_bar_class)) {
    lv_obj_add_style(obj, &s.groove, LV_PART_MAIN);
    lv_obj_add_style(obj, &s.fill, LV_PART_INDICATOR);
    lv_obj_add_style(obj, &s.knob, LV_PART_KNOB);
    if (lv_obj_check_type(obj, &lv_slider_class)) lv_obj_add_style(obj, &s.slider, LV_PART_MAIN);
  }
}

#ifdef GUPPY_CALIBRATE
void GuppyScreen::handle_calibrated(lv_event_t *event) {
  LOG_INFO("finished calibration");
  lv_obj_t *main_screen = (lv_obj_t *)event->user_data;
  lv_disp_load_scr(main_screen);
}

std::vector<float> GuppyScreen::load_calibration_coeff() {
  std::string config_path = fs::canonical("/proc/self/exe").parent_path() / "calibration.json";
  std::ifstream f(config_path);
  if (!f.is_open()) {
    return {};
  }

  json j;
  f >> j;
  if (!j.is_object()) {
    LOG_INFO("discarding calibration data: legacy or invalid format");
    return {};
  }

  if (!j.contains("version") || !j["version"].is_number() ||
      j["version"].get<double>() != calibration_version) {
    LOG_INFO("discarding calibration data: missing or unsupported version (expected {})", calibration_version);
    return {};
  }

  if (!j.contains("display_rotate") || !j["display_rotate"].is_number_unsigned()) {
    LOG_INFO("discarding calibration data: missing display_rotate");
    return {};
  }

  Config *conf = Config::get_instance();
  auto current_rotate = conf->get<std::uint32_t>("/ui/display_rotate");

  auto saved_rotate = j["display_rotate"].get<std::uint32_t>();
  if (saved_rotate != current_rotate) {
    LOG_INFO("discarding calibration data: display_rotate changed from {} to {}", saved_rotate, current_rotate);
    return {};
  }

  if (!j.contains("calibrations") || !j["calibrations"].is_array()) {
    LOG_INFO("discarding calibration data: missing calibrations");
    return {};
  }

  const auto& calibration_values = j["calibrations"];
  if (calibration_values.size() != 6) {
    LOG_INFO("discarding calibration data: expected 6 calibration coefficients, got {}", calibration_values.size());
    return {};
  }

  std::vector<float> coeffs;
  coeffs.reserve(calibration_values.size());
  for (const auto& v : calibration_values) {
    coeffs.push_back(v.get<float>());
  }
  return coeffs;
}

void GuppyScreen::save_calibration_coeff(lv_tc_coeff_t coeff) {
  auto config_path = fs::canonical("/proc/self/exe").parent_path() / "calibration.json";
  Config *conf = Config::get_instance();
  json j = {
    {"version", calibration_version},
    {"display_rotate", conf->get<std::uint32_t>("/ui/display_rotate")},
    {"calibrations", {coeff.a, coeff.b, coeff.c, coeff.d, coeff.e, coeff.f}},
  };
  std::ofstream f(config_path, std::ios::trunc);
  f << j.dump(2);
}
#endif

/*Set in lv_conf.h as `LV_TICK_CUSTOM_SYS_TIME_EXPR`*/
uint32_t custom_tick_get(void) {
  static uint64_t start_ms = 0;
  if (start_ms == 0) {
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    start_ms = static_cast<uint64_t>(ts_start.tv_sec) * 1000ULL +
               static_cast<uint64_t>(ts_start.tv_nsec) / 1000000ULL;
  }

  struct timespec ts_now;
  clock_gettime(CLOCK_MONOTONIC, &ts_now);
  const uint64_t now_ms = static_cast<uint64_t>(ts_now.tv_sec) * 1000ULL +
                          static_cast<uint64_t>(ts_now.tv_nsec) / 1000000ULL;

  return static_cast<uint32_t>(now_ms - start_ms);
}
