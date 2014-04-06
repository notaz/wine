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
#include <linux/fb.h>

struct fbdev_state {
    unsigned short pal[256];
};

static int fbdev_init_failed;
static unsigned short *fbdev_mem;
static int fbdev_pitch;

static void fbdev_init(void)
{
    struct fb_var_screeninfo fbvar;
    int fd, ret;

    fd = open("/dev/fb0", O_RDWR);
    if (fd == -1) {
        perror("open /dev/fb0");
        fbdev_init_failed = 1;
        return;
    }
    fbdev_mem = mmap(0, 800*480*2, PROT_WRITE|PROT_READ, MAP_SHARED, fd, 0);
    if (fbdev_mem == MAP_FAILED) {
        perror("mmap /dev/fb0");
        fbdev_init_failed = 1;
        return;
    }

    ret = ioctl(fd, FBIOGET_VSCREENINFO, &fbvar);
    if (ret == -1) {
        perror("FBIOGET_VSCREENINFO ioctl");
        fbdev_init_failed = 1;
        return;
    }

    // shouldn't change on resolution changes
    fbdev_pitch = fbvar.xres_virtual;

    // fbdev_to_screen assumes in 16bpp..
    if (fbvar.bits_per_pixel != 16) {
        FIXME("switching fb to 16bpp..\n");
        fbvar.bits_per_pixel = 16;
        ret = ioctl(fd, FBIOPUT_VSCREENINFO, &fbvar);
        if (ret == -1)
            perror("FBIOPUT_VSCREENINFO ioctl");
    }
}

int fbdev_to_screen(struct wined3d_surface *surface, const RECT *rect)
{
    int left, right, top, bottom, pitch;
    unsigned short *mypal;
    unsigned short *dst;
    unsigned char *src;
    unsigned int v;
    int i, w, h;

    if (fbdev_mem == NULL) {
        if (fbdev_init_failed)
            return 0;

        fbdev_init();

        if (fbdev_init_failed)
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
    dst = fbdev_mem + top * fbdev_pitch + left;
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
        dst += fbdev_pitch;
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
