#include "web_server_xr806_component.h"
#ifdef USE_XR806

#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome::web_server_xr806 {

void WebServerXR806::loop() {
  // The server exists once a user called WebServerBase::init(); before that there is nothing to serve.
  if (auto *server = this->base_->get_server(); server != nullptr)
    server->loop();
}

}  // namespace esphome::web_server_xr806

#endif  // USE_XR806
