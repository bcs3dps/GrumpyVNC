#include "setting_panel.h"
#include "config.h"
#include "logger.h"
#include "subprocess.hpp"
#include "simple_dialog.h"
#include "icons.h"
#include "theme.h"

#include <experimental/filesystem>
#include <vector>
#include <string>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace fs = std::experimental::filesystem;
namespace sp = subprocess;

#define SETP_HOST_POWER_MOONRAKER "moonraker"    // shutdown_host_cmd / reboot_host_cmd value: ask Moonraker (machine.shutdown / machine.reboot) instead of running a command

/*--------------------------------------------------------------------------*/
/* setp_restart_self:                                                       */
/*                                                                          */
/* Restart GrumpyScreen in place, for "Restart GUI" when no restart command */
/* is configured: replace this process with a fresh copy of the same       */
/* program, with the same arguments and environment.  The PID stays the    */
/* same, so a service manager supervising it sees no exit, and every       */
/* socket opened close-on-exec (the VNC server's included) is released by   */
/* the exec itself, so the new copy can listen on the same port at once.    */
/*                                                                          */
/* The arguments are read back from /proc/self/cmdline (NUL-separated)     */
/* rather than saved at start-up, so nothing outside this file has to keep */
/* them.  The program run is the executable's REAL path, read through the  */
/* /proc/self/exe link: exec'ing "/proc/self/exe" itself renames the        */
/* process "exe" (measured), and anything that finds GrumpyScreen by name   */
/* -- killall, pgrep, a launcher stopping it -- would then miss it.  The    */
/* link itself is only the fallback, for when it cannot be read or the file */
/* has been replaced on disk since (it still runs this very program).       */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : only when the restart FAILED (a successful exec never        */
/*            returns); the reason has been logged.                        */
/*--------------------------------------------------------------------------*/

static void setp_restart_self(void)
{
    std::string              sCmdline;
    std::vector<std::string> vArgs;
    std::vector<char *>      vArgv;
    std::string              sExe;
    char                     arrPath[4096];
    ssize_t                  lLen;
    char                     arrBuf[4096];
    size_t                   ulGot;
    size_t                   ulStart;
    size_t                   i;
    FILE                    *pF;

    /* The command line this process was started with. */
    pF = fopen("/proc/self/cmdline", "rb");
    if (pF == NULL)
    {   /* No /proc: nothing to restart from. */
        LOG_ERROR("Restart GUI: cannot read /proc/self/cmdline: {}", strerror(errno));
        return;
    }
    do
    {   /* One buffer's worth per iteration, until the file is read. */
        ulGot = fread(arrBuf, 1, sizeof(arrBuf), pF);
        sCmdline.append(arrBuf, ulGot);
    } while (ulGot > 0);
    fclose(pF);

    /* Split it at the NULs: every argument ends with one. */
    ulStart = 0;
    for (i = 0; i < sCmdline.size(); i++)
    {   /* One byte per iteration; a NUL closes an argument. */
        if (sCmdline[i] == '\0')
        {   /* The end of an argument. */
            vArgs.push_back(sCmdline.substr(ulStart, i - ulStart));
            ulStart = i + 1;
        }
    }
    if (vArgs.empty())
    {   /* An empty command line: nothing to run. */
        LOG_ERROR("Restart GUI: /proc/self/cmdline is empty");
        return;
    }

    /* The argv array execv wants: pointers into vArgs, then NULL. */
    for (i = 0; i < vArgs.size(); i++)
    {   /* One argument per iteration. */
        vArgv.push_back(&vArgs[i][0]);
    }
    vArgv.push_back(NULL);

    /* The executable's real path, so the new copy keeps the program's     */
    /* name; the link itself when that cannot be had, or when the file has  */
    /* been replaced on disk (the link's target then ends " (deleted)").    */
    sExe = "/proc/self/exe";
    lLen = readlink("/proc/self/exe", arrPath, sizeof(arrPath) - 1);
    if ((lLen > 0) && ((size_t)lLen < sizeof(arrPath) - 1))
    {   /* Read in full: use it unless the file is gone. */
        arrPath[lLen] = '\0';
        if (strstr(arrPath, " (deleted)") == NULL)
        {   /* The file is still there under that name. */
            sExe = arrPath;
        }
    }

    /* Become a fresh copy of this program; returns only on failure. */
    LOG_INFO("Restart GUI: restarting in place");
    execv(sExe.c_str(), vArgv.data());
    LOG_ERROR("Restart GUI: exec failed: {}", strerror(errno));
}

struct deferred_cmd_ctx {
    lv_obj_t * mbox;
    std::string cmd;
    const char * failure_title;
    const char * failure_message;
};

// Long enough for LVGL to render the dialog before the command blocks the UI thread.
static constexpr uint32_t DEFERRED_COMMAND_DELAY_MS = 500;
// The factory reset dialog is deliberately left up longer before we block.
static constexpr uint32_t FACTORY_RESET_DELAY_MS = 5000;

static int call_command(const std::string &cmd) {
    try {
        return sp::call(cmd);
    } catch (const std::exception &e) {
        LOG_ERROR("Failed to execute command '{}': {}", cmd, e.what());
        return -1;
    }
}

static void run_deferred_command_cb(lv_timer_t * t) {
    deferred_cmd_ctx * ctx = (deferred_cmd_ctx *)t->user_data;

    int ret = call_command(ctx->cmd);

    if (ret != 0) {
        simple_dialog_close(ctx->mbox);
        create_simple_dialog(lv_scr_act(),
                             ctx->failure_title,
                             ctx->failure_message,
                             true,
                             true);
    }

    delete ctx;
    lv_timer_del(t);
}

// Commands such as update, switch to stock and factory reset block for a long
// time and then reboot the printer. Calling them straight from the click
// handler freezes the UI thread before LVGL gets a chance to draw, so the press
// appears to do nothing at all until the machine reboots. Put the dialog up
// first and defer the command to a one shot timer so the screen is rendered
// before we block on it.
static void run_command_deferred(lv_obj_t * mbox,
                                 const std::string &cmd,
                                 const char * failure_title,
                                 const char * failure_message,
                                 uint32_t delay_ms) {
    deferred_cmd_ctx * ctx = new deferred_cmd_ctx{ mbox, cmd, failure_title, failure_message };
    lv_timer_t * timer = lv_timer_create(run_deferred_command_cb, delay_ms, ctx);
    lv_timer_set_repeat_count(timer, 1);
}

static void layout_tiles(lv_obj_t *cont, const std::vector<ButtonContainer *> &tiles) {
  static const size_t cols = 4;
  static lv_coord_t grid_col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
      LV_GRID_TEMPLATE_LAST};
  static lv_coord_t grid_row_dsc[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
  if (tiles.size() <= cols) grid_row_dsc[1] = LV_GRID_TEMPLATE_LAST;
  else grid_row_dsc[1] = LV_GRID_CONTENT;
  lv_obj_set_grid_dsc_array(cont, grid_col_dsc, grid_row_dsc);

  for (size_t i = 0; i < tiles.size(); i++) {
    tiles[i]->use_card();
    lv_obj_set_height(tiles[i]->get_container(), Theme::scale_r(92));
    lv_obj_set_grid_cell(tiles[i]->get_container(), LV_GRID_ALIGN_STRETCH, i % cols, 1,
                         LV_GRID_ALIGN_STRETCH, i / cols, 1);
  }
}

/*--------------------------------------------------------------------------*/
/* SettingPanel::SettingPanel:                                              */
/*                                                                          */
/* Build the Settings page: a "Machine" tab of service tiles (restart       */
/* Klipper, firmware restart, restart the GUI, support ZIP) and a "Danger"  */
/* tab (switch to stock, factory reset, the COSMOS update, shut the host     */
/* down, reboot the host).  A tile whose action is a configured command only */
/* appears when that command is set, and each grid is built from the tiles  */
/* left, four across, so a hidden tile never leaves a hole.  Restart Klipper */
/* is one of those optional tiles: a host where GrumpyScreen does not own    */
/* Klipper's service (a Moonraker-compatible server on a Pi, say) leaves it   */
/* empty and the tile goes.  Shut the host down and reboot the host each     */
/* appear for a command or for "moonraker" (see handle_callback) -- so a     */
/* configuration written before Reboot Host existed, with no reboot_host_cmd */
/* at all, simply does not show it.                                          */
/*                                                                          */
/* Arguments:                                                               */
/*     c      : the Moonraker websocket the tiles send through.             */
/*     (lock) : not used by this page.                                      */
/*     parent : the tab this page fills, or nullptr for a page of its own.  */
/*                                                                          */
/* Returns:                                                                 */
/*     (constructor) : nothing.                                             */
/*--------------------------------------------------------------------------*/

SettingPanel::SettingPanel(KWebSocketClient &c, std::mutex &, lv_obj_t *parent)
  : ws(c)
  , owns_cont(parent == nullptr)
  , cont(Theme::create_screen(parent))  // fills the tab: it is the page
  , tabview(lv_tabview_create(cont, LV_DIR_TOP, Theme::scale_r(36)))
  , service_tab(lv_tabview_add_tab(tabview, "Machine"))
  , danger_tab(lv_tabview_add_tab(tabview, "Danger"))
  , service_cont(Theme::create_screen(service_tab))
  , danger_cont(Theme::create_screen(danger_tab))
  , restart_klipper_btn(service_cont, Icons::REFRESH_IMG, "Restart\nKlipper", &SettingPanel::_handle_callback, this,
        "Restart Klipper", "Do you want to restart klipper?", {"Back", "Restart Klipper"})
  , restart_firmware_btn(service_cont, Icons::REFRESH_IMG, "Firmware\nRestart", &SettingPanel::_handle_callback, this,
        "Firmware Restart", "Do you want to perform a firmware restart?", {"Back", "Firmware Restart"})
  , guppy_restart_btn(service_cont, Icons::REFRESH_IMG, "Restart GUI", &SettingPanel::_handle_callback, this)
  , support_zip_btn(service_cont, Icons::SD_IMG, "Create\nSupport ZIP", &SettingPanel::_handle_callback, this)
  , switch_to_stock_btn(danger_cont, Icons::EMERGENCY, SWITCH_TO_STOCK_BUTTON_TEXT, &SettingPanel::_handle_callback, this,
          SWITCH_TO_STOCK_BUTTON_TITLE, SWITCH_TO_STOCK_BUTTON_PROMPT, {"Back", "Switch to Stock"})
  , factory_reset_btn(danger_cont, Icons::EMERGENCY, FACTORY_RESET_BUTTON_TEXT, &SettingPanel::_handle_callback, this,
		  FACTORY_RESET_BUTTON_TITLE, FACTORY_RESET_BUTTON_PROMPT, {"Back", "Factory Reset"})
#ifdef COSMOS
  , update_btn(danger_cont, Icons::EMERGENCY, UPDATE_BUTTON_TEXT, &SettingPanel::_handle_callback, this,
          UPDATE_BUTTON_TITLE, UPDATE_BUTTON_PROMPT, {"Back", "Update"})
#endif
  , shutdown_host_btn(danger_cont, Icons::EMERGENCY, "Shutdown\nHost", &SettingPanel::_handle_callback, this,
          "Shutdown host?", "Do you want to shutdown the host?", {"Back", "Shutdown Host"})
  , reboot_host_btn(danger_cont, Icons::REFRESH_IMG, "Reboot\nHost", &SettingPanel::_handle_callback, this,
          "Reboot host?", "Do you want to reboot the host?", {"Back", "Reboot Host"})  // confirmed first, like Shutdown
{
  /* The tab view fills the page. */
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(service_tab, 0, 0);
  lv_obj_set_style_pad_all(danger_tab, 0, 0);
  Theme::style_embedded_tabview(tabview);

  /* Which tiles each tab shows: the fixed ones, then each optional one    */
  /* only with a command behind it (a hidden tile is told to hide).        */
  Config *conf = Config::get_instance();
  auto has_cmd = [conf](const char *key) { return conf->get<std::string>(key) != ""; };
  std::vector<ButtonContainer *> service_tiles;
  std::vector<ButtonContainer *> danger_tiles;
  struct Optional { ButtonContainer *tile; const char *cmd; };
  for (const Optional &o : {Optional{&restart_klipper_btn, "/commands/restart_klipper_cmd"}}) {  /* Restart Klipper leads the Machine tab when it has a command. */
    if (has_cmd(o.cmd)) service_tiles.push_back(o.tile); else o.tile->hide();
  }
  service_tiles.push_back(&restart_firmware_btn);
  service_tiles.push_back(&guppy_restart_btn);
  for (const Optional &o : {Optional{&support_zip_btn, "/commands/support_zip_cmd"}}) {  /* Support ZIP, when it has a command. */
    if (has_cmd(o.cmd)) service_tiles.push_back(o.tile); else o.tile->hide();
  }
  for (const Optional &o : {Optional{&switch_to_stock_btn, "/commands/switch_to_stock_cmd"},
                            Optional{&factory_reset_btn, "/commands/factory_reset_cmd"}
                            }) {  /* The Danger tab's command tiles, each when it has a command. */
    if (has_cmd(o.cmd)) danger_tiles.push_back(o.tile); else o.tile->hide();
  }
#ifdef COSMOS
  /* COSMOS builds always offer their update. */
  danger_tiles.push_back(&update_btn);
#endif

  for (const Optional &o : {Optional{&shutdown_host_btn, "/commands/shutdown_host_cmd"},
                            Optional{&reboot_host_btn, "/commands/reboot_host_cmd"}
                            }) {  /* Shut down, then reboot, the host: each a command, or "moonraker". */
    if (has_cmd(o.cmd)) danger_tiles.push_back(o.tile); else o.tile->hide();
  }

  /* Lay each tab's tiles out, four across. */
  layout_tiles(service_cont, service_tiles);
  layout_tiles(danger_cont, danger_tiles);
}

SettingPanel::~SettingPanel() {
  if (owns_cont && cont != NULL) {
    lv_obj_del(cont);
    cont = NULL;
  }
}

lv_obj_t *SettingPanel::get_container() {
  return cont;
}

void SettingPanel::foreground() {
}

/*--------------------------------------------------------------------------*/
/* SettingPanel::handle_callback:                                           */
/*                                                                          */
/* Carry out a tile once it is clicked (a tile with a confirmation has      */
/* already been confirmed by the time the click arrives).  Most tiles run   */
/* their configured command; the long-running, rebooting ones put their    */
/* dialog up first and run the command from a one-shot timer, so the dialog */
/* is drawn before the UI thread blocks.                                    */
/*                                                                          */
/* Three tiles have a second way to act, chosen by configuration:           */
/*   Restart GUI with NO gui_restart_cmd restarts this program in place     */
/*     (setp_restart_self) -- for a host with no service script for it.     */
/*   Shut the host down with shutdown_host_cmd "moonraker", and reboot the  */
/*     host with reboot_host_cmd "moonraker", ask Moonraker to do it         */
/*     (machine.shutdown / machine.reboot), so the Moonraker-compatible      */
/*     server on the host applies its own rules -- it may refuse, which is   */
/*     why those dialogs can be closed -- instead of GrumpyScreen running a  */
/*     command with rights it may not have.                                 */
/*                                                                          */
/* Arguments:                                                               */
/*     event : the LVGL event; only a click on one of this page's tiles     */
/*             does anything.                                               */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; failures are shown as dialogs.                       */
/*--------------------------------------------------------------------------*/

void SettingPanel::handle_callback(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {  /* A tile was clicked: find which and act on it. */
      lv_obj_t *btn = lv_event_get_current_target(event);
    if (btn == restart_klipper_btn.get_container()) {  /* Restart Klipper: its configured service command. */
      Config *conf = Config::get_instance();
      auto restart_command = conf->get<std::string>("/commands/restart_klipper_cmd");
      auto ret = call_command(restart_command);
      if (ret != 0) {  /* The command failed. */
        create_simple_dialog(lv_scr_act(), "Restart Klipper Failed", "Failed to restart Klipper!", true, true);
      }
    } else if (btn == restart_firmware_btn.get_container()) {  /* Firmware restart: Moonraker does it. */
      ws.send_jsonrpc("printer.firmware_restart");
    } else if (btn == guppy_restart_btn.get_container()) {  /* Restart GUI: the configured command, or a restart in place when there is none. */
      Config *conf = Config::get_instance();
      auto restart_command = conf->get<std::string>("/commands/gui_restart_cmd");
      if (restart_command.empty()) {  /* No command: become a fresh copy of this program (returns only on failure). */
        setp_restart_self();
        create_simple_dialog(lv_scr_act(), "Restart GUI Failed", "Failed to restart GUI!", true, true);
      } else {  /* The configured command (a service restart, typically). */
        auto ret = call_command(restart_command);
        if (ret != 0) {  /* The command failed. */
          create_simple_dialog(lv_scr_act(), "Restart GUI Failed", "Failed to restart GUI!", true, true);
        }
      }
#ifdef COSMOS
    } else if (btn == update_btn.get_container()) {  /* COSMOS update: dialog first, then the long-running command. */
      Config *conf = Config::get_instance();
      auto update_cmd = conf->get<std::string>(std::string("/commands/cosmos_update_cmd"));
      lv_obj_t *mbox = create_simple_dialog(lv_scr_act(), UPDATE_BUTTON_TITLE " Initiated", UPDATE_BUTTON_SUCCESS, false, false);
      run_command_deferred(mbox, update_cmd,
                           UPDATE_BUTTON_TITLE " Failed", UPDATE_BUTTON_FAILURE,
                           DEFERRED_COMMAND_DELAY_MS);
#endif
    } else if (btn == shutdown_host_btn.get_container()) {  /* Shut the host down: through Moonraker, or with the configured command. */
      Config *conf = Config::get_instance();
      auto shutdown_host_cmd = conf->get<std::string>("/commands/shutdown_host_cmd");
      if (shutdown_host_cmd == SETP_HOST_POWER_MOONRAKER) {  /* Moonraker decides and does it; a refusal leaves the host up, so the dialog can be closed. */
        create_simple_dialog(lv_scr_act(), "Shutdown Host Requested", "Shutdown of the host has been requested", true, false);
        ws.send_jsonrpc("machine.shutdown");
      } else {  /* The command, after the dialog has had a chance to draw. */
        lv_obj_t *mbox = create_simple_dialog(lv_scr_act(), "Shutdown Host Initiated", "Shutdown of host has been initiated", false, false);
        run_command_deferred(mbox, shutdown_host_cmd,
                             "Shutdown Host Failed", "Failed to shutdown host!",
                             DEFERRED_COMMAND_DELAY_MS);
      }
    } else if (btn == reboot_host_btn.get_container()) {  /* Reboot the host: through Moonraker, or with the configured command. */
      Config *conf = Config::get_instance();
      auto reboot_host_cmd = conf->get<std::string>("/commands/reboot_host_cmd");
      if (reboot_host_cmd == SETP_HOST_POWER_MOONRAKER) {  /* Moonraker decides and does it; a refusal leaves the host up, so the dialog can be closed. */
        create_simple_dialog(lv_scr_act(), "Reboot Host Requested", "Reboot of the host has been requested", true, false);
        ws.send_jsonrpc("machine.reboot");
      } else {  /* The command, after the dialog has had a chance to draw. */
        lv_obj_t *mbox = create_simple_dialog(lv_scr_act(), "Reboot Host Initiated", "Reboot of host has been initiated", false, false);
        run_command_deferred(mbox, reboot_host_cmd,
                             "Reboot Host Failed", "Failed to reboot host!",
                             DEFERRED_COMMAND_DELAY_MS);
      }
    } else if (btn == support_zip_btn.get_container()) {  /* Support ZIP: run its command and say where the result went. */
      Config *conf = Config::get_instance();
      auto support_zip_cmd = conf->get<std::string>("/commands/support_zip_cmd");
      if (support_zip_cmd != "") {  /* A command is configured (the tile is hidden otherwise). */
        auto ret = call_command(support_zip_cmd);
        if (ret == 0) {  /* Made. */
          create_simple_dialog(lv_scr_act(), "Support ZIP Success", "The support.zip can be found in the config directory!", true, false);
        } else {  /* The command failed. */
          create_simple_dialog(lv_scr_act(), "Support ZIP Failed", "Failed to generate a support zip!", true, true);
        }
      }
    } else if (btn == switch_to_stock_btn.get_container()) {  /* Switch to stock: dialog first, then the long-running command. */
      Config *conf = Config::get_instance();
      auto switch_to_stock_cmd = conf->get<std::string>("/commands/switch_to_stock_cmd");
      lv_obj_t *mbox = create_simple_dialog(lv_scr_act(), SWITCH_TO_STOCK_BUTTON_TITLE " Initiated", SWITCH_TO_STOCK_BUTTON_SUCCESS, false, false);
      run_command_deferred(mbox, switch_to_stock_cmd,
                           SWITCH_TO_STOCK_BUTTON_TITLE " Failed", SWITCH_TO_STOCK_BUTTON_FAILURE,
                           DEFERRED_COMMAND_DELAY_MS);
    } else if (btn == factory_reset_btn.get_container()) {  /* Factory reset: dialog first and left up longer, then the command. */
      lv_obj_t *mbox  = create_simple_dialog(lv_scr_act(), FACTORY_RESET_BUTTON_TITLE " Initiated", FACTORY_RESET_BUTTON_SUCCESS, false, false);

      Config *conf = Config::get_instance();
      auto cmd = conf->get<std::string>("/commands/factory_reset_cmd");
      run_command_deferred(mbox, cmd,
                           FACTORY_RESET_BUTTON_TITLE " Failed", FACTORY_RESET_BUTTON_FAILURE,
                           FACTORY_RESET_DELAY_MS);
    }
  }
}
