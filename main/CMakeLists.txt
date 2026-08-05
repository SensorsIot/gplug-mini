# The capture is embedded only in the simulated build. FR-BLD-04 makes the
# production artefact incapable of fabricating readings, and CI checks the
# artefact rather than trusting this condition.
set(EMBEDS "")
if(CONFIG_GPLUG_SIM_METER)
    set(EMBEDS "../testing/host/fixtures/e450_serial8.hex")
endif()

# PRIV_REQUIRES, not REQUIRES: main exposes no headers, so nothing downstream
# inherits these. ESP-IDF 6 bundles none of them into a catch-all component.
idf_component_register(SRCS "app_main.cpp"
                            "obis_map.cpp"
                            "ha_discovery.cpp"
                            "net.cpp"
                            "sim_source.cpp"
                            "meter_uart.cpp"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES ${EMBEDS}
                       PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_app_format esp_timer
                                     esp_wifi esp_netif esp_event nvs_flash)
