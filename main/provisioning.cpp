// Provisioning — the SoftAP, the DNS redirect and the configuration page.
//
// Entered when no usable configuration is stored (FR-NVS-02) or on a deliberate
// button hold (FR-SUP-06). Never on a WiFi failure: a device in a basement that
// puts up a portal nobody can see is unreachable rather than recoverable
// (FR-SUP-04, D-C4).
//
// The identity of the network is decided in provisioning.h and tested at host
// tier; everything here is ESP-IDF and can only be exercised on the bench.
#include "provisioning.h"

#include <cstring>

#include "config.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "net.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

namespace gplug {
namespace {

constexpr const char* TAG = "prov";

// FR-PRV-06: leaving without a submission keeps whatever was stored. The
// timeout exists so a device knocked into Provisioning by accident returns to
// trying rather than sitting in a portal forever (D-C3).
constexpr uint32_t TIMEOUT_MS = 5 * 60 * 1000;

// Often enough that a stalled portal is obvious within one read, rare enough
// that five minutes of waiting does not fill the buffer.
constexpr int64_t DIAG_PERIOD_US = 5 * 1000 * 1000;

constexpr int DNS_PORT = 53;
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CLIENTS = 4;

httpd_handle_t server = nullptr;
TaskHandle_t dns_task_handle = nullptr;
volatile bool submitted = false;
Config staged{};

// ── The configuration page ──────────────────────────────────────────────────
//
// Served for every path, because the captive-portal probe each platform uses is
// a different URL and none of them are worth enumerating (FR-PRV-03).

const char PAGE_HEAD[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>gPlug-mini setup</title>"
    "<style>body{font-family:system-ui;margin:2rem;max-width:32rem}"
    "label{display:block;margin:.75rem 0 .25rem}input,select{width:100%;padding:.5rem;font-size:1rem}"
    "button{margin-top:1rem;padding:.6rem 1.2rem;font-size:1rem}"
    ".n{color:#666;font-size:.85rem}</style>"
    "<h1>gPlug-mini</h1><form method=post action=/save>";

const char PAGE_TAIL[] =
    "<label>WiFi network</label><input name=ssid maxlength=32 required>"
    "<label>WiFi password <span class=n>(leave blank for an open network)</span></label>"
    "<input name=pass type=password maxlength=63>"
    "<label>MQTT broker</label>"
    "<input name=broker maxlength=127 placeholder='mqtt://homeassistant.local:1883' required>"
    "<p class=n>The scheme is required. An address on its own is refused here rather "
    "than failing later as a connection error.</p>"
    "<label>Hostname <span class=n>(optional)</span></label><input name=host maxlength=32>"
    "<button type=submit>Save and restart</button></form>";

// Percent-decoding, in place. Returns the new length.
size_t url_decode(char* s) {
  char* w = s;
  for (char* r = s; *r; ++r) {
    if (*r == '+') {
      *w++ = ' ';
    } else if (*r == '%' && r[1] && r[2]) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(r[1]), lo = hex(r[2]);
      if (hi >= 0 && lo >= 0) {
        *w++ = static_cast<char>(hi * 16 + lo);
        r += 2;
        continue;
      }
      *w++ = *r;
    } else {
      *w++ = *r;
    }
  }
  *w = '\0';
  return static_cast<size_t>(w - s);
}

// One field out of an application/x-www-form-urlencoded body.
bool field(const char* body, const char* name, char* out, size_t cap) {
  char key[24];
  const int k = std::snprintf(key, sizeof(key), "%s=", name);
  if (k <= 0) return false;

  const char* p = body;
  while (p) {
    if (std::strncmp(p, key, static_cast<size_t>(k)) == 0) {
      p += k;
      const char* end = std::strchr(p, '&');
      const size_t n = end ? static_cast<size_t>(end - p) : std::strlen(p);
      if (n >= cap) return false;   // too long is a refusal, never a truncation
      std::memcpy(out, p, n);
      out[n] = '\0';
      url_decode(out);
      return true;
    }
    p = std::strchr(p, '&');
    if (p) ++p;
  }
  out[0] = '\0';
  return true;   // absent is not malformed; config_fault decides what is required
}

esp_err_t page_get(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN);

  // FR-PRV-07: list what the device can actually see. A typed SSID that differs
  // by one character from the real one fails as a wrong password, which is the
  // hardest possible way to discover a typo.
  wifi_scan_config_t scan = {};
  scan.show_hidden = false;
  if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found > 12) found = 12;
    wifi_ap_record_t records[12];
    if (found && esp_wifi_scan_get_ap_records(&found, records) == ESP_OK) {
      httpd_resp_send_chunk(req, "<p class=n>In range: ", HTTPD_RESP_USE_STRLEN);
      for (uint16_t i = 0; i < found; ++i) {
        char line[64];
        const int n = std::snprintf(line, sizeof(line), "%s%s (%d dBm)",
                                    i ? ", " : "",
                                    reinterpret_cast<const char*>(records[i].ssid),
                                    records[i].rssi);
        if (n > 0) httpd_resp_send_chunk(req, line, static_cast<ssize_t>(n));
      }
      httpd_resp_send_chunk(req, "</p>", HTTPD_RESP_USE_STRLEN);
    }
  } else {
    ESP_LOGW(TAG, "scan failed — the page is served without a network list");
  }

  httpd_resp_send_chunk(req, PAGE_TAIL, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t save_post(httpd_req_t* req) {
  char body[512];
  const int len = req->content_len < static_cast<int>(sizeof(body)) - 1
                      ? req->content_len
                      : static_cast<int>(sizeof(body)) - 1;
  int got = 0;
  while (got < len) {
    const int r = httpd_req_recv(req, body + got, static_cast<size_t>(len - got));
    if (r <= 0) return ESP_FAIL;
    got += r;
  }
  body[got] = '\0';

  Config c{};
  const bool parsed = field(body, "ssid", c.ssid, sizeof(c.ssid)) &&
                      field(body, "pass", c.passphrase, sizeof(c.passphrase)) &&
                      field(body, "broker", c.broker, sizeof(c.broker)) &&
                      field(body, "host", c.hostname, sizeof(c.hostname));

  const ConfigFault fault = parsed ? config_fault(c) : ConfigFault::NoSsid;
  if (fault != ConfigFault::None) {
    // FR-PRV-08 and the sibling rules: refuse here, with the reason. Accepting
    // a broker address with no scheme would store it, reboot, and present as a
    // connect error with nothing pointing back at the form.
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "<!doctype html><h1>Not saved</h1><p>%s</p><p><a href=/>Back</a></p>",
                  describe(fault));
    ESP_LOGW(TAG, "submission refused: %s", describe(fault));
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // FR-PRV-05: persisted before Provisioning ends. Answering first and saving
  // afterwards would report success for a write that had not happened.
  if (!config_save(c)) {
    ESP_LOGE(TAG, "NVS write failed — not leaving Provisioning");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "<!doctype html><h1>Could not store the configuration</h1>",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  staged = c;
  submitted = true;
  ESP_LOGI(TAG, "configuration stored for '%s'", c.ssid);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req,
                  "<!doctype html><h1>Saved</h1><p>The device is restarting and will "
                  "join the network you gave it.</p>",
                  HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Every other path returns the page rather than a 404, so whichever URL the
// client's captive-portal check uses lands on the form (FR-PRV-03).
esp_err_t catch_all(httpd_req_t* req) { return page_get(req); }

// ── DNS ─────────────────────────────────────────────────────────────────────
//
// Answers every A query with the SoftAP's own address. Small enough to write
// out: pulling in a DNS library to return one constant would be more code, not
// less, and the failure modes would be someone else's.

void dns_hijack(void* arg) {
  const uint32_t self = *static_cast<uint32_t*>(arg);
  const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(TAG, "DNS socket failed — clients will not be redirected");
    vTaskDelete(nullptr);
    return;
  }

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(DNS_PORT);
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "DNS bind failed — clients will not be redirected");
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  uint8_t buf[512];
  while (true) {
    sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    const int n = recvfrom(sock, buf, sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 12) continue;   // shorter than a header cannot be a query

    // Reply: flip QR, mark authoritative, one answer. The question is echoed
    // back verbatim, then a pointer to it, type A, class IN, and the address.
    buf[2] = 0x84;   // QR=1, AA=1
    buf[3] = 0x00;
    buf[6] = 0x00; buf[7] = 0x01;   // ANCOUNT = 1
    buf[8] = 0x00; buf[9] = 0x00;   // NSCOUNT
    buf[10] = 0x00; buf[11] = 0x00; // ARCOUNT

    int out = n;
    if (out + 16 > static_cast<int>(sizeof(buf))) continue;
    const uint8_t answer[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                               0x00, 0x00, 0x00, 0x3C, 0x00, 0x04 };
    std::memcpy(buf + out, answer, sizeof(answer));
    out += static_cast<int>(sizeof(answer));
    std::memcpy(buf + out, &self, 4);
    out += 4;

    sendto(sock, buf, static_cast<size_t>(out), 0,
           reinterpret_cast<sockaddr*>(&from), from_len);
  }
}

}  // namespace

bool provisioning_run() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

  char ssid[AP_SSID_MAX + 1];
  char pass[AP_PASSPHRASE_MAX + 1];
  if (ap_ssid(mac, ssid, sizeof(ssid)) == 0 ||
      ap_passphrase(mac, pass, sizeof(pass)) == 0) {
    ESP_LOGE(TAG, "could not derive the access point identity");
    return false;
  }

  // Before any netif exists. Provisioning runs before the station path on a
  // device with no configuration, so it cannot assume someone else has done it.
  net_stack_init();

  esp_netif_t* ap = esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();   // FR-PRV-07 needs a station to scan with

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

  wifi_config_t cfg = {};
  std::memcpy(cfg.ap.ssid, ssid, std::strlen(ssid));
  cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
  std::memcpy(cfg.ap.password, pass, std::strlen(pass));
  cfg.ap.channel = AP_CHANNEL;
  cfg.ap.max_connection = AP_MAX_CLIENTS;
  // FR-SEC-03, FR-PRV-01: never open. An open provisioning network hands the
  // household's WiFi password to anyone who associates while it is up.
  cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "provisioning: SSID %s, WPA2, %u minute timeout",
           ssid, static_cast<unsigned>(TIMEOUT_MS / 60000));

  esp_netif_ip_info_t ip = {};
  esp_netif_get_ip_info(ap, &ip);
  static uint32_t self_addr;
  self_addr = ip.ip.addr;
  xTaskCreate(dns_hijack, "dns", 3072, &self_addr, 5, &dns_task_handle);

  httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
  hcfg.uri_match_fn = httpd_uri_match_wildcard;
  hcfg.lru_purge_enable = true;
  if (httpd_start(&server, &hcfg) != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server failed to start");
    return false;
  }
  // Only the first four members are initialised. The websocket fields exist
  // solely when CONFIG_HTTPD_WS_SUPPORT is on, so naming them makes the file
  // depend on a Kconfig symbol that has nothing to do with this portal.
  httpd_uri_t save = {};
  save.uri = "/save";
  save.method = HTTP_POST;
  save.handler = save_post;

  httpd_uri_t any = {};
  any.uri = "/*";
  any.method = HTTP_GET;
  any.handler = catch_all;
  httpd_register_uri_handler(server, &save);
  httpd_register_uri_handler(server, &any);

  // A line every few seconds while the portal is up.
  //
  // Provisioning used to log once on entry and then sit silently for five
  // minutes, so a bench session could not tell "waiting for a client" from
  // "hung" — and on the first run it was hung, which took a JTAG halt-cause read
  // to establish. Silence is the worst thing a long-running state can report.
  //
  // It carries what a session actually needs: the network to look for, how many
  // clients have associated, and how much of the window is left.
  const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(TIMEOUT_MS) * 1000;
  int64_t next_report = 0;
  while (!submitted && esp_timer_get_time() < deadline) {
    const int64_t now = esp_timer_get_time();
    if (now >= next_report) {
      next_report = now + DIAG_PERIOD_US;
      wifi_sta_list_t clients = {};
      esp_wifi_ap_get_sta_list(&clients);
      ESP_LOGI(TAG, "diag: state=provisioning ssid=%s clients=%d left=%llds",
               ssid, clients.num,
               static_cast<long long>((deadline - now) / 1000000));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  if (!submitted) {
    // FR-PRV-06: nothing was stored, so nothing stored is disturbed.
    ESP_LOGI(TAG, "no submission — stored configuration is unchanged");
  }

  httpd_stop(server);
  server = nullptr;
  if (dns_task_handle) {
    vTaskDelete(dns_task_handle);
    dns_task_handle = nullptr;
  }
  esp_wifi_stop();
  esp_wifi_deinit();
  return submitted;
}

}  // namespace gplug
