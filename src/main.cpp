#include "lvgl/lvgl.h"
#include "theme.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#ifdef GUPPY_SDL
#include "lv_drivers/sdl/sdl.h"
#endif
#ifdef GUPPY_CALIBRATE
#include "lv_tc.h"
#include "lv_tc_screen.h"
#endif
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <cstdlib>
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;

static void hal_init(lv_color_t p, lv_color_t s);

#include "guppyscreen.h"
#include "hv/hlog.h"
#include "config.h"
#include "main_options.h"
#include "vnc/lv_vnc.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

using namespace hv;

/*--------------------------------------------------------------------------*/
/* LVGL's draw buffers (hal_init).  Upstream's are two of 128K pixels, 512   */
/* KB each at 32 bpp -- about 246 rows of a 532-pixel-wide panel apiece.  A  */
/* build can shrink them with two make variables: GUPPY_DRAW_BUF_PX=<n>      */
/* sets the pixels in each buffer, and GUPPY_DRAW_BUF_SINGLE=1 keeps one     */
/* buffer instead of two.  LVGL's own guidance is at least a tenth of the    */
/* screen; smaller only means a refresh is drawn in more, shorter parts.     */
/* A buffer must hold at least one whole display row, which hal_init checks */
/* against the display actually opened.                                     */
/*--------------------------------------------------------------------------*/

#ifdef GUPPY_DRAW_BUF_PX
#if (GUPPY_DRAW_BUF_PX) < 1
#error "GUPPY_DRAW_BUF_PX must be a positive number of pixels"
#endif
#define DISP_BUF_SIZE (GUPPY_DRAW_BUF_PX)          // pixels in each draw buffer, as the build set it
#else
#define DISP_BUF_SIZE (128 * 1024)                 // pixels in each draw buffer: upstream's default
#endif

/*--------------------------------------------------------------------------*/
/* Startup options.  Every one can also be set in the configuration file's   */
/* [vnc] section under the key named beside it; the command line wins.      */
/*--------------------------------------------------------------------------*/

#define MAIN_VNC_DEFAULT_WIDTH   532             // VNC display width when neither option nor config sets it (a 532x300 panel)
#define MAIN_VNC_DEFAULT_HEIGHT  300             // VNC display height, likewise
#define MAIN_VNC_MAX_SIDE        4096            // largest VNC width or height accepted, in pixels
#define MAIN_PORT_MAX            65535           // a TCP port is 16 bits

struct MainOptions
{
    std::string sDisplay;                        // --display (config "display"): "fbdev" or "vnc"; empty if not given
    std::string sListen;                         // --vnc-listen (config "listen"): IPv4 address to bind; empty if not given
    long        lPort;                           // --vnc-port (config "port"): TCP port; 0 if not given (5900)
    long        lWidth;                          // --vnc-width (config "width"): pixels; 0 if not given
    long        lHeight;                         // --vnc-height (config "height"): pixels; 0 if not given
    std::string sHextile;                        // --vnc-hextile (config "hextile"): "on" or "off"; empty if not given (on)
    std::string sWifi;                           // --vnc-wifi (config "wifi"): "on" or "off"; empty if not given (off)
    std::string sPassword;                       // --vnc-password (config "password"): VNC password; empty = no auth
};

static int MAIN_iVncWidth = MAIN_VNC_DEFAULT_WIDTH;    // the VNC display's width, read by hal_init in VNC mode
static int MAIN_iVncHeight = MAIN_VNC_DEFAULT_HEIGHT;  // the VNC display's height, read by hal_init in VNC mode
static bool MAIN_bVncWifi = false;                     // VNC mode builds the WiFi panel; answered by MAIN_VncWifiWanted

/*--------------------------------------------------------------------------*/
/* MAIN_VncWifiWanted:                                                      */
/*                                                                          */
/* Answer, for the System page, whether VNC mode builds the WiFi panel --   */
/* what main() decided from --vnc-wifi, else [vnc] wifi, else off (see     */
/* main_options.h for why it is off by default).  main() sets the answer   */
/* before GuppyScreen::init builds any panel.                               */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     bool : true when the WiFi panel is wanted in VNC mode.               */
/*--------------------------------------------------------------------------*/

bool MAIN_VncWifiWanted(void)
{
    /* Decided once in main(); never changes afterwards. */
    return(MAIN_bVncWifi);
}

/*--------------------------------------------------------------------------*/
/* main_usage:                                                              */
/*                                                                          */
/* Print the command line to stderr: for --help, and after a bad option so  */
/* the person sees what was expected.                                        */
/*                                                                          */
/* Arguments:                                                               */
/*     pcProgram : the program's name as it was run (argv[0]).              */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void main_usage(const char *pcProgram)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --display fbdev|vnc   where to draw: the local panel (fbdev + evdev, the default)\n"
            "                        or the built-in VNC server\n"
            "  --vnc-port N          VNC TCP port (default 5900)\n"
            "  --vnc-listen ADDR     IPv4 address to listen on (default: every interface)\n"
            "  --vnc-width N         VNC display width in pixels (default %d)\n"
            "  --vnc-height N        VNC display height in pixels (default %d)\n"
            "  --vnc-hextile on|off  let VNC clients that ask for Hextile have it (default on);\n"
            "                        off sends Raw to every client\n"
            "  --vnc-wifi on|off     in VNC mode, build the WiFi panel (default off); it manages\n"
            "                        the WiFi of the machine this program runs on\n"
            "  --vnc-password PW     require VNC authentication with password PW (first 8 bytes\n"
            "                        used; visible in ps); default: no authentication\n"
            "  --help                this text\n"
            "Each option can also be set in the configuration file's [vnc] section as\n"
            "display, port, listen, width, height, hextile, wifi and password; the command\n"
            "line wins.\n"
            "The configuration file is $CONFIG_FILE, or grumpyscreen.cfg beside the program.\n",
            pcProgram, MAIN_VNC_DEFAULT_WIDTH, MAIN_VNC_DEFAULT_HEIGHT);
}

/*--------------------------------------------------------------------------*/
/* main_parse_number:                                                       */
/*                                                                          */
/* Read a whole decimal number and hold it to a range.  strtol alone would   */
/* accept "12abc" as 12 and an empty string as 0, so the end pointer and     */
/* errno are both checked: anything but a complete in-range number fails.    */
/*                                                                          */
/* Arguments:                                                               */
/*     pcText : the text.                                                   */
/*     lMin   : smallest value accepted.                                    */
/*     lMax   : largest value accepted.                                     */
/*     plOut  : out; the value, set only on success.                        */
/*                                                                          */
/* Returns:                                                                 */
/*     bool : true when pcText is a whole number in range.                  */
/*--------------------------------------------------------------------------*/

static bool main_parse_number(const char *pcText, long lMin, long lMax, long *plOut)
{
    char *pcEnd;
    long  lValue;

    errno = 0;
    lValue = strtol(pcText, &pcEnd, 10);
    if ((pcEnd == pcText) || (*pcEnd != '\0') || (errno != 0) || (lValue < lMin) || (lValue > lMax))
    {   /* Not a whole number, or out of range. */
        return(false);
    }

    /* A good number. */
    *plOut = lValue;
    return(true);
}

/*--------------------------------------------------------------------------*/
/* main_parse_options:                                                      */
/*                                                                          */
/* Read the command line into a MainOptions.  Every option that takes a     */
/* value must have one; numbers are checked for form and range here, and so */
/* is an on / off switch; the display name is checked later with the        */
/* configuration's value, since either can supply it.  An unknown option is */
/* an error, not ignored, so a mistyped option cannot silently leave the     */
/* program on the wrong display.                                             */
/*                                                                          */
/* Arguments:                                                               */
/*     argc, argv : the command line.                                        */
/*     pOpt       : out; what was given (the rest left empty / zero).       */
/*                                                                          */
/* Returns:                                                                 */
/*     int : 0 to run; 1 for a bad command line (usage printed); 2 when    */
/*           --help was asked for (usage printed).                          */
/*--------------------------------------------------------------------------*/

static int main_parse_options(int argc, char **argv, MainOptions *pOpt)
{
    int         i;
    const char *pcArg;
    const char *pcValue;

    /* Nothing given yet: empty text and zero numbers mean "not on the command line". */
    pOpt->sDisplay.clear();
    pOpt->sListen.clear();
    pOpt->lPort = 0;
    pOpt->lWidth = 0;
    pOpt->lHeight = 0;
    pOpt->sHextile.clear();
    pOpt->sWifi.clear();
    pOpt->sPassword.clear();

    for (i = 1; i < argc; i++)
    {   /* One option per iteration; a value-taking option consumes the next word too. */
        pcArg = argv[i];

        if ((strcmp(pcArg, "--help") == 0) || (strcmp(pcArg, "-h") == 0))
        {   /* Help: show the usage and stop. */
            main_usage(argv[0]);
            return(2);
        }

        /* Every remaining option takes a value: the next word. */
        if (i + 1 >= argc)
        {   /* Nothing follows it (or it is unknown and last). */
            fprintf(stderr, "%s: option '%s' is unknown or has no value\n", argv[0], pcArg);
            main_usage(argv[0]);
            return(1);
        }
        pcValue = argv[i + 1];

        if (strcmp(pcArg, "--display") == 0)
        {   /* Where to draw; checked once the config is known. */
            pOpt->sDisplay = pcValue;
        }
        else if (strcmp(pcArg, "--vnc-listen") == 0)
        {   /* The bind address; the server checks its form. */
            pOpt->sListen = pcValue;
        }
        else if (strcmp(pcArg, "--vnc-port") == 0)
        {   /* A TCP port. */
            if (!main_parse_number(pcValue, 1, MAIN_PORT_MAX, &pOpt->lPort))
            {   /* Not a port number. */
                fprintf(stderr, "%s: --vnc-port needs a number from 1 to %d\n", argv[0], MAIN_PORT_MAX);
                return(1);
            }
        }
        else if (strcmp(pcArg, "--vnc-width") == 0)
        {   /* The VNC display's width. */
            if (!main_parse_number(pcValue, 1, MAIN_VNC_MAX_SIDE, &pOpt->lWidth))
            {   /* Not a usable width. */
                fprintf(stderr, "%s: --vnc-width needs a number from 1 to %d\n", argv[0], MAIN_VNC_MAX_SIDE);
                return(1);
            }
        }
        else if (strcmp(pcArg, "--vnc-height") == 0)
        {   /* The VNC display's height. */
            if (!main_parse_number(pcValue, 1, MAIN_VNC_MAX_SIDE, &pOpt->lHeight))
            {   /* Not a usable height. */
                fprintf(stderr, "%s: --vnc-height needs a number from 1 to %d\n", argv[0], MAIN_VNC_MAX_SIDE);
                return(1);
            }
        }
        else if (strcmp(pcArg, "--vnc-hextile") == 0)
        {   /* Whether Hextile is offered: exactly "on" or "off". */
            if ((strcmp(pcValue, "on") != 0) && (strcmp(pcValue, "off") != 0))
            {   /* Neither word. */
                fprintf(stderr, "%s: --vnc-hextile needs on or off\n", argv[0]);
                return(1);
            }
            pOpt->sHextile = pcValue;
        }
        else if (strcmp(pcArg, "--vnc-wifi") == 0)
        {   /* Whether VNC mode builds the WiFi panel: exactly "on" or "off". */
            if ((strcmp(pcValue, "on") != 0) && (strcmp(pcValue, "off") != 0))
            {   /* Neither word. */
                fprintf(stderr, "%s: --vnc-wifi needs on or off\n", argv[0]);
                return(1);
            }
            pOpt->sWifi = pcValue;
        }
        else if (strcmp(pcArg, "--vnc-password") == 0)
        {   /* The VNC password; any text, checked only for being non-empty later. */
            pOpt->sPassword = pcValue;
        }
        else
        {   /* Not an option this program has. */
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], pcArg);
            main_usage(argv[0]);
            return(1);
        }

        /* Step past the value just used. */
        i++;
    }

    /* The command line is good. */
    return(0);
}

#ifdef GUPPY_SDL
static int sdl_window_event_watch(void *userdata, SDL_Event *event) {
    (void) userdata;

    if (event->type == SDL_QUIT) {
        std::_Exit(0);
    }

    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_CLOSE) {
        sdl_quit_qry = true;
    }

    return 0;
}
#endif

/*--------------------------------------------------------------------------*/
/* main:                                                                    */
/*                                                                          */
/* Start GrumpyScreen: read the command line, load the configuration (and   */
/* its optional override), decide where to draw, and hand over to           */
/* GuppyScreen, whose loop never returns.                                    */
/*                                                                          */
/* The display is the local panel (fbdev + evdev, as upstream always has)   */
/* unless --display vnc, or the [vnc] section's "display: vnc", asks for the */
/* built-in VNC server.  In VNC mode the server is started HERE, before      */
/* GuppyScreen::init, because init and hal_init branch on LVNC_IsStarted()   */
/* -- starting it first is what makes that single test the source of truth  */
/* for the whole program.  A VNC server that cannot start (port in use, bad  */
/* address) stops the program rather than falling back to a panel a Pi does */
/* not have.                                                                */
/*                                                                          */
/* Arguments:                                                               */
/*     argc, argv : the command line (main_usage lists the options).        */
/*                                                                          */
/* Returns:                                                                 */
/*     int : 0 after --help (the UI loop itself never returns); 1 when the  */
/*           configuration cannot be loaded or the VNC server cannot start; */
/*           2 for a bad command line, display name or [vnc] value.         */
/*--------------------------------------------------------------------------*/

int main(int argc, char **argv) {
    MainOptions opt;
    int         iParse;
    std::string sDisplay;
    std::string sListen;
    std::string sHextileText;
    std::string sWifiText;
    std::string sPassword;
#if !defined(GUPPY_SDL) && !defined(GUPPY_NO_VNC)
    const char *pcListen;                        // used only where the VNC server can be started
    const char *pcPassword;                      // VNC password, or NULL for no authentication
    long        lPort;
    int         bHextile;
#endif

    /* STEP 1: the command line, before anything else, so --help and a typo */
    /* are answered without touching the configuration.                    */
    iParse = main_parse_options(argc, argv, &opt);
    if (iParse == 2) {  /* --help: the usage is printed, nothing more to do. */
        return(0);
    }
    if (iParse != 0) {  /* A bad command line: the reason and usage are printed. */
        return(2);
    }

    /* STEP 2: the configuration file -- $CONFIG_FILE, or grumpyscreen.cfg */
    /* beside the program.  Without it there is nothing to connect to.    */
    const char* config_file_env = std::getenv("CONFIG_FILE");
    fs::path config_path = config_file_env && config_file_env[0] != '\0'
        ? fs::path(config_file_env)
        : fs::canonical("/proc/self/exe").parent_path() / "grumpyscreen.cfg";

    Config *conf = Config::get_instance();
    if (fs::exists(config_path)) {  /* Present: it must also parse. */
        if (!conf->load(config_path.string())) {  /* Present but unreadable or malformed. */
            LOG_ERROR("Failed to load {}", config_path.string());
            return 1;
        }
    } else {  /* Missing altogether. */
        LOG_ERROR("Config file {} not found", config_path.string());
        return 1;
    }

    /* STEP 3: the optional override file, merged over the configuration. */
    const char* config_override_env = std::getenv("CONFIG_OVERRIDE_FILE");
    if (config_override_env && config_override_env[0] != '\0') {  /* An override is named. */
        fs::path config_override_path = fs::path(config_override_env);
        if (fs::exists(config_override_path)) {  /* Present: merge it, or stop if it will not load. */
            if (!conf->load_override(config_override_path.string())) {  /* Present but will not load. */
                LOG_ERROR("Failed to load override config {}", config_override_path.string());
                return 1;
            }
        } else {  /* Named but absent: carry on with the base configuration. */
            LOG_INFO("Override config file {} not found, continuing without overrides", config_override_path.string());
        }
    }

    /* STEP 4: where to draw -- the command line, else [vnc] display, else */
    /* the local panel.                                                    */
    sDisplay = opt.sDisplay;
    if (sDisplay.empty()) {  /* Not on the command line: the configuration decides. */
        sDisplay = conf->get<std::string>("/vnc/display", "fbdev");
    }
    if ((sDisplay != "fbdev") && (sDisplay != "vnc")) {  /* Neither display this program has. */
        fprintf(stderr, "%s: display '%s' is not 'fbdev' or 'vnc'\n", argv[0], sDisplay.c_str());
        return(2);
    }

    if (sDisplay == "vnc") {  /* STEP 5, VNC only: size, port and address, then start the server. */
#ifdef GUPPY_SDL
        /* The SDL build draws into a desktop window and has no VNC display. */
        fprintf(stderr, "%s: the VNC display is not available in the SDL build\n", argv[0]);
        return(2);
#elif defined(GUPPY_NO_VNC)
        /* Built without VNC support (GUPPY_NO_VNC): only the local panel. */
        fprintf(stderr, "%s: this build has no VNC support (built with GUPPY_NO_VNC)\n", argv[0]);
        return(2);
#else
        /* Each setting: the command line, else [vnc], else the default. */
        MAIN_iVncWidth = (int)opt.lWidth;
        if (MAIN_iVncWidth == 0) {  /* Not on the command line. */
            MAIN_iVncWidth = conf->get<int32_t>("/vnc/width", MAIN_VNC_DEFAULT_WIDTH);
        }
        MAIN_iVncHeight = (int)opt.lHeight;
        if (MAIN_iVncHeight == 0) {  /* Not on the command line. */
            MAIN_iVncHeight = conf->get<int32_t>("/vnc/height", MAIN_VNC_DEFAULT_HEIGHT);
        }
        lPort = opt.lPort;
        if (lPort == 0) {  /* Not on the command line; 0 from both means the server's default. */
            lPort = conf->get<int32_t>("/vnc/port", 0);
        }
        sListen = opt.sListen;
        if (sListen.empty()) {  /* Not on the command line; empty from both means every interface. */
            sListen = conf->get<std::string>("/vnc/listen", "");
        }

        if ((MAIN_iVncWidth < 1) || (MAIN_iVncWidth > MAIN_VNC_MAX_SIDE) ||
            (MAIN_iVncHeight < 1) || (MAIN_iVncHeight > MAIN_VNC_MAX_SIDE) ||
            (lPort < 0) || (lPort > MAIN_PORT_MAX)) {  /* A configuration value out of range. */
            fprintf(stderr, "%s: VNC size %dx%d or port %ld out of range\n", argv[0],
                    MAIN_iVncWidth, MAIN_iVncHeight, lPort);
            return(2);
        }

        /* An empty address means every interface, which the server spells NULL. */
        pcListen = NULL;
        if (!sListen.empty()) {  /* A specific address was given. */
            pcListen = sListen.c_str();
        }

        /* Hextile: the command line, else [vnc] hextile, else on.  The      */
        /* configuration reader turns on / off / true / false / yes / no /    */
        /* 1 / 0 into a boolean, other whole numbers into numbers and the     */
        /* rest into text, so text found here is a typo -- refused rather     */
        /* than read as "on".  (A number other than 0 or 1 is not text, and   */
        /* falls to the default, on.)                                         */
        if (!opt.sHextile.empty()) {  /* On the command line, already checked to be on or off. */
            bHextile = 0;
            if (opt.sHextile == "on") {  /* Hextile for the clients that ask for it. */
                bHextile = 1;
            }
        } else {  /* Not on the command line: the configuration, which defaults to on. */
            sHextileText = conf->get<std::string>("/vnc/hextile", "");
            if (!sHextileText.empty()) {  /* Text that is not a yes / no word. */
                fprintf(stderr, "%s: [vnc] hextile '%s' is not on or off\n", argv[0], sHextileText.c_str());
                return(2);
            }
            bHextile = 0;
            if (conf->get<bool>("/vnc/hextile", true)) {  /* On, or not set at all. */
                bHextile = 1;
            }
        }

        /* The WiFi panel: the command line, else [vnc] wifi, else off --   */
        /* read the same way as hextile, a typo refused rather than taken   */
        /* as a default.  The System page asks for it once it is built.     */
        if (!opt.sWifi.empty()) {  /* On the command line, already checked to be on or off. */
            MAIN_bVncWifi = (opt.sWifi == "on");  // a comparison, true only for "on"
        } else {  /* Not on the command line: the configuration, which defaults to off. */
            sWifiText = conf->get<std::string>("/vnc/wifi", "");
            if (!sWifiText.empty()) {  /* Text that is not a yes / no word. */
                fprintf(stderr, "%s: [vnc] wifi '%s' is not on or off\n", argv[0], sWifiText.c_str());
                return(2);
            }
            MAIN_bVncWifi = conf->get<bool>("/vnc/wifi", false);
        }

        /* The VNC password: the command line, else [vnc] password, else     */
        /* none.  An empty result from both means no authentication; the      */
        /* server requires it only when the string is non-empty.              */
        sPassword = opt.sPassword;
        if (sPassword.empty()) {  /* Not on the command line: the configuration decides. */
            sPassword = conf->get<std::string>("/vnc/password", "");
        }
        pcPassword = NULL;
        if (!sPassword.empty()) {  /* A password was given: VNC authentication is required. */
            pcPassword = sPassword.c_str();
        }

        /* The server logs its own reason if it cannot listen. */
        if (LVNC_Start(MAIN_iVncWidth, MAIN_iVncHeight, (int)lPort, pcListen, bHextile, pcPassword) != 0) {  /* No VNC display: nothing to draw on. */
            fprintf(stderr, "%s: the VNC server could not start\n", argv[0]);
            return(1);
        }
#endif
    }

    /* STEP 6: GrumpyScreen itself; the loop runs for the life of the program. */
    GuppyScreen::init(hal_init);
    GuppyScreen::loop();

    /* Not reached: the loop does not return. */
    return 0;
}

/*--------------------------------------------------------------------------*/
/* hal_init:                                                                */
/*                                                                          */
/* The display and input half of GrumpyScreen's start-up, called back from  */
/* GuppyScreen::init once LVGL is initialised: give LVGL its draw buffers    */
/* (two, or one in a build made with GUPPY_DRAW_BUF_SINGLE), register the    */
/* display driver and the pointer input device for whichever display this  */
/* run uses, and install the default theme sized to that display.  A draw  */
/* buffer too small for one row of that display ends the program (exit 1)  */
/* here, before LVGL can hang on it.                                        */
/*                                                                          */
/* Three displays: the SDL desktop window (a simulator build); the VNC       */
/* server when main started it (LVNC_IsStarted); otherwise the local panel,  */
/* /dev/fb0 through fbdev with evdev touch, exactly as upstream.  In VNC      */
/* mode the size is the configured VNC size, the configured display_rotate  */
/* is NOT applied (a VNC client shows the frame the way it is sent, and a    */
/* rotation meant for a mounted panel would turn it on its side), and touch  */
/* comes from the clients' pointer events instead of a device.              */
/*                                                                          */
/* Arguments:                                                               */
/*     primary   : the theme's primary colour.                              */
/*     secondary : the theme's secondary colour.                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; LVGL holds everything registered here.               */
/*--------------------------------------------------------------------------*/

static void hal_init(lv_color_t primary, lv_color_t secondary) {
    lv_disp_t * disp = nullptr;
    long        lRow;                              // the display's longer side, in pixels: one row at any rotation

    /* The draw buffers, static because LVGL keeps pointers to them.  Two by */
    /* default, as upstream; one in a GUPPY_DRAW_BUF_SINGLE build.  Every    */
    /* flush callback here (the framebuffer, the VNC server, SDL) finishes   */
    /* its copy and calls lv_disp_flush_ready before it returns, so LVGL     */
    /* never draws into one buffer while the other is being flushed: the     */
    /* second buffer costs its memory and buys no overlap.                   */
    static lv_color_t buf[DISP_BUF_SIZE];
#ifndef GUPPY_DRAW_BUF_SINGLE
    static lv_color_t buf2[DISP_BUF_SIZE];
#endif

    /* The descriptor LVGL draws through: the one buffer, or both. */
    static lv_disp_draw_buf_t disp_buf;
#ifdef GUPPY_DRAW_BUF_SINGLE
    lv_disp_draw_buf_init(&disp_buf, buf, nullptr, DISP_BUF_SIZE);
#else
    lv_disp_draw_buf_init(&disp_buf, buf, buf2, DISP_BUF_SIZE);
#endif

    /* The display driver: flush callback and size are set per display below. */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf   = &disp_buf;

#ifdef GUPPY_SDL
    /* The desktop simulator: an SDL window, closed through the event watch. */
    sdl_init();
    SDL_AddEventWatch(sdl_window_event_watch, nullptr);
    disp_drv.flush_cb = sdl_display_flush;
    disp_drv.hor_res = SDL_HOR_RES;
    disp_drv.ver_res = SDL_VER_RES;
#else
    if (LVNC_IsStarted()) {  /* VNC: LVGL flushes into the RFB server at the configured size, unrotated. */
        disp_drv.flush_cb   = LVNC_Flush;
        disp_drv.hor_res    = MAIN_iVncWidth;
        disp_drv.ver_res    = MAIN_iVncHeight;
        disp_drv.dpi        = LV_DPI_DEF * MAIN_iVncWidth / 480;  // LVGL defaults scale with the screen, as for a panel
    } else {  /* The local panel: /dev/fb0, sized by the framebuffer itself. */
        disp_drv.flush_cb   = fbdev_flush;

        uint32_t width;
        uint32_t height;
        uint32_t dpi;
        fbdev_get_sizes(&width, &height, &dpi);

        disp_drv.hor_res    = width;
        disp_drv.ver_res    = height;
        disp_drv.dpi        = LV_DPI_DEF * width / 480;  // LVGL defaults scale with the screen

        /* A panel mounted turned: 1 to 3 quarter turns, done in software. */
        Config *conf = Config::get_instance();
        auto rotate_value = conf->get<std::uint32_t>("/ui/display_rotate");
        if (rotate_value > 0 && rotate_value < 4) {  /* A valid rotation is configured. */
          disp_drv.sw_rotate = 1;
          disp_drv.rotated = rotate_value;
        }
    }
#endif

    /* LVGL splits each area it redraws into parts of as many whole rows as  */
    /* a draw buffer holds; a buffer shorter than one row gives parts of     */
    /* zero rows and a refresh that never ends.  Checked against the longer */
    /* side, which is the row length whichever way the display is rotated. */
    if (disp_drv.hor_res > disp_drv.ver_res) {  /* Wider than tall: the width is the longer side. */
        lRow = disp_drv.hor_res;
    } else {  /* Taller, or square: the height is. */
        lRow = disp_drv.ver_res;
    }

    if ((long)DISP_BUF_SIZE < lRow) {  /* The build's draw buffer cannot hold one row of this display: stop, saying why. */
        fprintf(stderr, "grumpyscreen: a draw buffer of %ld pixels cannot hold a %ld-pixel row of this display (build with a larger GUPPY_DRAW_BUF_PX)\n",
                (long)DISP_BUF_SIZE, lRow);
        exit(1);
    }

    disp = lv_disp_drv_register(&disp_drv);

#ifdef GUPPY_SDL
    /* The simulator's pointer is the desktop mouse. */
    static lv_indev_drv_t indev_drv_1;
    lv_indev_drv_init(&indev_drv_1);
    indev_drv_1.read_cb = sdl_mouse_read;
    indev_drv_1.type = LV_INDEV_TYPE_POINTER;
    lv_indev_drv_register(&indev_drv_1);
#else
    if (LVNC_IsStarted()) {  /* VNC: the pointer is fed by the clients' pointer events; no device, no calibration. */
        static lv_indev_drv_t vnc_indev_drv;
        lv_indev_drv_init(&vnc_indev_drv);
        vnc_indev_drv.read_cb = LVNC_IndevRead;
        vnc_indev_drv.type = LV_INDEV_TYPE_POINTER;
        lv_indev_drv_register(&vnc_indev_drv);
    } else {  /* The local panel: evdev touch, from LVGL_EVDEV_DEV or the driver's default device. */
        const char *path = std::getenv("LVGL_EVDEV_DEV");
        if (path != nullptr && path[0] != '\0') {  /* A device named in the environment. */
            LOG_INFO("Input Device is: {}", path);
            evdev_set_file(const_cast<char *>(path));
        } else {  /* None named: the driver's own default (the K1 relies on this). */
            evdev_init();
        }
        static lv_indev_drv_t indev_drv_1;
        lv_indev_drv_init(&indev_drv_1);
        indev_drv_1.read_cb = evdev_read; // no calibration
        indev_drv_1.type = LV_INDEV_TYPE_POINTER;

#ifdef GUPPY_CALIBRATE
        /* Calibrated builds read evdev through the calibration layer. */
        lv_tc_indev_drv_init(&indev_drv_1, evdev_read);
#endif
        lv_indev_drv_register(&indev_drv_1);
    }
#endif

    /* The default theme, its font scaled off the display actually in use:  */
    /* 12 on a 480x272 panel, 20 on 800x480.  Dark or light follows the      */
    /* configured page colour, so what LVGL paints on its own (scrollbars,   */
    /* cursors, chart defaults) suits a light theme too.                     */
    const bool dark = lv_color_brightness(Theme::col(Theme::BG)) < 128;
    lv_theme_t * th = lv_theme_default_init(disp, primary, secondary, dark, Theme::scale_font(12));
    lv_disp_set_theme(disp, th);
}
