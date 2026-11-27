#pragma once

#include <cstdint>

#include "esphome/core/gpio_pin.h"

namespace esphome::tuya {

/// What the low_power protocol needs from the platform that powers the Wi-Fi module.
///
/// A platform with power management implements this. The contract:
/// - Pass the host to TuyaUART::set_low_power_host() before TuyaUART::setup(). Doing so tells the platform side
///   that the external MCU decides when the module may sleep. mcu_wake_output() must be valid by then.
/// - Enter standby only while standby_requested() and after low_power_prepare_for_standby() returned true. It
///   returns false once on purpose right after it sent CMD12/01; after every false return, call
///   low_power_standby_cancelled() and try again later.
/// - After a standby, call low_power_standby_resumed() and clear the own standby request. With
///   standby_keeps_network(), a resume may ask for the standby again at once (the MCU's consent is kept).
/// - Everything runs on the main loop thread.
/// Without a host every sleep request of the MCU is answered with "busy".
class TuyaLowPowerHost {
 public:
  /// Whether the MCU link is idle and in sync, so sleep is allowed from its side.
  virtual void set_mcu_synchronized(bool synchronized) = 0;
  /// Monotonic count of received bytes lost or damaged by the MCU UART; 0 if the platform cannot tell.
  virtual uint32_t uart_rx_error_count() const { return 0; }
  /// An update is being written, so the module must stay awake.
  virtual bool ota_busy() const = 0;

  /// Whether the platform can put the module into standby on the MCU's request.
  virtual bool standby_supported() const = 0;
  /// The standby wake input the platform listens on, or -1.
  virtual int standby_wake_pin() const = 0;
  /// Whether standby is configured to wait for the MCU's consent.
  virtual bool standby_requires_external_sync() const = 0;
  /// Whether all network services could be stopped now.
  virtual bool standby_may_quiesce() const = 0;
  /// Whether all network services have stopped.
  virtual bool standby_services_quiesced() const = 0;
  /// Ask network services to stop (true) or resume (false) before a standby.
  virtual void request_preflight(bool requested) = 0;
  /// Whether stopping the network services is still requested; false ends the attempt with "busy".
  virtual bool preflight_requested() const = 0;
  /// Ask the platform to enter (true) or not to enter (false) standby.
  virtual void request_standby(bool requested) = 0;
  /// Whether the platform still means to enter standby; false ends the MCU's consent.
  virtual bool standby_requested() const = 0;
  /// Whether the platform is taking the network down on purpose for a standby.
  virtual bool network_quiescing() const = 0;
  /// Whether the network stays connected during standby, so a timer or received network data can wake the module
  /// while the MCU still sleeps.
  virtual bool standby_keeps_network() const { return false; }
  /// Maps the wake lines named in the MCU's product info (pin numbers of the module the MCU firmware was
  /// written for) to this module. With create_mcu_wake_output, also sets up the output to the MCU's wake line
  /// (see mcu_wake_output()). Returns the wake input in standby_wake_pin() numbering, or -1.
  virtual int configure_wake_lines(int /*wake_gpio*/, int /*mcu_wake_gpio*/, bool /*mcu_wake_active_high*/,
                                   bool /*create_mcu_wake_output*/) {
    return -1;
  }
  /// Platform-specific bit mask of what keeps the module awake, for diagnostics; 0 if unknown.
  virtual uint32_t standby_blockers() const { return 0; }
  /// The output to the MCU's wake line set up by configure_wake_lines(), or nullptr.
  virtual GPIOPin *mcu_wake_output() { return nullptr; }

 protected:
  ~TuyaLowPowerHost() = default;
};

}  // namespace esphome::tuya
