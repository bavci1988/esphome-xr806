#pragma once

#include <cstdint>
#include <cstddef>

#include "esphome/core/component.h"
#include "standby_coordinator_state.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

// Platform event metadata only: an MCU-specific driver decides whether
// a retained GPIO wake warrants CMD12/00, status reports or time sync.
struct XR806StandbyResume {
  uint32_t event_bitmap{0};
  int pm_return{-1};
  bool expected_source{false};
  bool wake_gpio{false};
  bool wake_timer{false};
};

// A participant owns its own transactions, IRQ setup and UART TX completion.
// Registration does not make that participant "idle" in the platform policy.
// A busy callback must return false and leave the underlying owner blocker set.
class XR806StandbyParticipant {
 public:
  virtual ~XR806StandbyParticipant() = default;
  // Optional nonblocking service drain, invoked while other owners still
  // have their busy votes asserted. Must never claim idle before close/drain.
  virtual void standby_preflight(bool requested) {}
  // Only the UART/MCU participant can satisfy the mandatory transport owner.
  virtual bool owns_uart() const { return false; }
  virtual uint32_t owned_services() const { return 0; }
  // A connected network owner can veto before acknowledging MCU CMD38.
  virtual bool may_quiesce() const { return true; }
  virtual bool prepare_for_standby() = 0;
  virtual void standby_cancelled() = 0;
  virtual void standby_resumed(const XR806StandbyResume &event) = 0;
};

class XR806StandbyController final : public Component {
 public:
  void set_wake_after_ms(uint32_t value) { wake_after_ms_ = value; }
  void set_quiet_ms(uint32_t value) { quiet_ms_ = value; }
  void set_min_awake_ms(uint32_t value) { min_awake_ms_ = value; }
  void set_wake_pin(uint8_t pin) {
    wake_pin_ = pin;
    has_wake_pin_ = true;
  }
  void set_wake_falling(bool value) { wake_falling_ = value; }
  void set_no_uart(bool value) { no_uart_ = value; }
  void set_no_network(bool value) { no_network_ = value; }
  void set_no_api(bool value) { no_api_ = value; }
  void set_no_ota(bool value) { no_ota_ = value; }
  void set_no_mdns(bool value) { no_mdns_ = value; }
  void set_external_sync_required(bool value) { external_sync_required_ = value; }
  void set_require_uart_participant(bool value) { require_uart_participant_ = value; }

  // Read-only platform capabilities; protocol participants validate their
  // own MCU product-info fields against these hardware settings.
  bool has_wake_pin() const { return this->has_wake_pin_; }
  uint8_t wake_pin() const { return this->wake_pin_; }
  bool requires_external_sync() const { return this->external_sync_required_; }

  bool may_quiesce() const;
  bool services_quiesced() const;
  void request_preflight(bool requested);
  bool preflight_requested() const { return this->preflight_requested_; }
  bool register_participant(XR806StandbyParticipant *participant);
  void request_standby(bool requested);

  void setup() override;
  void loop() override;
  void dump_config() override;

  // Must be initialized before the MCU's late setup, but after the
  // vendor platform/PM initialization already executed by the SDK.
  float get_setup_priority() const override { return setup_priority::DATA; }

 private:
  void attempt_standby_();
  void cancel_prepared_(size_t prepared);
  bool is_any_transport_unowned_() const;

  // MCU UART + WLAN + Native API + OTA + mDNS, plus one future board owner.
  static constexpr size_t MAX_PARTICIPANTS = 6;
  XR806StandbyParticipant *participants_[MAX_PARTICIPANTS]{};
  size_t participant_count_{0};
  XR806StandbyCoordinatorState state_{};

  uint32_t wake_after_ms_{3000};
  uint32_t quiet_ms_{250};
  uint32_t min_awake_ms_{5000};
  uint8_t wake_pin_{20};
  bool has_wake_pin_{false};
  bool wake_falling_{true};
  bool no_uart_{false};
  bool no_network_{false};
  bool no_api_{false};
  bool no_ota_{false};
  bool no_mdns_{false};
  bool external_sync_required_{false};
  bool require_uart_participant_{false};
  bool busy_{false};
  uint32_t request_since_ms_{0};
  uint32_t preflight_since_ms_{0};
  bool preflight_requested_{false};
  static constexpr uint32_t HANDSHAKE_PREFLIGHT_TIMEOUT_MS = 2000;
  static constexpr uint32_t PREFLIGHT_TIMEOUT_MS = 10000;
};

// nullptr when the production controller is not configured.
// This API stays generic; Tuya CMD01/12/38 and autobaud belong to the
// optional MCU component and must not be implemented here.
XR806StandbyController *standby_controller();

}  // namespace esphome::xr806
