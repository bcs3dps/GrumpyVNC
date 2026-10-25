/*--------------------------------------------------------------------------*/
/* Copyright (c) 2026 B. C. Services                                         */
/* Licensed under GPL-3.0.                                                   */
/*--------------------------------------------------------------------------*/
/* lv_vnc.c:                                                                */
/*                                                                          */
/* The LVGL side of the VNC display (see lv_vnc.h for the contract).  It     */
/* holds the one RFB server of the process and a small queue of client       */
/* pointer events.  The server's network thread only ever appends to the     */
/* queue; LVGL's thread drains it in LVNC_IndevRead.  Everything else here   */
/* runs on LVGL's thread.                                                    */
/*--------------------------------------------------------------------------*/

#include <pthread.h>
#include <string.h>

#include "lv_vnc.h"
#include "vnc_server.h"

/*--------------------------------------------------------------------------*/
/* The server takes pixels in LVGL's own layout, so the format follows the   */
/* LVGL build.  32-bit lv_color_t is blue, green, red, alpha (BGRA8888);     */
/* 16-bit without byte swapping is little-endian RGB565.  Any other build    */
/* would hand the server bytes it would misread, so it stops the compile.    */
/*--------------------------------------------------------------------------*/

#if LV_COLOR_DEPTH == 32
#define LVNC_PIXFMT          VNC_PIXFMT_BGRA8888 // LVGL depth 32: bytes B, G, R, A
#elif (LV_COLOR_DEPTH == 16) && (LV_COLOR_16_SWAP == 0)
#define LVNC_PIXFMT          VNC_PIXFMT_RGB565   // LVGL depth 16, unswapped: RGB565 little-endian
#else
#error "lv_vnc: LVGL must be built with LV_COLOR_DEPTH 32, or 16 without LV_COLOR_16_SWAP"
#endif

#define LVNC_QUEUE_LEN       64                  // pointer events held between two LVGL polls

/*--------------------------------------------------------------------------*/
/* The pointer-event queue.  A ring buffer under its own mutex, taken on     */
/* the network thread (append) and on LVGL's thread (take).  When it is      */
/* full the OLDEST event is dropped: the newest pointer state is the one     */
/* that must not be lost.                                                    */
/*--------------------------------------------------------------------------*/

typedef struct
{
    pthread_mutex_t mtx;                         // guards the members below, on both threads
    VncInputEvent   arrEvents[LVNC_QUEUE_LEN];   // the ring of queued pointer events
    int             iHead;                       // index of the oldest queued event
    int             iCount;                      // how many events are queued
} LvncQueue;

static VncServer *LVNC_pServer = NULL;           // the process's RFB server; NULL until LVNC_Start
static LvncQueue  LVNC_queue = { PTHREAD_MUTEX_INITIALIZER, {{0}}, 0, 0 }; // client pointer events
static lv_coord_t LVNC_iLastX = 0;               // last reported pointer x (LVGL thread only)
static lv_coord_t LVNC_iLastY = 0;               // last reported pointer y (LVGL thread only)
static int        LVNC_bLastPressed = 0;         // last reported button state (LVGL thread only)

/*--------------------------------------------------------------------------*/
/* LVNC_OnInput:                                                            */
/*                                                                          */
/* The server's input callback, called ON THE NETWORK THREAD.  It only       */
/* appends a pointer event to the queue and returns, as the server's         */
/* threading contract requires.  Key events are ignored: there is no LVGL    */
/* keypad device behind this display yet, and the panel-side client sends    */
/* only pointer events.                                                      */
/*                                                                          */
/* Arguments:                                                               */
/*     pUser : the LvncQueue registered with the server.                    */
/*     pEv   : the decoded client event, valid only during this call.       */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void LVNC_OnInput(void *pUser, const VncInputEvent *pEv)
{
    LvncQueue *pQ;
    int        iTail;

    if (pEv->eType != VNC_INPUT_POINTER)
    {   /* Not a pointer event: nothing behind this display takes keys. */
        return;
    }

    /* Append under the queue's lock: the LVGL thread takes from the same ring. */
    pQ = (LvncQueue *)pUser;
    pthread_mutex_lock(&pQ->mtx);
    if (pQ->iCount == LVNC_QUEUE_LEN)
    {   /* Full: drop the oldest so the newest state survives. */
        pQ->iHead = (pQ->iHead + 1) % LVNC_QUEUE_LEN;
        pQ->iCount--;
    }

    /* Store at the tail -- the slot just past the newest queued event --   */
    /* and release the lock.                                               */
    iTail = (pQ->iHead + pQ->iCount) % LVNC_QUEUE_LEN;
    pQ->arrEvents[iTail] = *pEv;
    pQ->iCount++;
    pthread_mutex_unlock(&pQ->mtx);
}

/*--------------------------------------------------------------------------*/
/* LVNC_Start:                                                              */
/*                                                                          */
/* Build the server configuration from the display and start the server.    */
/* A non-empty pcPassword turns on VNC Authentication; NULL or an empty      */
/* string leaves the server on security type None.                          */
/*                                                                          */
/* Arguments:                                                               */
/*     iWidth, iHeight : the display size in pixels.                        */
/*     iPort           : TCP port, or 0 or less for 5900.                   */
/*     pcListenAddr    : IPv4 bind address, or NULL for every interface.    */
/*     bHextile        : non-zero allows Hextile; zero is Raw for everyone. */
/*     pcPassword      : VNC password, or NULL / empty for no auth.         */
/*                                                                          */
/* Returns:                                                                 */
/*     int : 0 when listening; -1 when already started or on failure.      */
/*--------------------------------------------------------------------------*/

int LVNC_Start(int iWidth, int iHeight, int iPort, const char *pcListenAddr, int bHextile, const char *pcPassword)
{
    VncServerCfg cfg;

    if (LVNC_pServer != NULL)
    {   /* One VNC display per process: a second start is an error. */
        return(-1);
    }

    /* The configuration: the display's size and LVGL's pixel layout, where */
    /* to listen, no authentication, and this module's input queue.         */
    /* Everything not set is zero from the memset: no log sink (so stderr)  */
    /* and the server's default client limit.                               */
    memset(&cfg, 0, sizeof(cfg));
    cfg.iWidth = iWidth;
    cfg.iHeight = iHeight;
    cfg.ePixFmt = LVNC_PIXFMT;
    cfg.iPort = iPort;
    cfg.pcListenAddr = pcListenAddr;

    /* The server's switch is the negative, so a zeroed configuration keeps */
    /* Hextile available; this module's argument is the positive.          */
    if (bHextile)
    {   /* Hextile allowed: a client's own encoding list decides. */
        cfg.bNoHextile = 0;
    }
    else
    {   /* Hextile turned off: Raw for every client. */
        cfg.bNoHextile = 1;
    }

    /* A non-empty password selects VNC Authentication; NULL or empty leaves */
    /* the server on None.                                                    */
    if ((pcPassword != NULL) && (pcPassword[0] != '\0'))
    {   /* A password was given: require the VNC challenge-response. */
        cfg.eAuth = VNC_AUTH_VNC;
        cfg.pcPassword = pcPassword;
    }
    else
    {   /* No password: serve without authentication, as before. */
        cfg.eAuth = VNC_AUTH_NONE;
    }
    cfg.pfnInput = LVNC_OnInput;
    cfg.pInputUser = &LVNC_queue;

    /* Create the server; it checks the configuration and logs a refusal. */
    LVNC_pServer = VNCS_Create(&cfg);
    if (LVNC_pServer == NULL)
    {   /* The reason has been logged by the server. */
        return(-1);
    }

    if (VNCS_Start(LVNC_pServer) != VNCS_SUCCESS)
    {   /* Could not listen: release the server so a later start can retry. */
        VNCS_Destroy(LVNC_pServer);
        LVNC_pServer = NULL;
        return(-1);
    }

    /* Listening; flushes now land in the server. */
    return(0);
}

/*--------------------------------------------------------------------------*/
/* LVNC_IsStarted:                                                          */
/*                                                                          */
/* Report whether the server exists: LVNC_Start leaves LVNC_pServer set     */
/* only when the server is listening (a failed start clears it again), and  */
/* LVNC_Stop clears it.                                                     */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     int : non-zero while the VNC display is running, zero otherwise.     */
/*--------------------------------------------------------------------------*/

int LVNC_IsStarted(void)
{
    /* Running exactly when there is a server. */
    return(LVNC_pServer != NULL);
}

/*--------------------------------------------------------------------------*/
/* LVNC_Flush:                                                              */
/*                                                                          */
/* Submit the rendered area (its rows are exactly the area's width, so the   */
/* source stride is width times the pixel size), commit when LVGL reports    */
/* this as the last area of the refresh, and release LVGL's buffer.  With no */
/* server the pixels are dropped but LVGL is still released, so a failed     */
/* start cannot hang the UI loop waiting on flush_ready.                    */
/*                                                                          */
/* Arguments:                                                               */
/*     pDrv    : the display driver.                                        */
/*     pArea   : the rendered area, inclusive coordinates.                  */
/*     pPixels : its pixels.                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void LVNC_Flush(lv_disp_drv_t *pDrv, const lv_area_t *pArea, lv_color_t *pPixels)
{
    int iW;
    int iH;

    if (LVNC_pServer != NULL)
    {   /* A server to feed: hand over the area, and commit a finished refresh. */
        iW = pArea->x2 - pArea->x1 + 1;
        iH = pArea->y2 - pArea->y1 + 1;
        VNCS_SubmitRect(LVNC_pServer, pArea->x1, pArea->y1, iW, iH, pPixels,
                        iW * (int)sizeof(lv_color_t));

        if (lv_disp_flush_is_last(pDrv))
        {   /* Last area of this refresh: publish the whole frame at once. */
            VNCS_CommitFrame(LVNC_pServer);
        }
    }

    /* The copy is done (or skipped): LVGL may reuse its draw buffer. */
    lv_disp_flush_ready(pDrv);
}

/*--------------------------------------------------------------------------*/
/* LVNC_IndevRead:                                                          */
/*                                                                          */
/* Take at most one queued pointer event, remember it as the current state,  */
/* and report that state; set continue_reading while more are queued so     */
/* LVGL reads again in the same poll.  Button bit 0 (the left button, or a   */
/* touch) is what LVGL's pointer understands as pressed.                     */
/*                                                                          */
/* Arguments:                                                               */
/*     pDrv  : the input driver (not needed: there is one queue).           */
/*     pData : out; point, state and continue_reading.                      */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void LVNC_IndevRead(lv_indev_drv_t *pDrv, lv_indev_data_t *pData)
{
    VncInputEvent ev;
    int           bHave;
    int           bMore;

    /* Under the queue's mutex: take the oldest queued event, if any, and */
    /* note whether more remain behind it.                                */
    bHave = 0;
    pthread_mutex_lock(&LVNC_queue.mtx);
    if (LVNC_queue.iCount > 0)
    {   /* An event is waiting: take it from the head. */
        ev = LVNC_queue.arrEvents[LVNC_queue.iHead];
        LVNC_queue.iHead = (LVNC_queue.iHead + 1) % LVNC_QUEUE_LEN;
        LVNC_queue.iCount--;
        bHave = 1;
    }
    bMore = (LVNC_queue.iCount > 0);
    pthread_mutex_unlock(&LVNC_queue.mtx);

    if (bHave)
    {   /* A new event becomes the current pointer state. */
        LVNC_iLastX = (lv_coord_t)ev.iX;
        LVNC_iLastY = (lv_coord_t)ev.iY;
        LVNC_bLastPressed = ((ev.iButtons & 1) != 0);
    }

    /* Report the current state; ask to be read again while events remain. */
    pData->point.x = LVNC_iLastX;
    pData->point.y = LVNC_iLastY;
    if (LVNC_bLastPressed)
    {   /* Button or touch down. */
        pData->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {   /* Button or touch up. */
        pData->state = LV_INDEV_STATE_RELEASED;
    }
    pData->continue_reading = bMore;
}

/*--------------------------------------------------------------------------*/
/* LVNC_SetBlank:                                                           */
/*                                                                          */
/* Pass the host's blank / unblank to the server.                           */
/*                                                                          */
/* Arguments:                                                               */
/*     bBlank : non-zero to blank, zero to restore.                         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void LVNC_SetBlank(int bBlank)
{
    if (LVNC_pServer != NULL)
    {   /* Only a running server has clients to blank. */
        VNCS_SetBlank(LVNC_pServer, bBlank);
    }
}

/*--------------------------------------------------------------------------*/
/* LVNC_Stop:                                                               */
/*                                                                          */
/* Destroy the server (which stops it first) and forget any queued input.   */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void LVNC_Stop(void)
{
    if (LVNC_pServer != NULL)
    {   /* Running: stop and free it. */
        VNCS_Destroy(LVNC_pServer);
        LVNC_pServer = NULL;
    }

    /* No client remains to have sent anything still queued. */
    pthread_mutex_lock(&LVNC_queue.mtx);
    LVNC_queue.iHead = 0;
    LVNC_queue.iCount = 0;
    pthread_mutex_unlock(&LVNC_queue.mtx);
}
