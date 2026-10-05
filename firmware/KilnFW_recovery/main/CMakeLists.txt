# Stage header / semver / image-CRC code is linked from the application tree so
# the stage header format has exactly one implementation (the application's
# stager writes it, recovery_apply.c reads it).
set(APP_DRIVERS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../KilnFW/App/drivers")

idf_component_register(
    SRCS
        "recovery_main.c"
        "recovery_apply.c"
        "recovery_apply_esp.c"
        "${APP_DRIVERS_DIR}/update/stage_header.c"
        "${APP_DRIVERS_DIR}/update/update_semver.c"
        "${APP_DRIVERS_DIR}/http/ota_image_crc.c"
        "recovery_wifi.c"
        "recovery_http.c"
        "recovery_boot_verify.c"
        "recovery_image_check.c"
        "recovery_upload.c"
        "recovery_pico_proto.c"
        "recovery_pico.c"
        "recovery_lcd.c"
        "recovery_io.c"
        "recovery_text.c"
        "recovery_passphrase.c"
        "recovery_hold.c"
        "recovery_health.c"
    INCLUDE_DIRS "." "${APP_DRIVERS_DIR}/update" "${APP_DRIVERS_DIR}/http"
    EMBED_FILES "recovery_page.html"
    REQUIRES
        esp_wifi
        esp_netif
        lwip
        esp_event
        nvs_flash
        bootloader_support
        esp_http_server
        app_update
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_driver_uart
        esp_rom
        kilnlink
        esp_timer
        esp_partition
        esp_system
        esp_hw_support
        mbedtls
)
