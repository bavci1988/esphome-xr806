// Run from repository root:
//   g++ -std=c++20 -Wall -Wextra -Werror -I. tests/unit_tests/xr806_captive_dns_smoke.cpp -o /tmp/xr806_dns_test
//   /tmp/xr806_dns_test
#include <array>
#include <cassert>
#include <cstdint>

#include "esphome/components/xr806/captive_dns_packet.h"

int main() {
  constexpr uint8_t query[] = {
      0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 'e',  'x',
      'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01,
  };
  constexpr uint8_t ip[] = {192, 168, 4, 1};
  std::array<uint8_t, 272> answer{};
  size_t n = esphome::xr806::make_captive_dns_reply(query, sizeof(query), ip, answer.data(), answer.size());
  assert(n == sizeof(query) + 16);
  assert(answer[0] == 0x12 && answer[1] == 0x34);
  assert(answer[2] == 0x85);  // QR + AA + RD
  assert(answer[7] == 1);
  assert(answer[n - 4] == 192 && answer[n - 3] == 168 && answer[n - 2] == 4 && answer[n - 1] == 1);

  auto aaaa = std::array<uint8_t, sizeof(query)>{};
  for (size_t i = 0; i < sizeof(query); i++)
    aaaa[i] = query[i];
  aaaa[sizeof(query) - 1] = 0x01;  // IN
  aaaa[sizeof(query) - 3] = 0x1c;  // AAAA
  n = esphome::xr806::make_captive_dns_reply(aaaa.data(), aaaa.size(), ip, answer.data(), answer.size());
  assert(n == sizeof(query) && answer[7] == 0);  // NOERROR, NODATA

  auto malformed = std::array<uint8_t, sizeof(query)>{};
  for (size_t i = 0; i < sizeof(query); i++)
    malformed[i] = query[i];
  malformed[12] = 0xc0;  // compressed pointer in question rejected
  assert(esphome::xr806::make_captive_dns_reply(malformed.data(), malformed.size(), ip, answer.data(), answer.size()) ==
         0);
  malformed[12] = query[12];
  malformed[5] = 2;  // multiple questions rejected
  assert(esphome::xr806::make_captive_dns_reply(malformed.data(), malformed.size(), ip, answer.data(), answer.size()) ==
         0);

  assert(esphome::xr806::make_captive_dns_reply(query, sizeof(query), ip, answer.data(), 20) == 0);
}
