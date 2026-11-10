#include <gtest/gtest.h>

#include "esphome/components/web_server_xr806/multipart_stream.h"

#include <cstdint>
#include <string>

namespace esphome::web_server_xr806::testing {

static const std::string START_PART = "--xr806-boundary\r\nContent-Disposition: form-data; name=\"update\"; "
                                      "filename=\"firmware.ota.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n";
static const std::string END_PART = "\r\n--xr806-boundary--\r\n";

static void verify_split_upload(size_t chunk_size) {
  const std::string firmware = std::string("AWIH\0XZ1\0A\0B", 11) + "\r\n--xr806-bounda";
  const std::string payload = START_PART + firmware + END_PART;
  MultipartStream parser("xr806-boundary", payload.size());
  std::string collected;
  unsigned completes = 0;
  auto sink = [&](uint8_t *data, size_t size, bool final) {
    if (final)
      ++completes;
    else
      collected.append(reinterpret_cast<const char *>(data), size);
    return true;
  };
  for (size_t off = 0; off < payload.size(); off += chunk_size) {
    const size_t len = std::min(chunk_size, payload.size() - off);
    ASSERT_TRUE(parser.feed(reinterpret_cast<const uint8_t *>(payload.data() + off), len, sink));
  }
  ASSERT_TRUE(parser.complete());
  ASSERT_TRUE(parser.file_bytes() == firmware.size());
  ASSERT_TRUE(parser.filename() == "firmware.ota.bin");
  ASSERT_TRUE(collected == firmware);
  ASSERT_TRUE(completes == 1);
}

TEST(XR806MultipartStream, ParsesAndRejects) {
  ASSERT_TRUE(MultipartStream::boundary_from_content_type("multipart/form-data; boundary=xr806-boundary") ==
              "xr806-boundary");
  ASSERT_TRUE(MultipartStream::boundary_from_content_type("multipart/form-data; boundary=\"xr806-boundary\"") ==
              "xr806-boundary");
  ASSERT_TRUE(MultipartStream::boundary_from_content_type("multipart/form-data; boundary=bad\r\nEvil") == "");
  for (size_t step : {1u, 2u, 3u, 7u, 17u, 511u})
    verify_split_upload(step);

  const std::string valid = START_PART + "firmware" + END_PART;
  const auto safe_sink = [](uint8_t *, size_t, bool) { return true; };
  {
    MultipartStream parser("xr806-boundary", valid.size());
    ASSERT_TRUE(parser.feed(reinterpret_cast<const uint8_t *>(valid.data()), valid.size() - 1, safe_sink));
    ASSERT_TRUE(!parser.complete());
  }
  {
    MultipartStream parser("xr806-boundary", valid.size() + 1);
    ASSERT_TRUE(parser.feed(reinterpret_cast<const uint8_t *>(valid.data()), valid.size(), safe_sink));
    const uint8_t extra = 'X';
    ASSERT_TRUE(!parser.feed(&extra, 1, safe_sink));  // No early commit on invalid trailing bytes.
    ASSERT_TRUE(!parser.complete());
  }
  {
    const auto malformed = START_PART + "firmware" + "\r\n--wrong--\r\n";
    MultipartStream parser("xr806-boundary", malformed.size());
    ASSERT_TRUE(!parser.feed(reinterpret_cast<const uint8_t *>(malformed.data()), malformed.size(), safe_sink));
    ASSERT_TRUE(parser.failed());
  }
  {
    const std::string wrong_name = "--xr806-boundary\r\nContent-Disposition: form-data; name=\"evil\"; "
                                   "filename=\"x.bin\"\r\n\r\nfirmware\r\n--xr806-boundary--\r\n";
    MultipartStream parser("xr806-boundary", wrong_name.size());
    ASSERT_TRUE(!parser.feed(reinterpret_cast<const uint8_t *>(wrong_name.data()), wrong_name.size(), safe_sink));
  }
  // Browser-renamed downloads and other part types are accepted.
  {
    const std::string renamed = "--xr806-boundary\r\nContent-Disposition: form-data; name=\"update\"; "
                                "filename=\"firmware (1).ota\"\r\nContent-Type: application/x-ota\r\n\r\n"
                                "firmware" +
                                END_PART;
    MultipartStream parser("xr806-boundary", renamed.size());
    ASSERT_TRUE(parser.feed(reinterpret_cast<const uint8_t *>(renamed.data()), renamed.size(), safe_sink));
    ASSERT_TRUE(parser.complete());
    ASSERT_TRUE(parser.filename() == "firmware (1).ota");
  }
  // Control characters in the name, an empty type or a second part header are still rejected.
  for (const char *headers : {"filename=\"a\tb.ota\"\r\n\r\n", "filename=\"a.ota\"\r\nContent-Type: \r\n\r\n",
                              "filename=\"a.ota\"\r\nContent-Type: a/b\r\nX-Extra: 1\r\n\r\n"}) {
    const std::string bad = std::string("--xr806-boundary\r\nContent-Disposition: form-data; name=\"update\"; ") +
                            headers + "firmware" + END_PART;
    MultipartStream parser("xr806-boundary", bad.size());
    ASSERT_TRUE(!parser.feed(reinterpret_cast<const uint8_t *>(bad.data()), bad.size(), safe_sink));
  }
}

}  // namespace esphome::web_server_xr806::testing
