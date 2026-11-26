/*
 * TCP keepalive for the WLAN core of the XR806 (ARMv5TE, Thumb), written for esphome-xr806.
 *
 * Replaces the vendor's keepalive ("P2P server") of the WLAN firmware. It is linked into free RAM of the WLAN core
 * and hooked into two entries of the firmware's function table: the receive hook (called by the ROM for every
 * received data frame) and the MIB pre-handler (called for every MIB write from the host). Everything else it needs
 * is in the ROM, which is the same for every firmware version; the addresses are in rom.h.
 *
 * The host configures it with the vendor's MIBs 0x1076..0x1079 (wlan_ext_request), so the driver needs no change:
 * - 0x1076 server: the TCP connection (only server 0 is used); Enable = 0 stops everything;
 * - 0x1077 wake: accepted and ignored. Data from the peer always reaches the host; the vendor's IPv4 filters also
 *   let every packet from 0.0.0.0 (DHCP of any client) through for each unused entry;
 * - 0x1078 keepalive: period, timeout, retry limit, the stop count W (reserved1), and of entry 0 Enable, the frame
 *   length (PayloadLen) and the number of frames (Payload[0]); Enable = 0 stops sending. The host writes the frames
 *   directly into ka.frames before, back to back;
 * - 0x1079 host sleep.
 * Stopping keeps the state (sequence, retries, frame in flight, stop count) for the host to read.
 *
 * Every period it sends frame k = (sequence - TcpSeqInit) / length of a list of pre-encrypted frames, as a TCP
 * segment with PSH|ACK, and waits for Home Assistant's acknowledgement of exactly this frame. Data from Home
 * Assistant is never acknowledged here: it goes to the host, which wakes. The host reads the state after waking and
 * moves its own TCP sequence by the frames sent.
 */

#include <stdint.h>

#include "rom.h"

#include <stddef.h>

#define FRAME_BYTES 704    /* 30 frames of 23 bytes */
#define HEADER_BUFFER 1024 /* what the vendor firmware clears of the header buffer */

#define MIB_SERVER 0x1076
#define MIB_WAKE 0x1077
#define MIB_KEEPALIVE 0x1078
#define MIB_HOST_SLEEP 0x1079

#define STATE_WAKE_PACKET 2
#define STATE_KEEPALIVE_LOST 3

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#ifndef WAKE_PORT_1
#define WAKE_PORT_1 80
#endif
#ifndef WAKE_PORT_2
#define WAKE_PORT_2 3232
#endif
#ifndef WAKE_PORT_3
#define WAKE_PORT_3 6053
#endif

struct rom_timer {
  uint32_t words[5];
};

/* The host reads and writes this through the WLAN RAM window: the state at +4 (four words) and the frames. */
struct keepalive {
  uint8_t period_s;
  uint8_t timeout_s;
  uint8_t retry_limit;
  uint8_t countdown; /* with a stop count: frames still to build, retries included; sending ends at 0 */
  uint32_t seq;      /* next sequence number; TcpSeqInit + length * acknowledged frames */
  uint32_t ack;      /* acknowledgement number sent: the host's rcv_nxt, never moved here */
  uint32_t ip_id;
  uint8_t retries;
  uint8_t in_flight;
  uint8_t enabled; /* frames are sent and the peer's segments handled here */
  uint8_t host_sleep;
  struct rom_timer period_timer;
  struct rom_timer retransmit_timer;
  uint8_t frames[FRAME_BYTES];
  uint32_t seq_init;
  uint8_t src_ip[4];
  uint8_t dst_ip[4];
  uint8_t dst_mac[6];
  uint16_t src_port; /* host order */
  uint16_t dst_port;
  uint16_t frame_len;
  uint8_t frame_count;
  uint8_t enc_hdr;
  uint8_t enc_tail;
  uint8_t vif;
  uint8_t server;  /* the connection is configured; its segments are matched */
  uint8_t limited; /* a stop count was given */
};
_Static_assert(offsetof(struct keepalive, seq) == 4, "host contract");
_Static_assert(offsetof(struct keepalive, period_timer) == 0x14, "host contract");
_Static_assert(offsetof(struct keepalive, retransmit_timer) == 0x28, "host contract");
_Static_assert(offsetof(struct keepalive, frames) == 0x3C, "host contract");

struct keepalive ka = {0};

static uint16_t get_be16(const uint8_t *p) { return (uint16_t) ((p[0] << 8) | p[1]); }
static uint32_t get_be32(const uint8_t *p) { return ((uint32_t) get_be16(p) << 16) | get_be16(p + 2); }
static void put_be16(uint8_t *p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}
static void put_be32(uint8_t *p, uint32_t v) {
  put_be16(p, v >> 16);
  put_be16(p + 2, v);
}
static uint16_t get_le16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static uint32_t get_le32(const uint8_t *p) { return get_le16(p) | ((uint32_t) get_le16(p + 2) << 16); }
static void copy(void *dst, const void *src, uint32_t n) {
  uint8_t *d = dst;
  const uint8_t *s = src;
  while (n--)
    *d++ = *s++;
}
static void zero(void *dst, uint32_t n) {
  uint8_t *d = dst;
  while (n--)
    *d++ = 0;
}
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) {
  while (n--)
    if (*a++ != *b++)
      return 0;
  return 1;
}

static uint32_t sum16(const uint8_t *p, uint32_t n, uint32_t sum) {
  for (; n > 1; n -= 2, p += 2)
    sum += get_be16(p);
  if (n)
    sum += (uint32_t) p[0] << 8;
  return sum;
}
static uint16_t fold(uint32_t sum) {
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t) ~sum;
}

static void report(uint8_t state) {
  uint8_t *msg = rom_msg_alloc(8);
  if (msg == 0)
    return;
  msg[0] = 8;
  msg[1] = 0;
  msg[2] = 0x11; /* message type 0x0811: keepalive state */
  msg[3] = 0x08;
  msg[4] = 0; /* server */
  msg[5] = state;
  rom_msg_send(msg);
}

/* Frames the peer acknowledged = index of the frame to send. No divide instruction on this core, and the module
   links no library: count up. */
static uint32_t frames_acked(void) {
  uint32_t bytes = ka.seq - ka.seq_init;
  uint32_t k = 0;
  while (bytes >= ka.frame_len && k < ka.frame_count) {
    bytes -= ka.frame_len;
    k++;
  }
  return k;
}

/* Builds the frame and hands it to the MAC; 0 when no buffer was free. */
static int send_frame(uint32_t k) {
  uint8_t *desc = rom_frame_alloc(ROM_FRAME_TYPE_DATA, 0, 1);
  if (desc == 0)
    return 0;
  uint8_t *hdr = *(uint8_t **) (desc + ROM_DESC_HEADER);
  uint32_t payload_len = ka.frame_len;
  uint32_t mac_len = 26 + ka.enc_hdr;
  uint32_t total = mac_len + 8 + 20 + 20 + payload_len;
  zero(hdr, total + ka.enc_tail);

  /* 802.11 QoS data to the AP: addr1 = BSSID, addr2 = own address, addr3 = destination */
  const uint8_t *vif = rom_vif_info(ka.vif);
  uint16_t fc = 0x0188 | (ka.enc_hdr ? 0x4000 : 0);
  hdr[0] = fc;
  hdr[1] = fc >> 8;
  copy(hdr + 4, vif + ROM_VIF_BSSID, 6);
  copy(hdr + 10, vif + ROM_VIF_OWN_ADDRESS, 6);
  copy(hdr + 16, ka.dst_mac, 6);

  uint8_t *llc = hdr + mac_len;
  llc[0] = 0xAA;
  llc[1] = 0xAA;
  llc[2] = 0x03;
  llc[6] = 0x08; /* IPv4 */

  uint8_t *ip = llc + 8;
  ip[0] = 0x45;
  put_be16(ip + 2, 20 + 20 + payload_len);
  put_be16(ip + 4, ka.ip_id);
  ip[8] = 0xFF;
  ip[9] = 6;
  copy(ip + 12, ka.src_ip, 4);
  copy(ip + 16, ka.dst_ip, 4);
  put_be16(ip + 10, fold(sum16(ip, 20, 0)));

  uint8_t *tcp = ip + 20;
  put_be16(tcp, ka.src_port);
  put_be16(tcp + 2, ka.dst_port);
  put_be32(tcp + 4, ka.seq);
  put_be32(tcp + 8, ka.ack);
  tcp[12] = 0x50;
  tcp[13] = TCP_PSH | TCP_ACK;
  put_be16(tcp + 14, 0x2230); /* window as the vendor firmware sent it */
  copy(tcp + 20, ka.frames + k * payload_len, payload_len);
  uint32_t sum = sum16(ka.src_ip, 4, 0);
  sum = sum16(ka.dst_ip, 4, sum);
  sum += 6 + 20 + payload_len;
  put_be16(tcp + 16, fold(sum16(tcp, 20 + payload_len, sum)));

  rom_frame_send(ka.vif, desc, total + ka.enc_tail);
  return 1;
}

static void stop_timers(void) {
  rom_timer_stop(&ka.period_timer);
  rom_timer_stop(&ka.retransmit_timer);
}

/* Sends the next frame, if the list has one left; 0 when it is exhausted. */
static int send_keepalive(void) {
  uint32_t k = frames_acked();
  if (k >= ka.frame_count)
    return 0;
  if (ka.countdown != 0)
    ka.countdown--;
  ka.in_flight = 1;
  ka.retries++;
  send_frame(k);
  rom_timer_start(&ka.retransmit_timer, ka.timeout_s * 1000000U);
  return 1;
}

/* A frame still in flight belongs to the retransmit timer, which also gives up; sending it again from here would
   restart that timer every period, and with a period below the timeout it would never expire. */
static void on_period(void *arg) {
  (void) arg;
  if (!ka.enabled)
    return;
  /* After the stop count or the last frame nothing more is sent: Home Assistant pings after its own idle time, and
     that data wakes the host. */
  if (!ka.in_flight && !send_keepalive())
    return;
  if (ka.limited && ka.countdown == 0)
    return;
  rom_timer_start(&ka.period_timer, ka.period_s * 1000000U);
}

static void on_retransmit(void *arg) {
  (void) arg;
  if (!ka.enabled || !ka.in_flight)
    return;
  if (ka.retries < ka.retry_limit && send_keepalive())
    return;
  /* Lost: the frame stays "in flight" for the host to see. The connection's segments now all go to the host. */
  ka.enabled = 0;
  report(STATE_KEEPALIVE_LOST);
}

/* MIB pre-handler: returns 1 for the four keepalive MIBs. */
int keepalive_mib(const uint8_t *request) {
  const uint8_t *p = request + 4;
  uint32_t id = get_le16(request);
  if (id == MIB_SERVER) {
    /* EncHdrSize, EncTailSize, reserved, SrcIPv4Addr[4], then server 0 (28 bytes) */
    const uint8_t *s = p + 8;
    stop_timers();
    ka.enabled = 0;
    ka.server = get_le16(s) != 0;
    if (!ka.server)
      return 1;
    ka.in_flight = 0;
    ka.retries = 0;
    ka.enc_hdr = p[0];
    ka.enc_tail = p[1];
    copy(ka.src_ip, p + 4, 4);
    ka.ip_id = get_le16(s + 2);
    ka.seq_init = ka.seq = get_le32(s + 4);
    ka.ack = get_le32(s + 8);
    copy(ka.dst_ip, s + 12, 4);
    ka.src_port = get_le16(s + 16);
    ka.dst_port = get_le16(s + 18);
    copy(ka.dst_mac, s + 20, 6);
    ka.vif = rom_station_vif();
    return 1;
  }
  if (id == MIB_WAKE)
    return 1;
  if (id == MIB_KEEPALIVE) {
    const uint8_t *entry = p + 4; /* Enable, PayloadLen, Payload */
    stop_timers();
    ka.enabled = 0;
    if (get_le16(entry) == 0)
      return 1;
    uint32_t len = get_le16(entry + 2);
    uint32_t count = entry[4];
    if (!ka.server || p[0] == 0 || p[1] == 0 || len == 0 || count == 0 || len * count > FRAME_BYTES ||
        26 + ka.enc_hdr + 8 + 40 + len + ka.enc_tail > HEADER_BUFFER)
      return 1;
    ka.period_s = p[0];
    ka.timeout_s = p[1];
    ka.retry_limit = p[2];
    ka.countdown = p[3];
    ka.limited = p[3] != 0;
    ka.frame_len = len;
    ka.frame_count = count;
    ka.retries = 0;
    ka.in_flight = 0;
    ka.enabled = 1;
    /* the first frame goes out one period after configuration, not while the host suspends the WLAN */
    rom_timer_init(&ka.period_timer, on_period, 0);
    rom_timer_init(&ka.retransmit_timer, on_retransmit, 0);
    rom_timer_start(&ka.period_timer, ka.period_s * 1000000U);
    return 1;
  }
  if (id == MIB_HOST_SLEEP) {
    ka.host_sleep = p[0];
    return 1;
  }
  return 0;
}

static int wake_port(uint16_t port) { return port == WAKE_PORT_1 || port == WAKE_PORT_2 || port == WAKE_PORT_3; }

/*
 * Segment of the keepalive connection while the keepalive runs. Returns 1 to drop it, 0 to pass it to the host.
 * A frame only counts when the acknowledgement number is its sequence plus its length: a duplicate ACK or a ping
 * that crossed the frame must not count a frame that was lost on the air.
 */
static int peer_segment(const uint8_t *tcp, uint32_t data_len) {
  uint8_t flags = tcp[13];
  int acked = 0;
  if ((int32_t) (get_be32(tcp + 4) - ka.ack) >= 0 && (flags & TCP_ACK) && ka.in_flight &&
      get_be32(tcp + 8) == ka.seq + ka.frame_len) {
    /* The retransmit timer is left to expire: its callback returns when nothing is in flight. */
    ka.seq += ka.frame_len;
    ka.ip_id++;
    ka.retries = 0;
    ka.in_flight = 0;
    acked = 1;
  }
  if (flags & (TCP_RST | TCP_FIN)) {
    ka.enabled = 0; /* closed: no more frames, and the host learns of it at once */
    return 0;
  }
  if ((int32_t) (get_be32(tcp + 4) - ka.ack) < 0)
    return ka.host_sleep; /* old segment */
  if (!(flags & TCP_ACK))
    return 0;
  if (data_len == 0)
    return acked || ka.host_sleep; /* used up here; lwIP gets other ACKs while the host is awake */
  /* Data that also acknowledged the frame is dropped while the host sleeps: passing such a segment stalled the WLAN
     core with the vendor firmware. The peer sends the data again, and that copy goes to the host. */
  return acked && ka.host_sleep;
}

/* Receive hook: frame = LLC/SNAP header, then the packet. Returns 1 to drop the frame. */
int keepalive_receive(uint32_t vif, const uint8_t *frame) {
  (void) vif;
  const uint8_t *ip = frame + 8;
  if (get_be16(frame + 6) != 0x0800 || (ip[0] >> 4) != 4)
    return 0;
  uint32_t ihl = (ip[0] & 0x0F) * 4;
  if (ihl < 20)
    return 0;
  uint32_t ip_len = get_be16(ip + 2);
  int fragment = (get_be16(ip + 6) & 0x3FFF) != 0;
  if (ip[9] == 6 && !fragment && ip_len >= ihl + 20) {
    const uint8_t *tcp = ip + ihl;
    uint32_t tcp_len = (tcp[12] >> 4) * 4;
    if (ka.server && get_be16(tcp) == ka.dst_port && get_be16(tcp + 2) == ka.src_port && same(ip + 12, ka.dst_ip, 4) &&
        tcp_len >= 20 && ip_len >= ihl + tcp_len) {
      if (!ka.enabled)
        return 0; /* not, or no longer, kept alive here: the host handles the connection, and wakes for it */
      return peer_segment(tcp, ip_len - ihl - tcp_len);
    }
    if (!ka.host_sleep)
      return 0;
    if ((tcp[13] & 0x3F) == TCP_SYN && wake_port(get_be16(tcp + 2)))
      return 0; /* a new connection to the web server, OTA or the API wakes the host */
  } else if (!ka.host_sleep) {
    return 0;
  }
  return 1;
}
