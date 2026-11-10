#include "http_request_xr806.h"

#ifdef USE_XR806

#include <algorithm>
#include <cstring>

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

// The SDK headers define BOOL, VOID, TRUE and FALSE as macros, so they come last and stay in this file.
extern "C" {
#include "xr_config.h"
#include "net/HTTPClient/HTTPCUsr_api.h"
#include "mbedtls/base64.h"
}
// lwIP's POSIX socket names are function-like macros that would rename HttpContainer::read().
#undef read
#undef write
#undef close

namespace esphome::http_request {

ESPHOME_LOG_TAG(TAG, "http_request");

// Responses not yet closed; only touched from the main loop.
static uint8_t open_sessions = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Longest response header line that is read; longer ones end the search for that header.
static constexpr size_t HEADER_LINE_MAX = 512;

/// Decodes %XX escapes; invalid escapes are kept as they are.
static std::string percent_decode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    uint8_t byte;
    if (in[i] == '%' && i + 2 < in.size() && parse_hex(in.c_str() + i + 1, 2, &byte, 1) == 2) {
      out += static_cast<char>(byte);
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

/// Returns url without "user:password@". The SDK client does not parse it, so when authorization is not null
/// it receives the matching "Basic ..." header value (empty when the URL has no user information).
static std::string strip_userinfo(const std::string &url, std::string *authorization) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos)
    return url;
  const size_t host_start = scheme_end + 3;
  const size_t authority_end = std::min(url.find_first_of("/?#", host_start), url.size());
  const size_t at = url.rfind('@', authority_end);
  if (at == std::string::npos || at < host_start)
    return url;
  if (authorization != nullptr) {
    const std::string userinfo = percent_decode(url.substr(host_start, at - host_start));
    std::string encoded(((userinfo.size() + 2) / 3) * 4 + 1, '\0');
    size_t written = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char *>(&encoded[0]), encoded.size(), &written,
                          reinterpret_cast<const unsigned char *>(userinfo.data()), userinfo.size());
    encoded.resize(written);
    *authorization = "Basic " + encoded;
  }
  return url.substr(0, host_start) + url.substr(at + 1);
}

/// Calls on_value with the value of every response header called name.
template<typename F> static void for_each_header(uint32_t session, const char *name, F &&on_value) {
  char line[HEADER_LINE_MAX];
  if (HTTPClientFindFirstHeader(session, const_cast<char *>(name), line, nullptr) != HTTP_CLIENT_SUCCESS)
    return;
  for (;;) {
    UINT32 len = sizeof(line) - 1;
    if (HTTPClientGetNextHeader(session, line, &len) != HTTP_CLIENT_SUCCESS)
      break;
    // The SDK returns the whole line, "Name: value".
    const char *colon = std::strchr(line, ':');
    if (colon == nullptr)
      continue;
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
      value++;
    size_t value_len = std::strlen(value);
    while (value_len > 0 &&
           (value[value_len - 1] == ' ' || value[value_len - 1] == '\r' || value[value_len - 1] == '\n'))
      value_len--;
    on_value(std::string(value, value_len));
  }
  HTTPClientFindCloseHeader(session);
}

std::shared_ptr<HttpContainer> HttpRequestXR806::perform(const char *url, const char *method, const std::string &body,
                                                         const std::vector<Header> &request_headers,
                                                         const std::vector<std::string> &lower_case_collect_headers,
                                                         uint32_t timeout_ms) {
  std::string current = url;
  const char *current_method = method;
  for (uint16_t redirects = 0;; redirects++) {
    std::string location;
    auto container = this->perform_once_(current, current_method, body, request_headers, lower_case_collect_headers,
                                         timeout_ms, this->follow_redirects_ ? &location : nullptr);
    if (container == nullptr)
      return nullptr;
    if (location.empty() || redirects >= this->redirect_limit_) {
      if (!is_success(container->status_code)) {
        ESP_LOGE(TAG, "HTTP Request failed; URL: %s; Code: %d", strip_userinfo(current, nullptr).c_str(),
                 container->status_code);
        this->status_momentary_error(1000);
      }
      return container;
    }
    container->end();
    if (location.front() == '/') {
      // A path on the same server: keep the scheme and host of the current URL.
      const size_t host_start = current.find("://");
      const size_t path_start = host_start == std::string::npos ? std::string::npos : current.find('/', host_start + 3);
      current = current.substr(0, path_start) + location;
    } else {
      current = location;
    }
    if (container->status_code == HTTP_STATUS_SEE_OTHER)
      current_method = "GET";
    ESP_LOGD(TAG, "Following redirect to %s", strip_userinfo(current, nullptr).c_str());
  }
}

std::shared_ptr<HttpContainerXR806> HttpRequestXR806::perform_once_(
    const std::string &url, const char *method, const std::string &body, const std::vector<Header> &request_headers,
    const std::vector<std::string> &lower_case_collect_headers, uint32_t timeout_ms, std::string *location) {
  if (!network::is_connected()) {
    this->status_momentary_error(1000);
    ESP_LOGW(TAG, "HTTP Request failed; Not connected to network");
    return nullptr;
  }

  HTTP_VERB verb;
  if (std::strcmp(method, "GET") == 0) {
    verb = VerbGet;
  } else if (std::strcmp(method, "HEAD") == 0) {
    verb = VerbHead;
  } else if (std::strcmp(method, "POST") == 0) {
    verb = VerbPost;
  } else if (std::strcmp(method, "PUT") == 0) {
    verb = VerbPut;
  } else if (std::strcmp(method, "DELETE") == 0) {
    verb = VerbDelete;
  } else if (std::strcmp(method, "PATCH") == 0) {
    verb = VerbPatch;
  } else {
    ESP_LOGE(TAG, "HTTP method %s is not supported", method);
    return nullptr;
  }

  std::string authorization;
  const std::string request_url = strip_userinfo(url, &authorization);

  auto container = std::make_shared<HttpContainerXR806>();
  container->set_parent(this);
  container->set_secure(strncasecmp(url.c_str(), "https:", 6) == 0);
  container->timeout_s_ = std::max<uint32_t>(1, (timeout_ms + 999) / 1000);
  const uint32_t start = millis();

  // Certificate verification is not available; config validation requires verify_ssl: false.
  HTTPC_set_ssl_verify_mode(0);
  container->session_ = HTTPClientOpenRequest(0);
  if (container->session_ == 0) {
    ESP_LOGW(TAG, "HTTP Request failed; no memory for a session");
    this->status_momentary_error(1000);
    return nullptr;
  }
  open_sessions++;
  const uint32_t session = container->session_;
  HTTPClientSetVerb(session, verb);
  if (this->useragent_ != nullptr)
    HTTPClientAddRequestHeaders(session, const_cast<char *>("User-Agent"), const_cast<char *>(this->useragent_), FALSE);
  if (!authorization.empty()) {
    HTTPClientAddRequestHeaders(session, const_cast<char *>("Authorization"), const_cast<char *>(authorization.c_str()),
                                FALSE);
  }
  for (const auto &header : request_headers) {
    HTTPClientAddRequestHeaders(session, const_cast<char *>(header.name.c_str()),
                                const_cast<char *>(header.value.c_str()), FALSE);
  }

  const bool has_body = HTTP_VERB_HAS_BODY(verb);
  App.feed_wdt();
  UINT32 rc = HTTPClientSendRequest(session, const_cast<char *>(request_url.c_str()),
                                    has_body ? const_cast<char *>(body.data()) : nullptr, has_body ? body.size() : 0,
                                    has_body ? TRUE : FALSE, container->timeout_s_, 0);
  if (rc == HTTP_CLIENT_SUCCESS)
    rc = HTTPClientRecvResponse(session, container->timeout_s_);
  App.feed_wdt();
  if (rc != HTTP_CLIENT_SUCCESS) {
    ESP_LOGW(TAG, "HTTP Request failed; URL: %s; Error: %u", request_url.c_str(), static_cast<unsigned>(rc));
    this->status_momentary_error(1000);
    container->end();
    return nullptr;
  }

  HTTP_CLIENT info{};
  HTTPClientGetInfo(session, &info);
  container->status_code = static_cast<int>(info.HTTPStatusCode);
  bool has_length = false;
  for_each_header(session, "content-length", [&has_length](const std::string &) { has_length = true; });
  if (verb == VerbHead) {
    container->content_length = 0;
  } else if ((info.HttpFlags & HTTP_CLIENT_FLAG_CHUNKED) != 0 || !has_length) {
    // Chunked, or a body that ends when the server closes the connection: read until the end of the stream.
    container->set_chunked(true);
  } else {
    container->content_length = info.TotalResponseBodyLength;
  }
  ESP_LOGD(TAG, "Status: %d, Content-Length: %u", container->status_code,
           static_cast<unsigned>(container->content_length));

  for (const auto &name : lower_case_collect_headers) {
    for_each_header(session, name.c_str(), [&container, &name](const std::string &value) {
      ESP_LOGD(TAG, "Received response header, name: %s, value: %s", name.c_str(), value.c_str());
      container->response_headers_.push_back({name, value});
    });
  }
  if (location != nullptr && is_redirect(container->status_code))
    for_each_header(session, "location", [location](const std::string &value) { *location = value; });

  container->duration_ms = millis() - start;
  return container;
}

HttpContainerXR806::~HttpContainerXR806() { this->end(); }

int HttpContainerXR806::read(uint8_t *buf, size_t max_len) {
  if (this->session_ == 0)
    return HTTP_ERROR_CONNECTION_CLOSED;
  if (this->end_of_stream_ || this->is_read_complete())
    return 0;
  if (!this->is_chunked_)
    max_len = std::min(max_len, this->content_length - this->bytes_read_);

  const uint32_t start = millis();
  UINT32 received = 0;
  const UINT32 rc = HTTPClientReadData(this->session_, buf, max_len, this->timeout_s_, &received);
  this->duration_ms += millis() - start;
  this->bytes_read_ += received;

  if (rc == HTTP_CLIENT_EOS) {
    this->end_of_stream_ = true;
    if (this->is_chunked_) {
      // The whole body has arrived; from here the base class reports the read as complete.
      this->set_chunked(false);
      this->content_length = this->bytes_read_;
    }
  } else if (rc != HTTP_CLIENT_SUCCESS && received == 0) {
    ESP_LOGW(TAG, "Reading the response failed; Error: %u", static_cast<unsigned>(rc));
    return HTTP_ERROR_CONNECTION_CLOSED;
  }
  return static_cast<int>(received);
}

void HttpContainerXR806::end() {
  if (this->session_ == 0)
    return;
  HTTPClientCloseRequest(&this->session_);
  this->session_ = 0;
  open_sessions--;
}

bool HttpContainerXR806::any_open() { return open_sessions != 0; }

}  // namespace esphome::http_request

#endif  // USE_XR806
