#include "ota.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config_store.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#define TAG "pad_ota"
#define OTA_BUFFER_SIZE 4096
#define OTA_REBOOT_DELAY_MS 3000
#define OTA_TOTAL_TIMEOUT_US (180ll * 1000ll * 1000ll)
#define OTA_NO_PROGRESS_MS 15000u
#define OTA_EAGAIN_LIMIT 20u

#ifndef CONFIG_COMPANION_OTA_ALLOW_HTTP
#define CONFIG_COMPANION_OTA_ALLOW_HTTP 0
#endif

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_work;
static bool s_task_started;
static pad_ota_status_t s_status;
static char s_url[PAD_CONFIG_URL_MAX + 1];
static volatile bool s_reboot_now;
static volatile bool s_cancel_requested;
static TickType_t s_ready_at;

typedef struct {
    int64_t content_length;
    bool invalid_length;
} ota_http_headers_t;

static esp_err_t ota_http_event(esp_http_client_event_t *event)
{
    ota_http_headers_t *headers = (ota_http_headers_t *)event->user_data;
    if (!headers || event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key ||
        !event->header_value || strcasecmp(event->header_key, "Content-Length") != 0) return ESP_OK;

    errno = 0;
    char *end = NULL;
    unsigned long long length = strtoull(event->header_value, &end, 10);
    if (errno != 0 || end == event->header_value || length > (unsigned long long)INT64_MAX) {
        headers->invalid_length = true;
        return ESP_OK;
    }
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') {
        headers->invalid_length = true;
        return ESP_OK;
    }
    headers->content_length = (int64_t)length;
    return ESP_OK;
}

static void set_failure(esp_ota_handle_t handle, esp_err_t error)
{
    if (handle) esp_ota_abort(handle);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = PAD_OTA_FAILED;
    s_status.error = error;
    s_reboot_now = false;
    s_cancel_requested = false;
    xSemaphoreGive(s_lock);
    ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(error));
}

static void download_and_apply(const char *url)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(running);
    if (!update) {
        set_failure(0, ESP_ERR_NOT_SUPPORTED);
        return;
    }

    ota_http_headers_t headers = {.content_length = -1};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .event_handler = ota_http_event,
        .user_data = &headers,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
#if CONFIG_COMPANION_OTA_ALLOW_HTTP
        .max_redirection_count = 3,
#else
        .max_redirection_count = 0,
#endif
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        set_failure(0, ESP_ERR_NO_MEM);
        return;
    }

    esp_ota_handle_t handle = 0;
    char *buffer = malloc(OTA_BUFFER_SIZE);
    if (!buffer) {
        esp_http_client_cleanup(client);
        set_failure(0, ESP_ERR_NO_MEM);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    int64_t response_length = -1;
    int status = 0;
    if (err == ESP_OK) {
        int64_t header_result = esp_http_client_fetch_headers(client);
        if (header_result == -ESP_ERR_HTTP_EAGAIN) err = ESP_ERR_HTTP_READ_TIMEOUT;
        else if (header_result < 0) err = ESP_ERR_HTTP_FETCH_HEADER;
        else response_length = header_result > 0 ? header_result : -1;
        status = esp_http_client_get_status_code(client);
    }
    if (headers.content_length >= 0) response_length = headers.content_length;

    if (err != ESP_OK) {
        err = (err == ESP_ERR_HTTP_CONNECTING) ? ESP_ERR_HTTP_CONNECT : err;
        goto failed;
    }
    ESP_LOGI(TAG, "HTTP connected, status=%d length=%lld", status, (long long)response_length);
    if (status != HttpStatus_Ok) {
        err = ESP_FAIL;
        goto failed;
    }
    if (headers.invalid_length || response_length == 0 ||
        (response_length > 0 && (uint64_t)response_length > update->size)) {
        err = ESP_ERR_INVALID_SIZE;
        goto failed;
    }

    size_t total = (response_length > 0) ? (size_t)response_length : 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.total_bytes = total;
    xSemaphoreGive(s_lock);

    size_t image_size = total ? total : OTA_WITH_SEQUENTIAL_WRITES;
    err = esp_ota_begin(update, image_size, &handle);
    if (err != ESP_OK) goto failed;

    size_t written = 0;
    bool first_read_logged = false;
    int64_t started_us = esp_timer_get_time();
    uint32_t last_progress_ms = (uint32_t)(started_us / 1000);
    uint32_t eagain_count = 0;
    while (true) {
        if (s_cancel_requested) {
            err = ESP_ERR_INVALID_STATE;
            goto failed;
        }
        int64_t now_us = esp_timer_get_time();
        uint32_t now_ms = (uint32_t)(now_us / 1000);
        if (now_us - started_us >= OTA_TOTAL_TIMEOUT_US ||
            (uint32_t)(now_ms - last_progress_ms) >= OTA_NO_PROGRESS_MS) {
            err = ESP_ERR_TIMEOUT;
            goto failed;
        }
        int count = esp_http_client_read(client, buffer, OTA_BUFFER_SIZE);
        if (count == -ESP_ERR_HTTP_EAGAIN) {
            if (++eagain_count >= OTA_EAGAIN_LIMIT) {
                err = ESP_ERR_TIMEOUT;
                goto failed;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!first_read_logged) {
            ESP_LOGI(TAG, "first HTTP read returned %d", count);
            first_read_logged = true;
        }
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++eagain_count >= OTA_EAGAIN_LIMIT) {
                    err = ESP_ERR_TIMEOUT;
                    goto failed;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            err = ESP_ERR_HTTP_INCOMPLETE_DATA;
            goto failed;
        }
        if (count == 0) break;
        eagain_count = 0;
        last_progress_ms = now_ms;
        size_t chunk_size = (size_t)count;
        if (chunk_size > update->size - written ||
            (total && chunk_size > total - written)) {
            err = ESP_ERR_INVALID_SIZE;
            goto failed;
        }
        err = esp_ota_write(handle, buffer, chunk_size);
        if (err != ESP_OK) goto failed;
        written += chunk_size;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.bytes_written = written;
        xSemaphoreGive(s_lock);
        if (written % (256u * 1024u) == 0)
            ESP_LOGI(TAG, "downloaded %lu / %lu bytes", (unsigned long)written,
                     (unsigned long)(total ? total : update->size));
    }

    if (written == 0 || (total && written != total)) {
        err = ESP_ERR_INVALID_SIZE;
        goto failed;
    }

    esp_http_client_cleanup(client);
    client = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = PAD_OTA_VALIDATING;
    s_status.total_bytes = total ? total : written;
    xSemaphoreGive(s_lock);

    err = esp_ota_end(handle);
    handle = 0; /* esp_ota_end 后无论成功失败，handle 都已失效。 */
    if (err != ESP_OK) goto failed;
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) goto failed;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = PAD_OTA_READY;
    s_status.error = ESP_OK;
    s_status.bytes_written = written;
    s_status.total_bytes = total ? total : written;
    s_ready_at = xTaskGetTickCount() + pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS);
    s_reboot_now = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "image verified; reboot scheduled");
    free(buffer);
    return;

failed:
    if (client) esp_http_client_cleanup(client);
    free(buffer);
    set_failure(handle, err);
}

static void ota_task(void *arg)
{
    (void)arg;
    while (true) {
        xSemaphoreTake(s_work, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_cancel_requested) {
            s_cancel_requested = false;
            s_status.state = PAD_OTA_IDLE;
            s_status.error = ESP_OK;
            xSemaphoreGive(s_lock);
            continue;
        }
        char url[sizeof(s_url)];
        memcpy(url, s_url, sizeof(url));
        s_status.state = PAD_OTA_DOWNLOADING;
        s_status.bytes_written = 0;
        s_status.total_bytes = 0;
        s_status.error = ESP_OK;
        s_reboot_now = false;
        xSemaphoreGive(s_lock);
        download_and_apply(url);
        memset(url, 0, sizeof(url));
    }
}

esp_err_t pad_ota_confirm_current(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return ESP_ERR_NOT_FOUND;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) return err;
    if (state == ESP_OTA_IMG_PENDING_VERIFY)
        return esp_ota_mark_app_valid_cancel_rollback();
    return ESP_OK;
}

void pad_ota_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_work) s_work = xSemaphoreCreateBinary();
    if (!s_lock || !s_work) {
        ESP_LOGE(TAG, "failed to create OTA synchronization");
        return;
    }

    esp_err_t err = pad_ota_confirm_current();
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_ARG)
        ESP_LOGW(TAG, "could not confirm running image: %s", esp_err_to_name(err));

    const esp_partition_t *running = esp_ota_get_running_partition();
    bool rollback = running && esp_ota_get_next_update_partition(running) != NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == PAD_OTA_IDLE) s_status.rollback_supported = rollback;
    xSemaphoreGive(s_lock);

    if (!s_task_started) {
        if (xTaskCreate(ota_task, "pad_ota", 16384, NULL, 3, NULL)) s_task_started = true;
        else ESP_LOGE(TAG, "failed to create OTA task");
    }
}

esp_err_t pad_ota_start(const char *url)
{
    if (!url || url[0] == '\0' || !companion_config_url_valid(url)) return ESP_ERR_INVALID_ARG;
    if (strlen(url) > PAD_CONFIG_URL_MAX) return ESP_ERR_INVALID_ARG;
#if !CONFIG_COMPANION_OTA_ALLOW_HTTP
    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGW(TAG, "HTTP OTA disabled; use HTTPS");
        return ESP_ERR_INVALID_ARG;
    }
#endif
    if (!s_lock || !s_work) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state != PAD_OTA_IDLE && s_status.state != PAD_OTA_FAILED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_status.rollback_supported) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    strlcpy(s_url, url, sizeof(s_url));
    s_status.state = PAD_OTA_QUEUED;
    s_status.error = ESP_OK;
    s_status.bytes_written = 0;
    s_status.total_bytes = 0;
    s_reboot_now = false;
    s_cancel_requested = false;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_work);
    return ESP_OK;
}

esp_err_t pad_ota_cancel(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state != PAD_OTA_QUEUED && s_status.state != PAD_OTA_DOWNLOADING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_cancel_requested = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void pad_ota_get_status(pad_ota_status_t *out)
{
    if (!out) return;
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

bool pad_ota_is_busy(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_status.state >= PAD_OTA_QUEUED && s_status.state <= PAD_OTA_REBOOTING;
    xSemaphoreGive(s_lock);
    return busy;
}

void pad_ota_tick(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool reboot = s_status.state == PAD_OTA_READY &&
                  (s_reboot_now || (int32_t)(xTaskGetTickCount() - s_ready_at) >= 0);
    if (reboot) s_status.state = PAD_OTA_REBOOTING;
    xSemaphoreGive(s_lock);
    if (reboot) {
        ESP_LOGI(TAG, "rebooting into updated image");
        esp_restart();
    }
}

esp_err_t pad_ota_reboot_now(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state != PAD_OTA_READY) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_reboot_now = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
