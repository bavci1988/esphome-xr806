#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_API) && defined(USE_XR806_STANDBY_CONTROLLER) && \
    defined(USE_XR806_API_KEEPALIVE_OFFLOAD)

#include <cstdint>

#include "standby_controller.h"

namespace esphome::api {
class APIConnection;
}  // namespace esphome::api

namespace esphome::xr806 {

/// Experiment with the patched WLAN firmware (experimental_wifi_fw_standby_patch): while the CPU sleeps, the WLAN
/// core sends pre-encrypted PingResponse frames on the API connection, so Home Assistant does not ping and wake it.
///
/// Before standby the API encrypts a list of frames as usual; the socket write is caught (lwip_write is linked
/// through a wrapper) and the frames go to the firmware's keepalive with lwIP's sequence numbers. The firmware
/// sends frame k after Home Assistant acknowledged k frames, one per period. After the wake the host reads k from
/// the firmware, moves lwIP's sequence numbers by k frames and sends the frames the firmware did not send, so the
/// Noise counters of both sides match again. Anything unexpected closes the connection.
class XR806APIKeepaliveOffload {
 public:
  void configure(uint8_t period_s, bool host_sleep, bool wake_on_peer) {
    this->period_s_ = period_s;
    this->host_sleep_ = host_sleep;
    this->wake_on_peer_ = wake_on_peer;
  }
  bool enabled() const { return this->period_s_ != 0; }
  /// Right before the chip sleeps. wake_after_ms is the coordinator's wake timer; it is shortened when the frame
  /// list ends earlier.
  void arm(uint32_t wake_after_ms);
  /// After the wake, or when standby did not happen.
  void disarm(const XR806StandbyResume *event);
  /// False while standby should wait: shortly after the host sent held frames, or when Home Assistant would ping
  /// before the first firmware frame (then a ping response is sent first). Called from the standby preflight on
  /// every loop while standby is requested.
  bool ready_for_standby();

 protected:
  bool capture_frames_(api::APIConnection *client, uint32_t max_frames);
  bool configure_firmware_();
  void stop_firmware_();
  void send_unsent_(uint32_t from);
  void close_(const char *reason);

  api::APIConnection *client_{nullptr};
  int fd_{-1};
  uint32_t armed_ms_{0};
  uint32_t burst_ms_{0};
  uint32_t snd_nxt_{0};
  uint32_t rcv_nxt_{0};
  uint16_t frame_len_{0};
  uint8_t frame_count_{0};
  uint8_t period_s_{0};
  bool host_sleep_{false};
  bool wake_on_peer_{false};
};

}  // namespace esphome::xr806

#endif
