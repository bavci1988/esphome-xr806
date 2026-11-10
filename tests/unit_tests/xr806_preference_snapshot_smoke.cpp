// Host-only tests of the exact CRC and record layout used by XR806 preferences.
#include "esphome/components/xr806/preference_snapshot.h"

#include <array>
#include <cassert>
#include <cstring>

using esphome::xr806::PreferenceSnapshotHeader;
using esphome::xr806::preference_generation_newer;
using esphome::xr806::preference_snapshot_checksum;
using esphome::xr806::preference_snapshot_valid;

int main() {
  constexpr uint32_t magic = 0x58525046;
  constexpr uint16_t version = 1;
  std::array<uint8_t, 2048> sector{};

  // One valid saved WLAN preference record: key32, length16, data.
  constexpr uint32_t key = 0x12345678;
  constexpr char credentials[] = "wifi-ssid";
  constexpr uint16_t length = sizeof(credentials);
  constexpr uint16_t used = 6 + length;
  PreferenceSnapshotHeader hdr{magic, version, used, 7, 0};
  memcpy(sector.data(), &hdr, sizeof(hdr));
  memcpy(sector.data() + sizeof(hdr), &key, sizeof(key));
  memcpy(sector.data() + sizeof(hdr) + 4, &length, sizeof(length));
  memcpy(sector.data() + sizeof(hdr) + 6, credentials, length);
  hdr.checksum = preference_snapshot_checksum(sector.data(), used);
  memcpy(sector.data(), &hdr, sizeof(hdr));

  PreferenceSnapshotHeader decoded{};
  assert(preference_snapshot_valid(sector.data(), sector.size(), magic, version, &decoded));
  assert(decoded.generation == 7);
  assert(decoded.used == used);

  // Flip one bit of the saved password: CRC detects it.
  sector[sizeof(hdr) + 6] ^= 0x01;
  assert(!preference_snapshot_valid(sector.data(), sector.size(), magic, version, &decoded));
  sector[sizeof(hdr) + 6] ^= 0x01;
  assert(preference_snapshot_valid(sector.data(), sector.size(), magic, version, &decoded));

  // Torn sector write and invalid length must never become valid.
  sector[sizeof(hdr) + 4] = 0xff;
  sector[sizeof(hdr) + 5] = 0xff;
  assert(!preference_snapshot_valid(sector.data(), sector.size(), magic, version, &decoded));
  memcpy(sector.data() + sizeof(hdr) + 4, &length, sizeof(length));
  assert(preference_snapshot_valid(sector.data(), sector.size(), magic, version, &decoded));

  // Corrupt metadata generation or checksum and reject.
  sector[8] ^= 0x02;
  assert(!preference_snapshot_valid(sector.data(), sector.size(), magic, version, &decoded));
  sector[8] ^= 0x02;

  // The second FDCM slot is only accepted when fully valid, even if the
  // generation counter is numerically larger due to corruption.
  auto newer = sector;
  newer[8] = 0xff;
  assert(!preference_snapshot_valid(newer.data(), newer.size(), magic, version, &decoded));
  assert(preference_snapshot_valid(sector.data(), sector.size(), magic, version, &decoded));

  // Serial number arithmetic handles 32-bit rollover.
  assert(preference_generation_newer(8, 7));
  assert(preference_generation_newer(0, 0xffffffffU));
  assert(!preference_generation_newer(0xffffffffU, 0));
  assert(!preference_generation_newer(7, 7));
}
