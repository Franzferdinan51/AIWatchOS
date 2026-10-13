# aiwatchos_weather — real current conditions for the OS weather app.
#
# weather_parse.cpp is pure apart from cJSON so the host test in test/ can
# build it with plain g++ (against the ESP-IDF tree's own cJSON, the same
# source the firmware links). weather_service.cpp is firmware-only: the
# HTTPS fetch task (esp_http_client + TLS bundle), WiFi state from
# aiwatchos_net, snapshots published into aiwatchos_os's weather app.
idf_component_register(
    SRCS
        "src/weather_parse.cpp"
        "src/weather_service.cpp"
    INCLUDE_DIRS "include"
    # aiwatchos_os: provider seam + WeatherSnapshot + Board clock.
    # aiwatchos_net: wifi_connected() / net_config().
    # json: cJSON (same component the account API uses).
    REQUIRES aiwatchos_os aiwatchos_net esp_http_client esp-tls json
             esp_timer freertos esp_system
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    "-Wall" "-Wextra"
)
