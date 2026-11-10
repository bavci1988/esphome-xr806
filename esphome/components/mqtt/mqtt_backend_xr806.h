#pragma once
#include "mqtt_backend.h"

#ifdef USE_MQTT
#ifdef USE_XR806

#include <memory>
#include <string>
#include <vector>

#include "esphome/components/socket/ipv4_resolve.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/helpers.h"

namespace esphome::mqtt {

/// MQTT 3.1.1 over plain TCP on the XR806 lwIP sockets. Everything runs in the main loop: the socket is
/// non-blocking, incoming messages are handed to the callbacks in pieces, and outgoing QoS 1 and 2 messages are
/// not sent again after a lost connection, like the AsyncMqttClient backend.
class MQTTBackendXR806 final : public MQTTBackend {
 public:
  void set_keep_alive(uint16_t keep_alive) final { this->keep_alive_s_ = keep_alive; }
  void set_client_id(const char *client_id) final { this->client_id_ = client_id; }
  void set_clean_session(bool clean_session) final { this->clean_session_ = clean_session; }
  void set_credentials(const char *username, const char *password) final {
    this->username_ = username;
    this->password_ = password;
  }
  void set_will(const char *topic, uint8_t qos, bool retain, const char *payload) final {
    this->will_topic_ = topic;
    this->will_qos_ = qos;
    this->will_retain_ = retain;
    this->will_payload_ = payload;
  }
  void set_server(network::IPAddress ip, uint16_t port) final;
  void set_server(const char *host, uint16_t port) final {
    this->host_ = host;
    this->port_ = port;
  }
  void set_on_connect(std::function<on_connect_callback_t> &&callback) final {
    this->on_connect_.add(std::move(callback));
  }
  void set_on_disconnect(std::function<on_disconnect_callback_t> &&callback) final {
    this->on_disconnect_.add(std::move(callback));
  }
  void set_on_subscribe(std::function<on_subscribe_callback_t> &&callback) final {
    this->on_subscribe_.add(std::move(callback));
  }
  void set_on_unsubscribe(std::function<on_unsubscribe_callback_t> &&callback) final {
    this->on_unsubscribe_.add(std::move(callback));
  }
  void set_on_message(std::function<on_message_callback_t> &&callback) final {
    this->on_message_.add(std::move(callback));
  }
  void set_on_publish(std::function<on_publish_user_callback_t> &&callback) final {
    this->on_publish_.add(std::move(callback));
  }

  bool connected() const final { return this->state_ == State::STATE_CONNECTED; }
  void connect() final;
  void disconnect() final;
  bool subscribe(const char *topic, uint8_t qos) final;
  bool can_subscribe() const final { return this->tx_pending_.empty(); }
  bool unsubscribe(const char *topic) final;
  bool publish(const char *topic, const char *payload, size_t length, uint8_t qos, bool retain) final;
  using MQTTBackend::publish;
  bool publish_tracked(const MQTTMessage &message) final;
  bool publish_acknowledged() const final { return this->tracked_acked_; }

  void loop() final;

 protected:
  enum class State : uint8_t {
    STATE_DISCONNECTED,
    STATE_RESOLVING,
    STATE_TCP_CONNECTING,
    STATE_WAIT_CONNACK,
    STATE_CONNECTED,
  };
  /// Where the parser is inside the packet that is being received.
  enum class RxState : uint8_t {
    RX_STATE_TYPE,
    RX_STATE_LENGTH,
    RX_STATE_CONTROL,
    RX_STATE_TOPIC_LENGTH,
    RX_STATE_TOPIC,
    RX_STATE_PACKET_ID,
    RX_STATE_PAYLOAD,
  };

  void start_tcp_();
  void check_tcp_();
  /// Closes the socket; reports reason to the disconnect callbacks when a connection was open or being opened.
  void close_(MQTTClientDisconnectReason reason);
  void read_();
  /// Feeds received bytes to the parser; returns false when the connection was closed.
  bool parse_(const uint8_t *data, size_t len);
  bool end_of_header_();
  bool handle_control_();
  void deliver_payload_(const uint8_t *data, size_t len);
  void finish_publish_();
  /// Sends one packet made of the given pieces. A part the socket does not take is kept and sent from loop().
  /// While such a rest is waiting, a packet is refused unless queue_behind is set (short acknowledgements).
  bool send_(const struct iovec *iov, int count, bool queue_behind);
  void flush_();
  bool send_ack_(uint8_t type, uint16_t packet_id);
  uint16_t next_packet_id_();
  bool publish_(const char *topic, const char *payload, size_t length, uint8_t qos, bool retain, uint16_t *packet_id);

  // Connection settings; the strings belong to MQTTClientComponent and outlive the connection.
  const char *host_{nullptr};
  const char *client_id_{nullptr};
  const char *username_{nullptr};
  const char *password_{nullptr};
  const char *will_topic_{nullptr};
  const char *will_payload_{nullptr};
  std::string server_ip_;  ///< Holds the address text when set_server() gets an IPAddress
  uint16_t port_{1883};
  uint16_t keep_alive_s_{15};
  uint8_t will_qos_{0};
  bool will_retain_{false};
  bool clean_session_{true};

  std::unique_ptr<socket::Socket> socket_;
  socket::Ipv4Resolve resolve_;
  State state_{State::STATE_DISCONNECTED};
  uint32_t state_start_{0};
  uint32_t last_tx_{0};
  uint32_t ping_sent_{0};
  bool ping_outstanding_{false};
  /// Stopped reading before the socket would block, so read again without waiting for ready()
  bool rx_more_{false};

  // Receive parser
  RxState rx_state_{RxState::RX_STATE_TYPE};
  uint8_t rx_type_{0};
  uint8_t rx_length_shift_{0};
  uint8_t rx_control_len_{0};
  uint8_t rx_control_[4]{};
  uint32_t rx_remaining_{0};  ///< Bytes of the current packet still to come after the current field
  uint32_t rx_payload_total_{0};
  uint32_t rx_payload_index_{0};
  uint16_t rx_topic_length_{0};
  uint16_t rx_topic_read_{0};
  uint16_t rx_packet_id_{0};
  bool rx_topic_too_long_{false};
  std::string rx_topic_;

  /// Bytes of a packet the socket did not take yet; its capacity stays for the next one.
  std::vector<uint8_t> tx_pending_;
  size_t tx_pending_offset_{0};

  uint16_t last_packet_id_{0};
  uint16_t tracked_packet_id_{0};
  bool tracked_acked_{false};

  CallbackManager<on_connect_callback_t> on_connect_;
  CallbackManager<on_disconnect_callback_t> on_disconnect_;
  CallbackManager<on_subscribe_callback_t> on_subscribe_;
  CallbackManager<on_unsubscribe_callback_t> on_unsubscribe_;
  CallbackManager<on_message_callback_t> on_message_;
  CallbackManager<on_publish_user_callback_t> on_publish_;
};

}  // namespace esphome::mqtt

#endif  // USE_XR806
#endif  // USE_MQTT
