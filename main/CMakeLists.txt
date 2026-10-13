set(COMPONENT_SRCS app_main.cpp mdb_console.cpp)

set(COMPONENT_ADD_INCLUDEDIRS .)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS .
    # Component names come from their directory names: apps/muse -> muse,
    # apps/hermes -> hermes (not aiwatchos_muse / aiwatchos_hermes).
    REQUIRES aiwatchos_os muse hermes aiwatchos_net aiwatchos_weather esp_wifi esp_netif
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_lcd
)
