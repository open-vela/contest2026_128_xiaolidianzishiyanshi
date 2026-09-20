#include "xiaozhi/bsp_test.h"

#include <netutils/icmp_ping.h>
#include <nuttx/video/video.h>
#include <nuttx/video/v4l2_cap.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace xiaozhi {
namespace {

constexpr int kCameraWidth = 1280;
constexpr int kCameraHeight = 720;
constexpr size_t kCameraBytes =
    static_cast<size_t>(kCameraWidth) * kCameraHeight * 10 / 8;
constexpr int kPreviewWidth = 320;
constexpr int kPreviewHeight = 180;

struct PingState {
  int replies{0};
  long last_roundtrip_us{0};
  int error_code{0};
  long error_detail{0};
};

void PingCallback(const ping_result_s *result) {
  auto *state = static_cast<PingState *>(result->info->priv);
  if (result->code == ICMP_I_ROUNDTRIP) {
    state->last_roundtrip_us = result->extra;
  } else if (result->code == ICMP_I_FINISH) {
    state->replies = result->nreplies;
  } else if (result->code < 0) {
    state->error_code = result->code;
    state->error_detail = result->extra;
  }
}

BspTestResult RunPingTest() {
  PingState state;
  ping_info_s info{};
  info.hostname = "223.5.5.5";
#ifdef CONFIG_NET_BINDTODEVICE
  info.devname = nullptr;
#endif
  info.count = 3;
  info.datalen = 32;
  info.delay = 100;
  info.timeout = 1000;
  info.priv = &state;
  info.callback = PingCallback;
  icmp_ping(&info);

  BspTestResult result;
  result.success = state.replies > 0;
  char message[128];
  if (result.success) {
    std::snprintf(message, sizeof(message),
                  "223.5.5.5：3 次请求，%d 次应答，最近 RTT %.2f ms",
                  state.replies, state.last_roundtrip_us / 1000.0);
  } else {
    std::snprintf(message, sizeof(message),
                  "223.5.5.5 无应答（code=%d, detail=%ld）",
                  state.error_code, state.error_detail);
  }
  result.message = message;
  return result;
}

BspTestResult RunDnsTest() {
  BspTestResult result;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const int status = getaddrinfo("www.aliyun.com", nullptr, &hints,
                                 &addresses);
  if (status != 0 || addresses == nullptr) {
    result.message = "www.aliyun.com 解析失败: ";
    result.message += gai_strerror(status);
    return result;
  }

  char address[INET_ADDRSTRLEN]{};
  auto *ipv4 = reinterpret_cast<sockaddr_in *>(addresses->ai_addr);
  inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address));
  freeaddrinfo(addresses);
  result.success = address[0] != '\0';
  result.message = "www.aliyun.com -> ";
  result.message += result.success ? address : "无 IPv4 地址";
  return result;
}

uint8_t Raw10HighByte(const uint8_t *raw, int x, int y) {
  const size_t pixel = static_cast<size_t>(y) * kCameraWidth + x;
  return raw[(pixel / 4) * 5 + pixel % 4];
}

void ConvertRaw10ToRgb565(const uint8_t *raw, BspTestResult *result) {
  result->pixels.resize(static_cast<size_t>(kPreviewWidth) * kPreviewHeight);
  for (int out_y = 0; out_y < kPreviewHeight; ++out_y) {
    const int y = out_y * 4;
    for (int out_x = 0; out_x < kPreviewWidth; ++out_x) {
      const int x = out_x * 4;

      /* The sensor produces BGGR.  A 2x2 nearest-neighbour demosaic is
       * sufficient for the BSP preview and avoids a second full-size frame.
       * RAW10's high eight bits are the first four bytes of each five-byte
       * group; discarding the low two bits also maps naturally to RGB565.
       */
      const uint8_t blue = Raw10HighByte(raw, x, y);
      const uint8_t green0 = Raw10HighByte(raw, x + 1, y);
      const uint8_t green1 = Raw10HighByte(raw, x, y + 1);
      const uint8_t red = Raw10HighByte(raw, x + 1, y + 1);
      const uint8_t green =
          static_cast<uint8_t>((static_cast<unsigned>(green0) + green1) / 2);
      result->pixels[static_cast<size_t>(out_y) * kPreviewWidth + out_x] =
          static_cast<uint16_t>(((red & 0xf8) << 8) |
                                ((green & 0xfc) << 3) | (blue >> 3));
    }
  }
  result->width = kPreviewWidth;
  result->height = kPreviewHeight;
}

BspTestResult RunCameraTest() {
  BspTestResult result;
  int fd = open("/dev/video0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    result.message = "无法打开 /dev/video0: ";
    result.message += std::strerror(errno);
    return result;
  }

  uint8_t *frame = nullptr;
  bool streaming = false;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  v4l2_format format{};
  v4l2_requestbuffers request{};
  v4l2_buffer buffer{};
  format.type = type;
  format.fmt.pix.width = kCameraWidth;
  format.fmt.pix.height = kCameraHeight;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_ENTROPY;
  if (ioctl(fd, VIDIOC_S_FMT, reinterpret_cast<uintptr_t>(&format)) < 0) {
    result.message = "设置 RAW10 格式失败: ";
    result.message += std::strerror(errno);
    goto cleanup;
  }

  request.type = type;
  request.memory = V4L2_MEMORY_USERPTR;
  request.count = 1;
  request.mode = V4L2_BUF_MODE_FIFO;
  if (ioctl(fd, VIDIOC_REQBUFS, reinterpret_cast<uintptr_t>(&request)) < 0) {
    result.message = "申请摄像头缓冲失败: ";
    result.message += std::strerror(errno);
    goto cleanup;
  }

  frame = static_cast<uint8_t *>(memalign(64, kCameraBytes));
  if (frame == nullptr) {
    result.message = "摄像头帧缓冲内存不足";
    goto cleanup;
  }

  buffer.type = type;
  buffer.memory = V4L2_MEMORY_USERPTR;
  buffer.index = 0;
  buffer.m.userptr = reinterpret_cast<uintptr_t>(frame);
  buffer.length = kCameraBytes;
  if (ioctl(fd, VIDIOC_QBUF, reinterpret_cast<uintptr_t>(&buffer)) < 0 ||
      ioctl(fd, VIDIOC_STREAMON, reinterpret_cast<uintptr_t>(&type)) < 0) {
    result.message = "启动摄像头采集失败: ";
    result.message += std::strerror(errno);
    goto cleanup;
  }
  streaming = true;

  for (int attempt = 0; attempt < 30; ++attempt) {
    pollfd wait_fd{fd, POLLIN, 0};
    (void)poll(&wait_fd, 1, 100);
    std::memset(&buffer, 0, sizeof(buffer));
    buffer.type = type;
    buffer.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(fd, VIDIOC_DQBUF, reinterpret_cast<uintptr_t>(&buffer)) == 0) {
      if (buffer.bytesused < kCameraBytes) {
        result.message = "摄像头返回的 RAW10 帧不完整";
      } else {
        ConvertRaw10ToRgb565(frame, &result);
        result.success = true;
        result.message = "已拍摄 RAW10，转换为 320×180 RGB565";
      }
      goto cleanup;
    }
    if (errno != EAGAIN && errno != EINTR) {
      result.message = "读取摄像头帧失败: ";
      result.message += std::strerror(errno);
      goto cleanup;
    }
  }
  result.message = "等待摄像头帧超时（请检查 24 MHz XCLK）";

cleanup:
  if (streaming) {
    ioctl(fd, VIDIOC_STREAMOFF, reinterpret_cast<uintptr_t>(&type));
  }
  if (frame != nullptr) {
    free(frame);
  }
  close(fd);
  return result;
}

} // namespace

BspTestResult RunNuttxBspTest(BspTestType type) {
  switch (type) {
    case BspTestType::kCamera:
      return RunCameraTest();
    case BspTestType::kPing:
      return RunPingTest();
    case BspTestType::kDns:
      return RunDnsTest();
    default:
      return {false, "不支持的 BSP 测试", {}, 0, 0};
  }
}

} // namespace xiaozhi
