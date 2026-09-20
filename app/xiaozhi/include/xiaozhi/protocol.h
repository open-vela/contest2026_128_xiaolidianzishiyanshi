#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct cJSON;

namespace xiaozhi {

enum class ListeningMode {
  kAutoStop,
  kManualStop,
  kRealtime,
};

enum class AbortReason {
  kNone,
  kWakeWordDetected,
};

struct AudioPacket {
  int sample_rate{0};
  int frame_duration_ms{0};
  uint32_t timestamp{0};
  std::vector<uint8_t> payload;
};

class WebSocketTransport {
 public:
  using DataCallback = std::function<void(const uint8_t *, size_t, bool)>;
  using EventCallback = std::function<void()>;
  using ErrorCallback = std::function<void(const std::string &)>;

  virtual ~WebSocketTransport() = default;
  virtual void SetCallbacks(DataCallback data, EventCallback closed,
                            ErrorCallback error) = 0;
  virtual bool Connect(const std::string &url, const std::string &token,
                       const std::string &device_id,
                       const std::string &client_id, int version) = 0;
  virtual bool Send(const void *data, size_t size, bool binary) = 0;
  virtual void Close() = 0;
};

std::unique_ptr<WebSocketTransport> CreateNuttxWebSocketTransport();

class Protocol {
 public:
  using AudioCallback = std::function<void(AudioPacket)>;
  using JsonCallback = std::function<void(const cJSON *)>;
  using EventCallback = std::function<void()>;
  using ErrorCallback = std::function<void(const std::string &)>;

  explicit Protocol(std::unique_ptr<WebSocketTransport> transport);
  ~Protocol();

  Protocol(const Protocol &) = delete;
  Protocol &operator=(const Protocol &) = delete;

  void SetCallbacks(AudioCallback audio, JsonCallback json,
                    EventCallback opened, EventCallback closed,
                    ErrorCallback error);
  bool Open(const std::string &url, const std::string &token,
            const std::string &device_id, const std::string &client_id,
            int version);
  void Close();
  bool SendAudio(AudioPacket packet);
  bool SendStartListening(ListeningMode mode);
  bool SendStopListening();
  bool SendAbortSpeaking(AbortReason reason);
  bool SendWakeWordDetected(const std::string &wake_word);
  bool SendMcpMessage(const std::string &payload);

  int server_sample_rate() const { return server_sample_rate_; }
  int server_frame_duration_ms() const { return server_frame_duration_ms_; }
  const std::string &session_id() const { return session_id_; }

 private:
  bool SendJson(cJSON *root);
  void OnTransportData(const uint8_t *data, size_t size, bool binary);
  void ParseServerHello(const cJSON *root);

  std::unique_ptr<WebSocketTransport> transport_;
  AudioCallback audio_callback_;
  JsonCallback json_callback_;
  EventCallback opened_callback_;
  EventCallback closed_callback_;
  ErrorCallback error_callback_;
  int version_{1};
  int server_sample_rate_{24000};
  int server_frame_duration_ms_{60};
  std::string session_id_;
  bool hello_received_{false};
};

}  // namespace xiaozhi
