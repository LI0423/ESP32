#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "companion_model.h"
#include "companion_schedule.h"
#include "companion_timer.h"
#include "companion_ui.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd.h"
#include "network.h"
#include "ota.h"
#include "sdkconfig.h"

#define TAG "deskpal"
#define SERVICE_PERIOD_MS 50u
#define UI_PERIOD_MS 100u

static SemaphoreHandle_t s_output_lock;
static companion_timer_t s_boot_timer;
static companion_timer_record_t s_boot_record;
static bool s_restore_pending;
static bool s_timer_dirty;

static void publish_status(void)
{
    companion_state_t state;
    companion_model_snapshot(&state);
    char line[320];
    int length = snprintf(line, sizeof(line),
        "{\"v\":1,\"type\":\"status\",\"phase\":\"%s\",\"remaining\":%lu,"
        "\"total\":%lu,\"completed\":%lu,\"online\":%s,\"time_valid\":%s,"
        "\"standby\":%s,\"clock\":\"%s\",\"date\":\"%s\"}\n",
        companion_phase_name(state.phase),
        (unsigned long)state.remaining_seconds,
        (unsigned long)state.total_seconds,
        (unsigned long)state.completed_sessions,
        state.online ? "true" : "false",
        state.time_valid ? "true" : "false",
        state.standby ? "true" : "false",
        state.clock, state.date);
    if (length <= 0 || (size_t)length >= sizeof(line)) return;
    xSemaphoreTake(s_output_lock, portMAX_DELAY);
    fwrite(line, 1, (size_t)length, stdout);
    fflush(stdout);
    xSemaphoreGive(s_output_lock);
}

static void save_current_timer(void)
{
    companion_persist_snapshot_t snap;
    companion_model_persist_snapshot(&snap);
    companion_state_t state;
    companion_model_snapshot(&state);
    companion_timer_record_t record = {
        .phase = (uint8_t)snap.phase,
        .remaining_seconds = snap.remaining_seconds,
        .completed_sessions = snap.completed_sessions,
        .focus_seconds = CONFIG_COMPANION_FOCUS_SECONDS,
        .break_seconds = CONFIG_COMPANION_BREAK_SECONDS,
        .celebrate_seconds = CONFIG_COMPANION_CELEBRATE_SECONDS,
        .saved_epoch = state.time_valid ? (int64_t)time(NULL) : 0,
        .standby_until_epoch = snap.standby_until_epoch,
        .day_key = snap.day_key,
        .work_hours_override = snap.work_hours_override,
    };
    esp_err_t err = companion_store_save_timer(&record);
    if (err == ESP_OK) s_timer_dirty = false;
    else {
        s_timer_dirty = true;
        ESP_LOGW(TAG, "timer save failed: %s", esp_err_to_name(err));
    }
}

/*
 * Boot restore uses the stale-save-gap policy in companion_schedule.h.
 * We never fast-forward up to 7 days of wall clock or invent completed sessions.
 */
static void restore_from_epoch(void)
{
    if (!s_restore_pending || !companion_network_time_valid()) return;
    s_restore_pending = false;
    time_t now = time(NULL);
    companion_model_apply_boot_policy(true, (int64_t)now, s_boot_record.saved_epoch);
    s_timer_dirty = true;
}

static void apply_brightness_if_needed(void)
{
    uint8_t percent = 0;
    if (!companion_model_brightness_changed(&percent)) return;
    lcd_set_brightness(percent);
}

static void service_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    int64_t last_us = esp_timer_get_time();
    uint32_t elapsed_ms = 0;
    uint32_t last_save_attempt = 0;
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        companion_network_tick();
        pad_ota_tick();
        bool online = companion_network_is_connected();
        bool time_valid = companion_network_time_valid();
        companion_model_set_network(online);
        companion_model_set_time(time_valid);
        /* Apply boot policy before ticking so schedule rules do not race restore. */
        restore_from_epoch();

        int64_t now_us = esp_timer_get_time();
        int64_t delta_us = now_us - last_us;
        last_us = now_us;
        if (delta_us > 0) {
            elapsed_ms += (uint32_t)(delta_us / 1000);
            while (elapsed_ms >= 1000u) {
                elapsed_ms -= 1000u;
                if (companion_model_tick(1)) save_current_timer();
            }
        }
        if (companion_model_persist_requested()) save_current_timer();
        apply_brightness_if_needed();
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_timer_dirty && (uint32_t)(now_ms - last_save_attempt) >= 5000u) {
            save_current_timer();
            last_save_attempt = now_ms;
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SERVICE_PERIOD_MS));
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t last_status = 0;
    uint32_t last_ota_render = 0;
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (!pad_ota_is_busy() || (uint32_t)(now - last_ota_render) >= 1000u) {
            companion_state_t state;
            companion_model_snapshot(&state);
            companion_ui_render(&state);
            last_ota_render = now;
        }
        if ((uint32_t)(now - last_status) >= 1000u) {
            publish_status();
            last_status = now;
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(UI_PERIOD_MS));
    }
}

void app_main(void)
{
    s_output_lock = xSemaphoreCreateMutex();
    if (!s_output_lock) return;
    setenv("TZ", CONFIG_COMPANION_TIMEZONE, 1);
    tzset();

    esp_err_t err = companion_config_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "config unavailable: %s", esp_err_to_name(err));
    companion_config_t config;
    uint8_t brightness = 75;
    if (companion_config_get(&config) == ESP_OK) {
        brightness = config.brightness;
        lcd_init();
        lcd_set_brightness(brightness);
    } else {
        lcd_init();
    }

    companion_timer_reset(&s_boot_timer);
    memset(&s_boot_record, 0, sizeof(s_boot_record));
    err = companion_store_load_timer(&s_boot_record);
    if (err == ESP_OK && s_boot_record.valid &&
        s_boot_record.focus_seconds == s_boot_timer.focus_seconds &&
        s_boot_record.break_seconds == s_boot_timer.break_seconds &&
        s_boot_record.celebrate_seconds == s_boot_timer.celebrate_seconds) {
        companion_timer_restore(&s_boot_timer, (companion_phase_t)s_boot_record.phase,
                                s_boot_record.remaining_seconds, s_boot_record.completed_sessions);
    } else {
        s_boot_record.saved_epoch = 0;
        s_boot_record.standby_until_epoch = 0;
        s_boot_record.day_key = 0;
        s_boot_record.work_hours_override = false;
    }
    s_restore_pending = true;
    companion_model_init(&s_boot_timer);
    companion_model_set_configured_brightness(brightness);
    companion_model_load_schedule(s_boot_record.day_key, s_boot_record.standby_until_epoch,
                                  s_boot_record.work_hours_override);
    companion_ui_init();
    pad_ota_init();

    TaskHandle_t ui_handle = NULL;
    TaskHandle_t service_handle = NULL;
    if (xTaskCreate(ui_task, "deskpal_ui", 8192, NULL, 5, &ui_handle) != pdPASS) {
        ESP_LOGE(TAG, "UI task create failed");
        return;
    }
    if (xTaskCreate(service_task, "deskpal_service", 8192, NULL, 3, &service_handle) != pdPASS) {
        ESP_LOGE(TAG, "service task create failed");
        vTaskDelete(ui_handle);
        return;
    }
    companion_network_init();

    printf("\n=== Desktop Companion v1 ===\n");
    printf("Automatic focus/break timer; no physical controls.\n\n");
    fflush(stdout);

    char command[32];
    size_t command_length = 0;
    for (;;) {
        char chunk[64];
        ssize_t count = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (ssize_t i = 0; i < count; i++) {
            if (chunk[i] == '\n' || chunk[i] == '\r') {
                command[command_length] = '\0';
                if (!strcmp(command, "token")) {
                    companion_config_t current;
                    if (companion_config_get(&current) == ESP_OK)
                        printf("Admin token: %s\n", current.admin_token);
                } else if (!strcmp(command, "status")) {
                    publish_status();
                }
                command_length = 0;
            } else if (command_length < sizeof(command) - 1) {
                command[command_length++] = chunk[i];
            } else {
                command_length = 0;
            }
        }
        fflush(stdout);
    }
}
