#include "xiaozhi/activation.h"

#include <netutils/cJSON.h>

#include <utility>

namespace xiaozhi {
namespace {

void AddString(cJSON *object, const char *name, const std::string &value) {
  cJSON_AddStringToObject(object, name, value.c_str());
}

std::string TakeJson(cJSON *root) {
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (text == nullptr) {
    return {};
  }
  std::string result(text);
  cJSON_free(text);
  return result;
}

void ReadString(const cJSON *object, const char *name, std::string *output) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (cJSON_IsString(item)) {
    *output = item->valuestring;
  }
}

} // namespace

ActivationClient::ActivationClient(std::unique_ptr<HttpClient> http,
                                   std::string ota_url,
                                   DeviceRegistration registration)
    : http_(std::move(http)), ota_url_(std::move(ota_url)),
      registration_(std::move(registration)) {}

HttpRequest ActivationClient::MakeRequest(const std::string &url,
                                          const std::string &body) const {
  HttpRequest request;
  request.method = "POST";
  request.url = url;
  request.body = body;
  request.headers = {
      {"Activation-Version", "1"},
      {"Device-Id", registration_.device_id},
      {"Client-Id", registration_.client_id},
      {"User-Agent",
       registration_.board_name + "/" + registration_.application_version},
      {"Accept-Language", registration_.language},
      {"Content-Type", "application/json"},
  };
  return request;
}

std::string ActivationClient::MakeSystemInfoJson() const {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "version", 2);
  AddString(root, "language", registration_.language);
  AddString(root, "mac_address", registration_.device_id);
  AddString(root, "uuid", registration_.client_id);
  AddString(root, "chip_model_name", "esp32p4");

  cJSON *application = cJSON_AddObjectToObject(root, "application");
  AddString(application, "name", registration_.application_name);
  AddString(application, "version", registration_.application_version);
  AddString(application, "idf_version", "NuttX/openvela");

  cJSON *board = cJSON_AddObjectToObject(root, "board");
  AddString(board, "type", registration_.board_type);
  AddString(board, "name", registration_.board_name);
  AddString(board, "manufacturer", registration_.board_manufacturer);
  AddString(board, "mac", registration_.device_id);
  return TakeJson(root);
}

bool ActivationClient::Check(ActivationResult *result, std::string *error) {
  if (result == nullptr) {
    if (error != nullptr) {
      *error = "activation result pointer is null";
    }
    return false;
  }

  if (http_ == nullptr) {
    if (error != nullptr) {
      *error = "failed to initialize HTTP client";
    }
    return false;
  }

  HttpResponse response;
  if (!http_->Perform(MakeRequest(ota_url_, MakeSystemInfoJson()), &response,
                      error)) {
    return false;
  }
  if (response.status_code != 200) {
    if (error != nullptr) {
      *error = "version endpoint returned HTTP " +
               std::to_string(response.status_code) + ": " + response.body;
    }
    return false;
  }

  cJSON *root =
      cJSON_ParseWithLength(response.body.data(), response.body.size());
  if (root == nullptr) {
    if (error != nullptr) {
      *error = "version endpoint returned invalid JSON";
    }
    return false;
  }

  ActivationResult parsed;
  const cJSON *activation =
      cJSON_GetObjectItemCaseSensitive(root, "activation");
  if (cJSON_IsObject(activation)) {
    ReadString(activation, "code", &parsed.activation_code);
    ReadString(activation, "message", &parsed.activation_message);
    ReadString(activation, "challenge", &parsed.activation_challenge);
    const cJSON *timeout =
        cJSON_GetObjectItemCaseSensitive(activation, "timeout_ms");
    if (cJSON_IsNumber(timeout) && timeout->valueint > 0) {
      parsed.activation_timeout_ms = timeout->valueint;
    }
  }

  const cJSON *websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
  if (cJSON_IsObject(websocket)) {
    ReadString(websocket, "url", &parsed.websocket_url);
    ReadString(websocket, "token", &parsed.websocket_token);
    const cJSON *version =
        cJSON_GetObjectItemCaseSensitive(websocket, "version");
    if (cJSON_IsNumber(version)) {
      parsed.websocket_version = version->valueint;
    }
  }

  cJSON_Delete(root);
  *result = std::move(parsed);
  return true;
}

ActivationPollResult ActivationClient::Activate(const ActivationResult &result,
                                                std::string *error) {
  if (result.activation_challenge.empty()) {
    if (error != nullptr) {
      *error = "activation response did not include a challenge";
    }
    return ActivationPollResult::kFailed;
  }

  std::string url = ota_url_;
  if (url.empty() || url.back() != '/') {
    url += '/';
  }
  url += "activate";

  /* Activation-Version 1 devices do not own an eFuse HMAC key.  Upstream
   * deliberately posts an empty object while the server associates the
   * challenge with Device-Id and Client-Id from the headers.
   */
  HttpResponse response;
  if (!http_->Perform(MakeRequest(url, "{}"), &response, error)) {
    return ActivationPollResult::kFailed;
  }
  if (response.status_code == 200) {
    return ActivationPollResult::kActivated;
  }
  if (response.status_code == 202) {
    return ActivationPollResult::kPending;
  }
  if (error != nullptr) {
    *error = "activation endpoint returned HTTP " +
             std::to_string(response.status_code) + ": " + response.body;
  }
  return ActivationPollResult::kFailed;
}

} // namespace xiaozhi
