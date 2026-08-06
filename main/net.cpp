#include "net.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_event.h"
#include "ota.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "ha_discovery.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace gplug {
namespace {

constexpr const char* TAG = "net";
constexpr EventBits_t GOT_IP = BIT0;

EventGroupHandle_t events;
char mac_str[18] = "00:00:00:00:00:00";
char status_top[96];
char ota_top[96];
char state_top[96];

esp_mqtt_client_handle_t client = nullptr;
volatile bool connected = false;

void on_wifi(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    // Retry, always. Never start a SoftAP here: that is FR-SUP-04, and the
    // reason is that the device lives where nobody can see a portal.
    //
    // The reason code is the whole diagnostic value of this line: 201 is
    // NO_AP_FOUND, 202 AUTH_FAIL, 15 4WAY_HANDSHAKE_TIMEOUT. Without it a
    // wrong password and a missing access point look identical, and both look
    // like "the network is broken".
    const auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
    ESP_LOGW(TAG, "disconnected from '%.*s', reason %d — retrying",
             d ? d->ssid_len : 0, d ? reinterpret_cast<const char*>(d->ssid) : "",
             d ? d->reason : -1);
    connected = false;
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* e = static_cast<ip_event_got_ip_t*>(data);
    ESP_LOGI(TAG, "got " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(events, GOT_IP);
  }
}

void on_mqtt(void*, esp_event_base_t, int32_t id, void* data) {
  const auto* e = static_cast<esp_mqtt_event_handle_t>(data);
  switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "broker connected");
      connected = true;
      // Availability is retained so a subscriber that arrives later still
      // learns the device is up (FR-HA-06).
      esp_mqtt_client_publish(client, status_top, "online", 0, 1, 1);
      // QoS 1: an update command that the broker drops is a command the person
      // who sent it believes was delivered (Appendix D).
      esp_mqtt_client_subscribe(client, ota_top, 1);
      // FR-OTA-05/06. A session, not a successful boot, is what makes a new
      // image valid — an image that boots and cannot reach the broker is
      // precisely the one rollback exists for.
      ota_mark_valid_on_session();
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "broker disconnected, client retries");
      connected = false;
      break;
    case MQTT_EVENT_DATA:
      // FR-OTA-01. The only trigger there is: no polling, no version check, no
      // schedule (FR-OTA-02, D-U2).
      if (e && e->topic_len && std::strncmp(e->topic, ota_top, static_cast<size_t>(e->topic_len)) == 0) {
        if (!ota_handle_command(e->data, static_cast<size_t>(e->data_len))) {
          ESP_LOGW(TAG, "OTA command not acted on — see the reason above");
        }
      }
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGW(TAG, "mqtt error type %d", e ? e->error_handle->error_type : -1);
      break;
    default:
      break;
  }
}

}  // namespace

const char* wifi_mac() { return mac_str; }

void net_stack_init() {
  const esp_err_t netif = esp_netif_init();
  if (netif != ESP_OK && netif != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(netif);
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(loop);
}

void wifi_start_and_wait(const Config& conf) {
  // Storage is initialised in app_main, before the configuration is read.
  net_stack_init();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));

  events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                      &on_wifi, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                      &on_wifi, nullptr, nullptr));

  wifi_config_t cfg = {};
  // Copied, not printed. These are 802.11 octet fields rather than C strings: a
  // legal 32-octet SSID fills ssid[32] exactly and has no room for a terminator,
  // so snprintf would truncate it to 31 — silently turning a valid SSID into one
  // that matches no network. config.h has already rejected anything longer.
  std::memcpy(cfg.sta.ssid, conf.ssid,
              std::min(std::strlen(conf.ssid), sizeof(cfg.sta.ssid)));
  std::memcpy(cfg.sta.password, conf.passphrase,
              std::min(std::strlen(conf.passphrase), sizeof(cfg.sta.password)));

  // An open network has no passphrase, and demanding WPA2 of one makes it
  // unjoinable in a way that presents as bad credentials (config.h).
  const bool open_network = conf.passphrase[0] == '\0';

  // Stated rather than inherited from a zero-initialised struct: the defaults
  // for these two have changed between IDF versions, and both failure modes
  // present as a plain disconnect.
  cfg.sta.threshold.authmode = open_network ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  cfg.sta.pmf_cfg.capable = true;
  cfg.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  // No modem sleep. ESP-IDF defaults a station to WIFI_PS_MIN_MODEM, which
  // sleeps the radio between beacons — and an access point that does not see a
  // sleeping station answer will disassociate it for inactivity.
  //
  // Measured 2026-08-06 on two different benches with two different radios:
  //
  //   hostapd: STA ... associated
  //   hostapd: STA ... disassociated        four seconds later, repeatedly
  //   device:  disconnected, reason 4 (association expired), then 201
  //
  // No brownout and no reset in any of it — five minutes of uptime while the
  // link came and went, so this was never the supply. It is intermittent
  // because it depends on beacon timing, which is why the device sometimes held
  // a lease for minutes and sometimes could not stay up for four seconds.
  //
  // There is nothing to trade away here: FR-MTR has the device powered from the
  // meter's own 5 V rail, it is never on a battery, and it must publish within
  // 2 s of a cycle boundary (NFR-HA-01). Sleeping between beacons buys power
  // this product does not need and costs the connection it exists to keep.
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  uint8_t mac[6] = {};
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
  std::snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  ESP_LOGI(TAG, "station %s joining '%s'", mac_str, conf.ssid);

  xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
}

void mqtt_start(const Config& conf) {
  status_topic(status_top, sizeof(status_top), mac_str);
  std::snprintf(ota_top, sizeof(ota_top), "gplug/%s/cmd/ota", mac_str);
  state_topic(state_top, sizeof(state_top), mac_str);

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = conf.broker;
  cfg.credentials.client_id = mac_str;          // FR-HA-08
  // Registered before connecting: the broker publishes it when the connection
  // drops without a DISCONNECT, which is what a power cut looks like.
  cfg.session.last_will.topic = status_top;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;

  client = esp_mqtt_client_init(&cfg);
  ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, &on_mqtt, nullptr));
  ESP_ERROR_CHECK(esp_mqtt_client_start(client));
  ESP_LOGI(TAG, "broker %s, client id %s", conf.broker, mac_str);
}

bool mqtt_connected() { return connected; }

void mqtt_publish_discovery(const char* topic, const char* payload) {
  if (client == nullptr) return;
  esp_mqtt_client_publish(client, topic, payload, 0, 1, 1);   // QoS 1, retained
}

void mqtt_publish_state(const char* payload) {
  if (client == nullptr) return;
  esp_mqtt_client_publish(client, state_top, payload, 0, 0, 0); // QoS 0, not retained
}

}  // namespace gplug
