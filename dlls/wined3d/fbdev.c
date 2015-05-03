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

    var = getenv("WINE_FBDEV_DEV");
    if (var != NULL)
        devname = var;

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

    for (; buffers > 0; buffers--) {
        fbdev.mem = mmap(0, 800*480*2 * buffers, PROT_WRITE|PROT_READ,
                         MAP_SHARED, fbdev.fd, 0);
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

    ret = ioctl(fbdev.fd, FBIOGET_VSCREENINFO, &fbdev.fbvar);
    if (ret == -1) {
        perror("FBIOGET_VSCREENINFO ioctl");
        fbdev.init_failed = 1;
        return;
    }

    // shouldn't change on resolution changes
    fbdev.pitch = fbdev.fbvar.xres_virtual;

    fbdev.height = fbdev.fbvar.yres;

    // fbdev_to_screen assumes 16bpp...
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
            perror("FBIOPUT_VSCREENINFO ioctl");
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

// vsync (only if we are fast enough, assumes 60fps screen)
static void fbdev_do_vsync(void)
{
    static int estimate_x3;
    struct timeval tv;
    int now_x3, diff, arg = 0;

    gettimeofday(&tv, NULL);
    now_x3 = (tv.tv_sec * 1000000 + tv.tv_usec) * 3;
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

    ioctl(fbdev.fd, FBIO_WAITFORVSYNC, &arg);
}

int fbdev_to_screen(struct wined3d_surface *surface, const RECT *rect)
{
    int left, right, top, bottom, pitch;
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

    if (surface->resource.format->byte_count != 1)
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

    mypal = surface->fbdev->pal;
    dst = fbdev.mem + fbdev.buf_n * fbdev.pitch * fbdev.height
           + top * fbdev.pitch + left;
    src += top * pitch + left;
    w = right - left;

    //ERR("loc %x %d,%d %d,%d\n", surface->locations, left, top, right, bottom);

    for (h = bottom - top; h > 0; h--) {
        for (i = 0; i + 3 < w; i += 4) {
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
