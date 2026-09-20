#include "xiaozhi/http.h"

#include <curl/curl.h>
#include <nuttx/config.h>

#include <cstring>
#include <memory>
#include <string>

namespace xiaozhi {
namespace {

size_t AppendResponse(char *data, size_t size, size_t count, void *context) {
  const size_t bytes = size * count;
  static_cast<std::string *>(context)->append(data, bytes);
  return bytes;
}

class NuttxHttpClient final : public HttpClient {
public:
  bool Perform(const HttpRequest &request, HttpResponse *response,
               std::string *error) override {
    if (response == nullptr) {
      if (error != nullptr) {
        *error = "HTTP response pointer is null";
      }
      return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
      if (error != nullptr) {
        *error = "curl_easy_init failed";
      }
      return false;
    }

    char error_buffer[CURL_ERROR_SIZE]{};
    struct curl_slist *headers = nullptr;
    for (const auto &header : request.headers) {
      const std::string line = header.first + ": " + header.second;
      headers = curl_slist_append(headers, line.c_str());
    }

    response->body.clear();
    response->status_code = 0;
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(request.body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response->body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

    if (CONFIG_CONTEST2026_128_XIAOZHI_TLS_CA_PATH[0] != '\0') {
      curl_easy_setopt(curl, CURLOPT_CAINFO,
                       CONFIG_CONTEST2026_128_XIAOZHI_TLS_CA_PATH);
    }
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_TLS_ALLOW_INSECURE
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif

    const CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status_code);
    } else if (error != nullptr) {
      *error =
          error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(result);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
  }
};

} // namespace

std::unique_ptr<HttpClient> CreateNuttxHttpClient() {
  static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (initialized != CURLE_OK) {
    return nullptr;
  }
  return std::make_unique<NuttxHttpClient>();
}

} // namespace xiaozhi
