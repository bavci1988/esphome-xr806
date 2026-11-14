#include "api_standby.h"

#if defined(USE_XR806) && defined(USE_API) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/components/api/api_server.h"
#include "lowpower_policy.h"

#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
#include <cinttypes>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "api_offload_lwip.h"

extern "C" {
// The WLAN declarations depend on CONFIG_WLAN from the project configuration.
#include "xr_config.h"
#include "net/wlan/wlan.h"
#include "net/wlan/wlan_ext_req.h"
#include "lwip/sockets.h"
#include "kernel/os/os_thread.h"
}

// The build links with --wrap=lwip_write --wrap=lwip_writev when the experiment is enabled. While a capture is open
// on the main loop, the frame the API writes is kept here instead of going out, so the API's own Noise code
// encrypts it and the API component needs no change.
namespace {
struct FrameCapture {
  OS_ThreadHandle_t task{nullptr};
  int fd{-1};
  uint16_t len{0};
  bool overflow{false};
  uint8_t data[IPC_P2P_KPALIVE_PAYLOAD_LEN_MAX];
};
FrameCapture frame_capture;

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
  if (!capturing_here())
    return __real_lwip_write(s, data, size);
  return capture_write(s, data, size);
}

ssize_t __wrap_lwip_writev(int s, const struct iovec *iov, int iovcnt) {
  if (!capturing_here())
    return __real_lwip_writev(s, iov, iovcnt);
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; ++i)
    total += capture_write(s, iov[i].iov_base, iov[i].iov_len);
  return total;
}
}
#endif

namespace esphome::xr806 {

void XR806APIStandby::setup() {
  auto *controller = standby_controller();
  if (controller == nullptr || !controller->register_participant(this)) {
    this->mark_failed(LOG_STR("Cannot register Native API standby participant"));
    return;
  }
  lowpower_policy().set_api_idle(false);
}

void XR806APIStandby::on_shutdown() {
  this->shutting_down_ = true;
  lowpower_policy().set_api_idle(false);
  if (auto *controller = standby_controller(); controller != nullptr) {
    controller->request_standby(false);
    controller->request_preflight(false);
  }
}

bool XR806APIStandby::may_quiesce() const {
#ifdef USE_API_OUTGOING_CONNECTION
  // Dial-out can start network work without an inbound connection.
  return false;
#else
  if (this->shutting_down_)
    return false;
  // With the link kept up a connected client stays connected; its next packet wakes the chip.
  auto *controller = standby_controller();
  if (controller != nullptr && controller->stay_connected())
    return true;
  return !api::global_api_server->is_connected();
#endif
}

bool XR806APIStandby::clients_idle_() const {
  for (const auto &client : api::global_api_server->active_clients()) {
    if (!client->is_idle())
      return false;
  }
  return true;
}

void XR806APIStandby::standby_preflight(bool requested) {
  lowpower_policy().set_api_idle(requested && this->may_quiesce() && this->clients_idle_());
}

bool XR806APIStandby::prepare_for_standby() {
  if (!this->may_quiesce() || !this->clients_idle_())
    return false;
#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
  this->arm_offload_();
#endif
  return true;
}

void XR806APIStandby::standby_cancelled() {
#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
  this->disarm_offload_(nullptr);
#endif
  lowpower_policy().set_api_idle(false);
}

void XR806APIStandby::standby_resumed(const XR806StandbyResume &event) {
#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
  this->disarm_offload_(&event);
#endif
  lowpower_policy().set_api_idle(false);
}

#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
// Experiment for the stock WLAN firmware's TCP keepalive ("P2P server", MIBs 0x1076..0x1079). It shows how the
// firmware handles the connection while the CPU sleeps: whether it sends, which TCP sequence numbers it uses, and
// whether the peer's ACKs or data wake the CPU. A sealed frame uses up a Noise nonce and the firmware does not
// report what it sent, so the connection is closed on every wake.
ESPHOME_LOG_TAG(OFFLOAD_TAG, "xr806.api_offload");

static constexpr uint8_t OFFLOAD_SLOT = 0;
static constexpr uint8_t OFFLOAD_TX_TIMEOUT_S = 5;
static constexpr uint8_t OFFLOAD_TX_RETRIES = 3;
static constexpr uint16_t OFFLOAD_PROTOCOL_TCP = 0x01;

// The firmware commands take large structures; keep them off the main loop stack.
static union {
  wlan_ext_p2p_svr_set_t svr;
  wlan_ext_p2p_wkp_param_set_t wkp;
  wlan_ext_p2p_kpalive_param_set_t kpalive;
} offload_cmd;

static int offload_request(wlan_ext_cmd_t cmd, uint32_t param) {
  return wlan_ext_request(wlan_netif_get(WLAN_MODE_STA), cmd, param);
}

static const LogString *offload_state_name(wlan_ext_p2p_state_t state) {
  switch (state) {
    case WLAN_EXT_P2P_STATE_INIT:
      return LOG_STR("init");
    case WLAN_EXT_P2P_STATE_NORMAL:
      return LOG_STR("normal");
    case WLAN_EXT_P2P_STATE_WAKE_UP:
      return LOG_STR("wake packet");
    case WLAN_EXT_P2P_STATE_KPALIVE_LOSS:
      return LOG_STR("keepalive lost");
    default:
      return LOG_STR("unknown");
  }
}

void XR806APIStandby::arm_offload_() {
  auto *controller = standby_controller();
  if (this->offload_period_s_ == 0 || controller == nullptr || !controller->stay_connected())
    return;
  api::APIConnection *client = nullptr;
  size_t count = 0;
  for (const auto &conn : api::global_api_server->active_clients()) {
    client = conn.get();
    ++count;
  }
  if (count != 1 || !client->is_authenticated())
    return;

  // The API encrypts the PingResponse as usual; the capture keeps the frame and the socket it was meant for.
  frame_capture.fd = -1;
  frame_capture.len = 0;
  frame_capture.overflow = false;
  frame_capture.task = OS_ThreadGetCurrentHandle();
  const bool sent = client->send_message(api::PingResponse{});
  frame_capture.task = nullptr;
  if (!sent || frame_capture.fd < 0)
    return;  // Nothing reached the socket, so no nonce was used.
  // From here on the Noise nonce is used up; any failure has to end the connection.
  this->offload_client_ = client;
  this->offload_armed_ms_ = millis();
  this->offload_fd_ = frame_capture.fd;
  const uint16_t frame_len = frame_capture.len;
  const uint8_t *frame = frame_capture.data;
  // A Noise frame starts with indicator 1; a plaintext connection cannot be offloaded.
  if (frame_capture.overflow || frame_len == 0 || frame[0] != 0x01) {
    ESP_LOGW(OFFLOAD_TAG, "Not armed: no encrypted keepalive frame (%u bytes)", frame_len);
    this->disarm_offload_(nullptr);
    return;
  }

  xr806_offload_tcp tcp{};
  const int read = xr806_offload_read_tcp(this->offload_fd_, wlan_netif_get(WLAN_MODE_STA), &tcp);
  if (read != 0 || !tcp.established) {
    ESP_LOGW(OFFLOAD_TAG, "Not armed: TCP state %d, established %u", read, static_cast<unsigned>(tcp.established));
    this->disarm_offload_(nullptr);
    return;
  }
  this->offload_snd_nxt_ = tcp.snd_nxt;
  this->offload_rcv_nxt_ = tcp.rcv_nxt;

  auto &svr = offload_cmd.svr;
  memset(&svr, 0, sizeof(svr));
  memcpy(svr.SrcIPv4Addr, tcp.local_ip, sizeof(svr.SrcIPv4Addr));
  auto &server = svr.P2PServerCfgs[OFFLOAD_SLOT];
  server.Enable = 1;
  server.IPIdInit = 1;
  // The vendor example takes these from lwip_getsockack() and lwip_getsockseq(), that is snd_nxt and rcv_nxt.
  server.TcpSeqInit = tcp.snd_nxt;
  server.TcpAckInit = tcp.rcv_nxt;
  memcpy(server.DstIPv4Addr, tcp.remote_ip, sizeof(server.DstIPv4Addr));
  server.SrcPort = tcp.local_port;
  server.DstPort = tcp.remote_port;
  memcpy(server.DstMacAddr, tcp.next_hop_mac, sizeof(server.DstMacAddr));
  server.TcpOrUdp = OFFLOAD_PROTOCOL_TCP;
  int err = offload_request(WLAN_EXT_CMD_SET_P2P_SVR, reinterpret_cast<uint32_t>(&svr));

  if (err == 0 && this->offload_wake_on_peer_) {
    auto &wkp = offload_cmd.wkp;
    memset(&wkp, 0, sizeof(wkp));
    wkp.Enable = 1;
    memcpy(wkp.P2PIpv4FilterCfgs[0].Ipv4Filter, tcp.remote_ip, sizeof(wkp.P2PIpv4FilterCfgs[0].Ipv4Filter));
    err = offload_request(WLAN_EXT_CMD_SET_P2P_WKP_CFG, reinterpret_cast<uint32_t>(&wkp));
  }
  if (err == 0) {
    auto &kpalive = offload_cmd.kpalive;
    memset(&kpalive, 0, sizeof(kpalive));
    kpalive.KeepAlivePeriod_s = this->offload_period_s_;
    kpalive.TxTimeOut_s = OFFLOAD_TX_TIMEOUT_S;
    kpalive.TxRetryLimit = OFFLOAD_TX_RETRIES;
    auto &slot = kpalive.P2PKeepAliveParamCfgs[OFFLOAD_SLOT];
    slot.Enable = 1;
    slot.PayloadLen = frame_len;
    memcpy(slot.Payload, frame, frame_len);
    err = offload_request(WLAN_EXT_CMD_SET_P2P_KPALIVE_CFG, reinterpret_cast<uint32_t>(&kpalive));
  }
  if (err == 0 && this->offload_host_sleep_)
    err = offload_request(WLAN_EXT_CMD_SET_P2P_HOST_SLEEP, 1);

  ESP_LOGI(OFFLOAD_TAG,
           "Armed=%d: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u via %02X:%02X:%02X:%02X:%02X:%02X, seq %08" PRIX32
           " ack %08" PRIX32 ", frame %u bytes, every %u s",
           err == 0, tcp.local_ip[0], tcp.local_ip[1], tcp.local_ip[2], tcp.local_ip[3], tcp.local_port,
           tcp.remote_ip[0], tcp.remote_ip[1], tcp.remote_ip[2], tcp.remote_ip[3], tcp.remote_port, tcp.next_hop_mac[0],
           tcp.next_hop_mac[1], tcp.next_hop_mac[2], tcp.next_hop_mac[3], tcp.next_hop_mac[4], tcp.next_hop_mac[5],
           tcp.snd_nxt, tcp.rcv_nxt, frame_len, this->offload_period_s_);
  if (err != 0)
    ESP_LOGW(OFFLOAD_TAG, "Firmware refused the configuration (%d)", err);
}

void XR806APIStandby::disarm_offload_(const XR806StandbyResume *event) {
  if (this->offload_client_ == nullptr)
    return;
  const wlan_ext_p2p_state_t state = wlan_ext_p2p_state_get();
  const uint32_t elapsed = millis() - this->offload_armed_ms_;

  if (this->offload_host_sleep_)
    offload_request(WLAN_EXT_CMD_SET_P2P_HOST_SLEEP, 0);
  memset(&offload_cmd.kpalive, 0, sizeof(offload_cmd.kpalive));
  offload_request(WLAN_EXT_CMD_SET_P2P_KPALIVE_CFG, reinterpret_cast<uint32_t>(&offload_cmd.kpalive));
  if (this->offload_wake_on_peer_) {
    memset(&offload_cmd.wkp, 0, sizeof(offload_cmd.wkp));
    offload_request(WLAN_EXT_CMD_SET_P2P_WKP_CFG, reinterpret_cast<uint32_t>(&offload_cmd.wkp));
  }
  memset(&offload_cmd.svr, 0, sizeof(offload_cmd.svr));
  offload_request(WLAN_EXT_CMD_SET_P2P_SVR, reinterpret_cast<uint32_t>(&offload_cmd.svr));
  wlan_ext_p2p_state_set(WLAN_EXT_P2P_STATE_INIT);

  api::APIConnection *client = this->offload_client_;
  this->offload_client_ = nullptr;
  for (const auto &conn : api::global_api_server->active_clients()) {
    if (conn.get() != client)
      continue;
    xr806_offload_tcp tcp{};
    const int read = xr806_offload_read_tcp(this->offload_fd_, wlan_netif_get(WLAN_MODE_STA), &tcp);
    ESP_LOGI(OFFLOAD_TAG,
             "Disarmed after %" PRIu32 " ms: firmware state %s, wake wlan=%d timer=%d gpio=%d; lwIP read %d, "
             "snd_nxt +%" PRIu32 ", rcv_nxt +%" PRIu32 "; closing the connection",
             elapsed, LOG_STR_ARG(offload_state_name(state)), event != nullptr && event->wake_wlan,
             event != nullptr && event->wake_timer, event != nullptr && event->wake_gpio, read,
             tcp.snd_nxt - this->offload_snd_nxt_, tcp.rcv_nxt - this->offload_rcv_nxt_);
    client->on_fatal_error();
    return;
  }
  ESP_LOGI(OFFLOAD_TAG, "Disarmed after %" PRIu32 " ms: firmware state %s; the connection is already gone", elapsed,
           LOG_STR_ARG(offload_state_name(state)));
}
#endif  // USE_XR806_API_KEEPALIVE_OFFLOAD

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_API && USE_XR806_STANDBY_CONTROLLER
