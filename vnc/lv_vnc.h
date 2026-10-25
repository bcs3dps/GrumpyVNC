/*--------------------------------------------------------------------------*/
/* Copyright (c) 2026 B. C. Services                                         */
/* Licensed under GPL-3.0.                                                   */
/*--------------------------------------------------------------------------*/
/* lv_vnc.h:                                                                */
/*                                                                          */
/* The LVGL side of the VNC display: an LVGL 8.3 display driver whose flush  */
/* pushes each rendered area into the RFB server (vnc_server.h) and commits  */
/* the frame when LVGL flushes the last area of a refresh, plus a pointer    */
/* input device fed by client input.  This is the ONLY file that knows both  */
/* LVGL and the server; the server knows nothing of LVGL.                    */
/*                                                                          */
/* One VNC display per process: the host calls LVNC_Start once, then points */
/* its lv_disp_drv_t.flush_cb at LVNC_Flush and registers a pointer indev    */
/* whose read_cb is LVNC_IndevRead.  Threading: LVGL calls both callbacks on */
/* its own thread (inside lv_timer_handler); client input arrives on the     */
/* server's network thread and is only queued there, to be drained by        */
/* LVNC_IndevRead on the LVGL thread -- LVGL is not thread-safe.             */
/*--------------------------------------------------------------------------*/

#ifndef LV_VNC_H
#define LV_VNC_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef GUPPY_NO_VNC
/*--------------------------------------------------------------------------*/
/* LVNC_* stand-ins for a build WITHOUT VNC support (GUPPY_NO_VNC):         */
/*                                                                          */
/* The RFB server and this module's code are not compiled at all, to save   */
/* space, and these do-nothing versions keep every caller building          */
/* unchanged.  LVNC_IsStarted is always 0, so a host that branches on it    */
/* never takes a VNC path; LVNC_Start always fails; LVNC_Flush only reports  */
/* the flush done (so LVGL could not hang even if it were wired up);        */
/* LVNC_IndevRead reports no press; LVNC_SetBlank and LVNC_Stop do nothing. */
/*                                                                          */
/* Arguments:                                                               */
/*     as the real functions below; all ignored.                           */
/*                                                                          */
/* Returns:                                                                 */
/*     LVNC_Start -1, LVNC_IsStarted 0; the rest void.                      */
/*--------------------------------------------------------------------------*/

static inline int LVNC_Start(int iWidth, int iHeight, int iPort, const char *pcListenAddr, int bHextile, const char *pcPassword)
{
    /* No server in this build. */
    return(-1);
}

static inline int LVNC_IsStarted(void)
{
    /* Never started: the host stays on its local panel. */
    return(0);
}

static inline void LVNC_Flush(lv_disp_drv_t *pDrv, const lv_area_t *pArea, lv_color_t *pPixels)
{
    /* Nothing to draw into; release LVGL's buffer at once. */
    lv_disp_flush_ready(pDrv);
}

static inline void LVNC_IndevRead(lv_indev_drv_t *pDrv, lv_indev_data_t *pData)
{
    /* No clients, so never pressed. */
    pData->state = LV_INDEV_STATE_RELEASED;
}

static inline void LVNC_SetBlank(int bBlank)
{
    /* Nothing to blank. */
}

static inline void LVNC_Stop(void)
{
    /* Nothing to stop. */
}

#else /* GUPPY_NO_VNC */

/*--------------------------------------------------------------------------*/
/* LVNC_Start:                                                              */
/*                                                                          */
/* Create and start the RFB server sized to the LVGL display, taking pixels  */
/* in LVGL's own layout -- 32-bit BGRA8888, or 16-bit RGB565 when LVGL is    */
/* built for 16 bits without byte swapping -- and wire its input callback to */
/* this module's queue.  Call once, before registering the display driver,   */
/* so the very first flush already has a server to land in.                  */
/*                                                                          */
/* Authentication follows pcPassword: a non-empty password makes the server */
/* require VNC Authentication (RFC 6143 section 7.2.2, DES challenge-        */
/* response), and NULL or an empty string leaves it serving security type   */
/* None as before.                                                          */
/*                                                                          */
/* Arguments:                                                               */
/*     iWidth       : display width in pixels (the LVGL hor_res).           */
/*     iHeight      : display height in pixels (the LVGL ver_res).          */
/*     iPort        : TCP port to listen on; 0 or less selects 5900.        */
/*     pcListenAddr : IPv4 address to bind, or NULL for every interface.    */
/*     bHextile     : non-zero lets a client that lists Hextile before Raw  */
/*                    have Hextile; zero sends Raw to every client.         */
/*     pcPassword   : VNC password (first 8 bytes used), or NULL / empty    */
/*                    for no authentication.                                 */
/*                                                                          */
/* Returns:                                                                 */
/*     int : 0 once the server is listening; non-zero if it could not be     */
/*           created or started (the reason has been logged).               */
/*--------------------------------------------------------------------------*/

int LVNC_Start(int iWidth, int iHeight, int iPort, const char *pcListenAddr, int bHextile, const char *pcPassword);

/*--------------------------------------------------------------------------*/
/* LVNC_IsStarted:                                                          */
/*                                                                          */
/* Whether the VNC display is running -- the one test the host program uses */
/* to choose between its VNC and its panel paths (display driver, input,    */
/* blanking, panels that only make sense on a local panel).  It is the same  */
/* state LVNC_Flush tests, so the host can never disagree with the driver.  */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     int : non-zero between a successful LVNC_Start and LVNC_Stop, zero   */
/*           otherwise.                                                     */
/*--------------------------------------------------------------------------*/

int LVNC_IsStarted(void);

/*--------------------------------------------------------------------------*/
/* LVNC_Flush:                                                              */
/*                                                                          */
/* The LVGL flush_cb.  Hands the rendered area to the server's working       */
/* buffer, commits the frame when lv_disp_flush_is_last() says this was the  */
/* last area of the refresh, and tells LVGL the buffer is free.  The copy    */
/* is synchronous, so LVGL may reuse its draw buffer at once.                */
/*                                                                          */
/* Arguments:                                                               */
/*     pDrv    : the LVGL display driver being flushed.                     */
/*     pArea   : the area rendered, in display coordinates, inclusive.      */
/*     pPixels : its pixels, row-major, exactly pArea's width per row.      */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; LVGL learns completion through lv_disp_flush_ready. */
/*--------------------------------------------------------------------------*/

void LVNC_Flush(lv_disp_drv_t *pDrv, const lv_area_t *pArea, lv_color_t *pPixels);

/*--------------------------------------------------------------------------*/
/* LVNC_IndevRead:                                                          */
/*                                                                          */
/* The LVGL pointer read_cb.  Reports the next queued client pointer event,  */
/* and sets continue_reading while more are queued, so a quick tap whose     */
/* press and release both arrive between two LVGL polls still reaches LVGL   */
/* as a press followed by a release.  With nothing queued it reports the     */
/* last known position and button state, as LVGL expects every poll.        */
/*                                                                          */
/* Arguments:                                                               */
/*     pDrv  : the LVGL input driver; not used, since there is one queue.    */
/*     pData : out; point, state and continue_reading are filled in.        */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void LVNC_IndevRead(lv_indev_drv_t *pDrv, lv_indev_data_t *pData);

/*--------------------------------------------------------------------------*/
/* LVNC_SetBlank:                                                           */
/*                                                                          */
/* The host's screen-sleep hook for VNC mode: serve black while blanked,     */
/* the live frame again when restored.  A client that turns its own          */
/* backlight off after a spell of black therefore sleeps with the host.      */
/*                                                                          */
/* Arguments:                                                               */
/*     bBlank : non-zero to blank, zero to restore.                         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; a no-op before LVNC_Start.                           */
/*--------------------------------------------------------------------------*/

void LVNC_SetBlank(int bBlank);

/*--------------------------------------------------------------------------*/
/* LVNC_Stop:                                                               */
/*                                                                          */
/* Stop and free the server.  After it returns no client input arrives and  */
/* flushes are dropped; the display driver should not be flushed again.     */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; safe to call when never started.                     */
/*--------------------------------------------------------------------------*/

void LVNC_Stop(void);

#endif /* GUPPY_NO_VNC */

#ifdef __cplusplus
}
#endif

#endif /* LV_VNC_H */
