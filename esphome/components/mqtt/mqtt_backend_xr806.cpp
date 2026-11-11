#include "mqtt_backend_xr806.h"

#ifdef USE_MQTT
#ifdef USE_XR806

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_MQTT_TLS
extern "C" {
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
}
#endif

namespace esphome::mqtt {

ESPHOME_LOG_TAG(TAG, "mqtt.xr806");

// Control packet types (MQTT 3.1.1, section 2.2.1)
static constexpr uint8_t PACKET_CONNECT = 1;
static constexpr uint8_t PACKET_CONNACK = 2;
static constexpr uint8_t PACKET_PUBLISH = 3;
static constexpr uint8_t PACKET_PUBACK = 4;
static constexpr uint8_t PACKET_PUBREC = 5;
static constexpr uint8_t PACKET_PUBREL = 6;
static constexpr uint8_t PACKET_PUBCOMP = 7;
static constexpr uint8_t PACKET_SUBSCRIBE = 8;
static constexpr uint8_t PACKET_SUBACK = 9;
static constexpr uint8_t PACKET_UNSUBSCRIBE = 10;
static constexpr uint8_t PACKET_UNSUBACK = 11;
static constexpr uint8_t PACKET_PINGREQ = 12;
static constexpr uint8_t PACKET_PINGRESP = 13;
static constexpr uint8_t PACKET_DISCONNECT = 14;

// The TCP connect and the CONNACK must each arrive within this time.
static constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
// Messages on longer topics are dropped; the broker still gets its acknowledgement.
static constexpr uint16_t MAX_RX_TOPIC_LEN = 256;
// Bytes read per socket call, and calls per loop() before the rest waits for the next pass.
static constexpr size_t RX_CHUNK_SIZE = 256;
static constexpr uint8_t MAX_READS_PER_LOOP = 16;
// A disconnect waits this long for the queued bytes and the DISCONNECT to leave before it closes the socket.
static constexpr uint32_t CLOSE_TIMEOUT_MS = 2000;
// Room for the fixed header (type byte and up to 4 length bytes), a topic or ID length and a packet ID.
static constexpr size_t HEADER_BUFFER_SIZE = 1 + 4 + 2 + 2;

/// Writes the fixed header; returns the bytes written (at most 5).
static size_t put_fixed_header(uint8_t *buf, uint8_t type_and_flags, uint32_t remaining) {
  size_t pos = 0;
  buf[pos++] = type_and_flags;
  do {
    uint8_t byte = remaining & 0x7F;
    remaining >>= 7;
    if (remaining != 0)
      byte |= 0x80;
    buf[pos++] = byte;
  } while (remaining != 0);
  return pos;
}

static size_t string_size(const char *str) { return str == nullptr ? 0 : strlen(str); }

#ifdef USE_MQTT_TLS
// Network errors as mbedTLS's net module reports them; that module is not used here
static constexpr int TLS_ERR_SEND_FAILED = -0x004E;
static constexpr int TLS_ERR_RECV_FAILED = -0x004C;

struct MQTTBackendXR806::TlsSession {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt ca;
  mbedtls_x509_crt own_cert;
  mbedtls_pk_context own_key;
  /// Length of a record that mbedTLS has encrypted but not fully sent
  size_t inflight_len{0};
  bool active{false};
  bool skip_cn{false};

  TlsSession() {
    mbedtls_ssl_init(&this->ssl);
    mbedtls_ssl_config_init(&this->conf);
    mbedtls_x509_crt_init(&this->ca);
    mbedtls_x509_crt_init(&this->own_cert);
    mbedtls_pk_init(&this->own_key);
  }
  ~TlsSession() {
    mbedtls_ssl_free(&this->ssl);
    mbedtls_ssl_config_free(&this->conf);
    mbedtls_x509_crt_free(&this->ca);
    mbedtls_x509_crt_free(&this->own_cert);
    mbedtls_pk_free(&this->own_key);
  }
  TlsSession(const TlsSession &) = delete;
  TlsSession &operator=(const TlsSession &) = delete;
};

static int tls_send(void *ctx, const unsigned char *buf, size_t len) {
  const ssize_t ret = static_cast<socket::Socket *>(ctx)->write(buf, len);
  if (ret >= 0)
    return static_cast<int>(ret);
  return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : TLS_ERR_SEND_FAILED;
}

static int tls_recv(void *ctx, unsigned char *buf, size_t len) {
  const ssize_t ret = static_cast<socket::Socket *>(ctx)->read(buf, len);
  if (ret >= 0)
    return static_cast<int>(ret);  // 0 is the end of the stream
  return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : TLS_ERR_RECV_FAILED;
}

static int tls_random(void * /*ctx*/, unsigned char *out, size_t len) {
  return random_bytes(out, len) ? 0 : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

// Accepts a certificate whose name does not match the broker, for skip_cert_cn_check
static int tls_ignore_cn(void * /*ctx*/, mbedtls_x509_crt * /*crt*/, int /*depth*/, uint32_t *flags) {
  *flags &= ~MBEDTLS_X509_BADCERT_CN_MISMATCH;
  return 0;
}

static void log_tls_error(const char *what, int ret) {
  ESP_LOGW(TAG, "%s: -0x%04x", what, static_cast<unsigned>(-ret));
}
#endif  // USE_MQTT_TLS

void MQTTBackendXR806::set_server(network::IPAddress ip, uint16_t port) {
  char buf[network::IP_ADDRESS_BUFFER_SIZE];
  this->server_ip_ = ip.str_to(buf);
  this->set_server(this->server_ip_.c_str(), port);
}

// The backend of the MQTT client; there is at most one.
static MQTTBackendXR806 *instance = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

MQTTBackendXR806::MQTTBackendXR806() { instance = this; }

MQTTBackendXR806::~MQTTBackendXR806() {
  if (instance == this)
    instance = nullptr;
}

bool MQTTBackendXR806::any_open() { return instance != nullptr && instance->socket_ != nullptr; }

void MQTTBackendXR806::connect() {
  this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
  if (this->host_ == nullptr)
    return;
  // The work runs in loop(), so a failure is reported after the client has noted the attempt
  this->resolve_.forget();
  this->resolve_.start(this->host_, this->port_, TAG);
  this->state_ = State::STATE_RESOLVING;
}

void MQTTBackendXR806::disconnect() {
  if (this->state_ != State::STATE_CONNECTED) {
    this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    return;
  }
  uint8_t packet[2] = {PACKET_DISCONNECT << 4, 0};
  struct iovec iov = {packet, sizeof(packet)};
  if (!this->send_(&iov, 1, true) || this->socket_ == nullptr)
    return;  // The write failed and the connection is already closed and reported
  if (this->tx_pending_.empty()) {
    this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    return;
  }
  // Report the disconnect now and close once the rest has left
  this->state_ = State::STATE_CLOSING;
  this->state_start_ = App.get_loop_component_start_time();
  this->on_disconnect_.call(MQTTClientDisconnectReason::TCP_DISCONNECTED);
}

void MQTTBackendXR806::loop_closing_() {
  this->flush_();
  if (this->socket_ == nullptr)
    return;
  if (this->tx_pending_.empty() || App.get_loop_component_start_time() - this->state_start_ >= CLOSE_TIMEOUT_MS) {
    this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
  }
}

void MQTTBackendXR806::close_(MQTTClientDisconnectReason reason) {
  // A closing connection was reported when it began to close
  const bool was_open = this->state_ != State::STATE_DISCONNECTED && this->state_ != State::STATE_CLOSING;
#ifdef USE_MQTT_TLS
  if (this->tls_ != nullptr && this->tls_->active) {
    if (this->socket_ != nullptr && this->state_ != State::STATE_TLS_HANDSHAKE)
      mbedtls_ssl_close_notify(&this->tls_->ssl);  // Best effort; the socket is closed right after
    this->tls_->active = false;
    this->tls_->inflight_len = 0;
  }
#endif
  if (this->socket_ != nullptr) {
    this->socket_->close();
    this->socket_.reset();
  }
  this->state_ = State::STATE_DISCONNECTED;
  this->rx_state_ = RxState::RX_STATE_TYPE;
  this->rx_more_ = false;
  this->ping_outstanding_ = false;
  this->tx_pending_.clear();
  this->tx_pending_offset_ = 0;
  if (was_open)
    this->on_disconnect_.call(reason);
}

void MQTTBackendXR806::start_tcp_() {
  struct sockaddr_storage dest;
  const socklen_t dest_len =
      this->resolve_.to_sockaddr(reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest), this->port_);
  if (dest_len == 0) {
    this->close_(MQTTClientDisconnectReason::DNS_RESOLVE_ERROR);
    return;
  }
  this->socket_ = socket::socket_loop_monitored(dest.ss_family, SOCK_STREAM, IPPROTO_TCP);
  if (this->socket_ == nullptr) {
    ESP_LOGW(TAG, "Could not create socket: errno %d", errno);
    this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    return;
  }
  this->socket_->setblocking(false);
  int enable = 1;
  this->socket_->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
  if (this->socket_->connect(reinterpret_cast<struct sockaddr *>(&dest), dest_len) != 0 && errno != EINPROGRESS) {
    ESP_LOGW(TAG, "Connect failed: errno %d", errno);
    this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    return;
  }
  this->state_ = State::STATE_TCP_CONNECTING;
  this->state_start_ = App.get_loop_component_start_time();
}

void MQTTBackendXR806::check_tcp_() {
  int err = 0;
  switch (socket::poll_connect(*this->socket_, err)) {
    case socket::ConnectPollResult::CONNECT_POLL_RESULT_PENDING:
      return;
    case socket::ConnectPollResult::CONNECT_POLL_RESULT_ERROR:
      ESP_LOGW(TAG, "Connect failed: errno %d", err);
      this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
      return;
    default:
      break;
  }
#ifdef USE_MQTT_TLS
  if (this->tls_begin_())
    return;  // CONNECT follows the handshake
  if (this->socket_ == nullptr)
    return;  // TLS could not be set up; already closed and reported
#endif
  this->send_connect_();
}

void MQTTBackendXR806::send_connect_() {
  // CONNECT (section 3.1)
  const size_t client_id_len = string_size(this->client_id_);
  const bool has_will = this->will_topic_ != nullptr && this->will_topic_[0] != '\0';
  const size_t will_topic_len = has_will ? strlen(this->will_topic_) : 0;
  const size_t will_payload_len = has_will ? string_size(this->will_payload_) : 0;
  const bool has_username = this->username_ != nullptr;
  const bool has_password = has_username && this->password_ != nullptr;
  const size_t username_len = has_username ? strlen(this->username_) : 0;
  const size_t password_len = has_password ? strlen(this->password_) : 0;

  uint8_t flags = 0;
  if (this->clean_session_)
    flags |= 0x02;
  if (has_will)
    flags |= 0x04 | (std::min<uint8_t>(this->will_qos_, 2) << 3) | (this->will_retain_ ? 0x20 : 0);
  if (has_password)
    flags |= 0x40;
  if (has_username)
    flags |= 0x80;

  const uint32_t remaining = 10 + 2 + client_id_len + (has_will ? 4 + will_topic_len + will_payload_len : 0) +
                             (has_username ? 2 + username_len : 0) + (has_password ? 2 + password_len : 0);
  uint8_t header[5 + 10 + 2];
  size_t pos = put_fixed_header(header, PACKET_CONNECT << 4, remaining);
  static constexpr uint8_t PROTOCOL[] = {0, 4, 'M', 'Q', 'T', 'T', 4};
  memcpy(header + pos, PROTOCOL, sizeof(PROTOCOL));
  pos += sizeof(PROTOCOL);
  header[pos++] = flags;
  pos = put_be16(header + pos, this->keep_alive_s_) - header;
  pos = put_be16(header + pos, client_id_len) - header;

  uint8_t lengths[4][2];
  struct iovec iov[9];
  int count = 0;
  iov[count++] = {header, pos};
  iov[count++] = {const_cast<char *>(this->client_id_), client_id_len};
  auto add_string = [&](uint8_t *len_buf, const char *str, size_t len) {
    put_be16(len_buf, len);
    iov[count++] = {len_buf, 2};
    iov[count++] = {const_cast<char *>(str), len};
  };
  if (has_will) {
    add_string(lengths[0], this->will_topic_, will_topic_len);
    add_string(lengths[1], this->will_payload_, will_payload_len);
  }
  if (has_username)
    add_string(lengths[2], this->username_, username_len);
  if (has_password)
    add_string(lengths[3], this->password_, password_len);

  this->state_ = State::STATE_WAIT_CONNACK;
  this->state_start_ = App.get_loop_component_start_time();
  if (!this->send_(iov, count, false))
    this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
}

void MQTTBackendXR806::loop() {
  switch (this->state_) {
    case State::STATE_DISCONNECTED:
      return;
    case State::STATE_CLOSING:
      this->loop_closing_();
      return;
    case State::STATE_RESOLVING:
      if (this->resolve_.consume_failure()) {
        this->close_(MQTTClientDisconnectReason::DNS_RESOLVE_ERROR);
      } else if (this->resolve_.ready()) {
        this->start_tcp_();
      }
      // The client gives up on a lookup that takes too long
      return;
    case State::STATE_TCP_CONNECTING:
      if (App.get_loop_component_start_time() - this->state_start_ >= CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Connect timed out");
        this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
        return;
      }
      this->check_tcp_();
      return;
#ifdef USE_MQTT_TLS
    case State::STATE_TLS_HANDSHAKE:
      if (App.get_loop_component_start_time() - this->state_start_ >= CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "TLS handshake timed out");
        this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
        return;
      }
      this->tls_handshake_();
      return;
#endif
    case State::STATE_WAIT_CONNACK:
      if (App.get_loop_component_start_time() - this->state_start_ >= CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "No CONNACK from broker");
        this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
        return;
      }
      break;
    case State::STATE_CONNECTED:
      break;
  }

  this->flush_();
  if (this->socket_ == nullptr)
    return;
  this->read_();
  if (this->state_ != State::STATE_CONNECTED || this->keep_alive_s_ == 0)
    return;

  const uint32_t now = App.get_loop_component_start_time();
  const uint32_t keep_alive_ms = this->keep_alive_s_ * 1000U;
  if (this->ping_outstanding_) {
    if (now - this->ping_sent_ >= keep_alive_ms) {
      ESP_LOGW(TAG, "No answer to PINGREQ");
      this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    }
  } else if (now - this->last_tx_ >= keep_alive_ms / 2) {
    uint8_t packet[2] = {PACKET_PINGREQ << 4, 0};
    struct iovec iov = {packet, sizeof(packet)};
    if (this->send_(&iov, 1, false)) {
      this->ping_outstanding_ = true;
      this->ping_sent_ = now;
    }
  }
}

void MQTTBackendXR806::read_() {
  bool buffered = false;
#ifdef USE_MQTT_TLS
  // Decrypted bytes can wait in the TLS session while the socket has nothing new
  buffered = this->tls_ != nullptr && this->tls_->active && mbedtls_ssl_get_bytes_avail(&this->tls_->ssl) != 0;
#endif
  if (!this->rx_more_ && !buffered && !this->socket_->ready())
    return;
  this->rx_more_ = false;
  uint8_t buf[RX_CHUNK_SIZE];
  for (uint8_t i = 0; i < MAX_READS_PER_LOOP; i++) {
    const ssize_t len = this->transport_read_(buf, sizeof(buf));
    if (len == 0) {
      this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
      return;
    }
    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      ESP_LOGW(TAG, "Read failed: errno %d", errno);
      this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
      return;
    }
    if (!this->parse_(buf, len))
      return;
  }
  // The socket was not drained, so ready() cannot be relied on for the rest
  this->rx_more_ = true;
}

bool MQTTBackendXR806::parse_(const uint8_t *data, size_t len) {
  size_t pos = 0;
  while (pos < len) {
    const uint8_t byte = data[pos];
    switch (this->rx_state_) {
      case RxState::RX_STATE_TYPE:
        this->rx_type_ = byte;
        this->rx_remaining_ = 0;
        this->rx_length_shift_ = 0;
        this->rx_state_ = RxState::RX_STATE_LENGTH;
        pos++;
        break;
      case RxState::RX_STATE_LENGTH:
        this->rx_remaining_ |= static_cast<uint32_t>(byte & 0x7F) << this->rx_length_shift_;
        pos++;
        if ((byte & 0x80) == 0) {
          if (!this->end_of_header_())
            return false;
        } else {
          this->rx_length_shift_ += 7;
          if (this->rx_length_shift_ > 21) {
            ESP_LOGW(TAG, "Malformed packet from broker");
            this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
            return false;
          }
        }
        break;
      case RxState::RX_STATE_CONTROL:
        if (this->rx_control_len_ < sizeof(this->rx_control_))
          this->rx_control_[this->rx_control_len_++] = byte;
        pos++;
        if (--this->rx_remaining_ == 0) {
          this->rx_state_ = RxState::RX_STATE_TYPE;
          if (!this->handle_control_())
            return false;
        }
        break;
      case RxState::RX_STATE_TOPIC_LENGTH:
        this->rx_control_[this->rx_control_len_++] = byte;
        this->rx_remaining_--;
        pos++;
        if (this->rx_control_len_ == 2) {
          this->rx_topic_length_ = encode_uint16(this->rx_control_[0], this->rx_control_[1]);
          if (this->rx_topic_length_ == 0 || this->rx_topic_length_ > this->rx_remaining_) {
            ESP_LOGW(TAG, "Malformed packet from broker");
            this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
            return false;
          }
          this->rx_topic_.clear();
          this->rx_topic_read_ = 0;
          this->rx_topic_too_long_ = this->rx_topic_length_ > MAX_RX_TOPIC_LEN;
          this->rx_state_ = RxState::RX_STATE_TOPIC;
        }
        break;
      case RxState::RX_STATE_TOPIC: {
        const size_t take = std::min<size_t>(this->rx_topic_length_ - this->rx_topic_read_, len - pos);
        if (!this->rx_topic_too_long_) {
          this->rx_topic_.append(reinterpret_cast<const char *>(data + pos), take);
        }
        this->rx_topic_read_ += take;
        pos += take;
        this->rx_remaining_ -= take;
        if (this->rx_topic_read_ == this->rx_topic_length_) {
          const uint8_t qos = (this->rx_type_ >> 1) & 0x03;
          if (qos == 3 || (qos != 0 && this->rx_remaining_ < 2)) {
            ESP_LOGW(TAG, "Malformed packet from broker");
            this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
            return false;
          }
          this->rx_control_len_ = 0;
          this->rx_packet_id_ = 0;
          this->rx_payload_total_ = this->rx_remaining_ - (qos != 0 ? 2 : 0);
          this->rx_payload_index_ = 0;
          if (qos != 0) {
            this->rx_state_ = RxState::RX_STATE_PACKET_ID;
          } else if (this->rx_payload_total_ == 0) {
            this->deliver_payload_(nullptr, 0);
            this->finish_publish_();
            if (this->socket_ == nullptr)
              return false;
          } else {
            this->rx_state_ = RxState::RX_STATE_PAYLOAD;
          }
        }
        break;
      }
      case RxState::RX_STATE_PACKET_ID:
        this->rx_control_[this->rx_control_len_++] = byte;
        this->rx_remaining_--;
        pos++;
        if (this->rx_control_len_ == 2) {
          this->rx_packet_id_ = encode_uint16(this->rx_control_[0], this->rx_control_[1]);
          if (this->rx_payload_total_ == 0) {
            this->deliver_payload_(nullptr, 0);
            this->finish_publish_();
            if (this->socket_ == nullptr)
              return false;
          } else {
            this->rx_state_ = RxState::RX_STATE_PAYLOAD;
          }
        }
        break;
      case RxState::RX_STATE_PAYLOAD: {
        const size_t take = std::min<size_t>(this->rx_remaining_, len - pos);
        this->deliver_payload_(data + pos, take);
        if (this->socket_ == nullptr)
          return false;
        pos += take;
        this->rx_remaining_ -= take;
        if (this->rx_remaining_ == 0) {
          this->finish_publish_();
          if (this->socket_ == nullptr)
            return false;
        }
        break;
      }
    }
  }
  return true;
}

bool MQTTBackendXR806::end_of_header_() {
  const uint8_t type = this->rx_type_ >> 4;
  this->rx_control_len_ = 0;
  if (type == PACKET_PUBLISH) {
    if (this->state_ != State::STATE_CONNECTED || this->rx_remaining_ < 2) {
      ESP_LOGW(TAG, "Malformed packet from broker");
      this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
      return false;
    }
    this->rx_state_ = RxState::RX_STATE_TOPIC_LENGTH;
    return true;
  }
  if (this->rx_remaining_ == 0) {
    this->rx_state_ = RxState::RX_STATE_TYPE;
    return this->handle_control_();
  }
  // Every packet a client receives is short apart from PUBLISH; the rest of a longer one is skipped
  this->rx_state_ = RxState::RX_STATE_CONTROL;
  return true;
}

bool MQTTBackendXR806::handle_control_() {
  const uint8_t type = this->rx_type_ >> 4;
  const uint16_t packet_id = this->rx_control_len_ >= 2 ? encode_uint16(this->rx_control_[0], this->rx_control_[1]) : 0;
  if (this->state_ == State::STATE_WAIT_CONNACK) {
    if (type != PACKET_CONNACK || this->rx_control_len_ < 2) {
      ESP_LOGW(TAG, "Expected CONNACK from broker");
      this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
      return false;
    }
    const uint8_t return_code = this->rx_control_[1];
    if (return_code != 0) {
      // Return codes 1 to 5 match MQTTClientDisconnectReason
      this->close_(return_code <= 5 ? static_cast<MQTTClientDisconnectReason>(return_code)
                                    : MQTTClientDisconnectReason::MQTT_SERVER_UNAVAILABLE);
      return false;
    }
    this->state_ = State::STATE_CONNECTED;
    this->last_tx_ = App.get_loop_component_start_time();
    this->on_connect_.call((this->rx_control_[0] & 0x01) != 0);
    return this->socket_ != nullptr;
  }

  switch (type) {
    case PACKET_PUBACK:
    case PACKET_PUBCOMP:
      if (packet_id == this->tracked_packet_id_)
        this->tracked_acked_ = true;
      this->on_publish_.call(packet_id);
      break;
    case PACKET_PUBREC:
      this->send_ack_((PACKET_PUBREL << 4) | 0x02, packet_id);
      break;
    case PACKET_PUBREL:
      this->send_ack_(PACKET_PUBCOMP << 4, packet_id);
      break;
    case PACKET_SUBACK:
      if (this->rx_control_len_ >= 3)
        this->on_subscribe_.call(packet_id, this->rx_control_[2]);
      break;
    case PACKET_UNSUBACK:
      this->on_unsubscribe_.call(packet_id);
      break;
    case PACKET_PINGRESP:
      this->ping_outstanding_ = false;
      break;
    default:
      ESP_LOGV(TAG, "Ignored packet type %u", type);
      break;
  }
  return this->socket_ != nullptr;
}

void MQTTBackendXR806::deliver_payload_(const uint8_t *data, size_t len) {
  if (!this->rx_topic_too_long_) {
    this->on_message_.call(this->rx_topic_.c_str(), reinterpret_cast<const char *>(data), len, this->rx_payload_index_,
                           this->rx_payload_total_);
  } else if (this->rx_payload_index_ == 0) {
    ESP_LOGW(TAG, "Dropped message on a topic longer than %u bytes", static_cast<unsigned>(MAX_RX_TOPIC_LEN));
  }
  this->rx_payload_index_ += len;
}

void MQTTBackendXR806::finish_publish_() {
  this->rx_state_ = RxState::RX_STATE_TYPE;
  switch ((this->rx_type_ >> 1) & 0x03) {
    case 1:
      this->send_ack_(PACKET_PUBACK << 4, this->rx_packet_id_);
      break;
    case 2:
      this->send_ack_(PACKET_PUBREC << 4, this->rx_packet_id_);
      break;
    default:
      break;
  }
}

bool MQTTBackendXR806::send_ack_(uint8_t type_and_flags, uint16_t packet_id) {
  uint8_t packet[4] = {type_and_flags, 2};
  put_be16(packet + 2, packet_id);
  struct iovec iov = {packet, sizeof(packet)};
  return this->send_(&iov, 1, true);
}

bool MQTTBackendXR806::send_(const struct iovec *iov, int count, bool queue_behind) {
  if (this->socket_ == nullptr)
    return false;
  size_t total = 0;
  for (int i = 0; i < count; i++)
    total += iov[i].iov_len;

  size_t sent = 0;
#ifdef USE_MQTT_TLS
  if (this->tls_ != nullptr && this->tls_->active) {
    // A TLS record is built from one buffer, so the packet is always queued and then flushed
    if (!this->tx_pending_.empty() && !queue_behind)
      return false;
    for (int i = 0; i < count; i++) {
      const auto *base = static_cast<const uint8_t *>(iov[i].iov_base);
      this->tx_pending_.insert(this->tx_pending_.end(), base, base + iov[i].iov_len);
    }
    this->last_tx_ = App.get_loop_component_start_time();
    this->flush_();
    return this->socket_ != nullptr;
  }
#endif
  if (this->tx_pending_.empty()) {
    const ssize_t written = this->socket_->writev(iov, count);
    if (written < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "Write failed: errno %d", errno);
        this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
        return false;
      }
    } else {
      sent = written;
    }
  } else if (!queue_behind) {
    return false;
  }
  this->last_tx_ = App.get_loop_component_start_time();
  if (sent == total)
    return true;

  // Keep the rest so the stream stays whole
  for (int i = 0; i < count; i++) {
    const auto *base = static_cast<const uint8_t *>(iov[i].iov_base);
    const size_t skip = std::min(sent, iov[i].iov_len);
    sent -= skip;
    this->tx_pending_.insert(this->tx_pending_.end(), base + skip, base + iov[i].iov_len);
  }
  return true;
}

void MQTTBackendXR806::flush_() {
  if (this->tx_pending_.empty())
    return;
  const ssize_t written = this->transport_write_(this->tx_pending_.data() + this->tx_pending_offset_,
                                                 this->tx_pending_.size() - this->tx_pending_offset_);
  if (written < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ESP_LOGW(TAG, "Write failed: errno %d", errno);
      this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    }
    return;
  }
  this->tx_pending_offset_ += written;
  if (this->tx_pending_offset_ == this->tx_pending_.size()) {
    this->tx_pending_.clear();
    this->tx_pending_offset_ = 0;
  }
}

ssize_t MQTTBackendXR806::transport_read_(uint8_t *buf, size_t len) {
#ifdef USE_MQTT_TLS
  if (this->tls_ != nullptr && this->tls_->active) {
    const int ret = mbedtls_ssl_read(&this->tls_->ssl, buf, len);
    if (ret > 0)
      return ret;
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
      return 0;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      errno = EAGAIN;
      return -1;
    }
    ESP_LOGW(TAG, "TLS read failed: -0x%04x", static_cast<unsigned>(-ret));
    errno = EIO;
    return -1;
  }
#endif
  return this->socket_->read(buf, len);
}

ssize_t MQTTBackendXR806::transport_write_(const uint8_t *buf, size_t len) {
#ifdef USE_MQTT_TLS
  if (this->tls_ != nullptr && this->tls_->active) {
    auto &tls = *this->tls_;
    // After WANT_WRITE the record is already encrypted; mbedTLS must be called with the same length again
    if (tls.inflight_len != 0)
      len = tls.inflight_len;
    const int ret = mbedtls_ssl_write(&tls.ssl, buf, len);
    if (ret >= 0) {
      tls.inflight_len = 0;
      return ret;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) {
      tls.inflight_len = len;
      errno = EAGAIN;
      return -1;
    }
    ESP_LOGW(TAG, "TLS write failed: -0x%04x", static_cast<unsigned>(-ret));
    errno = EIO;
    return -1;
  }
#endif
  return this->socket_->write(buf, len);
}

#ifdef USE_MQTT_TLS
bool MQTTBackendXR806::tls_begin_() {
  if (this->tls_ == nullptr) {
    // Parsed once; later connections only reset the session and keep its buffers
    auto tls = make_unique<TlsSession>();
    int ret = mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret == 0 && this->ca_certificate_ != nullptr) {
      ret = mbedtls_x509_crt_parse(&tls->ca, reinterpret_cast<const unsigned char *>(this->ca_certificate_),
                                   strlen(this->ca_certificate_) + 1);
      if (ret != 0)
        log_tls_error("Cannot parse certificate_authority", ret);
    }
    if (ret == 0 && this->cl_certificate_ != nullptr) {
      ret = mbedtls_x509_crt_parse(&tls->own_cert, reinterpret_cast<const unsigned char *>(this->cl_certificate_),
                                   strlen(this->cl_certificate_) + 1);
      if (ret == 0) {
        ret = mbedtls_pk_parse_key(&tls->own_key, reinterpret_cast<const unsigned char *>(this->cl_key_),
                                   strlen(this->cl_key_) + 1, nullptr, 0);
      }
      if (ret == 0)
        ret = mbedtls_ssl_conf_own_cert(&tls->conf, &tls->own_cert, &tls->own_key);
      if (ret != 0)
        log_tls_error("Cannot load the client certificate", ret);
    }
    if (ret == 0) {
      mbedtls_ssl_conf_rng(&tls->conf, tls_random, nullptr);
      if (this->ca_certificate_ != nullptr) {
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&tls->conf, &tls->ca, nullptr);
        if (this->skip_cert_cn_check_)
          mbedtls_ssl_conf_verify(&tls->conf, tls_ignore_cn, nullptr);
      } else {
        // verify_ssl: false; the configuration requires a CA otherwise
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
      }
      ret = mbedtls_ssl_setup(&tls->ssl, &tls->conf);
      if (ret != 0)
        log_tls_error("TLS setup failed", ret);
    }
    if (ret != 0) {
      this->close_(MQTTClientDisconnectReason::TLS_BAD_FINGERPRINT);
      return false;
    }
    this->tls_ = std::move(tls);
  } else if (int ret = mbedtls_ssl_session_reset(&this->tls_->ssl); ret != 0) {
    log_tls_error("TLS reset failed", ret);
    this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    return false;
  }
  // Server name for SNI and for the certificate name check
  if (int ret = mbedtls_ssl_set_hostname(&this->tls_->ssl, this->host_); ret != 0) {
    log_tls_error("TLS host name rejected", ret);
    this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    return false;
  }
  mbedtls_ssl_set_bio(&this->tls_->ssl, this->socket_.get(), tls_send, tls_recv, nullptr);
  this->tls_->active = true;
  this->tls_->inflight_len = 0;
  this->state_ = State::STATE_TLS_HANDSHAKE;
  this->state_start_ = App.get_loop_component_start_time();
  this->tls_handshake_();
  return true;
}

void MQTTBackendXR806::tls_handshake_() {
  const int ret = mbedtls_ssl_handshake(&this->tls_->ssl);
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    return;
  if (ret != 0) {
    const uint32_t flags = mbedtls_ssl_get_verify_result(&this->tls_->ssl);
    if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
      ESP_LOGW(TAG, "Broker certificate rejected (flags 0x%08" PRIx32 ")", flags);
      this->close_(MQTTClientDisconnectReason::TLS_BAD_FINGERPRINT);
    } else {
      log_tls_error("TLS handshake failed", ret);
      this->close_(MQTTClientDisconnectReason::TCP_DISCONNECTED);
    }
    return;
  }
  ESP_LOGD(TAG, "TLS session up: %s", mbedtls_ssl_get_ciphersuite(&this->tls_->ssl));
  this->send_connect_();
}
#endif  // USE_MQTT_TLS

uint16_t MQTTBackendXR806::next_packet_id_() {
  if (++this->last_packet_id_ == 0)
    this->last_packet_id_ = 1;
  return this->last_packet_id_;
}

bool MQTTBackendXR806::subscribe(const char *topic, uint8_t qos) {
  if (!this->connected())
    return false;
  const size_t topic_len = strlen(topic);
  uint8_t header[HEADER_BUFFER_SIZE];
  size_t pos = put_fixed_header(header, (PACKET_SUBSCRIBE << 4) | 0x02, 2 + 2 + topic_len + 1);
  pos = put_be16(header + pos, this->next_packet_id_()) - header;
  pos = put_be16(header + pos, topic_len) - header;
  uint8_t options = std::min<uint8_t>(qos, 2);
  struct iovec iov[3] = {{header, pos}, {const_cast<char *>(topic), topic_len}, {&options, 1}};
  return this->send_(iov, 3, false);
}

bool MQTTBackendXR806::unsubscribe(const char *topic) {
  if (!this->connected())
    return false;
  const size_t topic_len = strlen(topic);
  uint8_t header[HEADER_BUFFER_SIZE];
  size_t pos = put_fixed_header(header, (PACKET_UNSUBSCRIBE << 4) | 0x02, 2 + 2 + topic_len);
  pos = put_be16(header + pos, this->next_packet_id_()) - header;
  pos = put_be16(header + pos, topic_len) - header;
  struct iovec iov[2] = {{header, pos}, {const_cast<char *>(topic), topic_len}};
  return this->send_(iov, 2, false);
}

bool MQTTBackendXR806::publish(const char *topic, const char *payload, size_t length, uint8_t qos, bool retain) {
  return this->publish_(topic, payload, length, qos, retain, nullptr);
}

bool MQTTBackendXR806::publish_(const char *topic, const char *payload, size_t length, uint8_t qos, bool retain,
                                uint16_t *packet_id) {
  if (!this->connected())
    return false;
  qos = std::min<uint8_t>(qos, 2);
  const size_t topic_len = strlen(topic);
  uint8_t header[HEADER_BUFFER_SIZE];
  size_t pos = put_fixed_header(header, (PACKET_PUBLISH << 4) | (qos << 1) | (retain ? 1 : 0),
                                2 + topic_len + (qos != 0 ? 2 : 0) + length);
  pos = put_be16(header + pos, topic_len) - header;
  uint8_t id_buf[2];
  uint16_t id = 0;
  if (qos != 0) {
    id = this->next_packet_id_();
    put_be16(id_buf, id);
  }
  struct iovec iov[4] = {{header, pos},
                         {const_cast<char *>(topic), topic_len},
                         {id_buf, qos != 0 ? sizeof(id_buf) : 0},
                         {const_cast<char *>(payload), length}};
  if (!this->send_(iov, 4, false))
    return false;
  if (packet_id != nullptr)
    *packet_id = id;
  return true;
}

bool MQTTBackendXR806::publish_tracked(const MQTTMessage &message) {
  this->tracked_acked_ = false;
  uint16_t packet_id = 0;
  if (!this->publish_(message.topic.c_str(), message.payload.c_str(), message.payload.length(),
                      std::max<uint8_t>(message.qos, 1), message.retain, &packet_id))
    return false;
  this->tracked_packet_id_ = packet_id;
  return true;
}

}  // namespace esphome::mqtt

#endif  // USE_XR806
#endif  // USE_MQTT
