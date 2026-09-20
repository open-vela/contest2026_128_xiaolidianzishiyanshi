#include "xiaozhi/wifi.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netutils/dhcpc.h>
#include <netutils/netlib.h>
#include <nuttx/config.h>
#include <nuttx/wireless/wireless.h>
#include <unistd.h>
#include <wireless/wapi.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifndef CONFIG_CONTEST2026_128_XIAOZHI_WIFI_STACKSIZE
#define CONFIG_CONTEST2026_128_XIAOZHI_WIFI_STACKSIZE 8192
#endif

namespace xiaozhi {
namespace {

class NuttxWifiService;
std::atomic<NuttxWifiService *> g_dhcp_owner{nullptr};

enum class WorkerCommand { kNone, kScan, kConnect };

class NuttxWifiService final : public WifiService {
public:
  explicit NuttxWifiService(std::string ifname) : ifname_(std::move(ifname)) {}
  ~NuttxWifiService() override { Stop(); }

  bool Start(WifiCallbacks callbacks) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      return true;
    }
    callbacks_ = std::move(callbacks);
    running_ = true;

    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
      running_ = false;
      return false;
    }
    pthread_attr_setstacksize(
        &attributes, CONFIG_CONTEST2026_128_XIAOZHI_WIFI_STACKSIZE);
    int result = pthread_create(&thread_, &attributes, ThreadEntry, this);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
      running_ = false;
      return false;
    }
    pthread_setname_np(thread_, "xz-wifi");
    started_ = true;
    return true;
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_) {
        return;
      }
      running_ = false;
      cancel_requested_ = true;
    }
    wake_.notify_all();
    ip_ready_.notify_all();
    pthread_join(thread_, nullptr);
    started_ = false;
  }

  void Scan() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ || command_ != WorkerCommand::kNone) {
        return;
      }
      cancel_requested_ = false;
      command_ = WorkerCommand::kScan;
    }
    wake_.notify_one();
  }

  void Connect(const WifiNetwork &network,
               const std::string &password) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return;
      }
      pending_network_ = network;
      pending_password_ = password;
      cancel_requested_ = false;
      command_ = WorkerCommand::kConnect;
    }
    wake_.notify_one();
  }

  void Cancel() override {
    WifiCallbacks callbacks;
    std::string ssid;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancel_requested_ = true;
      callbacks = callbacks_;
      ssid = pending_network_.ssid;
    }
    wake_.notify_all();
    if (callbacks.connection_changed) {
      callbacks.connection_changed(WifiConnectionStage::kCanceled, ssid,
                                   "canceled");
    }
  }

  bool WaitForIp() override {
    std::unique_lock<std::mutex> lock(mutex_);
    ip_ready_.wait(lock, [this]() { return !running_ || has_ip_; });
    return has_ip_;
  }

  void OnDhcpResult(const struct dhcpc_state *result) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      dhcp_done_ = true;
      dhcp_success_ = result != nullptr;
      if (result != nullptr) {
        dhcp_state_ = *result;
      }
    }
    wake_.notify_all();
  }

private:
  static void *ThreadEntry(void *argument) {
    static_cast<NuttxWifiService *>(argument)->Run();
    return nullptr;
  }

  static void DhcpCallback(struct dhcpc_state *result) {
    NuttxWifiService *owner = g_dhcp_owner.load();
    if (owner != nullptr) {
      owner->OnDhcpResult(result);
    }
  }

  bool IsCanceled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancel_requested_ || !running_;
  }

  void PublishConnection(WifiConnectionStage stage, const std::string &ssid,
                         const std::string &message) {
    WifiCallbacks callbacks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callbacks = callbacks_;
    }
    if (callbacks.connection_changed) {
      callbacks.connection_changed(stage, ssid, message);
    }
  }

  void PublishScan(bool scanning, const std::vector<WifiNetwork> &networks,
                   const std::string &error) {
    WifiCallbacks callbacks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callbacks = callbacks_;
    }
    if (callbacks.scan_changed) {
      callbacks.scan_changed(scanning, networks, error);
    }
  }

  void PublishStatusIfChanged(bool force) {
    WifiStatus status;
    struct in_addr address {};
    bool address_valid = false;
    if (netlib_get_ipv4addr(ifname_.c_str(), &address) == 0 &&
        address.s_addr != INADDR_ANY) {
      char buffer[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) != nullptr) {
        status.ip_address = buffer;
        address_valid = true;
      }
    }

    bool associated = false;
    int socket = wapi_make_socket();
    if (socket >= 0) {
      char essid[WAPI_ESSID_MAX_SIZE + 1] {};
      enum wapi_essid_flag_e flag;
      if (wapi_get_essid(socket, ifname_.c_str(), essid, &flag) == 0) {
        status.ssid = essid;
      }

      /* An IPv4 address can survive a link drop long enough to look usable.
       * Require a real AP association as well, otherwise WaitForIp() may let
       * activation start before WiFi is connected and DNS is reachable.
       */

      struct ether_addr ap {};
      if (wapi_get_ap(socket, ifname_.c_str(), &ap) == 0) {
        for (size_t i = 0; i < sizeof(ap.ether_addr_octet); ++i) {
          associated = associated || ap.ether_addr_octet[i] != 0;
        }
      }
      close(socket);
    }

    status.connected = address_valid && associated && !status.ssid.empty();
    if (!status.connected) {
      status.ip_address.clear();
    }

    WifiCallbacks callbacks;
    bool changed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      changed = force || status.connected != last_status_.connected ||
                status.ssid != last_status_.ssid ||
                status.ip_address != last_status_.ip_address;
      last_status_ = status;
      has_ip_ = status.connected;
      callbacks = callbacks_;
    }
    if (status.connected) {
      ip_ready_.notify_all();
    }
    if (changed && callbacks.status_changed) {
      callbacks.status_changed(status);
    }
  }

  void RunScan() {
    PublishScan(true, {}, "");
    int socket = wapi_make_socket();
    if (socket < 0) {
      PublishScan(false, {}, "扫描失败");
      return;
    }

    int result = wapi_scan_init(socket, ifname_.c_str(), nullptr);
    if (result == 0) {
      for (int attempt = 0; attempt < 40 && !IsCanceled(); ++attempt) {
        result = wapi_scan_stat(socket, ifname_.c_str());
        if (result <= 0) {
          break;
        }
        usleep(150 * 1000);
      }
    }

    std::vector<WifiNetwork> networks;
    if (result == 0 && !IsCanceled()) {
      struct wapi_list_s list {};
      result = wapi_scan_coll(socket, ifname_.c_str(), &list);
      if (result == 0) {
        for (struct wapi_scan_info_s *item = list.head.scan; item != nullptr;
             item = item->next) {
          if (!item->has_essid || item->essid[0] == '\0') {
            continue;
          }
          WifiNetwork network;
          network.ssid = item->essid;
          network.rssi = item->has_rssi ? item->rssi : -127;
          network.secure = item->has_encode &&
                           (item->encode & IW_ENCODE_DISABLED) == 0;
          auto old = std::find_if(
              networks.begin(), networks.end(),
              [&network](const WifiNetwork &value) {
                return value.ssid == network.ssid;
              });
          if (old == networks.end()) {
            networks.push_back(network);
          } else if (network.rssi > old->rssi) {
            *old = network;
          }
        }
        wapi_scan_coll_free(&list);
      }
    }
    close(socket);

    std::sort(networks.begin(), networks.end(),
              [](const WifiNetwork &left, const WifiNetwork &right) {
                return left.rssi > right.rssi;
              });
    PublishScan(false, networks,
                result < 0 && !IsCanceled() ? "扫描失败" : "");
  }

  void Disconnect() {
    int socket = wapi_make_socket();
    if (socket >= 0) {
      wpa_driver_wext_disconnect(socket, ifname_.c_str());
      close(socket);
    }
  }

  bool WaitForAssociation() {
    int socket = wapi_make_socket();
    if (socket < 0) {
      return false;
    }
    bool associated = false;
    for (int attempt = 0; attempt < 75 && !IsCanceled(); ++attempt) {
      int signal = 0;
      if (wapi_get_sensitivity(socket, ifname_.c_str(), &signal) == 0) {
        associated = true;
        break;
      }
      usleep(200 * 1000);
    }
    close(socket);
    return associated;
  }

  static int ApplyDhcp(const char *ifname, const struct dhcpc_state &state) {
    int result = netlib_set_ipv4addr(ifname, &state.ipaddr);
    if (result == 0 && state.netmask.s_addr != 0) {
      result = netlib_set_ipv4netmask(ifname, &state.netmask);
    }
    if (result == 0 && state.default_router.s_addr != 0) {
      result = netlib_set_dripv4addr(ifname, &state.default_router);
    }
#ifdef CONFIG_NETDB_DNSCLIENT
    if (result == 0 && state.dnsaddr.s_addr != 0) {
      result = netlib_set_ipv4dnsaddr(&state.dnsaddr);
    }
#endif
    return result;
  }

  bool ObtainDhcp() {
    uint8_t mac[IFHWADDRLEN] {};
    if (netlib_getmacaddr(ifname_.c_str(), mac) < 0) {
      return false;
    }

    struct in_addr empty {};
    netlib_set_ipv4addr(ifname_.c_str(), &empty);
    PublishStatusIfChanged(true);
    void *handle = dhcpc_open(ifname_.c_str(), mac, sizeof(mac));
    if (handle == nullptr) {
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      dhcp_done_ = false;
      dhcp_success_ = false;
    }
    g_dhcp_owner.store(this);
    int result = dhcpc_request_async(handle, DhcpCallback);
    if (result == 0) {
      std::unique_lock<std::mutex> lock(mutex_);
      while (running_ && !cancel_requested_ && !dhcp_done_) {
        wake_.wait_for(lock, std::chrono::milliseconds(200));
      }
      bool cancel = !running_ || cancel_requested_;
      bool success = dhcp_done_ && dhcp_success_;
      struct dhcpc_state state = dhcp_state_;
      lock.unlock();
      if (cancel) {
        dhcpc_cancel(handle);
      }
      if (success && !cancel) {
        result = ApplyDhcp(ifname_.c_str(), state);
      } else {
        result = -ECANCELED;
      }
    }
    g_dhcp_owner.store(nullptr);
    dhcpc_close(handle);
    return result == 0;
  }

  void RunConnect(const WifiNetwork &network, const std::string &password) {
    if (IsCanceled()) {
      return;
    }
    if (network.secure && (password.size() < 8 || password.size() > 63)) {
      PublishConnection(WifiConnectionStage::kFailed, network.ssid,
                        "密码长度应为 8-63");
      return;
    }

    PublishConnection(WifiConnectionStage::kAuthenticating, network.ssid,
                      "身份验证");
    struct wpa_wconfig_s config {};
    config.sta_mode = WAPI_MODE_MANAGED;
    config.ifname = ifname_.c_str();
    config.ssid = network.ssid.c_str();
    config.ssidlen = static_cast<uint8_t>(network.ssid.size());
    if (network.secure) {
      config.auth_wpa = IW_AUTH_WPA_VERSION_WPA2;
      config.cipher_mode = IW_AUTH_CIPHER_CCMP;
      config.alg = WPA_ALG_CCMP;
      config.passphrase = password.c_str();
      config.phraselen = static_cast<uint8_t>(password.size());
    } else {
      config.auth_wpa = IW_AUTH_WPA_VERSION_DISABLED;
      config.cipher_mode = IW_AUTH_CIPHER_NONE;
      config.alg = WPA_ALG_NONE;
    }

    if (wpa_driver_wext_associate(&config) < 0 || !WaitForAssociation()) {
      if (IsCanceled()) {
        Disconnect();
        return;
      }
      PublishConnection(WifiConnectionStage::kFailed, network.ssid,
                        "身份验证失败");
      return;
    }

    PublishConnection(WifiConnectionStage::kDhcp, network.ssid,
                      "DHCP 获取地址");
    if (!ObtainDhcp()) {
      if (IsCanceled()) {
        Disconnect();
        return;
      }
      PublishConnection(WifiConnectionStage::kFailed, network.ssid,
                        "DHCP 失败");
      return;
    }

    PublishStatusIfChanged(true);
    PublishConnection(WifiConnectionStage::kConnected, network.ssid,
                      "connected");
  }

  void Run() {
    PublishStatusIfChanged(true);
    while (true) {
      WorkerCommand command = WorkerCommand::kNone;
      WifiNetwork network;
      std::string password;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(1), [this]() {
          return !running_ || command_ != WorkerCommand::kNone;
        });
        if (!running_) {
          break;
        }
        command = command_;
        command_ = WorkerCommand::kNone;
        if (command == WorkerCommand::kConnect) {
          network = pending_network_;
          password = pending_password_;
        }
      }

      if (command == WorkerCommand::kScan) {
        RunScan();
      } else if (command == WorkerCommand::kConnect) {
        RunConnect(network, password);
      }
      PublishStatusIfChanged(false);
    }
    Disconnect();
  }

  std::string ifname_;
  pthread_t thread_ {};
  bool started_{false};
  bool running_{false};
  bool cancel_requested_{false};
  bool has_ip_{false};
  bool dhcp_done_{false};
  bool dhcp_success_{false};
  WorkerCommand command_{WorkerCommand::kNone};
  WifiNetwork pending_network_;
  std::string pending_password_;
  WifiStatus last_status_;
  struct dhcpc_state dhcp_state_ {};
  WifiCallbacks callbacks_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable ip_ready_;
};

} // namespace

std::unique_ptr<WifiService> CreateNuttxWifiService(const std::string &ifname) {
  return std::make_unique<NuttxWifiService>(ifname);
}

} // namespace xiaozhi
