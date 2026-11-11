#include "mqtt_standby.h"

#if defined(USE_XR806) && defined(USE_MQTT) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/core/log.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.mqtt_standby");

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
  if (!this->parked_) {
    this->reenable_ = !client->is_disabled();
    this->parked_ = true;
    if (this->reenable_)
      client->disable();
  }
  // Re-checked on every pass: an automation that enables the client again during the preflight is new work and
  // vetoes this attempt; it is not switched off again
  lowpower_policy().set_mqtt_idle(this->quiet_(client));
}

bool XR806MQTTStandby::quiet_(const mqtt::MQTTClientComponent *client) const {
  // Idle only once the broker has closed the connection after the DISCONNECT, or the timeout ran out
  return this->parked_ && client->is_disabled() && !mqtt::MQTTBackendXR806::any_open();
}

bool XR806MQTTStandby::prepare_for_standby() {
  auto *client = mqtt::global_mqtt_client;
  return client != nullptr && this->quiet_(client);
}

void XR806MQTTStandby::restore_() {
  if (!this->parked_)
    return;
  // Claim the blocker before the client starts network work again
  lowpower_policy().set_mqtt_idle(false);
  this->parked_ = false;
  if (this->reenable_ && !this->shutting_down_ && mqtt::global_mqtt_client != nullptr)
    mqtt::global_mqtt_client->enable();
}

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_MQTT && USE_XR806_STANDBY_CONTROLLER
