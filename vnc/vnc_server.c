/*--------------------------------------------------------------------------*/
/* Copyright (c) 2026 B. C. Services                                         */
/* Licensed under GPL-3.0.                                                   */
/*--------------------------------------------------------------------------*/
/* vnc_server.c:                                                            */
/*                                                                          */
/* The implementation behind vnc_server.h: a small RFB 3.8 server (RFC 6143) */
/* with one network thread.  That thread is a single control loop: it       */
/* poll()s the listening socket, every client socket and a wake pipe, and   */
/* everything network-side -- accepting, the handshake, parsing client       */
/* messages, building and sending updates -- happens on it, in order, so     */
/* there is no second network thread to race with.  The producer's calls     */
/* (SubmitRect, MarkDirty, CommitFrame, SetBlank) only touch shared state    */
/* under one mutex and then write a byte to the wake pipe.                   */
/*                                                                          */
/* Dirty tracking is by 16x16 TILE.  A producer's writes mark tiles in the   */
/* PENDING map; a commit compares each pending tile of the working buffer    */
/* with the published buffer and, only where the pixels differ, copies the   */
/* tile across and sets it in every client's DIRTY map.  An update for a    */
/* client is its dirty tiles, merged into one rectangle per horizontal run,  */
/* converted to the client's pixel format and sent in the encoding that     */
/* client chose: Hextile if its SetEncodings list names Hextile before Raw,  */
/* otherwise Raw (RFC 6143 section 7.7).  A server configured with           */
/* bNoHextile never chooses Hextile, so every client gets Raw.               */
/*                                                                          */
/* Security: this build implements type None and type 2, VNC password        */
/* authentication (RFC 6143 section 7.2.2).  With a password configured the   */
/* server offers type 2, sends a random 16-byte DES challenge, and checks     */
/* the client's response against the challenge encrypted with the password    */
/* before the client reaches ClientInit; with no password it offers type      */
/* None exactly as before.  The DES engine is des.c (the VDES_ functions),    */
/* used only for this challenge-response.                                     */
/*--------------------------------------------------------------------------*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE                              // MSG_NOSIGNAL, pipe2, accept4, SOCK_CLOEXEC
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "vnc_server.h"
#include "des.h"

/*--------------------------------------------------------------------------*/
/* Tuning and fixed values.                                                  */
/*--------------------------------------------------------------------------*/

#define VNCS_TILE                16              // tile edge in pixels: dirty tracking and diff granularity
#define VNCS_DEFAULT_PORT        5900            // RFB display :0, used when the config gives no port
#define VNCS_DEFAULT_MAX_CLIENTS 2               // a panel-side client plus one desktop viewer
#define VNCS_SEND_TIMEOUT_MS     5000            // a client that accepts no data this long is dropped
#define VNCS_RX_SIZE             64              // receive assembly buffer; > 20, the largest fixed message
#define VNCS_LOG_LINE            256             // longest log line this module formats
#define VNCS_LISTEN_BACKLOG      4               // pending connections the kernel may queue
#define VNCS_DESKTOP_NAME        "VNC server"    // desktop name sent in ServerInit (a viewer's title)
#define VNCS_HEXTILE_MAX_COLOURS 16              // a Hextile tile with more colours than this is sent raw

/*--------------------------------------------------------------------------*/
/* RFB wire values (RFC 6143).  Multi-byte fields on the wire are big-endian */
/* (network order); the section numbers are the RFC's.                       */
/*--------------------------------------------------------------------------*/

#define RFB_VERSION_STRING       "RFB 003.008\n" // ProtocolVersion this server offers (7.1.1)
#define RFB_VERSION_LEN          12              // every ProtocolVersion message is 12 bytes
#define RFB_SEC_NONE             1               // security type None (7.2.1)
#define RFB_SEC_VNC              2               // security type VNC Authentication (7.2.2); offered when a password is set
#define RFB_SEC_RESULT_OK        0               // SecurityResult: accepted (7.1.3)
#define RFB_SEC_RESULT_FAILED    1               // SecurityResult: rejected (7.1.3); a reason string follows on 3.8
#define RFB_VNC_CHALLENGE_LEN    16              // VNC Authentication challenge and response length (7.2.2)
#define RFB_CMSG_SETPIXELFORMAT  0               // client: SetPixelFormat, 20 bytes (7.5.1)
#define RFB_CMSG_SETENCODINGS    2               // client: SetEncodings, 4 + 4n bytes (7.5.2)
#define RFB_CMSG_FBUPDATEREQ     3               // client: FramebufferUpdateRequest, 10 bytes (7.5.3)
#define RFB_CMSG_KEYEVENT        4               // client: KeyEvent, 8 bytes (7.5.4)
#define RFB_CMSG_POINTEREVENT    5               // client: PointerEvent, 6 bytes (7.5.5)
#define RFB_CMSG_CUTTEXT         6               // client: ClientCutText, 8 + n bytes (7.5.6)
#define RFB_LEN_SETPIXELFORMAT   20              // type + 3 padding + 16-byte PIXEL_FORMAT
#define RFB_LEN_SETENCODINGS     4               // type + padding + u16 count (the list follows)
#define RFB_LEN_FBUPDATEREQ      10              // type + incremental + x, y, w, h
#define RFB_LEN_KEYEVENT         8               // type + down + 2 padding + u32 keysym
#define RFB_LEN_POINTEREVENT     6               // type + button mask + x, y
#define RFB_LEN_CUTTEXT          8               // type + 3 padding + u32 length (the text follows)
#define RFB_SMSG_FBUPDATE        0               // server: FramebufferUpdate (7.6.1)
#define RFB_ENC_RAW              0               // encoding Raw (7.7.1)
#define RFB_ENC_HEXTILE          5               // encoding Hextile (7.7.4)
#define RFB_ENC_LEN              4               // one SetEncodings list entry: an s32 encoding number
#define RFB_HEXTILE_RAW          1               // Hextile tile mask: the tile is raw pixels; other bits ignored
#define RFB_HEXTILE_BG_SPECIFIED 2               // Hextile tile mask: a background pixel follows the mask
#define RFB_HEXTILE_FG_SPECIFIED 4               // Hextile tile mask: a foreground pixel follows (one-colour subrects)
#define RFB_HEXTILE_ANY_SUBRECTS 8               // Hextile tile mask: a u8 subrect count and the subrects follow
#define RFB_HEXTILE_COLOURED     16              // Hextile tile mask: each subrect carries its own pixel value
#define RFB_HEXTILE_MAX_SUBRECTS 255             // the subrect count is a u8
#define RFB_PIXFMT_LEN           16              // PIXEL_FORMAT on the wire (7.4)
#define RFB_UPDATE_HDR_LEN       4               // type + padding + u16 rectangle count
#define RFB_RECT_HDR_LEN         12              // x, y, w, h (u16 each) + s32 encoding
#define RFB_MAX_RECTS            65535           // the rectangle count is a u16

/*--------------------------------------------------------------------------*/
/* Where one client is in the connection sequence (RFC 6143 section 7.1).    */
/* With a password set, VNCC_AUTH sits between VNCC_SECURITY and VNCC_INIT:   */
/* the client's 16-byte challenge response is awaited there, checked against  */
/* the challenge encrypted with the password, and answered with a            */
/* SecurityResult, after which the client moves to VNCC_INIT on success or    */
/* is dropped on failure.                                                     */
/*--------------------------------------------------------------------------*/

typedef enum
{
    VNCC_FREE = 0,                               // slot unused
    VNCC_VERSION,                                // waiting for the client's 12-byte ProtocolVersion
    VNCC_SECURITY,                               // 3.7 / 3.8: waiting for the chosen security type
    VNCC_AUTH,                                   // VNC auth: waiting for the 16-byte challenge response
    VNCC_INIT,                                   // waiting for the 1-byte ClientInit
    VNCC_NORMAL                                  // handshake complete; client messages flow
} VncClientState;

/*--------------------------------------------------------------------------*/
/* A true-colour RFB pixel format (RFC 6143 section 7.4), unpacked.          */
/*--------------------------------------------------------------------------*/

typedef struct
{
    int iBpp;                                    // bits per pixel on the wire: 8, 16 or 32
    int iDepth;                                  // useful bits; informational only
    int bBigEndian;                              // multi-byte pixels sent most significant byte first
    int bTrueColour;                             // 1 = true colour, the only mode this server serves
    int iRedMax;                                 // largest red value (e.g. 255, 31)
    int iGreenMax;                               // largest green value (e.g. 255, 63)
    int iBlueMax;                                // largest blue value (e.g. 255, 31)
    int iRedShift;                               // bit position of red within the pixel value
    int iGreenShift;                             // bit position of green within the pixel value
    int iBlueShift;                              // bit position of blue within the pixel value
} VncPixelFormat;

/*--------------------------------------------------------------------------*/
/* One rectangle of an update being built, in framebuffer pixels.            */
/*--------------------------------------------------------------------------*/

typedef struct
{
    int iX;                                      // left edge
    int iY;                                      // top edge
    int iW;                                      // width
    int iH;                                      // height
} VncRect;

/*--------------------------------------------------------------------------*/
/* What a Hextile decoder remembers from one tile to the next within a       */
/* rectangle (RFC 6143 section 7.7.4): the background and foreground pixel  */
/* values, which a tile may leave out to reuse the previous tile's.  The     */
/* encoder tracks the same two values so it only sends one when it changes. */
/* Pixel values are held as keys (see VNCS_HextileKey), in the client's      */
/* format.                                                                  */
/*--------------------------------------------------------------------------*/

typedef struct
{
    int           bBgValid;                      // ulBg is what the decoder holds as background
    unsigned long ulBg;                          // the background pixel key last sent
    int           bFgValid;                      // ulFg is what the decoder holds as foreground
    unsigned long ulFg;                          // the foreground pixel key last sent
} VncHextileState;

/*--------------------------------------------------------------------------*/
/* One client slot.  Only the network thread touches a client, EXCEPT its    */
/* pDirty map and eState, which a producer's commit also reads and writes -- */
/* those two are guarded by the server mutex.  With VNC authentication in     */
/* use, arrChallenge holds the 16-byte challenge sent to this client while    */
/* VNCC_AUTH awaits and checks its response.                                  */
/*--------------------------------------------------------------------------*/

typedef struct
{
    VncClientState eState;                       // connection stage, or VNCC_FREE (mutex-guarded)
    int            iFd;                          // connected socket; -1 when the slot is free
    int            iMinor;                       // negotiated RFB minor version: 3, 7 or 8
    unsigned char  arrChallenge[RFB_VNC_CHALLENGE_LEN]; // VNC auth: the challenge sent, awaiting its response
    unsigned char  arrRx[VNCS_RX_SIZE];          // bytes received but not yet parsed
    int            iRxLen;                       // how many bytes of arrRx are filled
    unsigned long  ulSkip;                       // variable-length payload bytes still to discard
    unsigned int   uEncLeft;                     // SetEncodings entries still to read from the stream
    int            bEncChosen;                   // the current SetEncodings list has named a usable encoding
    int            iEncoding;                    // what this client's updates use: RFB_ENC_RAW or RFB_ENC_HEXTILE
    VncPixelFormat fmt;                          // the pixel format this client asked for
    unsigned char *pDirty;                       // one byte per tile: 1 = changed since sent (mutex-guarded)
    VncRect       *pRects;                       // scratch list of rectangles for one update
    int            bRequest;                     // an update request is outstanding
    int            bIncremental;                 // ...and it is incremental
    VncRect        rcReq;                        // the requested region, clipped to the framebuffer
    unsigned char *pSend;                        // the update message being built for this client
    size_t         ulSendCap;                    // allocated size of pSend
} VncClient;

/*--------------------------------------------------------------------------*/
/* The server.  See the file header for which thread owns what.              */
/* eAuth is the security type served; when it is VNC_AUTH_VNC, arrPassword    */
/* holds the first 8 bytes of the password (the scheme uses no more).         */
/* VNCS_Create fills them and VNCS_Destroy wipes the password before freeing. */
/*--------------------------------------------------------------------------*/

struct VncServer
{
    int             iWidth;                      // framebuffer width in pixels
    int             iHeight;                     // framebuffer height in pixels
    VncPixFmt       ePixFmt;                     // the producer's pixel format
    VncAuth         eAuth;                        // security type served: NONE or VNC challenge-response
    unsigned char   arrPassword[8];              // VNC auth: first 8 password bytes, NUL-padded; wiped at destroy
    int             iBytesPP;                    // bytes per producer pixel: 4 or 2
    int             iStride;                     // bytes per framebuffer row
    int             iPort;                       // TCP port to listen on
    char           *pcListenAddr;                // copied bind address, or NULL for every interface
    VncInputCb      pfnInput;                    // client input sink, or NULL
    void           *pInputUser;                  // passed back to pfnInput
    VncLogCb        pfnLog;                      // error sink, or NULL for stderr
    void           *pLogUser;                    // passed back to pfnLog
    int             iMaxClients;                 // number of client slots
    int             bNoHextile;                  // 1 = Hextile is never chosen, every client gets Raw
    unsigned char  *pWork;                       // WORKING framebuffer: the producer writes here
    unsigned char  *pPub;                        // PUBLISHED framebuffer: clients are served from here
    int             iTilesX;                     // tiles across (width rounded up to VNCS_TILE)
    int             iTilesY;                     // tiles down (height rounded up to VNCS_TILE)
    unsigned char  *pPending;                    // one byte per tile: written since the last commit
    int             bBlank;                      // clients are served black while set
    VncPixelFormat  fmtNative;                   // the producer's format expressed as an RFB format
    pthread_mutex_t mtxLock;                     // guards pWork, pPub, pPending, bBlank, client pDirty/eState
    VncClient      *arrClients;                  // iMaxClients slots
    int             iListenFd;                   // listening socket; -1 when not started
    int             arrWake[2];                  // wake pipe: [1] written to wake the loop, [0] polled
    pthread_t       thrNet;                      // the network thread
    int             bStarted;                    // VNCS_Start succeeded and the thread is running
    volatile int    bStop;                       // set by VNCS_Stop to end the network loop
};

/*--------------------------------------------------------------------------*/
/* Logging.                                                                  */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* VNCS_Emit:                                                               */
/*                                                                          */
/* Hand one finished log line to the configured sink, or to stderr when     */
/* there is none.  Split from the formatting so VNCS_Create can report a    */
/* bad configuration before a server object exists to carry the sink.       */
/*                                                                          */
/* Arguments:                                                               */
/*     pfnLog : the sink from the configuration, or NULL.                    */
/*     pUser  : the value to pass back to the sink.                          */
/*     pcMsg  : the line, without a trailing newline.                        */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; logging cannot fail in a way the caller acts on.      */
/*--------------------------------------------------------------------------*/

static void VNCS_Emit(VncLogCb pfnLog, void *pUser, const char *pcMsg)
{
    if (pfnLog != NULL)
    {   /* The host wants the line in its own log. */
        pfnLog(pUser, pcMsg);
    }
    else
    {   /* No sink configured: stderr, with a prefix saying who is talking. */
        fprintf(stderr, "vnc_server: %s\n", pcMsg);
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_Log:                                                                */
/*                                                                          */
/* printf-style error report through the server's sink.  Errors only: the    */
/* module never logs success, so the host's log carries only what needs a    */
/* person's attention.                                                       */
/*                                                                          */
/* Arguments:                                                               */
/*     pS      : the server (its sink is used).                             */
/*     pcFmt   : printf format string.                                       */
/*     ...     : the values the format consumes.                             */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_Log(const VncServer *pS, const char *pcFmt, ...)
{
    char    arrLine[VNCS_LOG_LINE];
    va_list vaArgs;

    /* Format into a fixed line; an over-long message is truncated, not lost. */
    va_start(vaArgs, pcFmt);
    vsnprintf(arrLine, sizeof(arrLine), pcFmt, vaArgs);
    va_end(vaArgs);

    VNCS_Emit(pS->pfnLog, pS->pLogUser, arrLine);
}

/*--------------------------------------------------------------------------*/
/* Big-endian (network order) field helpers.                                 */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* VNCS_Put16:                                                              */
/*                                                                          */
/* Store a 16-bit value most significant byte first, as every RFB           */
/* multi-byte field is sent.                                                 */
/*                                                                          */
/* Arguments:                                                               */
/*     pDst  : where the two bytes go.                                      */
/*     uVal  : the value; only its low 16 bits are stored.                   */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_Put16(unsigned char *pDst, unsigned int uVal)
{
    pDst[0] = (unsigned char)((uVal >> 8) & 0xFF);
    pDst[1] = (unsigned char)(uVal & 0xFF);
}

/*--------------------------------------------------------------------------*/
/* VNCS_Put32:                                                              */
/*                                                                          */
/* Store a 32-bit value most significant byte first.                         */
/*                                                                          */
/* Arguments:                                                               */
/*     pDst  : where the four bytes go.                                     */
/*     ulVal : the value; only its low 32 bits are stored.                   */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_Put32(unsigned char *pDst, unsigned long ulVal)
{
    pDst[0] = (unsigned char)((ulVal >> 24) & 0xFF);
    pDst[1] = (unsigned char)((ulVal >> 16) & 0xFF);
    pDst[2] = (unsigned char)((ulVal >> 8) & 0xFF);
    pDst[3] = (unsigned char)(ulVal & 0xFF);
}

/*--------------------------------------------------------------------------*/
/* VNCS_Get16:                                                              */
/*                                                                          */
/* Read a big-endian 16-bit field.                                          */
/*                                                                          */
/* Arguments:                                                               */
/*     pSrc : the two bytes.                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     unsigned int : the value, 0 to 65535.                                 */
/*--------------------------------------------------------------------------*/

static unsigned int VNCS_Get16(const unsigned char *pSrc)
{
    /* Most significant byte first. */
    return(((unsigned int)pSrc[0] << 8) | (unsigned int)pSrc[1]);
}

/*--------------------------------------------------------------------------*/
/* VNCS_Get32:                                                              */
/*                                                                          */
/* Read a big-endian 32-bit field.                                          */
/*                                                                          */
/* Arguments:                                                               */
/*     pSrc : the four bytes.                                               */
/*                                                                          */
/* Returns:                                                                 */
/*     unsigned long : the value.                                           */
/*--------------------------------------------------------------------------*/

static unsigned long VNCS_Get32(const unsigned char *pSrc)
{
    /* Most significant byte first. */
    return(((unsigned long)pSrc[0] << 24) | ((unsigned long)pSrc[1] << 16) |
           ((unsigned long)pSrc[2] << 8) | (unsigned long)pSrc[3]);
}

/*--------------------------------------------------------------------------*/
/* Pixel formats.                                                            */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* VNCS_NativeFormat:                                                       */
/*                                                                          */
/* Describe the producer's pixel layout as an RFB pixel format.  This is     */
/* what ServerInit advertises, and a client that asks for exactly it gets    */
/* a straight memory copy instead of a per-pixel conversion.                 */
/*                                                                          */
/*   BGRA8888: 32 bpp little-endian, bytes B,G,R,A -> as a pixel value red   */
/*             is bits 16-23, green 8-15, blue 0-7; alpha (24-31) unused.    */
/*   RGB565:   16 bpp little-endian, red bits 11-15, green 5-10, blue 0-4.   */
/*                                                                          */
/* Arguments:                                                               */
/*     ePix : the producer's format.                                         */
/*     pF   : out; filled with the matching RFB pixel format.                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_NativeFormat(VncPixFmt ePix, VncPixelFormat *pF)
{
    memset(pF, 0, sizeof(*pF));
    pF->bBigEndian = 0;
    pF->bTrueColour = 1;

    if (ePix == VNC_PIXFMT_RGB565)
    {   /* 5-6-5 packed into 16 bits. */
        pF->iBpp = 16;
        pF->iDepth = 16;
        pF->iRedMax = 31;
        pF->iGreenMax = 63;
        pF->iBlueMax = 31;
        pF->iRedShift = 11;
        pF->iGreenShift = 5;
        pF->iBlueShift = 0;
    }
    else
    {   /* BGRA8888: 8 bits per component in a 32-bit little-endian word. */
        pF->iBpp = 32;
        pF->iDepth = 24;
        pF->iRedMax = 255;
        pF->iGreenMax = 255;
        pF->iBlueMax = 255;
        pF->iRedShift = 16;
        pF->iGreenShift = 8;
        pF->iBlueShift = 0;
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_WriteFormat:                                                        */
/*                                                                          */
/* Serialise a pixel format into the 16-byte PIXEL_FORMAT wire structure     */
/* (RFC 6143 section 7.4): bpp, depth, big-endian flag, true-colour flag,    */
/* three u16 maxima, three u8 shifts, three bytes of padding.                */
/*                                                                          */
/* Arguments:                                                               */
/*     pF   : the format.                                                   */
/*     pDst : out; 16 bytes.                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_WriteFormat(const VncPixelFormat *pF, unsigned char *pDst)
{
    memset(pDst, 0, RFB_PIXFMT_LEN);
    pDst[0] = (unsigned char)pF->iBpp;
    pDst[1] = (unsigned char)pF->iDepth;
    pDst[2] = (unsigned char)pF->bBigEndian;
    pDst[3] = (unsigned char)pF->bTrueColour;
    VNCS_Put16(&pDst[4], (unsigned int)pF->iRedMax);
    VNCS_Put16(&pDst[6], (unsigned int)pF->iGreenMax);
    VNCS_Put16(&pDst[8], (unsigned int)pF->iBlueMax);
    pDst[10] = (unsigned char)pF->iRedShift;
    pDst[11] = (unsigned char)pF->iGreenShift;
    pDst[12] = (unsigned char)pF->iBlueShift;
}

/*--------------------------------------------------------------------------*/
/* VNCS_ReadFormat:                                                         */
/*                                                                          */
/* Parse and validate a client's PIXEL_FORMAT.  Only true colour at 8, 16    */
/* or 32 bits per pixel is served; a colour-map format would need a palette  */
/* this server does not keep, and a shift that puts a component outside the  */
/* pixel would make the conversion write garbage, so both are refused.       */
/*                                                                          */
/* Arguments:                                                               */
/*     pSrc : the 16 wire bytes.                                            */
/*     pF   : out; the parsed format (only valid on success).               */
/*                                                                          */
/* Returns:                                                                 */
/*     int : VNCS_SUCCESS if the format is one this server can serve,        */
/*           VNCS_FAILURE otherwise.                                         */
/*--------------------------------------------------------------------------*/

static int VNCS_ReadFormat(const unsigned char *pSrc, VncPixelFormat *pF)
{
    pF->iBpp = pSrc[0];
    pF->iDepth = pSrc[1];
    pF->bBigEndian = (pSrc[2] != 0);
    pF->bTrueColour = (pSrc[3] != 0);
    pF->iRedMax = (int)VNCS_Get16(&pSrc[4]);
    pF->iGreenMax = (int)VNCS_Get16(&pSrc[6]);
    pF->iBlueMax = (int)VNCS_Get16(&pSrc[8]);
    pF->iRedShift = pSrc[10];
    pF->iGreenShift = pSrc[11];
    pF->iBlueShift = pSrc[12];

    if ((pF->iBpp != 8) && (pF->iBpp != 16) && (pF->iBpp != 32))
    {   /* Only whole-byte pixel sizes are served. */
        return(VNCS_FAILURE);
    }

    if (!pF->bTrueColour)
    {   /* Colour-map formats need a palette this server does not keep. */
        return(VNCS_FAILURE);
    }

    if ((pF->iRedMax < 1) || (pF->iGreenMax < 1) || (pF->iBlueMax < 1))
    {   /* A zero maximum would divide nothing into nothing; not a usable format. */
        return(VNCS_FAILURE);
    }

    if ((pF->iRedShift >= pF->iBpp) || (pF->iGreenShift >= pF->iBpp) ||
        (pF->iBlueShift >= pF->iBpp))
    {   /* A component shifted outside the pixel cannot be represented. */
        return(VNCS_FAILURE);
    }

    /* A true-colour format this server can convert to. */
    return(VNCS_SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* VNCS_FormatsEqual:                                                       */
/*                                                                          */
/* Whether two formats lay pixels out identically in memory, so a copy      */
/* suffices.  Depth is ignored (it is informational); the byte order only    */
/* matters above 8 bits per pixel.                                          */
/*                                                                          */
/* Arguments:                                                               */
/*     pA, pB : the two formats.                                            */
/*                                                                          */
/* Returns:                                                                 */
/*     int : non-zero if identical in memory, zero otherwise.               */
/*--------------------------------------------------------------------------*/

static int VNCS_FormatsEqual(const VncPixelFormat *pA, const VncPixelFormat *pB)
{
    if (pA->iBpp != pB->iBpp)
    {   /* Different pixel sizes can never be copied. */
        return(0);
    }

    if ((pA->iBpp > 8) && (pA->bBigEndian != pB->bBigEndian))
    {   /* Same size, opposite byte order. */
        return(0);
    }

    /* Identical only if every component sits in the same bits at the same range. */
    return((pA->bTrueColour == pB->bTrueColour) &&
           (pA->iRedMax == pB->iRedMax) && (pA->iGreenMax == pB->iGreenMax) &&
           (pA->iBlueMax == pB->iBlueMax) && (pA->iRedShift == pB->iRedShift) &&
           (pA->iGreenShift == pB->iGreenShift) && (pA->iBlueShift == pB->iBlueShift));
}

/*--------------------------------------------------------------------------*/
/* VNCS_ConvertRow:                                                         */
/*                                                                          */
/* Produce one row of client pixels from one row of producer pixels.  Three  */
/* paths, cheapest first: blanked (zeroes are black in any true-colour      */
/* format), identical formats (memcpy), and the general per-pixel repack --  */
/* unpack to 8-bit R, G, B, scale each to the client's maximum with          */
/* rounding, shift into place, and store in the client's size and byte      */
/* order.                                                                    */
/*                                                                          */
/* Arguments:                                                               */
/*     pS     : the server (producer format and its RFB description).       */
/*     pF     : the client's format.                                        */
/*     pSrc   : the first producer pixel of the row.                        */
/*     pDst   : where the client pixels go.                                 */
/*     iCount : pixels in the row.                                          */
/*     bBlank : non-zero to emit black instead of the source.               */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_ConvertRow(const VncServer *pS, const VncPixelFormat *pF,
                            const unsigned char *pSrc, unsigned char *pDst,
                            int iCount, int bBlank)
{
    int          i;
    int          iOutBytes;
    unsigned int uR;
    unsigned int uG;
    unsigned int uB;
    unsigned int uV;
    unsigned long ulPix;

    iOutBytes = pF->iBpp / 8;

    if (bBlank)
    {   /* Blanked: all components zero, which is black in every true-colour format. */
        memset(pDst, 0, (size_t)iCount * (size_t)iOutBytes);
        return;
    }

    if (VNCS_FormatsEqual(pF, &pS->fmtNative))
    {   /* The client asked for exactly the producer's layout: copy the row. */
        memcpy(pDst, pSrc, (size_t)iCount * (size_t)iOutBytes);
        return;
    }

    for (i = 0; i < iCount; i++)
    {   /* One pixel per iteration. */

        /* Unpack the producer pixel to 8-bit red, green and blue. */
        if (pS->ePixFmt == VNC_PIXFMT_RGB565)
        {   /* 5-6-5: widen each field to 8 bits by repeating its top bits. */
            uV = (unsigned int)pSrc[0] | ((unsigned int)pSrc[1] << 8);
            uR = (uV >> 11) & 0x1F;
            uG = (uV >> 5) & 0x3F;
            uB = uV & 0x1F;
            uR = (uR << 3) | (uR >> 2);
            uG = (uG << 2) | (uG >> 4);
            uB = (uB << 3) | (uB >> 2);
            pSrc += 2;
        }
        else
        {   /* BGRA8888: bytes are blue, green, red, alpha; alpha is not sent. */
            uB = pSrc[0];
            uG = pSrc[1];
            uR = pSrc[2];
            pSrc += 4;
        }

        /* Scale each component to the client's range, rounding to nearest, */
        /* and place it at the client's shift.                              */
        ulPix = ((unsigned long)((uR * (unsigned int)pF->iRedMax + 127) / 255) << pF->iRedShift) |
                ((unsigned long)((uG * (unsigned int)pF->iGreenMax + 127) / 255) << pF->iGreenShift) |
                ((unsigned long)((uB * (unsigned int)pF->iBlueMax + 127) / 255) << pF->iBlueShift);

        /* Store the value in the client's pixel size and byte order. */
        if (iOutBytes == 1)
        {   /* 8 bpp: no byte order to consider. */
            pDst[0] = (unsigned char)(ulPix & 0xFF);
        }
        else if (iOutBytes == 2)
        {   /* 16 bpp in the client's byte order. */
            if (pF->bBigEndian)
            {   /* Most significant byte first. */
                pDst[0] = (unsigned char)((ulPix >> 8) & 0xFF);
                pDst[1] = (unsigned char)(ulPix & 0xFF);
            }
            else
            {   /* Least significant byte first. */
                pDst[0] = (unsigned char)(ulPix & 0xFF);
                pDst[1] = (unsigned char)((ulPix >> 8) & 0xFF);
            }
        }
        else
        {   /* 32 bpp in the client's byte order. */
            if (pF->bBigEndian)
            {   /* Most significant byte first. */
                VNCS_Put32(pDst, ulPix);
            }
            else
            {   /* Least significant byte first. */
                pDst[0] = (unsigned char)(ulPix & 0xFF);
                pDst[1] = (unsigned char)((ulPix >> 8) & 0xFF);
                pDst[2] = (unsigned char)((ulPix >> 16) & 0xFF);
                pDst[3] = (unsigned char)((ulPix >> 24) & 0xFF);
            }
        }
        pDst += iOutBytes;
    }
}

/*--------------------------------------------------------------------------*/
/* Rectangles and tiles.                                                     */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* VNCS_ClipRect:                                                           */
/*                                                                          */
/* Clip a rectangle to the framebuffer in place.  Every entry point that     */
/* takes a caller's rectangle clips it here, so no later loop can index      */
/* outside a buffer.                                                        */
/*                                                                          */
/* Arguments:                                                               */
/*     pS  : the server (its width and height).                             */
/*     pR  : in/out; the rectangle.                                         */
/*                                                                          */
/* Returns:                                                                 */
/*     int : non-zero if anything is left after clipping, zero if empty.    */
/*--------------------------------------------------------------------------*/

static int VNCS_ClipRect(const VncServer *pS, VncRect *pR)
{
    if (pR->iX < 0)
    {   /* Starts left of the buffer: drop the part outside. */
        pR->iW += pR->iX;
        pR->iX = 0;
    }

    if (pR->iY < 0)
    {   /* Starts above the buffer: drop the part outside. */
        pR->iH += pR->iY;
        pR->iY = 0;
    }

    if (pR->iX + pR->iW > pS->iWidth)
    {   /* Runs off the right edge. */
        pR->iW = pS->iWidth - pR->iX;
    }

    if (pR->iY + pR->iH > pS->iHeight)
    {   /* Runs off the bottom edge. */
        pR->iH = pS->iHeight - pR->iY;
    }

    /* Empty if either dimension has gone to nothing. */
    return((pR->iW > 0) && (pR->iH > 0));
}

/*--------------------------------------------------------------------------*/
/* VNCS_MarkTiles:                                                          */
/*                                                                          */
/* Set every tile a (clipped, non-empty) rectangle touches in a tile map.    */
/* Used for the pending map and for the client dirty maps alike.            */
/*                                                                          */
/* Arguments:                                                               */
/*     pS   : the server (its tile grid).                                   */
/*     pMap : the tile map, iTilesX * iTilesY bytes.                        */
/*     pR   : the rectangle, already clipped.                               */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_MarkTiles(const VncServer *pS, unsigned char *pMap, const VncRect *pR)
{
    int iTx;
    int iTy;
    int iTx0;
    int iTx1;
    int iTy0;
    int iTy1;

    /* The first and last tile columns and rows the rectangle touches. */
    iTx0 = pR->iX / VNCS_TILE;
    iTx1 = (pR->iX + pR->iW - 1) / VNCS_TILE;
    iTy0 = pR->iY / VNCS_TILE;
    iTy1 = (pR->iY + pR->iH - 1) / VNCS_TILE;

    for (iTy = iTy0; iTy <= iTy1; iTy++)
    {   /* One tile row per iteration. */
        for (iTx = iTx0; iTx <= iTx1; iTx++)
        {   /* One tile per iteration. */
            pMap[iTy * pS->iTilesX + iTx] = 1;
        }
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_MarkAllClients:                                                     */
/*                                                                          */
/* Mark the whole screen dirty for every connected client.  Used when what   */
/* a client should see changes wholesale -- blanking and unblanking -- so    */
/* each gets a complete frame at its next request.  Caller holds the mutex.  */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_MarkAllClients(VncServer *pS)
{
    int i;

    for (i = 0; i < pS->iMaxClients; i++)
    {   /* One client slot per iteration; only fully connected clients have a map in use. */
        if (pS->arrClients[i].eState == VNCC_NORMAL)
        {   /* Connected: everything is new to it. */
            memset(pS->arrClients[i].pDirty, 1, (size_t)(pS->iTilesX * pS->iTilesY));
        }
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_Wake:                                                               */
/*                                                                          */
/* Nudge the network thread out of poll() so it looks at the new state.     */
/* The pipe is non-blocking: if it is already full the thread is certainly   */
/* going to wake, so a write refused as full is not an error; any other      */
/* failure is logged.                                                        */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_Wake(VncServer *pS)
{
    unsigned char ucByte;
    ssize_t       lWritten;

    if (pS->arrWake[1] < 0)
    {   /* Not started: there is no thread to wake. */
        return;
    }

    /* One byte is enough; its value means nothing. */
    ucByte = 1;
    lWritten = write(pS->arrWake[1], &ucByte, 1);
    if ((lWritten < 0) && (errno != EAGAIN) && (errno != EINTR))
    {   /* A full pipe (EAGAIN) already guarantees a wake; any other failure */
        /* means this change waits for the loop's next event to be seen.    */
        VNCS_Log(pS, "wake pipe write failed: %s", strerror(errno));
    }
}

/*--------------------------------------------------------------------------*/
/* Client connection handling (network thread only).                         */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* VNCS_SendAll:                                                            */
/*                                                                          */
/* Send a whole buffer to a client.  MSG_NOSIGNAL on every call, so a peer   */
/* that has gone cannot raise SIGPIPE and kill the host; the socket's send   */
/* timeout bounds how long a stalled peer can hold the network thread.       */
/*                                                                          */
/* Arguments:                                                               */
/*     pS    : the server (for logging).                                    */
/*     iFd   : the client socket.                                           */
/*     pBuf  : the bytes to send.                                           */
/*     ulLen : how many.                                                    */
/*                                                                          */
/* Returns:                                                                 */
/*     int : VNCS_SUCCESS when every byte went, VNCS_FAILURE on an error or  */
/*           timeout (the caller drops the client).                          */
/*--------------------------------------------------------------------------*/

static int VNCS_SendAll(const VncServer *pS, int iFd, const unsigned char *pBuf, size_t ulLen)
{
    ssize_t lSent;

    while (ulLen > 0)
    {   /* One send() per iteration until the buffer is gone. */
        lSent = send(iFd, pBuf, ulLen, MSG_NOSIGNAL);
        if (lSent < 0)
        {   /* Interrupted is harmless; anything else (including the timeout) ends the client. */
            if (errno == EINTR)
            {   /* A signal arrived mid-send: try again. */
                continue;
            }
            VNCS_Log(pS, "send failed: %s", strerror(errno));
            return(VNCS_FAILURE);
        }
        pBuf += lSent;
        ulLen -= (size_t)lSent;
    }

    /* Everything was sent. */
    return(VNCS_SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* VNCS_ClientDrop:                                                         */
/*                                                                          */
/* Close a client and return its slot.  The state change and the dirty-map   */
/* free happen under the mutex because a producer's commit walks the client  */
/* maps; the socket and the send buffer are the network thread's alone.     */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*     pC : the client.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void VNCS_ClientDrop(VncServer *pS, VncClient *pC)
{
    if (pC->iFd >= 0)
    {   /* Close the connection first so the peer sees the end promptly. */
        close(pC->iFd);
        pC->iFd = -1;
    }

    /* Retire the slot as the commit path sees it. */
    pthread_mutex_lock(&pS->mtxLock);
    pC->eState = VNCC_FREE;
    free(pC->pDirty);
    pC->pDirty = NULL;
    pthread_mutex_unlock(&pS->mtxLock);

    /* Network-thread-only resources. */
    free(pC->pRects);
    pC->pRects = NULL;
    free(pC->pSend);
    pC->pSend = NULL;
    pC->ulSendCap = 0;
    pC->iRxLen = 0;
    pC->ulSkip = 0;
    pC->uEncLeft = 0;
    pC->bRequest = 0;
}

/*--------------------------------------------------------------------------*/
/* VNCS_Accept:                                                             */
/*                                                                          */
/* Take one pending connection: find a free slot (or refuse by closing),     */
/* set the socket options that matter -- no Nagle delay for small updates,   */
/* and a send timeout so a dead peer cannot hang the loop -- allocate the    */
/* client's maps, and open the handshake by sending our ProtocolVersion.     */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; a failure closes the new connection and is logged.   */
/*--------------------------------------------------------------------------*/

static void VNCS_Accept(VncServer *pS)
{
    int            iFd;
    int            i;
    int            iOne;
    int            iTiles;
    struct timeval tvSend;
    VncClient     *pC;

    iFd = accept4(pS->iListenFd, NULL, NULL, SOCK_CLOEXEC);
    if (iFd < 0)
    {   /* Nothing to take after all (or a transient error): leave it for the next poll. */
        return;
    }

    /* Find a free slot. */
    pC = NULL;
    for (i = 0; i < pS->iMaxClients; i++)
    {   /* One slot per iteration; the first free one is taken. */
        if (pS->arrClients[i].eState == VNCC_FREE)
        {   /* Free: use it. */
            pC = &pS->arrClients[i];
            break;
        }
    }

    if (pC == NULL)
    {   /* Every slot is in use: refuse by closing. */
        VNCS_Log(pS, "client refused: all %d slots in use", pS->iMaxClients);
        close(iFd);
        return;
    }

    /* Small updates go out at once rather than waiting to coalesce. */
    iOne = 1;
    setsockopt(iFd, IPPROTO_TCP, TCP_NODELAY, &iOne, sizeof(iOne));

    /* Bound every send, so a peer that stops reading is dropped, not waited on. */
    tvSend.tv_sec = VNCS_SEND_TIMEOUT_MS / 1000;
    tvSend.tv_usec = (VNCS_SEND_TIMEOUT_MS % 1000) * 1000;
    setsockopt(iFd, SOL_SOCKET, SO_SNDTIMEO, &tvSend, sizeof(tvSend));

    /* Per-client maps: the dirty tiles, and room for one rectangle per tile. */
    iTiles = pS->iTilesX * pS->iTilesY;
    pC->pRects = (VncRect *)malloc((size_t)iTiles * sizeof(VncRect));
    if (pC->pRects == NULL)
    {   /* Out of memory: cannot serve this client. */
        VNCS_Log(pS, "client refused: out of memory");
        close(iFd);
        return;
    }

    /* Publish the new slot to the commit path under the mutex. */
    pthread_mutex_lock(&pS->mtxLock);
    pC->pDirty = (unsigned char *)calloc((size_t)iTiles, 1);
    if (pC->pDirty == NULL)
    {   /* Out of memory: undo and refuse. */
        pthread_mutex_unlock(&pS->mtxLock);
        free(pC->pRects);
        pC->pRects = NULL;
        VNCS_Log(pS, "client refused: out of memory");
        close(iFd);
        return;
    }
    pC->eState = VNCC_VERSION;
    pthread_mutex_unlock(&pS->mtxLock);

    /* Fresh per-connection state. */
    pC->iFd = iFd;
    pC->iMinor = 8;
    pC->iRxLen = 0;
    pC->ulSkip = 0;
    pC->uEncLeft = 0;
    pC->bEncChosen = 0;
    pC->iEncoding = RFB_ENC_RAW;
    pC->bRequest = 0;
    pC->bIncremental = 0;
    pC->fmt = pS->fmtNative;

    /* The server speaks first: our ProtocolVersion (RFC 6143 section 7.1.1). */
    if (VNCS_SendAll(pS, iFd, (const unsigned char *)RFB_VERSION_STRING,
                     RFB_VERSION_LEN) != VNCS_SUCCESS)
    {   /* Gone before the handshake began. */
        VNCS_ClientDrop(pS, pC);
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_SendServerInit:                                                     */
/*                                                                          */
/* Send ServerInit (RFC 6143 section 7.3.2): framebuffer size, the native    */
/* pixel format, and the desktop name.                                       */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*     pC : the client.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     int : VNCS_SUCCESS if sent, VNCS_FAILURE if the send failed.         */
/*--------------------------------------------------------------------------*/

static int VNCS_SendServerInit(VncServer *pS, VncClient *pC)
{
    unsigned char arrMsg[2 + 2 + RFB_PIXFMT_LEN + 4 + sizeof(VNCS_DESKTOP_NAME)];
    size_t        ulNameLen;

    ulNameLen = strlen(VNCS_DESKTOP_NAME);

    /* width, height, PIXEL_FORMAT, name length, name. */
    VNCS_Put16(&arrMsg[0], (unsigned int)pS->iWidth);
    VNCS_Put16(&arrMsg[2], (unsigned int)pS->iHeight);
    VNCS_WriteFormat(&pS->fmtNative, &arrMsg[4]);
    VNCS_Put32(&arrMsg[4 + RFB_PIXFMT_LEN], (unsigned long)ulNameLen);
    memcpy(&arrMsg[8 + RFB_PIXFMT_LEN], VNCS_DESKTOP_NAME, ulNameLen);

    /* Hand the result of the send straight back. */
    return(VNCS_SendAll(pS, pC->iFd, arrMsg, 8 + RFB_PIXFMT_LEN + ulNameLen));
}

/*--------------------------------------------------------------------------*/
/* VNCS_MakeChallenge:                                                      */
/*                                                                          */
/* Fill a 16-byte VNC-authentication challenge with random bytes from        */
/* /dev/urandom.  RFC 6143 section 7.2.2 needs the challenge unpredictable   */
/* so a captured response cannot be replayed; reading the kernel CSPRNG      */
/* keeps that guarantee without adding an RNG of our own.  A failed or short */
/* read is treated as failure rather than sending a weak challenge, so a     */
/* client is dropped instead of authenticated against guessable bytes.       */
/*                                                                          */
/* Arguments:                                                               */
/*     pS     : the server, used only to log a failure.                     */
/*     pOut16 : 16-byte buffer to receive the challenge.                    */
/*                                                                          */
/* Returns:                                                                 */
/*     int : VNCS_SUCCESS with pOut16 filled, or VNCS_FAILURE (logged) when  */
/*           the random bytes could not be read.                            */
/*--------------------------------------------------------------------------*/

static int VNCS_MakeChallenge(VncServer *pS, unsigned char *pOut16)
{
    int     iFd;
    int     iGot;
    ssize_t iN;

    /* Open the kernel random source read-only. */
    iFd = open("/dev/urandom", O_RDONLY);
    if (iFd < 0)
    {   /* No random source: refuse rather than send a predictable challenge. */
        VNCS_Log(pS, "auth: cannot open /dev/urandom: %s", strerror(errno));
        return(VNCS_FAILURE);
    }

    iGot = 0;
    while (iGot < RFB_VNC_CHALLENGE_LEN)
    {   /* Loop over short reads until all 16 bytes are in hand. */
        iN = read(iFd, pOut16 + iGot, (size_t)(RFB_VNC_CHALLENGE_LEN - iGot));
        if (iN < 0)
        {   /* An error: retry an interrupted read, give up on any other. */
            if (errno == EINTR)
            {   /* Interrupted by a signal before any byte: try again. */
                continue;
            }
            VNCS_Log(pS, "auth: read from /dev/urandom failed: %s", strerror(errno));
            close(iFd);
            return(VNCS_FAILURE);
        }
        if (iN == 0)
        {   /* EOF from /dev/urandom should never happen; do not spin. */
            VNCS_Log(pS, "auth: unexpected EOF from /dev/urandom");
            close(iFd);
            return(VNCS_FAILURE);
        }
        iGot += (int)iN;
    }

    close(iFd);

    /* Sixteen random bytes are ready in pOut16. */
    return(VNCS_SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* VNCS_SendChallenge:                                                      */
/*                                                                          */
/* Make this client's random challenge, keep it for checking the response,   */
/* send it, and move the client into VNCC_AUTH.  Shared by the 3.7 / 3.8     */
/* flow (after the client picks VNC Authentication) and the 3.3 flow (right  */
/* after the server names the type).                                         */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*     pC : the client to challenge.                                        */
/*                                                                          */
/* Returns:                                                                 */
/*     int : VNCS_SUCCESS with the challenge sent and eState VNCC_AUTH, or    */
/*           VNCS_FAILURE (logged) when it could not be made or sent -- the   */
/*           caller then drops the client.                                   */
/*--------------------------------------------------------------------------*/

static int VNCS_SendChallenge(VncServer *pS, VncClient *pC)
{
    /* A fresh random challenge, held until the response is checked. */
    if (VNCS_MakeChallenge(pS, pC->arrChallenge) != VNCS_SUCCESS)
    {   /* The reason has been logged; the caller drops the client. */
        return(VNCS_FAILURE);
    }

    /* Send it; the client encrypts it with the password and returns 16 bytes. */
    if (VNCS_SendAll(pS, pC->iFd, pC->arrChallenge, RFB_VNC_CHALLENGE_LEN) != VNCS_SUCCESS)
    {   /* The socket write failed. */
        return(VNCS_FAILURE);
    }

    /* Now waiting for the 16-byte response in VNCC_AUTH. */
    pC->eState = VNCC_AUTH;
    return(VNCS_SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* VNCS_AuthResponseOk:                                                     */
/*                                                                          */
/* Check a client's 16-byte response (the front of arrRx) against the        */
/* challenge.  The expected response is the challenge DES-encrypted with the  */
/* server's password -- the same computation the client made, RFC 6143       */
/* section 7.2.2 -- and the two are compared in CONSTANT TIME so the number   */
/* of leading matching bytes cannot be read from how long the compare takes.  */
/* The derived expected bytes are scrubbed from the stack before returning.   */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server (arrPassword holds the first 8 password bytes).       */
/*     pC : the client (arrChallenge the sent challenge, arrRx the response). */
/*                                                                          */
/* Returns:                                                                 */
/*     int : VNCS_SUCCESS if the response matches, else VNCS_FAILURE.        */
/*--------------------------------------------------------------------------*/

static int VNCS_AuthResponseOk(VncServer *pS, VncClient *pC)
{
    unsigned char arrExpected[RFB_VNC_CHALLENGE_LEN];
    int           iDiff;
    int           i;

    /* Re-derive the response the client should have sent for this challenge. */
    VDES_VncEncryptChallenge(pS->arrPassword, sizeof(pS->arrPassword),
                             pC->arrChallenge, arrExpected);

    /* Constant-time compare: OR every byte's difference so the loop runs the  */
    /* full length whatever the input, and no early mismatch shortens it.      */
    iDiff = 0;
    for (i = 0; i < RFB_VNC_CHALLENGE_LEN; i++)
    {   /* One byte per iteration: accumulate any difference. */
        iDiff |= (int)(arrExpected[i] ^ pC->arrRx[i]);
    }

    /* The expected response is password-derived; do not leave it on the stack. */
    memset(arrExpected, 0, sizeof(arrExpected));

    if (iDiff != 0)
    {   /* At least one byte differed: the password was wrong. */
        return(VNCS_FAILURE);
    }

    /* Every byte matched: the client holds the password. */
    return(VNCS_SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* VNCS_SendAuthFail:                                                       */
/*                                                                          */
/* Send a failed SecurityResult after a wrong password (RFC 6143 section     */
/* 7.1.3): the u32 1, and on RFB 3.8 a reason string a viewer may show.      */
/* Best effort -- the client is dropped next regardless, so a send failure    */
/* here needs no separate handling.                                         */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*     pC : the client being rejected (iMinor selects the 3.8 reason).      */
/*                                                                          */
/* Returns:                                                                 */
/*     void : the result (and reason on 3.8) is sent best-effort.           */
/*--------------------------------------------------------------------------*/

static void VNCS_SendAuthFail(VncServer *pS, VncClient *pC)
{
    static const char acReason[] = "Authentication failed";
    unsigned char     arrOut[4];
    unsigned char     arrLen[4];

    /* SecurityResult: failed. */
    VNCS_Put32(arrOut, (unsigned long)RFB_SEC_RESULT_FAILED);
    if (VNCS_SendAll(pS, pC->iFd, arrOut, 4) != VNCS_SUCCESS)
    {   /* The client is being dropped anyway; nothing more to do. */
        return;
    }

    if (pC->iMinor >= 8)
    {   /* 3.8 follows a failed result with a reason string: u32 length, text. */
        VNCS_Put32(arrLen, (unsigned long)(sizeof(acReason) - 1));
        if (VNCS_SendAll(pS, pC->iFd, arrLen, 4) != VNCS_SUCCESS)
        {   /* Give up quietly; the drop follows. */
            return;
        }
        VNCS_SendAll(pS, pC->iFd, (const unsigned char *)acReason, sizeof(acReason) - 1);
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_HandleMessage:                                                      */
/*                                                                          */
/* Advance one client by one unit of input from the front of its receive     */
/* buffer: one handshake step, or one complete client message.  Knowing the  */
/* length of every message type is what lets the stream be parsed at all;    */
/* an unknown type cannot be skipped, so it ends the connection.  Variable   */
/* payloads are never buffered: the SetEncodings list is counted into        */
/* uEncLeft and read an entry at a time, ClientCutText's length goes into    */
/* ulSkip and its bytes are discarded as they arrive -- so a client cannot   */
/* make the server allocate from a length it sent.                           */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*     pC : the client (its arrRx / iRxLen hold the unparsed bytes).        */
/*                                                                          */
/* Returns:                                                                 */
/*     int : bytes consumed from the front of arrRx; 0 when more bytes are   */
/*           needed first; -1 when the client must be dropped.               */
/*--------------------------------------------------------------------------*/

static int VNCS_HandleMessage(VncServer *pS, VncClient *pC)
{
    unsigned char arrOut[4];
    unsigned int  uCount;
    int           iNeed;
    VncRect       rc;
    VncInputEvent ev;

    switch (pC->eState)
    {   /* Dispatch on where the client is in the connection sequence. */

        case VNCC_VERSION:
            /* The client's ProtocolVersion, "RFB xxx.yyy\n". */
            if (pC->iRxLen < RFB_VERSION_LEN)
            {   /* Not all twelve bytes yet. */
                return(0);
            }
            if (memcmp(pC->arrRx, "RFB 003.", 8) != 0)
            {   /* Not an RFB 3.x client. */
                VNCS_Log(pS, "client dropped: not an RFB 3.x version string");
                return(-1);
            }

            /* Map the minor version onto the three flows RFC 6143 section 7.1 defines. */
            pC->iMinor = (pC->arrRx[8] - '0') * 100 + (pC->arrRx[9] - '0') * 10 + (pC->arrRx[10] - '0');
            if (pC->iMinor >= 8)
            {   /* 3.8 and anything newer is spoken as 3.8. */
                pC->iMinor = 8;
            }
            else if (pC->iMinor == 7)
            {   /* 3.7: the security list, but no result message for None. */
                pC->iMinor = 7;
            }
            else
            {   /* 3.3 (and the odd non-standard minor): the server names the type. */
                pC->iMinor = 3;
            }

            /* The security offer depends on whether a password is set: with   */
            /* one the server offers VNC Authentication, without one None.      */
            /* 3.7 / 3.8 send a one-entry list and wait for the client's        */
            /* choice; 3.3 names the single type as a u32, and for VNC the       */
            /* 16-byte challenge follows at once (moving to VNCC_AUTH).          */
            if (pC->iMinor >= 7)
            {   /* 3.7 / 3.8: a one-entry list, then wait for the choice. */
                arrOut[0] = 1;
                if (pS->eAuth == VNC_AUTH_VNC)
                {   /* A password is set: offer VNC Authentication. */
                    arrOut[1] = RFB_SEC_VNC;
                }
                else
                {   /* No password: offer None. */
                    arrOut[1] = RFB_SEC_NONE;
                }
                if (VNCS_SendAll(pS, pC->iFd, arrOut, 2) != VNCS_SUCCESS)
                {   /* The send failed; drop. */
                    return(-1);
                }
                pC->eState = VNCC_SECURITY;
            }
            else if (pS->eAuth == VNC_AUTH_VNC)
            {   /* 3.3 with a password: name VNC as a u32, then challenge at once. */
                VNCS_Put32(arrOut, (unsigned long)RFB_SEC_VNC);
                if (VNCS_SendAll(pS, pC->iFd, arrOut, 4) != VNCS_SUCCESS)
                {   /* The send failed; drop. */
                    return(-1);
                }
                if (VNCS_SendChallenge(pS, pC) != VNCS_SUCCESS)
                {   /* The challenge could not be made or sent; drop. */
                    return(-1);
                }
            }
            else
            {   /* 3.3 with no password: the server names None; ClientInit next. */
                VNCS_Put32(arrOut, (unsigned long)RFB_SEC_NONE);
                if (VNCS_SendAll(pS, pC->iFd, arrOut, 4) != VNCS_SUCCESS)
                {   /* The send failed; drop. */
                    return(-1);
                }
                pC->eState = VNCC_INIT;
            }
            /* The twelve version bytes are consumed. */
            return(RFB_VERSION_LEN);

        case VNCC_SECURITY:
            /* The client's chosen security type: one byte.  It must be the    */
            /* single type the server offered -- VNC Authentication when a      */
            /* password is set, else None.  For VNC the server sends a fresh    */
            /* random challenge and waits in VNCC_AUTH; for None it answers a   */
            /* SecurityResult on 3.8 and moves to VNCC_INIT.                    */
            if (pS->eAuth == VNC_AUTH_VNC)
            {   /* A password is set: the client must have chosen VNC. */
                if (pC->arrRx[0] != RFB_SEC_VNC)
                {   /* It chose something that was not offered. */
                    VNCS_Log(pS, "client dropped: chose security type %u", (unsigned int)pC->arrRx[0]);
                    return(-1);
                }
                if (VNCS_SendChallenge(pS, pC) != VNCS_SUCCESS)
                {   /* The challenge could not be made or sent; drop. */
                    return(-1);
                }
                /* The one security-type byte is consumed; the response is next. */
                return(1);
            }

            /* No password: None is the only offered type. */
            if (pC->arrRx[0] != RFB_SEC_NONE)
            {   /* It chose something that was not offered. */
                VNCS_Log(pS, "client dropped: chose security type %u", (unsigned int)pC->arrRx[0]);
                return(-1);
            }
            if (pC->iMinor == 8)
            {   /* 3.8 sends a SecurityResult even for None; 3.7 does not. */
                VNCS_Put32(arrOut, (unsigned long)RFB_SEC_RESULT_OK);
                if (VNCS_SendAll(pS, pC->iFd, arrOut, 4) != VNCS_SUCCESS)
                {   /* The send failed; drop. */
                    return(-1);
                }
            }
            pC->eState = VNCC_INIT;

            /* The one security-type byte is consumed. */
            return(1);

        case VNCC_AUTH:
            /* The client's 16-byte DES response to the challenge (7.2.2).  Wait */
            /* until all sixteen have arrived, then check them against the       */
            /* challenge encrypted with the password: a match sends an OK        */
            /* SecurityResult and moves to ClientInit, a mismatch sends a failed */
            /* result (with a reason on 3.8) and drops the client.               */
            if (pC->iRxLen < RFB_VNC_CHALLENGE_LEN)
            {   /* The whole response has not arrived yet. */
                return(0);
            }
            if (VNCS_AuthResponseOk(pS, pC) != VNCS_SUCCESS)
            {   /* Wrong password: tell the client, log it, and drop. */
                VNCS_SendAuthFail(pS, pC);
                VNCS_Log(pS, "client dropped: VNC authentication failed");
                return(-1);
            }
            /* Correct password: accept and move on to ClientInit. */
            VNCS_Put32(arrOut, (unsigned long)RFB_SEC_RESULT_OK);
            if (VNCS_SendAll(pS, pC->iFd, arrOut, 4) != VNCS_SUCCESS)
            {   /* The send failed; drop. */
                return(-1);
            }
            pC->eState = VNCC_INIT;

            /* The sixteen response bytes are consumed. */
            return(RFB_VNC_CHALLENGE_LEN);

        case VNCC_INIT:
            /* ClientInit: one byte, the shared flag.  Clients always share here. */
            if (VNCS_SendServerInit(pS, pC) != VNCS_SUCCESS)
            {   /* The send failed; drop. */
                return(-1);
            }

            /* Now connected: everything on screen is new to this client, so */
            /* even an incremental first request gets the whole frame.       */
            pthread_mutex_lock(&pS->mtxLock);
            memset(pC->pDirty, 1, (size_t)(pS->iTilesX * pS->iTilesY));
            pC->eState = VNCC_NORMAL;
            pthread_mutex_unlock(&pS->mtxLock);

            /* The one ClientInit byte is consumed. */
            return(1);

        case VNCC_NORMAL:
            /* A client message: its type byte decides its length. */
            switch (pC->arrRx[0])
            {   /* The fixed part of each message type. */
                case RFB_CMSG_SETPIXELFORMAT:
                    iNeed = RFB_LEN_SETPIXELFORMAT;
                    break;
                case RFB_CMSG_SETENCODINGS:
                    iNeed = RFB_LEN_SETENCODINGS;
                    break;
                case RFB_CMSG_FBUPDATEREQ:
                    iNeed = RFB_LEN_FBUPDATEREQ;
                    break;
                case RFB_CMSG_KEYEVENT:
                    iNeed = RFB_LEN_KEYEVENT;
                    break;
                case RFB_CMSG_POINTEREVENT:
                    iNeed = RFB_LEN_POINTEREVENT;
                    break;
                case RFB_CMSG_CUTTEXT:
                    iNeed = RFB_LEN_CUTTEXT;
                    break;
                default:
                    /* An unknown type has an unknown length: the stream cannot continue. */
                    VNCS_Log(pS, "client dropped: unknown message type %u", (unsigned int)pC->arrRx[0]);
                    return(-1);
            }

            if (pC->iRxLen < iNeed)
            {   /* The fixed part has not all arrived. */
                return(0);
            }

            switch (pC->arrRx[0])
            {   /* Act on the complete message. */
                case RFB_CMSG_SETPIXELFORMAT:
                    /* From now on this client's updates use its format. */
                    if (VNCS_ReadFormat(&pC->arrRx[4], &pC->fmt) != VNCS_SUCCESS)
                    {   /* A format this server cannot produce. */
                        VNCS_Log(pS, "client dropped: unsupported pixel format");
                        return(-1);
                    }
                    break;

                case RFB_CMSG_SETENCODINGS:
                    /* The encodings this client accepts, in order of       */
                    /* preference.  The list itself is read an entry at a    */
                    /* time as it arrives (VNCS_ClientRead), never stored.   */
                    /* Until it names Raw or Hextile the client gets Raw,    */
                    /* which every client accepts.                           */
                    uCount = VNCS_Get16(&pC->arrRx[2]);
                    pC->uEncLeft = uCount;
                    pC->bEncChosen = 0;
                    pC->iEncoding = RFB_ENC_RAW;
                    break;

                case RFB_CMSG_FBUPDATEREQ:
                    /* Record the request; the service pass answers it. */
                    rc.iX = (int)VNCS_Get16(&pC->arrRx[2]);
                    rc.iY = (int)VNCS_Get16(&pC->arrRx[4]);
                    rc.iW = (int)VNCS_Get16(&pC->arrRx[6]);
                    rc.iH = (int)VNCS_Get16(&pC->arrRx[8]);
                    if (!VNCS_ClipRect(pS, &rc))
                    {   /* Entirely off-screen: an empty region (answered with no rectangles). */
                        rc.iW = 0;
                        rc.iH = 0;
                    }
                    pC->rcReq = rc;
                    pC->bIncremental = (pC->arrRx[1] != 0);
                    pC->bRequest = 1;
                    break;

                case RFB_CMSG_KEYEVENT:
                    /* Pass the key to the producer, if it wants input. */
                    if (pS->pfnInput != NULL)
                    {   /* Called on this (network) thread; the producer only enqueues. */
                        memset(&ev, 0, sizeof(ev));
                        ev.eType = VNC_INPUT_KEY;
                        ev.iDown = (pC->arrRx[1] != 0);
                        ev.uKeySym = (unsigned int)VNCS_Get32(&pC->arrRx[4]);
                        pS->pfnInput(pS->pInputUser, &ev);
                    }
                    break;

                case RFB_CMSG_POINTEREVENT:
                    /* Pass the pointer to the producer, clamped onto the screen. */
                    if (pS->pfnInput != NULL)
                    {   /* Called on this (network) thread; the producer only enqueues. */
                        memset(&ev, 0, sizeof(ev));
                        ev.eType = VNC_INPUT_POINTER;
                        ev.iButtons = pC->arrRx[1];
                        ev.iX = (int)VNCS_Get16(&pC->arrRx[2]);
                        ev.iY = (int)VNCS_Get16(&pC->arrRx[4]);
                        if (ev.iX >= pS->iWidth)
                        {   /* Beyond the right edge: pin to it. */
                            ev.iX = pS->iWidth - 1;
                        }
                        if (ev.iY >= pS->iHeight)
                        {   /* Beyond the bottom edge: pin to it. */
                            ev.iY = pS->iHeight - 1;
                        }
                        pS->pfnInput(pS->pInputUser, &ev);
                    }
                    break;

                case RFB_CMSG_CUTTEXT:
                    /* Clipboard text is not used: discard its payload as it arrives. */
                    pC->ulSkip = VNCS_Get32(&pC->arrRx[4]);
                    break;

                default:
                    /* Unreachable: the length switch above already rejected unknown types. */
                    return(-1);
            }
            /* The message's fixed part is consumed; what follows it is read   */
            /* later through uEncLeft (an encoding list) or discarded through  */
            /* ulSkip (cut text).                                              */
            return(iNeed);

        default:
            /* A free slot has no input to parse. */
            return(-1);
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_NoteEncoding:                                                       */
/*                                                                          */
/* Take one entry of a client's SetEncodings list into account.  The list   */
/* is in the client's order of preference (RFC 6143 section 7.5.2), so the  */
/* FIRST entry this server can produce -- Raw or Hextile -- decides, and the */
/* rest of the list is only read past.  Anything else (CopyRect, the         */
/* pseudo-encodings, encodings not built here) is ignored.                   */
/*                                                                          */
/* With the server's bNoHextile set, a Hextile entry is ignored like any     */
/* other encoding this server does not produce, so the list is read on to    */
/* Raw -- and a list with no Raw at all still ends in Raw, since the client  */
/* starts on it and every client must accept it (RFC 6143 section 7.7.1).    */
/*                                                                          */
/* Arguments:                                                               */
/*     pS    : the server, for its bNoHextile setting.                      */
/*     pC    : the client.                                                  */
/*     ulEnc : the entry, an s32 read as unsigned (a negative               */
/*             pseudo-encoding therefore matches neither value below).      */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; the choice is left in pC->iEncoding.                 */
/*--------------------------------------------------------------------------*/

static void VNCS_NoteEncoding(const VncServer *pS, VncClient *pC, unsigned long ulEnc)
{
    if (pC->bEncChosen)
    {   /* An earlier, preferred entry already decided. */
        return;
    }

    if (ulEnc == RFB_ENC_RAW)
    {   /* Raw preferred over anything later. */
        pC->iEncoding = RFB_ENC_RAW;
        pC->bEncChosen = 1;
    }
    else if ((ulEnc == RFB_ENC_HEXTILE) && !pS->bNoHextile)
    {   /* Hextile, allowed on this server: preferred over anything later, Raw included. */
        pC->iEncoding = RFB_ENC_HEXTILE;
        pC->bEncChosen = 1;
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_ClientRead:                                                         */
/*                                                                          */
/* Read what a readable client has sent and parse as far as the bytes go.   */
/* A SetEncodings list still streaming in is read first, one four-byte       */
/* entry at a time; then payload bytes owed to ulSkip are discarded; then    */
/* complete units are handed to VNCS_HandleMessage until it needs more.      */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*     pC : the client poll() reported readable.                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; a closed or misbehaving client is dropped.           */
/*--------------------------------------------------------------------------*/

static void VNCS_ClientRead(VncServer *pS, VncClient *pC)
{
    ssize_t lGot;
    int     iUsed;
    int     iDrop;

    lGot = recv(pC->iFd, &pC->arrRx[pC->iRxLen], (size_t)(VNCS_RX_SIZE - pC->iRxLen), 0);
    if (lGot <= 0)
    {   /* 0 is an orderly close; below 0 an error, unless merely interrupted. */
        if ((lGot < 0) && ((errno == EINTR) || (errno == EAGAIN)))
        {   /* Nothing lost: try again at the next poll. */
            return;
        }
        VNCS_ClientDrop(pS, pC);
        return;
    }
    pC->iRxLen += (int)lGot;

    while (pC->iRxLen > 0)
    {   /* One unit of parsing per iteration. */

        if (pC->uEncLeft > 0)
        {   /* The next entry of a SetEncodings list: four bytes, read in place. */
            if (pC->iRxLen < RFB_ENC_LEN)
            {   /* Not all four bytes yet. */
                break;
            }
            VNCS_NoteEncoding(pS, pC, VNCS_Get32(pC->arrRx));
            pC->uEncLeft--;
            memmove(pC->arrRx, &pC->arrRx[RFB_ENC_LEN], (size_t)(pC->iRxLen - RFB_ENC_LEN));
            pC->iRxLen -= RFB_ENC_LEN;
            continue;
        }

        if (pC->ulSkip > 0)
        {   /* Discard bytes owed to a variable-length payload. */
            iDrop = pC->iRxLen;
            if ((unsigned long)iDrop > pC->ulSkip)
            {   /* Only part of what arrived belongs to the payload. */
                iDrop = (int)pC->ulSkip;
            }
            memmove(pC->arrRx, &pC->arrRx[iDrop], (size_t)(pC->iRxLen - iDrop));
            pC->iRxLen -= iDrop;
            pC->ulSkip -= (unsigned long)iDrop;
            continue;
        }

        iUsed = VNCS_HandleMessage(pS, pC);
        if (iUsed < 0)
        {   /* A protocol error or a failed send: end the connection. */
            VNCS_ClientDrop(pS, pC);
            return;
        }
        if (iUsed == 0)
        {   /* An incomplete message: wait for more bytes. */
            break;
        }

        /* Drop the consumed bytes from the front of the buffer. */
        memmove(pC->arrRx, &pC->arrRx[iUsed], (size_t)(pC->iRxLen - iUsed));
        pC->iRxLen -= iUsed;
    }
}

/*--------------------------------------------------------------------------*/
/* Hextile encoding (network thread only, called under the mutex).           */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* VNCS_HextileKey:                                                         */
/*                                                                          */
/* Turn one client-format pixel -- its 1, 2 or 4 bytes exactly as they will */
/* go on the wire -- into a number that is equal for two pixels precisely   */
/* when their bytes are equal.  The bytes are packed first byte lowest,     */
/* which is not the pixel's value in the client's byte order, and does not  */
/* need to be: a key is only ever compared, and VNCS_HextilePutKey writes   */
/* it back in the same fixed order, so the bytes round-trip unchanged.      */
/*                                                                          */
/* Arguments:                                                               */
/*     pPix   : the pixel's first byte.                                     */
/*     iBytes : bytes per client pixel: 1, 2 or 4.                          */
/*                                                                          */
/* Returns:                                                                 */
/*     unsigned long : the key.                                             */
/*--------------------------------------------------------------------------*/

static unsigned long VNCS_HextileKey(const unsigned char *pPix, int iBytes)
{
    unsigned long ulKey;
    int           i;

    ulKey = 0;
    for (i = 0; i < iBytes; i++)
    {   /* One byte per iteration, the first byte in the lowest bits. */
        ulKey |= (unsigned long)pPix[i] << (8 * i);
    }

    /* Equal keys mean byte-for-byte equal pixels. */
    return(ulKey);
}

/*--------------------------------------------------------------------------*/
/* VNCS_HextilePutKey:                                                      */
/*                                                                          */
/* Write a pixel key back out as the pixel's wire bytes, in the order       */
/* VNCS_HextileKey read them -- the inverse of that function.               */
/*                                                                          */
/* Arguments:                                                               */
/*     pDst   : where the pixel's bytes go.                                 */
/*     ulKey  : the key.                                                    */
/*     iBytes : bytes per client pixel: 1, 2 or 4.                          */
/*                                                                          */
/* Returns:                                                                 */
/*     unsigned char * : the byte after the pixel, where the caller writes  */
/*                       next.                                              */
/*--------------------------------------------------------------------------*/

static unsigned char *VNCS_HextilePutKey(unsigned char *pDst, unsigned long ulKey, int iBytes)
{
    int i;

    for (i = 0; i < iBytes; i++)
    {   /* One byte per iteration, lowest bits first. */
        pDst[i] = (unsigned char)((ulKey >> (8 * i)) & 0xFF);
    }

    /* The caller continues after the pixel. */
    return(pDst + iBytes);
}

/*--------------------------------------------------------------------------*/
/* VNCS_HextileTile:                                                        */
/*                                                                          */
/* Encode one Hextile tile (at most 16x16) from its client-format pixels,   */
/* in the smallest of the forms RFC 6143 section 7.7.4 allows:               */
/*                                                                          */
/*   one colour      -> the mask alone, plus the background pixel when it   */
/*                      differs from what the decoder holds;               */
/*   two colours     -> background (the commoner colour) and foreground,    */
/*                      each sent only when it changed, then plain          */
/*                      subrectangles of the foreground;                    */
/*   3 to 16 colours -> background, then subrectangles each carrying its   */
/*                      own pixel value;                                   */
/*   anything else   -> the raw pixels.                                    */
/*                                                                          */
/* A flat user interface is mostly one- and two-colour tiles, which is where */
/* the saving comes from.  The compact form is built in a local buffer and   */
/* used only when it is SMALLER than the raw tile, so a tile never takes     */
/* more than one byte beyond its raw size -- the bound the caller sizes its  */
/* message buffer by, which makes writing past that buffer impossible.       */
/*                                                                          */
/* Subrectangles are found greedily: in raster order, each pixel that is not */
/* background and not yet covered starts a rectangle, which runs right as    */
/* far as its colour does and then down as far as every pixel under that    */
/* run matches.                                                              */
/*                                                                          */
/* Arguments:                                                               */
/*     pTile  : the tile's pixels in the client's format, rows packed        */
/*              (iW * iBytes bytes per row).                                 */
/*     iW, iH : the tile's width and height, 1 to 16 each.                  */
/*     iBytes : bytes per client pixel: 1, 2 or 4.                          */
/*     pSt    : in/out; what the decoder remembers from the previous tile.  */
/*     pDst   : where the encoded tile goes; room for 1 + iW * iH * iBytes. */
/*                                                                          */
/* Returns:                                                                 */
/*     size_t : bytes written to pDst.                                      */
/*--------------------------------------------------------------------------*/

static size_t VNCS_HextileTile(const unsigned char *pTile, int iW, int iH, int iBytes,
                               VncHextileState *pSt, unsigned char *pDst)
{
    unsigned long  arrKey[VNCS_TILE * VNCS_TILE];
    unsigned char  arrDone[VNCS_TILE * VNCS_TILE];
    unsigned long  arrColour[VNCS_HEXTILE_MAX_COLOURS];
    int            arrUses[VNCS_HEXTILE_MAX_COLOURS];
    unsigned char  arrForm[1 + 2 * 4 + 1 + RFB_HEXTILE_MAX_SUBRECTS * (4 + 2)];
    int            iPixels;
    int            iColours;
    int            iBest;
    int            i;
    int            j;
    int            iX;
    int            iY;
    int            iRunW;
    int            iRunH;
    int            bRowOk;
    int            bFits;
    int            iSubrects;
    int            bColoured;
    unsigned long  ulBg;
    unsigned long  ulFg;
    unsigned long  ulKey;
    unsigned char *pOut;
    unsigned char *pCount;
    size_t         ulRaw;
    size_t         ulSubrect;
    size_t         ulForm;

    if ((iW < 1) || (iW > VNCS_TILE) || (iH < 1) || (iH > VNCS_TILE) ||
        (iBytes < 1) || (iBytes > 4))
    {   /* Not a tile: VNCS_HextileRect never passes one, and every buffer */
        /* here is sized for 16x16 at 4 bytes, so write nothing at all.     */
        return(0);
    }

    /* The tile's size in pixels, and what it costs raw: the mask byte plus */
    /* every pixel -- the size the compact form has to beat.                */
    iPixels = iW * iH;
    ulRaw = 1 + (size_t)iPixels * (size_t)iBytes;

    /* STEP 1: a comparable key per pixel, and the tile's colours with how  */
    /* often each is used -- stopping at the first colour too many, since   */
    /* that tile can only go raw.                                            */
    iColours = 0;
    for (i = 0; i < iPixels; i++)
    {   /* One pixel per iteration. */
        ulKey = VNCS_HextileKey(pTile + (size_t)i * (size_t)iBytes, iBytes);
        arrKey[i] = ulKey;
        for (j = 0; j < iColours; j++)
        {   /* One colour seen so far per iteration; stop at a match. */
            if (arrColour[j] == ulKey)
            {   /* Seen before. */
                break;
            }
        }

        if (j < iColours)
        {   /* A known colour: one more use of it. */
            arrUses[j]++;
        }
        else if (iColours < VNCS_HEXTILE_MAX_COLOURS)
        {   /* A new colour, and room to record it. */
            arrColour[iColours] = ulKey;
            arrUses[iColours] = 1;
            iColours++;
        }
        else
        {   /* One colour too many: this tile goes raw. */
            iColours = VNCS_HEXTILE_MAX_COLOURS + 1;
            break;
        }
    }

    if (iColours <= VNCS_HEXTILE_MAX_COLOURS)
    {   /* Few enough colours: try the compact form in arrForm. */

        /* STEP 2: the background is the commonest colour.  With exactly two */
        /* colours the other one is the foreground, and the subrectangles    */
        /* need no colour of their own; with more they each carry one.       */
        iBest = 0;
        for (j = 1; j < iColours; j++)
        {   /* One colour per iteration: keep the most used. */
            if (arrUses[j] > arrUses[iBest])
            {   /* Used more than the best so far. */
                iBest = j;
            }
        }
        ulBg = arrColour[iBest];
        ulFg = 0;
        bColoured = (iColours > 2);
        if (iColours == 2)
        {   /* Two colours: the foreground is the one that is not background. */
            ulFg = arrColour[1 - iBest];
        }

        /* STEP 3: after the mask byte, whichever of background and         */
        /* foreground the decoder does not already hold.  arrForm[0] is the */
        /* mask, filled in once every part is decided.                      */
        arrForm[0] = 0;
        pOut = &arrForm[1];
        if (!pSt->bBgValid || (pSt->ulBg != ulBg))
        {   /* A background the decoder does not hold: send it. */
            arrForm[0] |= RFB_HEXTILE_BG_SPECIFIED;
            pOut = VNCS_HextilePutKey(pOut, ulBg, iBytes);
        }
        if ((iColours == 2) && (!pSt->bFgValid || (pSt->ulFg != ulFg)))
        {   /* Two colours, and a foreground the decoder does not hold. */
            arrForm[0] |= RFB_HEXTILE_FG_SPECIFIED;
            pOut = VNCS_HextilePutKey(pOut, ulFg, iBytes);
        }

        /* STEP 4: subrectangles covering every pixel that is not the       */
        /* background, abandoned as soon as the form would be no smaller    */
        /* than raw or the u8 count would overflow.                         */
        bFits = 1;
        if (iColours > 1)
        {   /* Something besides background: a count byte, then rectangles. */
            arrForm[0] |= RFB_HEXTILE_ANY_SUBRECTS;
            ulSubrect = 2;
            if (bColoured)
            {   /* Each rectangle leads with its own pixel value. */
                arrForm[0] |= RFB_HEXTILE_COLOURED;
                ulSubrect += (size_t)iBytes;
            }
            pCount = pOut;
            pOut++;
            iSubrects = 0;
            memset(arrDone, 0, (size_t)iPixels);

            for (iY = 0; (iY < iH) && bFits; iY++)
            {   /* One pixel row per iteration. */
                for (iX = 0; (iX < iW) && bFits; iX++)
                {   /* One pixel per iteration: start a rectangle if it needs one. */
                    i = iY * iW + iX;
                    if (arrDone[i] || (arrKey[i] == ulBg))
                    {   /* Background, or already inside a rectangle. */
                        continue;
                    }

                    /* This pixel's colour; the rectangle runs right while */
                    /* it holds and nothing is covered.                    */
                    ulKey = arrKey[i];
                    iRunW = 1;
                    while ((iX + iRunW < iW) && (arrKey[i + iRunW] == ulKey) && !arrDone[i + iRunW])
                    {   /* One more pixel joins the run. */
                        iRunW++;
                    }

                    /* Then down while every pixel under the run matches. */
                    iRunH = 1;
                    bRowOk = 1;
                    while ((iY + iRunH < iH) && bRowOk)
                    {   /* One candidate row below per iteration. */
                        for (j = 0; j < iRunW; j++)
                        {   /* One pixel of that row per iteration. */
                            if ((arrKey[(iY + iRunH) * iW + iX + j] != ulKey) ||
                                arrDone[(iY + iRunH) * iW + iX + j])
                            {   /* Another colour, or a covered pixel: growth stops. */
                                bRowOk = 0;
                                break;
                            }
                        }
                        if (bRowOk)
                        {   /* The whole row matched: the rectangle takes it. */
                            iRunH++;
                        }
                    }

                    if ((iSubrects == RFB_HEXTILE_MAX_SUBRECTS) ||
                        ((size_t)(pOut - arrForm) + ulSubrect >= ulRaw))
                    {   /* The count would overflow, or raw is no larger: give this form up. */
                        bFits = 0;
                        break;
                    }

                    /* Mark its pixels covered, then write its wire form:  */
                    /* [pixel,] x in the high nibble and y in the low, then */
                    /* width-1 and height-1 the same way.                   */
                    for (j = 0; j < iRunH; j++)
                    {   /* One covered row per iteration. */
                        memset(&arrDone[(iY + j) * iW + iX], 1, (size_t)iRunW);
                    }
                    if (bColoured)
                    {   /* This rectangle's own colour. */
                        pOut = VNCS_HextilePutKey(pOut, ulKey, iBytes);
                    }
                    pOut[0] = (unsigned char)((iX << 4) | iY);
                    pOut[1] = (unsigned char)(((iRunW - 1) << 4) | (iRunH - 1));
                    pOut += 2;
                    iSubrects++;
                }
            }

            /* The count, known only now, into the byte kept for it. */
            *pCount = (unsigned char)iSubrects;
        }

        /* STEP 5: take the compact form if it beat raw, and record what   */
        /* the decoder now holds: the background always; the foreground if */
        /* one was sent in a two-colour tile; after coloured subrectangles */
        /* the foreground is treated as unknown, to stay safe with every    */
        /* decoder.                                                        */
        ulForm = (size_t)(pOut - arrForm);
        if (bFits && (ulForm < ulRaw))
        {   /* Smaller than raw: this is the tile. */
            memcpy(pDst, arrForm, ulForm);
            pSt->bBgValid = 1;
            pSt->ulBg = ulBg;
            if (iColours == 2)
            {   /* The decoder now holds this foreground. */
                pSt->bFgValid = 1;
                pSt->ulFg = ulFg;
            }
            else if (bColoured)
            {   /* Not relied on after a coloured tile. */
                pSt->bFgValid = 0;
            }
            return(ulForm);
        }
    }

    /* Raw: the mask and the pixels as they are.  What the decoder holds is */
    /* not relied on after a raw tile, so the next tile sends both again.   */
    pDst[0] = RFB_HEXTILE_RAW;
    memcpy(&pDst[1], pTile, ulRaw - 1);
    pSt->bBgValid = 0;
    pSt->bFgValid = 0;
    return(ulRaw);
}

/*--------------------------------------------------------------------------*/
/* VNCS_HextileRect:                                                        */
/*                                                                          */
/* Encode one update rectangle as Hextile: split it into 16x16 tiles, left  */
/* to right and top to bottom, the last column and row possibly smaller     */
/* (RFC 6143 section 7.7.4), convert each tile's pixels from the published   */
/* buffer to the client's format, and hand it to VNCS_HextileTile.  The      */
/* background / foreground memory starts empty for every rectangle.  Caller */
/* holds the mutex (the pixels are the published buffer's).                 */
/*                                                                          */
/* Arguments:                                                               */
/*     pS   : the server (published buffer, blank state).                   */
/*     pC   : the client (its pixel format).                                */
/*     pR   : the rectangle, inside the framebuffer.                        */
/*     pDst : where the encoded tiles go; room for the raw size plus one    */
/*            byte per tile.                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     size_t : bytes written.                                              */
/*--------------------------------------------------------------------------*/

static size_t VNCS_HextileRect(const VncServer *pS, const VncClient *pC, const VncRect *pR,
                               unsigned char *pDst)
{
    unsigned char   arrTile[VNCS_TILE * VNCS_TILE * 4];
    VncHextileState st;
    int             iTx;
    int             iTy;
    int             iTw;
    int             iTh;
    int             iRow;
    int             iBytes;
    unsigned char  *pOut;

    /* A new rectangle: the decoder starts knowing no background or */
    /* foreground, and nothing is written yet.                     */
    iBytes = pC->fmt.iBpp / 8;
    memset(&st, 0, sizeof(st));
    pOut = pDst;

    for (iTy = 0; iTy < pR->iH; iTy += VNCS_TILE)
    {   /* One row of tiles per iteration, top to bottom. */
        iTh = pR->iH - iTy;
        if (iTh > VNCS_TILE)
        {   /* A full-height tile row. */
            iTh = VNCS_TILE;
        }

        for (iTx = 0; iTx < pR->iW; iTx += VNCS_TILE)
        {   /* One tile per iteration, left to right. */
            iTw = pR->iW - iTx;
            if (iTw > VNCS_TILE)
            {   /* A full-width tile. */
                iTw = VNCS_TILE;
            }

            /* The tile's rows, converted into packed client pixels. */
            for (iRow = 0; iRow < iTh; iRow++)
            {   /* One pixel row of the tile per iteration. */
                VNCS_ConvertRow(pS, &pC->fmt,
                                pS->pPub + (size_t)(pR->iY + iTy + iRow) * (size_t)pS->iStride +
                                    (size_t)(pR->iX + iTx) * (size_t)pS->iBytesPP,
                                &arrTile[(size_t)iRow * (size_t)iTw * (size_t)iBytes],
                                iTw, pS->bBlank);
            }

            pOut += VNCS_HextileTile(arrTile, iTw, iTh, iBytes, &st, pOut);
        }
    }

    /* Everything the rectangle's tiles took. */
    return((size_t)(pOut - pDst));
}

/*--------------------------------------------------------------------------*/
/* Building and sending updates (network thread only).                       */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* VNCS_BuildUpdate:                                                        */
/*                                                                          */
/* Build one FramebufferUpdate for a client with an outstanding request.    */
/* CALLER HOLDS THE MUTEX, because the pixels come from the published       */
/* buffer and the dirty map is shared with the commit path; the message is  */
/* built completely here so the caller can release the mutex BEFORE it      */
/* sends -- a slow client must never stall a producer.                      */
/*                                                                          */
/* A non-incremental request gets its whole region, dirty or not (RFC 6143   */
/* section 7.5.3 -- the client's idle probe depends on this).  An            */
/* incremental one gets the dirty tiles inside its region, merged into one   */
/* rectangle per horizontal run of tiles; a tile is only marked clean if it  */
/* lies wholly inside the region, so a partly requested tile is sent again   */
/* in full later.  If nothing is dirty the request stays outstanding.        */
/*                                                                          */
/* Each rectangle goes out in the client's chosen encoding, Raw or Hextile.  */
/* The buffer is sized for the worst case (Raw, plus one mask byte per       */
/* Hextile tile); the message length is what was actually written.          */
/*                                                                          */
/* Arguments:                                                               */
/*     pS     : the server.                                                 */
/*     pC     : the client.                                                 */
/*     pulLen : out; the message length when one is built.                  */
/*                                                                          */
/* Returns:                                                                 */
/*     int : 1 when a message is ready in pC->pSend, 0 when there is        */
/*           nothing to send yet, -1 when memory ran out.                   */
/*--------------------------------------------------------------------------*/

static int VNCS_BuildUpdate(VncServer *pS, VncClient *pC, size_t *pulLen)
{
    int            iTx;
    int            iTy;
    int            iTx0;
    int            iTx1;
    int            iRun;
    int            iRects;
    int            iRow;
    int            iOutBytes;
    size_t         ulNeed;
    unsigned char *pNew;
    unsigned char *pDst;
    VncRect        rc;
    VncRect        rcTile;

    iRects = 0;

    if (!pC->bIncremental)
    {   /* Non-incremental: the whole requested region, now. */
        if ((pC->rcReq.iW > 0) && (pC->rcReq.iH > 0))
        {   /* A real region: one rectangle covering it. */
            pC->pRects[0] = pC->rcReq;
            iRects = 1;
        }
    }
    else
    {   /* Incremental: collect runs of dirty tiles that meet the region. */
        for (iTy = 0; iTy < pS->iTilesY; iTy++)
        {   /* One tile row per iteration. */
            iTx = 0;
            while (iTx < pS->iTilesX)
            {   /* One run of dirty tiles (or one clean tile) per iteration. */
                if (!pC->pDirty[iTy * pS->iTilesX + iTx])
                {   /* Clean: step past it. */
                    iTx++;
                    continue;
                }

                /* Extend the run over every consecutive dirty tile. */
                iTx0 = iTx;
                while ((iTx < pS->iTilesX) && pC->pDirty[iTy * pS->iTilesX + iTx])
                {   /* One more dirty tile in the run. */
                    iTx++;
                }
                iTx1 = iTx - 1;

                /* The run as a rectangle, clipped to the framebuffer, then to the request. */
                rc.iX = iTx0 * VNCS_TILE;
                rc.iY = iTy * VNCS_TILE;
                rc.iW = (iTx1 - iTx0 + 1) * VNCS_TILE;
                rc.iH = VNCS_TILE;
                VNCS_ClipRect(pS, &rc);
                if (rc.iX < pC->rcReq.iX)
                {   /* Trim the part left of the request. */
                    rc.iW -= pC->rcReq.iX - rc.iX;
                    rc.iX = pC->rcReq.iX;
                }
                if (rc.iY < pC->rcReq.iY)
                {   /* Trim the part above the request. */
                    rc.iH -= pC->rcReq.iY - rc.iY;
                    rc.iY = pC->rcReq.iY;
                }
                if (rc.iX + rc.iW > pC->rcReq.iX + pC->rcReq.iW)
                {   /* Trim the part right of the request. */
                    rc.iW = pC->rcReq.iX + pC->rcReq.iW - rc.iX;
                }
                if (rc.iY + rc.iH > pC->rcReq.iY + pC->rcReq.iH)
                {   /* Trim the part below the request. */
                    rc.iH = pC->rcReq.iY + pC->rcReq.iH - rc.iY;
                }
                if ((rc.iW <= 0) || (rc.iH <= 0))
                {   /* The run lies outside the request: leave it dirty for later. */
                    continue;
                }

                if (iRects >= RFB_MAX_RECTS)
                {   /* The count field is a u16; the rest waits for the next request. */
                    break;
                }
                pC->pRects[iRects] = rc;
                iRects++;

                /* Mark clean only the tiles the request wholly covers. */
                for (iRun = iTx0; iRun <= iTx1; iRun++)
                {   /* One tile of the run per iteration. */
                    rcTile.iX = iRun * VNCS_TILE;
                    rcTile.iY = iTy * VNCS_TILE;
                    rcTile.iW = VNCS_TILE;
                    rcTile.iH = VNCS_TILE;
                    VNCS_ClipRect(pS, &rcTile);
                    if ((rcTile.iX >= pC->rcReq.iX) && (rcTile.iY >= pC->rcReq.iY) &&
                        (rcTile.iX + rcTile.iW <= pC->rcReq.iX + pC->rcReq.iW) &&
                        (rcTile.iY + rcTile.iH <= pC->rcReq.iY + pC->rcReq.iH))
                    {   /* Wholly inside: this update carries all of it. */
                        pC->pDirty[iTy * pS->iTilesX + iRun] = 0;
                    }
                }
            }
        }

        if (iRects == 0)
        {   /* Nothing new inside the region: keep the request waiting. */
            return(0);
        }
    }

    /* Size the message for the worst case: header, then per rectangle a    */
    /* header and its pixels as Raw -- plus, for Hextile, one mask byte per  */
    /* tile, since a Hextile tile is never more than one byte over Raw.     */
    iOutBytes = pC->fmt.iBpp / 8;
    ulNeed = RFB_UPDATE_HDR_LEN;
    for (iRun = 0; iRun < iRects; iRun++)
    {   /* One rectangle per iteration. */
        ulNeed += RFB_RECT_HDR_LEN +
                  (size_t)pC->pRects[iRun].iW * (size_t)pC->pRects[iRun].iH * (size_t)iOutBytes;
        if (pC->iEncoding == RFB_ENC_HEXTILE)
        {   /* Hextile: the per-tile mask bytes on top. */
            ulNeed += (size_t)((pC->pRects[iRun].iW + VNCS_TILE - 1) / VNCS_TILE) *
                      (size_t)((pC->pRects[iRun].iH + VNCS_TILE - 1) / VNCS_TILE);
        }
    }

    if (ulNeed > pC->ulSendCap)
    {   /* Grow the client's send buffer to fit. */
        pNew = (unsigned char *)realloc(pC->pSend, ulNeed);
        if (pNew == NULL)
        {   /* Out of memory: the caller drops the client. */
            return(-1);
        }
        pC->pSend = pNew;
        pC->ulSendCap = ulNeed;
    }

    /* FramebufferUpdate header: type, padding, rectangle count. */
    pDst = pC->pSend;
    pDst[0] = RFB_SMSG_FBUPDATE;
    pDst[1] = 0;
    VNCS_Put16(&pDst[2], (unsigned int)iRects);
    pDst += RFB_UPDATE_HDR_LEN;

    for (iRun = 0; iRun < iRects; iRun++)
    {   /* One rectangle per iteration: its header, then its pixels in the client's encoding. */
        rc = pC->pRects[iRun];
        VNCS_Put16(&pDst[0], (unsigned int)rc.iX);
        VNCS_Put16(&pDst[2], (unsigned int)rc.iY);
        VNCS_Put16(&pDst[4], (unsigned int)rc.iW);
        VNCS_Put16(&pDst[6], (unsigned int)rc.iH);
        VNCS_Put32(&pDst[8], (unsigned long)pC->iEncoding);
        pDst += RFB_RECT_HDR_LEN;

        if (pC->iEncoding == RFB_ENC_HEXTILE)
        {   /* Hextile: the encoder picks each tile's form. */
            pDst += VNCS_HextileRect(pS, pC, &rc, pDst);
        }
        else
        {   /* Raw: the rows as they are. */
            for (iRow = 0; iRow < rc.iH; iRow++)
            {   /* One pixel row per iteration, read from the PUBLISHED buffer. */
                VNCS_ConvertRow(pS, &pC->fmt,
                                pS->pPub + (size_t)(rc.iY + iRow) * (size_t)pS->iStride +
                                    (size_t)rc.iX * (size_t)pS->iBytesPP,
                                pDst, rc.iW, pS->bBlank);
                pDst += (size_t)rc.iW * (size_t)iOutBytes;
            }
        }
    }

    /* A complete message is ready: its length is what was written, which */
    /* for Hextile is usually far less than the space reserved.            */
    *pulLen = (size_t)(pDst - pC->pSend);
    return(1);
}

/*--------------------------------------------------------------------------*/
/* VNCS_ServiceClient:                                                      */
/*                                                                          */
/* Answer a client's outstanding request if there is anything to send.  The  */
/* message is built under the mutex and sent after it is released, which is  */
/* the rule that keeps a slow client from ever stalling a producer.          */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*     pC : a fully connected client.                                       */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; a failed send drops the client.                      */
/*--------------------------------------------------------------------------*/

static void VNCS_ServiceClient(VncServer *pS, VncClient *pC)
{
    int    iBuilt;
    size_t ulLen;

    if (!pC->bRequest)
    {   /* The client has not asked for anything. */
        return;
    }

    /* Build under the mutex. */
    ulLen = 0;
    pthread_mutex_lock(&pS->mtxLock);
    iBuilt = VNCS_BuildUpdate(pS, pC, &ulLen);
    pthread_mutex_unlock(&pS->mtxLock);

    if (iBuilt < 0)
    {   /* Could not build: this client cannot be served. */
        VNCS_Log(pS, "client dropped: out of memory building an update");
        VNCS_ClientDrop(pS, pC);
        return;
    }

    if (iBuilt == 0)
    {   /* Nothing new: the request stays outstanding until a commit. */
        return;
    }

    /* Answered: the next update waits for the client's next request.  Then */
    /* send, outside the mutex.                                              */
    pC->bRequest = 0;
    if (VNCS_SendAll(pS, pC->iFd, pC->pSend, ulLen) != VNCS_SUCCESS)
    {   /* The client stopped taking data. */
        VNCS_ClientDrop(pS, pC);
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_NetThread:                                                          */
/*                                                                          */
/* The network thread: one control loop.  Each pass polls the wake pipe,    */
/* the listener and every client; drains the wake pipe; accepts a new       */
/* client; reads from each readable client; then services every connected   */
/* client's outstanding request.  It sleeps in poll() with no timeout, so an */
/* idle server costs nothing -- a commit or a stop writes to the wake pipe.  */
/*                                                                          */
/* Arguments:                                                               */
/*     pvArg : the VncServer.                                               */
/*                                                                          */
/* Returns:                                                                 */
/*     void * : NULL; the thread's result is not used.                      */
/*--------------------------------------------------------------------------*/

static void *VNCS_NetThread(void *pvArg)
{
    VncServer     *pS;
    struct pollfd *pFds;
    int           *pSlot;
    int            iN;
    int            i;
    int            iK;
    int            iRc;
    unsigned char  arrDrain[64];
    ssize_t        lGot;

    pS = (VncServer *)pvArg;

    /* One poll entry each for the wake pipe and the listener, plus one per slot. */
    pFds = (struct pollfd *)calloc((size_t)(pS->iMaxClients + 2), sizeof(struct pollfd));
    pSlot = (int *)calloc((size_t)(pS->iMaxClients + 2), sizeof(int));
    if ((pFds == NULL) || (pSlot == NULL))
    {   /* Cannot run the loop at all. */
        VNCS_Log(pS, "network thread: out of memory");
        free(pFds);
        free(pSlot);
        return(NULL);
    }

    while (!pS->bStop)
    {   /* One pass of the control loop per iteration. */

        /* Build the poll set: wake pipe, listener, then every live client. */
        iN = 0;
        pFds[iN].fd = pS->arrWake[0];
        pFds[iN].events = POLLIN;
        iN++;
        pFds[iN].fd = pS->iListenFd;
        pFds[iN].events = POLLIN;
        iN++;
        for (i = 0; i < pS->iMaxClients; i++)
        {   /* One client slot per iteration. */
            if (pS->arrClients[i].eState != VNCC_FREE)
            {   /* Live: watch it for input and for hang-up. */
                pFds[iN].fd = pS->arrClients[i].iFd;
                pFds[iN].events = POLLIN;
                pSlot[iN] = i;
                iN++;
            }
        }

        iRc = poll(pFds, (nfds_t)iN, -1);
        if (iRc < 0)
        {   /* A signal is harmless; anything else ends the loop. */
            if (errno == EINTR)
            {   /* Interrupted: poll again. */
                continue;
            }
            VNCS_Log(pS, "network thread: poll failed: %s", strerror(errno));
            break;
        }

        if (pFds[0].revents != 0)
        {   /* Woken: drain the pipe so the next poll sleeps again. */
            do
            {   /* Read until the pipe is empty (it is non-blocking). */
                lGot = read(pS->arrWake[0], arrDrain, sizeof(arrDrain));
            } while (lGot > 0);
        }

        if (pS->bStop)
        {   /* VNCS_Stop asked us to finish. */
            break;
        }

        if (pFds[1].revents & POLLIN)
        {   /* A client is connecting. */
            VNCS_Accept(pS);
        }

        for (iK = 2; iK < iN; iK++)
        {   /* One polled client per iteration. */
            if (pFds[iK].revents & (POLLIN | POLLHUP | POLLERR))
            {   /* Data, a hang-up or an error: the read tells which. */
                if (pS->arrClients[pSlot[iK]].eState != VNCC_FREE)
                {   /* Still live (an earlier step this pass may have dropped it). */
                    VNCS_ClientRead(pS, &pS->arrClients[pSlot[iK]]);
                }
            }
        }

        for (i = 0; i < pS->iMaxClients; i++)
        {   /* One client slot per iteration: answer what can be answered. */
            if (pS->arrClients[i].eState == VNCC_NORMAL)
            {   /* Fully connected: it may have a request waiting. */
                VNCS_ServiceClient(pS, &pS->arrClients[i]);
            }
        }
    }

    /* Leaving: close every client. */
    for (i = 0; i < pS->iMaxClients; i++)
    {   /* One client slot per iteration. */
        if (pS->arrClients[i].eState != VNCC_FREE)
        {   /* Live: close it. */
            VNCS_ClientDrop(pS, &pS->arrClients[i]);
        }
    }

    free(pFds);
    free(pSlot);

    /* The thread's result is not used. */
    return(NULL);
}

/*--------------------------------------------------------------------------*/
/* Public interface.  Each function's contract is in vnc_server.h; these     */
/* headers say how it is carried out.                                       */
/*--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------*/
/* VNCS_Create:                                                             */
/*                                                                          */
/* Validate the configuration, then allocate the server, both framebuffers  */
/* (cleared to opaque black), the pending tile map and the client slots.    */
/* Refuses VNC_AUTH_VNC: password authentication is not built in this        */
/* version, and serving without it would silently drop a protection the      */
/* caller asked for.                                                        */
/*                                                                          */
/* Arguments:                                                               */
/*     pCfg : the configuration (see vnc_server.h).                         */
/*                                                                          */
/* Returns:                                                                 */
/*     VncServer * : the server, or NULL (reason reported, nothing kept).    */
/*--------------------------------------------------------------------------*/

VncServer *VNCS_Create(const VncServerCfg *pCfg)
{
    VncServer *pS;
    char       arrMsg[VNCS_LOG_LINE];
    size_t     ulBytes;
    size_t     ulPix;
    int        i;

    if (pCfg == NULL)
    {   /* Nothing to build from, and no sink to report through. */
        VNCS_Emit(NULL, NULL, "VNCS_Create: no configuration");
        return(NULL);
    }

    if ((pCfg->iWidth < 1) || (pCfg->iHeight < 1) ||
        (pCfg->iWidth > 65535) || (pCfg->iHeight > 65535))
    {   /* RFB sizes are u16 and must be positive. */
        snprintf(arrMsg, sizeof(arrMsg), "VNCS_Create: invalid size %dx%d",
                 pCfg->iWidth, pCfg->iHeight);
        VNCS_Emit(pCfg->pfnLog, pCfg->pLogUser, arrMsg);
        return(NULL);
    }

    if ((pCfg->ePixFmt != VNC_PIXFMT_BGRA8888) && (pCfg->ePixFmt != VNC_PIXFMT_RGB565))
    {   /* Only the two documented producer formats are handled. */
        VNCS_Emit(pCfg->pfnLog, pCfg->pLogUser, "VNCS_Create: unsupported pixel format");
        return(NULL);
    }

    /* Security type: None, or VNC Authentication when a password is set.     */
    /* VNC_AUTH_VNC needs a non-empty password; any other type, or VNC with   */
    /* no password, is refused so the server never serves without the         */
    /* protection the caller asked for.  The password's first 8 bytes are     */
    /* copied into the server below with the rest of the configuration.       */
    if ((pCfg->eAuth != VNC_AUTH_NONE) && (pCfg->eAuth != VNC_AUTH_VNC))
    {   /* An authentication type this build does not implement. */
        VNCS_Emit(pCfg->pfnLog, pCfg->pLogUser,
                  "VNCS_Create: unknown authentication type");
        return(NULL);
    }
    if ((pCfg->eAuth == VNC_AUTH_VNC) &&
        ((pCfg->pcPassword == NULL) || (pCfg->pcPassword[0] == '\0')))
    {   /* VNC authentication was asked for with no password to check against. */
        VNCS_Emit(pCfg->pfnLog, pCfg->pLogUser,
                  "VNCS_Create: VNC_AUTH_VNC needs a non-empty password");
        return(NULL);
    }

    pS = (VncServer *)calloc(1, sizeof(VncServer));
    if (pS == NULL)
    {   /* Out of memory before anything else exists. */
        VNCS_Emit(pCfg->pfnLog, pCfg->pLogUser, "VNCS_Create: out of memory");
        return(NULL);
    }

    /* Copy the configuration. */
    pS->iWidth = pCfg->iWidth;
    pS->iHeight = pCfg->iHeight;
    pS->ePixFmt = pCfg->ePixFmt;
    pS->pfnInput = pCfg->pfnInput;
    pS->pInputUser = pCfg->pInputUser;
    pS->pfnLog = pCfg->pfnLog;
    pS->pLogUser = pCfg->pLogUser;
    pS->bNoHextile = (pCfg->bNoHextile != 0);  // any non-zero value turns Hextile off
    pS->eAuth = pCfg->eAuth;
    if (pCfg->eAuth == VNC_AUTH_VNC)
    {   /* Keep the first 8 password bytes as the DES key material; calloc     */
        /* zeroed the rest, so a shorter password is NUL-padded to the length  */
        /* the VNC scheme uses.                                                */
        for (i = 0; i < (int)sizeof(pS->arrPassword); i++)
        {   /* One key byte per iteration, stopping at the password's NUL. */
            if (pCfg->pcPassword[i] == '\0')
            {   /* End of the password: the rest stays the calloc'd NUL. */
                break;
            }
            pS->arrPassword[i] = (unsigned char)pCfg->pcPassword[i];
        }
    }
    pS->iListenFd = -1;
    pS->arrWake[0] = -1;
    pS->arrWake[1] = -1;

    if (pCfg->iPort > 0)
    {   /* The caller chose a port. */
        pS->iPort = pCfg->iPort;
    }
    else
    {   /* Default: RFB display :0. */
        pS->iPort = VNCS_DEFAULT_PORT;
    }

    if (pCfg->iMaxClients > 0)
    {   /* The caller chose a client limit. */
        pS->iMaxClients = pCfg->iMaxClients;
    }
    else
    {   /* Default: a panel-side client plus one viewer. */
        pS->iMaxClients = VNCS_DEFAULT_MAX_CLIENTS;
    }

    if (pCfg->ePixFmt == VNC_PIXFMT_RGB565)
    {   /* Two bytes per producer pixel. */
        pS->iBytesPP = 2;
    }
    else
    {   /* Four bytes per producer pixel. */
        pS->iBytesPP = 4;
    }
    pS->iStride = pS->iWidth * pS->iBytesPP;
    VNCS_NativeFormat(pS->ePixFmt, &pS->fmtNative);

    /* The tile grid covers the screen, the last row and column possibly partial. */
    pS->iTilesX = (pS->iWidth + VNCS_TILE - 1) / VNCS_TILE;
    pS->iTilesY = (pS->iHeight + VNCS_TILE - 1) / VNCS_TILE;

    /* Buffers, maps and slots.  calloc zeroes, which is also black for RGB565. */
    ulPix = (size_t)pS->iWidth * (size_t)pS->iHeight;
    ulBytes = ulPix * (size_t)pS->iBytesPP;
    pS->pWork = (unsigned char *)calloc(ulBytes, 1);
    pS->pPub = (unsigned char *)calloc(ulBytes, 1);
    pS->pPending = (unsigned char *)calloc((size_t)(pS->iTilesX * pS->iTilesY), 1);
    pS->arrClients = (VncClient *)calloc((size_t)pS->iMaxClients, sizeof(VncClient));
    if (pCfg->pcListenAddr != NULL)
    {   /* Keep our own copy of the bind address. */
        pS->pcListenAddr = strdup(pCfg->pcListenAddr);
    }

    if ((pS->pWork == NULL) || (pS->pPub == NULL) || (pS->pPending == NULL) ||
        (pS->arrClients == NULL) ||
        ((pCfg->pcListenAddr != NULL) && (pS->pcListenAddr == NULL)))
    {   /* Any allocation failed: release what did succeed. */
        VNCS_Emit(pCfg->pfnLog, pCfg->pLogUser, "VNCS_Create: out of memory");
        free(pS->pWork);
        free(pS->pPub);
        free(pS->pPending);
        free(pS->arrClients);
        free(pS->pcListenAddr);
        free(pS);
        return(NULL);
    }

    if (pS->ePixFmt == VNC_PIXFMT_BGRA8888)
    {   /* Opaque black for BGRA: colour bytes zero (from calloc), alpha 0xFF. */
        for (i = 0; i < (int)ulPix; i++)
        {   /* One pixel's alpha byte per iteration, in both buffers. */
            pS->pWork[(size_t)i * 4 + 3] = 0xFF;
            pS->pPub[(size_t)i * 4 + 3] = 0xFF;
        }
    }

    for (i = 0; i < pS->iMaxClients; i++)
    {   /* One slot per iteration: free, with no socket. */
        pS->arrClients[i].eState = VNCC_FREE;
        pS->arrClients[i].iFd = -1;
    }

    pthread_mutex_init(&pS->mtxLock, NULL);

    /* A server ready to be started. */
    return(pS);
}

/*--------------------------------------------------------------------------*/
/* VNCS_Start:                                                              */
/*                                                                          */
/* Bind and listen on the configured address and port, create the wake       */
/* pipe, and start the network thread.  SO_REUSEADDR lets a restarted host  */
/* rebind at once instead of waiting out TIME_WAIT from its last run.        */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     int : VNCS_SUCCESS once listening, VNCS_FAILURE otherwise (logged;   */
/*           anything opened is closed again).                              */
/*--------------------------------------------------------------------------*/

int VNCS_Start(VncServer *pS)
{
    struct sockaddr_in sinAddr;
    int                iOne;

    if (pS == NULL)
    {   /* Nothing to start. */
        return(VNCS_FAILURE);
    }

    if (pS->bStarted)
    {   /* A second start is a caller error and changes nothing. */
        VNCS_Log(pS, "VNCS_Start: already started");
        return(VNCS_FAILURE);
    }

    /* The address to bind: a specific IPv4 address, or every interface. */
    memset(&sinAddr, 0, sizeof(sinAddr));
    sinAddr.sin_family = AF_INET;
    sinAddr.sin_port = htons((unsigned short)pS->iPort);
    if (pS->pcListenAddr != NULL)
    {   /* The caller named an address. */
        if (inet_pton(AF_INET, pS->pcListenAddr, &sinAddr.sin_addr) != 1)
        {   /* Not a dotted IPv4 address. */
            VNCS_Log(pS, "VNCS_Start: bad listen address '%s'", pS->pcListenAddr);
            return(VNCS_FAILURE);
        }
    }
    else
    {   /* Every interface. */
        sinAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    pS->iListenFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (pS->iListenFd < 0)
    {   /* No socket at all. */
        VNCS_Log(pS, "VNCS_Start: socket failed: %s", strerror(errno));
        return(VNCS_FAILURE);
    }

    iOne = 1;
    setsockopt(pS->iListenFd, SOL_SOCKET, SO_REUSEADDR, &iOne, sizeof(iOne));

    if ((bind(pS->iListenFd, (struct sockaddr *)&sinAddr, sizeof(sinAddr)) != 0) ||
        (listen(pS->iListenFd, VNCS_LISTEN_BACKLOG) != 0))
    {   /* Port in use, no permission, or the address is not local. */
        VNCS_Log(pS, "VNCS_Start: cannot listen on port %d: %s", pS->iPort, strerror(errno));
        close(pS->iListenFd);
        pS->iListenFd = -1;
        return(VNCS_FAILURE);
    }

    if (pipe2(pS->arrWake, O_NONBLOCK | O_CLOEXEC) != 0)
    {   /* No wake pipe: commits could not reach the thread. */
        VNCS_Log(pS, "VNCS_Start: pipe failed: %s", strerror(errno));
        close(pS->iListenFd);
        pS->iListenFd = -1;
        return(VNCS_FAILURE);
    }

    pS->bStop = 0;
    if (pthread_create(&pS->thrNet, NULL, VNCS_NetThread, pS) != 0)
    {   /* No thread: undo everything opened above. */
        VNCS_Log(pS, "VNCS_Start: cannot start the network thread");
        close(pS->arrWake[0]);
        close(pS->arrWake[1]);
        pS->arrWake[0] = -1;
        pS->arrWake[1] = -1;
        close(pS->iListenFd);
        pS->iListenFd = -1;
        return(VNCS_FAILURE);
    }
    pS->bStarted = 1;

    /* Listening, with the network thread running. */
    return(VNCS_SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* VNCS_SubmitRect:                                                         */
/*                                                                          */
/* Clip the rectangle (moving the source origin with it), copy its rows into */
/* the working buffer and mark its tiles pending, all under the mutex.       */
/*                                                                          */
/* Arguments:                                                               */
/*     pS              : the server.                                        */
/*     iX, iY, iW, iH  : the rectangle in framebuffer pixels.                */
/*     pvPixels        : its pixels, row-major, in the producer format.      */
/*     iSrcStrideBytes : bytes between the starts of consecutive rows.       */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void VNCS_SubmitRect(VncServer *pS, int iX, int iY, int iW, int iH,
                     const void *pvPixels, int iSrcStrideBytes)
{
    VncRect              rc;
    int                  iSrcX;
    int                  iSrcY;
    int                  iRow;
    const unsigned char *pSrc;

    if ((pS == NULL) || (pvPixels == NULL))
    {   /* Nothing to copy into or from. */
        return;
    }

    /* Clip, remembering how far the origin moved so the source moves too. */
    rc.iX = iX;
    rc.iY = iY;
    rc.iW = iW;
    rc.iH = iH;
    if (!VNCS_ClipRect(pS, &rc))
    {   /* Entirely off-screen: nothing to do. */
        return;
    }
    iSrcX = rc.iX - iX;
    iSrcY = rc.iY - iY;
    pSrc = (const unsigned char *)pvPixels;

    pthread_mutex_lock(&pS->mtxLock);
    for (iRow = 0; iRow < rc.iH; iRow++)
    {   /* One row per iteration into the WORKING buffer. */
        memcpy(pS->pWork + (size_t)(rc.iY + iRow) * (size_t)pS->iStride +
                   (size_t)rc.iX * (size_t)pS->iBytesPP,
               pSrc + (size_t)(iSrcY + iRow) * (size_t)iSrcStrideBytes +
                   (size_t)iSrcX * (size_t)pS->iBytesPP,
               (size_t)rc.iW * (size_t)pS->iBytesPP);
    }
    VNCS_MarkTiles(pS, pS->pPending, &rc);
    pthread_mutex_unlock(&pS->mtxLock);
}

/*--------------------------------------------------------------------------*/
/* VNCS_Framebuffer:                                                        */
/*                                                                          */
/* Hand out the working buffer for the in-place model.                      */
/*                                                                          */
/* Arguments:                                                               */
/*     pS            : the server.                                          */
/*     piStrideBytes : out (may be NULL); bytes per row.                    */
/*                                                                          */
/* Returns:                                                                 */
/*     unsigned char * : the working buffer, or NULL if pS is NULL.          */
/*--------------------------------------------------------------------------*/

unsigned char *VNCS_Framebuffer(VncServer *pS, int *piStrideBytes)
{
    if (pS == NULL)
    {   /* No server, no buffer. */
        return(NULL);
    }

    if (piStrideBytes != NULL)
    {   /* The caller wants the row stride too. */
        *piStrideBytes = pS->iStride;
    }

    /* The producer renders into the working buffer and commits. */
    return(pS->pWork);
}

/*--------------------------------------------------------------------------*/
/* VNCS_MarkDirty:                                                          */
/*                                                                          */
/* Clip the rectangle and mark its tiles pending, under the mutex.          */
/*                                                                          */
/* Arguments:                                                               */
/*     pS             : the server.                                         */
/*     iX, iY, iW, iH : the changed rectangle in framebuffer pixels.         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void VNCS_MarkDirty(VncServer *pS, int iX, int iY, int iW, int iH)
{
    VncRect rc;

    if (pS == NULL)
    {   /* No server. */
        return;
    }

    rc.iX = iX;
    rc.iY = iY;
    rc.iW = iW;
    rc.iH = iH;
    if (!VNCS_ClipRect(pS, &rc))
    {   /* Entirely off-screen. */
        return;
    }

    pthread_mutex_lock(&pS->mtxLock);
    VNCS_MarkTiles(pS, pS->pPending, &rc);
    pthread_mutex_unlock(&pS->mtxLock);
}

/*--------------------------------------------------------------------------*/
/* VNCS_CommitFrame:                                                        */
/*                                                                          */
/* The tile diff.  For each pending tile, compare it row by row between the  */
/* working and published buffers, stopping at the first differing row; only */
/* a tile that differs is copied across and marked dirty for the clients.   */
/* While blanked, the published buffer still follows the producer (so       */
/* unblanking shows the current frame) but no client is marked, since all   */
/* they would be sent is more black.                                        */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void VNCS_CommitFrame(VncServer *pS)
{
    int     iTx;
    int     iTy;
    int     iRow;
    int     iChanged;
    int     iAny;
    int     i;
    size_t  ulOff;
    size_t  ulRowBytes;
    VncRect rc;

    if (pS == NULL)
    {   /* No server. */
        return;
    }

    iAny = 0;
    pthread_mutex_lock(&pS->mtxLock);
    for (iTy = 0; iTy < pS->iTilesY; iTy++)
    {   /* One tile row per iteration. */
        for (iTx = 0; iTx < pS->iTilesX; iTx++)
        {   /* One tile per iteration. */
            if (!pS->pPending[iTy * pS->iTilesX + iTx])
            {   /* Not written since the last commit. */
                continue;
            }
            pS->pPending[iTy * pS->iTilesX + iTx] = 0;

            /* The tile's area; the last row and column of tiles may be partial. */
            rc.iX = iTx * VNCS_TILE;
            rc.iY = iTy * VNCS_TILE;
            rc.iW = VNCS_TILE;
            rc.iH = VNCS_TILE;
            VNCS_ClipRect(pS, &rc);
            ulRowBytes = (size_t)rc.iW * (size_t)pS->iBytesPP;

            /* Did any row actually change?  Stop at the first that did. */
            iChanged = 0;
            for (iRow = 0; iRow < rc.iH; iRow++)
            {   /* One row of the tile per iteration. */
                ulOff = (size_t)(rc.iY + iRow) * (size_t)pS->iStride +
                        (size_t)rc.iX * (size_t)pS->iBytesPP;
                if (memcmp(pS->pWork + ulOff, pS->pPub + ulOff, ulRowBytes) != 0)
                {   /* A difference: the tile has to go out. */
                    iChanged = 1;
                    break;
                }
            }

            if (!iChanged)
            {   /* Redrawn to identical pixels: nothing to publish or send. */
                continue;
            }

            for (iRow = 0; iRow < rc.iH; iRow++)
            {   /* One row of the tile per iteration: publish it. */
                ulOff = (size_t)(rc.iY + iRow) * (size_t)pS->iStride +
                        (size_t)rc.iX * (size_t)pS->iBytesPP;
                memcpy(pS->pPub + ulOff, pS->pWork + ulOff, ulRowBytes);
            }

            if (!pS->bBlank)
            {   /* Visible: every connected client needs this tile. */
                for (i = 0; i < pS->iMaxClients; i++)
                {   /* One client slot per iteration. */
                    if (pS->arrClients[i].eState == VNCC_NORMAL)
                    {   /* Connected: mark the tile for it. */
                        pS->arrClients[i].pDirty[iTy * pS->iTilesX + iTx] = 1;
                    }
                }
                iAny = 1;
            }
        }
    }
    pthread_mutex_unlock(&pS->mtxLock);

    if (iAny)
    {   /* Something for the clients: wake the network thread to answer waiting requests. */
        VNCS_Wake(pS);
    }
}

/*--------------------------------------------------------------------------*/
/* VNCS_SetBlank:                                                           */
/*                                                                          */
/* Change the blank state and, if it really changed, mark the whole screen   */
/* dirty for every client -- blanking so they receive the black frame,       */
/* unblanking so they receive the current one -- and wake the network        */
/* thread.                                                                   */
/*                                                                          */
/* Arguments:                                                               */
/*     pS     : the server.                                                 */
/*     bBlank : non-zero to blank, zero to restore.                         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void VNCS_SetBlank(VncServer *pS, int bBlank)
{
    int bNew;

    if (pS == NULL)
    {   /* No server. */
        return;
    }

    bNew = (bBlank != 0);

    pthread_mutex_lock(&pS->mtxLock);
    if (bNew == pS->bBlank)
    {   /* Already in that state: nothing to do. */
        pthread_mutex_unlock(&pS->mtxLock);
        return;
    }
    pS->bBlank = bNew;
    VNCS_MarkAllClients(pS);
    pthread_mutex_unlock(&pS->mtxLock);

    VNCS_Wake(pS);
}

/*--------------------------------------------------------------------------*/
/* VNCS_Stop:                                                               */
/*                                                                          */
/* Ask the network thread to finish, wake it, wait for it (it closes every   */
/* client on its way out), then close the listener and the wake pipe.        */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server.                                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void VNCS_Stop(VncServer *pS)
{
    if ((pS == NULL) || !pS->bStarted)
    {   /* Never started, or already stopped. */
        return;
    }

    pS->bStop = 1;
    VNCS_Wake(pS);
    pthread_join(pS->thrNet, NULL);
    pS->bStarted = 0;

    close(pS->iListenFd);
    pS->iListenFd = -1;
    close(pS->arrWake[0]);
    close(pS->arrWake[1]);
    pS->arrWake[0] = -1;
    pS->arrWake[1] = -1;
}

/*--------------------------------------------------------------------------*/
/* VNCS_Destroy:                                                            */
/*                                                                          */
/* Stop the server if it is running, then free everything it owns.          */
/*                                                                          */
/* Arguments:                                                               */
/*     pS : the server, or NULL.                                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void VNCS_Destroy(VncServer *pS)
{
    if (pS == NULL)
    {   /* Nothing to free. */
        return;
    }

    VNCS_Stop(pS);

    /* Wipe the server's copy of the password before the memory is freed. */
    memset(pS->arrPassword, 0, sizeof(pS->arrPassword));

    pthread_mutex_destroy(&pS->mtxLock);
    free(pS->pWork);
    free(pS->pPub);
    free(pS->pPending);
    free(pS->arrClients);
    free(pS->pcListenAddr);
    free(pS);
}
