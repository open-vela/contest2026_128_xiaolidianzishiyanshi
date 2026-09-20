#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xiaozhi {

struct HttpRequest {
  std::string method;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  long timeout_seconds{30};
};

struct HttpResponse {
  long status_code{0};
  std::string body;
};

class HttpClient {
public:
  virtual ~HttpClient() = default;
  virtual bool Perform(const HttpRequest &request, HttpResponse *response,
                       std::string *error) = 0;
};

std::unique_ptr<HttpClient> CreateNuttxHttpClient();

} // namespace xiaozhi
