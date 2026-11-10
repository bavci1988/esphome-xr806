#include "esphome/components/xr806/preference_journal.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

using esphome::xr806::PreferenceJournal;
using esphome::xr806::PreferenceSnapshotHeader;

static constexpr size_t SIZE = 2048;
using Journal = PreferenceJournal<SIZE>;
using Put = Journal::PutResult;

// Two slots that behave like FDCM with one snapshot per 4 KiB sector: a write erases first.
// cut_after simulates power loss: the write stops after that many bytes and the call fails.
struct FakeFlash {
  std::array<std::array<uint8_t, SIZE>, 2> slots{};
  std::array<bool, 2> formatted{false, false};
  long cut_after{-1};
  bool corrupt_readback{false};
  unsigned writes{0};
  unsigned erases{0};

  bool read(uint8_t slot, uint8_t *data, size_t size) {
    if (!this->formatted[slot] || size != SIZE)
      return false;
    std::memcpy(data, this->slots[slot].data(), size);
    if (this->corrupt_readback)
      data[size - 1] ^= 0x01;
    return true;
  }
  bool write(uint8_t slot, const uint8_t *data, size_t size) {
    this->writes++;
    this->slots[slot].fill(0xFF);
    this->formatted[slot] = true;
    if (this->cut_after >= 0) {
      std::memcpy(this->slots[slot].data(), data, static_cast<size_t>(this->cut_after));
      return false;
    }
    std::memcpy(this->slots[slot].data(), data, size);
    return true;
  }
  bool erase(uint8_t slot) {
    this->erases++;
    this->slots[slot].fill(0xFF);
    this->formatted[slot] = false;
    return true;
  }
};

struct Wifi {
  char ssid[33];
  char password[65];
};

static Wifi make_wifi(const char *ssid, const char *password) {
  Wifi w{};
  std::strncpy(w.ssid, ssid, sizeof(w.ssid) - 1);
  std::strncpy(w.password, password, sizeof(w.password) - 1);
  return w;
}

static bool load_wifi(Journal &j, Wifi *out) { return j.get(0x1234, reinterpret_cast<uint8_t *>(out), sizeof(*out)); }

static Put put_wifi(Journal &j, const Wifi &w) {
  return j.put(0x1234, reinterpret_cast<const uint8_t *>(&w), sizeof(w));
}

static void test_empty_flash_then_round_trip_across_reboot() {
  FakeFlash flash;
  {
    Journal j;
    assert(!j.load(flash));
    Wifi w{};
    assert(!load_wifi(j, &w));
    assert(put_wifi(j, make_wifi("home", "secret123")) == Put::PUT_RESULT_CHANGED);
    assert(j.commit(flash));
    assert(j.active_slot() == 0);
    assert(j.generation() == 1);
  }
  Journal after_reboot;
  assert(after_reboot.load(flash));
  Wifi w{};
  assert(load_wifi(after_reboot, &w));
  assert(std::strcmp(w.ssid, "home") == 0 && std::strcmp(w.password, "secret123") == 0);
}

static void test_commits_alternate_and_newest_wins() {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  for (int i = 0; i < 5; i++) {
    char ssid[16];
    std::snprintf(ssid, sizeof(ssid), "net%d", i);
    put_wifi(j, make_wifi(ssid, "password"));
    assert(j.commit(flash));
    assert(j.active_slot() == (i % 2));
  }
  Journal r;
  assert(r.load(flash));
  Wifi w{};
  assert(load_wifi(r, &w) && std::strcmp(w.ssid, "net4") == 0);
  assert(r.generation() == 5);
}

static void test_power_loss_during_write_keeps_previous_snapshot() {
  // Cut right after the erase, inside the header and inside the records.
  for (long cut : {0L, 4L, 15L, 100L, 119L}) {
    FakeFlash flash;
    Journal j;
    j.load(flash);
    put_wifi(j, make_wifi("old", "oldpassword"));
    assert(j.commit(flash));
    put_wifi(j, make_wifi("new", "newpassword"));
    flash.cut_after = cut;
    assert(!j.commit(flash));
    assert(j.dirty());  // The change is kept for the next attempt.

    Journal after_reboot;
    assert(after_reboot.load(flash));
    Wifi w{};
    assert(load_wifi(after_reboot, &w) && std::strcmp(w.ssid, "old") == 0);

    // The next commit goes to the damaged slot again, never over the good one.
    flash.cut_after = -1;
    assert(after_reboot.active_slot() == 0);
    put_wifi(after_reboot, make_wifi("retry", "retrypassword"));
    assert(after_reboot.commit(flash));
    assert(after_reboot.active_slot() == 1);
  }
}

static void test_power_loss_after_records_keeps_new_snapshot() {
  // Header and records are complete; only unused padding is missing, so the new snapshot is valid.
  FakeFlash flash;
  Journal j;
  j.load(flash);
  put_wifi(j, make_wifi("old", "oldpassword"));
  assert(j.commit(flash));
  put_wifi(j, make_wifi("new", "newpassword"));
  flash.cut_after = static_cast<long>(Journal::HEADER_SIZE + j.used());
  assert(!j.commit(flash));
  Journal after_reboot;
  assert(after_reboot.load(flash));
  Wifi w{};
  assert(load_wifi(after_reboot, &w) && std::strcmp(w.ssid, "new") == 0);
}

static void test_failed_readback_keeps_previous_slot() {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  put_wifi(j, make_wifi("a", "aaaaaaaa"));
  assert(j.commit(flash));
  put_wifi(j, make_wifi("b", "bbbbbbbb"));
  flash.corrupt_readback = true;
  assert(!j.commit(flash));
  assert(j.active_slot() == 0 && j.generation() == 1 && j.dirty());
  flash.corrupt_readback = false;
  Journal r;
  assert(r.load(flash));
  Wifi w{};
  assert(load_wifi(r, &w));
  // Slot 1 holds a complete, valid snapshot even though the readback was reported bad; it is newer.
  assert(std::strcmp(w.ssid, "b") == 0);
}

static void test_unchanged_value_causes_no_write() {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  const Wifi w = make_wifi("same", "samesame");
  assert(put_wifi(j, w) == Put::PUT_RESULT_CHANGED);
  assert(j.commit(flash));
  const unsigned writes = flash.writes;
  assert(put_wifi(j, w) == Put::PUT_RESULT_UNCHANGED);
  assert(!j.dirty());
  assert(j.commit(flash));
  assert(flash.writes == writes);
}

static void test_records_are_kept_and_replaced() {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  const uint32_t a = 7, b = 0xDEADBEEF;
  assert(j.put(1, reinterpret_cast<const uint8_t *>(&a), sizeof(a)) == Put::PUT_RESULT_CHANGED);
  assert(j.put(2, reinterpret_cast<const uint8_t *>(&b), sizeof(b)) == Put::PUT_RESULT_CHANGED);
  const uint16_t a2 = 9;  // Same key, different length replaces the record.
  assert(j.put(1, reinterpret_cast<const uint8_t *>(&a2), sizeof(a2)) == Put::PUT_RESULT_CHANGED);
  assert(j.commit(flash));
  Journal r;
  assert(r.load(flash));
  uint32_t out32 = 0;
  uint16_t out16 = 0;
  assert(!r.get(1, reinterpret_cast<uint8_t *>(&out32), sizeof(out32)));  // Wrong length is not returned.
  assert(r.get(1, reinterpret_cast<uint8_t *>(&out16), sizeof(out16)) && out16 == 9);
  assert(r.get(2, reinterpret_cast<uint8_t *>(&out32), sizeof(out32)) && out32 == b);
  assert(r.used() == 2 * Journal::RECORD_HEADER_SIZE + sizeof(uint16_t) + sizeof(uint32_t));
}

static void test_full_store_rejects_without_losing_data() {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  std::array<uint8_t, 1000> big{};
  big.fill(0xAB);
  assert(j.put(1, big.data(), big.size()) == Put::PUT_RESULT_CHANGED);
  assert(j.put(2, big.data(), big.size()) == Put::PUT_RESULT_CHANGED);
  assert(j.put(3, big.data(), big.size()) == Put::PUT_RESULT_FULL);
  assert(j.put(4, big.data(), SIZE) == Put::PUT_RESULT_FULL);
  std::array<uint8_t, 1000> out{};
  assert(j.get(1, out.data(), out.size()) && out == big);
  assert(j.get(2, out.data(), out.size()) && out == big);
}

static void test_corrupt_slots_are_ignored() {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  put_wifi(j, make_wifi("good", "goodgood"));
  assert(j.commit(flash));
  put_wifi(j, make_wifi("newer", "newernewer"));
  assert(j.commit(flash));
  flash.slots[1][40] ^= 0x10;  // Bit flip in the newer snapshot.
  Journal r;
  assert(r.load(flash));
  Wifi w{};
  assert(load_wifi(r, &w) && std::strcmp(w.ssid, "good") == 0);
  flash.slots[0][20] ^= 0x01;
  Journal empty;
  assert(!empty.load(flash));
  assert(empty.used() == 0);
}

static void test_damaged_record_length_is_rejected() {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  const uint32_t v = 1;
  j.put(5, reinterpret_cast<const uint8_t *>(&v), sizeof(v));
  assert(j.commit(flash));
  // Make the record length point past the end, then fix the CRC so only the walk can catch it.
  auto &slot = flash.slots[0];
  const uint16_t bad_len = 0x7FFF;
  std::memcpy(slot.data() + Journal::HEADER_SIZE + 4, &bad_len, sizeof(bad_len));
  PreferenceSnapshotHeader header;
  std::memcpy(&header, slot.data(), sizeof(header));
  header.checksum = esphome::xr806::preference_snapshot_checksum(slot.data(), header.used);
  std::memcpy(slot.data(), &header, sizeof(header));
  Journal r;
  assert(!r.load(flash));
}

static void test_generation_wraps() {
  assert(esphome::xr806::preference_generation_newer(0, 0xFFFFFFFF));
  assert(!esphome::xr806::preference_generation_newer(0xFFFFFFFF, 0));
  assert(esphome::xr806::preference_generation_newer(2, 1));
  assert(!esphome::xr806::preference_generation_newer(1, 1));
}

static void test_clear_commits_empty_and_erases_old_slot() {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  put_wifi(j, make_wifi("home", "secret123"));
  assert(j.commit(flash));
  assert(j.clear(flash));
  assert(flash.erases == 1);
  assert(!flash.formatted[0]);  // The slot that held the credentials is gone.
  Journal r;
  assert(r.load(flash));
  Wifi w{};
  assert(!load_wifi(r, &w));
  assert(r.used() == 0);
}

int main() {
  test_empty_flash_then_round_trip_across_reboot();
  test_commits_alternate_and_newest_wins();
  test_power_loss_during_write_keeps_previous_snapshot();
  test_power_loss_after_records_keeps_new_snapshot();
  test_failed_readback_keeps_previous_slot();
  test_unchanged_value_causes_no_write();
  test_records_are_kept_and_replaced();
  test_full_store_rejects_without_losing_data();
  test_corrupt_slots_are_ignored();
  test_damaged_record_length_is_rejected();
  test_generation_wraps();
  test_clear_commits_empty_and_erases_old_slot();
  std::printf("xr806 preference journal: all tests passed\n");
  return 0;
}
