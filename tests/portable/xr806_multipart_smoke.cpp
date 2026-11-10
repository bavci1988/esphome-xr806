#include "esphome/components/web_server_xr806/multipart_stream.h"

#include <cassert>
#include <cstdint>
#include <string>

using esphome::web_server_xr806::MultipartStream;

static const std::string kStart =
    "--xr806-boundary\r\nContent-Disposition: form-data; name=\"update\"; "
    "filename=\"firmware.ota.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n";
static const std::string kEnd = "\r\n--xr806-boundary--\r\n";

static void verify_split_upload(size_t chunk_size) {
  const std::string firmware = std::string("AWIH\0XZ1\0A\0B", 11) + "\r\n--xr806-bounda";
  const std::string payload = kStart + firmware + kEnd;
  MultipartStream parser("xr806-boundary", payload.size());
  std::string collected;
  unsigned completes = 0;
  auto sink = [&](uint8_t *data, size_t size, bool final) {
    if (final) ++completes;
    else collected.append(reinterpret_cast<const char *>(data), size);
    return true;
  };
  for (size_t off = 0; off < payload.size(); off += chunk_size) {
    const size_t len = std::min(chunk_size, payload.size() - off);
    assert(parser.feed(reinterpret_cast<const uint8_t *>(payload.data() + off), len, sink));
  }
  assert(parser.complete());
  assert(parser.file_bytes() == firmware.size());
  assert(parser.filename() == "firmware.ota.bin");
  assert(collected == firmware);
  assert(completes == 1);
}

int main() {
  assert(MultipartStream::boundary_from_content_type("multipart/form-data; boundary=xr806-boundary") ==
         "xr806-boundary");
  assert(MultipartStream::boundary_from_content_type("multipart/form-data; boundary=\"xr806-boundary\"") ==
         "xr806-boundary");
  assert(MultipartStream::boundary_from_content_type("multipart/form-data; boundary=bad\r\nEvil") == "");
  for (size_t step : {1u, 2u, 3u, 7u, 17u, 511u})
    verify_split_upload(step);

  const std::string valid = kStart + "firmware" + kEnd;
  const auto safe_sink = [](uint8_t *, size_t, bool) { return true; };
  {
    MultipartStream parser("xr806-boundary", valid.size());
    assert(parser.feed(reinterpret_cast<const uint8_t *>(valid.data()), valid.size() - 1, safe_sink));
    assert(!parser.complete());
  }
  {
    MultipartStream parser("xr806-boundary", valid.size() + 1);
    assert(parser.feed(reinterpret_cast<const uint8_t *>(valid.data()), valid.size(), safe_sink));
    const uint8_t extra = 'X';
    assert(!parser.feed(&extra, 1, safe_sink));  // No early commit on invalid trailing bytes.
    assert(!parser.complete());
  }
  {
    const auto malformed = kStart + "firmware" + "\r\n--wrong--\r\n";
    MultipartStream parser("xr806-boundary", malformed.size());
    assert(!parser.feed(reinterpret_cast<const uint8_t *>(malformed.data()), malformed.size(), safe_sink));
    assert(parser.failed());
  }
  {
    const std::string wrong_name =
        "--xr806-boundary\r\nContent-Disposition: form-data; name=\"evil\"; "
        "filename=\"x.bin\"\r\n\r\nfirmware\r\n--xr806-boundary--\r\n";
    MultipartStream parser("xr806-boundary", wrong_name.size());
    assert(!parser.feed(reinterpret_cast<const uint8_t *>(wrong_name.data()), wrong_name.size(), safe_sink));
  }
}
