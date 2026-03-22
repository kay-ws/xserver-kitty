/*
 * Copyright © 2004 PillowElephantBadgerBankPond
 * Copyright © 2014 Sergii Pylypenko
 * Copyright © 2014 Hayaki Saito
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of PillowElephantBadgerBankPond not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.    PillowElephantBadgerBankPond makes no
 * representations about the suitability of this software for any purpose.    It
 * is provided "as is" without express or implied warranty.
 *
 * PillowElephantBadgerBankPond DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL PillowElephantBadgerBankPond BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * It's really not my fault - see it was the elephants!!
 *    - jaymz
 *
 */
#include "xorg-server.h"
#include "sixel-config.h"
#include "kdrive.h"
#include <termios.h>
#include <X11/keysym.h>
#include <sys/wait.h>
#if USE_MUTEX
# include <pthread.h>
#endif
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zlib.h>
#include <errno.h>

#undef USE_DECMOUSE
#undef USE_FILTER_RECTANGLE

static void kittyFini(void);
static Bool kittyScreenInit(KdScreenInfo *screen);
static Bool kittyFinishInitScreen(ScreenPtr pScreen);
static Bool kittyCreateRes(ScreenPtr pScreen);
static Bool kittyMapFramebuffer(KdScreenInfo *screen);

static void kittyKeyboardFini(KdKeyboardInfo *ki);
static Status kittyKeyboardInit(KdKeyboardInfo *ki);
static Status kittyKeyboardEnable(KdKeyboardInfo *ki);
static void kittyKeyboardDisable(KdKeyboardInfo *ki);
static void kittyKeyboardLeds(KdKeyboardInfo *ki, int leds);
static void kittyKeyboardBell(KdKeyboardInfo *ki, int volume, int frequency, int duration);

static Bool kittyMouseInit(KdPointerInfo *pi);
static void kittyMouseFini(KdPointerInfo *pi);
static Status kittyMouseEnable(KdPointerInfo *pi);
static void kittyMouseDisable(KdPointerInfo *pi);

KdKeyboardInfo *kittyKeyboard = NULL;
KdPointerInfo *kittyPointer = NULL;
#if USE_MUTEX
pthread_mutex_t kitty_mutex;
#endif

/* Debug tracing (disabled by default). Define XKITTY_TRACE to enable. */
#ifdef XKITTY_TRACE
#include <stdio.h>
#define TRACE(s) fprintf(stderr, s)
#define TRACE1(s, a1) fprintf(stderr, s, a1)
#define TRACE2(s, a1,a2) fprintf(stderr, s, a1,a2)
#define TRACE3(s, a1,a2,a3) fprintf(stderr, s, a1,a2,a3)
#define TRACE4(s, a1,a2,a3,a4) fprintf(stderr, s, a1,a2,a3,a4)
#define TRACE5(s, a1,a2,a3,a4,a5) fprintf(stderr, s, a1,a2,a3,a4,a5)
#else
#define TRACE(s) do{}while(0)
#define TRACE1(s,a1) do{}while(0)
#define TRACE2(s,a1,a2) do{}while(0)
#define TRACE3(s,a1,a2,a3) do{}while(0)
#define TRACE4(s,a1,a2,a3,a4) do{}while(0)
#define TRACE5(s,a1,a2,a3,a4,a5) do{}while(0)
#endif

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t
base64_encode(char *dst, const unsigned char *src, size_t len)
{
    size_t i, j;
    for (i = 0, j = 0; i + 2 < len; i += 3) {
        dst[j++] = base64_table[(src[i] >> 2) & 0x3F];
        dst[j++] = base64_table[((src[i] & 0x3) << 4) | (src[i+1] >> 4)];
        dst[j++] = base64_table[((src[i+1] & 0xF) << 2) | (src[i+2] >> 6)];
        dst[j++] = base64_table[src[i+2] & 0x3F];
    }
    if (i < len) {
        dst[j++] = base64_table[(src[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            dst[j++] = base64_table[((src[i] & 0x3) << 4) | (src[i+1] >> 4)];
            dst[j++] = base64_table[(src[i+1] & 0xF) << 2];
        } else {
            dst[j++] = base64_table[(src[i] & 0x3) << 4];
            dst[j++] = '=';
        }
        dst[j++] = '=';
    }
    dst[j] = '\0';
    return j;
}

KdKeyboardDriver kittyKeyboardDriver = {
    .name = "keyboard",
    .Init = kittyKeyboardInit,
    .Fini = kittyKeyboardFini,
    .Enable = kittyKeyboardEnable,
    .Disable = kittyKeyboardDisable,
    .Leds = kittyKeyboardLeds,
    .Bell = kittyKeyboardBell,
};

KdPointerDriver kittyMouseDriver = {
    .name = "mouse",
    .Init = kittyMouseInit,
    .Fini = kittyMouseFini,
    .Enable = kittyMouseEnable,
    .Disable = kittyMouseDisable,
};


KdCardFuncs kittyFuncs = {
    .scrinit = kittyScreenInit,    /* scrinit */
    .finishInitScreen = kittyFinishInitScreen, /* finishInitScreen */
    .createRes = kittyCreateRes,    /* createRes */
};

/* Transfer mode: 1 = tempfile (t=t), 0 = direct/inline (t=d) */
static int kitty_use_tempfile = 1;
/* Compression: 1 = zlib (o=z), 0 = raw RGB */
static int kitty_compress = 1;

#define KITTY_TEMPFILE_PATH "/tmp/xkitty-frame.bin"

int mouseState = 0;

enum { NUMRECTS = 32, FULLSCREEN_REFRESH_TIME = 1000 };

typedef struct
{
    int w;
    int h;
    int pitch;
    int pixel_w, pixel_h;
    int cell_w, cell_h;
    Rotation randr;
    Bool shadow;
    unsigned char *buffer;
    unsigned char *bitmap;
    /* Kitty Graphics protocol */
    char *base64_buf;        /* base64 output buffer */
    size_t base64_buf_size;  /* allocated size */
    unsigned char *zlib_buf; /* zlib compressed output buffer */
    size_t zlib_buf_size;    /* allocated size */
} KITTY_Driver;

static KITTY_Driver *g_driver = NULL;

#define KITTY_UP                (1 << 12 | ('A' - '@'))
#define KITTY_DOWN              (1 << 12 | ('B' - '@'))
#define KITTY_RIGHT             (1 << 12 | ('C' - '@'))
#define KITTY_LEFT              (1 << 12 | ('D' - '@'))
#define KITTY_END               (1 << 12 | ('F' - '@'))
#define KITTY_HOME              (1 << 12 | ('H' - '@'))
#define KITTY_FOCUSIN           (1 << 12 | ('I' - '@'))
#define KITTY_FOCUSOUT          (1 << 12 | ('O' - '@'))
#define KITTY_F1                (1 << 12 | ('P' - '@'))
#define KITTY_F2                (1 << 12 | ('Q' - '@'))
#define KITTY_F3                (1 << 12 | ('R' - '@'))
#define KITTY_F4                (1 << 12 | ('S' - '@'))
#define KITTY_FKEYS             (1 << 12 | ('~' - '@'))
#define KITTY_MOUSE_SGR         (1 << 12 | ('<' - ';') << 4 << 6 | ('M' - '@'))
#define KITTY_MOUSE_SGR_RELEASE (1 << 12 | ('<' - ';') << 4 << 6 | ('m' - '@'))
#define KITTY_MOUSE_DEC         (1 << 12 | ('&' - 0x1f) << 6 | ('w' - '@'))
#define KITTY_DTTERM_SEQS       (1 << 12 | ('t' - '@'))
#define KITTY_UNKNOWN           (513)

typedef struct _key {
    int params[256];
    int nparams;
    int value;
} kitty_key_t;

enum _state {
    STATE_GROUND = 0,
    STATE_ESC = 1,
    STATE_CSI = 2,
    STATE_CSI_IGNORE = 3,
    STATE_CSI_PARAM = 4,
};

static int get_input(char *buf, int size) {
    fd_set fdset;
    struct timeval timeout;
    FD_ZERO(&fdset);
    FD_SET(STDIN_FILENO, &fdset);
    timeout.tv_sec = 0;
    timeout.tv_usec = 1;
    if (select(STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout) == 1)
        return read(STDIN_FILENO, buf, size);
    return 0;
}

static int getkeys(char *buf, int nread, kitty_key_t *keys)
{
    int i, c;
    int size = 0;
    static int state = STATE_GROUND;
    static int ibytes = 0;
    static int pbytes = 0;

    for (i = 0; i < nread; i++) {
        c = buf[i];
restart:
        switch (state) {
        case STATE_GROUND:
            switch (c) {
            case 0x1b:
                state = STATE_ESC;
                break;
            default:
                keys[size++].value = c;
                break;
            }
            break;
        case STATE_ESC:
            switch (c) {
            case 'O':
            case '[':
                keys[size].nparams = 0;
                pbytes = 0;
                state = STATE_CSI;
                break;
            default:
                keys[size++].value = 0x1b;
                state = STATE_GROUND;
                goto restart;
            }
            break;
        case STATE_CSI:
            switch (c) {
            case '\x1b':
                state = STATE_ESC;
                break;
            case '\x00'...'\x1a':
            case '\x1c'...'\x1f':
            case '\x7f':
                break;
            case ' '...'/':
                ibytes = c - ' ';
                pbytes = 0;
                state = STATE_CSI_PARAM;
                break;
            case '0'...'9':
                ibytes = 0;
                pbytes = c - '0';
                keys[size].nparams = 0;
                state = STATE_CSI_PARAM;
                break;
            case '<'...'?':
                ibytes = (c - ';') << 4;
                keys[size].nparams = 0;
                state = STATE_CSI_PARAM;
                break;
            case '@'...'~':
                keys[size].nparams = 0;
                keys[size++].value = 1 << 12 | (c - '@');
                state = STATE_GROUND;
                break;
            default:
                state = STATE_GROUND;
                break;
            }
            break;
        case STATE_CSI_PARAM:
            switch (c) {
            case '\x1b':
                state = STATE_ESC;
                break;
            case '\x00'...'\x1a':
            case '\x1c'...'\x1f':
            case '\x7f':
                break;
            case ' '...'/':
                ibytes |= c - 0x1f;
                state = STATE_CSI_PARAM;
                break;
            case '0'...'9':
                pbytes = pbytes * 10 + c - '0';
                state = STATE_CSI_PARAM;
                break;
            case ':'...';':
                if (keys[size].nparams < sizeof(keys[size].params) / sizeof(*keys[size].params)) {
                    keys[size].params[keys[size].nparams++] = pbytes;
                    pbytes = 0;
                }
                break;
            case '@'...'~':
                if (keys[size].nparams < sizeof(keys[size].params) / sizeof(*keys[size].params)) {
                    keys[size].params[keys[size].nparams++] = pbytes;
                    keys[size++].value = 1 << 12 | ibytes << 6  | (c - '@');
                }
                state = STATE_GROUND;
                break;
            default:
                state = STATE_GROUND;
                break;
            }
            break;
        }
    }
    return size;
}



static int SendModifierKey(int state, uint8_t press_state)
{
    int posted = 0;

    if (state & 1) {
        KdEnqueueKeyboardEvent(kittyKeyboard, 42+8, press_state);
    }
    if (state & 2) {
        KdEnqueueKeyboardEvent(kittyKeyboard, 56+8, press_state);
    }
    if (state & 4) {
        KdEnqueueKeyboardEvent(kittyKeyboard, 29+8, press_state);
    }

    return posted;
}

static int GetScancode(int code)
{
    static u_char tbl[] = {
         0,  0,  0,  0,  0,  0,  0,  0, 14, 15, 36,  0,  0, 28,  0,  0, /* 0x00 - 0x0f */
         0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0, /* 0x10 - 0x1f */
        57,  2, 40,  4,  5,  6,  8, 40, 10, 11,  9, 13, 51, 12, 52, 53, /* 0x20 - 0x2f */
        11,  2,  3,  4,  5,  6,  7,  8,  9, 10, 39, 39, 51, 13, 52, 53, /* 0x30 - 0x3f */
         3, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, /* 0x40 - 0x4f */
        25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 26, 43, 27,  7, 12, /* 0x50 - 0x5f */
        41, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, /* 0x60 - 0x6f */
        25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 26, 43, 27, 41, 14, /* 0x70 - 0x7f */
    };

    if(code <= 0x7f && tbl[code] > 0) {
        return tbl[code] + 8;
    } else {
        return 0;
    }
}


static int GetState(int code)
{
    if (code <= 0x7f) {
        static u_char tbl[] = {
            4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 4, 4, 4, 0, 4, 4, /* 0x00 - 0x0f */
            4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 4, 4, 4, 4, /* 0x10 - 0x1f */
            0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, /* 0x20 - 0x2f */
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, /* 0x30 - 0x3f */
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x40 - 0x4f */
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, /* 0x50 - 0x5f */
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x60 - 0x6f */
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, /* 0x70 - 0x7f */
        };

        /* Shift */
        return tbl[code];
    }
    return 0;
}

static int TranslateKey(int value)
{
    /* Set the keysym information */
    int scancode = GetScancode(value);

    if (scancode == 0 && value < 0x20) {
        /* It seems Ctrl+N key */
        scancode = GetScancode(value + 0x60);
    }
    return scancode;
}

struct termios orig_termios;

static void tty_raw(void)
{
    struct termios raw;

    if (tcgetattr(fileno(stdin), &orig_termios) < 0) {
        perror("can't set raw mode");
    }
    raw = orig_termios;
    raw.c_iflag &= ~(/*BRKINT |*/ ICRNL /*| INPCK | ISTRIP | IXON*/);
    raw.c_lflag &= ~(ECHO | ICANON /*| IEXTEN | ISIG*/);
    raw.c_lflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    raw.c_cc[VINTR] = 0; raw.c_cc[VKILL] = 0; raw.c_cc[VQUIT] = 0;
    raw.c_cc[VSTOP] = 0; raw.c_cc[VSUSP] = 0;
    if (tcsetattr(fileno(stdin), TCSAFLUSH, &raw) < 0) {
        perror("can't set raw mode");
    }
}

static void tty_restore(void)
{
    tcsetattr(fileno(stdin), TCSADRAIN, &orig_termios);
}

#define KITTY_CHUNK_SIZE 4096

static void
kitty_send_frame_tempfile(KITTY_Driver *driver)
{
    const unsigned char *data;
    size_t data_size;

    int rgb_size = driver->w * driver->h * 3;

    if (kitty_compress) {
        /* zlib compress the RGB data */
        uLongf compressed_size = driver->zlib_buf_size;
        int zret = compress2(driver->zlib_buf, &compressed_size,
                             driver->bitmap, rgb_size, Z_BEST_SPEED);
        if (zret != Z_OK) {
            fprintf(stderr, "[kitty] compress2 failed: %d\n", zret);
            return;
        }
        data = (unsigned char *)driver->zlib_buf;
        data_size = compressed_size;
    } else {
        data = driver->bitmap;
        data_size = (size_t)rgb_size;
    }

    /* Write data to temp file (single-writer: Xkitty is single-threaded,
     * printf follows fclose, so bcon never reads an incomplete file) */
    FILE *fp = fopen(KITTY_TEMPFILE_PATH, "wb");
    if (!fp) {
        fprintf(stderr, "[kitty] fopen %s failed: %s\n",
                KITTY_TEMPFILE_PATH, strerror(errno));
        return;
    }
    if (fwrite(data, 1, data_size, fp) != data_size) {
        fprintf(stderr, "[kitty] fwrite failed: %s\n", strerror(errno));
        fclose(fp);
        return;
    }
    fclose(fp);

    /* base64 encode the file path */
    char path_b64[256];
    base64_encode(path_b64,
                  (const unsigned char *)KITTY_TEMPFILE_PATH,
                  strlen(KITTY_TEMPFILE_PATH));

    /* Send Kitty Graphics command with file path only */
    if (kitty_compress) {
        printf("\033_Ga=T,i=1,f=24,o=z,q=2,s=%d,v=%d,t=t;%s\033\\",
               driver->w, driver->h, path_b64);
    } else {
        printf("\033_Ga=T,i=1,f=24,q=2,s=%d,v=%d,t=t;%s\033\\",
               driver->w, driver->h, path_b64);
    }
    fflush(stdout);
}

static void
kitty_send_frame(KITTY_Driver *driver)
{
    /* t=t mode: write to tempfile, send path only */
    if (kitty_use_tempfile) {
        kitty_send_frame_tempfile(driver);
        return;
    }

    /* t=d mode: existing inline base64 transfer */
    int rgb_size = driver->w * driver->h * 3;

    /* zlib compress the RGB data */
    uLongf compressed_size = driver->zlib_buf_size;
    int zret = compress2(driver->zlib_buf, &compressed_size,
                         driver->bitmap, rgb_size, Z_BEST_SPEED);
    if (zret != Z_OK) {
        fprintf(stderr, "[kitty] compress2 failed: %d (buf_size=%zu, rgb=%d)\n",
                zret, driver->zlib_buf_size, rgb_size);
        return;
    }

    fprintf(stderr, "[kitty] frame: rgb=%d zlib=%lu ratio=%.1f%%\n",
            rgb_size, (unsigned long)compressed_size,
            100.0 * compressed_size / rgb_size);

    size_t b64_len = base64_encode(driver->base64_buf,
                                   (unsigned char *)driver->zlib_buf,
                                   compressed_size);

    size_t offset = 0;
    int first = 1;

    while (offset < b64_len) {
        size_t remaining = b64_len - offset;
        size_t chunk = (remaining > KITTY_CHUNK_SIZE) ? KITTY_CHUNK_SIZE : remaining;
        int more = (offset + chunk < b64_len) ? 1 : 0;

        if (first) {
            printf("\033_Ga=T,i=1,f=24,o=z,q=2,s=%d,v=%d,m=%d;", driver->w, driver->h, more);
            first = 0;
        } else {
            printf("\033_Gm=%d;", more);
        }

        fwrite(driver->base64_buf + offset, 1, chunk, stdout);
        printf("\033\\");

        offset += chunk;
    }
    fflush(stdout);
}

static int KITTY_Flip(KITTY_Driver *driver)
{
    int x, y;
    unsigned char *src = driver->buffer;
    unsigned char *dst = driver->bitmap;

    /* XRGB (32bpp) → RGB (24bpp) conversion
     * In memory (little-endian): B G R X → extract R, G, B */
    for (y = 0; y < driver->h; y++) {
        unsigned char *src_row = src + y * driver->pitch;
        unsigned char *dst_row = dst + y * driver->w * 3;
        for (x = 0; x < driver->w; x++) {
            dst_row[0] = src_row[2]; /* R */
            dst_row[1] = src_row[1]; /* G */
            dst_row[2] = src_row[0]; /* B */
            src_row += 4;
            dst_row += 3;
        }
    }

    /* Move cursor to top-left */
    printf("\033[H");

    /* Send frame via Kitty Graphics Protocol */
    kitty_send_frame(driver);

    return 0;
}


static void KITTY_UpdateRects(KITTY_Driver *driver, int numrects, pixman_box16_t *rects)
{
    (void)numrects;
    (void)rects;
    KITTY_Flip(driver);
}


static Bool kittyMapFramebuffer(KdScreenInfo *screen)
{
    KITTY_Driver *driver = screen->driver;
    KdPointerMatrix m;

    /* Always use a shadow framebuffer managed by KDrive/fb (32bpp).
     * We convert to RGB for SIXEL in the update path. */
    driver->shadow = TRUE;

    KdComputePointerMatrix (&m, driver->randr, screen->width, screen->height);

    KdSetPointerMatrix (&m);

    screen->width = driver->w;
    screen->height = driver->h;

    TRACE2("%s: shadow %d\n", __func__, driver->shadow);

    if (!KdShadowFbAlloc(screen,
                         driver->randr & (RR_Rotate_90|RR_Rotate_270)))
        return FALSE;

    return TRUE;
}

static void
kittySetScreenSizes(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    KITTY_Driver *driver = screen->driver;

    if (driver->randr & (RR_Rotate_0|RR_Rotate_180))
    {
        pScreen->width = driver->w;
        pScreen->height = driver->h;
        pScreen->mmWidth = screen->width_mm;
        pScreen->mmHeight = screen->height_mm;
    }
    else
    {
        pScreen->width = driver->h;
        pScreen->height = driver->w;
        pScreen->mmWidth = screen->height_mm;
        pScreen->mmHeight = screen->width_mm;
    }
}

static Bool
kittyUnmapFramebuffer(KdScreenInfo *screen)
{
    KdShadowFbFree (screen);
    return TRUE;
}

static Bool kittyScreenInit(KdScreenInfo *screen)
{
    KITTY_Driver *driver;
    struct winsize ws;

    TRACE1("%s\n", __func__);

    if (!screen->width || !screen->height)
    {
        screen->width = 640;
        screen->height = 480;
    }
    /* Force a sane default pixel format (depth 24, bpp 32, XRGB8888). */
    screen->fb.depth = 24;
    screen->fb.bitsPerPixel = 32;
    screen->fb.visuals = (1 << TrueColor);
    screen->fb.redMask   = 0x00ff0000;
    screen->fb.greenMask = 0x0000ff00;
    screen->fb.blueMask  = 0x000000ff;

    driver = g_driver = calloc(1, sizeof(KITTY_Driver));

    TRACE3("Attempting for %dx%d/%dbpp mode\n",
           screen->width, screen->height, screen->fb.depth);

    /* Set up the new mode framebuffer */
    driver->w = screen->width;
    driver->h = screen->height;
    driver->pixel_w = 0;
    driver->pixel_h = 0;
    driver->cell_w = 0;
    driver->cell_h = 0;
    /* driver->pitch used by shadow window refers to 32bpp buffer stride */

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    driver->pixel_w = ws.ws_xpixel;
    driver->pixel_h = ws.ws_ypixel;
    driver->cell_w = ws.ws_col;
    driver->cell_h = ws.ws_row;

    /* require dtterm response (KITTY_DTTERM_SEQS) */
    if (driver->cell_w <= 0 || driver->cell_h <= 0) {
        printf("\033[18t");
    }
    if (driver->pixel_w <= 0 || driver->pixel_h <= 0) {
        printf("\033[14t");
    }

    /* 32bpp shadow destination buffer for shadowUpdatePacked */
    driver->pitch = screen->width * 4;
    driver->buffer = calloc(1, driver->pitch * screen->height);
    if (!driver->buffer) {
        printf("Couldn't allocate buffer for requested mode\n");
        return FALSE;
    }
    /* 3-bytes-per-pixel RGB staging buffer for Kitty Graphics encoder */
    driver->bitmap = calloc(1, 3 * screen->width * screen->height);
    if (!driver->bitmap) {
        printf("Couldn't allocate buffer for requested mode\n");
        return FALSE;
    }

    /* zlib output buffer: compressBound gives worst-case compressed size */
    {
        uLong rgb_size = driver->w * driver->h * 3;
        driver->zlib_buf_size = compressBound(rgb_size);
        driver->zlib_buf = malloc(driver->zlib_buf_size);
        if (!driver->zlib_buf) {
            fprintf(stderr, "KITTY: failed to allocate zlib buffer (%zu bytes)\n",
                    driver->zlib_buf_size);
            return FALSE;
        }
    }

    /* base64 buffer sized for worst-case zlib output */
    driver->base64_buf_size = ((driver->zlib_buf_size + 2) / 3) * 4 + 1;
    driver->base64_buf = malloc(driver->base64_buf_size);
    if (!driver->base64_buf) {
        fprintf(stderr, "KITTY: failed to allocate base64 buffer (%zu bytes)\n",
                driver->base64_buf_size);
        return FALSE;
    }

    driver->randr = screen->randr;
    screen->driver = driver;

    TRACE3("Set %dx%d/%dbpp mode\n", driver->w, driver->h, screen->fb.depth);

    /* Keep visuals/masks/bpp consistent with XRGB8888 */
    screen->fb.visuals = (1 << TrueColor);
    screen->fb.redMask = 0x00ff0000;
    screen->fb.greenMask = 0x0000ff00;
    screen->fb.blueMask = 0x000000ff;
    screen->fb.bitsPerPixel = 32;
    /* Keep shadow enabled so KDrive issues shadow updates */
    screen->fb.shadow = TRUE;
    screen->rate = 30;

    printf("\033]1;Freedesktop.org X server on Kitty Graphics\007");
    return kittyMapFramebuffer(screen);
}

static void kittyShadowUpdate(ScreenPtr pScreen, shadowBufPtr pBuf)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    KITTY_Driver *driver = screen->driver;
    pixman_box16_t *rects;
    int numrects;

    if (driver->shadow)
    {
        shadowUpdatePacked(pScreen, pBuf);
    }

    rects = pixman_region_rectangles(&pBuf->pDamage->damage, &numrects);
    KITTY_UpdateRects(driver, numrects, rects);
}

static void *kittyShadowWindow(ScreenPtr pScreen, CARD32 row, CARD32 offset, int mode, CARD32 *size, void *closure)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    KITTY_Driver *driver = screen->driver;

    if (!pScreenPriv->enabled) {
        return NULL;
    }

    *size = driver->pitch;

    TRACE1("%s\n", __func__);

    return (void *)((CARD8 *)driver->buffer + row * (*size) + offset);
}


static Bool kittyCreateRes(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    KITTY_Driver *driver = screen->driver;

    TRACE1("%s\n", __func__);

    /*
     * Hack: Kdrive assumes we have dumb videobuffer, which updates automatically,
     * and does not call update callback if shadow flag is not set.
     */
    screen->fb.shadow = TRUE;
    KdShadowSet(pScreen, driver->randr, kittyShadowUpdate, kittyShadowWindow);

    return TRUE;
}


#ifdef RANDR
static Bool kittyRandRGetInfo(ScreenPtr pScreen, Rotation *rotations)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    KITTY_Driver *driver = screen->driver;
    RRScreenSizePtr pSize;
    Rotation randr;
    int n;

    TRACE1("%s", __func__);

    *rotations = RR_Rotate_All|RR_Reflect_All;

    for (n = 0; n < pScreen->numDepths; n++)
        if (pScreen->allowedDepths[n].numVids)
            break;
    if (n == pScreen->numDepths) {
        return FALSE;
    }

    pSize = RRRegisterSize(pScreen,
                           screen->width,
                           screen->height,
                           screen->width_mm,
                           screen->height_mm);

    randr = KdSubRotation(driver->randr, screen->randr);

    RRSetCurrentConfig(pScreen, randr, 0, pSize);

    return TRUE;
}

static Bool kittyRandRSetConfig(ScreenPtr pScreen,
                                Rotation randr,
                                int rate,
                                RRScreenSizePtr pSize)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    KITTY_Driver *driver = screen->driver;
    Bool wasEnabled = pScreenPriv->enabled;
    KITTY_Driver oldDriver;
    int oldwidth;
    int oldheight;
    int oldmmwidth;
    int oldmmheight;

    if (wasEnabled) {
        KdDisableScreen (pScreen);
    }

    oldDriver = *driver;

    oldwidth = screen->width;
    oldheight = screen->height;
    oldmmwidth = pScreen->mmWidth;
    oldmmheight = pScreen->mmHeight;

    /*
     * Set new configuration
     */

    driver->randr = KdAddRotation(screen->randr, randr);

    TRACE2("%s driver->randr %d", __func__, driver->randr);

    kittyUnmapFramebuffer(screen);

    if (!kittyMapFramebuffer(screen)) {
        goto bail4;
    }

    KdShadowUnset(screen->pScreen);

    if (!kittyCreateRes(screen->pScreen)) {
        goto bail4;
    }

    kittySetScreenSizes(screen->pScreen);

    /*
     * Set frame buffer mapping
     */
    (*pScreen->ModifyPixmapHeader)(fbGetScreenPixmap(pScreen),
                                   pScreen->width,
                                   pScreen->height,
                                   screen->fb.depth,
                                   screen->fb.bitsPerPixel,
                                   screen->fb.byteStride,
                                   screen->fb.frameBuffer);

    /* set the subpixel order */

    KdSetSubpixelOrder(pScreen, driver->randr);
    if (wasEnabled) {
        KdEnableScreen(pScreen);
    }

    return TRUE;

bail4:
    kittyUnmapFramebuffer(screen);
    *driver = oldDriver;
    (void) kittyMapFramebuffer(screen);
    pScreen->width = oldwidth;
    pScreen->height = oldheight;
    pScreen->mmWidth = oldmmwidth;
    pScreen->mmHeight = oldmmheight;

    if (wasEnabled) {
        KdEnableScreen (pScreen);
    }
    return FALSE;
}

static Bool kittyRandRInit(ScreenPtr pScreen)
{
    rrScrPrivPtr pScrPriv;

    TRACE1("%s", __func__);

    if (!RRScreenInit(pScreen)) {
        return FALSE;
    }

    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = kittyRandRGetInfo;
    pScrPriv->rrSetConfig = kittyRandRSetConfig;
    return TRUE;
}
#endif


static Bool kittyFinishInitScreen(ScreenPtr pScreen)
{
    if (!shadowSetup(pScreen)) {
        return FALSE;
    }

#ifdef RANDR
    if (!kittyRandRInit(pScreen)) {
        return FALSE;
    }
#endif
    return TRUE;
}

static void kittyKeyboardFini(KdKeyboardInfo *ki)
{
    TRACE1("kittyKeyboardFini() %p\n", ki);
    kittyKeyboard = NULL;
}

static Status kittyKeyboardInit(KdKeyboardInfo *ki)
{
    ki->minScanCode = 8;
    ki->maxScanCode = 255;
    free(ki->name);
    free(ki->xkbRules);
    free(ki->xkbModel);
    free(ki->xkbLayout);
    ki->name = strdup("Kitty terminal keyboard");
    ki->xkbRules = strdup("evdev");
    ki->xkbModel = strdup("pc105");
    ki->xkbLayout = strdup("us");
    kittyKeyboard = ki;
    TRACE1("kittyKeyboardInit() %p\n", ki);
    return Success;
}

static Status kittyKeyboardEnable(KdKeyboardInfo *ki)
{
    return Success;
}

static void kittyKeyboardDisable(KdKeyboardInfo *ki)
{
}

static void kittyKeyboardLeds(KdKeyboardInfo *ki, int leds)
{
}

static void kittyKeyboardBell(KdKeyboardInfo *ki, int volume, int frequency, int duration)
{
}

static Status kittyMouseInit(KdPointerInfo *pi)
{
    pi->nButtons = 7;
    pi->name = strdup("Kitty terminal mouse");
    kittyPointer = pi;
    TRACE1("kittyMouseInit() %p\n", pi);
    return Success;
}

static void kittyMouseFini(KdPointerInfo *pi)
{
    TRACE1("kittyMouseFini() %p\n", pi);
    kittyPointer = NULL;
}

static Status kittyMouseEnable(KdPointerInfo *pi)
{
    /* TODO */
    return Success;
}

static void kittyMouseDisable(KdPointerInfo *pi)
{
    /* TODO */
    return;
}

void InitCard(char *name)
{
    KdCardInfoAdd(&kittyFuncs,  0);
    TRACE1("InitCard: %s\n", name);
}

void InitOutput(ScreenInfo *pScreenInfo, int argc, char **argv)
{
    KdInitOutput(pScreenInfo, argc, argv);
    signal(SIGHUP, SIG_DFL);
    TRACE("InitOutput()\n");
}

void InitInput(int argc, char **argv)
{
    KdPointerInfo *pi;
    KdKeyboardInfo *ki;

    KdAddKeyboardDriver(&kittyKeyboardDriver);
    KdAddPointerDriver(&kittyMouseDriver);

    ki = KdNewKeyboard();
    if (ki) {
        ki->name = strdup("KDrive Keyboard");
        ki->driverPrivate = strdup("keyboard");
        KdAddKeyboard(ki);
    }
    pi = KdNewPointer();
    if (pi) {
        pi->name = strdup("KDrive Pointer");
        pi->driverPrivate = strdup("mouse");
        pi->nButtons = 5;
        pi->inputClass = KD_MOUSE;
        pi->emulateMiddleButton = kdEmulateMiddleButton;
        pi->transformCoordinates = !kdRawPointerCoordinates;
        KdAddPointer(pi);
    }

    KdInitInput();
}

#ifdef DDXBEFORERESET
void ddxBeforeReset(void)
{
}
#endif

void ddxUseMsg(void)
{
    KdUseMsg();
    ErrorF("-kitty-transfer t|d   Kitty Graphics transfer mode (default: t=tempfile)\n");
    ErrorF("-nocompress            Disable zlib compression (send raw RGB)\n");
}

int ddxProcessArgument(int argc, char **argv, int i)
{
    if (!strcmp(argv[i], "-kitty-transfer")) {
        if (i + 1 < argc) {
            if (!strcmp(argv[i + 1], "d"))
                kitty_use_tempfile = 0;
            else if (!strcmp(argv[i + 1], "t"))
                kitty_use_tempfile = 1;
            else
                UseMsg();
            return 2;
        }
        UseMsg();
        return 0;
    }
    if (!strcmp(argv[i], "-nocompress")) {
        kitty_compress = 0;
        return 1;
    }
    return KdProcessArgument(argc, argv, i);
}

static void kittyPollInput(void)
{
    int posted = 0;
    static int prev_x = -1, prev_y = -1;
    static int mouse_x = -1, mouse_y = -1;
#if KITTY_DEBUG
    static int events = 0;
#endif
    char buf[4096];
    static kitty_key_t keys[4096];
    int scancode;
    kitty_key_t *key;
    int nread, nkeys;
    int i;
    int state;

    nread = get_input(buf, sizeof(buf));
    if (nread > 0) {
        nkeys = getkeys(buf, nread, keys);
        if (nkeys >= sizeof(keys) / sizeof(*keys)) {
            nkeys = sizeof(keys) / sizeof(*keys) - 1;
        }
        for (i = 0; i < nkeys; ++i) {
            key = keys + i;
            switch (key->value) {
            case KITTY_DTTERM_SEQS:
                if (g_driver) {
                    switch (key->params[0]) {
                    case 4:
                        g_driver->pixel_h = key->params[1];
                        g_driver->pixel_w = key->params[2];
                        break;
                    case 8:
                        g_driver->cell_h = key->params[1];
                        g_driver->cell_w = key->params[2];
                        break;
                    default:
                        break;
                    }
                }
                break;

            case KITTY_MOUSE_SGR:
            case KITTY_MOUSE_SGR_RELEASE:
            {
                int cb = key->params[0];
                int px = key->params[1];
                int py = key->params[2];
                int is_release = (key->value == KITTY_MOUSE_SGR_RELEASE);
                int is_motion = (cb & 32);
                int is_wheel  = (cb & 64);
                int button    = (cb & 3);

                if (is_wheel) {
                    /* scroll wheel: button 0=up(KD_BUTTON_4), 1=down(KD_BUTTON_5) */
                    int btn_flag = (button == 0) ? KD_BUTTON_4 : KD_BUTTON_5;
                    KdEnqueuePointerEvent(kittyPointer, mouseState | btn_flag, px, py, 0);
                    KdEnqueuePointerEvent(kittyPointer, mouseState, px, py, 0);
                } else if (is_motion) {
                    /* mouse motion */
                    KdEnqueuePointerEvent(kittyPointer, mouseState, px, py, 0);
                } else {
                    /* button press/release */
                    int btn_flag = 0;
                    switch (button) {
                        case 0: btn_flag = KD_BUTTON_1; break;
                        case 1: btn_flag = KD_BUTTON_2; break;
                        case 2: btn_flag = KD_BUTTON_3; break;
                    }
                    if (is_release) {
                        mouseState &= ~btn_flag;
                    } else {
                        mouseState |= btn_flag;
                    }
                    KdEnqueuePointerEvent(kittyPointer, mouseState, px, py, 0);
                }
                mouse_x = px;
                mouse_y = py;
                break;
            }

            case KITTY_FKEYS:
                /* TODO: modifyFunctionKeys */
                switch (key->params[0]) {
                case 2:  scancode = 110+8; break;
                case 3:  scancode = 111+8; break;
                case 5:  scancode = 104+8; break;
                case 6:  scancode = 109+8; break;
                case 7:  scancode = 102+8; break;
                case 8:  scancode = 107+8; break;
                case 11: scancode =  59+8; break;
                case 12: scancode =  60+8; break;
                case 13: scancode =  61+8; break;
                case 14: scancode =  62+8; break;
                case 15: scancode =  63+8; break;
                case 17: scancode =  64+8; break;
                case 18: scancode =  65+8; break;
                case 19: scancode =  66+8; break;
                case 20: scancode =  67+8; break;
                case 21: scancode =  68+8; break;
                case 23: scancode =  87+8; break;
                case 24: scancode =  88+8; break;
                default:
                    scancode = 0;
                    break;
                }
                if (key->nparams == 2) {
                    key->params[1]--;
                    SendModifierKey(key->params[1], 0);
                }
                KdEnqueueKeyboardEvent(kittyKeyboard, scancode, 0);
                if (key->nparams == 2) {
                    SendModifierKey(key->params[1], 1);
                }
                KdEnqueueKeyboardEvent(kittyKeyboard, scancode, 1);
                break;
            case KITTY_FOCUSIN:
                KITTY_Flip(g_driver);
                break;
            case KITTY_FOCUSOUT:
                /* TODO: */
                /* KITTY_Flip(g_driver); */
                break;
            default:
                if ((key->value >= KITTY_UP && key->value <= KITTY_LEFT) ||
                    (key->value >= KITTY_END && key->value <= KITTY_HOME) ||
                    (key->value >= KITTY_F1 && key->value <= KITTY_F4)) {
                    /* TODO: modifyCursorKeys, modifyOtherKeys */
                    switch(key->value) {
                    case KITTY_UP:    scancode = 103+8; break;
                    case KITTY_DOWN:  scancode = 108+8; break;
                    case KITTY_RIGHT: scancode = 106+8; break;
                    case KITTY_LEFT:  scancode = 105+8; break;
                    case KITTY_HOME:  scancode = 102+8; break;
                    case KITTY_END:   scancode = 107+8; break;
                    case KITTY_F1:    scancode =  59+8; break;
                    case KITTY_F2:    scancode =  60+8; break;
                    case KITTY_F3:    scancode =  61+8; break;
                    case KITTY_F4:    scancode =  62+8; break;
                    default:
                        scancode = 0;
                        break;
                    }
                    if (key->nparams >= 1) {
                        key->params[key->nparams-1]--;
                        posted += SendModifierKey(key->params[key->nparams-1], 0);
                    }
                    KdEnqueueKeyboardEvent(kittyKeyboard, scancode, 0);
                    if (key->nparams >= 1) {
                        posted += SendModifierKey(key->params[key->nparams-1], 1);
                    }
                    KdEnqueueKeyboardEvent(kittyKeyboard, scancode, 1);
                }
                else {
                    state = GetState(key->value);
                    scancode = TranslateKey(key->value);
                    if (state) {
                        SendModifierKey(state, 0);
                    }
                    KdEnqueueKeyboardEvent(kittyKeyboard, scancode, 0);
                    if (state) {
                        SendModifierKey(state, 1);
                    }
                    KdEnqueueKeyboardEvent(kittyKeyboard, scancode, 1);
                    if (key->value == 0x0c) {
                        KITTY_Flip(g_driver);
                    }
                }
                break;
            }
        }
    }
    if (prev_x != mouse_x || prev_y != mouse_y) {
        KdEnqueuePointerEvent(kittyPointer, mouseState, mouse_x, mouse_y, 0);
        prev_x = mouse_x;
        prev_y = mouse_y;
    }
}

static int kittyInit(void)
{
    tty_raw();

    /* alternate screen */
    printf("\033[?1049h");

    /* カーソル非表示 */
    printf("\033[?25l");

    /* フォーカスイベント有効化 */
    printf("\033[?1004h");

    /* SGR mouse: all motion + pixel coordinates */
    printf("\033[?1003h");
    printf("\033[?1016h");

    fflush(stdout);

    return 0;
}


static void kittyFini(void)
{
    fd_set fdset;
    struct timeval timeout;
    char buf[4096];

    printf("\033\\");
    fflush(stdout);

    printf("\033[?1016l");
    printf("\033[?1003l");
    printf("\033[?1004l");
    printf("\033[?25h");    /* カーソル表示 */
    printf("\033[?1049l");  /* alternate screen 復元 */
    fflush(stdout);

    FD_ZERO(&fdset);
    FD_SET(STDIN_FILENO, &fdset);
    timeout.tv_sec = 0;
    timeout.tv_usec = 10000;

    if (select(STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout) == 1) {
        while (read(STDIN_FILENO, buf, sizeof(buf))) {
            ;
        }
    }
    tty_restore();

    if (g_driver) {
        free(g_driver->zlib_buf);
        free(g_driver->base64_buf);
        free(g_driver->bitmap);
        free(g_driver->buffer);
        free(g_driver);
    }
}

static void
kittyBell(int volume, int pitch, int duration)
{
    if (volume && pitch)
        printf("\007");
}

void CloseInput(void)
{
    KdCloseInput();
    kittyFini();
}

static void kittyNotifyFd(int fd, int ready, void *data)
{
    (void)fd; (void)ready; (void)data;
    kittyPollInput();
}

void OsVendorInit(void)
{
    /* Initialize terminal + kitty, then hook stdin for input */
    kittyInit();
    SetNotifyFd(STDIN_FILENO, kittyNotifyFd, X_NOTIFY_READ, NULL);
}

/* Input thread hook (may be called by os/inputthread.c) */
void ddxInputThreadInit(void)
{
    /* Nothing special needed for kitty */
}
