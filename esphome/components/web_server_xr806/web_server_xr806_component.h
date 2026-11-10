#pragma once
#include "esphome/core/defines.h"
#ifdef USE_XR806

#include "esphome/core/component.h"

namespace esphome::web_server_base {
class WebServerBase;
}  // namespace esphome::web_server_base

namespace esphome::web_server_xr806 {

/// Serves the lwIP HTTP sockets from the main loop. The XRadio SDK has no HTTP server
/// task, so the shared AsyncWebServer is polled here instead.
class WebServerXR806 final : public Component {
 public:
  explicit WebServerXR806(web_server_base::WebServerBase *base) : base_(base) {}
  void loop() override;

 protected:
  web_server_base::WebServerBase *base_;
};

}  // namespace esphome::web_server_xr806

#endif  // USE_XR806
