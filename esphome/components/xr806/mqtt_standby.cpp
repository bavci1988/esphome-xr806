#include "mqtt_standby.h"

#if defined(USE_MQTT) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.mqtt_standby");

// Time for the DISCONNECT and the closing of the connection after the client was disabled.
static constexpr uint32_t DISCONNECT_WAIT_MS = 1000;
// With stay_connected: how long a standby waits for the client to connect to the broker.
static constexpr uint32_t CONNECT_WAIT_MS = 10000;

static bool keeps_connection() {
  auto *controller = standby_controller();
  return controller != nullptr && controller->stay_connected();
}

void XR806MQTTStandby::setup() {
  auto *controller = standby_controller();
  if (controller == nullptr || !controller->register_participant(this)) {
    this->mark_failed(LOG_STR("Cannot register MQTT standby participant"));
    return;
  }
  lowpower_policy().set_mqtt_idle(false);
}

void XR806MQTTStandby::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 MQTT standby: disconnects cleanly before standby and reconnects on wake");
}

void XR806MQTTStandby::on_shutdown() {
  this->shutting_down_ = true;
  lowpower_policy().set_mqtt_idle(false);
  if (auto *controller = standby_controller(); controller != nullptr) {
    controller->request_standby(false);
    controller->request_preflight(false);
  }
}

void XR806MQTTStandby::standby_preflight(bool requested) {
  if (!requested) {
    this->restore_();
    return;
  }
  auto *client = mqtt::global_mqtt_client;
  if (!this->may_quiesce() || client == nullptr) {
    lowpower_policy().set_mqtt_idle(false);  // Fail closed: no client, no idle vote
    return;
  }
  if (keeps_connection()) {
    // The connection stays open and the broker's next packet wakes the chip. A client that is still
    // connecting keeps it awake for a while; a broker that cannot be reached must not stop standby.
    if (!client->is_connected() && !client->is_disabled() && !this->connect_waiting_) {
      this->connect_waiting_ = true;
      this->connect_wait_since_ms_ = App.get_loop_component_start_time();
    }
    lowpower_policy().set_mqtt_idle(this->connection_quiet_(client));
    return;
  }
  if (!this->parked_) {
    this->reenable_ = !client->is_disabled();
    this->parked_ = true;
    this->parked_ms_ = App.get_loop_component_start_time();
    if (this->reenable_)
      client->disable();
  }
  // Re-checked on every pass: an automation that enables the client again during the preflight is new work and
  // vetoes this attempt; it is not switched off again
  lowpower_policy().set_mqtt_idle(this->quiet_(client));
}

bool XR806MQTTStandby::quiet_(const mqtt::MQTTClientComponent *client) const {
  return this->parked_ && client->is_disabled() &&
         App.get_loop_component_start_time() - this->parked_ms_ >= DISCONNECT_WAIT_MS;
}

bool XR806MQTTStandby::connection_quiet_(mqtt::MQTTClientComponent *client) const {
  return client->is_connected() || client->is_disabled() ||
         (this->connect_waiting_ &&
          App.get_loop_component_start_time() - this->connect_wait_since_ms_ >= CONNECT_WAIT_MS);
}

bool XR806MQTTStandby::prepare_for_standby() {
  auto *client = mqtt::global_mqtt_client;
  if (client == nullptr)
    return false;
  if (keeps_connection())
    return this->connection_quiet_(client);
  return this->quiet_(client);
}

void XR806MQTTStandby::restore_() {
  // Claim the blocker before the client starts network work again
  lowpower_policy().set_mqtt_idle(false);
  this->connect_waiting_ = false;
  if (!this->parked_)
    return;
  this->parked_ = false;
  if (this->reenable_ && !this->shutting_down_ && mqtt::global_mqtt_client != nullptr)
    mqtt::global_mqtt_client->enable();
}

}  // namespace esphome::xr806

#endif  // USE_MQTT && USE_XR806_STANDBY_CONTROLLER
