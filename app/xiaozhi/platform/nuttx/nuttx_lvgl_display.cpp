#include "xiaozhi/display.h"
#include "xiaozhi/lvgl_lock.h"

#include <lvgl/lvgl.h>
#include <nuttx/config.h>
#include <pthread.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

extern "C" {
#include "lv_100ask_xz_ai_main.h"
#include "bsp_test_ui.h"
#include "wifi_ui.h"
#include "xiaozhi_font.h"
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_PPA
#include "lv_draw_ppa_nuttx.h"
#endif
}

namespace xiaozhi {
namespace {

int CreateGuiThread(pthread_t *thread, void *(*entry)(void *), void *argument) {
  pthread_attr_t attributes;
  int result = pthread_attr_init(&attributes);
  if (result != 0) {
    return result;
  }

  result = pthread_attr_setstacksize(
      &attributes, CONFIG_CONTEST2026_128_XIAOZHI_GUI_STACKSIZE);
  if (result == 0) {
    result = pthread_create(thread, &attributes, entry, argument);
  }
  pthread_attr_destroy(&attributes);
  return result;
}

class NuttxLvglDisplay final : public Display {
public:
  NuttxLvglDisplay(std::string framebuffer_path, std::string input_path)
      : framebuffer_path_(std::move(framebuffer_path)),
        input_path_(std::move(input_path)) {}

  ~NuttxLvglDisplay() override { Stop(); }

  bool Start() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (thread_started_) {
        return initialized_ok_;
      }
      running_ = true;
      initialization_done_ = false;
      initialized_ok_ = false;
    }

    if (CreateGuiThread(&thread_, ThreadEntry, this) != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
      return false;
    }
    pthread_setname_np(thread_, "xz-gui");
    thread_started_ = true;

    std::unique_lock<std::mutex> lock(mutex_);
    initialized_.wait(lock, [this]() { return initialization_done_; });
    return initialized_ok_;
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!thread_started_) {
        return;
      }
      running_ = false;
    }
    wake_.notify_all();
    pthread_join(thread_, nullptr);
    thread_started_ = false;
  }

  void SetStatus(const std::string &status) override {
    Post([status]() { SetStateString(status.c_str()); });
  }

  void SetConnected(bool connected) override {
    Post([connected]() { SetWifi(connected ? 1 : 0); });
  }

  void SetChatMessage(const std::string &role,
                      const std::string &content) override {
    Post([role, content]() {
      std::string message;
      if (role == "user") {
        message = "You: " + content;
      } else {
        message = content;
      }
      SetText(message.c_str());
    });
  }

  void SetEmotion(const std::string &emotion) override {
    Post([emotion]() { ::SetEmotion(emotion.c_str()); });
  }

  void ShowNotification(const std::string &text) override {
    Post([text]() { SetText(text.c_str()); });
  }

  void SetTalkButtonCallback(TalkButtonCallback callback) override {
    Post([this, callback = std::move(callback)]() mutable {
      talk_button_callback_ = std::move(callback);
      ::SetTalkButtonCallback(TalkButtonThunk, this);
    });
  }

  void SetTalkButtonState(bool listening, bool enabled) override {
    Post([listening, enabled]() {
      ::SetTalkButtonState(listening ? 1 : 0, enabled ? 1 : 0);
    });
  }

  void SetWifiCallbacks(WifiScanCallback scan, WifiConnectCallback connect,
                        WifiCancelCallback cancel) override {
    Post([this, scan = std::move(scan), connect = std::move(connect),
          cancel = std::move(cancel)]() mutable {
      wifi_scan_callback_ = std::move(scan);
      wifi_connect_callback_ = std::move(connect);
      wifi_cancel_callback_ = std::move(cancel);
      LvglLockGuard lock;
      wifi_ui_set_callbacks(wifi_ui_, WifiScanThunk, WifiConnectThunk,
                            WifiCancelThunk, this);
    });
  }

  void UpdateWifiStatus(const WifiStatus &status) override {
    Post([this, status]() {
      {
        LvglLockGuard lock;
        wifi_ui_set_status(wifi_ui_, status.ssid.c_str(),
                           status.ip_address.c_str(),
                           status.connected ? 1 : 0);
      }
      ::SetWifi(status.connected ? 1 : 0);
    });
  }

  void UpdateWifiScan(bool scanning,
                      const std::vector<WifiNetwork> &networks,
                      const std::string &error) override {
    Post([this, scanning, networks, error]() {
      wifi_ui_network_t items[WIFI_UI_MAX_NETWORKS]{};
      size_t count =
          networks.size() > WIFI_UI_MAX_NETWORKS ? WIFI_UI_MAX_NETWORKS
                                                 : networks.size();
      for (size_t index = 0; index < count; ++index) {
        std::snprintf(items[index].ssid, sizeof(items[index].ssid), "%s",
                      networks[index].ssid.c_str());
        items[index].rssi = networks[index].rssi;
        items[index].secure = networks[index].secure ? 1 : 0;
      }
      LvglLockGuard lock;
      wifi_ui_set_scan_result(wifi_ui_, scanning ? 1 : 0, items, count,
                              error.c_str());
    });
  }

  void UpdateWifiConnection(WifiConnectionStage stage,
                            const std::string &ssid,
                            const std::string &message) override {
    Post([this, stage, ssid, message]() {
      LvglLockGuard lock;
      wifi_ui_set_connection(
          wifi_ui_, static_cast<wifi_ui_connection_stage_t>(stage),
          ssid.c_str(), message.c_str());
    });
  }

  void SetBspTestCallback(BspTestCallback callback) override {
    Post([this, callback = std::move(callback)]() mutable {
      bsp_test_callback_ = std::move(callback);
      bsp_test_ui_set_callback(bsp_test_ui_, BspTestThunk, this);
    });
  }

  void SetBspTestRunning(BspTestType type) override {
    Post([this, type]() {
      bsp_test_ui_set_running(bsp_test_ui_, ToUiTestType(type));
    });
  }

  void SetBspTestResult(BspTestType type, BspTestResult result) override {
    Post([this, type, result = std::move(result)]() {
      bsp_test_ui_set_result(
          bsp_test_ui_, ToUiTestType(type), result.success ? 1 : 0,
          result.message.c_str(),
          result.pixels.empty() ? nullptr : result.pixels.data(),
          result.width, result.height);
    });
  }

private:
  using Task = std::function<void()>;

  static void *ThreadEntry(void *argument) {
    static_cast<NuttxLvglDisplay *>(argument)->Run();
    return nullptr;
  }

  static void TalkButtonThunk(void *argument) {
    auto *display = static_cast<NuttxLvglDisplay *>(argument);
    if (display->talk_button_callback_) {
      display->talk_button_callback_();
    }
  }

  static void WifiScanThunk(void *argument) {
    auto *display = static_cast<NuttxLvglDisplay *>(argument);
    if (display->wifi_scan_callback_) {
      display->wifi_scan_callback_();
    }
  }

  static void WifiConnectThunk(const wifi_ui_network_t *network,
                               const char *password, void *argument) {
    auto *display = static_cast<NuttxLvglDisplay *>(argument);
    if (!display->wifi_connect_callback_) {
      return;
    }
    WifiNetwork item;
    item.ssid = network->ssid;
    item.rssi = network->rssi;
    item.secure = network->secure != 0;
    display->wifi_connect_callback_(item, password == nullptr ? "" : password);
  }

  static void WifiCancelThunk(void *argument) {
    auto *display = static_cast<NuttxLvglDisplay *>(argument);
    if (display->wifi_cancel_callback_) {
      display->wifi_cancel_callback_();
    }
  }

  static BspTestType FromUiTestType(bsp_test_type_t type) {
    switch (type) {
      case BSP_TEST_MICROPHONE:
        return BspTestType::kMicrophone;
      case BSP_TEST_SPEAKER:
        return BspTestType::kSpeaker;
      case BSP_TEST_CAMERA:
        return BspTestType::kCamera;
      case BSP_TEST_DNS:
        return BspTestType::kDns;
      case BSP_TEST_SD_MOUNT:
        return BspTestType::kSdMount;
      case BSP_TEST_SD_LIST:
        return BspTestType::kSdList;
      case BSP_TEST_PING:
      default:
        return BspTestType::kPing;
    }
  }

  static bsp_test_type_t ToUiTestType(BspTestType type) {
    switch (type) {
      case BspTestType::kMicrophone:
        return BSP_TEST_MICROPHONE;
      case BspTestType::kSpeaker:
        return BSP_TEST_SPEAKER;
      case BspTestType::kCamera:
        return BSP_TEST_CAMERA;
      case BspTestType::kDns:
        return BSP_TEST_DNS;
      case BspTestType::kSdMount:
        return BSP_TEST_SD_MOUNT;
      case BspTestType::kSdList:
        return BSP_TEST_SD_LIST;
      case BspTestType::kPing:
      default:
        return BSP_TEST_PING;
    }
  }

  static void BspTestThunk(bsp_test_type_t type, void *argument) {
    auto *display = static_cast<NuttxLvglDisplay *>(argument);
    if (display->bsp_test_callback_) {
      display->bsp_test_callback_(FromUiTestType(type));
    }
  }

  void Post(Task task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return;
      }
      tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
  }

  void Run() {
    lv_nuttx_dsc_t descriptor;
    lv_nuttx_result_t result{};

    if (lv_is_initialized()) {
      std::fprintf(stderr, "xiaozhi UI: LVGL is already initialized\n");
      FinishInitialization(false);
      return;
    }

    lv_init();
    lv_nuttx_dsc_init(&descriptor);
    descriptor.fb_path = framebuffer_path_.c_str();
    if (!input_path_.empty()) {
      descriptor.input_path = input_path_.c_str();
    }
    lv_nuttx_init(&descriptor, &result);
    if (result.disp == nullptr) {
      std::fprintf(stderr, "xiaozhi UI: display initialization failed\n");
      lv_deinit();
      FinishInitialization(false);
      return;
    }

#ifdef CONFIG_CONTEST2026_128_XIAOZHI_PPA
    int ppa_result = lv_draw_ppa_nuttx_init();
    if (ppa_result < 0) {
      std::fprintf(stderr,
                   "xiaozhi UI: PPA draw unit initialization failed: %d\n",
                   ppa_result);
      lv_nuttx_deinit(&result);
      lv_deinit();
      FinishInitialization(false);
      return;
    }
#endif

    lv_100ask_xz_ai_main();
    wifi_ui_ = wifi_ui_create(lv_screen_active());
    bsp_test_ui_ = bsp_test_ui_create(lv_screen_active());
    if (wifi_ui_ == nullptr || bsp_test_ui_ == nullptr) {
      std::fprintf(stderr, "xiaozhi UI: side panel initialization failed\n");
      bsp_test_ui_destroy(bsp_test_ui_);
      bsp_test_ui_ = nullptr;
      wifi_ui_destroy(wifi_ui_);
      wifi_ui_ = nullptr;
      lv_100ask_xz_ai_deinit();
      lv_nuttx_deinit(&result);
      lv_deinit();
      FinishInitialization(false);
      return;
    }
    FinishInitialization(true);

    while (true) {
      std::deque<Task> pending;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
          break;
        }
        pending.swap(tasks_);
      }
      for (auto &task : pending) {
        task();
      }

      uint32_t delay_ms;
      {
        LvglLockGuard lock;
        delay_ms = lv_timer_handler();
      }
      if (delay_ms == 0) {
        delay_ms = 1;
      } else if (delay_ms > 50) {
        delay_ms = 50;
      }

      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                     [this]() { return !running_ || !tasks_.empty(); });
    }

    bsp_test_ui_destroy(bsp_test_ui_);
    bsp_test_ui_ = nullptr;
    wifi_ui_destroy(wifi_ui_);
    wifi_ui_ = nullptr;
    lv_100ask_xz_ai_deinit();
    lv_nuttx_deinit(&result);
    lv_deinit();
  }

  void FinishInitialization(bool success) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      initialized_ok_ = success;
      initialization_done_ = true;
      if (!success) {
        running_ = false;
      }
    }
    initialized_.notify_all();
  }

  std::string framebuffer_path_;
  std::string input_path_;
  pthread_t thread_{};
  bool thread_started_{false};
  bool running_{false};
  bool initialization_done_{false};
  bool initialized_ok_{false};
  std::mutex mutex_;
  std::condition_variable initialized_;
  std::condition_variable wake_;
  std::deque<Task> tasks_;
  TalkButtonCallback talk_button_callback_;
  WifiScanCallback wifi_scan_callback_;
  WifiConnectCallback wifi_connect_callback_;
  WifiCancelCallback wifi_cancel_callback_;
  BspTestCallback bsp_test_callback_;
  wifi_ui_t *wifi_ui_{nullptr};
  bsp_test_ui_t *bsp_test_ui_{nullptr};
};

} // namespace

std::unique_ptr<Display>
CreateNuttxLvglDisplay(const std::string &framebuffer_path,
                       const std::string &input_path) {
  return std::make_unique<NuttxLvglDisplay>(framebuffer_path, input_path);
}

} // namespace xiaozhi
