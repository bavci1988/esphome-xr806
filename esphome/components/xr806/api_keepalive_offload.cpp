#include "api_keepalive_offload.h"

#if defined(USE_XR806) && defined(USE_API) && defined(USE_XR806_STANDBY_CONTROLLER) && \
    defined(USE_XR806_API_KEEPALIVE_OFFLOAD)

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "esphome/components/api/api_server.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "api_offload_lwip.h"
#include "wlan_fw_swap.h"
#include "wlan_mib.h"

extern "C" {
// The WLAN declarations depend on CONFIG_WLAN from the project configuration.
#include "xr_config.h"
#include "net/wlan/wlan.h"
#include "net/wlan/wlan_ext_req.h"
#include "lwip/sockets.h"
#include "kernel/os/os_thread.h"
#include "driver/chip/hal_wakeup.h"
}

namespace {

// The patched firmware takes the frames from six areas of 128 bytes: the payloads of the three keepalive entries,
// then those of the three wake pattern entries. These stay disabled, so the firmware never reads them otherwise; the
// host writes them through the WLAN RAM window (WLAN 0x0900BE04 + e * 0x84).
constexpr size_t ENTRY_COUNT = IPC_P2P_SERVER_MAX;
constexpr size_t ENTRY_PAYLOAD = IPC_P2P_KPALIVE_PAYLOAD_LEN_MAX;
constexpr size_t AREA_COUNT = 2 * ENTRY_COUNT;
constexpr uint32_t WAKE_PAYLOAD_ADDRESS = 0x0900BE04;
constexpr uintptr_t WAKE_PAYLOAD_APP_ADDRESS = 0x6800C000 + (WAKE_PAYLOAD_ADDRESS - 0x09000000);
constexpr size_t WAKE_ENTRY_SIZE = 0x84;

// The build links with --wrap=lwip_write --wrap=lwip_writev when the experiment is enabled. While a capture is open
// on the main loop, what the API writes is kept here instead of going out, so the API's own Noise code encrypts the
// frames and the API component needs no change.
struct FrameCapture {
  OS_ThreadHandle_t task{nullptr};
  int fd{-1};
  uint16_t len{0};
  bool overflow{false};
  uint8_t data[AREA_COUNT * ENTRY_PAYLOAD];
};
FrameCapture frame_capture;
// millis() of the last write that went out through lwIP, on any socket.
volatile uint32_t last_write_ms = 0;

bool capturing_here() { return frame_capture.task != nullptr && frame_capture.task == OS_ThreadGetCurrentHandle(); }

// Keeps the bytes of the first socket written to during the capture; reports them all as sent.
ssize_t capture_write(int fd, const void *data, size_t size) {
  if (frame_capture.fd < 0)
    frame_capture.fd = fd;
  if (fd != frame_capture.fd || frame_capture.len + size > sizeof(frame_capture.data)) {
    frame_capture.overflow = true;
  } else {
    memcpy(frame_capture.data + frame_capture.len, data, size);
    frame_capture.len += size;
  }
  return static_cast<ssize_t>(size);
}

}  // namespace

extern "C" {
ssize_t __real_lwip_write(int s, const void *data, size_t size);
ssize_t __real_lwip_writev(int s, const struct iovec *iov, int iovcnt);

ssize_t __wrap_lwip_write(int s, const void *data, size_t size) {
  if (!capturing_here()) {
    last_write_ms = esphome::millis();
    return __real_lwip_write(s, data, size);
  }
  return capture_write(s, data, size);
}

ssize_t __wrap_lwip_writev(int s, const struct iovec *iov, int iovcnt) {
  if (!capturing_here()) {
    last_write_ms = esphome::millis();
    return __real_lwip_writev(s, iov, iovcnt);
  }
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; ++i)
    total += capture_write(s, iov[i].iov_base, iov[i].iov_len);
  return total;
}
}

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.api_offload");

static constexpr uint8_t SERVER = 0;
static constexpr uint16_t IP_ID_INIT = 1;
static constexpr uint8_t TX_TIMEOUT_S = 5;
static constexpr uint8_t TX_RETRIES = 3;
static constexpr uint16_t PROTOCOL_TCP = 0x01;
// Run-time state of server 0 in the patched firmware (WLAN core addresses): sequence, acknowledgement, IP ID, and
// a word with the retry count (byte 0) and the frame-in-flight flag (byte 1).
static constexpr uint32_t STATE_ADDRESS = 0x0900BC04;
static constexpr uint32_t STATE_WORDS = 4;
static constexpr uint32_t ACK_WAIT_MS = 500;
// Home Assistant pings when it received nothing for 20 s. The first firmware frame goes out one period after
// arming, so the time since the last write plus one period must stay below this.
static constexpr uint32_t PEER_PING_LIMIT_MS = 17000;
// The WLAN driver refused to suspend right after the host sent the held frames; wait this long after such a burst.
static constexpr uint32_t SETTLE_MS = 2000;
static constexpr uint32_t PEER_ANSWER_WAIT_MS = 300;

// The firmware commands take large structures; keep them off the main loop stack.
static union {
  wlan_ext_p2p_svr_set_t svr;
  wlan_ext_p2p_wkp_param_set_t wkp;
  wlan_ext_p2p_kpalive_param_set_t kpalive;
} offload_cmd;

static int request(wlan_ext_cmd_t cmd, uint32_t param) {
  return wlan_ext_request(wlan_netif_get(WLAN_MODE_STA), cmd, param);
}

struct FirmwareState {
  uint32_t seq;
  uint32_t ack;
  uint32_t ip_id;
  uint8_t retries;
  bool in_flight;
};

static bool read_state(FirmwareState &state) {
  uint32_t words[STATE_WORDS];
  if (wlan_read_words(STATE_ADDRESS, words, STATE_WORDS) != 0)
    return false;
  state.seq = words[0];
  state.ack = words[1];
  state.ip_id = words[2];
  state.retries = words[3] & 0xFF;
  state.in_flight = ((words[3] >> 8) & 0xFF) != 0;
  return true;
}

bool XR806APIKeepaliveOffload::capture_frames_(api::APIConnection *client, uint32_t max_frames) {
  frame_capture.fd = -1;
  frame_capture.len = 0;
  frame_capture.overflow = false;
  frame_capture.task = OS_ThreadGetCurrentHandle();
  uint8_t count = 0;
  uint16_t len = 0;
  while (true) {
    const uint16_t before = frame_capture.len;
    if (!client->send_message(api::PingResponse{}) || frame_capture.overflow)
      break;
    const uint16_t written = frame_capture.len - before;
    if (written == 0 || (len != 0 && written != len))
      break;
    len = written;
    ++count;
    // Every area holds a whole number of frames.
    if (count == (ENTRY_PAYLOAD / len) * AREA_COUNT || count >= max_frames)
      break;
  }
  frame_capture.task = nullptr;
  this->fd_ = frame_capture.fd;
  this->frame_len_ = len;
  this->frame_count_ = count;
  // A Noise frame starts with indicator 1; a plaintext connection cannot be offloaded.
  return count != 0 && frame_capture.data[0] == 0x01 && !frame_capture.overflow;
}

bool XR806APIKeepaliveOffload::configure_firmware_() {
  xr806_offload_tcp tcp{};
  const int read = xr806_offload_read_tcp(this->fd_, wlan_netif_get(WLAN_MODE_STA), &tcp);
  if (read != 0 || !tcp.established) {
    ESP_LOGW(TAG, "Not armed: TCP state %d, established %u", read, static_cast<unsigned>(tcp.established));
    return false;
  }
  this->snd_nxt_ = tcp.snd_nxt;
  this->rcv_nxt_ = tcp.rcv_nxt;

  auto &svr = offload_cmd.svr;
  memset(&svr, 0, sizeof(svr));
  memcpy(svr.SrcIPv4Addr, tcp.local_ip, sizeof(svr.SrcIPv4Addr));
  auto &server = svr.P2PServerCfgs[SERVER];
  server.Enable = 1;
  server.IPIdInit = IP_ID_INIT;
  server.TcpSeqInit = tcp.snd_nxt;
  server.TcpAckInit = tcp.rcv_nxt;
  memcpy(server.DstIPv4Addr, tcp.remote_ip, sizeof(server.DstIPv4Addr));
  server.SrcPort = tcp.local_port;
  server.DstPort = tcp.remote_port;
  memcpy(server.DstMacAddr, tcp.next_hop_mac, sizeof(server.DstMacAddr));
  server.TcpOrUdp = PROTOCOL_TCP;
  if (request(WLAN_EXT_CMD_SET_P2P_SVR, reinterpret_cast<uint32_t>(&svr)) != 0)
    return false;

  if (this->wake_on_peer_) {
    auto &wkp = offload_cmd.wkp;
    memset(&wkp, 0, sizeof(wkp));
    wkp.Enable = 1;
    memcpy(wkp.P2PIpv4FilterCfgs[0].Ipv4Filter, tcp.remote_ip, sizeof(wkp.P2PIpv4FilterCfgs[0].Ipv4Filter));
    if (request(WLAN_EXT_CMD_SET_P2P_WKP_CFG, reinterpret_cast<uint32_t>(&wkp)) != 0)
      return false;
  }

  // Server 0 sends from all six areas; keepalive entries 1 and 2 only hold frames and stay disabled.
  auto &kpalive = offload_cmd.kpalive;
  memset(&kpalive, 0, sizeof(kpalive));
  kpalive.KeepAlivePeriod_s = this->period_s_;
  kpalive.TxTimeOut_s = TX_TIMEOUT_S;
  kpalive.TxRetryLimit = TX_RETRIES;
  const size_t per_entry = ENTRY_PAYLOAD / this->frame_len_;
  for (size_t e = 0; e < ENTRY_COUNT; ++e) {
    auto &slot = kpalive.P2PKeepAliveParamCfgs[e];
    slot.Enable = e == SERVER;
    slot.PayloadLen = this->frame_len_;
    const size_t first = e * per_entry;
    if (first < this->frame_count_) {
      const size_t frames = std::min(per_entry, this->frame_count_ - first);
      memcpy(slot.Payload, frame_capture.data + first * this->frame_len_, frames * this->frame_len_);
    }
  }
  // After the wake pattern MIB, which clears disabled entries. The window takes 32-bit accesses.
  for (size_t e = 0; e < ENTRY_COUNT; ++e) {
    uint32_t words[ENTRY_PAYLOAD / 4]{};
    const size_t first = (ENTRY_COUNT + e) * per_entry;
    if (first < this->frame_count_) {
      const size_t frames = std::min(per_entry, this->frame_count_ - first);
      memcpy(words, frame_capture.data + first * this->frame_len_, frames * this->frame_len_);
    }
    auto *area = reinterpret_cast<volatile uint32_t *>(WAKE_PAYLOAD_APP_ADDRESS + e * WAKE_ENTRY_SIZE);
    for (size_t w = 0; w < ENTRY_PAYLOAD / 4; ++w)
      area[w] = words[w];
    // Read the last word back through the firmware, which also confirms the window mapping.
    uint32_t check = 0;
    if (wlan_read_words(WAKE_PAYLOAD_ADDRESS + e * WAKE_ENTRY_SIZE + ENTRY_PAYLOAD - 4, &check, 1) != 0 ||
        check != words[ENTRY_PAYLOAD / 4 - 1]) {
      ESP_LOGW(TAG, "Not armed: frame area %u reads back %08" PRIX32, static_cast<unsigned>(ENTRY_COUNT + e), check);
      return false;
    }
  }
  if (request(WLAN_EXT_CMD_SET_P2P_KPALIVE_CFG, reinterpret_cast<uint32_t>(&kpalive)) != 0)
    return false;
  if (this->host_sleep_ && request(WLAN_EXT_CMD_SET_P2P_HOST_SLEEP, 1) != 0)
    return false;
  ESP_LOGI(TAG, "Armed: %u frames of %u bytes every %u s, %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u, seq %08" PRIX32,
           this->frame_count_, this->frame_len_, this->period_s_, tcp.local_ip[0], tcp.local_ip[1], tcp.local_ip[2],
           tcp.local_ip[3], tcp.local_port, tcp.remote_ip[0], tcp.remote_ip[1], tcp.remote_ip[2], tcp.remote_ip[3],
           tcp.remote_port, tcp.snd_nxt);
  return true;
}

void XR806APIKeepaliveOffload::stop_firmware_() {
  memset(&offload_cmd.kpalive, 0, sizeof(offload_cmd.kpalive));
  request(WLAN_EXT_CMD_SET_P2P_KPALIVE_CFG, reinterpret_cast<uint32_t>(&offload_cmd.kpalive));
  if (this->wake_on_peer_) {
    memset(&offload_cmd.wkp, 0, sizeof(offload_cmd.wkp));
    request(WLAN_EXT_CMD_SET_P2P_WKP_CFG, reinterpret_cast<uint32_t>(&offload_cmd.wkp));
  }
}

void XR806APIKeepaliveOffload::send_unsent_(uint32_t from) {
  for (uint32_t k = from; k < this->frame_count_; ++k) {
    if (__real_lwip_write(this->fd_, frame_capture.data + k * this->frame_len_, this->frame_len_) != this->frame_len_) {
      this->close_("sending a held frame failed");
      return;
    }
  }
}

// Home Assistant sent no API message while the firmware kept the connection alive, only TCP ACKs. The API would
// then send its own ping and, once the time since the last received message passed its disconnect limit, drop the
// connection in the next loop before the answer arrives. A ping sent here gets its answer into the socket before the
// API's next read, which comes before its keepalive check.
void XR806APIKeepaliveOffload::await_peer_message_() {
  if (!this->client_->send_message(api::PingRequest{}))
    return;
  const uint32_t start = millis();
  while (millis() - start < PEER_ANSWER_WAIT_MS) {
    int pending = 0;
    if (lwip_ioctl(this->fd_, FIONREAD, &pending) != 0 || pending > 0)
      return;
    delay(5);
  }
  ESP_LOGD(TAG, "No answer to the ping within %" PRIu32 " ms", PEER_ANSWER_WAIT_MS);
}

void XR806APIKeepaliveOffload::close_(const char *reason) {
  ESP_LOGW(TAG, "Closing the API connection: %s", reason);
  for (const auto &conn : api::global_api_server->active_clients()) {
    if (conn.get() == this->client_)
      this->client_->on_fatal_error();
  }
}

static api::APIConnection *single_client() {
  api::APIConnection *client = nullptr;
  size_t count = 0;
  for (const auto &conn : api::global_api_server->active_clients()) {
    client = conn.get();
    ++count;
  }
  return count == 1 && client->is_authenticated() ? client : nullptr;
}

bool XR806APIKeepaliveOffload::ready_for_standby() {
  auto *controller = standby_controller();
  if (!this->enabled() || controller == nullptr || !controller->stay_connected() || !wlan_fw_patched_active())
    return true;
  const uint32_t now = millis();
  if (now - this->burst_ms_ < SETTLE_MS)
    return false;
  auto *client = single_client();
  if (client == nullptr)
    return true;
  if (now - last_write_ms + this->period_s_ * 1000U <= PEER_PING_LIMIT_MS)
    return true;
  // Home Assistant would ping before the first firmware frame; restart its timer and try again shortly.
  ESP_LOGD(TAG, "Standby waits: %" PRIu32 " ms since the last write, sending a ping response first",
           now - last_write_ms);
  if (!client->send_message(api::PingResponse{}))
    return true;
  this->burst_ms_ = now;
  return false;
}

void XR806APIKeepaliveOffload::arm(uint32_t wake_after_ms) {
  this->client_ = nullptr;
  auto *controller = standby_controller();
  if (!this->enabled() || controller == nullptr || !controller->stay_connected())
    return;
  if (!wlan_fw_patched_active()) {
    ESP_LOGD(TAG, "Not armed: the patched WLAN firmware is not running yet");
    return;
  }
  api::APIConnection *client = single_client();
  if (client == nullptr)
    return;

  // Every frame takes a Noise nonce, and the host sends those the firmware did not, so encrypt only what the
  // firmware can send before the wake timer (frame k goes out k + 1 periods after arming), plus one for a late wake;
  // a list that runs out would repeat its last frame and nonce.
  const uint32_t max_frames = wake_after_ms / (this->period_s_ * 1000U) + 1;
  if (!this->capture_frames_(client, max_frames)) {
    if (this->frame_count_ == 0 && frame_capture.len == 0)
      return;  // Nothing reached the socket, so no nonce was used.
    this->client_ = client;
    this->close_("could not capture the keepalive frames");
    this->client_ = nullptr;
    return;
  }
  this->client_ = client;
  this->armed_ms_ = millis();
  if (!this->configure_firmware_()) {
    // The frames hold Noise nonces: send them now so both sides stay in step.
    ESP_LOGW(TAG, "Firmware refused the keepalive configuration");
    this->stop_firmware_();
    memset(&offload_cmd.svr, 0, sizeof(offload_cmd.svr));
    request(WLAN_EXT_CMD_SET_P2P_SVR, reinterpret_cast<uint32_t>(&offload_cmd.svr));
    this->send_unsent_(0);
    this->burst_ms_ = millis();
    this->client_ = nullptr;
    return;
  }
  // Wake before the firmware runs out of frames: frame n goes out n periods after arming.
  const uint32_t list_ms = (static_cast<uint32_t>(this->frame_count_) * this->period_s_ * 1000) +
                           (static_cast<uint32_t>(this->period_s_) * 500);
  if (list_ms < wake_after_ms)
    HAL_Wakeup_SetTimer_mS(list_ms);
  ESP_LOGD(TAG, "Armed details: seq %08" PRIX32 " ack %08" PRIX32 " IP ID %u, wake timer %" PRIu32 " ms",
           this->snd_nxt_, this->rcv_nxt_, IP_ID_INIT, std::min(list_ms, wake_after_ms));
}

void XR806APIKeepaliveOffload::disarm(const XR806StandbyResume *event) {
  if (this->client_ == nullptr)
    return;
  const uint32_t elapsed = millis() - this->armed_ms_;
  if (this->host_sleep_)
    request(WLAN_EXT_CMD_SET_P2P_HOST_SLEEP, 0);

  // A frame in flight is counted once its ACK arrives; the firmware only handles ACKs while the keepalive is on.
  FirmwareState state{};
  bool ok = read_state(state);
  const uint32_t wait_start = millis();
  while (ok && state.in_flight && millis() - wait_start < ACK_WAIT_MS) {
    delay(10);
    ok = read_state(state);
  }
  this->stop_firmware_();
  ok = ok && read_state(state);
  memset(&offload_cmd.svr, 0, sizeof(offload_cmd.svr));
  request(WLAN_EXT_CMD_SET_P2P_SVR, reinterpret_cast<uint32_t>(&offload_cmd.svr));
  wlan_ext_p2p_state_set(WLAN_EXT_P2P_STATE_INIT);

  ESP_LOGD(TAG,
           "Firmware state: seq %08" PRIX32 " (lwIP %08" PRIX32 ") ack %08" PRIX32 " (lwIP %08" PRIX32
           ") IP ID %" PRIu32 " retries %u in flight %d, read %s",
           state.seq, this->snd_nxt_, state.ack, this->rcv_nxt_, state.ip_id, state.retries, state.in_flight,
           ok ? LOG_STR_LITERAL("ok") : LOG_STR_LITERAL("failed"));
  bool alive = false;
  for (const auto &conn : api::global_api_server->active_clients())
    alive = alive || conn.get() == this->client_;
  // The firmware adds the frame length to its sequence for every frame Home Assistant acknowledged; the IP ID
  // also counts other frames, such as acknowledgements, so it is only logged.
  const uint32_t advanced = state.seq - this->snd_nxt_;
  const uint32_t sent = this->frame_len_ != 0 ? advanced / this->frame_len_ : 0;
  ESP_LOGI(TAG,
           "Woke after %" PRIu32 " ms (wlan=%d timer=%d gpio=%d): firmware sent %" PRIu32
           " of %u frames, seq %08" PRIX32 "%s",
           elapsed, event != nullptr && event->wake_wlan, event != nullptr && event->wake_timer,
           event != nullptr && event->wake_gpio, sent, this->frame_count_, state.seq,
           state.in_flight ? ", one still unacknowledged" : "");
  if (!alive) {
    ESP_LOGI(TAG, "The API connection is gone");
  } else if (!ok) {
    this->close_("could not read the firmware state");
  } else if (state.in_flight) {
    this->close_("a frame is still unacknowledged");
  } else if (sent > this->frame_count_ || advanced != sent * this->frame_len_) {
    this->close_("the firmware state does not match the frames");
  } else if (xr806_offload_advance(this->fd_, sent * this->frame_len_) != 0) {
    this->close_("lwIP could not take over the sequence numbers");
  } else {
    this->send_unsent_(sent);
    this->burst_ms_ = millis();
    this->await_peer_message_();
    ESP_LOGD(TAG, "Resynced: lwIP sequence +%" PRIu32 " bytes, %" PRIu32 " held frames sent by the host",
             sent * this->frame_len_, this->frame_count_ - sent);
  }
  this->client_ = nullptr;
}

}  // namespace esphome::xr806

#endif
