#pragma once

// Host-testable gate for the low_power_v0 half-duplex MCU request stream.
// Tuya has no transaction numbers: only one request may be awaiting its
// matching ACK, and report completion is a separate protocol event.
#include <cstddef>
#include <cstdint>

namespace esphome::tuya {

class TuyaLowPowerTransactionGate {
 public:
  static constexpr uint32_t ACK_TIMEOUT_MS = 1500;
  static constexpr uint32_t REPLY_QUARANTINE_MS = 300;

  enum class Reply : uint8_t { NONE, MATCH, WRONG_TYPE, STALE };
  enum class Timeout : uint8_t { NONE, EXPIRED };

  bool begin(uint8_t command, uint32_t now) {
    if (pending_ || quarantined(now)) {
      return false;
    }
    // Only these outgoing commands have a transactional response in v0.
    if (command != 0x01 && command != 0x02 && command != 0x09) {
      return false;
    }
    pending_ = true;
    command_ = command;
    sent_ms_ = now;
    ++epoch_;
    return true;
  }

  // Called only on checksum-valid packets. Distinguish CMD01 descriptor,
  // CMD02 empty ACK, and CMD09 empty ACK. CMD05 is never an ACK of CMD09.
  Reply observe(uint8_t command, size_t payload_size, uint32_t now) {
    if (!pending_) {
      return quarantined(now) ? Reply::STALE : Reply::NONE;
    }
    if (command != command_) {
      return Reply::WRONG_TYPE;
    }
    if (static_cast<uint32_t>(now - sent_ms_) >= ACK_TIMEOUT_MS) {
      this->timeout(now);
      return Reply::STALE;
    }
    if ((command_ == 0x01 && payload_size == 0) ||
        (command_ != 0x01 && payload_size != 0)) {
      return Reply::WRONG_TYPE;
    }
    pending_ = false;
    return Reply::MATCH;
  }

  Timeout expire(uint32_t now) {
    if (!pending_ || static_cast<uint32_t>(now - sent_ms_) < ACK_TIMEOUT_MS) {
      return Timeout::NONE;
    }
    this->timeout(now);
    return Timeout::EXPIRED;
  }

  void timeout(uint32_t now) {
    pending_ = false;
    quarantine_until_ = now + REPLY_QUARANTINE_MS;
    quarantine_active_ = true;
    ++epoch_;
  }

  void reset(uint32_t now) {
    pending_ = false;
    quarantine_until_ = now + REPLY_QUARANTINE_MS;
    quarantine_active_ = true;
    ++epoch_;
  }

  bool pending() const { return pending_; }
  uint8_t command() const { return command_; }
  uint32_t epoch() const { return epoch_; }
  bool quarantined(uint32_t now) const {
    return quarantine_active_ && static_cast<int32_t>(now - quarantine_until_) < 0;
  }
  bool can_send(uint32_t now) const { return !pending_ && !quarantined(now); }

 private:
  uint32_t sent_ms_{0};
  uint32_t quarantine_until_{0};
  uint32_t epoch_{0};
  uint8_t command_{0};
  bool pending_{false};
  bool quarantine_active_{false};
};

}  // namespace esphome::tuya
