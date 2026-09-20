#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "xiaozhi/audio.h"
#include "xiaozhi/device_state_machine.h"
#include "xiaozhi/display.h"
#include "xiaozhi/protocol.h"
#include "xiaozhi/wifi.h"

namespace xiaozhi {

struct ApplicationConfig {
  std::string ota_url;
  std::string websocket_url;
  std::string token;
  std::string capture_path;
  std::string playback_path;
  std::string client_id_path;
  std::string framebuffer_path;
  std::string input_path;
  int protocol_version{1};
  std::string language;
  std::string board_type;
  std::string board_name;
  std::string board_manufacturer;
  std::string application_version;
  bool provision_from_server{true};
};

class Application {
public:
  explicit Application(ApplicationConfig config);
  ~Application();

  int Run();
  void RequestStop();

private:
  using Task = std::function<void()>;

  void Schedule(Task task);
  void ToggleListening();
  void StartListening(ListeningMode mode);
  void StopListening();
  void AbortSpeaking(AbortReason reason);
  void HandleJson(const cJSON *root);
  bool ConnectWebSocket(bool reconnecting);
  void HandleWebSocketDisconnected(const std::string &message);
  void SetError(const std::string &message);
  bool Provision(const std::string &device_id, const std::string &client_id);

  ApplicationConfig config_;
  DeviceStateMachine state_;
  std::unique_ptr<AudioDevice> audio_;
  std::unique_ptr<Display> display_;
  std::unique_ptr<Protocol> protocol_;
  std::unique_ptr<WifiService> wifi_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Task> tasks_;
  std::string device_id_;
  std::string client_id_;
  ListeningMode listening_mode_{ListeningMode::kManualStop};
  bool websocket_connected_{false};
  bool websocket_connecting_{false};
  bool listen_after_connect_{false};
  bool stop_requested_{false};
};

} // namespace xiaozhi
