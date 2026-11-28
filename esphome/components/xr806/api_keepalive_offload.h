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

/// Keepalive by the changed WLAN firmware (ha_wifi_fw_keepalive): while the CPU sleeps, the WLAN
/// core sends pre-encrypted PingResponse frames on the API connection, so Home Assistant does not ping and wake it.
///
/// Before standby the API encrypts a list of frames as usual; the socket write is caught (lwip_write is linked
/// through a wrapper) and the frames go to the firmware's keepalive with lwIP's sequence numbers. The firmware
/// sends frame k after Home Assistant acknowledged k frames, one per period. After the wake the host reads k from
/// the firmware, moves lwIP's sequence numbers by k frames and sends the frames the firmware did not send, so the
/// Noise counters of both sides match again. Anything unexpected closes the connection.
/// True when the standby that just ended was the planned one: the patched firmware sent its last frame and Home
/// Assistant's ping woke the host. The coordinator then treats it like its timer wake.
bool keepalive_offload_planned_wake();
/// Right before standby is committed: true while the WLAN driver's stay-awake timer runs, for a few attempts in a
/// row. The attempt then waits another quiet interval instead of being refused by the driver.
bool keepalive_offload_postpone_for_driver();

class XR806APIKeepaliveOffload {
 public:
  void configure(uint8_t period_s, bool host_sleep) {
    this->period_s_ = period_s;
    this->host_sleep_ = host_sleep;
  }
  bool enabled() const { return this->period_s_ != 0; }
  /// Right before the chip sleeps. wake_after_ms is the coordinator's wake timer; it is shortened when the frame
  /// list ends earlier.
  void arm(uint32_t wake_after_ms);
  /// After the wake, or when standby did not happen.
  void disarm(const XR806StandbyResume *event);
  /// After a WLAN wake while armed: whether the firmware had sent its last frame, so this is the planned wake.
  bool planned_wake();
  /// False while standby should wait: shortly after the host sent held frames, when Home Assistant would ping
  /// before the first firmware frame (then a ping response is sent first), or for up to half a second per standby
  /// attempt while the WLAN driver's stay-awake timer runs. Called from the standby preflight on every loop while
  /// standby is requested.
  bool ready_for_standby();

 protected:
  bool driver_may_suspend_(uint32_t now);
  bool capture_frames_(api::APIConnection *client, uint32_t max_frames);
  bool configure_firmware_();
  void stop_firmware_();
  void send_unsent_(uint32_t from);
  void await_peer_message_();
  void close_(const char *reason);

  api::APIConnection *client_{nullptr};
  int fd_{-1};
  uint32_t armed_ms_{0};
  uint32_t burst_ms_{0};
  uint32_t stay_awake_asked_ms_{0};
  uint32_t snd_nxt_{0};
  uint32_t rcv_nxt_{0};
  uint16_t frame_len_{0};
  uint16_t stay_awake_waited_ms_{0};
  uint8_t frame_count_{0};
  uint8_t wake_frames_{0};
  uint8_t period_s_{0};
  bool host_sleep_{false};
};

}  // namespace esphome::xr806

#endif
