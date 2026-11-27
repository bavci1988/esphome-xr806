#pragma once

// Protocol-controlled Tuya MCU autobaud, exercised independently of ESPHome.
// Each candidate sends CMD01 and accepts only a checksummed product-info reply.
// Any other valid frame keeps the current rate and asks for the product info again once the MCU pauses.
#include <cstddef>
#include <cstdint>

namespace esphome::tuya {

class TuyaLowPowerAutoBaud {
 public:
  static constexpr uint32_t PROBE_TIMEOUT_MS = 500;
  // A silent MCU (asleep or absent) is probed less often after the first rounds.
  static constexpr uint32_t SLOW_PROBE_TIMEOUT_MS = 2000;
  static constexpr uint32_t FAST_PROBE_ATTEMPTS = 20;
  // Quiet time after a frame before the product query is sent again at the same rate.
  static constexpr uint32_t MIN_PROBE_GAP_MS = 100;
  static constexpr uint32_t BAUD_SLOW = 9600;
  static constexpr uint32_t BAUD_FAST = 115200;

  void start(uint32_t configured_baud) {
    this->first_baud_ = configured_baud == BAUD_FAST ? BAUD_FAST : BAUD_SLOW;
    this->candidate_ = this->first_baud_;
    this->attempts_ = 0;
    this->silent_probes_ = 0;
    this->searching_ = true;
    this->waiting_ = false;
    this->locked_ = false;
    this->sent_at_ms_ = 0;
  }

  bool searching() const { return this->searching_; }
  bool locked() const { return this->locked_; }
  bool waiting() const { return this->waiting_; }
  uint32_t baud() const { return this->candidate_; }
  uint32_t attempts() const { return this->attempts_; }

  // Poll from the main loop. A timeout advances to the other supported speed.
  // No sleep, busy-wait or task blocking is necessary.
  bool probe_due(uint32_t now) {
    if (!this->searching_)
      return false;
    if (!this->waiting_)
      return true;
    const uint32_t since_probe = now - this->sent_at_ms_;
    if (this->rate_proven_) {
      // Ask again at this rate once the MCU pauses, or after a full window of steady traffic.
      if (static_cast<uint32_t>(now - this->heard_at_ms_) < MIN_PROBE_GAP_MS && since_probe < PROBE_TIMEOUT_MS)
        return false;
      this->waiting_ = false;
      return true;
    }
    if (since_probe < this->timeout_())
      return false;
    this->waiting_ = false;
    this->candidate_ = this->candidate_ == BAUD_SLOW ? BAUD_FAST : BAUD_SLOW;
    return true;
  }

  void probe_sent(uint32_t now) {
    this->waiting_ = true;
    this->rate_proven_ = false;
    this->sent_at_ms_ = now;
    ++this->attempts_;
    ++this->silent_probes_;
  }

  // Call only for fully received frames; this independent check also makes
  // it possible to test rejection of bad checksums and wrong commands.
  bool accept_frame(const uint8_t *frame, size_t frame_len, uint32_t now) {
    if (!this->searching_ || !this->waiting_ || frame == nullptr ||
        (!this->rate_proven_ && static_cast<uint32_t>(now - this->sent_at_ms_) >= this->timeout_()))
      return false;
    // Version 0 (HC32 firmware 1.3.1) or 3 (standard Tuya MCU replies).
    if (frame_len < 9 || frame[0] != 0x55 || frame[1] != 0xAA || (frame[2] != 0x00 && frame[2] != 0x03) ||
        frame[3] != 0x01)
      return false;
    const size_t payload_len = (static_cast<size_t>(frame[4]) << 8) | frame[5];
    if (payload_len > 512 || frame_len != payload_len + 7)
      return false;
    // Product information is JSON. Do not lock to an empty/stray ACK.
    if (payload_len < 2 || frame[6] != '{' || frame[6 + payload_len - 1] != '}')
      return false;
    uint8_t checksum = 0;
    for (size_t i = 0; i + 1 < frame_len; ++i)
      checksum += frame[i];
    if (checksum != frame[frame_len - 1])
      return false;
    this->searching_ = false;
    this->waiting_ = false;
    this->locked_ = true;
    return true;
  }

  // Call for a valid frame that accept_frame() rejected. It proves the current rate and shows that
  // the MCU is awake: the rate is kept and the product query goes out again once the MCU pauses.
  // A product reply that follows in the same read is still accepted.
  void probe_again(uint32_t now) {
    if (!this->searching_ || !this->waiting_)
      return;
    this->rate_proven_ = true;
    this->heard_at_ms_ = now;
    // An awake MCU: probe at the fast pace again. attempts_ keeps counting, so a scan that never locks is still
    // reported.
    this->silent_probes_ = 0;
  }

  void stop() {
    this->searching_ = false;
    this->waiting_ = false;
  }

 private:
  uint32_t timeout_() const {
    return this->silent_probes_ <= FAST_PROBE_ATTEMPTS ? PROBE_TIMEOUT_MS : SLOW_PROBE_TIMEOUT_MS;
  }

  uint32_t first_baud_{BAUD_SLOW};
  uint32_t candidate_{BAUD_SLOW};
  uint32_t sent_at_ms_{0};
  uint32_t attempts_{0};
  uint32_t silent_probes_{0};  // Probes since the MCU was last heard; picks the fast or slow pace
  uint32_t heard_at_ms_{0};    // Last valid frame that was not the product info
  bool searching_{false};
  // A valid frame arrived at the current rate since the last probe.
  bool rate_proven_{false};
  bool waiting_{false};
  bool locked_{false};
};

}  // namespace esphome::tuya
