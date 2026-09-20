#include "xiaozhi/application.h"

#include <netutils/cJSON.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

#include "xiaozhi/activation.h"
#include "xiaozhi/identity.h"

namespace xiaozhi {

Application::Application(ApplicationConfig config)
    : config_(std::move(config)) {
  audio_ = CreateNuttxAudioDevice(config_.capture_path, config_.playback_path);
  display_ =
      CreateNuttxLvglDisplay(config_.framebuffer_path, config_.input_path);
  protocol_ = std::make_unique<Protocol>(CreateNuttxWebSocketTransport());
  wifi_ = CreateNuttxWifiService("wlan0");
}

Application::~Application() {
  if (wifi_) {
    wifi_->Stop();
  }
  if (audio_) {
    audio_->Stop();
  }
  if (protocol_) {
    protocol_->Close();
  }
  if (display_) {
    display_->Stop();
  }
}

void Application::Schedule(Task task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  wake_.notify_one();
}

void Application::SetError(const std::string &message) {
  std::fprintf(stderr, "xiaozhi: %s\n", message.c_str());
  state_.TransitionTo(DeviceState::kError);
  audio_->SetCaptureEnabled(false);
  display_->ShowNotification(message);
}

bool Application::ConnectWebSocket(bool reconnecting) {
  if (websocket_connected_ || websocket_connecting_) {
    return true;
  }

  websocket_connecting_ = true;
  state_.TransitionTo(DeviceState::kConnecting);
  display_->ShowNotification(reconnecting ? "正在重新连接 WebSocket"
                                          : "正在连接 WebSocket");
  if (protocol_->Open(config_.websocket_url, config_.token, device_id_,
                      client_id_, config_.protocol_version)) {
    return true;
  }

  websocket_connecting_ = false;
  state_.TransitionTo(DeviceState::kError);
  display_->ShowNotification("WebSocket 连接失败，点击说话重试");
  return false;
}

void Application::HandleWebSocketDisconnected(const std::string &message) {
  if (stop_requested_) {
    return;
  }

  if (!message.empty()) {
    std::fprintf(stderr, "xiaozhi websocket: %s\n", message.c_str());
  }
  websocket_connected_ = false;
  websocket_connecting_ = false;
  audio_->SetCaptureEnabled(false);
  audio_->ClearPlayback();
  state_.TransitionTo(DeviceState::kError);
  display_->ShowNotification("WebSocket 已断开，点击说话重新连接");
}

bool Application::Provision(const std::string &device_id,
                            const std::string &client_id) {
  DeviceRegistration registration;
  registration.device_id = device_id;
  registration.client_id = client_id;
  registration.language = config_.language;
  registration.board_type = config_.board_type;
  registration.board_name = config_.board_name;
  registration.board_manufacturer = config_.board_manufacturer;
  registration.application_name = "xiaozhi";
  registration.application_version = config_.application_version;

  ActivationClient activation(CreateNuttxHttpClient(), config_.ota_url,
                              std::move(registration));
  while (!stop_requested_) {
    display_->SetStatus("Checking activation");
    ActivationResult result;
    std::string error;
    if (!activation.Check(&result, &error)) {
      std::fprintf(stderr, "xiaozhi: activation check failed: %s\n",
                   error.c_str());
      display_->SetStatus("Waiting to retry activation");

      /* DNS and the default route can lag behind association/DHCP, and the
       * network can disappear again at any time.  An activation transport
       * failure is therefore recoverable: wait for WiFi readiness and retry
       * instead of turning a temporary network outage into process exit.
       */

      if (!wifi_->WaitForIp()) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::seconds(3));
      continue;
    }

    if (!result.websocket_url.empty()) {
      config_.websocket_url = result.websocket_url;
    }
    if (!result.websocket_token.empty()) {
      config_.token = result.websocket_token;
    }
    if (result.websocket_version >= 1 && result.websocket_version <= 3) {
      config_.protocol_version = result.websocket_version;
    }

    if (!result.activation_required()) {
      if (result.websocket_url.empty() && result.websocket_token.empty() &&
          config_.token.empty()) {
        SetError("Activation response did not include WebSocket settings");
        return false;
      }
      std::printf("xiaozhi: activation complete\n");
      return true;
    }

    display_->SetStatus("Activation");
    std::string notice = result.activation_message;
    if (!result.activation_code.empty()) {
      if (!notice.empty()) {
        notice += "\n";
      }
      notice += "Pairing code: " + result.activation_code;
      std::printf("\n*** XiaoZhi pairing code: %s ***\n\n",
                  result.activation_code.c_str());
    }
    display_->ShowNotification(notice);

    for (int attempt = 0; attempt < 10 && !stop_requested_; ++attempt) {
      const ActivationPollResult poll = activation.Activate(result, &error);
      if (poll == ActivationPollResult::kActivated) {
        std::printf("xiaozhi: pairing accepted\n");
        break;
      }
      if (poll == ActivationPollResult::kFailed) {
        std::fprintf(stderr, "xiaozhi: activation poll failed: %s\n",
                     error.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(10));
      } else {
        std::this_thread::sleep_for(std::chrono::seconds(3));
      }
    }
    /* Recheck the version endpoint.  It either returns the same pairing code
     * (still pending) or the authenticated WebSocket configuration.
     */
  }
  return false;
}

void Application::StartListening(ListeningMode mode) {
  if (!websocket_connected_) {
    listen_after_connect_ = true;
    ConnectWebSocket(true);
    return;
  }
  if (state_.state() == DeviceState::kSpeaking) {
    AbortSpeaking(AbortReason::kNone);
  }
  if (!state_.TransitionTo(DeviceState::kListening)) {
    return;
  }
  listening_mode_ = mode;
  audio_->ClearPlayback();
  if (!protocol_->SendStartListening(mode)) {
    HandleWebSocketDisconnected("failed to send listen/start");
    return;
  }
  audio_->SetCaptureEnabled(true);
}

void Application::StopListening() {
  if (state_.state() != DeviceState::kListening) {
    return;
  }
  audio_->SetCaptureEnabled(false);
  if (!protocol_->SendStopListening()) {
    HandleWebSocketDisconnected("failed to send listen/stop");
    return;
  }
  state_.TransitionTo(DeviceState::kIdle);
}

void Application::ToggleListening() {
  const DeviceState current = state_.state();
  if (!websocket_connected_) {
    listen_after_connect_ = true;
    ConnectWebSocket(true);
    return;
  }
  if (current == DeviceState::kListening) {
    StopListening();
  } else if (current == DeviceState::kIdle ||
             current == DeviceState::kSpeaking) {
    StartListening(ListeningMode::kManualStop);
  }
}

void Application::AbortSpeaking(AbortReason reason) {
  protocol_->SendAbortSpeaking(reason);
  audio_->ClearPlayback();
  state_.TransitionTo(DeviceState::kIdle);
}

void Application::HandleJson(const cJSON *root) {
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (!cJSON_IsString(type)) {
    return;
  }

  if (std::strcmp(type->valuestring, "tts") == 0) {
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(text)) {
      std::printf("xiaozhi: assistant: %s\n", text->valuestring);
      display_->SetChatMessage("assistant", text->valuestring);
    }
    if (!cJSON_IsString(state)) {
      return;
    }
    if (std::strcmp(state->valuestring, "start") == 0) {
      audio_->SetCaptureEnabled(false);
      /* Binary audio is delivered directly by the WebSocket worker while
       * JSON state changes are serialized through Schedule().  Clearing the
       * queue here can therefore discard the first Opus packets that arrive
       * immediately after tts/start, creating a discontinuity at the start
       * of speech.  Stale audio is already cleared when a new listening turn
       * begins or speaking is aborted.
       */
      state_.TransitionTo(DeviceState::kSpeaking);
      display_->SetEmotion("happy");
    } else if (std::strcmp(state->valuestring, "stop") == 0) {
      if (state_.state() == DeviceState::kSpeaking) {
        state_.TransitionTo(DeviceState::kIdle);
        if (listening_mode_ != ListeningMode::kManualStop) {
          StartListening(listening_mode_);
        }
      }
    }
    return;
  }

  if (std::strcmp(type->valuestring, "stt") == 0) {
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(text)) {
      std::printf("xiaozhi: user: %s\n", text->valuestring);
      display_->SetChatMessage("user", text->valuestring);
    }
    return;
  }

  if (std::strcmp(type->valuestring, "llm") == 0) {
    const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    if (cJSON_IsString(emotion)) {
      std::printf("xiaozhi: emotion: %s\n", emotion->valuestring);
      display_->SetEmotion(emotion->valuestring);
    }
    return;
  }

  if (std::strcmp(type->valuestring, "mcp") == 0) {
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const cJSON *method =
        payload == nullptr
            ? nullptr
            : cJSON_GetObjectItemCaseSensitive(payload, "method");
    const cJSON *id = payload == nullptr
                          ? nullptr
                          : cJSON_GetObjectItemCaseSensitive(payload, "id");
    if (!cJSON_IsString(method) || id == nullptr) {
      return;
    }

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
    cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(id, true));
    cJSON *result = cJSON_AddObjectToObject(reply, "result");
    if (std::strcmp(method->valuestring, "initialize") == 0) {
      cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
      cJSON_AddObjectToObject(result, "capabilities");
      cJSON *server = cJSON_AddObjectToObject(result, "serverInfo");
      cJSON_AddStringToObject(server, "name", "openvela-xiaozhi");
      cJSON_AddStringToObject(server, "version", "0.1.0");
    } else if (std::strcmp(method->valuestring, "tools/list") == 0) {
      cJSON_AddArrayToObject(result, "tools");
    } else {
      cJSON_DeleteItemFromObject(reply, "result");
      cJSON *error = cJSON_AddObjectToObject(reply, "error");
      cJSON_AddNumberToObject(error, "code", -32601);
      cJSON_AddStringToObject(error, "message", "Method not found");
    }

    char *text = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    if (text != nullptr) {
      protocol_->SendMcpMessage(text);
      cJSON_free(text);
    }
  }
}

int Application::Run() {
  if (!display_->Start()) {
    std::fprintf(stderr, "xiaozhi: failed to start LVGL display\n");
    return 1;
  }

  display_->SetTalkButtonCallback(
      [this]() { Schedule([this]() { ToggleListening(); }); });
  display_->SetTalkButtonState(false, false);

  display_->SetWifiCallbacks(
      [this]() { wifi_->Scan(); },
      [this](const WifiNetwork &network, const std::string &password) {
        wifi_->Connect(network, password);
      },
      [this]() { wifi_->Cancel(); });

  WifiCallbacks wifi_callbacks;
  wifi_callbacks.scan_changed =
      [this](bool scanning, const std::vector<WifiNetwork> &networks,
             const std::string &error) {
        display_->UpdateWifiScan(scanning, networks, error);
      };
  wifi_callbacks.connection_changed =
      [this](WifiConnectionStage stage, const std::string &ssid,
             const std::string &message) {
        display_->UpdateWifiConnection(stage, ssid, message);
      };
  wifi_callbacks.status_changed = [this](const WifiStatus &status) {
    display_->UpdateWifiStatus(status);
  };
  if (!wifi_->Start(std::move(wifi_callbacks))) {
    SetError("failed to start WiFi service");
    return 1;
  }

  state_.SetListener([this](DeviceState, DeviceState next) {
    std::printf("xiaozhi: status=%s\n", DeviceStateName(next));
    display_->SetStatus(DeviceStateName(next));
    const bool can_talk = next == DeviceState::kIdle ||
                          next == DeviceState::kListening ||
                          next == DeviceState::kSpeaking ||
                          next == DeviceState::kError;
    display_->SetTalkButtonState(next == DeviceState::kListening, can_talk);
  });

  protocol_->SetCallbacks(
      [this](AudioPacket packet) {
        audio_->QueuePlayback(std::move(packet.payload));
      },
      [this](const cJSON *root) {
        cJSON *copy = cJSON_Duplicate(root, true);
        Schedule([this, copy]() {
          HandleJson(copy);
          cJSON_Delete(copy);
        });
      },
      [this]() {
        const int rate = protocol_->server_sample_rate();
        Schedule([this, rate]() {
          websocket_connected_ = true;
          websocket_connecting_ = false;
          audio_->SetOutputSampleRate(rate);
          state_.TransitionTo(DeviceState::kIdle);
          if (listen_after_connect_) {
            listen_after_connect_ = false;
            StartListening(ListeningMode::kManualStop);
          }
        });
      },
      [this]() {
        Schedule([this]() { HandleWebSocketDisconnected(""); });
      },
      [this](const std::string &message) {
        Schedule([this, message]() { HandleWebSocketDisconnected(message); });
      });

  state_.TransitionTo(DeviceState::kConnecting);
  display_->ShowNotification("等待 WiFi 连接");
  if (!wifi_->WaitForIp()) {
    SetError("WiFi service stopped before network was ready");
    return 1;
  }

  if (!audio_->Start(
          [this](std::vector<uint8_t> payload, uint32_t timestamp) {
            if (state_.state() != DeviceState::kListening) {
              return;
            }
            AudioPacket packet;
            packet.sample_rate = 16000;
            packet.frame_duration_ms = 60;
            packet.timestamp = timestamp;
            packet.payload = std::move(payload);
            protocol_->SendAudio(std::move(packet));
          },
          [this](const std::string &message) {
            Schedule([this, message]() { SetError(message); });
          })) {
    SetError("failed to start NuttX audio devices");
    return 1;
  }

  device_id_ = GetDeviceId();
  client_id_ =
      LoadOrCreateClientId(config_.client_id_path.c_str(), device_id_);
  if (config_.provision_from_server && !Provision(device_id_, client_id_)) {
    return 1;
  }
  ConnectWebSocket(false);

  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_requested_) {
    wake_.wait_for(lock, std::chrono::seconds(1),
                   [this]() { return stop_requested_ || !tasks_.empty(); });
    while (!tasks_.empty()) {
      Task task = std::move(tasks_.front());
      tasks_.pop_front();
      lock.unlock();
      task();
      lock.lock();
    }
  }

  lock.unlock();
  audio_->SetCaptureEnabled(false);
  protocol_->Close();
  audio_->Stop();
  state_.TransitionTo(DeviceState::kStopped);
  display_->Stop();
  return 0;
}

void Application::RequestStop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  wake_.notify_one();
}

} // namespace xiaozhi
