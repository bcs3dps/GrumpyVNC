#include "system_panel.h"
#include "theme.h"
#include "main_options.h"
#include "vnc/lv_vnc.h"

static WifiPanelOptions embedded_wifi_options(lv_obj_t *parent) {
  WifiPanelOptions opts;
  opts.parent = parent;
  opts.show_refresh_button = false;
  opts.list_grow = 3;
  opts.detail_grow = 2;
  opts.flush = true;
  return opts;
}

/*--------------------------------------------------------------------------*/
/* sysp_wifi_wanted:                                                        */
/*                                                                          */
/* Whether the System page gets a WiFi panel at all.  On a local panel it   */
/* always does, as upstream.  In VNC mode only when MAIN_VncWifiWanted says  */
/* so (--vnc-wifi on, or [vnc] wifi: on): served from a machine with no     */
/* panel of its own, the WiFi panel would manage THAT machine's WiFi, and a */
/* mis-tap could take it -- and the VNC connection -- off the network.       */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     bool : true to build the WiFi tab and the WiFi panel.                */
/*--------------------------------------------------------------------------*/

static bool sysp_wifi_wanted(void)
{
    if (!LVNC_IsStarted())
    {   /* A local panel: always, as upstream. */
        return(true);
    }

    /* VNC mode: only when asked for. */
    return(MAIN_VncWifiWanted());
}

/*--------------------------------------------------------------------------*/
/* sysp_network_page:                                                       */
/*                                                                          */
/* The WiFi tab, when the WiFi panel is wanted; otherwise no tab at all, so */
/* the tab view holds Info alone and its one button fills the bar.  (LVGL   */
/* 8.3 cannot remove a tab once added, and a hidden tab button still keeps  */
/* its share of the bar, so leaving it out is the only way to get a full-   */
/* width Info.)  Called before Info is added, so that WiFi, when present,   */
/* is the first tab, as upstream orders them.                               */
/*                                                                          */
/* Arguments:                                                               */
/*     tabview : the System page's tab view, with no tabs yet.              */
/*                                                                          */
/* Returns:                                                                 */
/*     lv_obj_t * : the WiFi tab, or nullptr when there is to be none.      */
/*--------------------------------------------------------------------------*/

static lv_obj_t *sysp_network_page(lv_obj_t *tabview)
{
    if (sysp_wifi_wanted())
    {   /* Wanted: the WiFi tab, first, as upstream. */
        return(lv_tabview_add_tab(tabview, "WiFi"));
    }

    /* Not wanted: no tab. */
    return(nullptr);
}

/*--------------------------------------------------------------------------*/
/* sysp_make_wifi:                                                          */
/*                                                                          */
/* Build the WiFi panel into its tab, or nothing when there is no tab.  The */
/* panel brings its own widgets (a list, an on-screen keyboard, a spinner   */
/* whose animation runs for its whole life, drawn or hidden) and a          */
/* wpa_supplicant monitor on a thread of its own, so not building it is     */
/* what saves all of them.  When built, it finds the supplicant through the */
/* WPA_SUPPLICANT_SOCKET environment variable (default                      */
/* /var/run/wpa_supplicant), so a relay offering another machine's          */
/* supplicant there would need no change here.                             */
/*                                                                          */
/* Arguments:                                                               */
/*     l     : the LVGL lock, for the panel's background updates.           */
/*     pPage : the WiFi tab, or nullptr when there is none.                 */
/*                                                                          */
/* Returns:                                                                 */
/*     WifiPanel * : the panel, owned by the SystemPanel and deleted in its  */
/*                   destructor; nullptr when not built.                    */
/*--------------------------------------------------------------------------*/

static WifiPanel *sysp_make_wifi(std::mutex &l, lv_obj_t *pPage)
{
    if (pPage == nullptr)
    {   /* No WiFi tab: no panel and no monitor thread. */
        return(nullptr);
    }

    /* A WiFi tab: the panel fills it, embedded as upstream embeds it. */
    return(new WifiPanel(l, embedded_wifi_options(pPage)));
}

/*--------------------------------------------------------------------------*/
/* SystemPanel::SystemPanel:                                                */
/*                                                                          */
/* Build the System page: a tab view with the WiFi panel and the system     */
/* information panel, each filling its tab, refreshed when its tab is       */
/* chosen.                                                                  */
/*                                                                          */
/* In VNC mode the WiFi panel is NOT BUILT unless --vnc-wifi on ([vnc]      */
/* wifi: on) asks for it (sysp_wifi_wanted): no WiFi tab, no WiFi widgets,  */
/* no supplicant monitor thread, and Info is the only tab, filling the bar. */
/* Asked for, the WiFi tab is there exactly as on a local panel.            */
/*                                                                          */
/* Arguments:                                                               */
/*     l      : the LVGL lock, for the WiFi panel's background updates.    */
/*     parent : the tab this page fills, or nullptr for a page of its own.  */
/*                                                                          */
/* Returns:                                                                 */
/*     (constructor) : nothing.                                             */
/*--------------------------------------------------------------------------*/

SystemPanel::SystemPanel(std::mutex &l, lv_obj_t *parent)
  : owns_cont(parent == nullptr)
  , cont(Theme::create_screen(parent))  // fills the tab: it is the page
  , tabview(lv_tabview_create(cont, LV_DIR_TOP, Theme::scale_r(36)))
  , network_tab(sysp_network_page(tabview))    // before Info, so WiFi (when built) is the first tab
  , info_tab(lv_tabview_add_tab(tabview, "Info"))
  , wifi_panel(sysp_make_wifi(l, network_tab))  // nullptr when there is no WiFi tab
  , sysinfo_panel(info_tab)
{
  /* The page loses its own padding and the tab view is sized to the    */
  /* whole page, so the tab bar and the pages reach the page's edges.   */
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));

  /* Choosing a tab refreshes the page chosen (refresh_active_tab):     */
  /* the WiFi panel starts a scan, and the information panel rebuilds   */
  /* its network and version lines.                                     */
  lv_obj_add_event_cb(tabview, &SystemPanel::_tabview_event_cb,
                      LV_EVENT_VALUE_CHANGED, this);

  /* Each page loses its padding as well, so its panel fills it edge to  */
  /* edge -- the WiFi page only when there is one.                        */
  if (network_tab != nullptr) {  /* The WiFi tab exists. */
    lv_obj_set_style_pad_all(network_tab, 0, 0);
  }
  lv_obj_set_style_pad_all(info_tab, 0, 0);

  /* The tab bar's look: square buttons on an opaque, unpadded,   */
  /* borderless bar in the theme's surface colour.                */
  Theme::style_embedded_tabview(tabview);
}

/*--------------------------------------------------------------------------*/
/* SystemPanel::~SystemPanel:                                               */
/*                                                                          */
/* Tear the System page down: its widgets when this panel made its own      */
/* screen, then the WiFi panel if one was built.  The WiFi panel goes AFTER */
/* the widgets, the order it went in when it was a member (members are      */
/* destroyed after the destructor's body), and its own destructor stops its */
/* supplicant monitor thread.                                               */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     (destructor) : nothing.                                              */
/*--------------------------------------------------------------------------*/

SystemPanel::~SystemPanel() {
  /* A page of its own (no parent tab): delete it and every widget on it. */
  if (owns_cont && cont != NULL) {  /* This panel created the page. */
    lv_obj_del(cont);
    cont = NULL;
  }

  /* The WiFi panel, when one was built. */
  if (wifi_panel != nullptr) {  /* Built: free it. */
    delete wifi_panel;
    wifi_panel = nullptr;
  }
}

void SystemPanel::foreground() {
  refresh_active_tab();
}

void SystemPanel::_tabview_event_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
    static_cast<SystemPanel *>(lv_event_get_user_data(event))->refresh_active_tab();
  }
}

/*--------------------------------------------------------------------------*/
/* SystemPanel::refresh_active_tab:                                         */
/*                                                                          */
/* Refresh the page that is showing: the WiFi panel starts a scan, the      */
/* information panel rebuilds its network and version lines.  Runs when a  */
/* tab is chosen and when the System page comes to the front.  With no      */
/* WiFi panel built there is no WiFi tab, so only Info can be showing.       */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void SystemPanel::refresh_active_tab() {
  /* The active tab's index among the tab view's pages says which it is. */
  const uint16_t idx = lv_tabview_get_tab_act(tabview);
  if ((network_tab != nullptr) && (idx == lv_obj_get_index(network_tab))) {  /* The WiFi tab. */
    wifi_panel->foreground();
  } else if (idx == lv_obj_get_index(info_tab)) {  /* The information tab. */
    sysinfo_panel.foreground();
  }
}
