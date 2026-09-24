idf_component_register(SRC_DIRS "." INCLUDE_DIRS "." PRIV_REQUIRES nvs_flash esp_timer esp_driver_gpio example_common)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ezb_plat_radio_receive_done" "-Wl,--wrap=ezb_plat_radio_transmit_done")
