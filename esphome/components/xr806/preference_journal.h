#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Allocation-free preference store over two flash slots. It has no dependency on the
// XRadio SDK or the ESPHome core, so it is tested on the host with simulated flash.
//
// A snapshot is a 16-byte header followed by packed records {key32, len16, data[len]}.
// The CRC covers the header fields and all records. Each commit writes the complete
// snapshot to the slot that does not hold the current one and reads it back, so an
// interrupted write leaves the previous snapshot intact and loadable.
namespace esphome::xr806 {

struct PreferenceSnapshotHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t used;
  uint32_t generation;
  uint32_t checksum;
};
static_assert(sizeof(PreferenceSnapshotHeader) == 16);

/// CRC-32 (IEEE 802.3) over the header without its checksum field, then the records.
inline uint32_t preference_snapshot_checksum(const uint8_t *data, size_t used) {
  uint32_t crc = 0xFFFFFFFF;
  auto update = [&crc](const uint8_t *bytes, size_t n) {
    for (size_t j = 0; j < n; j++) {
      crc ^= bytes[j];
      for (unsigned bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  };
  update(data, offsetof(PreferenceSnapshotHeader, checksum));
  update(data + sizeof(PreferenceSnapshotHeader), used);
  return ~crc;
}

/// Generations are compared modulo 2^32, so 0 after 0xFFFFFFFF counts as newer.
inline bool preference_generation_newer(uint32_t candidate, uint32_t current) {
  return static_cast<int32_t>(candidate - current) > 0;
}

/// Storage needs: bool read(uint8_t slot, uint8_t *data, size_t size),
/// bool write(uint8_t slot, const uint8_t *data, size_t size) and bool erase(uint8_t slot).
template<size_t SIZE> class PreferenceJournal {
 public:
  static constexpr uint32_t MAGIC = 0x58525046;  // "FPRX" in little-endian byte order
  static constexpr uint16_t VERSION = 1;
  static constexpr size_t HEADER_SIZE = sizeof(PreferenceSnapshotHeader);
  static constexpr size_t RECORD_HEADER_SIZE = 6;
  static_assert(SIZE > HEADER_SIZE && SIZE - HEADER_SIZE <= UINT16_MAX);

  enum class PutResult : uint8_t { PUT_RESULT_UNCHANGED, PUT_RESULT_CHANGED, PUT_RESULT_FULL };

  /// Loads the newer valid snapshot. Returns false when neither slot holds one; the store is then empty.
  template<typename Storage> bool load(Storage &storage) {
    this->used_ = 0;
    this->generation_ = 0;
    this->active_slot_ = 1;  // With no snapshot, the first commit goes to slot 0.
    this->dirty_ = false;
    bool found = false;
    for (uint8_t slot = 0; slot < 2; slot++) {
      PreferenceSnapshotHeader header{};
      if (!storage.read(slot, this->scratch_, SIZE) || !valid_(this->scratch_, &header))
        continue;
      if (found && !preference_generation_newer(header.generation, this->generation_))
        continue;
      std::memcpy(this->data_, this->scratch_, SIZE);
      this->used_ = header.used;
      this->generation_ = header.generation;
      this->active_slot_ = slot;
      found = true;
    }
    return found;
  }

  bool get(uint32_t key, uint8_t *dest, size_t len) const {
    const size_t offset = this->find_(key);
    if (offset == NOT_FOUND || record_len_(this->data_ + offset) != len)
      return false;
    std::memcpy(dest, this->data_ + offset + RECORD_HEADER_SIZE, len);
    return true;
  }

  /// Stores a record in RAM. Unchanged data does not mark the store dirty, so it causes no flash write.
  PutResult put(uint32_t key, const uint8_t *src, size_t len) {
    if (len == 0 || len > SIZE - HEADER_SIZE - RECORD_HEADER_SIZE)
      return PutResult::PUT_RESULT_FULL;
    const size_t existing = this->find_(key);
    if (existing != NOT_FOUND && record_len_(this->data_ + existing) == len &&
        std::memcmp(this->data_ + existing + RECORD_HEADER_SIZE, src, len) == 0)
      return PutResult::PUT_RESULT_UNCHANGED;

    // Build the new record set separately, so a record that does not fit leaves the old set untouched.
    std::memset(this->scratch_, 0, SIZE);
    size_t out = HEADER_SIZE;
    const size_t end = HEADER_SIZE + this->used_;
    for (size_t cursor = HEADER_SIZE; cursor < end;) {
      const size_t bytes = RECORD_HEADER_SIZE + record_len_(this->data_ + cursor);
      if (cursor != existing) {
        std::memcpy(this->scratch_ + out, this->data_ + cursor, bytes);
        out += bytes;
      }
      cursor += bytes;
    }
    if (RECORD_HEADER_SIZE + len > SIZE - out)
      return PutResult::PUT_RESULT_FULL;
    const auto record_len = static_cast<uint16_t>(len);
    std::memcpy(this->scratch_ + out, &key, sizeof(key));
    std::memcpy(this->scratch_ + out + 4, &record_len, sizeof(record_len));
    std::memcpy(this->scratch_ + out + RECORD_HEADER_SIZE, src, len);
    out += RECORD_HEADER_SIZE + len;

    std::memcpy(this->data_, this->scratch_, SIZE);
    this->used_ = out - HEADER_SIZE;
    this->dirty_ = true;
    return PutResult::PUT_RESULT_CHANGED;
  }

  /// Writes pending changes to the other slot and verifies them. On failure the
  /// previous snapshot stays current and the changes stay pending.
  template<typename Storage> bool commit(Storage &storage) {
    if (!this->dirty_)
      return true;
    const uint8_t target = this->active_slot_ ^ 1U;
    const PreferenceSnapshotHeader header{MAGIC, VERSION, static_cast<uint16_t>(this->used_), this->generation_ + 1, 0};
    std::memcpy(this->data_, &header, sizeof(header));
    const uint32_t checksum = preference_snapshot_checksum(this->data_, this->used_);
    std::memcpy(this->data_ + offsetof(PreferenceSnapshotHeader, checksum), &checksum, sizeof(checksum));
    if (!storage.write(target, this->data_, SIZE))
      return false;
    if (!storage.read(target, this->scratch_, SIZE) || std::memcmp(this->scratch_, this->data_, SIZE) != 0)
      return false;
    this->active_slot_ = target;
    this->generation_ = header.generation;
    this->dirty_ = false;
    return true;
  }

  /// Commits an empty snapshot, then erases the older slot, which may still hold credentials.
  /// An interruption leaves either the old state or the empty one.
  template<typename Storage> bool clear(Storage &storage) {
    this->used_ = 0;
    std::memset(this->data_, 0, SIZE);
    this->dirty_ = true;
    if (!this->commit(storage))
      return false;
    return storage.erase(this->active_slot_ ^ 1U);
  }

  bool dirty() const { return this->dirty_; }
  size_t used() const { return this->used_; }
  uint32_t generation() const { return this->generation_; }
  uint8_t active_slot() const { return this->active_slot_; }

 protected:
  static constexpr size_t NOT_FOUND = SIZE;

  static uint16_t record_len_(const uint8_t *record) {
    uint16_t len;
    std::memcpy(&len, record + 4, sizeof(len));
    return len;
  }

  size_t find_(uint32_t key) const {
    const size_t end = HEADER_SIZE + this->used_;
    for (size_t cursor = HEADER_SIZE; cursor < end; cursor += RECORD_HEADER_SIZE + record_len_(this->data_ + cursor)) {
      uint32_t stored_key;
      std::memcpy(&stored_key, this->data_ + cursor, sizeof(stored_key));
      if (stored_key == key)
        return cursor;
    }
    return NOT_FOUND;
  }

  static bool valid_(const uint8_t *data, PreferenceSnapshotHeader *out) {
    PreferenceSnapshotHeader header;
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != MAGIC || header.version != VERSION || header.used > SIZE - HEADER_SIZE)
      return false;
    // Walk the records so a damaged length can never make a lookup leave the buffer.
    const size_t end = HEADER_SIZE + header.used;
    size_t cursor = HEADER_SIZE;
    while (cursor < end) {
      if (end - cursor < RECORD_HEADER_SIZE)
        return false;
      const uint16_t len = record_len_(data + cursor);
      if (len == 0 || len > end - cursor - RECORD_HEADER_SIZE)
        return false;
      cursor += RECORD_HEADER_SIZE + len;
    }
    if (preference_snapshot_checksum(data, header.used) != header.checksum)
      return false;
    *out = header;
    return true;
  }

  alignas(4) uint8_t data_[SIZE]{};
  alignas(4) uint8_t scratch_[SIZE]{};
  size_t used_{0};
  uint32_t generation_{0};
  uint8_t active_slot_{1};
  bool dirty_{false};
};

}  // namespace esphome::xr806
