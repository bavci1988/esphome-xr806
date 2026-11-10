// Dependency-free host sanity checks for the native XR806 HTTP parser.
#include "esphome/components/xr806/http_request_parser.h"

#include <cassert>
#include <cstring>

using esphome::xr806::HttpMethod;
using esphome::xr806::HttpParseStatus;
using esphome::xr806::HttpRequestView;
using esphome::xr806::parse_http_request;

static HttpParseStatus check(const char *request, size_t capacity = 1024) {
  HttpRequestView view{};
  return parse_http_request(request, strlen(request), capacity, &view);
}

int main() {
  {
    const char request[] = "GET /health?x=1 HTTP/1.1\r\nHost: example\r\n\r\n";
    HttpRequestView view{};
    assert(parse_http_request(request, strlen(request), 1024, &view) == HttpParseStatus::COMPLETE);
    assert(view.method == HttpMethod::GET);
    assert(view.uri_length == 11);
    assert(memcmp(view.uri, "/health?x=1", 11) == 0);
    assert(view.body_length == 0);
  }
  {
    const char request[] = "POST /wifisave HTTP/1.1\r\ncontent-length: 7\r\n\r\nssid=ok";
    HttpRequestView view{};
    assert(parse_http_request(request, strlen(request), 1024, &view) == HttpParseStatus::COMPLETE);
    assert(view.method == HttpMethod::POST);
    assert(view.body_length == 7);
    assert(memcmp(view.body, "ssid=ok", 7) == 0);
  }
  assert(check("POST /save HTTP/1.1\r\nContent-Length: 4\r\n\r\nxx") == HttpParseStatus::INCOMPLETE);
  assert(check("GET / HTTP/1.1\r\nHost: test\r\n") == HttpParseStatus::INCOMPLETE);
  assert(check("POST /save HTTP/1.1\r\n\r\n") == HttpParseStatus::BAD_REQUEST);
  assert(check("POST /save HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == HttpParseStatus::BAD_REQUEST);
  assert(check("POST /save HTTP/1.1\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\nxx") ==
         HttpParseStatus::BAD_REQUEST);
  assert(check("POST /save HTTP/1.1\r\nContent-Length: 999\r\n\r\n") == HttpParseStatus::TOO_LARGE);
  assert(check("GET / HTTP/1.1\r\nContent-Length: 1\r\n\r\nx") == HttpParseStatus::BAD_REQUEST);
  assert(check("GET / HTTP/2.0\r\n\r\n") == HttpParseStatus::BAD_REQUEST);
  assert(check("OPTIONS / HTTP/1.1\r\n\r\n") == HttpParseStatus::METHOD_NOT_ALLOWED);
  assert(check("GET / HTTP/1.1\r\nX: 1\r\n  Y: 2\r\n\r\n") == HttpParseStatus::BAD_REQUEST);
  assert(check("GET / HTTP/1.1\r\n\r\n", 13) == HttpParseStatus::BAD_REQUEST);
}
