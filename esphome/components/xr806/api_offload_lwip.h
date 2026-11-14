#pragma once

// Reads the lwIP state of one TCP connection for the WLAN firmware keepalive offload.
// C interface: the implementation needs the lwIP private headers, which do not compile as C++.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct netif;

struct xr806_offload_tcp {
  uint32_t snd_nxt;  // next sequence number this side sends
  uint32_t rcv_nxt;  // next sequence number expected from the peer
  uint8_t local_ip[4];
  uint8_t remote_ip[4];
  uint8_t next_hop_mac[6];  // the peer itself on the local subnet, else the gateway
  uint16_t local_port;
  uint16_t remote_port;
  uint8_t established;  // ESTABLISHED, with no unsent or unacknowledged data
};

/// 0 on success. -1: no such TCP socket; -2: not IPv4 or no IPv4 address; -3: next hop MAC not in the ARP table.
int xr806_offload_read_tcp(int fd, struct netif *nif, struct xr806_offload_tcp *out);

#ifdef __cplusplus
}
#endif
