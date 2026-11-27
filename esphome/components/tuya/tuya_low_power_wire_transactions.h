#pragma once

// Protocol-neutral MCU wire-transaction gate for Tuya low_power.
// No sequence numbers exist in the original protocol: serializing writes
// prevents local overlap but cannot prove an arbitrarily late same-CMD ACK
// belongs to the latest transaction. An ambiguous timeout blocks new writes.
// Waiting commands: CMD01, CMD02, CMD09, and for a version-3 MCU the write (CMD06) and status query (CMD08).
#include <cstddef>
#include <cstdint>

namespace esphome::tuya {

class TuyaLowPowerWireTransactions {
 public:
  // REPORT: a version-3 write (CMD06) or status query (CMD08), both answered by the MCU's CMD07 report
  enum class Kind : uint8_t { NONE, PRODUCT, NETWORK, WRITE, REPORT };
  static constexpr uint32_t ACK_TIMEOUT_MS = 1500;

  bool begin(uint8_t command, uint32_t now) {
    if (active_ || faulted_)
      return false;
    if (command == 0x01) {
      kind_ = Kind::PRODUCT;
    } else if (command == 0x02) {
      kind_ = Kind::NETWORK;
    } else if (command == 0x09) {
      kind_ = Kind::WRITE;
    } else if (command == 0x06 || command == 0x08) {
      kind_ = Kind::REPORT;
    } else {
      return false;  // Direct replies/notifications must not claim a queue slot.
    }
    active_ = true;
    command_ = command;
    started_ms_ = now;
    ++generation_;
    return true;
  }

  bool accepts(uint8_t command, size_t len) const {
    if (!active_)
      return false;
    switch (kind_) {
      case Kind::PRODUCT:
        return command == 0x01 && len != 0;
      case Kind::NETWORK:
        // Empty, or the two pin bytes of a version-3 MCU's working-mode answer
        return command == 0x02 && (len == 0 || len == 2);
      case Kind::WRITE:
        return command == 0x09 && len == 0;
      case Kind::REPORT:
        return command == 0x07 && len >= 4;
      default:
        return false;
    }
  }

  bool complete(uint8_t command, size_t len) {
    if (!accepts(command, len))
      return false;
    active_ = false;
    kind_ = Kind::NONE;
    ++generation_;
    return true;
  }

  // Handles ACKs arriving after the deadline but before a timeout sweep.
  // Wrap-safe for elapsed intervals shorter than 2^31 milliseconds.
  bool overdue(uint32_t now) const { return active_ && static_cast<uint32_t>(now - started_ms_) >= ACK_TIMEOUT_MS; }

  bool expire(uint32_t now) {
    if (!this->overdue(now))
      return false;
    active_ = false;
    kind_ = Kind::NONE;
    faulted_ = true;
    ++generation_;
    return true;
  }

  /// A version-3 write or status query without a CMD07 report in time. MCU firmware 1.0.9 applies writes without
  /// reporting them, and a late report carries its own values, so nothing is ambiguous: no quarantine.
  bool abandon_report(uint32_t now) {
    if (!this->overdue(now) || kind_ != Kind::REPORT)
      return false;
    active_ = false;
    kind_ = Kind::NONE;
    ++generation_;
    return true;
  }
  /// The waiting network-state command was not answered and will be sent again; it changes nothing on the MCU.
  bool abandon_network() {
    if (!active_ || kind_ != Kind::NETWORK)
      return false;
    active_ = false;
    kind_ = Kind::NONE;
    ++generation_;
    return true;
  }

  void transport_fault() {
    active_ = false;
    kind_ = Kind::NONE;
    faulted_ = true;
    ++generation_;
  }

  // Explicit full MCU status/identity recovery, not a CMD09 ACK.
  void recovered() {
    if (active_)
      return;
    faulted_ = false;
    ++generation_;
  }

  bool active() const { return active_; }
  bool faulted() const { return faulted_; }
  bool idle() const { return !active_ && !faulted_; }
  Kind kind() const { return kind_; }
  /// The waiting command, valid while active().
  uint8_t command() const { return command_; }
  uint32_t generation() const { return generation_; }

 private:
  Kind kind_{Kind::NONE};
  uint32_t started_ms_{0};
  uint32_t generation_{0};
  uint8_t command_{0};
  bool active_{false};
  bool faulted_{false};
};

}  // namespace esphome::tuya
