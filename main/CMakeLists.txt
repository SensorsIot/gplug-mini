# PRIV_REQUIRES, not REQUIRES: `main` exposes no headers of its own, so nothing
# downstream inherits these. esp_driver_gpio carries driver/gpio.h and
# esp_app_format carries esp_app_desc.h — ESP-IDF 6 bundles neither into a
# catch-all component, so each one has to be named.
idf_component_register(SRCS "app_main.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_app_format)
