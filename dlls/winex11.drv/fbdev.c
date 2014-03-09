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

#include "x11drv.h"

#include "windef.h"
#include "wingdi.h"
#include "wine/debug.h"
#include "wine/library.h"

WINE_DEFAULT_DEBUG_CHANNEL(fbdev);

int input_pos_ofs_x;
int input_pos_ofs_y;
int input_rightclick_modifier;
int input_rightclick_hack_on;

#define MODE_COUNT 4
static struct x11drv_mode_info *dd_modes;
static int fd;


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

  ret = ioctl(fd, FBIOGET_VSCREENINFO, &fbvar);
  if (ret == -1) {
    perror("FBIOGET_VSCREENINFO ioctl");
    goto out;
  }

  for (i = 0; i < dd_mode_count; i++) {
    if (dd_modes[i].width == fbvar.xres && dd_modes[i].height == fbvar.yres
      && dd_modes[i].bpp == screen_bpp)
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
  int newpos_x, newpos_y;
  int pos_set = 0;
  int ret;

  mode = mode % MODE_COUNT;

  /* only set modes from the original color depth */
  if (screen_bpp != dd_modes[mode].bpp)
  {
      FIXME("Cannot change screen BPP from %d to %d\n",
        screen_bpp, dd_modes[mode].bpp);
  }

  TRACE("Resizing X display to %dx%d\n", 
        dd_modes[mode].width, dd_modes[mode].height);

  ret = ioctl(fd, FBIOGET_VSCREENINFO, &fbvar);
  if (ret == -1) {
    perror("FBIOGET_VSCREENINFO ioctl");
    return DISP_CHANGE_FAILED;
  }

	ret = ioctl(fd, OMAPFB_QUERY_PLANE, &pi);
	if (ret != 0) {
		perror("QUERY_PLANE");
    return DISP_CHANGE_FAILED;
	}

  newpos_x = (800 - dd_modes[mode].width) / 2;
  newpos_y = (480 - dd_modes[mode].height) / 2;
  if (newpos_x < pi.pos_x || newpos_y < pi.pos_y) {
    pi.pos_x = newpos_x;
    pi.pos_y = newpos_y;
    ret = ioctl(fd, OMAPFB_SETUP_PLANE, &pi);
    if (ret != 0)
      perror("OMAPFB_SETUP_PLANE ioctl");
    pos_set = (ret != 0);
  }

  // keep virtual resolution as-is to not break xorg,
  // wine still seems to use the new resolution for directx,
  // which is what we want
  fbvar.xres = dd_modes[mode].width;
  fbvar.yres = dd_modes[mode].height;
  fbvar.bits_per_pixel = screen_bpp;
  //fbvar.bits_per_pixel = dd_modes[mode].bpp;

  ret = ioctl(fd, FBIOPUT_VSCREENINFO, &fbvar);
  if (ret == -1) {
    perror("FBIOPUT_VSCREENINFO ioctl");
    return DISP_CHANGE_FAILED;
  }

  if (!pos_set) {
    pi.pos_x = newpos_x;
    pi.pos_y = newpos_y;
    pi.out_width = fbvar.xres;
    pi.out_height = fbvar.yres;
    ret = ioctl(fd, OMAPFB_SETUP_PLANE, &pi);
    if (ret != 0)
      perror("OMAPFB_SETUP_PLANE ioctl (after)");
  }

  input_pos_ofs_x = -newpos_x;
  input_pos_ofs_y = -newpos_y;

  // ?
  XWarpPointer(gdi_display, None, DefaultRootWindow(gdi_display), 0, 0, 0, 0, 0, 0);
  XSync(gdi_display, False);
  X11DRV_resize_desktop(dd_modes[mode].width, dd_modes[mode].height);
  return DISP_CHANGE_SUCCESSFUL;
}

void X11DRV_fbdev_Init(void)
{
  if (fd > 0) return; /* already initialized? */

  fd = open("/dev/fb0", O_RDWR);
  if (fd == -1) {
    ERR("open /dev/fb0\n");
    return;
  }

  X11DRV_expect_error(gdi_display, XVidModeErrorHandler, NULL);

  dd_modes = X11DRV_Settings_SetHandlers("XF86VidMode", 
                                         X11DRV_fbdev_GetCurrentMode, 
                                         X11DRV_fbdev_SetCurrentMode, 
                                         MODE_COUNT, 1);

  X11DRV_Settings_AddOneMode(800, 480, 16, 60);
  X11DRV_Settings_AddOneMode(640, 480, 16, 60);
  X11DRV_Settings_AddOneMode(800, 480, 8, 60);
  X11DRV_Settings_AddOneMode(640, 480, 8, 60);

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
