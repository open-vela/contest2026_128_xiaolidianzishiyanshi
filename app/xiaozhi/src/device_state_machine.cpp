#include "xiaozhi/device_state_machine.h"

#include <cstdio>

namespace xiaozhi {

const char *DeviceStateName(DeviceState state) {
  switch (state) {
    case DeviceState::kStarting:
      return "starting";
    case DeviceState::kIdle:
      return "idle";
    case DeviceState::kConnecting:
      return "connecting";
    case DeviceState::kListening:
      return "listening";
    case DeviceState::kSpeaking:
      return "speaking";
    case DeviceState::kError:
      return "error";
    case DeviceState::kStopped:
      return "stopped";
  }

  return "unknown";
}

DeviceState DeviceStateMachine::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

bool DeviceStateMachine::CanTransition(DeviceState from,
                                       DeviceState to) const {
  if (from == to) {
    return true;
  }

  if (to == DeviceState::kError || to == DeviceState::kStopped) {
    return true;
  }

  switch (from) {
    case DeviceState::kStarting:
      return to == DeviceState::kIdle || to == DeviceState::kConnecting;
    case DeviceState::kIdle:
      return to == DeviceState::kConnecting ||
             to == DeviceState::kListening ||
             to == DeviceState::kSpeaking;
    case DeviceState::kConnecting:
      return to == DeviceState::kIdle || to == DeviceState::kListening;
    case DeviceState::kListening:
      return to == DeviceState::kIdle || to == DeviceState::kSpeaking;
    case DeviceState::kSpeaking:
      return to == DeviceState::kIdle || to == DeviceState::kListening;
    case DeviceState::kError:
      return to == DeviceState::kConnecting || to == DeviceState::kIdle;
    case DeviceState::kStopped:
      return false;
  }

  return false;
}

bool DeviceStateMachine::TransitionTo(DeviceState next) {
  Listener listener;
  DeviceState old;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old = state_;
    if (!CanTransition(old, next)) {
      std::fprintf(stderr, "xiaozhi: invalid state transition %s -> %s\n",
                   DeviceStateName(old), DeviceStateName(next));
      return false;
    }

    if (old == next) {
      return true;
    }

    state_ = next;
    listener = listener_;
  }

  std::printf("xiaozhi: state %s -> %s\n", DeviceStateName(old),
              DeviceStateName(next));
  if (listener) {
    listener(old, next);
  }
  return true;
}

void DeviceStateMachine::SetListener(Listener listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  listener_ = std::move(listener);
}

}  // namespace xiaozhi
