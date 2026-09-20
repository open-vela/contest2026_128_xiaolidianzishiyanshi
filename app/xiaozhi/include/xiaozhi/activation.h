#pragma once

#include <memory>
#include <string>

#include "xiaozhi/http.h"

namespace xiaozhi {

struct DeviceRegistration {
  std::string device_id;
  std::string client_id;
  std::string language;
  std::string board_type;
  std::string board_name;
  std::string board_manufacturer;
  std::string application_name;
  std::string application_version;
};

struct ActivationResult {
  std::string activation_code;
  std::string activation_message;
  std::string activation_challenge;
  int activation_timeout_ms{30000};
  std::string websocket_url;
  std::string websocket_token;
  int websocket_version{0};

  bool activation_required() const {
    return !activation_code.empty() || !activation_challenge.empty();
  }
};

enum class ActivationPollResult {
  kActivated,
  kPending,
  kFailed,
};

/* Platform-independent implementation of the activation/configuration part of
 * the upstream OTA endpoint.  Firmware partition updates intentionally stay
 * outside this class; the endpoint is needed here because it also provisions
 * the authenticated WebSocket URL.
 */
class ActivationClient {
public:
  ActivationClient(std::unique_ptr<HttpClient> http, std::string ota_url,
                   DeviceRegistration registration);

  bool Check(ActivationResult *result, std::string *error);
  ActivationPollResult Activate(const ActivationResult &result,
                                std::string *error);

private:
  HttpRequest MakeRequest(const std::string &url,
                          const std::string &body) const;
  std::string MakeSystemInfoJson() const;

  std::unique_ptr<HttpClient> http_;
  std::string ota_url_;
  DeviceRegistration registration_;
};

} // namespace xiaozhi
