#pragma once
#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_XR806_HTTP_PORTAL)

#include <stddef.h>
#include <stdint.h>

#include "esphome/core/component.h"

namespace esphome::xr806 {

// Transitional native lwIP HTTP/DNS server for XR806 bring-up.
// A future AsyncWebServer-compatible backend can replace this component.
class XR806HTTPServer : public Component {
 public:
  void set_port(uint16_t port) { this->port_ = port; }
  void set_captive_dns(bool enable) { this->captive_dns_ = enable; }
  void set_ap_only(bool enable) { this->ap_only_ = enable; }
  void set_portal_active(bool active) { this->portal_active_ = active; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }
  void dump_config() override;

 protected:
  static constexpr size_t MAX_REQUEST = 1024;
  static constexpr size_t MAX_RESPONSE = 2304;

  bool is_ap_active_() const;
  bool start_http_();
  bool start_dns_();
  void handle_dns_();
  void accept_client_();
  void handle_client_();
  void prepare_request_();
  void send_response_(int status, const char *content_type, const char *body);
  void send_static_response_(int status, const char *content_type, const uint8_t *body, size_t size,
                             const char *encoding);
  void close_client_();

  uint16_t port_{80};
  bool captive_dns_{false};
  bool ap_only_{false};
  bool portal_active_{true};
  bool ap_active_{false};
  bool save_pending_{false};
  int listen_fd_{-1};
  int dns_fd_{-1};
  int client_fd_{-1};
  uint32_t last_http_attempt_{0};
  uint32_t last_dns_attempt_{0};
  uint32_t client_started_{0};
  size_t request_used_{0};
  size_t response_len_{0};
  size_t response_sent_{0};
  const uint8_t *static_body_{nullptr};
  size_t static_body_len_{0};
  size_t static_body_sent_{0};
  char request_[MAX_REQUEST + 1]{};
  char response_[MAX_RESPONSE]{};
  char pending_ssid_[33]{};
  char pending_password_[65]{};
};

}  // namespace esphome::xr806
#endif
