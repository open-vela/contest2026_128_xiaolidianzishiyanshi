/****************************************************************************
 * contest2026_128_xiaolidianzishiyanshi/app/hello_app/touch_test_main.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/input/touchscreen.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TOUCH_TEST_DEFAULT_DEVPATH  "/dev/input0"
#define TOUCH_TEST_MAX_POINTS       10
#define TOUCH_TEST_POLL_TIMEOUT_MS  3000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct touch_test_sample_s
{
  int32_t npoints;
  int32_t dummy;
  struct touch_point_s point[TOUCH_TEST_MAX_POINTS];
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void touch_test_print_flags(uint8_t flags)
{
  bool separator = false;

#define TOUCH_TEST_PRINT_FLAG(flag, name) \
  do \
    { \
      if ((flags & (flag)) != 0) \
        { \
          printf("%s%s", separator ? "|" : "", name); \
          separator = true; \
        } \
    } \
  while (0)

  TOUCH_TEST_PRINT_FLAG(TOUCH_DOWN, "DOWN");
  TOUCH_TEST_PRINT_FLAG(TOUCH_MOVE, "MOVE");
  TOUCH_TEST_PRINT_FLAG(TOUCH_UP, "UP");
  TOUCH_TEST_PRINT_FLAG(TOUCH_ID_VALID, "ID_VALID");
  TOUCH_TEST_PRINT_FLAG(TOUCH_POS_VALID, "POS_VALID");
  TOUCH_TEST_PRINT_FLAG(TOUCH_PRESSURE_VALID, "PRESSURE_VALID");
  TOUCH_TEST_PRINT_FLAG(TOUCH_SIZE_VALID, "SIZE_VALID");
  TOUCH_TEST_PRINT_FLAG(TOUCH_GESTURE_VALID, "GESTURE_VALID");

#undef TOUCH_TEST_PRINT_FLAG

  if (!separator)
    {
      printf("none");
    }
}

static void touch_test_print_capabilities(int fd)
{
  struct touch_resolution_s resolution;
  uint32_t frequency;
  uint8_t maxpoints;

  if (ioctl(fd, TSIOC_GETMAXPOINTS,
            (unsigned long)(uintptr_t)&maxpoints) == 0)
    {
      printf("touch_test: max points: %u\n", maxpoints);
    }
  else
    {
      printf("touch_test: TSIOC_GETMAXPOINTS unsupported: %d\n", errno);
    }

  if (ioctl(fd, TSIOC_GETRESOLUTION,
            (unsigned long)(uintptr_t)&resolution) == 0)
    {
      printf("touch_test: resolution: %" PRIu16 "x%" PRIu16 "\n",
             resolution.res_x, resolution.res_y);
    }
  else
    {
      printf("touch_test: TSIOC_GETRESOLUTION unsupported: %d\n", errno);
    }

  if (ioctl(fd, TSIOC_GETFREQUENCY,
            (unsigned long)(uintptr_t)&frequency) == 0)
    {
      printf("touch_test: bus frequency: %" PRIu32 " Hz\n", frequency);
    }
  else
    {
      printf("touch_test: TSIOC_GETFREQUENCY unsupported: %d\n", errno);
    }
}

static void touch_test_print_sample(const struct touch_test_sample_s *sample,
                                    ssize_t nbytes, unsigned long sequence)
{
  int32_t npoints = sample->npoints;
  int32_t i;

  printf("touch_test: sample %lu: read=%zd, npoints=%" PRId32 "\n",
         sequence, nbytes, npoints);

  if (npoints < 0 || npoints > TOUCH_TEST_MAX_POINTS)
    {
      printf("touch_test: invalid point count (buffer limit %d)\n",
             TOUCH_TEST_MAX_POINTS);
      return;
    }

  for (i = 0; i < npoints; i++)
    {
      const struct touch_point_s *point = &sample->point[i];

      printf("  point[%" PRId32 "]: id=%u flags=0x%02x(",
             i, point->id, point->flags);
      touch_test_print_flags(point->flags);
      printf(") x=%d y=%d h=%d w=%d pressure=%u gesture=%u"
             " timestamp=%" PRIu64 "\n",
             point->x, point->y, point->h, point->w, point->pressure,
             point->gesture, point->timestamp);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  const char *devpath = TOUCH_TEST_DEFAULT_DEVPATH;
  struct touch_test_sample_s sample;
  struct pollfd pfd;
  unsigned long limit = 0;
  unsigned long sequence = 0;
  char *endptr;
  ssize_t nbytes;
  int fd;
  int ret;

  if (argc > 3)
    {
      fprintf(stderr, "Usage: %s [input-device] [sample-count]\n", argv[0]);
      return EXIT_FAILURE;
    }

  if (argc >= 2)
    {
      devpath = argv[1];
    }

  if (argc == 3)
    {
      errno = 0;
      limit = strtoul(argv[2], &endptr, 0);
      if (errno != 0 || endptr == argv[2] || *endptr != '\0')
        {
          fprintf(stderr, "touch_test: invalid sample count: %s\n", argv[2]);
          return EXIT_FAILURE;
        }
    }

  fd = open(devpath, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      fprintf(stderr, "touch_test: open %s failed: %d\n", devpath, errno);
      return EXIT_FAILURE;
    }

  printf("touch_test: opened %s (sample buffer=%zu bytes)\n",
         devpath, sizeof(sample));
  touch_test_print_capabilities(fd);
  printf("touch_test: touch the panel; waiting with poll(), Ctrl-C stops\n");

  pfd.fd = fd;
  pfd.events = POLLIN;

  while (limit == 0 || sequence < limit)
    {
      pfd.revents = 0;
      ret = poll(&pfd, 1, TOUCH_TEST_POLL_TIMEOUT_MS);
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          fprintf(stderr, "touch_test: poll failed: %d\n", errno);
          close(fd);
          return EXIT_FAILURE;
        }

      if (ret == 0)
        {
          printf("touch_test: no input event within %d ms\n",
                 TOUCH_TEST_POLL_TIMEOUT_MS);
          continue;
        }

      if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
          fprintf(stderr, "touch_test: poll revents=0x%lx\n",
                  (unsigned long)pfd.revents);
          close(fd);
          return EXIT_FAILURE;
        }

      memset(&sample, 0, sizeof(sample));
      nbytes = read(fd, &sample, sizeof(sample));
      if (nbytes < 0)
        {
          if (errno == EAGAIN || errno == EINTR)
            {
              continue;
            }

          fprintf(stderr, "touch_test: read failed: %d\n", errno);
          close(fd);
          return EXIT_FAILURE;
        }

      if (nbytes == 0)
        {
          printf("touch_test: read returned no sample\n");
          continue;
        }

      sequence++;
      touch_test_print_sample(&sample, nbytes, sequence);
    }

  close(fd);
  return EXIT_SUCCESS;
}
