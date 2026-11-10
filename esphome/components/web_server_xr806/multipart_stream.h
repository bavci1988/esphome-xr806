#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace esphome::web_server_xr806 {

/// Single-file, bounded streaming multipart parser for the standard /update
/// endpoint. No flash commit is signalled until the entire HTTP body and final
/// MIME delimiter are present. No firmware bytes are retained in RAM long-term.
class MultipartStream {
 public:
  using Sink = std::function<bool(uint8_t *data, size_t size, bool final)>;

  static std::string boundary_from_content_type(std::string_view value) {
    constexpr std::string_view PREFIX = "multipart/form-data;";
    if (value.size() < PREFIX.size() || value.substr(0, PREFIX.size()) != PREFIX)
      return {};
    auto pos = value.find("boundary=");
    if (pos == value.npos)
      return {};
    pos += 9;
    const bool quoted = pos < value.size() && value[pos] == '"';
    if (quoted)
      ++pos;
    const size_t begin = pos;
    while (pos < value.size()) {
      const unsigned char ch = static_cast<unsigned char>(value[pos]);
      if ((quoted && ch == '"') || (!quoted && (ch == ';' || std::isspace(ch))))
        break;
      if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '+' || ch == '/'))
        return {};
      ++pos;
    }
    if (pos == begin || pos - begin > 70 || (quoted && (pos >= value.size() || value[pos] != '"')))
      return {};
    return std::string(value.substr(begin, pos - begin));
  }

  MultipartStream(std::string boundary, size_t total)
      : boundary_(std::move(boundary)), total_(total),
        first_line_("--" + boundary_ + "\r\n"), closing_("\r\n--" + boundary_ + "--") {}

  bool feed(const uint8_t *data, size_t size, const Sink &sink) {
    if (failed_ || done_ || data == nullptr || size > total_ - received_)
      return this->fail_();
    for (size_t i = 0; i < size; ++i) {
      ++received_;
      buffer_.push_back(static_cast<char>(data[i]));
      if (phase_ == Phase::FIRST_LINE) {
        if (buffer_.size() > first_line_.size() || first_line_.compare(0, buffer_.size(), buffer_) != 0)
          return this->fail_();
        if (buffer_.size() == first_line_.size()) {
          buffer_.clear();
          phase_ = Phase::HEADERS;
        }
      } else if (phase_ == Phase::HEADERS) {
        if (buffer_.size() > 1024)
          return this->fail_();
        if (buffer_.size() >= 4 && buffer_.compare(buffer_.size() - 4, 4, "\r\n\r\n") == 0) {
          if (!this->validate_headers_())
            return this->fail_();
          buffer_.clear();
          phase_ = Phase::BODY;
        }
      } else if (phase_ == Phase::BODY) {
        // Retain the maximum possible delimiter suffix; stream everything
        // else in small blocks without buffering the entire XZ firmware.
        const size_t keep = closing_.size() + 2;
        if (buffer_.size() > keep + 1024) {
          const size_t output = buffer_.size() - keep;
          if (!sink(reinterpret_cast<uint8_t *>(buffer_.data()), output, false))
            return this->fail_();
          file_bytes_ += output;
          buffer_.erase(0, output);
        }
      }
    }
    if (received_ != total_)
      return true;
    if (phase_ != Phase::BODY)
      return this->fail_();
    size_t trailer = closing_.size();
    if (buffer_.size() >= trailer + 2 &&
        buffer_.compare(buffer_.size() - 2, 2, "\r\n") == 0)
      trailer += 2;
    if (buffer_.size() < trailer ||
        buffer_.compare(buffer_.size() - trailer, closing_.size(), closing_) != 0)
      return this->fail_();
    const size_t tail = buffer_.size() - trailer;
    if (file_bytes_ + tail == 0)
      return this->fail_();
    if (tail && !sink(reinterpret_cast<uint8_t *>(buffer_.data()), tail, false))
      return this->fail_();
    file_bytes_ += tail;
    buffer_.clear();
    // The caller's end() now validates XZ/image and commits the alternate
    // firmware slot. This marker must never precede complete HTTP framing.
    if (!sink(nullptr, 0, true))
      return this->fail_();
    phase_ = Phase::DONE;
    done_ = true;
    return true;
  }

  bool complete() const { return done_ && received_ == total_; }
  bool failed() const { return failed_; }
  size_t file_bytes() const { return file_bytes_; }
  const std::string &filename() const { return filename_; }

 private:
  enum class Phase : uint8_t { FIRST_LINE, HEADERS, BODY, DONE };
  bool fail_() {
    failed_ = true;
    return false;
  }

  bool validate_headers_() {
    // Strict single-part upload. No unbounded embedded form fields, nested
    // multipart, filenames with directories, or Content-Transfer-Encoding.
    constexpr std::string_view DISPOSITION = "Content-Disposition: form-data;";
    const std::string_view headers{buffer_};
    const size_t first = headers.find("\r\n");
    if (first == headers.npos || headers.substr(0, DISPOSITION.size()) != DISPOSITION)
      return false;
    const auto disposition = headers.substr(0, first);
    if (disposition.find("; name=\"update\"") == disposition.npos)
      return false;
    const auto fn = disposition.find("; filename=\"");
    if (fn == disposition.npos)
      return false;
    const size_t start = fn + 12;
    const size_t end = disposition.find('"', start);
    if (end == disposition.npos || end == start || end - start > 96)
      return false;
    filename_ = std::string(disposition.substr(start, end - start));
    for (char c : filename_) {
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
        return false;
    }
    // The only optional extra header we permit is Content-Type for the blob.
    auto remainder = headers.substr(first + 2);
    if (remainder == "\r\n")
      return true;
    constexpr std::string_view CONTENT_TYPE = "Content-Type: application/octet-stream\r\n\r\n";
    return remainder == CONTENT_TYPE;
  }

  std::string boundary_;
  size_t total_{0};
  std::string first_line_;
  std::string closing_;
  std::string buffer_;
  std::string filename_;
  Phase phase_{Phase::FIRST_LINE};
  size_t received_{0};
  size_t file_bytes_{0};
  bool done_{false};
  bool failed_{false};
};

}  // namespace esphome::web_server_xr806
