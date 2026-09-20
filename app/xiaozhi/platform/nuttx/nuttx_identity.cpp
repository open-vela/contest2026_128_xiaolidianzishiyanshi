#include "xiaozhi/identity.h"

#include <net/if.h>
#include <nuttx/net/ioctl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include <sys/random.h>
}

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>

namespace xiaozhi {
namespace {

bool ReadMacAddress(const char *interface, uint8_t mac[6]) {
  const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0) {
    return false;
  }

  struct ifreq request {};
  std::strncpy(request.ifr_name, interface, IFNAMSIZ - 1);
  const int result = ioctl(socket_fd, SIOCGIFHWADDR,
                           reinterpret_cast<unsigned long>(&request));
  close(socket_fd);
  if (result < 0) {
    return false;
  }
  std::memcpy(mac, request.ifr_hwaddr.sa_data, 6);
  return true;
}

std::string FormatUuid(const uint8_t bytes[16]) {
  char uuid[37];
  std::snprintf(
      uuid, sizeof(uuid),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
      bytes[14], bytes[15]);
  return uuid;
}

void DeriveStableUuid(const std::string &seed, uint8_t bytes[16]) {
  for (uint32_t word = 0; word < 4; ++word) {
    uint32_t hash = 2166136261u ^ (word * 0x9e3779b9u);
    for (const unsigned char byte : seed) {
      hash ^= byte;
      hash *= 16777619u;
    }
    hash ^= word;
    hash *= 16777619u;
    bytes[word * 4] = static_cast<uint8_t>(hash >> 24);
    bytes[word * 4 + 1] = static_cast<uint8_t>(hash >> 16);
    bytes[word * 4 + 2] = static_cast<uint8_t>(hash >> 8);
    bytes[word * 4 + 3] = static_cast<uint8_t>(hash);
  }
}

} // namespace

std::string GetDeviceId() {
  static const char *const interfaces[] = {"wlan0", "eth0", "wlan1"};
  uint8_t mac[6]{};
  for (const char *interface : interfaces) {
    if (ReadMacAddress(interface, mac)) {
      char text[18];
      std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
                    mac[1], mac[2], mac[3], mac[4], mac[5]);
      return text;
    }
  }
  return "00:00:00:00:00:00";
}

std::string LoadOrCreateClientId(const char *path,
                                 const std::string &fallback_seed) {
  char existing[64]{};
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    const ssize_t size = read(fd, existing, sizeof(existing) - 1);
    close(fd);
    if (size > 0) {
      existing[size] = '\0';
      char *newline = std::strpbrk(existing, "\r\n");
      if (newline != nullptr) {
        *newline = '\0';
      }
      if (std::strlen(existing) == 36) {
        return existing;
      }
    }
  }

  uint8_t random[16]{};
  if (!fallback_seed.empty() && fallback_seed != "00:00:00:00:00:00") {
    DeriveStableUuid(fallback_seed, random);
  } else if (getrandom(random, sizeof(random), 0) != sizeof(random)) {
    const uint32_t seed =
        static_cast<uint32_t>(getpid()) ^
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(path));
    for (size_t index = 0; index < sizeof(random); ++index) {
      random[index] =
          static_cast<uint8_t>((seed >> ((index % 4) * 8)) ^ (index * 37));
    }
  }
  random[6] = (random[6] & 0x0f) | 0x40;
  random[8] = (random[8] & 0x3f) | 0x80;
  const std::string uuid = FormatUuid(random);

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd >= 0) {
    const ssize_t ignored = write(fd, uuid.data(), uuid.size());
    (void)ignored;
    close(fd);
  } else {
    std::fprintf(stderr,
                 "xiaozhi: client-id file unavailable (%s); using stable "
                 "network identity\n",
                 std::strerror(errno));
  }
  return uuid;
}

} // namespace xiaozhi
