#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

void companion_network_init(void);
void companion_network_tick(void);
bool companion_network_is_connected(void);
bool companion_network_time_valid(void);
esp_err_t companion_network_apply_config(const companion_config_t *config);
esp_err_t companion_network_set_brightness(uint8_t brightness);
void companion_network_request_restart(void);

#ifdef __cplusplus
}
#endif
