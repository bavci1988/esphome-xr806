#include "http_portal.h"

#if defined(USE_XR806) && defined(USE_XR806_HTTP_PORTAL)

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "captive_dns_packet.h"

extern "C" {
#include "xr_config.h"
#include "demo/trv16_port/prj_config.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "net/wlan/wlan.h"
}

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806_http");

static constexpr uint32_t RETRY_MS = 5000;
static constexpr uint32_t CLIENT_TIMEOUT_MS = 5000;

// Keep the page standalone. The XR806 has no Arduino web framework.
// This is deliberately limited to Wi-Fi provisioning and HTTP diagnostics.
static const char PAGE[] =
    "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>XR806 setup</title></head><body><h2>XR806 Wi-Fi setup</h2>"
    "<form method=\"POST\" action=\"/wifisave\">"
    "<label>Wi-Fi SSID <input name=\"ssid\" maxlength=\"32\" required></label><p>"
    "<label>Password <input type=\"password\" name=\"psk\" maxlength=\"63\"></label></p>"
    "<button type=\"submit\">Connect</button></form>"
    "<p>Credentials are applied now but the current XR806 preferences backend does not retain them over power loss.</p>"
    "</body></html>";

static int socket_nonblocking(int fd) {
  unsigned long nonblocking = 1;
  return lwip_ioctl(fd, FIONBIO, &nonblocking);
}

static bool transient_error() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

static int hex_digit(char ch) {
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  return -1;
}

// Decode one application/x-www-form-urlencoded value with no truncation.
// In particular, %00 must not become an embedded NUL in a Wi-Fi credential.
static bool decode_form_value(const char *data, size_t length, char *output, size_t capacity) {
  size_t used = 0;
  for (size_t i = 0; i < length; i++) {
    unsigned char value = static_cast<unsigned char>(data[i]);
    if (value == '%') {
      if (i + 2 >= length)
        return false;
      const int hi = hex_digit(data[i + 1]);
      const int lo = hex_digit(data[i + 2]);
      if (hi < 0 || lo < 0)
        return false;
      value = static_cast<unsigned char>((hi << 4) | lo);
      i += 2;
    } else if (value == '+') {
      value = ' ';
    }
    if (value == 0 || used + 1 >= capacity)
      return false;
    output[used++] = static_cast<char>(value);
  }
  output[used] = '\0';
  return true;
}

static bool parse_wifi_form(const char *data, size_t length, char ssid[33], char password[65]) {
  bool got_ssid = false;
  bool got_password = false;
  size_t index = 0;
  while (index < length) {
    const size_t begin = index;
    while (index < length && data[index] != '&')
      index++;
    const size_t end = index;
    size_t equal = begin;
    while (equal < end && data[equal] != '=')
      equal++;
    if (equal < end && equal - begin == 4 && memcmp(data + begin, "ssid", 4) == 0 && !got_ssid) {
      if (!decode_form_value(data + equal + 1, end - equal - 1, ssid, 33))
        return false;
      got_ssid = true;
    } else if (equal < end && equal - begin == 3 && memcmp(data + begin, "psk", 3) == 0 && !got_password) {
      if (!decode_form_value(data + equal + 1, end - equal - 1, password, 65))
        return false;
      got_password = true;
    }
    index++;
  }
  const size_t password_size = strlen(password);
  return got_ssid && ssid[0] != '\0' &&
         (password_size == 0 || (password_size >= 8 && password_size <= 63));
}

bool XR806HTTPServer::is_ap_active_() const {
  struct netif *nif = wlan_netif_get(WLAN_MODE_HOSTAP);
  return nif != nullptr && wlan_netif_get_mode(nif) == WLAN_MODE_HOSTAP && netif_is_up(nif) &&
         netif_is_link_up(nif) && !ip4_addr_isany_val(*netif_ip4_addr(nif));
}

bool XR806HTTPServer::start_http_() {
  int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  const int reuse = 1;
  lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (socket_nonblocking(fd) != 0 || lwip_bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      lwip_listen(fd, 2) != 0) {
    lwip_close(fd);
    return false;
  }
  this->listen_fd_ = fd;
  ESP_LOGI(TAG, "HTTP server listening on port %u", this->port_);
  return true;
}

bool XR806HTTPServer::start_dns_() {
  struct netif *nif = wlan_netif_get(WLAN_MODE_HOSTAP);
  if (nif == nullptr)
    return false;
  int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return false;

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = ip4_addr_get_u32(netif_ip4_addr(nif));
  if (socket_nonblocking(fd) != 0 || lwip_bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    lwip_close(fd);
    return false;
  }
  this->dns_fd_ = fd;
  ESP_LOGI(TAG, "Captive DNS listening on AP address");
  return true;
}

void XR806HTTPServer::handle_dns_() {
  uint8_t query[256];
  uint8_t response[272];
  struct sockaddr_in remote {};
  socklen_t remote_size = sizeof(remote);
  const int received = lwip_recvfrom(this->dns_fd_, query, sizeof(query), MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr *>(&remote), &remote_size);
  if (received <= 0)
    return;
  struct netif *nif = wlan_netif_get(WLAN_MODE_HOSTAP);
  if (nif == nullptr)
    return;
  const ip4_addr_t *addr = netif_ip4_addr(nif);
  const uint8_t ip[4] = {ip4_addr1(addr), ip4_addr2(addr), ip4_addr3(addr), ip4_addr4(addr)};
  const size_t size = make_captive_dns_reply(query, static_cast<size_t>(received), ip, response, sizeof(response));
  if (size != 0)
    lwip_sendto(this->dns_fd_, response, size, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&remote), remote_size);
}

void XR806HTTPServer::accept_client_() {
  struct sockaddr_in addr {};
  socklen_t size = sizeof(addr);
  int fd = lwip_accept(this->listen_fd_, reinterpret_cast<sockaddr *>(&addr), &size);
  if (fd < 0)
    return;
  if (socket_nonblocking(fd) != 0) {
    lwip_close(fd);
    return;
  }
  this->client_fd_ = fd;
  this->client_started_ = millis();
  this->request_used_ = this->response_len_ = this->response_sent_ = 0;
  this->save_pending_ = false;
  this->request_[0] = '\0';
}

void XR806HTTPServer::send_response_(int status, const char *content_type, const char *body) {
  const size_t body_len = strlen(body);
  const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
                       status == 403 ? "Forbidden" : status == 405 ? "Method Not Allowed" :
                       status == 413 ? "Content Too Large" : "Not Found";
  const int header_len = snprintf(this->response_, sizeof(this->response_),
                                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                                  "Cache-Control: no-store\r\nConnection: close\r\n"
                                  "Content-Length: %u\r\n\r\n",
                                  status, reason, content_type, static_cast<unsigned>(body_len));
  if (header_len < 0 || static_cast<size_t>(header_len) + body_len > sizeof(this->response_)) {
    this->response_len_ = 0;
    this->close_client_();
    return;
  }
  memcpy(this->response_ + header_len, body, body_len);
  this->response_len_ = static_cast<size_t>(header_len) + body_len;
}

void XR806HTTPServer::prepare_request_() {
  const char *header_end = strstr(this->request_, "\r\n\r\n");
  if (header_end == nullptr)
    return;
  const size_t header_len = static_cast<size_t>(header_end + 4 - this->request_);
  const bool is_get = strncmp(this->request_, "GET ", 4) == 0;
  const bool is_post = strncmp(this->request_, "POST ", 5) == 0;
  if (!is_get && !is_post) {
    this->send_response_(405, "text/plain", "Only GET and POST are supported");
    return;
  }
  const char *path = this->request_ + (is_get ? 4 : 5);
  const char *path_end = strchr(path, ' ');
  if (path_end == nullptr || path_end >= header_end || path_end - path > 64) {
    this->send_response_(400, "text/plain", "Malformed request");
    return;
  }
  const size_t path_len = static_cast<size_t>(path_end - path);
  if (is_get) {
    if (path_len == 7 && memcmp(path, "/health", 7) == 0) {
      this->send_response_(200, "application/json", "{\"ok\":true,\"platform\":\"xr806\"}");
    } else if (this->ap_active_) {
      // Returning HTTP 200 HTML for OS probe URLs triggers the captive portal UI.
      this->send_response_(200, "text/html; charset=utf-8", PAGE);
    } else {
      this->send_response_(404, "text/plain", "XR806 HTTP service; /health is available");
    }
    return;
  }
  if (!this->ap_active_ || path_len != 9 || memcmp(path, "/wifisave", 9) != 0) {
    this->send_response_(403, "text/plain", "Wi-Fi provisioning is restricted to SoftAP");
    return;
  }

  const char *length_header = strstr(this->request_, "\r\nContent-Length:");
  if (length_header == nullptr || length_header >= header_end) {
    this->send_response_(400, "text/plain", "Missing Content-Length");
    return;
  }
  const char *digits = length_header + strlen("\r\nContent-Length:");
  while (digits < header_end && *digits == ' ')
    digits++;
  if (digits == header_end || *digits < '0' || *digits > '9') {
    this->send_response_(400, "text/plain", "Malformed Content-Length");
    return;
  }
  unsigned body_length = 0;
  while (digits < header_end && *digits >= '0' && *digits <= '9') {
    body_length = body_length * 10 + static_cast<unsigned>(*digits - '0');
    if (body_length > 512) {
      this->send_response_(413, "text/plain", "Form too large");
      return;
    }
    digits++;
  }
  if (this->request_used_ < header_len + body_length)
    return;
  this->pending_ssid_[0] = '\0';
  this->pending_password_[0] = '\0';
  if (!parse_wifi_form(this->request_ + header_len, body_length, this->pending_ssid_, this->pending_password_)) {
    this->send_response_(400, "text/plain", "Invalid SSID or password");
    return;
  }
  this->send_response_(200, "text/plain",
                       "Wi-Fi settings applied. This XR806 build cannot yet persist them across a power cycle.");
  this->save_pending_ = true;
}

void XR806HTTPServer::handle_client_() {
  if (millis() - this->client_started_ > CLIENT_TIMEOUT_MS) {
    this->close_client_();
    return;
  }
  if (this->response_len_ == 0) {
    const int max_read = static_cast<int>(MAX_REQUEST - this->request_used_);
    if (max_read <= 0) {
      this->send_response_(413, "text/plain", "Request too large");
    } else {
      const int received = lwip_recv(this->client_fd_, this->request_ + this->request_used_, max_read, MSG_DONTWAIT);
      if (received > 0) {
        this->request_used_ += static_cast<size_t>(received);
        this->request_[this->request_used_] = '\0';
        this->prepare_request_();
      } else if (received == 0 || (received < 0 && !transient_error())) {
        this->close_client_();
        return;
      }
    }
  }
  if (this->client_fd_ < 0 || this->response_len_ == 0)
    return;

  const int sent = lwip_send(this->client_fd_, this->response_ + this->response_sent_,
                             this->response_len_ - this->response_sent_, MSG_DONTWAIT);
  if (sent < 0 && !transient_error()) {
    this->close_client_();
    return;
  }
  if (sent > 0)
    this->response_sent_ += static_cast<size_t>(sent);
  if (this->response_sent_ == this->response_len_) {
    // Close the HTTP response before changing the radio mode.
    const bool should_save = this->save_pending_;
    this->save_pending_ = false;
    if (this->client_fd_ >= 0) {
      lwip_close(this->client_fd_);
      this->client_fd_ = -1;
    }
    if (should_save && wifi::global_wifi_component != nullptr) {
      wifi::global_wifi_component->save_wifi_sta(this->pending_ssid_, this->pending_password_);
    }
    this->close_client_();
  }
}

void XR806HTTPServer::close_client_() {
  if (this->client_fd_ >= 0) {
    lwip_close(this->client_fd_);
    this->client_fd_ = -1;
  }
  this->request_used_ = this->response_len_ = this->response_sent_ = 0;
  this->save_pending_ = false;
  memset(this->pending_ssid_, 0, sizeof(this->pending_ssid_));
  memset(this->pending_password_, 0, sizeof(this->pending_password_));
}

void XR806HTTPServer::setup() { ESP_LOGI(TAG, "Native lwIP HTTP/DNS support enabled"); }

void XR806HTTPServer::loop() {
  const uint32_t now = millis();
  const bool ap = this->is_ap_active_();
  if (ap != this->ap_active_) {
    this->ap_active_ = ap;
    if (!ap && this->dns_fd_ >= 0) {
      lwip_close(this->dns_fd_);
      this->dns_fd_ = -1;
    }
  }
  if (this->listen_fd_ < 0 && (this->last_http_attempt_ == 0 || now - this->last_http_attempt_ >= RETRY_MS)) {
    this->last_http_attempt_ = now;
    if (!this->start_http_())
      ESP_LOGW(TAG, "HTTP bind failed; retrying");
  }
  if (this->captive_dns_ && ap) {
    if (this->dns_fd_ < 0 && (this->last_dns_attempt_ == 0 || now - this->last_dns_attempt_ >= RETRY_MS)) {
      this->last_dns_attempt_ = now;
      if (!this->start_dns_())
        ESP_LOGW(TAG, "Captive DNS bind failed; retrying");
    }
    if (this->dns_fd_ >= 0)
      this->handle_dns_();
  }
  if (this->listen_fd_ < 0)
    return;
  if (this->client_fd_ < 0)
    this->accept_client_();
  if (this->client_fd_ >= 0)
    this->handle_client_();
}

void XR806HTTPServer::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 native HTTP port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "Captive DNS: %s", YESNO(this->captive_dns_));
}

}  // namespace esphome::xr806
#endif
