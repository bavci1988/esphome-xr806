#pragma once
#include "esphome/core/defines.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/web_server_base/captive_dns.h"

namespace esphome::captive_portal {

class CaptivePortal final : public AsyncWebHandler, public Component {
 public:
  CaptivePortal(web_server_base::WebServerBase *base);
  void setup() override;
  void dump_config() override;
  void loop() override { this->dns_.loop(); }
  float get_setup_priority() const override;
  void start();
  bool is_active() const { return this->active_; }
  void end() {
    this->active_ = false;
    this->disable_loop();  // Stop processing DNS requests
    this->base_->deinit();
    this->dns_.stop();
  }

  bool canHandle(AsyncWebServerRequest *request) const override {
    if (!this->active_)
      return false;
    // Captive detection and the upstream portal UI use GET. Also accept a
    // bounded POST to /wifisave: browsers may submit credentials as a form.
    if (request->method() == HTTP_GET)
      return true;
    if (request->method() != HTTP_POST)
      return false;
#if defined(USE_ESP32) || defined(USE_XR806)
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(url_buf) == ESPHOME_F("/wifisave");
#else
    return request->url() == ESPHOME_F("/wifisave");
#endif
  }

  void handle_config(AsyncWebServerRequest *request);

  void handle_wifisave(AsyncWebServerRequest *request);

  void handleRequest(AsyncWebServerRequest *req) override;

 protected:
  web_server_base::WebServerBase *base_;
  bool initialized_{false};
  bool active_{false};
  web_server_base::CaptiveDNS dns_;
};

extern CaptivePortal *global_captive_portal;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::captive_portal

#endif
