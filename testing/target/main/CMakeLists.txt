# Component names, not the pre-6.0 umbrella `driver`. ESP-IDF 6 split it up, and
# the UART driver now lives in esp_driver_uart — asking for `driver` builds
# cleanly right up to `fatal error: driver/uart.h: No such file or directory`.
idf_component_register(SRCS "target_tests.cpp"
                       REQUIRES unity
                                esp_driver_uart
                                esp_driver_gpio
                                nvs_flash
                                esp_system
                                esp_timer
                                soc
                                hal)
