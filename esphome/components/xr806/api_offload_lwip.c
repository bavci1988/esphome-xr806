// lwIP side of the XR806 API keepalive offload. C, because lwip/priv/sockets_priv.h does not compile as C++.

#include "esphome/core/defines.h"

#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD

#include <string.h>

#include "lwip/api.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"

#include "esphome/components/xr806/api_offload_lwip.h"

static void put_ip4(uint8_t out[4], const ip4_addr_t *addr) { memcpy(out, &addr->addr, 4); }

// The lwIP build here has no core locking. The caller reads an idle connection right before standby, from
// the main loop that owns the socket, so the tcpip thread has nothing to change in these fields.
int xr806_offload_read_tcp(int fd, struct netif *nif, struct xr806_offload_tcp *out) {
  struct lwip_sock *sock = lwip_socket_dbg_get_socket(fd);
  if (sock == NULL || sock->conn == NULL || NETCONNTYPE_GROUP(sock->conn->type) != NETCONN_TCP)
    return -1;
  const struct tcp_pcb *pcb = sock->conn->pcb.tcp;
  if (pcb == NULL)
    return -1;
  if (nif == NULL || !IP_IS_V4(&pcb->remote_ip) || ip4_addr_isany_val(*netif_ip4_addr(nif)))
    return -2;

  memset(out, 0, sizeof(*out));
  out->snd_nxt = pcb->snd_nxt;
  out->rcv_nxt = pcb->rcv_nxt;
  out->local_port = pcb->local_port;
  out->remote_port = pcb->remote_port;
  out->established = pcb->state == ESTABLISHED && pcb->unsent == NULL && pcb->unacked == NULL;
  put_ip4(out->local_ip, netif_ip4_addr(nif));
  const ip4_addr_t *remote = ip_2_ip4(&pcb->remote_ip);
  put_ip4(out->remote_ip, remote);

  ip4_addr_t next_hop;
  if (ip4_addr_netcmp(remote, netif_ip4_addr(nif), netif_ip4_netmask(nif))) {
    ip4_addr_copy(next_hop, *remote);
  } else {
    ip4_addr_copy(next_hop, *netif_ip4_gw(nif));
  }
  struct eth_addr mac;
  if (!etharp_get_mac_from_ip(&next_hop, &mac))
    return -3;
  memcpy(out->next_hop_mac, mac.addr, sizeof(out->next_hop_mac));
  return 0;
}

struct xr806_advance_call {
  struct tcpip_api_call_data base;
  struct tcp_pcb *pcb;
  uint32_t bytes;
  int result;
};

static err_t xr806_do_advance(struct tcpip_api_call_data *call) {
  struct xr806_advance_call *advance = (struct xr806_advance_call *) call;
  struct tcp_pcb *pcb = advance->pcb;
  if (pcb->state != ESTABLISHED) {
    advance->result = -1;
  } else if (pcb->unsent != NULL || pcb->unacked != NULL) {
    advance->result = -2;
  } else {
    pcb->snd_nxt += advance->bytes;
    pcb->snd_lbb += advance->bytes;
    pcb->lastack += advance->bytes;
    pcb->snd_wl2 = pcb->lastack;
    advance->result = 0;
  }
  return ERR_OK;
}

int xr806_offload_advance(int fd, uint32_t bytes) {
  struct lwip_sock *sock = lwip_socket_dbg_get_socket(fd);
  if (sock == NULL || sock->conn == NULL || NETCONNTYPE_GROUP(sock->conn->type) != NETCONN_TCP ||
      sock->conn->pcb.tcp == NULL)
    return -1;
  struct xr806_advance_call call;
  memset(&call, 0, sizeof(call));
  call.pcb = sock->conn->pcb.tcp;
  call.bytes = bytes;
  call.result = -1;
  if (tcpip_api_call(xr806_do_advance, &call.base) != ERR_OK)
    return -1;
  return call.result;
}

#endif  // USE_XR806_API_KEEPALIVE_OFFLOAD
