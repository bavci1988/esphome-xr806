#include <gtest/gtest.h>

#include "esphome/components/xr806/preference_journal.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace esphome::xr806::testing {

static constexpr size_t SIZE = 2048;
using Journal = PreferenceJournal<SIZE>;
using Put = Journal::PutResult;

// Two slots that behave like FDCM with one snapshot per 4 KiB sector: a write erases first.
// cut_after simulates power loss: the write stops after that many bytes and the call fails.
struct FakeFlash {
  std::array<std::array<uint8_t, SIZE>, 2> slots{};
  std::array<bool, 2> formatted{false, false};
  int64_t cut_after{-1};
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

TEST(XR806PreferenceJournal, EmptyFlashThenRoundTripAcrossReboot) {
  FakeFlash flash;
  {
    Journal j;
    ASSERT_TRUE(!j.load(flash));
    Wifi w{};
    ASSERT_TRUE(!load_wifi(j, &w));
    ASSERT_TRUE(put_wifi(j, make_wifi("home", "secret123")) == Put::PUT_RESULT_CHANGED);
    ASSERT_TRUE(j.commit(flash));
    ASSERT_TRUE(j.active_slot() == 0);
    ASSERT_TRUE(j.generation() == 1);
  }
  Journal after_reboot;
  ASSERT_TRUE(after_reboot.load(flash));
  Wifi w{};
  ASSERT_TRUE(load_wifi(after_reboot, &w));
  ASSERT_TRUE(std::strcmp(w.ssid, "home") == 0 && std::strcmp(w.password, "secret123") == 0);
}

TEST(XR806PreferenceJournal, CommitsAlternateAndNewestWins) {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  for (int i = 0; i < 5; i++) {
    char ssid[16];
    std::snprintf(ssid, sizeof(ssid), "net%d", i);
    put_wifi(j, make_wifi(ssid, "password"));
    ASSERT_TRUE(j.commit(flash));
    ASSERT_TRUE(j.active_slot() == (i % 2));
  }
  Journal r;
  ASSERT_TRUE(r.load(flash));
  Wifi w{};
  ASSERT_TRUE(load_wifi(r, &w) && std::strcmp(w.ssid, "net4") == 0);
  ASSERT_TRUE(r.generation() == 5);
}

TEST(XR806PreferenceJournal, PowerLossDuringWriteKeepsPreviousSnapshot) {
  // Cut right after the erase, inside the header and inside the records.
  for (int64_t cut : {0, 4, 15, 100, 119}) {
    FakeFlash flash;
    Journal j;
    j.load(flash);
    put_wifi(j, make_wifi("old", "oldpassword"));
    ASSERT_TRUE(j.commit(flash));
    put_wifi(j, make_wifi("new", "newpassword"));
    flash.cut_after = cut;
    ASSERT_TRUE(!j.commit(flash));
    ASSERT_TRUE(j.dirty());  // The change is kept for the next attempt.

    Journal after_reboot;
    ASSERT_TRUE(after_reboot.load(flash));
    Wifi w{};
    ASSERT_TRUE(load_wifi(after_reboot, &w) && std::strcmp(w.ssid, "old") == 0);

    // The next commit goes to the damaged slot again, never over the good one.
    flash.cut_after = -1;
    ASSERT_TRUE(after_reboot.active_slot() == 0);
    put_wifi(after_reboot, make_wifi("retry", "retrypassword"));
    ASSERT_TRUE(after_reboot.commit(flash));
    ASSERT_TRUE(after_reboot.active_slot() == 1);
  }
}

TEST(XR806PreferenceJournal, PowerLossAfterRecordsKeepsNewSnapshot) {
  // Header and records are complete; only unused padding is missing, so the new snapshot is valid.
  FakeFlash flash;
  Journal j;
  j.load(flash);
  put_wifi(j, make_wifi("old", "oldpassword"));
  ASSERT_TRUE(j.commit(flash));
  put_wifi(j, make_wifi("new", "newpassword"));
  flash.cut_after = static_cast<int64_t>(Journal::HEADER_SIZE + j.used());
  ASSERT_TRUE(!j.commit(flash));
  Journal after_reboot;
  ASSERT_TRUE(after_reboot.load(flash));
  Wifi w{};
  ASSERT_TRUE(load_wifi(after_reboot, &w) && std::strcmp(w.ssid, "new") == 0);
}

TEST(XR806PreferenceJournal, FailedReadbackKeepsPreviousSlot) {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  put_wifi(j, make_wifi("a", "aaaaaaaa"));
  ASSERT_TRUE(j.commit(flash));
  put_wifi(j, make_wifi("b", "bbbbbbbb"));
  flash.corrupt_readback = true;
  ASSERT_TRUE(!j.commit(flash));
  ASSERT_TRUE(j.active_slot() == 0 && j.generation() == 1 && j.dirty());
  flash.corrupt_readback = false;
  Journal r;
  ASSERT_TRUE(r.load(flash));
  Wifi w{};
  ASSERT_TRUE(load_wifi(r, &w));
  // Slot 1 holds a complete, valid snapshot even though the readback was reported bad; it is newer.
  ASSERT_TRUE(std::strcmp(w.ssid, "b") == 0);
}

TEST(XR806PreferenceJournal, UnchangedValueCausesNoWrite) {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  const Wifi w = make_wifi("same", "samesame");
  ASSERT_TRUE(put_wifi(j, w) == Put::PUT_RESULT_CHANGED);
  ASSERT_TRUE(j.commit(flash));
  const unsigned writes = flash.writes;
  ASSERT_TRUE(put_wifi(j, w) == Put::PUT_RESULT_UNCHANGED);
  ASSERT_TRUE(!j.dirty());
  ASSERT_TRUE(j.commit(flash));
  ASSERT_TRUE(flash.writes == writes);
}

TEST(XR806PreferenceJournal, RecordsAreKeptAndReplaced) {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  const uint32_t a = 7, b = 0xDEADBEEF;
  ASSERT_TRUE(j.put(1, reinterpret_cast<const uint8_t *>(&a), sizeof(a)) == Put::PUT_RESULT_CHANGED);
  ASSERT_TRUE(j.put(2, reinterpret_cast<const uint8_t *>(&b), sizeof(b)) == Put::PUT_RESULT_CHANGED);
  const uint16_t a2 = 9;  // Same key, different length replaces the record.
  ASSERT_TRUE(j.put(1, reinterpret_cast<const uint8_t *>(&a2), sizeof(a2)) == Put::PUT_RESULT_CHANGED);
  ASSERT_TRUE(j.commit(flash));
  Journal r;
  ASSERT_TRUE(r.load(flash));
  uint32_t out32 = 0;
  uint16_t out16 = 0;
  ASSERT_TRUE(!r.get(1, reinterpret_cast<uint8_t *>(&out32), sizeof(out32)));  // Wrong length is not returned.
  ASSERT_TRUE(r.get(1, reinterpret_cast<uint8_t *>(&out16), sizeof(out16)) && out16 == 9);
  ASSERT_TRUE(r.get(2, reinterpret_cast<uint8_t *>(&out32), sizeof(out32)) && out32 == b);
  ASSERT_TRUE(r.used() == 2 * Journal::RECORD_HEADER_SIZE + sizeof(uint16_t) + sizeof(uint32_t));
}

TEST(XR806PreferenceJournal, FullStoreRejectsWithoutLosingData) {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  std::array<uint8_t, 1000> big{};
  big.fill(0xAB);
  ASSERT_TRUE(j.put(1, big.data(), big.size()) == Put::PUT_RESULT_CHANGED);
  ASSERT_TRUE(j.put(2, big.data(), big.size()) == Put::PUT_RESULT_CHANGED);
  ASSERT_TRUE(j.put(3, big.data(), big.size()) == Put::PUT_RESULT_FULL);
  ASSERT_TRUE(j.put(4, big.data(), SIZE) == Put::PUT_RESULT_FULL);
  std::array<uint8_t, 1000> out{};
  ASSERT_TRUE(j.get(1, out.data(), out.size()) && out == big);
  ASSERT_TRUE(j.get(2, out.data(), out.size()) && out == big);
}

TEST(XR806PreferenceJournal, CorruptSlotsAreIgnored) {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  put_wifi(j, make_wifi("good", "goodgood"));
  ASSERT_TRUE(j.commit(flash));
  put_wifi(j, make_wifi("newer", "newernewer"));
  ASSERT_TRUE(j.commit(flash));
  flash.slots[1][40] ^= 0x10;  // Bit flip in the newer snapshot.
  Journal r;
  ASSERT_TRUE(r.load(flash));
  Wifi w{};
  ASSERT_TRUE(load_wifi(r, &w) && std::strcmp(w.ssid, "good") == 0);
  flash.slots[0][20] ^= 0x01;
  Journal empty;
  ASSERT_TRUE(!empty.load(flash));
  ASSERT_TRUE(empty.used() == 0);
}

TEST(XR806PreferenceJournal, DamagedRecordLengthIsRejected) {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  const uint32_t v = 1;
  j.put(5, reinterpret_cast<const uint8_t *>(&v), sizeof(v));
  ASSERT_TRUE(j.commit(flash));
  // Make the record length point past the end, then fix the CRC so only the walk can catch it.
  auto &slot = flash.slots[0];
  const uint16_t bad_len = 0x7FFF;
  std::memcpy(slot.data() + Journal::HEADER_SIZE + 4, &bad_len, sizeof(bad_len));
  PreferenceSnapshotHeader header;
  std::memcpy(&header, slot.data(), sizeof(header));
  header.checksum = preference_snapshot_checksum(slot.data(), header.used);
  std::memcpy(slot.data(), &header, sizeof(header));
  Journal r;
  ASSERT_TRUE(!r.load(flash));
}

TEST(XR806PreferenceJournal, GenerationWraps) {
  ASSERT_TRUE(preference_generation_newer(0, 0xFFFFFFFF));
  ASSERT_TRUE(!preference_generation_newer(0xFFFFFFFF, 0));
  ASSERT_TRUE(preference_generation_newer(2, 1));
  ASSERT_TRUE(!preference_generation_newer(1, 1));
}

TEST(XR806PreferenceJournal, ClearCommitsEmptyAndErasesOldSlot) {
  FakeFlash flash;
  Journal j;
  j.load(flash);
  put_wifi(j, make_wifi("home", "secret123"));
  ASSERT_TRUE(j.commit(flash));
  ASSERT_TRUE(j.clear(flash));
  ASSERT_TRUE(flash.erases == 1);
  ASSERT_TRUE(!flash.formatted[0]);  // The slot that held the credentials is gone.
  Journal r;
  ASSERT_TRUE(r.load(flash));
  Wifi w{};
  ASSERT_TRUE(!load_wifi(r, &w));
  ASSERT_TRUE(r.used() == 0);
}

}  // namespace esphome::xr806::testing
