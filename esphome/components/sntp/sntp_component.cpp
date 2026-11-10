#include "sntp_component.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include "esp_sntp.h"
#elif USE_ESP8266
#include "sntp.h"
#else
#include "lwip/apps/sntp.h"
#endif
#ifdef USE_XR806
#include "lwip/tcpip.h"
#endif

namespace esphome::sntp {

ESPHOME_LOG_TAG(TAG, "sntp");

#if defined(USE_ESP32)
SNTPComponent *SNTPComponent::instance = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
#endif

void SNTPComponent::setup() {
#if defined(USE_ESP32)
  SNTPComponent::instance = this;
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  size_t i = 0;
  for (auto &server : this->servers_) {
    esp_sntp_setservername(i++, server);
  }
  esp_sntp_set_sync_interval(this->get_update_interval());
  esp_sntp_set_time_sync_notification_cb([](struct timeval *tv) {
    if (SNTPComponent::instance != nullptr) {
      SNTPComponent::instance->defer([]() { SNTPComponent::instance->time_synced(); });
    }
  });
  esp_sntp_init();
#elif defined(USE_XR806)
  this->start_lwip_in_tcpip_thread_();
#else
  this->start_lwip_();
#endif
}

#ifndef USE_ESP32
void SNTPComponent::start_lwip_() {
  sntp_stop();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);

  size_t i = 0;
  for (auto &server : this->servers_) {
    sntp_setservername(i++, server);
  }

#if defined(USE_ESP8266)
  settimeofday_cb([this](bool from_sntp) {
    if (from_sntp)
      this->time_synced();
  });
#endif

  sntp_init();
}
#endif

#ifdef USE_XR806
static constexpr uint32_t START_RETRY_ID = 0;
static constexpr uint32_t START_RETRY_INTERVAL_MS = 1000;

void SNTPComponent::start_lwip_in_tcpip_thread_() {
  // lwIP runs without core locking on XR806, so its raw API may only be used from the TCP/IP thread.
  if (tcpip_callback([](void *self) { static_cast<SNTPComponent *>(self)->start_lwip_(); }, this) == ERR_OK) {
    this->cancel_interval(START_RETRY_ID);
    this->status_clear_warning();
  } else if (!this->status_has_warning()) {
    // The TCP/IP thread's message box is full. Retry on its own, so this works with update_interval: never too.
    this->status_set_warning();
    this->set_interval(START_RETRY_ID, START_RETRY_INTERVAL_MS, [this]() { this->start_lwip_in_tcpip_thread_(); });
  }
}
#endif
void SNTPComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "SNTP Time:");
  size_t i = 0;
  for (auto &server : this->servers_) {
    ESP_LOGCONFIG(TAG, "  Server %zu: '%s'", i++, server);
  }
  RealTimeClock::dump_config();
}
void SNTPComponent::update() {
#if defined(USE_XR806)
  // start_lwip_() stops a running client first, so this also restarts after a failed setup().
  this->has_time_ = false;
  this->start_lwip_in_tcpip_thread_();
#elif !defined(USE_ESP32)
  // Some platforms currently cannot set the sync interval at runtime so we need
  // to do the re-sync by hand for now.
  if (sntp_enabled()) {
    sntp_stop();
    this->has_time_ = false;
    sntp_init();
  }
#endif
}
void SNTPComponent::loop() {
// The loop is used to infer whether we have valid time on platforms where we
// cannot tell whether SNTP has succeeded.
// One limitation of this approach is that we cannot tell if it was the SNTP
// component that set the time.
// ESP-IDF and ESP8266 use callbacks from the SNTP task to trigger the
// `on_time_sync` trigger on successful sync events.
#if defined(USE_ESP32) || defined(USE_ESP8266)
  this->disable_loop();
#endif

  if (this->has_time_)
    return;

  this->time_synced();
}

void SNTPComponent::time_synced() {
  auto time = this->now();
  this->has_time_ = time.is_valid();
  if (!this->has_time_)
    return;

  ESP_LOGD(TAG, "Synchronized time: %04d-%02d-%02d %02d:%02d:%02d", time.year, time.month, time.day_of_month, time.hour,
           time.minute, time.second);
  this->time_sync_callback_.call();
}

}  // namespace esphome::sntp
