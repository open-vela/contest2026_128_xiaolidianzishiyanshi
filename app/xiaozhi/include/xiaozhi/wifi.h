#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xiaozhi {

struct WifiNetwork {
  std::string ssid;
  int rssi{0};
  bool secure{false};
};

enum class WifiConnectionStage {
  kIdle,
  kAuthenticating,
  kDhcp,
  kConnected,
  kFailed,
  kCanceled,
};

struct WifiStatus {
  std::string ssid;
  std::string ip_address;
  bool connected{false};
};

struct WifiCallbacks {
  std::function<void(bool scanning, const std::vector<WifiNetwork> &networks,
                     const std::string &error)>
      scan_changed;
  std::function<void(WifiConnectionStage stage, const std::string &ssid,
                     const std::string &message)>
      connection_changed;
  std::function<void(const WifiStatus &status)> status_changed;
};

class WifiService {
public:
  virtual ~WifiService() = default;
  virtual bool Start(WifiCallbacks callbacks) = 0;
  virtual void Stop() = 0;
  virtual void Scan() = 0;
  virtual void Connect(const WifiNetwork &network,
                       const std::string &password) = 0;
  virtual void Cancel() = 0;
  virtual bool WaitForIp() = 0;
};

std::unique_ptr<WifiService> CreateNuttxWifiService(const std::string &ifname);

} // namespace xiaozhi
