#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "xiaozhi/wifi.h"

namespace xiaozhi {

class Display {
public:
  using TalkButtonCallback = std::function<void()>;
  using WifiScanCallback = std::function<void()>;
  using WifiConnectCallback =
      std::function<void(const WifiNetwork &, const std::string &)>;
  using WifiCancelCallback = std::function<void()>;

  virtual ~Display() = default;
  virtual bool Start() = 0;
  virtual void Stop() = 0;
  virtual void SetStatus(const std::string &status) = 0;
  virtual void SetConnected(bool connected) = 0;
  virtual void SetChatMessage(const std::string &role,
                              const std::string &content) = 0;
  virtual void SetEmotion(const std::string &emotion) = 0;
  virtual void ShowNotification(const std::string &text) = 0;
  virtual void SetTalkButtonCallback(TalkButtonCallback callback) = 0;
  virtual void SetTalkButtonState(bool listening, bool enabled) = 0;
  virtual void SetWifiCallbacks(WifiScanCallback scan,
                                WifiConnectCallback connect,
                                WifiCancelCallback cancel) = 0;
  virtual void UpdateWifiStatus(const WifiStatus &status) = 0;
  virtual void UpdateWifiScan(bool scanning,
                              const std::vector<WifiNetwork> &networks,
                              const std::string &error) = 0;
  virtual void UpdateWifiConnection(WifiConnectionStage stage,
                                    const std::string &ssid,
                                    const std::string &message) = 0;
};

std::unique_ptr<Display>
CreateNuttxLvglDisplay(const std::string &framebuffer_path,
                       const std::string &input_path);

} // namespace xiaozhi
