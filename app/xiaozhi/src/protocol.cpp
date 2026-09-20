#include "xiaozhi/protocol.h"

#include <arpa/inet.h>
#include <netutils/cJSON.h>
#include <nuttx/config.h>

#include <cstdio>
#include <cstring>
#include <utility>

namespace xiaozhi {
namespace {

struct BinaryProtocol2 {
  uint16_t version;
  uint16_t type;
  uint32_t reserved;
  uint32_t timestamp;
  uint32_t payload_size;
  uint8_t payload[];
} __attribute__((packed));

struct BinaryProtocol3 {
  uint8_t type;
  uint8_t reserved;
  uint16_t payload_size;
  uint8_t payload[];
} __attribute__((packed));

const char *ListeningModeName(ListeningMode mode) {
  switch (mode) {
  case ListeningMode::kManualStop:
    return "manual";
  case ListeningMode::kRealtime:
    return "realtime";
  case ListeningMode::kAutoStop:
  default:
    return "auto";
  }
}

} // namespace

Protocol::Protocol(std::unique_ptr<WebSocketTransport> transport)
    : transport_(std::move(transport)) {
  transport_->SetCallbacks(
      [this](const uint8_t *data, size_t size, bool binary) {
        OnTransportData(data, size, binary);
      },
      [this]() {
        hello_received_ = false;
        if (closed_callback_) {
          closed_callback_();
        }
      },
      [this](const std::string &message) {
        if (error_callback_) {
          error_callback_(message);
        }
      });
}

Protocol::~Protocol() { Close(); }

void Protocol::SetCallbacks(AudioCallback audio, JsonCallback json,
                            EventCallback opened, EventCallback closed,
                            ErrorCallback error) {
  audio_callback_ = std::move(audio);
  json_callback_ = std::move(json);
  opened_callback_ = std::move(opened);
  closed_callback_ = std::move(closed);
  error_callback_ = std::move(error);
}

bool Protocol::Open(const std::string &url, const std::string &token,
                    const std::string &device_id, const std::string &client_id,
                    int version) {
  version_ = version;
  hello_received_ = false;
  session_id_.clear();
  if (!transport_->Connect(url, token, device_id, client_id, version_)) {
    return false;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "hello");
  cJSON_AddNumberToObject(root, "version", version_);
  cJSON_AddStringToObject(root, "transport", "websocket");
  cJSON *features = cJSON_AddObjectToObject(root, "features");
  cJSON_AddBoolToObject(features, "mcp", true);
  cJSON *audio = cJSON_AddObjectToObject(root, "audio_params");
  cJSON_AddStringToObject(audio, "format", "opus");
  cJSON_AddNumberToObject(audio, "sample_rate", 16000);
  cJSON_AddNumberToObject(audio, "channels", 1);
  cJSON_AddNumberToObject(audio, "frame_duration", 60);
  return SendJson(root);
}

void Protocol::Close() {
  if (transport_) {
    transport_->Close();
  }
  hello_received_ = false;
}

bool Protocol::SendJson(cJSON *root) {
  if (root == nullptr) {
    return false;
  }

  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (text == nullptr) {
    return false;
  }

  const bool result = transport_->Send(text, std::strlen(text), false);
  cJSON_free(text);
  return result;
}

bool Protocol::SendAudio(AudioPacket packet) {
  if (version_ == 2) {
    std::vector<uint8_t> data(sizeof(BinaryProtocol2) + packet.payload.size());
    auto *header = reinterpret_cast<BinaryProtocol2 *>(data.data());
    header->version = htons(2);
    header->type = htons(0);
    header->reserved = 0;
    header->timestamp = htonl(packet.timestamp);
    header->payload_size = htonl(packet.payload.size());
    std::memcpy(header->payload, packet.payload.data(), packet.payload.size());
    return transport_->Send(data.data(), data.size(), true);
  }

  if (version_ == 3) {
    if (packet.payload.size() > UINT16_MAX) {
      return false;
    }
    std::vector<uint8_t> data(sizeof(BinaryProtocol3) + packet.payload.size());
    auto *header = reinterpret_cast<BinaryProtocol3 *>(data.data());
    header->type = 0;
    header->reserved = 0;
    header->payload_size = htons(packet.payload.size());
    std::memcpy(header->payload, packet.payload.data(), packet.payload.size());
    return transport_->Send(data.data(), data.size(), true);
  }

  return transport_->Send(packet.payload.data(), packet.payload.size(), true);
}

bool Protocol::SendStartListening(ListeningMode mode) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root, "type", "listen");
  cJSON_AddStringToObject(root, "state", "start");
  cJSON_AddStringToObject(root, "mode", ListeningModeName(mode));
  return SendJson(root);
}

bool Protocol::SendStopListening() {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root, "type", "listen");
  cJSON_AddStringToObject(root, "state", "stop");
  return SendJson(root);
}

bool Protocol::SendAbortSpeaking(AbortReason reason) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root, "type", "abort");
  if (reason == AbortReason::kWakeWordDetected) {
    cJSON_AddStringToObject(root, "reason", "wake_word_detected");
  }
  return SendJson(root);
}

bool Protocol::SendWakeWordDetected(const std::string &wake_word) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root, "type", "listen");
  cJSON_AddStringToObject(root, "state", "detect");
  cJSON_AddStringToObject(root, "text", wake_word.c_str());
  return SendJson(root);
}

bool Protocol::SendMcpMessage(const std::string &payload) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root, "type", "mcp");
  cJSON *mcp = cJSON_Parse(payload.c_str());
  if (mcp == nullptr) {
    cJSON_Delete(root);
    return false;
  }
  cJSON_AddItemToObject(root, "payload", mcp);
  return SendJson(root);
}

void Protocol::OnTransportData(const uint8_t *data, size_t size, bool binary) {
  if (binary) {
    AudioPacket packet;
    packet.sample_rate = server_sample_rate_;
    packet.frame_duration_ms = server_frame_duration_ms_;
    const uint8_t *payload = data;
    size_t payload_size = size;

    if (version_ == 2) {
      if (size < sizeof(BinaryProtocol2)) {
        return;
      }
      const auto *header = reinterpret_cast<const BinaryProtocol2 *>(data);
      payload_size = ntohl(header->payload_size);
      if (payload_size > size - sizeof(BinaryProtocol2)) {
        return;
      }
      packet.timestamp = ntohl(header->timestamp);
      payload = header->payload;
    } else if (version_ == 3) {
      if (size < sizeof(BinaryProtocol3)) {
        return;
      }
      const auto *header = reinterpret_cast<const BinaryProtocol3 *>(data);
      payload_size = ntohs(header->payload_size);
      if (payload_size > size - sizeof(BinaryProtocol3)) {
        return;
      }
      payload = header->payload;
    }

    packet.payload.assign(payload, payload + payload_size);
    if (audio_callback_) {
      audio_callback_(std::move(packet));
    }
    return;
  }

  cJSON *root =
      cJSON_ParseWithLength(reinterpret_cast<const char *>(data), size);
  if (root == nullptr) {
    return;
  }
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
  std::fprintf(stderr, "xiaozhi websocket: rx json=%.*s\n",
               static_cast<int>(size), reinterpret_cast<const char *>(data));
#endif

  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (cJSON_IsString(type) && std::strcmp(type->valuestring, "hello") == 0) {
    ParseServerHello(root);
  } else if (json_callback_) {
    json_callback_(root);
  }
  cJSON_Delete(root);
}

void Protocol::ParseServerHello(const cJSON *root) {
  const cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
  if (!cJSON_IsString(transport) ||
      std::strcmp(transport->valuestring, "websocket") != 0) {
    if (error_callback_) {
      error_callback_("invalid server hello transport");
    }
    return;
  }

  const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
  if (cJSON_IsString(session)) {
    session_id_ = session->valuestring;
  }

  const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
  if (cJSON_IsObject(audio)) {
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
    const cJSON *duration =
        cJSON_GetObjectItemCaseSensitive(audio, "frame_duration");
    if (cJSON_IsNumber(rate)) {
      server_sample_rate_ = rate->valueint;
    }
    if (cJSON_IsNumber(duration)) {
      server_frame_duration_ms_ = duration->valueint;
    }
  }

  if (!hello_received_) {
    hello_received_ = true;
    if (opened_callback_) {
      opened_callback_();
    }
  }
}

} // namespace xiaozhi
