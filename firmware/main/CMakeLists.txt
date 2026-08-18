idf_component_register(
    SRCS
        "main.c"
        "output_hal_gpio.c"
        "input_hal_gpio.c"
        "nvs_config_hal.c"
        "nvs_keystore_hal.c"
        "nvs_lock_hal.c"
        "nvs_calib_hal.c"
        "nvs_event_log_hal.c"
        "diag_hal.c"
        "ota_hal.c"
        "factory_reset.c"
        "power_hal.c"
        "watchdog.c"
        "ble/ble_app.c"
        "ble/gatt_svr.c"
    INCLUDE_DIRS "." "ble"
    REQUIRES board_config core
    PRIV_REQUIRES esp_driver_gpio esp_driver_ledc nvs_flash esp_timer esp_system esp_adc bt app_update esp_pm
)
