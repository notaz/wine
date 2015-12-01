/*
 * fbdev hack for wined3d
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"
#include "wined3d_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(fbdev);
//WINE_DECLARE_DEBUG_CHANNEL(fps);

#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/fb.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

// placed into wined3d_surface
struct fbdev_state {
    unsigned short pal[256];
};

static struct {
    int init_failed;
    int fd;
    struct fb_var_screeninfo fbvar;
    unsigned short *mem;
    int buffers;
    int buf_n;
    int pitch;
    int height;
    int vsync;
} fbdev;

static void fbdev_init(void)
{
    const char *devname = "/dev/fb0";
    const char *var;
    int fbvar_update = 0;
    int buffers = 1;
    int ret;

    if ((var = getenv("WINE_FBDEV_DEV")))
        devname = var;
    else if ((var = getenv("WINE_FBDEV_USE_SCALER")) && atoi(var) != 0)
        devname = "/dev/fb1";

    var = getenv("WINE_FBDEV_DOUBLEBUF");
    if (var != NULL && atoi(var) != 0)
        buffers = 3;

    fbdev.fd = open(devname, O_RDWR);
    if (fbdev.fd == -1) {
        fprintf(stderr, "open '%s'", devname);
        perror("");
        fbdev.init_failed = 1;
        return;
    }

    ret = ioctl(fbdev.fd, FBIOGET_VSCREENINFO, &fbdev.fbvar);
    if (ret == -1) {
        perror("FBIOGET_VSCREENINFO ioctl");
        fbdev.init_failed = 1;
        return;
    }

    // shouldn't change on resolution changes
    fbdev.pitch = fbdev.fbvar.xres_virtual;

    // hardcoded for now to avoid communicating with winex11
    fbdev.height = 600;

    for (; buffers > 0; buffers--) {
        fbdev.mem = mmap(0, fbdev.pitch * fbdev.height * 2 * buffers,
                         PROT_WRITE|PROT_READ, MAP_SHARED, fbdev.fd, 0);
        if (fbdev.mem == MAP_FAILED) {
            fprintf(stderr, "mmap '%s' %d buffer(s)", devname, buffers);
            perror("");
            continue;
        }
        break;
    }
    if (buffers == 0) {
        fbdev.init_failed = 1;
        return;
    }
    memset(fbdev.mem, 0, fbdev.pitch * fbdev.height * 2 * buffers);

    // fbdev_to_screen assumes 16bpp...
    // note: normally should be already set up by winex11.drv/fbdev.c
    if (fbdev.fbvar.bits_per_pixel != 16) {
        FIXME("switching fb to 16bpp..\n");
        fbdev.fbvar.bits_per_pixel = 16;
        fbvar_update = 1;
    }

    if (fbdev.fbvar.yres_virtual < fbdev.height * buffers) {
        fbdev.fbvar.yres_virtual = fbdev.height * buffers;
        fbvar_update = 1;
    }

    if (fbvar_update) {
        ret = ioctl(fbdev.fd, FBIOPUT_VSCREENINFO, &fbdev.fbvar);
        if (ret == -1)
            perror("wined3d/fbdev: FBIOPUT_VSCREENINFO ioctl");
    }

    var = getenv("WINE_FBDEV_VSYNC");
    if (var != NULL && atoi(var) != 0)
        fbdev.vsync = 1;

    fbdev.buffers = buffers;
}

static void fbdev_do_flip(void)
{
    fbdev.fbvar.yoffset = fbdev.buf_n * fbdev.height;
    ioctl(fbdev.fd, FBIOPAN_DISPLAY, &fbdev.fbvar);

    fbdev.buf_n++;
    if (fbdev.buf_n >= fbdev.buffers)
        fbdev.buf_n = 0;
}

static int get_usec_x3(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000000 + tv.tv_usec) * 3;
}

// vsync (only if we are fast enough, assumes 60fps screen)
static void fbdev_do_vsync(void)
{
    static int estimate_x3;
    int now_x3, diff, arg = 0;

    now_x3 = get_usec_x3();
    estimate_x3 += 50000;
    diff = now_x3 - estimate_x3;

    if (diff > 50000 * 6 || diff < -50000 * 2) {
        // out of sync; restart in "lagging"
        // state to avoid spurious blocking
        estimate_x3 = now_x3 - 50000;
        return;
    }
    if (diff >= 0)
        // too slow
        return;

    // the system timer and LCD use different oscillators, so there is
    // a slow device-specific drift, can't rely on the timer alone
    ioctl(fbdev.fd, FBIO_WAITFORVSYNC, &arg);

    // make the target time a bit before the estimated vsync
    // so that flip time isn't too close to time of hw register fetch
    estimate_x3 = get_usec_x3() - 1000 * 3;
}

#ifdef __arm__
void fbdev_to_screen_asm(unsigned short *dst, unsigned char *src,
  unsigned short *mypal, int w, // r2, r3
  int h, int pitch_d, int pitch_s);
asm("                            \n\
fbdev_to_screen_asm:             \n\
    push  {r4-r10,lr}            \n\
    ldr   r6, [sp, #4*8+4] @ p_d \n\
    ldr   r7, [sp, #4*8+8] @ p_s \n\
    movw  r9, #0x1fe             \n\
    ldr   lr, [sp, #4*8+0] @ h   \n\
    sub   r6, r3, lsl #1         \n\
    sub   r7, r3                 \n\
0:                               \n\
    mov   r8, r3                 \n\
    subs  lr, #1                 \n\
    poplt {r4-r10,pc}            \n\
                                 \n\
1:  ldr   r12, [r1], #4          \n\
    tst   r1, #0x3c              \n\
    and   r4, r9, r12, lsl #1    \n\
    and   r5, r9, r12, lsr #7    \n\
    and   r10,r9, r12, lsr #15   \n\
    and   r12,r9, r12, lsr #23   \n\
    ldrh  r4, [r2, r4]           \n\
    ldrh  r5, [r2, r5]           \n\
    ldrh  r10,[r2, r10]          \n\
    ldrh  r12,[r2, r12]          \n\
    bne   9f                     \n\
    pld   [r1, #64]              \n\
9:  pkhbt r4, r4,  r5,  lsl #16  \n\
    pkhbt r5, r10, r12, lsl #16  \n\
    subs  r8, #4                 \n\
    strd  r4, r5, [r0], #8       \n\
    bgt   1b                     \n\
                                 \n\
    add   r0, r6                 \n\
    add   r1, r7                 \n\
    b     0b                     \n\
");
#endif

int fbdev_to_screen(struct wined3d_surface *surface, const RECT *rect)
{
    int left, right, top, bottom, pitch;
    unsigned int byte_count;
    unsigned short *mypal;
    unsigned short *dst;
    unsigned char *src;
    unsigned int v;
    int i, w, h;

    if (fbdev.mem == NULL) {
        if (fbdev.init_failed)
            return 0;

        fbdev_init();

        if (fbdev.init_failed)
            return 0;
    }

    byte_count = surface->resource.format->byte_count;
    if (byte_count != 1 && byte_count != 2)
        return 0;

    if (surface->locations & WINED3D_LOCATION_DIB)
        src = surface->dib.bitmap_data;
    else if (surface->locations & WINED3D_LOCATION_SYSMEM)
        src = surface->resource.heap_memory;
    else if (surface->locations & WINED3D_LOCATION_USER_MEMORY)
        src = surface->user_memory;
    else {
        ERR("surface->locations %x\n", surface->locations);
        return 0;
    }

    left = 0;
    right = surface->resource.width;
    top = 0;
    bottom = surface->resource.height;
    pitch = wined3d_surface_get_pitch(surface);

    if (rect) {
        if (rect->left > left)
            left = rect->left;
        if (rect->right < right)
            right = rect->right;
        if (rect->top > top)
            top = rect->top;
        if (rect->bottom < bottom)
            bottom = rect->bottom;
    }

    if (surface->fbdev == NULL) {
        ERR("no surface->fbdev?\n");
        return 0;
    }

    src += top * pitch + left * byte_count;
    __builtin_prefetch(src);
    dst = fbdev.mem + fbdev.buf_n * fbdev.pitch * fbdev.height
           + top * fbdev.pitch + left;
    mypal = surface->fbdev->pal;
    w = right - left;
    h = bottom - top;

    //ERR("loc %x %d,%d %d,%d\n", surface->locations, left, top, right, bottom);

    if (byte_count == 1) {
#ifdef __arm__
        if ((((long)dst | fbdev.pitch) & 7) == 0 && (w & 3) == 0)
            fbdev_to_screen_asm(dst, src, mypal, w, h, fbdev.pitch * 2, pitch);
        else
#endif
        for (; h > 0; h--) {
            for (i = 0; i + 3 < w; i += 4) {
                __builtin_prefetch(src + i + 64);
                v = *(unsigned int *)&src[i];
                dst[i + 0] = mypal[(v >>  0) & 0xff];
                dst[i + 1] = mypal[(v >>  8) & 0xff];
                dst[i + 2] = mypal[(v >> 16) & 0xff];
                dst[i + 3] = mypal[(v >> 24)       ];
            }
            for (; i < w; i++)
                dst[i] = mypal[src[i]];

            src += pitch;
            dst += fbdev.pitch;
        }
    }
    else {
        for (; h > 0; h--) {
            memcpy(dst, src, w * 2);

            src += pitch;
            dst += fbdev.pitch;
        }
    }

    h = bottom - top;
    if (w == surface->resource.width
        && h == surface->resource.height)
    {
        if (fbdev.buffers > 1)
            fbdev_do_flip();
        if (fbdev.vsync)
            fbdev_do_vsync();
    }

    return 1;
}

void fbdev_realize_palette(struct wined3d_surface *surface)
{
    struct wined3d_palette *palette = surface->palette;
    unsigned short *mypal;
    int i;

    if (palette == NULL || surface->swapchain == NULL)
        return;

    if (surface->fbdev == NULL) {
        surface->fbdev = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*surface->fbdev));
        if (surface->fbdev == NULL)
            return;
    }

    mypal = surface->fbdev->pal;

    // rrrr rggg gggb bbbb
    for (i = 0; i < 256; i++)
        mypal[i] = ((palette->palents[i].peRed << 8) & 0xf800)
                 | ((palette->palents[i].peGreen << 3) & 0x07e0)
                 |  (palette->palents[i].peBlue >> 3);
}

// vim:ts=4:sw=4:expandtab
