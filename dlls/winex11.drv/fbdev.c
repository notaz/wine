/*
 * fbdev hack
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

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/omapfb.h>
#include <errno.h>

#include "x11drv.h"

#include "windef.h"
#include "wingdi.h"
#include "wine/debug.h"
#include "wine/library.h"

WINE_DEFAULT_DEBUG_CHANNEL(fbdev);

int input_pos_ofs_x;
int input_pos_ofs_y;
int input_pos_mul_x = 1 << 16;
int input_pos_mul_y = 1 << 16;
int input_rightclick_modifier;
int input_rightclick_hack_on;

#define MAX_WIDTH 800
#define MAX_HEIGHT 600
#define MODE_COUNT 12

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480

enum scaling_mode {
  SMODE_ONLY_DOWN = 0,
  SMODE_ONLY_FULLSCREEN,
  SMODE_ONLY_4_3,
};

static struct {
  struct x11drv_mode_info *dd_modes;
  int current_mode;
  enum scaling_mode scaler_mode;
  int fd, scaler_fd;
  int scaler_bufs;
} g;


static int scaler_resize_fb(int fd, int w, int h)
{
  struct fb_var_screeninfo fbvar;
  int ret;

  ret = ioctl(fd, FBIOGET_VSCREENINFO, &fbvar);
  if (ret == -1) {
    perror("scaler: FBIOGET_VSCREENINFO");
    return ret;
  }

  fbvar.xoffset = 0;
  fbvar.yoffset = 0;
  fbvar.xres = w;
  fbvar.yres = h;
  fbvar.xres_virtual = MAX_WIDTH; // wined3d/fbdev.c expects a constant
  fbvar.yres_virtual = MAX_HEIGHT * g.scaler_bufs;

  ret = ioctl(fd, FBIOPUT_VSCREENINFO, &fbvar);
  if (ret == -1)
    perror("scaler: FBIOPUT_VSCREENINFO");

  return ret;
}

static int scaler_prepare(void)
{
  const char *devname = "/dev/fb1";
  struct omapfb_plane_info pi;
  struct omapfb_mem_info mi;
  unsigned int size, size_cur;
  int mem_blocks = 3;
  int fd, ret;

  fd = open(devname, O_RDWR);
  if (fd == -1) {
    ERR("scaler: open %s: %s\n", devname, strerror(errno));
    return -1;
  }

  memset(&pi, 0, sizeof(pi));
  memset(&mi, 0, sizeof(mi));

  ret = ioctl(fd, OMAPFB_QUERY_PLANE, &pi);
  if (ret != 0) {
    perror("scaler: QUERY_PLANE");
    goto out;
  }

  ret = ioctl(fd, OMAPFB_QUERY_MEM, &mi);
  if (ret != 0) {
    perror("scaler: QUERY_MEM");
    goto out;
  }
  size_cur = mi.size;

  /* must disable when changing stuff */
  if (pi.enabled) {
    pi.enabled = 0;
    ret = ioctl(fd, OMAPFB_SETUP_PLANE, &pi);
    if (ret != 0)
      perror("scaler: SETUP_PLANE");
  }

  /* allocate more mem, if needed */
  size = MAX_WIDTH * MAX_HEIGHT * 2;
  for (; size_cur < size * mem_blocks && mem_blocks > 0; mem_blocks--) {
    mi.size = size * mem_blocks;
    ret = ioctl(fd, OMAPFB_SETUP_MEM, &mi);
    if (ret == 0)
      break;
    mi.size = size_cur;
  }
  if (mem_blocks <= 0) {
    ERR("scaler: failed to allocate at least %d bytes of vram:\n", size);
    perror("scaler: SETUP_MEM");
    ret = -1;
    goto out;
  }
  g.scaler_bufs = mem_blocks;

  pi.enabled = 0;

  ret = ioctl(fd, OMAPFB_SETUP_PLANE, &pi);
  if (ret != 0) {
    perror("scaler: SETUP_PLANE");
    goto out;
  }

  ret = scaler_resize_fb(fd, 640, 480);

out:
  if (ret != 0) {
    close(fd);
    fd = -1;
  }
  return fd;
}

static void scaler_setup(int fd, int w, int h, int enable)
{
  struct omapfb_plane_info pi;
  int ret;

  memset(&pi, 0, sizeof(pi));
  ret = ioctl(fd, OMAPFB_QUERY_PLANE, &pi);
  if (ret != 0) {
    perror("scaler: QUERY_PLANE");
    return;
  }

  if (enable) {
    switch (g.scaler_mode) {
    case SMODE_ONLY_DOWN:
    default:
      pi.out_width = w;
      pi.out_height = h;
      break;
    case SMODE_ONLY_FULLSCREEN:
      pi.out_width = SCREEN_WIDTH;
      pi.out_height = SCREEN_HEIGHT;
      break;
    case SMODE_ONLY_4_3:
      pi.out_width = SCREEN_HEIGHT * 4 / 3;
      pi.out_height = SCREEN_HEIGHT;
      break;
    }
    if (pi.out_width > SCREEN_WIDTH)
      pi.out_width = SCREEN_WIDTH;
    if (pi.out_height > SCREEN_HEIGHT)
      pi.out_height = SCREEN_HEIGHT;
    pi.pos_x = (SCREEN_WIDTH - pi.out_width) / 2;
    pi.pos_y = (SCREEN_HEIGHT - pi.out_height) / 2;

    input_pos_ofs_x = -pi.pos_x;
    input_pos_ofs_y = -pi.pos_y;
    input_pos_mul_x = (w << 16) / pi.out_width;
    input_pos_mul_y = (h << 16) / pi.out_height;
  }

  pi.enabled = enable;
  ret = ioctl(fd, OMAPFB_SETUP_PLANE, &pi);
  if (ret != 0)
    perror("scaler: SETUP_PLANE");
}

static void scaler_update(int fd, int w, int h)
{
  if (w == SCREEN_WIDTH && h == SCREEN_HEIGHT) {
    // assume that the game entered windowed mode
    scaler_setup(fd, w, h, 0);
    g.current_mode = -1;
    return;
  }

  scaler_resize_fb(fd, w, h);
  scaler_setup(fd, w, h, 1);
}

static int XVidModeErrorHandler(Display *dpy, XErrorEvent *event, void *arg)
{
    return 1;
}

static int X11DRV_fbdev_GetCurrentMode(void)
{
  struct fb_var_screeninfo fbvar;
  unsigned int dd_mode_count = X11DRV_Settings_GetModeCount();
  unsigned int i;
  int ret;

  // the real fb resolution is not necessarily what wine was told
  // it is, so just return cached mode
  if (g.current_mode >= 0)
    return g.current_mode;

  ret = ioctl(g.fd, FBIOGET_VSCREENINFO, &fbvar);
  if (ret == -1) {
    perror("FBIOGET_VSCREENINFO ioctl");
    goto out;
  }

  for (i = 0; i < dd_mode_count; i++) {
    if (g.dd_modes[i].width == fbvar.xres
      && g.dd_modes[i].height == fbvar.yres
      && g.dd_modes[i].bpp == screen_bpp)
    {
      TRACE("mode=%d\n", i);
      return i;
    }
  }

out:
  ERR("In unknown mode, returning default\n");
  return 0;
}

static LONG X11DRV_fbdev_SetCurrentMode(int mode)
{
  struct fb_var_screeninfo fbvar;
  struct omapfb_plane_info pi = { 0, };
  int newpos_x = 0, newpos_y = 0;
  int scaler_needed = 0;
  int width, height;
  int pos_set = 0;
  int ret;

  g.current_mode = -1;
  mode = mode % MODE_COUNT;

  /* only set modes from the original color depth */
  if (screen_bpp != g.dd_modes[mode].bpp)
  {
      FIXME("Cannot change screen BPP from %d to %d\n",
        screen_bpp, g.dd_modes[mode].bpp);
  }

  width = g.dd_modes[mode].width, height = g.dd_modes[mode].height;
  MESSAGE("Resizing X display to %dx%d@%d\n",
          width, height, g.dd_modes[mode].bpp);

  ret = ioctl(g.fd, FBIOGET_VSCREENINFO, &fbvar);
  if (ret == -1) {
    perror("FBIOGET_VSCREENINFO ioctl");
    return DISP_CHANGE_FAILED;
  }

  ret = ioctl(g.fd, OMAPFB_QUERY_PLANE, &pi);
  if (ret != 0) {
    perror("QUERY_PLANE");
    return DISP_CHANGE_FAILED;
  }

  if (width < SCREEN_WIDTH)
    newpos_x = (SCREEN_WIDTH - width) / 2;
  if (height < SCREEN_HEIGHT)
    newpos_y = (SCREEN_HEIGHT - height) / 2;
  if (newpos_x < pi.pos_x || newpos_y < pi.pos_y) {
    pi.pos_x = newpos_x;
    pi.pos_y = newpos_y;
    ret = ioctl(g.fd, OMAPFB_SETUP_PLANE, &pi);
    if (ret != 0)
      perror("OMAPFB_SETUP_PLANE ioctl");
    pos_set = (ret != 0);
  }

  // keep virtual resolution as-is to not break xorg,
  // wine still seems to use the new resolution for directx,
  // which is what we want
  fbvar.xres = width;
  fbvar.yres = height;
  // asume fb with g.fd can't go above screen res, but can go below
  if (fbvar.xres > SCREEN_WIDTH) {
    fbvar.xres = SCREEN_WIDTH;
    scaler_needed = 1;
  }
  if (fbvar.yres > SCREEN_HEIGHT) {
    fbvar.yres = SCREEN_HEIGHT;
    scaler_needed = 1;
  }
  fbvar.bits_per_pixel = screen_bpp;
  //fbvar.bits_per_pixel = g.dd_modes[mode].bpp;
  fbvar.xoffset = fbvar.yoffset = 0;

  ret = ioctl(g.fd, FBIOPUT_VSCREENINFO, &fbvar);
  if (ret == -1) {
    perror("FBIOPUT_VSCREENINFO ioctl");
    return DISP_CHANGE_FAILED;
  }

  if (!pos_set) {
    pi.pos_x = newpos_x;
    pi.pos_y = newpos_y;
    pi.out_width = fbvar.xres;
    pi.out_height = fbvar.yres;
    ret = ioctl(g.fd, OMAPFB_SETUP_PLANE, &pi);
    if (ret != 0)
      perror("OMAPFB_SETUP_PLANE ioctl (after)");
  }

  if (g.scaler_fd != -1)
    // will also set up input_pos_*
    scaler_update(g.scaler_fd, width, height);
  else if (scaler_needed)
    ERR("you might want to enable the scaler\n");

  if (g.scaler_fd == -1) {
    input_pos_ofs_x = -newpos_x;
    input_pos_ofs_y = -newpos_y;
    input_pos_mul_x =
    input_pos_mul_y = 1 << 16;
  }

  // ?
  XWarpPointer(gdi_display, None, DefaultRootWindow(gdi_display), 0, 0, 0, 0, 0, 0);
  XSync(gdi_display, False);
  X11DRV_resize_desktop(g.dd_modes[mode].width, g.dd_modes[mode].height);
  g.current_mode = mode;
  return DISP_CHANGE_SUCCESSFUL;
}

void X11DRV_fbdev_Init(void)
{
  const char *devname = "/dev/fb0";
  const char *var;
  int mode_count;

  if (g.fd > 0) return; /* already initialized? */

  g.fd = -1;
  g.scaler_fd = -1;
  g.current_mode = -1;

  var = getenv("WINE_FBDEV_DEV");
  if (var != NULL)
    devname = var;

  g.fd = open(devname, O_RDWR);
  if (g.fd == -1) {
    ERR("open %s: %s\n", devname, strerror(errno));
    return;
  }

  var = getenv("WINE_FBDEV_USE_SCALER");
  if (var != NULL && atoi(var) != 0) {
    g.scaler_fd = scaler_prepare();
    var = getenv("WINE_FBDEV_SCALER_MODE");
    if (var != NULL)
      g.scaler_mode = atoi(var);
  }

  mode_count = MODE_COUNT;
  if (g.scaler_fd == -1)
    mode_count -= 2;

  X11DRV_expect_error(gdi_display, XVidModeErrorHandler, NULL);

  g.dd_modes = X11DRV_Settings_SetHandlers("XF86VidMode", 
                                           X11DRV_fbdev_GetCurrentMode, 
                                           X11DRV_fbdev_SetCurrentMode, 
                                           mode_count, 1);

  // update MODE_COUNT et al. if you change this
  if (mode_count == MODE_COUNT)
    X11DRV_Settings_AddOneMode(800, 600, 16, 60);
  X11DRV_Settings_AddOneMode(800, 480, 16, 60);
  X11DRV_Settings_AddOneMode(640, 480, 16, 60);
  X11DRV_Settings_AddOneMode(640, 400, 16, 60);
  X11DRV_Settings_AddOneMode(400, 300, 16, 60);
  X11DRV_Settings_AddOneMode(320, 240, 16, 60);

  if (mode_count == MODE_COUNT)
    X11DRV_Settings_AddOneMode(800, 600, 8, 60);
  X11DRV_Settings_AddOneMode(800, 480, 8, 60);
  X11DRV_Settings_AddOneMode(640, 480, 8, 60);
  X11DRV_Settings_AddOneMode(640, 400, 8, 60);
  X11DRV_Settings_AddOneMode(400, 300, 8, 60);
  X11DRV_Settings_AddOneMode(320, 240, 8, 60);

  /* add modes for different color depths */
  //X11DRV_Settings_AddDepthModes();

  /* rightclick modifier hack for touchscreen */
  input_rightclick_modifier = XK_VoidSymbol;
  {
    const char *setting = getenv("WINE_RIGHTCLICK_MODIFIER");
    if (setting != NULL)
      input_rightclick_modifier = strtol(setting, NULL, 0);
  }

  TRACE("Enabling fbdev modes\n");
}

// vim:ts=2:sw=2:expandtab
