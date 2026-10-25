/*--------------------------------------------------------------------------*/
/* Copyright (c) 2026 B. C. Services                                         */
/* Licensed under GPL-3.0.                                                   */
/*--------------------------------------------------------------------------*/
/* vnc_server.h:                                                            */
/*                                                                          */
/* A small, self-contained RFB (VNC) SERVER that knows nothing about LVGL,   */
/* GrumpyScreen, or any GUI toolkit.  A "producer" hands it rendered pixels  */
/* and the rectangles that changed; the server keeps a single in-RAM        */
/* framebuffer, serves it to connected RFB clients over TCP (RFB 3.8,        */
/* RFC 6143), and hands client input (pointer / key) back to the producer   */
/* through one callback.  There is no X, no framebuffer device, and no       */
/* external library: it is the "direct render to VNC" path.                  */
/*                                                                          */
/* It is deliberately toolkit-agnostic, so the SAME server can be driven by  */
/* an LVGL display driver (lv_vnc.c calls VNCS_SubmitRect from LVGL's flush   */
/* callback) or by any other producer -- a test-pattern generator, another    */
/* toolkit -- feeding the same interface.  Everything toolkit-specific lives  */
/* on the OTHER side of this header.                                         */
/*                                                                          */
/* Frame model -- why there are two buffers.  A toolkit usually redraws one  */
/* screen refresh as SEVERAL rectangles, so a client served mid-refresh      */
/* would see half the old frame and half the new.  The producer therefore    */
/* writes a WORKING framebuffer (VNCS_SubmitRect, or VNCS_Framebuffer plus    */
/* VNCS_MarkDirty) and calls VNCS_CommitFrame when the refresh is complete.  */
/* The commit compares the changed area of the working buffer with the       */
/* PUBLISHED buffer in 16x16 tiles, copies only the tiles whose pixels       */
/* really differ, and queues just those to each client.  Clients are only    */
/* ever served from the published buffer, so they only ever see whole frames */
/* -- and pixels a redraw reproduced unchanged are never sent at all.       */
/*                                                                          */
/* Threading contract, because it is the part that is easy to get wrong:     */
/*   - The server owns a network thread (accept plus per-client I/O).        */
/*   - VNCS_SubmitRect, VNCS_Framebuffer, VNCS_MarkDirty, VNCS_CommitFrame    */
/*     and VNCS_SetBlank are safe to call from the producer's own thread      */
/*     WHILE the network thread runs; the server serialises buffer access.   */
/*   - The server never holds its buffer lock while it writes to a socket:   */
/*     it copies what a client needs under the lock and sends afterwards, so */
/*     a slow client can never stall a producer's call.                      */
/*   - Every send is made with MSG_NOSIGNAL, so a client that vanishes       */
/*     mid-send cannot raise SIGPIPE and kill the host program; the server   */
/*     never changes the host's signal handlers.                             */
/*   - The input callback fires ON THE NETWORK THREAD.  The producer must    */
/*     therefore only ENQUEUE the event and return; it must not render, must */
/*     not call any toolkit function that is not thread-safe, and must not    */
/*     call back into VNCS_* from inside the callback.  (The LVGL glue pushes */
/*     the event to a queue that LVGL's own indev read drains on its thread.) */
/*                                                                          */
/* Protocol behaviour a client can rely on: a NON-incremental update request */
/* is answered at once from the published frame (RFC 6143 section 7.5.3);   */
/* an incremental one is held until something has changed, so an idle       */
/* screen costs no traffic.  Rectangles are sent in Hextile when the         */
/* client's SetEncodings list names Hextile before Raw, and in Raw           */
/* otherwise (RFC 6143 section 7.7).  A server created with bNoHextile set   */
/* sends Raw to every client, whatever its list says.                        */
/*--------------------------------------------------------------------------*/

#ifndef VNC_SERVER_H
#define VNC_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------*/
/* Return convention (project coding style): a function that can fail        */
/* returns 0 on success and non-zero on failure, so a caller reads as        */
/* "if (VNCS_Start(s) != 0) { ... failed ... }".  Constructors return the    */
/* object pointer or NULL.                                                   */
/*--------------------------------------------------------------------------*/

#define VNCS_SUCCESS  0                          // a call did its job
#define VNCS_FAILURE  (-1)                       // a call did not

/*--------------------------------------------------------------------------*/
/* Source pixel format of the framebuffer the PRODUCER supplies.  This is    */
/* the format of the bytes in VNCS_SubmitRect / VNCS_Framebuffer; the server */
/* converts from it to whatever pixel format each RFB client negotiates with */
/* SetPixelFormat, so the producer never has to care what a client asked     */
/* for.  LVGL at LV_COLOR_DEPTH 32 hands over lv_color_t, whose bytes are     */
/* blue, green, red, alpha in ascending address order -- that is BGRA8888.   */
/*--------------------------------------------------------------------------*/

typedef enum
{
    VNC_PIXFMT_BGRA8888 = 0,                     // 32 bpp, bytes B,G,R,A -- LVGL depth 32
    VNC_PIXFMT_RGB565   = 1                      // 16 bpp, little-endian -- LVGL depth 16
} VncPixFmt;

/*--------------------------------------------------------------------------*/
/* How a client proves it may connect.  VNC_AUTH_NONE serves with no         */
/* challenge.  VNC_AUTH_VNC is the classic RFB password challenge-response   */
/* (RFC 6143 section 7.2.2), using the password's first 8 bytes as the       */
/* scheme requires; VNCS_Create takes it when pcPassword is non-empty and    */
/* the server then offers only that type.                                     */
/*--------------------------------------------------------------------------*/

typedef enum
{
    VNC_AUTH_NONE = 0,                           // security type 1: no authentication
    VNC_AUTH_VNC  = 1                            // security type 2: VNC DES challenge-response
} VncAuth;

/*--------------------------------------------------------------------------*/
/* Which kind of input a client sent, carried in VncInputEvent.eType.        */
/*--------------------------------------------------------------------------*/

typedef enum
{
    VNC_INPUT_POINTER = 0,                       // a PointerEvent (move and/or button)
    VNC_INPUT_KEY     = 1                        // a KeyEvent (key down or up)
} VncInputType;

/*--------------------------------------------------------------------------*/
/* One decoded client input event.  The server fills this and passes it to   */
/* the producer's callback; the producer copies out what it needs and        */
/* returns at once (see the threading contract in the file header).  Which    */
/* fields are meaningful depends on eType.                                    */
/*--------------------------------------------------------------------------*/

typedef struct
{
    VncInputType eType;                          // POINTER or KEY -- selects the fields below
    int          iX;                             // POINTER: x in framebuffer pixels (0..width-1)
    int          iY;                             // POINTER: y in framebuffer pixels (0..height-1)
    int          iButtons;                       // POINTER: RFB button mask, bit 0 = left button
    unsigned int uKeySym;                        // KEY: X11 keysym of the key
    int          iDown;                          // KEY: 1 = pressed, 0 = released
} VncInputEvent;

/*--------------------------------------------------------------------------*/
/* The producer's input sink.  The server calls this once per decoded client */
/* event, ON THE NETWORK THREAD.  pUser is the pInputUser handed to           */
/* VNCS_Create, so the callback can find the producer's own state without a   */
/* global.  pEv is valid only for the duration of the call.                   */
/*--------------------------------------------------------------------------*/

typedef void (*VncInputCb)(void *pUser, const VncInputEvent *pEv);

/*--------------------------------------------------------------------------*/
/* The host's log sink.  The server reports errors only -- never success --  */
/* through this, so its messages land in the host program's own log; with   */
/* no sink configured they go to stderr instead.  pcMsg is one complete      */
/* line with no trailing newline, valid only for the duration of the call.   */
/* It may be called from the producer's thread or the network thread, so a  */
/* sink that writes somewhere shared must do its own locking.                */
/*--------------------------------------------------------------------------*/

typedef void (*VncLogCb)(void *pUser, const char *pcMsg);

/*--------------------------------------------------------------------------*/
/* Everything the caller chooses at creation.  VNCS_Create copies all of it, */
/* the strings included, so the caller may let the struct and its strings go */
/* out of scope afterwards.  Only the two user pointers, pInputUser and      */
/* pLogUser, are kept exactly as given, because the server hands them back   */
/* to the callbacks -- whatever they point at must stay valid until          */
/* VNCS_Destroy returns.                                                     */
/*--------------------------------------------------------------------------*/

typedef struct
{
    int         iWidth;                          // framebuffer width in pixels (panel width, e.g. 532)
    int         iHeight;                         // framebuffer height in pixels (panel height, e.g. 300)
    VncPixFmt   ePixFmt;                         // format of the pixels the producer will supply
    int         iPort;                           // TCP port to listen on (5900 is RFB display :0)
    const char *pcListenAddr;                    // bind address, or NULL for all interfaces
    VncAuth     eAuth;                            // NONE, or VNC DES challenge-response using pcPassword
    const char *pcPassword;                       // VNC_AUTH_VNC password, first 8 bytes used; required if VNC, ignored if NONE
    VncInputCb  pfnInput;                         // input sink, or NULL to ignore all client input
    void       *pInputUser;                       // opaque value passed to pfnInput
    int         iMaxClients;                       // simultaneous clients allowed (0 selects a default)
    int         bNoHextile;                        // 1 = Raw to every client, never Hextile; 0 (the default) = a client that lists Hextile before Raw gets Hextile
    VncLogCb    pfnLog;                            // error log sink, or NULL for stderr
    void       *pLogUser;                          // opaque value passed to pfnLog
} VncServerCfg;

/*--------------------------------------------------------------------------*/
/* Opaque server handle.  The layout lives in vnc_server.c; callers only     */
/* ever hold the pointer.                                                    */
/*--------------------------------------------------------------------------*/

typedef struct VncServer VncServer;

/*--------------------------------------------------------------------------*/
/* VNCS_Create:                                                             */
/*                                                                          */
/* Allocate a server and its two in-RAM framebuffers (working and published, */
/* see the frame model above) from the configuration, but do NOT open the    */
/* listening socket yet (that is VNCS_Start) -- so a producer can finish its  */
/* own setup, install its input queue, and only then begin accepting         */
/* clients.  Both framebuffers start cleared to opaque black.                */
/*                                                                          */
/* Arguments:                                                               */
/*     pCfg : the configuration above; copied, so it need not outlive the    */
/*            call.  iWidth and iHeight must be positive; ePixFmt must be a   */
/*            supported format; eAuth is VNC_AUTH_NONE, or VNC_AUTH_VNC with  */
/*            a non-empty pcPassword (see VncAuth).                           */
/*                                                                          */
/* Returns:                                                                 */
/*     VncServer * : the new server, or NULL if the arguments are invalid or */
/*                   allocation failed.  A NULL return has already reported  */
/*                   the reason through pfnLog (or stderr) and leaves nothing */
/*                   allocated.                                               */
/*--------------------------------------------------------------------------*/

VncServer *VNCS_Create(const VncServerCfg *pCfg);

/*--------------------------------------------------------------------------*/
/* VNCS_Start:                                                              */
/*                                                                          */
/* Open the listening socket and spawn the network thread.  After this       */
/* returns success the server is live: clients may connect and the input     */
/* callback may begin firing on the network thread.  Call it once: a second  */
/* call fails and changes nothing.                                           */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : a server from VNCS_Create.                                       */
/*                                                                          */
/* Returns:                                                                 */
/*     int : VNCS_SUCCESS once listening, or VNCS_FAILURE if the socket      */
/*           could not be bound or the thread could not be started (the      */
/*           reason is logged; the server is left safe to VNCS_Destroy).     */
/*--------------------------------------------------------------------------*/

int VNCS_Start(VncServer *pS);

/*--------------------------------------------------------------------------*/
/* VNCS_SubmitRect:                                                         */
/*                                                                          */
/* The COUPLING CALL.  Copy one rectangle of freshly rendered pixels into    */
/* the WORKING framebuffer and add it to the pending region; clients see it  */
/* after the next VNCS_CommitFrame.  This is what the LVGL flush callback     */
/* calls, once per invalidated area, and it is why the producer never        */
/* touches the server's buffers directly in this model.  Pixels are in the   */
/* format named by the config's ePixFmt.  Safe to call from the producer     */
/* thread while clients are connected.                                       */
/*                                                                          */
/* Arguments:                                                               */
/*     pS              : the server.                                        */
/*     iX, iY          : top-left of the rectangle, in framebuffer pixels.   */
/*     iW, iH          : rectangle width and height in pixels; a rectangle   */
/*                       that runs off an edge is clipped, not rejected.      */
/*     pvPixels        : source pixels, row-major, ePixFmt format.           */
/*     iSrcStrideBytes : bytes from one source row to the next (lets the     */
/*                       caller pass a sub-rectangle of a larger buffer).     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.  An out-of-range or empty rectangle is a no-op; there */
/*            is no failure a caller could act on here.                      */
/*--------------------------------------------------------------------------*/

void VNCS_SubmitRect(VncServer *pS, int iX, int iY, int iW, int iH,
                     const void *pvPixels, int iSrcStrideBytes);

/*--------------------------------------------------------------------------*/
/* VNCS_Framebuffer:                                                        */
/*                                                                          */
/* The IN-PLACE alternative to VNCS_SubmitRect, for a producer that would    */
/* rather render straight into the server's buffer than copy into it: hand   */
/* back a pointer to the WORKING framebuffer and its row stride.  A producer */
/* that uses this must report every write with VNCS_MarkDirty, finish each   */
/* frame with VNCS_CommitFrame, and must NOT also use VNCS_SubmitRect --     */
/* pick one model and keep to it.  The pointer is stable for the life of    */
/* the server.                                                               */
/*                                                                          */
/* Arguments:                                                               */
/*     pS            : the server.                                          */
/*     piStrideBytes : out; receives the bytes per framebuffer row.  May be  */
/*                     NULL if the caller does not need it.                  */
/*                                                                          */
/* Returns:                                                                 */
/*     unsigned char * : the working framebuffer base (ePixFmt pixels), or   */
/*                       NULL if pS is NULL.                                 */
/*--------------------------------------------------------------------------*/

unsigned char *VNCS_Framebuffer(VncServer *pS, int *piStrideBytes);

/*--------------------------------------------------------------------------*/
/* VNCS_MarkDirty:                                                          */
/*                                                                          */
/* Mark a rectangle of the working framebuffer changed WITHOUT copying       */
/* pixels -- the companion to VNCS_Framebuffer for the in-place model.  Adds */
/* the rectangle to the pending region; clients see it after the next        */
/* VNCS_CommitFrame.  A rectangle that runs off an edge is clipped.          */
/*                                                                          */
/* Arguments:                                                               */
/*     pS         : the server.                                            */
/*     iX, iY     : top-left of the changed rectangle, in framebuffer pixels. */
/*     iW, iH     : its width and height in pixels.                          */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; an empty or out-of-range rectangle is a no-op.        */
/*--------------------------------------------------------------------------*/

void VNCS_MarkDirty(VncServer *pS, int iX, int iY, int iW, int iH);

/*--------------------------------------------------------------------------*/
/* VNCS_CommitFrame:                                                        */
/*                                                                          */
/* The producer's "this refresh is complete" signal.  Publish everything     */
/* submitted since the last commit: for each 16x16 tile the pending region   */
/* touches, compare the working and published buffers, copy only the tiles   */
/* that really differ, add those tiles to every client's dirty region, and   */
/* wake the network thread so any waiting incremental request is answered.   */
/* A tile whose pixels a redraw reproduced exactly is never sent.  An LVGL   */
/* producer calls this when lv_disp_flush_is_last() says the area it just     */
/* flushed was the last of the refresh.                                      */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.  The pending region is empty afterwards; committing   */
/*            with nothing pending is a cheap no-op.                         */
/*--------------------------------------------------------------------------*/

void VNCS_CommitFrame(VncServer *pS);

/*--------------------------------------------------------------------------*/
/* VNCS_SetBlank:                                                           */
/*                                                                          */
/* Blank or restore what clients see, leaving both framebuffers untouched -- */
/* the VNC equivalent of blanking a panel, for a host's screen-sleep.  While */
/* blanked, every update is served as solid black, so a client that turns    */
/* its own backlight off after a spell of black goes dark with the host.     */
/* Restoring marks the whole screen dirty, so every client gets the current  */
/* frame back in full.  Asking for the state already in force is a no-op.    */
/*                                                                          */
/* Arguments:                                                               */
/*     pS     : the server.                                                 */
/*     bBlank : non-zero to blank, zero to restore.                         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void VNCS_SetBlank(VncServer *pS, int bBlank);

/*--------------------------------------------------------------------------*/
/* VNCS_Stop:                                                               */
/*                                                                          */
/* Stop accepting clients, close every connection and the listening socket,  */
/* and join the network thread.  After it returns the input callback will    */
/* not fire again, so a producer can safely tear down whatever the callback  */
/* touched.  Safe to call on a server that was created but never started,    */
/* and safe to call more than once.                                         */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; teardown does not fail in a way a caller could act    */
/*            on, and any error is logged.                                  */
/*--------------------------------------------------------------------------*/

void VNCS_Stop(VncServer *pS);

/*--------------------------------------------------------------------------*/
/* VNCS_Destroy:                                                            */
/*                                                                          */
/* Stop the server if it is still running (so a caller may skip VNCS_Stop)   */
/* and free it and its framebuffer.  The handle is invalid afterwards.       */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server, or NULL (a NULL is ignored, so teardown paths need   */
/*          no guard).                                                       */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void VNCS_Destroy(VncServer *pS);

#ifdef __cplusplus
}
#endif

#endif /* VNC_SERVER_H */
