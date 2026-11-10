#include "web_server_xr806.h"
#ifdef USE_XR806

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_WEBSERVER
#include "esphome/components/web_server/web_server.h"
#endif

namespace esphome::web_server_xr806 {

ESPHOME_LOG_TAG(TAG, "web_server_xr806");

static bool equals_ci(const std::string &lhs, const char *rhs) {
  const size_t len = std::strlen(rhs);
  if (lhs.size() != len)
    return false;
  for (size_t i = 0; i < len; ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
      return false;
  }
  return true;
}

static const char *reason_phrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 202:
      return "Accepted";
    case 204:
      return "No Content";
    case 302:
      return "Found";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 408:
      return "Request Timeout";
    case 411:
      return "Length Required";
    case 413:
      return "Content Too Large";
    case 415:
      return "Unsupported Media Type";
    case 431:
      return "Request Header Fields Too Large";
    case 503:
      return "Service Unavailable";
    default:
      return "Response";
  }
}

void AsyncWebServerResponse::addHeader(const char *name, const char *value) {
  if (name != nullptr && value != nullptr)
    this->headers_.emplace_back(name, value);
}

void AsyncResponseStream::print(float value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value));
  this->text_.append(buf);
}
void AsyncResponseStream::print(int value) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%d", value);
  this->text_.append(buf);
}
void AsyncResponseStream::printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int count = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (count > 0 && count <= 8192) {
    const size_t offset = this->text_.size();
    this->text_.resize(offset + static_cast<size_t>(count) + 1);
    std::vsnprintf(this->text_.data() + offset, static_cast<size_t>(count) + 1, format, args);
    this->text_.resize(offset + static_cast<size_t>(count));
  }
  va_end(args);
}

AsyncWebServerRequest::AsyncWebServerRequest(http_method method, std::string uri, std::string query, std::string body,
                                             std::vector<std::pair<std::string, std::string>> headers,
                                             std::unique_ptr<socket::Socket> *socket)
    : method_(method),
      uri_(std::move(uri)),
      query_(std::move(query)),
      body_(std::move(body)),
      headers_(std::move(headers)),
      socket_(socket) {}

std::string AsyncWebServerRequest::decode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
    } else if (s[i] == '%' && i + 2 < s.size() && digit(s[i + 1]) >= 0 && digit(s[i + 2]) >= 0) {
      const uint8_t decoded = static_cast<uint8_t>((digit(s[i + 1]) << 4) | digit(s[i + 2]));
      // Do not allow NUL in paths/form keys: HTTP handler strings are nul-terminated.
      if (decoded != 0)
        out.push_back(static_cast<char>(decoded));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}
StringRef AsyncWebServerRequest::url_to(std::span<char, URL_BUF_SIZE> buf) const {
  const std::string decoded = decode(this->uri_);
  const size_t n = std::min(decoded.size(), buf.size() - 1);
  std::memcpy(buf.data(), decoded.data(), n);
  buf[n] = 0;
  return StringRef(buf.data(), n);
}
size_t AsyncWebServerRequest::get_header_to(const char *name, std::span<char> buf) const {
  for (const auto &h : this->headers_) {
    if (!equals_ci(h.first, name))
      continue;
    if (h.second.size() >= buf.size())
      return SIZE_MAX;
    std::memcpy(buf.data(), h.second.data(), h.second.size());
    buf[h.second.size()] = 0;
    return h.second.size();
  }
  return 0;
}
bool AsyncWebServerRequest::hasHeader(const char *name) const {
  for (const auto &h : this->headers_)
    if (equals_ci(h.first, name))
      return true;
  return false;
}
bool AsyncWebServerRequest::find_arg(const std::string &query, const char *name, std::string *out) {
  for (size_t pos = 0; pos < query.size();) {
    const size_t end = query.find('&', pos);
    const size_t finish = end == std::string::npos ? query.size() : end;
    const size_t eq = query.find('=', pos);
    const size_t key_end = eq == std::string::npos || eq >= finish ? finish : eq;
    if (decode(query.substr(pos, key_end - pos)) == name) {
      if (out != nullptr)
        *out = key_end == finish ? "" : decode(query.substr(key_end + 1, finish - key_end - 1));
      return true;
    }
    pos = finish + 1;
  }
  return false;
}
bool AsyncWebServerRequest::hasArg(const char *name) const {
  return find_arg(this->query_, name, nullptr) || find_arg(this->body_, name, nullptr);
}
std::string AsyncWebServerRequest::arg(const char *name) const {
  std::string val;
  if (find_arg(this->body_, name, &val) || find_arg(this->query_, name, &val))
    return val;
  return {};
}
bool AsyncWebServerRequest::authenticate(const char *basic_auth_hash) const {
  if (basic_auth_hash == nullptr)
    return false;
  for (const auto &h : this->headers_) {
    if (!equals_ci(h.first, "Authorization"))
      continue;
    const std::string expected = std::string("Basic ") + basic_auth_hash;
    if (h.second.size() != expected.size())
      return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < expected.size(); ++i)
      diff |= static_cast<uint8_t>(h.second[i] ^ expected[i]);
    return diff == 0;
  }
  return false;
}
void AsyncWebServerRequest::requestAuthentication(const char * /*realm*/, bool /*is_digest*/) {
  auto *r = this->beginResponse(401, "text/plain", std::string("Authentication required"));
  r->addHeader("WWW-Authenticate", "Basic realm=\"ESPHome\"");
  this->send(r);
}
void AsyncWebServerRequest::redirect(const char *url) {
  auto *r = this->beginResponse(302, "text/plain", std::string("Redirecting"));
  r->addHeader("Location", url);
  this->send(r);
}
void AsyncWebServerRequest::send(int status, const char *type, const char *text) {
  this->send(new AsyncWebServerResponseContent(status, type, text == nullptr ? "" : text));
}
void AsyncWebServerRequest::send(AsyncWebServerResponse *response) { this->response_.reset(response); }
AsyncWebServerResponse *AsyncWebServerRequest::beginResponse(int status, const char *type) {
  return new AsyncWebServerResponseContent(status, type, "");
}
AsyncWebServerResponse *AsyncWebServerRequest::beginResponse(int status, const char *type, const std::string &text) {
  return new AsyncWebServerResponseContent(status, type, text);
}
AsyncWebServerResponse *AsyncWebServerRequest::beginResponse(int status, const char *type, const uint8_t *data,
                                                             size_t len) {
  return new AsyncWebServerResponseProgmem(status, type, data, len);
}
AsyncResponseStream *AsyncWebServerRequest::beginResponseStream(const char *type) {
  return new AsyncResponseStream(type);
}
DefaultHeaders &DefaultHeaders::Instance() {
  static DefaultHeaders headers;
  return headers;
}

bool AsyncWebServer::begin() {
  if (this->listener_ != nullptr)
    return true;
  auto socket = socket::socket_ip(SOCK_STREAM, IPPROTO_TCP);
  if (socket == nullptr) {
    ESP_LOGE(TAG, "HTTP socket allocation failed");
    return false;
  }
  int reuse = 1;
  socket->setsockopt(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_storage addr{};
  const socklen_t len = socket::set_sockaddr_any(reinterpret_cast<sockaddr *>(&addr), sizeof(addr), this->port_);
  if (len == 0 || socket->bind(reinterpret_cast<const sockaddr *>(&addr), len) != 0 ||
      socket->listen(MAX_CLIENTS) != 0 || socket->setblocking(false) != 0) {
    ESP_LOGE(TAG, "Cannot bind/listen on HTTP port %u (errno=%d)", this->port_, errno);
    return false;
  }
  this->listener_ = std::move(socket);
  ESP_LOGI(TAG, "Native XR806 HTTP listener on port %u", this->port_);
  return true;
}

void AsyncWebServer::end() {
  // Event streams own their sockets apart from the client list; close them too.
#ifdef USE_WEBSERVER
  for (auto *source = AsyncEventSource::first_source; source != nullptr; source = source->next_source())
    source->close_all();
#endif
  this->clients_.clear();
  this->stop_accepting();
}

void AsyncWebServer::stop_accepting() { this->listener_.reset(); }

bool AsyncWebServer::has_active_connections() const {
  if (!this->clients_.empty())
    return true;
#ifdef USE_WEBSERVER
  for (auto *source = AsyncEventSource::first_source; source != nullptr; source = source->next_source()) {
    if (!source->empty())
      return true;
  }
#endif
  return false;
}

void AsyncWebServer::dispatch_(Client &c) {
  // Split headers and body only after Content-Length bytes have arrived.
  const size_t line_end = c.input.find("\r\n");
  if (line_end == std::string::npos) {
    c.socket.reset();
    return;
  }
  const std::string request_line = c.input.substr(0, line_end);
  const size_t first_space = request_line.find(' ');
  const size_t second_space = request_line.find(' ', first_space == std::string::npos ? 0 : first_space + 1);
  if (first_space == std::string::npos || second_space == std::string::npos ||
      request_line.substr(second_space + 1) != "HTTP/1.1" && request_line.substr(second_space + 1) != "HTTP/1.0") {
    c.socket.reset();
    return;
  }
  const std::string verb = request_line.substr(0, first_space);
  const http_method method = verb == "GET"       ? HTTP_GET
                             : verb == "POST"    ? HTTP_POST
                             : verb == "OPTIONS" ? HTTP_OPTIONS
                                                 : HTTP_UNKNOWN;
  const std::string target = request_line.substr(first_space + 1, second_space - first_space - 1);
  if (target.empty() || target.front() != '/' || target.size() > AsyncWebServerRequest::URL_BUF_SIZE - 1) {
    c.socket.reset();
    return;
  }
  const size_t qm = target.find('?');
  const std::string uri = target.substr(0, qm);
  const std::string query = qm == std::string::npos ? "" : target.substr(qm + 1);
#ifdef USE_WEBSERVER_OTA
  const bool is_ota_upload = method == HTTP_POST && uri == "/update";
#endif
  std::vector<std::pair<std::string, std::string>> headers;
  size_t header_cursor = line_end + 2;
  size_t content_length = 0;
  bool has_length = false, unsupported_transfer = false, bad_length = false;
  if (c.header_end > MAX_HEADER_BYTES) {
    c.socket.reset();
    return;
  }
  while (header_cursor < c.header_end - 2) {
    const size_t end = c.input.find("\r\n", header_cursor);
    if (end == std::string::npos || end > c.header_end) {
      c.socket.reset();
      return;
    }
    const std::string line = c.input.substr(header_cursor, end - header_cursor);
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      c.socket.reset();
      return;
    }
    std::string name = line.substr(0, colon);
    const size_t start = line.find_first_not_of(" \t", colon + 1);
    std::string value = start == std::string::npos ? "" : line.substr(start);
    if (equals_ci(name, "Content-Length")) {
      if (has_length) {
        bad_length = true;
        break;
      }
      has_length = true;
      char *tail = nullptr;
      const unsigned long parsed = std::strtoul(value.c_str(), &tail, 10);
      if (value.empty() || value.front() < '0' || value.front() > '9' || tail == value.c_str() || *tail != 0 ||
#ifdef USE_WEBSERVER_OTA
          parsed > (is_ota_upload ? MAX_UPLOAD_BYTES : MAX_BODY_BYTES)
#else
          parsed > MAX_BODY_BYTES
#endif
      )
        bad_length = true;
      else
        content_length = static_cast<size_t>(parsed);
    }
    // XR806 handles only Content-Length framing. Even "identity" alongside
    // Content-Length is ambiguous across proxies; fail closed.
    if (equals_ci(name, "Transfer-Encoding"))
      unsupported_transfer = true;
    headers.emplace_back(std::move(name), std::move(value));
    header_cursor = end + 2;
  }
  c.dispatched = true;
  if (method == HTTP_UNKNOWN || unsupported_transfer || bad_length || (method == HTTP_POST && !has_length)) {
    c.response = std::make_unique<AsyncWebServerResponseContent>(method == HTTP_UNKNOWN ? 405
                                                                 : unsupported_transfer ? 415
                                                                 : bad_length           ? 413
                                                                                        : 411,
                                                                 "text/plain", "Unsupported HTTP request");
    return;
  }
#ifdef USE_WEBSERVER_OTA
  if (is_ota_upload) {
    std::string boundary;
    for (const auto &h : headers) {
      if (equals_ci(h.first, "Content-Type")) {
        boundary = MultipartStream::boundary_from_content_type(h.second);
        break;
      }
    }
    if (boundary.empty() || content_length == 0) {
      c.response = std::make_unique<AsyncWebServerResponseContent>(400, "text/plain", "Invalid multipart update");
      return;
    }
    c.upload_request = std::make_unique<AsyncWebServerRequest>(method, uri, query, "", std::move(headers), &c.socket);
    c.upload_request->set_content_length(content_length);
    for (auto *handler : this->handlers_) {
      if (handler->canHandle(c.upload_request.get())) {
        c.upload_handler = handler;
        break;
      }
    }
    if (c.upload_handler == nullptr) {
      c.response = std::make_unique<AsyncWebServerResponseContent>(404, "text/plain", "OTA handler unavailable");
      return;
    }
    // Run the standard origin/auth guard before accepting any flash bytes.
    c.upload_handler->handleUpload(c.upload_request.get(), "", 0, nullptr, 0, false);
    if (c.upload_request->has_response()) {
      c.response = c.upload_request->take_response();
      return;
    }
    c.multipart = std::make_unique<MultipartStream>(std::move(boundary), content_length);
    c.is_upload = true;
    c.upload_expected = content_length;
    c.input.erase(0, c.header_end);
    c.header_end = 0;
    return;
  }
#endif
  if (c.input.size() - c.header_end < content_length) {
    c.dispatched = false;
    return;
  }
  const std::string body = c.input.substr(c.header_end, content_length);
  AsyncWebServerRequest request(method, uri, query, body, std::move(headers), &c.socket);
  bool handled = false;
  for (auto *handler : this->handlers_) {
    if (handler->canHandle(&request)) {
      handler->handleRequest(&request);
      handled = true;
      break;
    }
  }
  if (!handled && this->not_found_)
    this->not_found_(&request);
  c.response = request.take_response();
  if (c.socket != nullptr && c.response == nullptr)
    c.response = std::make_unique<AsyncWebServerResponseContent>(404, "text/plain", "Not found");
}

#ifdef USE_WEBSERVER_OTA
void AsyncWebServer::process_upload_(Client &c) {
  if (!c.is_upload || c.upload_done || c.upload_request == nullptr || c.multipart == nullptr)
    return;
  if (c.input.size() > c.upload_expected - c.upload_received) {
    c.response = std::make_unique<AsyncWebServerResponseContent>(400, "text/plain", "Upload body length exceeded");
    c.upload_done = true;
    return;
  }
  const size_t length = c.input.size();
  if (length == 0)
    return;
  auto callback = [&c](uint8_t *data, size_t len, bool final) -> bool {
    c.upload_handler->handleUpload(c.upload_request.get(), c.multipart->filename(), c.upload_file_index, data, len,
                                   final);
    c.upload_file_index += len;
    return !c.upload_request->has_response();
  };
  if (!c.multipart->feed(reinterpret_cast<const uint8_t *>(c.input.data()), length, callback)) {
    // Keep the OTA handler's own error; only a framing error gets the generic answer.
    c.response = c.upload_request->take_response();
    if (c.response == nullptr)
      c.response = std::make_unique<AsyncWebServerResponseContent>(400, "text/plain", "Malformed firmware upload");
    c.upload_done = true;
    return;
  }
  c.upload_received += length;
  c.input.clear();
  if (c.upload_received == c.upload_expected) {
    c.upload_done = true;
    if (!c.multipart->complete()) {
      c.response = std::make_unique<AsyncWebServerResponseContent>(400, "text/plain", "Incomplete firmware upload");
      return;
    }
    c.upload_handler->handleRequest(c.upload_request.get());
    c.response = c.upload_request->take_response();
    if (c.response == nullptr)
      c.response = std::make_unique<AsyncWebServerResponseContent>(500, "text/plain", "OTA response missing");
  }
}
#endif

void AsyncWebServer::process_(Client &c) {
  if (!c.socket)
    return;
  const uint32_t now = App.get_loop_component_start_time();
  if (now - c.started > CLIENT_TIMEOUT_MS) {
    c.socket.reset();
    return;
  }
  if (!c.dispatched) {
    char buf[512];
    for (size_t nread = 0; nread < 4; ++nread) {
      const ssize_t n = c.socket->read(buf, sizeof(buf));
      if (n == 0) {
        c.socket.reset();
        return;
      }
      if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
          c.socket.reset();
        break;
      }
      c.input.append(buf, static_cast<size_t>(n));
      c.started = now;  // Timeout is an idle timer, not a total download budget.
      if (c.header_end == 0) {
        const size_t marker = c.input.find("\r\n\r\n");
        if (marker != std::string::npos)
          c.header_end = marker + 4;
      }
      if ((c.header_end == 0 && c.input.size() > MAX_HEADER_BYTES) || c.header_end > MAX_HEADER_BYTES) {
        c.socket.reset();
        return;
      }
      if (c.input.size() > MAX_HEADER_BYTES + MAX_BODY_BYTES) {
        c.socket.reset();
        return;
      }
    }
    if (c.header_end == 0)
      return;
    this->dispatch_(c);
  }
#ifdef USE_WEBSERVER_OTA
  if (c.is_upload && !c.upload_done && c.socket != nullptr) {
    this->process_upload_(c);
    if (!c.upload_done) {
      char upload_buf[512];
      for (size_t i = 0; i < 4 && !c.upload_done; ++i) {
        const ssize_t n = c.socket->read(upload_buf, sizeof(upload_buf));
        if (n == 0) {
          c.socket.reset();
          break;
        }
        if (n < 0) {
          if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            c.socket.reset();
          break;
        }
        c.input.append(upload_buf, static_cast<size_t>(n));
        c.started = now;
        this->process_upload_(c);
      }
    }
  }
#endif
  if (c.socket == nullptr || c.response == nullptr)
    return;
  if (c.output.empty()) {
    const auto &r = *c.response;
    char start[128];
    std::snprintf(start, sizeof(start), "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\n", r.status(),
                  reason_phrase(r.status()), r.get_content_size());
    c.output = start;
    if (!r.content_type().empty()) {
      c.output += "Content-Type: " + r.content_type() + "\r\n";
    }
    for (auto &h : DefaultHeaders::Instance().headers()) {
      c.output += h.name;
      c.output += ": ";
      c.output += h.value;
      c.output += "\r\n";
    }
    for (auto &h : r.headers()) {
      c.output += h.first + ": " + h.second + "\r\n";
    }
    c.output += "\r\n";
  }
  while (c.output_sent < c.output.size()) {
    const ssize_t n = c.socket->send(c.output.data() + c.output_sent, c.output.size() - c.output_sent, MSG_DONTWAIT);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        c.socket.reset();
      return;
    }
    if (n == 0)
      return;
    c.output_sent += static_cast<size_t>(n);
    c.started = now;
  }
  const size_t body_size = c.response->get_content_size();
  const char *body_data = c.response->get_content_data();
  if (body_size != 0 && body_data == nullptr) {
    c.socket.reset();
    return;
  }
  while (c.body_sent < body_size) {
    const ssize_t n =
        c.socket->send(body_data + c.body_sent, std::min<size_t>(1024, body_size - c.body_sent), MSG_DONTWAIT);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        c.socket.reset();
      return;
    }
    if (n == 0)
      return;
    c.body_sent += static_cast<size_t>(n);
    c.started = now;
  }
  c.socket.reset();
}

void AsyncWebServer::loop() {
  // The listener is parked during standby preflight, but previously accepted
  // requests must still drain and complete before HTTP_BUSY can be cleared.
  if (this->listener_ != nullptr && this->clients_.size() < MAX_CLIENTS) {
    sockaddr_storage peer{};
    socklen_t len = sizeof(peer);
    auto incoming = this->listener_->accept(reinterpret_cast<sockaddr *>(&peer), &len);
    if (incoming != nullptr) {
      incoming->setblocking(false);
      Client client{};
      client.socket = std::move(incoming);
      client.started = App.get_loop_component_start_time();
      this->clients_.push_back(std::move(client));
    }
  }
  for (auto &client : this->clients_)
    this->process_(client);
  this->clients_.remove_if([](const Client &c) { return c.socket == nullptr; });
}

#ifdef USE_WEBSERVER
AsyncEventSource *AsyncEventSource::first_source =
    nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void AsyncEventSource::close_all() {
  this->sessions_.clear();
  this->listing_session_ = nullptr;
}

bool AsyncEventSource::canHandle(AsyncWebServerRequest *req) const {
  if (req->method() != HTTP_GET)
    return false;
  char url[AsyncWebServerRequest::URL_BUF_SIZE];
  return req->url_to(url) == this->path_;
}
void AsyncEventSource::handleRequest(AsyncWebServerRequest *req) {
  // Keep one enumeration cursor per browser. The previous shared cursor
  // restarted for all users on every new connection.
  if (this->sessions_.size() >= 2) {
    req->send(503, "text/plain", "Too many event clients");
    return;
  }
  auto sock = req->take_socket();
  if (sock == nullptr)
    return;
  Session session{};
  session.socket = std::move(sock);
  session.last_progress = App.get_loop_component_start_time();
  session.pending = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n";
  session.entities_iterator = std::make_unique<web_server::ListEntitiesIterator>(this->web_server_, this);
  session.entities_iterator->begin(this->web_server_->include_internal_);
  this->sessions_.push_back(std::move(session));
  // WebServer::loop() drives the event streams and stops itself while none is open.
  this->web_server_->enable_loop();

  // The standard web frontend expects a config-bearing ping immediately,
  // not only the periodic uptime pings.
  auto config = this->web_server_->get_config_json();
  this->queue_to_session_(this->sessions_.back(), config.c_str(), config.size(), "ping",
                          App.get_loop_component_start_time(), 30000);
#ifdef USE_WEBSERVER_SORTING
  for (const auto &group : this->web_server_->sorting_groups_) {
    json::JsonBuilder builder;
    auto root = builder.root();
    root["name"] = json::linked(group.second.name);
    root["sorting_weight"] = group.second.weight;
    auto message = builder.serialize();
    this->queue_to_session_(this->sessions_.back(), message.c_str(), message.size(), "sorting_group", 0, 0);
  }
#endif
}
void AsyncEventSource::queue_to_session_(Session &session, const char *message, size_t len, const char *event,
                                         uint32_t id, uint32_t reconnect) {
  if (session.socket == nullptr)
    return;
  std::string frame;
  if (event != nullptr)
    frame += std::string("event: ") + event + "\n";
  if (id != 0) {
    char decimal[UINT32_MAX_STR_SIZE];
    uint32_to_str(decimal, id);
    frame += "id: ";
    frame += decimal;
    frame += "\n";
  }
  if (reconnect != 0) {
    char decimal[UINT32_MAX_STR_SIZE];
    uint32_to_str(decimal, reconnect);
    frame += "retry: ";
    frame += decimal;
    frame += "\n";
  }
  frame += "data: ";
  frame.append(message, len);
  frame += "\n\n";
  char prefix[24];
  std::snprintf(prefix, sizeof(prefix), "%zx\r\n", frame.size());
  const size_t extra = std::strlen(prefix) + frame.size() + 2;
  // Bounded queue: slow browsers are disconnected instead of consuming
  // unbounded SRAM or stalling the MCU UART and main application loop.
  if (extra > 8192 || session.pending.size() > 8192 - extra) {
    session.socket.reset();
    return;
  }
  session.pending += prefix;
  session.pending += frame;
  session.pending += "\r\n";
}
void AsyncEventSource::queue_(const char *message, size_t len, const char *event, uint32_t id, uint32_t reconnect) {
  for (auto &session : this->sessions_)
    this->queue_to_session_(session, message, len, event, id, reconnect);
}
void AsyncEventSource::try_send_nodefer(const char *message, size_t len, const char *event, uint32_t id,
                                        uint32_t reconnect) {
  this->queue_(message, len, event, id, reconnect);
}
void AsyncEventSource::deferrable_send_state(void *source, bool detail_all, message_generator_t *generator) {
  if (this->sessions_.empty())
    return;
  if (detail_all) {
    if (this->listing_session_ == nullptr || this->listing_session_->socket == nullptr)
      return;
  } else {
    bool has_ready_client = false;
    for (const auto &session : this->sessions_)
      has_ready_client |= session.socket != nullptr && session.entities_iterator->completed();
    if (!has_ready_client)
      return;
  }
  json::JsonBuilder builder;
  generator(this->web_server_, source, builder);
  auto message = builder.serialize();
  if (detail_all) {
    this->queue_to_session_(*listing_session_, message.c_str(), message.size(), "state", 0, 0);
  } else {
    for (auto &session : this->sessions_) {
      if (session.socket != nullptr && session.entities_iterator->completed())
        this->queue_to_session_(session, message.c_str(), message.size(), "state", 0, 0);
    }
  }
}
bool AsyncEventSource::loop() {
  if (this->sessions_.empty())
    return false;
  const uint32_t now = App.get_loop_component_start_time();
  for (auto &session : this->sessions_) {
    if (session.socket == nullptr)
      continue;
    if (!session.entities_iterator->completed()) {
      this->listing_session_ = &session;
      session.entities_iterator->try_advance(1);
      this->listing_session_ = nullptr;
      if (session.socket == nullptr)
        continue;
    }
    if (now - session.last_progress > 20000) {
      session.socket.reset();
      continue;
    }
    const size_t pending = session.pending.size() - session.sent;
    if (pending == 0)
      continue;
    const ssize_t n =
        session.socket->send(session.pending.data() + session.sent, std::min<size_t>(1024, pending), MSG_DONTWAIT);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        session.socket.reset();
      continue;
    }
    if (n > 0) {
      session.sent += static_cast<size_t>(n);
      session.last_progress = now;
      if (session.sent == session.pending.size()) {
        session.pending.clear();
        session.sent = 0;
      }
    }
  }
  this->sessions_.erase(std::remove_if(this->sessions_.begin(), this->sessions_.end(),
                                       [](const Session &s) { return s.socket == nullptr; }),
                        this->sessions_.end());
  return !this->sessions_.empty();
}
#endif

}  // namespace esphome::web_server_xr806
#endif  // USE_XR806
