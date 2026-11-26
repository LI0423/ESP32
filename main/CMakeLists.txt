# COMPONENT_DIR, not CMAKE_CURRENT_SOURCE_DIR: IDF include()s this file while
# collecting requirements, and the latter is not the component directory then.
if(NOT EXISTS "${COMPONENT_DIR}/admin_cert.h")
    message(FATAL_ERROR
        "main/admin_cert.h is missing. It is gitignored because it embeds the "
        "admin private key. From the project root run: bash tools/gen_admin_cert.sh")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "companion_model.c"
        "companion_progress.c"
        "companion_schedule.c"
        "companion_timer.c"
        "companion_ui.c"
        "config_store.c"
        "lcd.c"
        "network.c"
        "ota.c"
        "provisioning.c"
        "../assets/deskpal/deskpal_sprites.c"
    INCLUDE_DIRS
        "."
        "../assets/deskpal"
    REQUIRES
        app_update
        driver
        esp_event
        esp_http_client
        esp_http_server
        esp_https_server
        esp_lcd
        esp_netif
        esp_partition
        esp_timer
        esp_wifi
        json
        mbedtls
        mdns
        nvs_flash
)
