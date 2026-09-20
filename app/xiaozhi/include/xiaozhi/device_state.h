#pragma once

namespace xiaozhi {

enum class DeviceState {
  kStarting,
  kIdle,
  kConnecting,
  kListening,
  kSpeaking,
  kError,
  kStopped,
};

const char *DeviceStateName(DeviceState state);

}  // namespace xiaozhi
