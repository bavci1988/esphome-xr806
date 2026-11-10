#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Minimal RFC 1035 DNS responder for a local captive portal.
// The caller owns the buffers. No sockets, dynamic allocation or ESPHome dependency.
namespace esphome::xr806 {

inline size_t make_captive_dns_reply(const uint8_t *query, size_t size, const uint8_t ip[4], uint8_t *reply,
                                     size_t capacity) {
  if (size < 17 || capacity < 33 || (query[2] & 0xf8) != 0 || query[4] != 0 || query[5] != 1)
    return 0;  // malformed, response packet, non-standard opcode or multiple questions

  size_t pos = 12;
  for (;;) {
    if (pos >= size)
      return 0;
    const uint8_t length = query[pos++];
    if (length == 0)
      break;
    // Compression in the incoming question is intentionally unsupported.
    // Reject both compression pointers and extended label types safely.
    if (length > 63 || pos + length > size || pos + length > 255)
      return 0;
    pos += length;
  }
  if (pos + 4 > size)
    return 0;

  const bool answer_a = query[pos] == 0 && query[pos + 1] == 1 && query[pos + 2] == 0 && query[pos + 3] == 1;
  const size_t question_size = pos + 4;
  const size_t response_size = question_size + (answer_a ? 16 : 0);
  if (response_size > capacity)
    return 0;

  memcpy(reply, query, question_size);
  reply[2] = static_cast<uint8_t>(0x84 | (query[2] & 1));  // QR, AA, preserve RD; no recursion available
  reply[3] = 0;                                            // NOERROR, RA=0
  reply[4] = 0;
  reply[5] = 1;
  reply[6] = 0;
  reply[7] = answer_a ? 1 : 0;
  reply[8] = reply[9] = reply[10] = reply[11] = 0;

  if (answer_a) {
    static constexpr uint8_t ANSWER[] = {
        0xc0, 0x0c,              // owner: pointer to the query name
        0x00, 0x01,              // A
        0x00, 0x01,              // IN
        0x00, 0x00, 0x00, 0x00,  // TTL=0 (avoid stale redirects)
        0x00, 0x04,              // four bytes
    };
    memcpy(reply + question_size, ANSWER, sizeof(ANSWER));
    memcpy(reply + question_size + sizeof(ANSWER), ip, 4);
  }
  return response_size;
}

}  // namespace esphome::xr806
