/**
 * @file fbdev.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "fbdev.h"
#if USE_FBDEV || USE_BSD_FBDEV

#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#if USE_BSD_FBDEV
#include <sys/fcntl.h>
#include <sys/time.h>
#include <sys/consio.h>
#include <sys/fbio.h>
#else  /* USE_BSD_FBDEV */
#include <linux/fb.h>
#endif /* USE_BSD_FBDEV */

/*********************
 *      DEFINES
 *********************/
#ifndef FBDEV_PATH
#define FBDEV_PATH  "/dev/fb0"
#endif

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *      STRUCTURES
 **********************/

struct bsd_fb_var_info{
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t xres;
    uint32_t yres;
    int bits_per_pixel;
 };

struct bsd_fb_fix_info{
    long int line_length;
    long int smem_len;
};

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/
#if USE_BSD_FBDEV
static struct bsd_fb_var_info vinfo;
static struct bsd_fb_fix_info finfo;
#else
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
#endif /* USE_BSD_FBDEV */
static char *fbp = 0;
static long int screensize = 0;
static int fbfd = 0;

/**********************
 *      MACROS
 **********************/

#if USE_BSD_FBDEV
#define FBIOBLANK FBIO_BLANK
#endif /* USE_BSD_FBDEV */

#if FBDEV_SUNXI
/*--------------------------------------------------------------------------*/
/* Allwinner sunxi (disp2) panels -- built only with FBDEV_SUNXI (the build  */
/* passes it for a panel on that display engine; every other build is       */
/* exactly upstream's).  That driver's /dev/fb0 needs four things a plain   */
/* framebuffer does not:                                                     */
/*   - the mapping's cache mode is chosen by an ioctl BEFORE mmap, from a    */
/*     flag the last program to map it may have left set, so it is set      */
/*     explicitly; a cached mapping is invisible to the display engine until */
/*     it is cleaned to RAM (FBIO_CACHE_SYNC);                               */
/*   - the layer uses PER-PIXEL alpha, so a pixel whose top byte is 0 does    */
/*     not show: every 32-bit write forces that byte to 0xFF;               */
/*   - the fb holds two pages (yres_virtual = 2 x yres) and the one shown is  */
/*     chosen by FBIOPAN_DISPLAY, which waits for vsync -- so a refresh is   */
/*     drawn into the hidden page and then panned to, and never tears;      */
/*   - the backlight is not fbdev blanking but the display driver's          */
/*     brightness, set and read through /dev/disp.                           */
/*--------------------------------------------------------------------------*/

#define FBDEV_SUNXI_CACHE_SYNC     0x4630        // fb ioctl: clean the mapping to RAM; arg {offset, length}
#define FBDEV_SUNXI_ENABLE_CACHE   0x4631        // fb ioctl: word 0 = 1 cached / 0 uncached; takes effect at the next mmap
#define FBDEV_SUNXI_DISP_PATH      "/dev/disp"   // the display engine's control device (root only)
#define FBDEV_SUNXI_SET_BRIGHTNESS 0x102         // /dev/disp ioctl: arg {screen, brightness 0..255, 0, 0}
#define FBDEV_SUNXI_GET_BRIGHTNESS 0x103         // /dev/disp ioctl: arg {screen, 0, 0, 0}; the brightness is the return value
#define FBDEV_SUNXI_SCREEN         0             // display (screen) number the panel is on
#define FBDEV_SUNXI_LIT_DEFAULT    175           // brightness for a panel found dark at wake: the stock GUI's default level
#define FBDEV_SUNXI_ALPHA_OPAQUE   0xFF000000u   // OR-ed into every 32-bit pixel: alpha byte 0xFF, fully opaque
#define FBDEV_SUNXI_LOG_MAX        3             // failures of a per-refresh ioctl reported before going quiet

static int           fbdev_sunxi_pages = 1;      // pages the fb holds: 2 = flip between them, 1 = draw in place
static int           fbdev_sunxi_front = 0;      // the page on screen now: 0 or 1
static int           fbdev_sunxi_seeded = 0;     // this refresh's hidden page has been copied from the shown one
static unsigned long fbdev_sunxi_lit = 0;        // brightness to restore at wake; 0 until one is known
static int           fbdev_sunxi_sync_fails = 0; // FBIO_CACHE_SYNC failures seen (only the first few are logged)
static int           fbdev_sunxi_pan_fails = 0;  // FBIOPAN_DISPLAY failures seen (likewise)

/*--------------------------------------------------------------------------*/
/* fbdev_sunxi_cache_mode:                                                  */
/*                                                                          */
/* Choose the cache mode the NEXT mmap of /dev/fb0 gets.  Called with 1      */
/* just before this driver maps the fb (so it never inherits whatever the    */
/* previous owner left), and with 0 when it lets go (the boot default, for   */
/* whoever maps next).  A failure is logged and the caller carries on: the   */
/* worst case is an uncached mapping, which is slower but still correct.     */
/*                                                                          */
/* Arguments:                                                               */
/*     iOn : 1 for a cached mapping, 0 for uncached.                        */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; a failure is logged.                                 */
/*--------------------------------------------------------------------------*/

static void fbdev_sunxi_cache_mode(int iOn)
{
    unsigned long arrArg[2];

    /* Word 0 is the mode; word 1 is unused by the driver. */
    arrArg[0] = (unsigned long)iOn;
    arrArg[1] = 0;
    if (ioctl(fbfd, FBDEV_SUNXI_ENABLE_CACHE, arrArg) == -1)
    {   /* Not accepted: say so, and live with the mode the driver has. */
        perror("ioctl(FBIO_ENABLE_CACHE)");
    }
}

/*--------------------------------------------------------------------------*/
/* fbdev_sunxi_present:                                                     */
/*                                                                          */
/* End of a refresh: clean the whole mapping to RAM so the display engine    */
/* sees it, then -- when the fb has two pages -- pan to the page just drawn, */
/* which waits for vsync, and make it the shown page.  The next refresh      */
/* then draws into the other page.  The pan is given reserved[] = {0, 0,     */
/* xres, yres}: with rotation support in the driver, zeros there make it    */
/* return success WITHOUT panning.  A failed pan leaves the shown page as it */
/* was; the next refresh re-seeds from it.                                  */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; the first few failures of each ioctl are logged.     */
/*--------------------------------------------------------------------------*/

static void fbdev_sunxi_present(void)
{
    unsigned long            arrRegion[2];
    struct fb_var_screeninfo var;

    /* The whole buffer, both pages, in one clean. */
    arrRegion[0] = 0;
    arrRegion[1] = finfo.smem_len;
    if (ioctl(fbfd, FBDEV_SUNXI_CACHE_SYNC, arrRegion) == -1)
    {   /* The engine may show stale pixels; report the first few times. */
        fbdev_sunxi_sync_fails++;
        if (fbdev_sunxi_sync_fails <= FBDEV_SUNXI_LOG_MAX)
        {   /* Still within the reporting budget. */
            perror("ioctl(FBIO_CACHE_SYNC)");
        }
    }

    if (fbdev_sunxi_pages == 2)
    {   /* Flip to the page just drawn. */
        var = vinfo;
        var.xoffset = 0;
        var.yoffset = (uint32_t)(1 - fbdev_sunxi_front) * vinfo.yres;
        var.reserved[0] = 0;
        var.reserved[1] = 0;
        var.reserved[2] = vinfo.xres;
        var.reserved[3] = vinfo.yres;
        var.activate = FB_ACTIVATE_NOW;
        if (ioctl(fbfd, FBIOPAN_DISPLAY, &var) == -1)
        {   /* Not shown: the old page stays; report the first few times. */
            fbdev_sunxi_pan_fails++;
            if (fbdev_sunxi_pan_fails <= FBDEV_SUNXI_LOG_MAX)
            {   /* Still within the reporting budget. */
                perror("ioctl(FBIOPAN_DISPLAY)");
            }
        }
        else
        {   /* Shown: that page is now the front. */
            fbdev_sunxi_front = 1 - fbdev_sunxi_front;
        }
    }

    /* The next refresh starts by seeding its page afresh. */
    fbdev_sunxi_seeded = 0;
}

/*--------------------------------------------------------------------------*/
/* fbdev_sunxi_flush32:                                                     */
/*                                                                          */
/* Write one rendered area into the fb, 32 bits per pixel, alpha forced     */
/* opaque.  With two pages it draws into the HIDDEN one, first copying the   */
/* shown page into it once per refresh -- LVGL redraws only what changed,   */
/* so the rest of the frame has to be there already.  When LVGL says this   */
/* is the refresh's last area, the frame is presented.                     */
/*                                                                          */
/* Arguments:                                                               */
/*     drv            : the display driver, to ask whether this is the last */
/*                      area of the refresh.                                */
/*     x1, y1, x2, y2 : the area, already clipped to the screen.            */
/*     color_p        : its pixels, row after row, (x2 - x1 + 1) per row.   */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; the caller reports the flush ready.                  */
/*--------------------------------------------------------------------------*/

static void fbdev_sunxi_flush32(lv_disp_drv_t * drv, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                const lv_color_t * color_p)
{
    size_t          ulPageBytes;
    int             iDraw;
    uint32_t       *pPage;
    uint32_t       *pDst;
    const uint32_t *pSrc;
    int32_t         iWidth;
    int32_t         x;
    int32_t         y;

    /* One page is yres rows of line_length bytes. */
    ulPageBytes = (size_t)vinfo.yres * (size_t)finfo.line_length;

    /* Draw in place with one page; with two, into the hidden one, seeded once per refresh. */
    iDraw = fbdev_sunxi_front;
    if (fbdev_sunxi_pages == 2)
    {   /* Flipping: the page not on screen. */
        iDraw = 1 - fbdev_sunxi_front;
        if (!fbdev_sunxi_seeded)
        {   /* First area of this refresh: start from the frame being shown. */
            memcpy(fbp + (size_t)iDraw * ulPageBytes, fbp + (size_t)fbdev_sunxi_front * ulPageBytes, ulPageBytes);
            fbdev_sunxi_seeded = 1;
        }
    }
    pPage = (uint32_t *)(fbp + (size_t)iDraw * ulPageBytes);

    /* Copy the area in, every pixel made opaque. */
    iWidth = x2 - x1 + 1;
    pSrc = (const uint32_t *)color_p;
    for (y = y1; y <= y2; y++)
    {   /* One row of the area per iteration. */
        pDst = pPage + (size_t)y * (size_t)(finfo.line_length / 4) + (size_t)x1;
        for (x = 0; x < iWidth; x++)
        {   /* One pixel per iteration: LVGL's BGRA, alpha byte set. */
            pDst[x] = pSrc[x] | FBDEV_SUNXI_ALPHA_OPAQUE;
        }
        pSrc += iWidth;
    }

    if (lv_disp_flush_is_last(drv))
    {   /* The refresh is complete: show it. */
        fbdev_sunxi_present();
    }
}

/*--------------------------------------------------------------------------*/
/* fbdev_sunxi_disp:                                                        */
/*                                                                          */
/* One /dev/disp brightness call, opening the device for just that call      */
/* (display sleep and wake are rare).  The argument block is the driver's:  */
/* {screen, value, 0, 0}.                                                    */
/*                                                                          */
/* Arguments:                                                               */
/*     ulCmd   : FBDEV_SUNXI_SET_BRIGHTNESS or FBDEV_SUNXI_GET_BRIGHTNESS.  */
/*     ulValue : the brightness to set (0..255); ignored by a get.          */
/*                                                                          */
/* Returns:                                                                 */
/*     long : a get's brightness, a set's result; -1 on failure (logged).   */
/*--------------------------------------------------------------------------*/

static long fbdev_sunxi_disp(unsigned long ulCmd, unsigned long ulValue)
{
    int           iDisp;
    unsigned long arrArg[4];
    long          lRet;

    iDisp = open(FBDEV_SUNXI_DISP_PATH, O_RDWR);
    if (iDisp == -1)
    {   /* No display control (not root, or not this driver). */
        perror("open(" FBDEV_SUNXI_DISP_PATH ")");
        return(-1);
    }

    /* The screen, the value, two unused words. */
    arrArg[0] = FBDEV_SUNXI_SCREEN;
    arrArg[1] = ulValue;
    arrArg[2] = 0;
    arrArg[3] = 0;
    lRet = ioctl(iDisp, ulCmd, arrArg);
    if (lRet == -1)
    {   /* Refused. */
        perror("ioctl(" FBDEV_SUNXI_DISP_PATH ")");
    }
    close(iDisp);

    /* A get's value, or the set's result. */
    return(lRet);
}

/*--------------------------------------------------------------------------*/
/* fbdev_sunxi_backlight_off:                                               */
/*                                                                          */
/* Display sleep: remember the lit brightness (when there is one), then set */
/* it to 0 -- what the stock GUI's own screen-off does.  The pixels stay as  */
/* they are; only the light goes.                                            */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void fbdev_sunxi_backlight_off(void)
{
    long lNow;

    /* Keep the level to wake to, unless it is already dark. */
    lNow = fbdev_sunxi_disp(FBDEV_SUNXI_GET_BRIGHTNESS, 0);
    if (lNow > 0)
    {   /* Lit: this is the level to come back to. */
        fbdev_sunxi_lit = (unsigned long)lNow;
    }
    fbdev_sunxi_disp(FBDEV_SUNXI_SET_BRIGHTNESS, 0);
}

/*--------------------------------------------------------------------------*/
/* fbdev_sunxi_backlight_on:                                                */
/*                                                                          */
/* Wake, and start-up: back to the remembered level; with none remembered,  */
/* a panel that is already lit is left as it is, and a dark one -- the      */
/* brightness outlives the stock GUI, so a panel it idled dark stays dark    */
/* until something sets it -- is lit at the stock GUI's default level.       */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

static void fbdev_sunxi_backlight_on(void)
{
    long lNow;

    if (fbdev_sunxi_lit > 0)
    {   /* A level was remembered at sleep: restore it. */
        fbdev_sunxi_disp(FBDEV_SUNXI_SET_BRIGHTNESS, fbdev_sunxi_lit);
        return;
    }

    /* Nothing remembered: look at the panel as it is. */
    lNow = fbdev_sunxi_disp(FBDEV_SUNXI_GET_BRIGHTNESS, 0);
    if (lNow > 0)
    {   /* Already lit: leave it, and remember it. */
        fbdev_sunxi_lit = (unsigned long)lNow;
        return;
    }

    /* Dark (or unreadable): light it at the default level. */
    fbdev_sunxi_disp(FBDEV_SUNXI_SET_BRIGHTNESS, FBDEV_SUNXI_LIT_DEFAULT);
    fbdev_sunxi_lit = FBDEV_SUNXI_LIT_DEFAULT;
}
#endif /* FBDEV_SUNXI */

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/*--------------------------------------------------------------------------*/
/* fbdev_init:                                                              */
/*                                                                          */
/* Open the framebuffer, read its geometry and map it.  The screen is not    */
/* cleared, so what is shown stays until the first flush.  With FBDEV_SUNXI */
/* the cache mode is set before the mapping, the page count and the shown   */
/* page are read, and a failed FBIOBLANK is not fatal (that driver does not  */
/* use fbdev blanking).                                                      */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; a failure is reported and leaves fbp unmapped, which  */
/*            makes every flush a no-op.                                    */
/*--------------------------------------------------------------------------*/

void fbdev_init(void)
{
    /* Open the file for reading and writing. */
    fbfd = open(FBDEV_PATH, O_RDWR);
    if(fbfd == -1) {  /* No framebuffer: nothing can be drawn. */
        perror("Error: cannot open framebuffer device");
        return;
    }
    LV_LOG_INFO("The framebuffer device was opened successfully");

    /* Make sure that the display is on. */
    if (ioctl(fbfd, FBIOBLANK, FB_BLANK_UNBLANK) != 0) {  /* Refused. */
        perror("ioctl(FBIOBLANK)");
#if !FBDEV_SUNXI
        return;
#endif
    }

#if USE_BSD_FBDEV
    struct fbtype fb;
    unsigned line_length;

    /* Get fb type. */
    if (ioctl(fbfd, FBIOGTYPE, &fb) != 0) {  /* Unreadable. */
        perror("ioctl(FBIOGTYPE)");
        return;
    }

    /* Get screen width. */
    if (ioctl(fbfd, FBIO_GETLINEWIDTH, &line_length) != 0) {  /* Unreadable. */
        perror("ioctl(FBIO_GETLINEWIDTH)");
        return;
    }

    vinfo.xres = (unsigned) fb.fb_width;
    vinfo.yres = (unsigned) fb.fb_height;
    vinfo.bits_per_pixel = fb.fb_depth;
    vinfo.xoffset = 0;
    vinfo.yoffset = 0;
    finfo.line_length = line_length;
    finfo.smem_len = finfo.line_length * vinfo.yres;
#else /* USE_BSD_FBDEV */

    /* Get fixed screen information. */
    if(ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) {  /* Unreadable. */
        perror("Error reading fixed information");
        return;
    }

    /* Get variable screen information. */
    if(ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) {  /* Unreadable. */
        perror("Error reading variable information");
        return;
    }
#endif /* USE_BSD_FBDEV */

    LV_LOG_INFO("%dx%d, %dbpp", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    /* Figure out the size of the screen in bytes. */
    screensize =  finfo.smem_len; //finfo.line_length * vinfo.yres;

#if FBDEV_SUNXI
    /* The cache mode must be chosen BEFORE the mapping is made. */
    fbdev_sunxi_cache_mode(1);
#endif

    /* Map the device to memory. */
    fbp = (char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if((intptr_t)fbp == -1) {  /* Not mappable: leave fbp so flushes do nothing. */
        perror("Error: failed to map framebuffer device to memory");
        return;
    }

    /* Don't initialise the memory to retain what's currently displayed / avoid clearing the screen.  */
    /* This is important for applications that only draw to a subsection of the full framebuffer.    */

#if FBDEV_SUNXI
    /* Two pages when the buffer holds two frames; the shown one is by yoffset. */
    fbdev_sunxi_pages = 1;
    fbdev_sunxi_front = 0;
    if ((vinfo.yres_virtual >= 2 * vinfo.yres) &&
        ((long int)finfo.smem_len >= 2 * (long int)vinfo.yres * (long int)finfo.line_length)) {  /* Room to flip. */
        fbdev_sunxi_pages = 2;
        if (vinfo.yoffset >= vinfo.yres) {  /* The second page is on screen now. */
            fbdev_sunxi_front = 1;
        }
    }
#endif

    LV_LOG_INFO("The framebuffer device was mapped to memory successfully");

}

/*--------------------------------------------------------------------------*/
/* fbdev_exit:                                                              */
/*                                                                          */
/* Let the framebuffer go.  With FBDEV_SUNXI the boot default cache mode is */
/* restored and the mapping released first, so the next program to map the */
/* fb (the stock GUI, relaunched) starts as it would after boot.            */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing.                                                      */
/*--------------------------------------------------------------------------*/

void fbdev_exit(void)
{
#if FBDEV_SUNXI
    /* Restore the default mode, then unmap. */
    fbdev_sunxi_cache_mode(0);
    if ((fbp != NULL) && ((intptr_t)fbp != -1)) {  /* A mapping was made. */
        munmap(fbp, screensize);
        fbp = NULL;
    }
#endif
    close(fbfd);
}

/*--------------------------------------------------------------------------*/
/* fbdev_blank:                                                             */
/*                                                                          */
/* Display sleep: blank the panel -- fbdev blanking upstream, the backlight  */
/* turned off with FBDEV_SUNXI (that driver ignores fbdev blanking).        */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; a failure is logged.                                 */
/*--------------------------------------------------------------------------*/

void fbdev_blank(void) {
#if FBDEV_SUNXI
  /* The light off; the frame stays. */
  fbdev_sunxi_backlight_off();
#else
  if (ioctl(fbfd, FBIOBLANK, FB_BLANK_NORMAL) == -1) {  /* Refused. */
    LV_LOG_INFO("Failed to blank display");
  }
#endif
}

/*--------------------------------------------------------------------------*/
/* fbdev_unblank:                                                           */
/*                                                                          */
/* Wake (and start-up): unblank and show page 0 upstream; with FBDEV_SUNXI  */
/* the backlight is restored instead, and the page is left alone -- the     */
/* flip logic tracks which page is shown, and panning here would undo it.   */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; a failure is logged.                                 */
/*--------------------------------------------------------------------------*/

void fbdev_unblank(void) {
#if FBDEV_SUNXI
  /* The light back on, at the level it had. */
  fbdev_sunxi_backlight_on();
#else
  struct fb_var_screeninfo var_info;
  if (ioctl(fbfd, FBIOBLANK, FB_BLANK_UNBLANK) == -1) {  /* Refused. */
    LV_LOG_INFO("Failed to unblank display");
  }

  /* Page 0 back on screen. */
  if (ioctl(fbfd, FBIOGET_VSCREENINFO, &var_info) == -1) {  /* Unreadable. */
    LV_LOG_INFO("Failed to fetch screen info");
  }

  var_info.yoffset = 0;

  if (ioctl(fbfd, FBIOPAN_DISPLAY, &var_info) == -1) {  /* Refused. */
    LV_LOG_INFO("Failed to pan display");
  }
#endif
}


/*--------------------------------------------------------------------------*/
/* fbdev_flush:                                                             */
/*                                                                          */
/* LVGL's flush callback: copy a rendered area into the framebuffer at the  */
/* pixel depth the fb uses, then tell LVGL it is done.  Areas wholly off    */
/* screen are skipped and the rest clipped to the screen.  A 32-bit fb with */
/* FBDEV_SUNXI goes through fbdev_sunxi_flush32 (alpha, pages, present); an */
/* off-screen LAST area there still presents the refresh.                   */
/*                                                                          */
/* Arguments:                                                               */
/*     drv     : the display driver this flush belongs to.                  */
/*     area    : the area to copy `color_p` to.                             */
/*     color_p : the area's pixels.                                         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : nothing; LVGL is told through lv_disp_flush_ready.            */
/*--------------------------------------------------------------------------*/

void fbdev_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p)
{
    if(fbp == NULL ||
            area->x2 < 0 ||
            area->y2 < 0 ||
            area->x1 > (int32_t)vinfo.xres - 1 ||
            area->y1 > (int32_t)vinfo.yres - 1) {  /* Nothing mapped, or nothing on screen. */
#if FBDEV_SUNXI
        if ((fbp != NULL) && (vinfo.bits_per_pixel == 32) && lv_disp_flush_is_last(drv)) {  /* Still the refresh's end. */
            fbdev_sunxi_present();
        }
#endif
        lv_disp_flush_ready(drv);
        return;
    }

    /*Truncate the area to the screen*/
    int32_t act_x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t act_y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t act_x2 = area->x2 > (int32_t)vinfo.xres - 1 ? (int32_t)vinfo.xres - 1 : area->x2;
    int32_t act_y2 = area->y2 > (int32_t)vinfo.yres - 1 ? (int32_t)vinfo.yres - 1 : area->y2;

#if FBDEV_SUNXI
    if(vinfo.bits_per_pixel == 32) {  /* The sunxi panel's depth: its own path. */
        fbdev_sunxi_flush32(drv, act_x1, act_y1, act_x2, act_y2, color_p);
        lv_disp_flush_ready(drv);
        return;
    }
#endif

    lv_coord_t w = (act_x2 - act_x1 + 1);
    long int location = 0;
    long int byte_location = 0;
    unsigned char bit_location = 0;

    /*32 or 24 bit per pixel*/
    if(vinfo.bits_per_pixel == 32 || vinfo.bits_per_pixel == 24) {  /* One row per iteration below. */
        uint32_t * fbp32 = (uint32_t *)fbp;
        int32_t y;
        for(y = act_y1; y <= act_y2; y++) {  /* One row of the area per iteration. */
            location = (act_x1 + vinfo.xoffset) + (y + vinfo.yoffset) * finfo.line_length / 4;
            memcpy(&fbp32[location], (uint32_t *)color_p, (act_x2 - act_x1 + 1) * 4);
            color_p += w;
        }
    }
    /*16 bit per pixel*/
    else if(vinfo.bits_per_pixel == 16) {  /* One row per iteration below. */
        uint16_t * fbp16 = (uint16_t *)fbp;
        int32_t y;
        for(y = act_y1; y <= act_y2; y++) {  /* One row of the area per iteration. */
            location = (act_x1 + vinfo.xoffset) + (y + vinfo.yoffset) * finfo.line_length / 2;
            memcpy(&fbp16[location], (uint32_t *)color_p, (act_x2 - act_x1 + 1) * 2);
            color_p += w;
        }
    }
    /*8 bit per pixel*/
    else if(vinfo.bits_per_pixel == 8) {  /* One row per iteration below. */
        uint8_t * fbp8 = (uint8_t *)fbp;
        int32_t y;
        for(y = act_y1; y <= act_y2; y++) {  /* One row of the area per iteration. */
            location = (act_x1 + vinfo.xoffset) + (y + vinfo.yoffset) * finfo.line_length;
            memcpy(&fbp8[location], (uint32_t *)color_p, (act_x2 - act_x1 + 1));
            color_p += w;
        }
    }
    /*1 bit per pixel*/
    else if(vinfo.bits_per_pixel == 1) {  /* One pixel at a time, set bit by bit. */
        uint8_t * fbp8 = (uint8_t *)fbp;
        int32_t x;
        int32_t y;
        for(y = act_y1; y <= act_y2; y++) {  /* One row of the area per iteration. */
            for(x = act_x1; x <= act_x2; x++) {  /* One pixel per iteration. */
                location = (x + vinfo.xoffset) + (y + vinfo.yoffset) * vinfo.xres;
                byte_location = location / 8; /* find the byte we need to change */
                bit_location = location % 8; /* inside the byte found, find the bit we need to change */
                fbp8[byte_location] &= ~(((uint8_t)(1)) << bit_location);
                fbp8[byte_location] |= ((uint8_t)(color_p->full)) << bit_location;
                color_p++;
            }

            color_p += area->x2 - act_x2;
        }
    } else {  /* Not supported bit per pixel: nothing written. */
        /*Not supported bit per pixel*/
    }

    //May be some direct update command is required
    //ret = ioctl(state->fd, FBIO_UPDATE, (unsigned long)((uintptr_t)rect));

    /* The area is written. */
    lv_disp_flush_ready(drv);
}

void fbdev_get_sizes(uint32_t *width, uint32_t *height, uint32_t *dpi) {
    if (width)
        *width = vinfo.xres;

    if (height)
        *height = vinfo.yres;

    if (dpi && vinfo.height)
        *dpi = DIV_ROUND_UP(vinfo.xres * 254, vinfo.width * 10);
}

void fbdev_set_offset(uint32_t xoffset, uint32_t yoffset) {
    vinfo.xoffset = xoffset;
    vinfo.yoffset = yoffset;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif
