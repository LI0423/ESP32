#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "companion_progress.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMPANION_SSID_MAX       32
#define COMPANION_WIFI_PASS_MAX  64
#define COMPANION_URL_MAX        256
#define COMPANION_ADMIN_TOKEN_MAX 64

typedef struct {
    char wifi_ssid[COMPANION_SSID_MAX + 1];
    char wifi_password[COMPANION_WIFI_PASS_MAX + 1];
    char ota_url[COMPANION_URL_MAX + 1];
    char admin_token[COMPANION_ADMIN_TOKEN_MAX + 1];
    uint8_t brightness;
    uint8_t daily_goal;
} companion_config_t;

typedef struct {
    bool valid;
    uint8_t phase;
    uint32_t remaining_seconds;
    uint32_t completed_sessions;
    uint32_t focus_seconds;
    uint32_t break_seconds;
    uint32_t celebrate_seconds;
    int64_t saved_epoch;
    int64_t standby_until_epoch;
    int32_t day_key;
    bool work_hours_override;
    uint32_t streak;
    bool goal_met_today;
} companion_timer_record_t;

esp_err_t companion_config_init(void);
esp_err_t companion_config_get(companion_config_t *out);
esp_err_t companion_config_set(const companion_config_t *config);
esp_err_t companion_config_set_wifi(const char *ssid, const char *password, bool clear_password);
esp_err_t companion_config_set_brightness(uint8_t brightness);
esp_err_t companion_config_validate(const companion_config_t *config);
bool companion_config_has_wifi(void);
bool companion_config_url_valid(const char *url);

esp_err_t companion_store_load_timer(companion_timer_record_t *out);
esp_err_t companion_store_save_timer(const companion_timer_record_t *record);

/* The old Desktop Status Pad used the same namespace and Wi-Fi keys. */
#define PAD_CONFIG_URL_MAX COMPANION_URL_MAX

#ifdef __cplusplus
}
#endif
