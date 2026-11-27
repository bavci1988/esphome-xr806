#include "bk7236_tuya_standby.h"

#if defined(USE_LIBRETINY_VARIANT_BK7236) && defined(USE_TUYA_LOW_POWER)

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <Arduino.h>
#ifdef USE_OTA
#include <Update.h>
#endif

namespace esphome::bk7236_tuya_standby {

ESPHOME_LOG_TAG(TAG, "bk7236_tuya_standby");

// A new attempt after the hub or the MCU line was not ready, like the quiet time of the XR806 standby
static constexpr uint32_t RETRY_MS = 250;

void BK7236TuyaStandby::setup() {
  // The CPU sleeps only while the MCU has agreed to; LT_AUTO_LIGHT_SLEEP alone would let it sleep whenever
  // the Wi-Fi link allows, and the MCU's UART frames do not wake it
  lt_light_sleep_allow(false);
  // The MCU may still sleep after a module restart and answer CMD01 only after a pulse on its wake line,
  // which the product info names: use the line learned before
  this->wake_pref_ = global_preferences->make_preference<WakeLinesPref>(fnv1_hash("tuya_mcu_wake_lines"));
  WakeLinesPref saved{};
  if (this->wake_pref_.load(&saved) && saved.mcu_wake >= 0) {
    this->set_mcu_wake_(saved.mcu_wake, saved.active_high != 0);
    this->saved_ = saved;
  }
}

void BK7236TuyaStandby::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  if (this->sleeping_) {
    if (this->wake_requested_) {
      this->wake_requested_ = false;
      this->standby_requested_ = false;
      this->leave_();
      ESP_LOGD(TAG, "MCU woke the module");
      this->tuya_->low_power_standby_resumed(true, true, false);
    } else if (!this->standby_requested_) {
      // The hub ended the standby itself (a command for the MCU, an update, ...)
      this->leave_();
      ESP_LOGD(TAG, "Standby ended by the Tuya hub");
    }
    return;
  }
  if (!this->standby_requested_ || static_cast<int32_t>(now - this->next_attempt_ms_) < 0)
    return;
  // Right after agreeing to sleep the MCU may still hold its wake line: wait instead of refusing
  if (this->wake_input_ >= 0 && !this->wake_in_.digital_read()) {
    if (!this->wake_held_logged_) {
      this->wake_held_logged_ = true;
      ESP_LOGD(TAG, "Standby waits: the MCU holds its wake line P%d low", this->wake_input_);
    }
    this->next_attempt_ms_ = now + RETRY_MS;
    return;
  }
  this->wake_held_logged_ = false;
  if (!this->tuya_->low_power_prepare_for_standby()) {
    // The hub may first send its own notice and agree on the next attempt
    ESP_LOGV(TAG, "Tuya hub not ready for standby yet");
    this->tuya_->low_power_standby_cancelled();
    this->next_attempt_ms_ = now + RETRY_MS;
    return;
  }
  this->enter_();
}

void BK7236TuyaStandby::enter_() {
  this->wake_requested_ = false;
  lt_light_sleep_allow(true);
  this->sleeping_ = true;
  ESP_LOGD(TAG, "Standby: the CPU may sleep, Wi-Fi stays connected");
}

void BK7236TuyaStandby::leave_() {
  lt_light_sleep_allow(false);
  this->sleeping_ = false;
}

bool BK7236TuyaStandby::ota_busy() const {
#ifdef USE_OTA
  return Update.isRunning();
#else
  return false;
#endif
}

bool BK7236TuyaStandby::standby_may_quiesce() const { return millis() >= this->boot_hold_ms_ && !this->ota_busy(); }

int BK7236TuyaStandby::map_wake_line(int gpio) {
  if (gpio < 0)
    return -1;
  if (pinInfo(gpio) != nullptr)
    return gpio;
  // WXU numbering (PA0..23 = 0..23, PB = 24 and up) for the pads the T3-U has in the same place
  if (gpio == 20)  // PA20, pad 9
    return 32;
  if (gpio == 28)  // PB4, pad 2
    return 16;
  return -1;
}

int BK7236TuyaStandby::configure_wake_lines(int wake_gpio, int mcu_wake_gpio, bool mcu_wake_active_high,
                                            bool create_mcu_wake_output) {
  const int wake = map_wake_line(wake_gpio);
  const int mcu_wake = map_wake_line(mcu_wake_gpio);
  if (create_mcu_wake_output && mcu_wake >= 0) {
    this->set_mcu_wake_(static_cast<int8_t>(mcu_wake), mcu_wake_active_high);
    ESP_LOGI(TAG, "MCU wake output from product info: P%d", mcu_wake);
    const WakeLinesPref current{static_cast<int8_t>(mcu_wake), static_cast<uint8_t>(mcu_wake_active_high)};
    if (current.mcu_wake != this->saved_.mcu_wake || current.active_high != this->saved_.active_high) {
      // Written only when the MCU names other lines (another MCU board), not on every start
      this->saved_ = current;
      this->wake_pref_.save(&current);
    }
  }
  if (wake >= 0 && this->wake_input_ < 0) {
    // The MCU pulls its line low to wake the module; the interrupt is also a light sleep wake source
    this->wake_in_.set_pin(static_cast<uint8_t>(wake));
    this->wake_in_.set_flags(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
    this->wake_in_.setup();
    this->wake_in_.attach_interrupt(&BK7236TuyaStandby::wake_isr, this, gpio::INTERRUPT_FALLING_EDGE);
    this->wake_input_ = static_cast<int8_t>(wake);
    ESP_LOGI(TAG, "Standby wake input from product info: P%d", wake);
  }
  if (this->wake_input_ < 0)
    ESP_LOGW(TAG, "MCU wake lines wk=%d mwk=%d have no known pins on this module", wake_gpio, mcu_wake_gpio);
  return this->wake_input_;
}

void BK7236TuyaStandby::set_mcu_wake_(int8_t pin, bool active_high) {
  this->mcu_wake_.set_pin(static_cast<uint8_t>(pin));
  this->mcu_wake_.set_inverted(!active_high);
  this->mcu_wake_.set_flags(gpio::FLAG_OUTPUT);
  this->mcu_wake_created_ = true;
}

void BK7236TuyaStandby::dump_config() {
  ESP_LOGCONFIG(TAG,
                "BK7236 Tuya standby:\n"
                "  Boot hold: %" PRIu32 " s\n"
                "  Wake input: %d\n"
                "  MCU wake output: %d",
                this->boot_hold_ms_ / 1000, this->wake_input_, this->mcu_wake_created_ ? this->mcu_wake_.get_pin() : -1);
}

}  // namespace esphome::bk7236_tuya_standby

#endif  // USE_LIBRETINY_VARIANT_BK7236 && USE_TUYA_LOW_POWER
