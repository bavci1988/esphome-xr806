#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// A bounded, dependency-free request-head parser shared by the XR806 native
// captive portal and the future AsyncWebServer-compatible REST/SSE transport.
// No heap allocations, lwIP types, HTTP keep-alive, or chunked request bodies.
namespace esphome::xr806 {

enum class HttpParseStatus : uint8_t {
  INCOMPLETE,
  COMPLETE,
  BAD_REQUEST,
  TOO_LARGE,
  METHOD_NOT_ALLOWED,
};

enum class HttpMethod : uint8_t { GET, POST };

struct HttpRequestView {
  HttpMethod method{HttpMethod::GET};
  const char *uri{nullptr};
  size_t uri_length{0};
  const char *body{nullptr};
  size_t body_length{0};
  size_t header_length{0};
};

inline bool http_ascii_equal(const char *a, size_t length, const char *b) {
  if (strlen(b) != length)
    return false;
  for (size_t i = 0; i < length; i++) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z')
      x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z')
      y = static_cast<char>(y - 'A' + 'a');
    if (x != y)
      return false;
  }
  return true;
}

inline HttpParseStatus parse_http_request(const char *data, size_t length, size_t capacity,
                                          HttpRequestView *out) {
  if (data == nullptr || out == nullptr || length > capacity)
    return HttpParseStatus::BAD_REQUEST;

  size_t header_length = 0;
  for (size_t i = 0; i + 3 < length; i++) {
    if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
      header_length = i + 4;
      break;
    }
  }
  if (header_length == 0)
    return length >= capacity ? HttpParseStatus::TOO_LARGE : HttpParseStatus::INCOMPLETE;

  // Require one start line, explicit HTTP/1.x version, and no NUL anywhere
  // in the header (including the request target).
  if (memchr(data, '\0', header_length) != nullptr)
    return HttpParseStatus::BAD_REQUEST;

  size_t line_end = 0;
  while (line_end + 1 < header_length) {
    if (data[line_end] == '\r' && data[line_end + 1] == '\n')
      break;
    line_end++;
  }
  if (line_end < 12 || line_end + 1 >= header_length)
    return HttpParseStatus::BAD_REQUEST;

  size_t method_end = 0;
  while (method_end < line_end && data[method_end] != ' ')
    method_end++;
  if (method_end == line_end)
    return HttpParseStatus::BAD_REQUEST;

  HttpMethod method;
  if (method_end == 3 && memcmp(data, "GET", 3) == 0) {
    method = HttpMethod::GET;
  } else if (method_end == 4 && memcmp(data, "POST", 4) == 0) {
    method = HttpMethod::POST;
  } else {
    return HttpParseStatus::METHOD_NOT_ALLOWED;
  }

  const size_t uri_start = method_end + 1;
  size_t uri_end = uri_start;
  while (uri_end < line_end && data[uri_end] != ' ')
    uri_end++;
  if (uri_start == uri_end || uri_end >= line_end || data[uri_start] != '/')
    return HttpParseStatus::BAD_REQUEST;

  const size_t uri_length = uri_end - uri_start;
  if (uri_length > 512)
    return HttpParseStatus::TOO_LARGE;
  const size_t version_start = uri_end + 1;
  if (line_end - version_start != 8 ||
      (memcmp(data + version_start, "HTTP/1.1", 8) != 0 &&
       memcmp(data + version_start, "HTTP/1.0", 8) != 0))
    return HttpParseStatus::BAD_REQUEST;

  bool has_content_length = false;
  size_t body_length = 0;
  size_t cursor = line_end + 2;
  const size_t last_header = header_length - 2;
  while (cursor < last_header) {
    const size_t begin = cursor;
    while (cursor + 1 < last_header && !(data[cursor] == '\r' && data[cursor + 1] == '\n'))
      cursor++;
    if (cursor + 1 >= last_header)
      return HttpParseStatus::BAD_REQUEST;
    const size_t end = cursor;
    size_t colon = begin;
    while (colon < end && data[colon] != ':')
      colon++;
    if (colon == begin || colon == end)
      return HttpParseStatus::BAD_REQUEST;
    if (data[begin] == ' ' || data[begin] == '\t')
      return HttpParseStatus::BAD_REQUEST;  // No obsolete folded headers.

    if (http_ascii_equal(data + begin, colon - begin, "Transfer-Encoding"))
      return HttpParseStatus::BAD_REQUEST;  // Chunked parsing is not implemented.

    if (http_ascii_equal(data + begin, colon - begin, "Content-Length")) {
      if (has_content_length)
        return HttpParseStatus::BAD_REQUEST;  // Reject ambiguous duplicate lengths.
      has_content_length = true;
      size_t number = colon + 1;
      while (number < end && (data[number] == ' ' || data[number] == '\t'))
        number++;
      if (number == end)
        return HttpParseStatus::BAD_REQUEST;
      size_t parsed = 0;
      bool digits = false;
      for (; number < end && data[number] >= '0' && data[number] <= '9'; number++) {
        digits = true;
        parsed = parsed * 10 + static_cast<unsigned>(data[number] - '0');
        if (parsed > 512)
          return HttpParseStatus::TOO_LARGE;
      }
      while (number < end && (data[number] == ' ' || data[number] == '\t'))
        number++;
      if (!digits || number != end)
        return HttpParseStatus::BAD_REQUEST;
      body_length = parsed;
    }
    cursor += 2;
  }
  if (method == HttpMethod::POST && !has_content_length)
    return HttpParseStatus::BAD_REQUEST;
  if (method == HttpMethod::GET && body_length != 0)
    return HttpParseStatus::BAD_REQUEST;
  if (header_length + body_length > capacity)
    return HttpParseStatus::TOO_LARGE;
  if (length < header_length + body_length)
    return HttpParseStatus::INCOMPLETE;

  *out = {method, data + uri_start, uri_length, data + header_length, body_length, header_length};
  return HttpParseStatus::COMPLETE;
}

}  // namespace esphome::xr806
