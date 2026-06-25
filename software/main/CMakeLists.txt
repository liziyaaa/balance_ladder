idf_component_register(
    SRCS
        "app_main.cpp"
        "app_state.cpp"
        "balance_controller.cpp"
        "ble_debug.cpp"
        "imu_mpu6050.cpp"
        "motor_drv8837.cpp"
        "status_led.cpp"
        "ui_oled.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer nvs_flash bt
)
