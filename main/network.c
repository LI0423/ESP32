#include "network.h"

#include <stdint.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "companion_model.h"
#include "lcd.h"
#include "mdns.h"
#include "provisioning.h"
#include "sdkconfig.h"

#define TAG "companion_net"
#define WIFI_RETRY_BASE_MS 1000
#define WIFI_RETRY_MAX_MS 32000
#define GOT_IP_TIMEOUT_MS 15000
#define RESTART_DELAY_MS 500
#define SNTP_RETRY_MS 5000
#define TIME_VALID_MS (24u * 60u * 60u * 1000u)

#ifndef CONFIG_COMPANION_AP_PASSWORD
#define CONFIG_COMPANION_AP_PASSWORD "deskpal"
#endif
#ifndef CONFIG_COMPANION_NTP_SERVER
#define CONFIG_COMPANION_NTP_SERVER "pool.ntp.org"
#endif

static SemaphoreHandle_t s_lock;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static companion_config_t s_config;
static volatile bool s_initialized;
static volatile bool s_wifi_started;
static volatile bool s_sta_desired;
static volatile bool s_connected;
static volatile bool s_wifi_restarting;
static volatile bool s_waiting_ip;
static volatile uint32_t s_retry_at_ms;
static volatile uint32_t s_got_ip_deadline_ms;
static volatile uint32_t s_retry_count;
static volatile bool s_restart_pending;
static volatile uint32_t s_restart_at_ms;
static volatile bool s_time_valid;
static uint32_t s_last_sync_ms;
static bool s_sntp_started;
static uint32_t s_sntp_retry_at_ms;
static bool s_web_started;
static uint32_t s_web_start_at_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void apply_brightness(uint8_t brightness)
{
    if (brightness < 5) brightness = 5;
    if (brightness > 100) brightness = 100;
    lcd_set_brightness(brightness);
}

static void copy_wifi_bytes(char *destination, size_t capacity,
                            const char *source, size_t source_capacity)
{
    size_t length = strnlen(source, source_capacity);
    if (length > capacity) length = capacity;
    memcpy(destination, source, length);
}

static void schedule_reconnect(void)
{
    if (!s_sta_desired || s_wifi_restarting || s_connected) return;
    uint32_t shift = s_retry_count < 5 ? s_retry_count : 5;
    uint32_t delay = WIFI_RETRY_BASE_MS << shift;
    if (delay > WIFI_RETRY_MAX_MS) delay = WIFI_RETRY_MAX_MS;
    s_retry_count++;
    s_retry_at_ms = now_ms() + delay;
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_last_sync_ms = now_ms();
    s_time_valid = true;
    ESP_LOGI(TAG, "NTP time synchronized");
}

static esp_err_t start_sntp(void)
{
    if (s_sntp_started) return ESP_OK;
#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 2
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST(CONFIG_COMPANION_NTP_SERVER, "162.159.200.1"));
#else
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_COMPANION_NTP_SERVER);
#endif
    config.sync_cb = sntp_sync_cb;
    config.server_from_dhcp = false;
    config.renew_servers_after_new_IP = false;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started");
    } else {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        s_waiting_ip = true;
        s_got_ip_deadline_ms = now_ms() + GOT_IP_TIMEOUT_MS;
        s_retry_at_ms = 0;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_waiting_ip = false;
        if (s_sta_desired && !s_wifi_restarting) {
            if (event_data) {
                const wifi_event_sta_disconnected_t *event = event_data;
                ESP_LOGW(TAG, "STA disconnected (reason=%d)", event->reason);
            }
            if (s_retry_at_ms == 0) schedule_reconnect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_waiting_ip = false;
        s_retry_count = 0;
        s_retry_at_ms = 0;
        esp_netif_ip_info_t ip_info = {0};
        char ip[16] = "-";
        if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK)
            esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
        ESP_LOGI(TAG, "STA got IP: %s", ip);
        s_sntp_retry_at_ms = now_ms();
    }
}

static esp_err_t make_sta_config(const companion_config_t *config, wifi_config_t *wifi)
{
    copy_wifi_bytes((char *)wifi->sta.ssid, sizeof(wifi->sta.ssid),
                    config->wifi_ssid, sizeof(config->wifi_ssid));
    copy_wifi_bytes((char *)wifi->sta.password, sizeof(wifi->sta.password),
                    config->wifi_password, sizeof(config->wifi_password));
    wifi->sta.threshold.authmode = config->wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi->sta.pmf_cfg.capable = true;
    wifi->sta.pmf_cfg.required = false;
    return ESP_OK;
}

static esp_err_t make_ap_config(wifi_config_t *wifi)
{
    const char *ssid = "DeskPal-Setup";
    memcpy(wifi->ap.ssid, ssid, strlen(ssid));
    wifi->ap.ssid_len = strlen(ssid);
    wifi->ap.channel = 6;
    wifi->ap.max_connection = 4;
    const char *password = CONFIG_COMPANION_AP_PASSWORD;
    size_t password_len = strlen(password);
    ESP_LOGI(TAG, "AP password length=%zu", password_len);
    if (password_len < 8 || password_len > 64) {
        ESP_LOGE(TAG, "AP password must be 8..64 bytes");
        return ESP_ERR_INVALID_ARG;
    }
    copy_wifi_bytes((char *)wifi->ap.password, sizeof(wifi->ap.password),
                    password, password_len + 1);
    wifi->ap.authmode = WIFI_AUTH_WPA2_PSK;
    return ESP_OK;
}

static esp_err_t start_wifi(const companion_config_t *config)
{
    wifi_config_t ap = {0};
    wifi_config_t sta = {0};
    esp_err_t err = make_ap_config(&ap);
    if (err != ESP_OK) return err;
    bool use_sta = config->wifi_ssid[0] != '\0';
    if (use_sta) {
        err = make_sta_config(config, &sta);
        if (err != ESP_OK) return err;
    }

    err = esp_wifi_set_mode(use_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK && use_sta) err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err == ESP_OK && !s_wifi_started) {
        err = esp_wifi_start();
        s_wifi_started = err == ESP_OK;
    }
    if (err != ESP_OK) return err;

    s_sta_desired = use_sta;
    s_connected = false;
    s_waiting_ip = false;
    s_retry_count = 0;
    s_retry_at_ms = 0;
    if (use_sta) {
        err = esp_wifi_connect();
        if (err != ESP_OK) schedule_reconnect();
    }
    return err;
}

static esp_err_t restart_wifi(const companion_config_t *config)
{
    s_sta_desired = false;
    s_connected = false;
    s_waiting_ip = false;
    s_retry_at_ms = 0;
    if (s_wifi_started) {
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) return err;
        s_wifi_started = false;
    }
    return start_wifi(config);
}

static void wifi_reconnect_tick(void)
{
    if (!s_initialized || s_wifi_restarting || !s_sta_desired || s_connected) return;
    uint32_t timestamp = now_ms();
    if (s_waiting_ip) {
        if ((int32_t)(timestamp - s_got_ip_deadline_ms) >= 0) {
            s_waiting_ip = false;
            esp_wifi_disconnect();
            schedule_reconnect();
        }
        return;
    }
    if (s_retry_at_ms != 0 && (int32_t)(timestamp - s_retry_at_ms) >= 0) {
        s_retry_at_ms = 0;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) schedule_reconnect();
    }
}

void companion_network_init(void)
{
    if (s_initialized) return;
    if (companion_config_init() != ESP_OK || companion_config_get(&s_config) != ESP_OK) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err != ESP_OK) return;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) return;
    esp_netif_set_hostname(s_sta_netif, "deskpal");
    esp_netif_set_hostname(s_ap_netif, "deskpal");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) return;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (err == ESP_OK)
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    if (err != ESP_OK) return;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_web_started = false;
    s_web_start_at_ms = now_ms() + (s_config.wifi_ssid[0] ? 10000u : 0u);
    s_initialized = true;
    err = restart_wifi(&s_config);
    if (err != ESP_OK) ESP_LOGE(TAG, "initial Wi-Fi start failed: %s", esp_err_to_name(err));
    if (!s_config.wifi_ssid[0]) {
        companion_provisioning_start();
        s_web_started = true;
    }
    apply_brightness(s_config.brightness);
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("deskpal");
        mdns_instance_name_set("Desktop Companion");
        mdns_service_add(NULL, "_http", "_tcp", 443, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }
    ESP_LOGI(TAG, "network ready (%s)", s_config.wifi_ssid[0] ? "STA" : "setup AP");
}

void companion_network_tick(void)
{
    if (!s_initialized) return;
    if (s_restart_pending && (int32_t)(now_ms() - s_restart_at_ms) >= 0) {
        s_restart_pending = false;
        esp_restart();
    }
    wifi_reconnect_tick();
    uint32_t timestamp = now_ms();
    if (!s_web_started && (s_connected || (int32_t)(timestamp - s_web_start_at_ms) >= 0)) {
        companion_provisioning_start();
        s_web_started = true;
    }
    if (s_connected && !s_sntp_started && s_sntp_retry_at_ms != 0 &&
        (int32_t)(timestamp - s_sntp_retry_at_ms) >= 0) {
        s_sntp_retry_at_ms = timestamp + SNTP_RETRY_MS;
        if (start_sntp() != ESP_OK) s_sntp_retry_at_ms = timestamp + SNTP_RETRY_MS;
    }
}

bool companion_network_is_connected(void)
{
    return s_initialized && s_connected;
}

bool companion_network_time_valid(void)
{
    return s_time_valid && (uint32_t)(now_ms() - s_last_sync_ms) < TIME_VALID_MS;
}

esp_err_t companion_network_apply_config(const companion_config_t *config)
{
    if (!s_initialized || !s_lock || !config) return ESP_ERR_INVALID_STATE;
    esp_err_t err = companion_config_validate(config);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool wifi_changed = strcmp(s_config.wifi_ssid, config->wifi_ssid) != 0 ||
                        strcmp(s_config.wifi_password, config->wifi_password) != 0;
    bool brightness_changed = s_config.brightness != config->brightness;
    s_wifi_restarting = wifi_changed;
    xSemaphoreGive(s_lock);

    err = companion_config_set(config);
    if (err != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_wifi_restarting = false;
        xSemaphoreGive(s_lock);
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = *config;
    xSemaphoreGive(s_lock);
    if (brightness_changed) {
        companion_model_set_configured_brightness(config->brightness);
        /* Effective level (incl. standby dim) is applied by the service task. */
    }
    if (wifi_changed) {
        err = restart_wifi(config);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_wifi_restarting = false;
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t companion_network_set_brightness(uint8_t brightness)
{
    if (brightness < 5 || brightness > 100) return ESP_ERR_INVALID_ARG;
    companion_config_t next;
    esp_err_t err = companion_config_get(&next);
    if (err != ESP_OK) return err;
    next.brightness = brightness;
    err = companion_config_set(&next);
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = next;
    xSemaphoreGive(s_lock);
    companion_model_set_configured_brightness(brightness);
    return ESP_OK;
}

void companion_network_request_restart(void)
{
    s_restart_at_ms = now_ms() + RESTART_DELAY_MS;
    s_restart_pending = true;
}
