#pragma once
#include "esphome/core/defines.h"
#ifdef USE_XR806

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/socket/socket.h"

#ifdef USE_WEBSERVER
#include "esphome/components/json/json_util.h"
#include "esphome/components/web_server/list_entities.h"
#endif

namespace esphome::web_server {
class WebServer;
}

namespace esphome::web_server_idf {

// ESPHome HTTP handler contract; implemented on the existing lwIP sockets, not
// the ESP-IDF httpd, ESPAsyncWebServer or a vendor-specific duplicate REST API.
enum http_method : uint8_t { HTTP_GET, HTTP_POST, HTTP_OPTIONS, HTTP_PUT, HTTP_DELETE, HTTP_UNKNOWN };
class AsyncWebServerRequest;
class AsyncWebServer;
class AsyncWebHandler;

class AsyncWebServerResponse {
 public:
  AsyncWebServerResponse(int code, const char *type) : status_(code), content_type_(type == nullptr ? "" : type) {}
  virtual ~AsyncWebServerResponse() = default;
  void addHeader(const char *name, const char *value);
  virtual const char *get_content_data() const = 0;
  virtual size_t get_content_size() const = 0;
  int status() const { return this->status_; }
  const std::string &content_type() const { return this->content_type_; }
  const std::vector<std::pair<std::string, std::string>> &headers() const { return this->headers_; }

 protected:
  int status_;
  std::string content_type_;
  std::vector<std::pair<std::string, std::string>> headers_;
};

class AsyncWebServerResponseContent : public AsyncWebServerResponse {
 public:
  AsyncWebServerResponseContent(int code, const char *type, std::string text)
      : AsyncWebServerResponse(code, type), text_(std::move(text)) {}
  const char *get_content_data() const override { return text_.data(); }
  size_t get_content_size() const override { return text_.size(); }

 protected:
  std::string text_;
};

class AsyncWebServerResponseProgmem : public AsyncWebServerResponse {
 public:
  AsyncWebServerResponseProgmem(int code, const char *type, const uint8_t *data, size_t size)
      : AsyncWebServerResponse(code, type), data_(data), size_(size) {}
  const char *get_content_data() const override { return reinterpret_cast<const char *>(data_); }
  size_t get_content_size() const override { return size_; }

 protected:
  const uint8_t *data_;
  size_t size_;
};

class AsyncResponseStream : public AsyncWebServerResponseContent {
 public:
  explicit AsyncResponseStream(const char *type) : AsyncWebServerResponseContent(200, type, "") {}
  void print(const char *text) { text_.append(text); }
  void print(const std::string &text) { text_.append(text); }
  void print(float value);
  void print(int value);
  void printf(const char *format, ...) __attribute__((format(printf, 2, 3)));
  void write(uint8_t value) { text_.push_back(static_cast<char>(value)); }
};

class AsyncWebServerRequest {
 public:
  static constexpr size_t URL_BUF_SIZE = 257;
  AsyncWebServerRequest(http_method method, std::string uri, std::string query, std::string body,
                        std::vector<std::pair<std::string, std::string>> headers,
                        std::unique_ptr<socket::Socket> *socket);
  http_method method() const { return method_; }
  StringRef url_to(std::span<char, URL_BUF_SIZE> buf) const;
  bool hasHeader(const char *name) const;
  size_t get_header_to(const char *name, std::span<char> buf) const;
  std::string arg(const char *name) const;
  std::string arg(const std::string &name) const { return this->arg(name.c_str()); }
  bool hasArg(const char *name) const;
  bool hasArg(const std::string &name) const { return this->hasArg(name.c_str()); }
  bool authenticate(const char *basic_auth_hash) const;
  void requestAuthentication(const char *realm = nullptr, bool is_digest = false);
  void redirect(const char *url);
  void send(int status, const char *type = nullptr, const char *text = nullptr);
  void send(AsyncWebServerResponse *response);
  AsyncWebServerResponse *beginResponse(int status, const char *type);
  AsyncWebServerResponse *beginResponse(int status, const char *type, const std::string &text);
  AsyncWebServerResponse *beginResponse(int status, const char *type, const uint8_t *data, size_t len);
  AsyncResponseStream *beginResponseStream(const char *type);
  std::unique_ptr<AsyncWebServerResponse> take_response() { return std::move(response_); }
  std::unique_ptr<socket::Socket> take_socket() { return socket_ == nullptr ? nullptr : std::move(*socket_); }
  bool has_response() const { return response_ != nullptr; }

 protected:
  static std::string decode_(const std::string &s);
  static bool find_arg_(const std::string &query, const char *name, std::string *result);
  http_method method_;
  std::string uri_;
  std::string query_;
  std::string body_;
  std::vector<std::pair<std::string, std::string>> headers_;
  std::unique_ptr<socket::Socket> *socket_;
  std::unique_ptr<AsyncWebServerResponse> response_;
};

class AsyncWebHandler {
 public:
  virtual ~AsyncWebHandler() = default;
  virtual bool canHandle(AsyncWebServerRequest *request) const { return false; }
  virtual void handleRequest(AsyncWebServerRequest *request) {}
  virtual void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index, uint8_t *data,
                            size_t len, bool final) {}
  virtual void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {}
  virtual bool isRequestHandlerTrivial() const { return true; }
};

class AsyncWebServer {
 public:
  explicit AsyncWebServer(uint16_t port) : port_(port) {}
  ~AsyncWebServer() { this->end(); }
  void begin();
  void end();
  void loop();
  AsyncWebHandler &addHandler(AsyncWebHandler *handler) {
    handlers_.push_back(handler);
    return *handler;
  }
  void onNotFound(std::function<void(AsyncWebServerRequest *)> fn) { not_found_ = std::move(fn); }

 protected:
  struct Client {
    std::unique_ptr<socket::Socket> socket;
    std::string input;
    std::string output;
    std::unique_ptr<AsyncWebServerResponse> response;
    size_t header_end{0}, expected_body{0}, output_sent{0}, body_sent{0};
    uint32_t started{0};
    bool dispatched{false};
  };
  static constexpr size_t MAX_CLIENTS = 3;
  static constexpr size_t MAX_HEADER_BYTES = 2048;
  static constexpr size_t MAX_BODY_BYTES = 4096;
  static constexpr uint32_t CLIENT_TIMEOUT_MS = 5000;
  void dispatch_(Client &client);
  void process_(Client &client);
  uint16_t port_;
  std::unique_ptr<socket::ListenSocket> listener_;
  std::vector<Client> clients_;
  std::vector<AsyncWebHandler *> handlers_;
  std::function<void(AsyncWebServerRequest *)> not_found_;
};

struct HttpHeader {
  const char *name;
  const char *value;
};

class DefaultHeaders {
 public:
  static DefaultHeaders &Instance();
  void addHeader(const char *name, const char *value) { headers_.push_back({name, value}); }
  const std::vector<HttpHeader> &headers() const { return headers_; }

 protected:
  std::vector<HttpHeader> headers_;
};

#ifdef USE_WEBSERVER
using message_generator_t = void(esphome::web_server::WebServer *, void *, json::JsonBuilder &);

class AsyncEventSource : public AsyncWebHandler {
 public:
  AsyncEventSource(StringRef path, esphome::web_server::WebServer *web_server)
      : path_(path), web_server_(web_server) {}
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void try_send_nodefer(const char *message, size_t len, const char *event = nullptr, uint32_t id = 0,
                        uint32_t reconnect = 0);
  void deferrable_send_state(void *source, bool detail_all, message_generator_t *generator);
  bool loop();
  bool empty() const { return sessions_.empty(); }

 protected:
  struct Session {
    std::unique_ptr<socket::Socket> socket;
    std::string pending;
    size_t sent{0};
    uint32_t last_progress{0};
  };
  void queue_(const char *message, size_t len, const char *event, uint32_t id, uint32_t reconnect);
  StringRef path_;
  esphome::web_server::WebServer *web_server_;
  std::vector<Session> sessions_;
  web_server::ListEntitiesIterator entities_iterator_{web_server_, this};
};
#endif

}  // namespace esphome::web_server_idf

// Keep the WebServerBase/REST handler surface shared with ESPHome's native IDF backend.
using namespace esphome::web_server_idf;  // NOLINT(google-global-names-in-headers)
#endif  // USE_XR806
