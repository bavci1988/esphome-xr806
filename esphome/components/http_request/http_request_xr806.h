#pragma once

#include "http_request.h"

#ifdef USE_XR806

namespace esphome::http_request {

class HttpRequestXR806;

/// Response of the XRadio SDK HTTP client (net/HTTPClient), read in pieces so a firmware image never sits in RAM.
class HttpContainerXR806 : public HttpContainer {
 public:
  ~HttpContainerXR806() override;
  int read(uint8_t *buf, size_t max_len) override;
  void end() override;

  /// True while any response is still open; standby must not switch the radio off under it.
  static bool any_open();

 protected:
  friend class HttpRequestXR806;
  uint32_t session_{0};  ///< SDK HTTP_SESSION_HANDLE; 0 once closed
  uint32_t timeout_s_{1};
  bool end_of_stream_{false};
};

/// http_request on XR806 through the XRadio SDK HTTP client, with TLS from mbedTLS.
/// HTTPS runs without certificate verification; user information in the URL is sent as Basic authentication.
class HttpRequestXR806 final : public HttpRequestComponent {
 public:
  HttpRequestXR806();
  /// True while a request waits in the queue or a response is still open; standby must wait for it.
  static bool any_busy();

 protected:
  std::shared_ptr<HttpContainer> perform(const char *url, const char *method, const std::string &body,
                                         const std::vector<Header> &request_headers,
                                         const std::vector<std::string> &lower_case_collect_headers,
                                         uint32_t timeout_ms) override;
  std::shared_ptr<HttpContainerXR806> perform_once_(const std::string &url, const char *method, const std::string &body,
                                                    const std::vector<Header> &request_headers,
                                                    const std::vector<std::string> &lower_case_collect_headers,
                                                    uint32_t timeout_ms, std::string *location);
};

}  // namespace esphome::http_request

#endif  // USE_XR806
