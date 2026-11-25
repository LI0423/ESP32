#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAD_OTA_IDLE = 0,
    PAD_OTA_QUEUED,
    PAD_OTA_DOWNLOADING,
    PAD_OTA_VALIDATING,
    PAD_OTA_READY,
    PAD_OTA_REBOOTING,
    PAD_OTA_FAILED,
} pad_ota_state_t;

typedef struct {
    pad_ota_state_t state;
    size_t bytes_written;
    size_t total_bytes;       /* Content-Length，未知时为 0 */
    esp_err_t error;
    bool rollback_supported;
} pad_ota_status_t;

void pad_ota_init(void);
void pad_ota_tick(void);

/* URL 必须是 http:// 或 https://，最长 255 字节。 */
esp_err_t pad_ota_start(const char *url);
esp_err_t pad_ota_cancel(void);
void pad_ota_get_status(pad_ota_status_t *out);
bool pad_ota_is_busy(void);

/* 确认当前运行固件有效，取消首次启动后的待验证/回滚状态。 */
esp_err_t pad_ota_confirm_current(void);

/* 仅在 PAD_OTA_READY 时立即重启到新镜像。 */
esp_err_t pad_ota_reboot_now(void);

#ifdef __cplusplus
}
#endif
