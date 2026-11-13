set(bridge_requires
    bt
    driver
    display
    esp_event
    esp_netif
    esp_timer
    esp_wifi
    espressif__cjson
    espressif__network_provisioning
    fatfs
    lwip
    mbedtls
    nvs_flash
    sdmmc)

# IDF >= 5.2/6.x split the legacy `driver` umbrella into per-peripheral
# components. Add them only when present so 5.0/5.1 keeps working.
if(EXISTS "${IDF_PATH}/components/esp_driver_uart")
    list(APPEND bridge_requires esp_driver_uart esp_driver_gpio)
endif()
if(EXISTS "${IDF_PATH}/components/esp_driver_sdspi")
    list(APPEND bridge_requires esp_driver_sdspi)
endif()

idf_component_register(
    SRCS
        "bridge_ring.c"
        "bridge_read.c"
        "bridge_port.c"
        "bridge_net.c"
        "bridge_prov.c"
        "bridge_button.c"
        "bridge_sd.c"
        "bridge_event.c"
        "bridge_time.c"
        "bridge_auth.c"
        "bridge_v1.c"
    INCLUDE_DIRS "include"
    REQUIRES ${bridge_requires})
