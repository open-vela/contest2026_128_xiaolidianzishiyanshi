#include "xiaozhi/protocol.h"

#include <libwebsockets.h>
#include <nuttx/config.h>
#include <pthread.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xiaozhi {
namespace {

constexpr size_t kMaximumOutgoingMessages = 64;

int CreateWebSocketThread(pthread_t *thread, void *(*entry)(void *),
                          void *argument) {
  pthread_attr_t attributes;
  int result = pthread_attr_init(&attributes);
  if (result != 0) {
    return result;
  }

  result = pthread_attr_setstacksize(
      &attributes, CONFIG_CONTEST2026_128_XIAOZHI_WEBSOCKET_STACKSIZE);
  if (result == 0) {
    result = pthread_create(thread, &attributes, entry, argument);
  }
  pthread_attr_destroy(&attributes);
  return result;
}

struct OutgoingMessage {
  std::vector<uint8_t> data;
  bool binary{false};
};

class LwsWebSocket final : public WebSocketTransport {
public:
  ~LwsWebSocket() override { Close(); }

  void SetCallbacks(DataCallback data, EventCallback closed,
                    ErrorCallback error) override {
    std::lock_guard<std::mutex> lock(mutex_);
    data_callback_ = std::move(data);
    closed_callback_ = std::move(closed);
    error_callback_ = std::move(error);
  }

  bool Connect(const std::string &url, const std::string &token,
               const std::string &device_id, const std::string &client_id,
               int version) override {
    Close();
    if (!ParseUrl(url)) {
      ReportError("invalid WebSocket URL: " + url);
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      token_ = token;
      device_id_ = device_id;
      client_id_ = client_id;
      version_ = version;
      stop_ = false;
      initialized_ = false;
      intentional_close_ = false;
    }

    if (CreateWebSocketThread(&thread_, ThreadEntry, this) != 0) {
      ReportError("pthread_create for WebSocket service failed");
      return false;
    }
    pthread_setname_np(thread_, "xz-websocket");
    thread_started_ = true;

    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_.wait_for(lock, std::chrono::seconds(5),
                         [this]() { return initialized_; })) {
      lock.unlock();
      Close();
      ReportError("WebSocket service initialization timed out");
      return false;
    }
    return context_ != nullptr && wsi_ != nullptr;
  }

  bool Send(const void *data, size_t size, bool binary) override {
    if (data == nullptr || size == 0) {
      return false;
    }

    struct lws_context *context;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_ || !thread_started_) {
        return false;
      }

      /* Bound microphone backlog when the network stalls.  Prefer dropping
       * the oldest binary audio frame so control JSON is retained and audio
       * resumes close to real time instead of several seconds behind.
       */

      if (outgoing_.size() >= kMaximumOutgoingMessages) {
        auto stale = outgoing_.begin();
        while (stale != outgoing_.end() && !stale->binary) {
          ++stale;
        }
        if (stale == outgoing_.end()) {
          return false;
        }
        outgoing_.erase(stale);
      }

      const auto *first = static_cast<const uint8_t *>(data);
      outgoing_.push_back({std::vector<uint8_t>(first, first + size), binary});
      context = context_;
    }

    if (context != nullptr) {
      lws_cancel_service(context);
    }
    return true;
  }

  void Close() override {
    struct lws_context *context;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!thread_started_) {
        return;
      }
      stop_ = true;
      intentional_close_ = true;
      context = context_;
    }

    if (context != nullptr) {
      lws_cancel_service(context);
    }
    pthread_join(thread_, nullptr);

    std::lock_guard<std::mutex> lock(mutex_);
    thread_started_ = false;
    initialized_ = false;
    context_ = nullptr;
    wsi_ = nullptr;
    outgoing_.clear();
    receive_.clear();
  }

private:
  static void *ThreadEntry(void *argument) {
    static_cast<LwsWebSocket *>(argument)->ServiceLoop();
    return nullptr;
  }

  static int Callback(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *input, size_t length) {
    (void)user;
    auto *self =
        static_cast<LwsWebSocket *>(lws_context_user(lws_get_context(wsi)));
    return self == nullptr ? 0
                           : self->HandleCallback(wsi, reason, input, length);
  }

  bool ParseUrl(const std::string &url) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
      return false;
    }
    const std::string scheme = url.substr(0, scheme_end);
    ssl_ = scheme == "wss";
    if (!ssl_ && scheme != "ws") {
      return false;
    }

    const size_t authority = scheme_end + 3;
    const size_t slash = url.find('/', authority);
    std::string host_port = url.substr(authority, slash - authority);
    path_ = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
      host_ = host_port.substr(0, colon);
      char *end = nullptr;
      const long value = std::strtol(host_port.c_str() + colon + 1, &end, 10);
      if (end == nullptr || *end != '\0' || value <= 0 || value > 65535) {
        return false;
      }
      port_ = static_cast<int>(value);
    } else {
      host_ = host_port;
      port_ = ssl_ ? 443 : 80;
    }
    return !host_.empty();
  }

  void ServiceLoop() {
    static const struct lws_protocols protocols[] = {
        {"xiaozhi", Callback, 0, 64 * 1024, 0, nullptr, 0},
        {nullptr, nullptr, 0, 0, 0, nullptr, 0}};

    struct lws_context_creation_info info {};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user = this;
    if (CONFIG_CONTEST2026_128_XIAOZHI_TLS_CA_PATH[0] != '\0') {
      info.client_ssl_ca_filepath = CONFIG_CONTEST2026_128_XIAOZHI_TLS_CA_PATH;
    }

    struct lws_context *context = lws_create_context(&info);
    if (context == nullptr) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = true;
      }
      ready_.notify_all();
      ReportError("lws_create_context failed");
      return;
    }

    struct lws_client_connect_info connect {};
    connect.context = context;
    connect.address = host_.c_str();
    connect.port = port_;
    connect.path = path_.c_str();
    connect.host = host_.c_str();
    connect.origin = host_.c_str();
    connect.protocol = protocols[0].name;
    connect.ssl_connection = ssl_ ? LCCSCF_USE_SSL : 0;
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_TLS_ALLOW_INSECURE
    if (ssl_) {
      std::fprintf(stderr,
                   "xiaozhi websocket: WARNING: TLS verification disabled\n");
      connect.ssl_connection |=
          LCCSCF_ALLOW_INSECURE | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    }
#endif

    struct lws *wsi = lws_client_connect_via_info(&connect);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      context_ = context;
      wsi_ = wsi;
      initialized_ = true;
    }
    ready_.notify_all();

    if (wsi == nullptr) {
      ReportError("lws_client_connect_via_info failed");
    } else {
      while (true) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (stop_) {
            break;
          }
        }
        if (lws_service(context, 250) < 0) {
          ReportError("libwebsockets service loop failed");
          break;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (wsi_ != nullptr && !outgoing_.empty()) {
          lws_callback_on_writable(wsi_);
        }
      }
    }

    lws_context_destroy(context);
    std::lock_guard<std::mutex> lock(mutex_);
    context_ = nullptr;
    wsi_ = nullptr;
  }

  int AddHeader(struct lws *wsi, unsigned char **cursor, unsigned char *end,
                const char *name, const std::string &value) {
    if (value.empty()) {
      return 0;
    }
    return lws_add_http_header_by_name(
        wsi, reinterpret_cast<const unsigned char *>(name),
        reinterpret_cast<const unsigned char *>(value.c_str()), value.size(),
        cursor, end);
  }

  int HandleCallback(struct lws *wsi, enum lws_callback_reasons reason,
                     void *input, size_t length) {
    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
      auto **cursor = static_cast<unsigned char **>(input);
      unsigned char *end = *cursor + length;
      std::string auth = token_;
      if (!auth.empty() && auth.find(' ') == std::string::npos) {
        auth = "Bearer " + auth;
      }
      if (AddHeader(wsi, cursor, end, "authorization:", auth) < 0 ||
          AddHeader(wsi, cursor, end,
                    "protocol-version:", std::to_string(version_)) < 0 ||
          AddHeader(wsi, cursor, end, "device-id:", device_id_) < 0 ||
          AddHeader(wsi, cursor, end, "client-id:", client_id_) < 0) {
        return -1;
      }
      break;
    }

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      lws_callback_on_writable(wsi);
      break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
      DataCallback callback;
      std::vector<uint8_t> complete;
      bool binary = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (receive_.empty()) {
          receive_binary_ = lws_frame_is_binary(wsi) != 0;
        }
        const auto *bytes = static_cast<const uint8_t *>(input);
        receive_.insert(receive_.end(), bytes, bytes + length);
        if (!lws_is_final_fragment(wsi) ||
            lws_remaining_packet_payload(wsi) != 0) {
          break;
        }
        complete.swap(receive_);
        binary = receive_binary_;
        callback = data_callback_;
      }
      if (callback) {
        callback(complete.data(), complete.size(), binary);
      }
      break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
      OutgoingMessage message;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outgoing_.empty()) {
          break;
        }
        message = std::move(outgoing_.front());
        outgoing_.pop_front();
      }
      std::vector<uint8_t> frame(LWS_PRE + message.data.size());
      std::memcpy(frame.data() + LWS_PRE, message.data.data(),
                  message.data.size());
      const int written =
          lws_write(wsi, frame.data() + LWS_PRE, message.data.size(),
                    message.binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
      if (written != static_cast<int>(message.data.size())) {
        ReportError("short WebSocket write");
        return -1;
      }
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
      if (message.binary) {
        ++sent_audio_frames_;
        sent_audio_bytes_ += message.data.size();
        const auto now = std::chrono::steady_clock::now();
        if (now - sent_audio_report_time_ >= std::chrono::seconds(1)) {
          std::fprintf(stderr,
                       "xiaozhi websocket: tx audio_frames=%zu "
                       "audio_bytes=%zu\n",
                       sent_audio_frames_, sent_audio_bytes_);
          sent_audio_frames_ = 0;
          sent_audio_bytes_ = 0;
          sent_audio_report_time_ = now;
        }
      } else {
        std::fprintf(stderr, "xiaozhi websocket: tx json=%.*s\n",
                     static_cast<int>(message.data.size()),
                     reinterpret_cast<const char *>(message.data.data()));
      }
#endif
      std::lock_guard<std::mutex> lock(mutex_);
      if (!outgoing_.empty()) {
        lws_callback_on_writable(wsi);
      }
      break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      ReportError(input == nullptr ? "WebSocket connection failed"
                                   : static_cast<const char *>(input));
      break;

    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
      const auto *bytes = static_cast<const uint8_t *>(input);
      const unsigned int code =
          length >= 2 ? (static_cast<unsigned int>(bytes[0]) << 8) | bytes[1]
                      : 0;
      const int reason_length = length > 2 ? static_cast<int>(length - 2) : 0;
      std::fprintf(
          stderr, "xiaozhi websocket: peer close code=%u reason=%.*s\n", code,
          reason_length,
          reason_length == 0 ? "" : reinterpret_cast<const char *>(bytes + 2));
      break;
    }

    case LWS_CALLBACK_CLIENT_CLOSED: {
      EventCallback callback;
      bool intentional;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        wsi_ = nullptr;
        intentional = intentional_close_;
        callback = closed_callback_;
      }
      if (!intentional && callback) {
        callback();
      }
      break;
    }

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
      std::lock_guard<std::mutex> lock(mutex_);
      if (wsi_ != nullptr && !outgoing_.empty()) {
        lws_callback_on_writable(wsi_);
      }
      break;
    }

    default:
      break;
    }
    return 0;
  }

  void ReportError(const std::string &message) {
    ErrorCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = error_callback_;
    }
    if (callback) {
      callback(message);
    } else {
      std::fprintf(stderr, "xiaozhi websocket: %s\n", message.c_str());
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  pthread_t thread_{};
  bool thread_started_{false};
  bool initialized_{false};
  bool stop_{false};
  bool intentional_close_{false};
  bool ssl_{false};
  int port_{0};
  int version_{1};
  std::string host_;
  std::string path_;
  std::string token_;
  std::string device_id_;
  std::string client_id_;
  struct lws_context *context_{nullptr};
  struct lws *wsi_{nullptr};
  std::deque<OutgoingMessage> outgoing_;
  std::vector<uint8_t> receive_;
  bool receive_binary_{false};
  DataCallback data_callback_;
  EventCallback closed_callback_;
  ErrorCallback error_callback_;
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
  size_t sent_audio_frames_{0};
  size_t sent_audio_bytes_{0};
  std::chrono::steady_clock::time_point sent_audio_report_time_{
      std::chrono::steady_clock::now()};
#endif
};

} // namespace

std::unique_ptr<WebSocketTransport> CreateNuttxWebSocketTransport() {
  return std::make_unique<LwsWebSocket>();
}

} // namespace xiaozhi
