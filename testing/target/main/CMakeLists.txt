idf_component_register(SRCS "target_tests.cpp"
                       REQUIRES unity driver nvs_flash esp_system esp_timer
                                esp_hal_gpio soc hal)
