/****************************************************************************
 * contest2026_128_xiaolidianzishiyanshi/app/hello_app/lcd_test_main.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/video/fb.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LCD_TEST_DEFAULT_DEVPATH "/dev/lcd0"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t lcd_test_pixel(fb_coord_t x, fb_coord_t y,
                               fb_coord_t width, fb_coord_t height)
{
  static const uint16_t g_colors[] =
  {
    0xffff, /* White */
    0xffe0, /* Yellow */
    0x07ff, /* Cyan */
    0x07e0, /* Green */
    0xf81f, /* Magenta */
    0xf800, /* Red */
    0x001f, /* Blue */
    0x0000  /* Black */
  };

  fb_coord_t border_x = width / 64;
  fb_coord_t border_y = height / 64;
  unsigned int index;

  if (border_x < 2)
    {
      border_x = 2;
    }

  if (border_y < 2)
    {
      border_y = 2;
    }

  /* A white border makes clipping and panel offsets easy to spot. */

  if (x < border_x || x >= width - border_x ||
      y < border_y || y >= height - border_y)
    {
      return 0xffff;
    }

  /* Draw a black center cross over eight standard RGB565 color bars. */

  if ((x >= width / 2 - 1 && x <= width / 2 + 1) ||
      (y >= height / 2 - 1 && y <= height / 2 + 1))
    {
      return 0x0000;
    }

  index = (unsigned int)((uint32_t)x * 8 / width);
  if (index > 7)
    {
      index = 7;
    }

  return g_colors[index];
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  const char *devpath = LCD_TEST_DEFAULT_DEVPATH;
  struct fb_videoinfo_s vinfo;
  struct lcd_planeinfo_s pinfo;
  struct lcddev_run_s run;
  uint16_t *line;
  fb_coord_t x;
  fb_coord_t y;
  int fd;
  int ret;

  if (argc > 2)
    {
      fprintf(stderr, "Usage: %s [lcd-device]\n", argv[0]);
      return EXIT_FAILURE;
    }

  if (argc == 2)
    {
      devpath = argv[1];
    }

  fd = open(devpath, O_RDWR);
  if (fd < 0)
    {
      fprintf(stderr, "lcd_test: open %s failed: %d\n", devpath, errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, LCDDEVIO_GETVIDEOINFO,
              (unsigned long)(uintptr_t)&vinfo);
  if (ret < 0)
    {
      fprintf(stderr, "lcd_test: GETVIDEOINFO failed: %d\n", errno);
      goto errout_close;
    }

  ret = ioctl(fd, LCDDEVIO_GETPLANEINFO,
              (unsigned long)(uintptr_t)&pinfo);
  if (ret < 0)
    {
      fprintf(stderr, "lcd_test: GETPLANEINFO failed: %d\n", errno);
      goto errout_close;
    }

  printf("lcd_test: %s, %" PRIu32 "x%" PRIu32 ", fmt=%u, bpp=%u\n",
         devpath, (uint32_t)vinfo.xres, (uint32_t)vinfo.yres,
         vinfo.fmt, pinfo.bpp);

  if (vinfo.fmt != FB_FMT_RGB16_565 || pinfo.bpp != 16 ||
      vinfo.xres == 0 || vinfo.yres == 0)
    {
      fprintf(stderr, "lcd_test: only RGB565 LCDs are supported\n");
      goto errout_close;
    }

  line = malloc((size_t)vinfo.xres * sizeof(*line));
  if (line == NULL)
    {
      fprintf(stderr, "lcd_test: line buffer allocation failed\n");
      goto errout_close;
    }

  ret = ioctl(fd, LCDDEVIO_SETPOWER, CONFIG_LCD_MAXPOWER);
  if (ret < 0)
    {
      fprintf(stderr, "lcd_test: SETPOWER failed: %d\n", errno);
      goto errout_free;
    }

  run.col     = 0;
  run.data    = (uint8_t *)line;
  run.npixels = vinfo.xres;

  for (y = 0; y < vinfo.yres; y++)
    {
      for (x = 0; x < vinfo.xres; x++)
        {
          line[x] = lcd_test_pixel(x, y, vinfo.xres, vinfo.yres);
        }

      run.row = y;
      ret = ioctl(fd, LCDDEVIO_PUTRUN,
                  (unsigned long)(uintptr_t)&run);
      if (ret < 0)
        {
          fprintf(stderr, "lcd_test: PUTRUN row %" PRIu32
                  " failed: %d\n", (uint32_t)y, errno);
          goto errout_free;
        }
    }

  printf("lcd_test: color bars drawn successfully\n");
  free(line);
  close(fd);
  return EXIT_SUCCESS;

errout_free:
  free(line);
errout_close:
  close(fd);
  return EXIT_FAILURE;
}
