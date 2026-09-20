#pragma once

#include <functional>
#include <mutex>

#include "xiaozhi/device_state.h"

namespace xiaozhi {

class DeviceStateMachine {
 public:
  using Listener = std::function<void(DeviceState, DeviceState)>;

  DeviceState state() const;
  bool TransitionTo(DeviceState next);
  void SetListener(Listener listener);

 private:
  bool CanTransition(DeviceState from, DeviceState to) const;

  mutable std::mutex mutex_;
  DeviceState state_{DeviceState::kStarting};
  Listener listener_;
};

}  // namespace xiaozhi
