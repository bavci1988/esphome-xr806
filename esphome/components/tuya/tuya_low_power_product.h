#pragma once

// Strict, bounded extraction of the product metadata used by the original
// TRV16 WXU/HC32 initialization. No JSON library or platform dependency.
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome::tuya {

struct TuyaLowPowerProductInfo {
  std::string pid;
  std::string version;
  int wake_gpio{-1};      // wk: logical WXU MCU-to-WXU wake source
  int mcu_wake_gpio{-1};  // mwk: vendor-encoded WXU-to-MCU wake line
  int sleep_mode{-1};     // sm: 1 standby request, 0 hibernation request
  bool parsed{false};

  static bool find_key(const std::string &s, const char *key, size_t &value) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t pos = s.find(needle);
    if (pos == std::string::npos)
      return false;
    size_t i = pos + needle.size();
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
      ++i;
    if (i >= s.size() || s[i] != ':')
      return false;
    ++i;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
      ++i;
    value = i;
    return i < s.size();
  }

  static bool get_string(const std::string &s, const char *key, std::string &value) {
    size_t i = 0;
    if (!find_key(s, key, i) || s[i] != '"')
      return false;
    ++i;
    const size_t begin = i;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' || static_cast<unsigned char>(s[i]) < 32)
        return false;  // No escape sequences in the verified TRV16 fields.
      ++i;
    }
    if (i >= s.size() || i == begin || i - begin > 48)
      return false;
    value = s.substr(begin, i - begin);
    return true;
  }

  static bool get_integer(const std::string &s, const char *key, int &value) {
    size_t i = 0;
    if (!find_key(s, key, i) || !std::isdigit(static_cast<unsigned char>(s[i])))
      return false;
    unsigned n = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      n = 10 * n + static_cast<unsigned>(s[i++] - '0');
      if (n > 255)
        return false;
    }
    value = static_cast<int>(n);
    return true;
  }

  bool parse(const uint8_t *data, size_t len) {
    *this = {};
    if (data == nullptr || len < 12 || len > 512)
      return false;
    const std::string s(reinterpret_cast<const char *>(data), len);
    if (s.front() != '{' || s.back() != '}')
      return false;
    parsed = get_string(s, "p", pid) && get_string(s, "v", version) && get_integer(s, "wk", wake_gpio) &&
             get_integer(s, "mwk", mcu_wake_gpio) && get_integer(s, "sm", sleep_mode) && sleep_mode <= 1;
    return parsed;
  }

  bool is_verified_trv16() const {
    return parsed && pid == "ihedujbuli6hsx9b" && wake_gpio == 20 && mcu_wake_gpio == 28;
  }
};

}  // namespace esphome::tuya
