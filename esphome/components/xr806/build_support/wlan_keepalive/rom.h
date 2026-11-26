/*
 * Services of the XR806 WLAN core used by keepalive.c. The ROM is the same for every firmware version; functions
 * that a firmware patch may replace are called through their entry in the firmware's function table.
 * Found by reverse engineering; see docs/WLAN_FIRMWARE_NOTES.md in the xr806-sdk repository.
 */
#pragma once

#include <stdint.h>

#define ROM_FN(address, type) ((type) (address))

/* Timer node: {next, prev, expiry, callback, argument}. The ROM unlinks a node (prev = 0) before it calls the
   callback, and stop returns at once for a node that is not linked. Start clamps a negative delay to 0. */
typedef void (*rom_timer_callback)(void *);
#define rom_timer_init(node, cb, arg) ROM_FN(0x000195B1, void (*)(void *, rom_timer_callback, void *))(node, cb, arg)
#define ROM_TIMER_MIN_US 1000U
static inline void rom_timer_start(void *node, uint32_t us) {
  ROM_FN(0x0000EE47, void (*)(void *, uint32_t))(node, us < ROM_TIMER_MIN_US ? ROM_TIMER_MIN_US : us);
}
#define rom_timer_stop(node) \
  ROM_FN(0x0000EDDD, void (*)(void *))(node) /* start and stop go through slots 0xFFF00468/6C */

/* Message to the host: {u16 length, u16 type, payload}. */
#define rom_msg_alloc(len) ROM_FN(0x0000E5ED, uint8_t *(*) (uint32_t))(len)
#define rom_msg_send(msg) ROM_FN(0x0000EA03, void (*)(uint8_t *))(msg)

/* Frame descriptor of the MAC; the 802.11 header buffer is at +0x1C. The vendor patch zeroes that buffer in its
   wrapper of this allocation, the ROM does not. */
#define ROM_FRAME_TYPE_DATA 0x0B
#define ROM_DESC_HEADER 0x1C
#define rom_frame_alloc(type, a, b) ROM_FN(0x0000CF7D, uint8_t *(*) (uint32_t, uint32_t, uint32_t))(type, a, b)

/* Station interface: index in 0xFFF06678 + 0xA, the per-interface MAC state at 0xFFF06678 + 0x14 + 4 * vif and
   the BSS entry whose addresses go into the header. */
#define ROM_MAC_STATE 0xFFF06678U
#define rom_station_vif() (*(volatile uint8_t *) (ROM_MAC_STATE + 0x0A))
#define ROM_VIF_BSSID 0x0A
#define ROM_VIF_OWN_ADDRESS 0x04
static inline const uint8_t *rom_vif_info(uint32_t vif) {
  return *(const uint8_t *const *) (0xFFF06108U + vif * 4 + 0x340 + 0x3C);
}
/* The per-interface MAC state that rom_frame_send reads the rate from. */
static inline int rom_vif_ready(uint32_t vif) {
  return *(const uint8_t *const *) (ROM_MAC_STATE + 0x14 + vif * 4) != 0;
}

/* Hands a built data frame to the MAC: what the vendor keepalive did after building its frame (0x0900A5DA in
   02.132), with the descriptor fields it set. len includes the cipher tail. */
static inline void rom_frame_send(uint32_t vif, uint8_t *d, uint32_t len) {
  const uint8_t *mac = *(const uint8_t *const *) (ROM_MAC_STATE + 0x14 + vif * 4);
  uint8_t *hdr = *(uint8_t **) (d + ROM_DESC_HEADER);
  uint8_t rate = mac[0x0E] ? mac[0x121] : mac[0x120];
  if (rate == 0xFF || mac[0x0F] < 3)
    rate = mac[0x0C];
  d[0xC1] = vif;
  if (rate & 0x80) {
    rate &= 0x3F;
    *(uint32_t *) (d + 0x58) = 0x40800000U;
  } else {
    *(uint32_t *) (d + 0x58) = 0x40800008U;
    if (rate & 0x40) {
      rate &= 0xBF;
      *(uint32_t *) (d + 0x58) = 0x4080000CU;
    }
  }
  d[0x0C] = rate;
  d[0x63] = rate;
  d[0x0F] = 8;
  *(uint32_t *) (d + 0x00) = 0;
  *(uint32_t *) (d + 0xDC) = 0;
  *(uint32_t *) (d + 0x134) = 0;
  *(uint16_t *) (d + 0x5C) = (uint16_t) len;
  *(uint32_t *) (d + 0x14) = 0;
  *(uint32_t *) (d + 0x18) = (uint16_t) len;
  d[0x0E] = 0;
  *(int32_t *) (d + 0x9C) = *(const int16_t *) (0xFFF02290U + 8);
  *(uint32_t *) (d + 0x94) = 0;
  d[0x60] = *(const uint8_t *) (0xFFF01334U + d[0x0D]);
  *(uint8_t **) (d + 0x54) = hdr;
  *(uint32_t *) (d + 0x64) = 0;
  ROM_FN(0x0000D5A5, void (*)(void *, uint32_t))(d + 0x54, 0);
  d[0xAF] = 0xFF;
  d[0x62] = (d[0x0F] >> 4) & 7;
  ROM_FN(0x0000799F, void (*)(void *))(d + 0x54);
  d[0xC3] = ROM_FN(0x0000B219, uint8_t(*)(uint32_t, const uint8_t *))(vif, hdr + 4);
  (*(void (*const *)(uint8_t *)) 0xFFF00D7CU)(d); /* patch function in both versions */
  *(uint32_t *) (d + 0x84) |= 2;
  ROM_FN(0x0000E045, void (*)(uint8_t *))(d);
}
