#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
// Vendor WLAN driver (libxrwireless.a). xr_drv_common is the driver's xradio_common instance.
extern char xr_drv_common[];
int xr_msg_mib_read(void *hw_priv, uint16_t mib_id, void *buf, size_t buf_size, size_t arg_size);
}

namespace esphome::xr806 {

/// Reads up to 16 words of WLAN core memory through the stock firmware's register debug request (MIB 9, as used
/// by DBG_BH_FW_RW_REGS): {u16 flag, u16 size, u32 address[16]}, flag bit 0 clear for a read; the firmware returns
/// the values in place of the addresses. Addresses are those of the WLAN core. Returns the driver's error code.
inline int wlan_read_words(uint32_t address, uint32_t *words, uint32_t count) {
  static constexpr uint16_t MIB_FW_RW_REGS = 9;
  static constexpr uint32_t MAX_WORDS = 16;
  struct {
    uint16_t flag;
    uint16_t size;
    uint32_t value[MAX_WORDS];
  } request{};
  if (count == 0 || count > MAX_WORDS)
    return -1;
  request.size = static_cast<uint16_t>(4 + 4 * count);
  for (uint32_t i = 0; i < count; ++i)
    request.value[i] = address + 4 * i;
  const int err = xr_msg_mib_read(xr_drv_common, MIB_FW_RW_REGS, &request, sizeof(request), request.size);
  if (err == 0)
    memcpy(words, request.value, 4 * count);
  return err;
}

}  // namespace esphome::xr806
