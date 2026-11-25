#include "config_store.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "companion_cfg"
#define NVS_NAMESPACE "agentpad"

#ifndef CONFIG_PAD_DEFAULT_BRIGHTNESS
#define CONFIG_PAD_DEFAULT_BRIGHTNESS 75
#endif

static SemaphoreHandle_t s_lock;
static companion_config_t s_config;
static bool s_loaded;

static int clamp_int(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static bool text_valid(const char *text, size_t max_len, bool allow_empty)
{
    if (!text) return false;
    size_t len = strnlen(text, max_len + 1);
    if (len > max_len || (!allow_empty && len == 0)) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool companion_config_url_valid(const char *url)
{
    if (!url) return false;
    if (url[0] == '\0') return true;
    if (!text_valid(url, COMPANION_URL_MAX, false)) return false;
    const char *host = NULL;
    if (!strncmp(url, "http://", 7)) host = url + 7;
    else if (!strncmp(url, "https://", 8)) host = url + 8;
    else return false;
    if (*host == '\0' || *host == '/' || *host == '?' || *host == '#') return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p == '\\' || *p == '"' || *p == '<' || *p == '>' || *p == ' ') return false;
    }
    return true;
}

esp_err_t companion_config_validate(const companion_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!text_valid(config->wifi_ssid, COMPANION_SSID_MAX, true) ||
        !text_valid(config->wifi_password, COMPANION_WIFI_PASS_MAX, true) ||
        !text_valid(config->admin_token, COMPANION_ADMIN_TOKEN_MAX, true) ||
        !companion_config_url_valid(config->ota_url)) return ESP_ERR_INVALID_ARG;
    if (config->brightness < 5 || config->brightness > 100) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static void config_defaults(companion_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->brightness = (uint8_t)clamp_int(CONFIG_PAD_DEFAULT_BRIGHTNESS, 5, 100);
}

static esp_err_t read_string(nvs_handle_t nvs, const char *key, char *out,
                             size_t capacity, const char *fallback)
{
    strlcpy(out, fallback, capacity);
    size_t length = 0;
    esp_err_t err = nvs_get_blob(nvs, key, NULL, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH ||
        err == ESP_ERR_INVALID_SIZE) return ESP_OK;
    if (err != ESP_OK) return err;
    if (length == 0 || length >= capacity) return ESP_OK;
    err = nvs_get_blob(nvs, key, out, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH ||
        err == ESP_ERR_INVALID_SIZE) {
        strlcpy(out, fallback, capacity);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    if (length == 0 || memchr(out, '\0', length) == NULL || out[length - 1] != '\0')
        strlcpy(out, fallback, capacity);
    return ESP_OK;
}

static esp_err_t read_i32(nvs_handle_t nvs, const char *key, int *out, int fallback)
{
    *out = fallback;
    int32_t value = 0;
    esp_err_t err = nvs_get_i32(nvs, key, &value);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH ||
        err == ESP_ERR_INVALID_SIZE) return ESP_OK;
    if (err != ESP_OK) return err;
    *out = (int)value;
    return ESP_OK;
}

static esp_err_t load_config(companion_config_t *config)
{
    companion_config_t defaults;
    config_defaults(&defaults);
    *config = defaults;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    err = read_string(nvs, "wifi_ssid", config->wifi_ssid,
                      sizeof(config->wifi_ssid), defaults.wifi_ssid);
    if (err == ESP_OK)
        err = read_string(nvs, "wifi_pass", config->wifi_password,
                          sizeof(config->wifi_password), defaults.wifi_password);
    if (err == ESP_OK)
        err = read_string(nvs, "ota_url", config->ota_url,
                          sizeof(config->ota_url), defaults.ota_url);
    if (err == ESP_OK)
        err = read_string(nvs, "admin_token_v2", config->admin_token,
                          sizeof(config->admin_token), defaults.admin_token);
    int brightness = defaults.brightness;
    if (err == ESP_OK) err = read_i32(nvs, "brightness", &brightness, defaults.brightness);
    if (err == ESP_OK) config->brightness = (uint8_t)clamp_int(brightness, 5, 100);
    nvs_close(nvs);
    if (err != ESP_OK) return err;

    if (companion_config_validate(config) != ESP_OK) {
        ESP_LOGW(TAG, "stored configuration invalid; using safe defaults");
        *config = defaults;
    }
    return ESP_OK;
}

static esp_err_t write_string(nvs_handle_t nvs, const char *key, const char *value)
{
    if (value[0] == '\0') {
        esp_err_t err = nvs_erase_key(nvs, key);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    return nvs_set_blob(nvs, key, value, strlen(value) + 1);
}

static esp_err_t store_config(const companion_config_t *config)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = write_string(nvs, "wifi_ssid", config->wifi_ssid);
    if (err == ESP_OK) err = write_string(nvs, "wifi_pass", config->wifi_password);
    if (err == ESP_OK) err = write_string(nvs, "ota_url", config->ota_url);
    if (err == ESP_OK) err = write_string(nvs, "admin_token_v2", config->admin_token);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "brightness", config->brightness);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t ensure_admin_token_locked(void)
{
    if (s_config.admin_token[0]) return ESP_OK;
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    uint32_t c = esp_random();
    snprintf(s_config.admin_token, sizeof(s_config.admin_token),
             "%08lx%08lx%08lx", (unsigned long)a, (unsigned long)b, (unsigned long)c);
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = write_string(nvs, "admin_token_v2", s_config.admin_token);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK)
        ESP_LOGW(TAG, "Admin token (also shown once): %s", s_config.admin_token);
    return err;
}

esp_err_t companion_config_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_loaded) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase/reinit");
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err == ESP_OK) err = load_config(&s_config);
    if (err == ESP_OK) err = ensure_admin_token_locked();
    if (err == ESP_OK) s_loaded = true;
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGE(TAG, "config init failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t companion_config_get(companion_config_t *out)
{
    if (!out || !s_lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_loaded) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_config;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t companion_config_set(const companion_config_t *config)
{
    if (!config || !s_lock) return ESP_ERR_INVALID_ARG;
    esp_err_t err = companion_config_validate(config);
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_loaded) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    err = store_config(config);
    if (err == ESP_OK) s_config = *config;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t companion_config_set_wifi(const char *ssid, const char *password, bool clear_password)
{
    companion_config_t next;
    esp_err_t err = companion_config_get(&next);
    if (err != ESP_OK) return err;
    if (ssid) {
        if (!text_valid(ssid, COMPANION_SSID_MAX, true)) return ESP_ERR_INVALID_ARG;
        strlcpy(next.wifi_ssid, ssid, sizeof(next.wifi_ssid));
    }
    if (clear_password) {
        if (password && password[0]) return ESP_ERR_INVALID_ARG;
        next.wifi_password[0] = '\0';
    } else if (password && password[0]) {
        if (!text_valid(password, COMPANION_WIFI_PASS_MAX, false)) return ESP_ERR_INVALID_ARG;
        strlcpy(next.wifi_password, password, sizeof(next.wifi_password));
    }
    return companion_config_set(&next);
}

esp_err_t companion_config_set_brightness(uint8_t brightness)
{
    if (brightness < 5 || brightness > 100) return ESP_ERR_INVALID_ARG;
    companion_config_t next;
    esp_err_t err = companion_config_get(&next);
    if (err != ESP_OK) return err;
    next.brightness = brightness;
    return companion_config_set(&next);
}

bool companion_config_has_wifi(void)
{
    companion_config_t config;
    return companion_config_get(&config) == ESP_OK && config.wifi_ssid[0] != '\0';
}

esp_err_t companion_store_load_timer(companion_timer_record_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    int version = 0;
    err = read_i32(nvs, "tm_version", &version, 0);
    int phase = -1, remaining = -1, completed = -1;
    int focus = -1, break_seconds = -1, celebrate = -1;
    if (err == ESP_OK && version == 1) {
        err = read_i32(nvs, "tm_phase", &phase, -1);
        if (err == ESP_OK) err = read_i32(nvs, "tm_remaining", &remaining, -1);
        if (err == ESP_OK) err = read_i32(nvs, "tm_completed", &completed, -1);
        if (err == ESP_OK) err = read_i32(nvs, "tm_focus", &focus, -1);
        if (err == ESP_OK) err = read_i32(nvs, "tm_break", &break_seconds, -1);
        if (err == ESP_OK) err = read_i32(nvs, "tm_celebrate", &celebrate, -1);
    }
    int64_t saved_epoch = 0;
    if (err == ESP_OK && version == 1) {
        err = nvs_get_i64(nvs, "tm_saved_epoch", &saved_epoch);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            saved_epoch = 0;
            err = ESP_OK;
        } else if (err == ESP_ERR_NVS_TYPE_MISMATCH || err == ESP_ERR_INVALID_SIZE) {
            saved_epoch = 0;
            err = ESP_OK;
        }
    }
    nvs_close(nvs);
    if (err != ESP_OK) return err;

    uint32_t phase_total = 0;
    if (phase == 0) phase_total = (uint32_t)focus;
    else if (phase == 1) phase_total = (uint32_t)celebrate;
    else if (phase == 2) phase_total = (uint32_t)break_seconds;
    bool valid = version == 1 && phase >= 0 && phase <= 2 &&
                 remaining > 0 && (uint32_t)remaining <= phase_total &&
                 completed >= 0 && completed <= 1000000 &&
                 focus >= 5 && focus <= 86400 &&
                 break_seconds >= 5 && break_seconds <= 86400 &&
                 celebrate >= 1 && celebrate <= 3600 && saved_epoch >= 0;
    if (valid) {
        out->valid = true;
        out->phase = (uint8_t)phase;
        out->remaining_seconds = (uint32_t)remaining;
        out->completed_sessions = (uint32_t)completed;
        out->focus_seconds = (uint32_t)focus;
        out->break_seconds = (uint32_t)break_seconds;
        out->celebrate_seconds = (uint32_t)celebrate;
        out->saved_epoch = saved_epoch;
    }
    return ESP_OK;
}

esp_err_t companion_store_save_timer(const companion_timer_record_t *record)
{
    if (!record) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(nvs, "tm_version", 1);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "tm_phase", record->phase);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "tm_remaining", (int32_t)record->remaining_seconds);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "tm_completed", (int32_t)record->completed_sessions);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "tm_focus", (int32_t)record->focus_seconds);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "tm_break", (int32_t)record->break_seconds);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "tm_celebrate", (int32_t)record->celebrate_seconds);
    if (err == ESP_OK) err = nvs_set_i64(nvs, "tm_saved_epoch", record->saved_epoch);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
