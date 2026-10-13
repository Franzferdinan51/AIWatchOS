# aiwatchos_net — shared network + Muse transport for AIWatchOS.
#
# Layout mirrors the repo's other components. net_proto.cpp is pure C++17
# (no ESP-IDF includes) so the host test in test/ can build it with plain
# g++; everything else is firmware-only.
idf_component_register(
    SRCS
        "src/net_proto.cpp"
        "src/net_config.cpp"
        "src/net_manager.cpp"
        "src/muse_account.cpp"
        "src/muse_link.cpp"
    INCLUDE_DIRS "include"
    # aiwatchos_os: Board time seam (board_set_epoch_ms) for SNTP.
    # noise_core: ClientSession / PsaCryptoBackend (Noise XX transport).
    # json: cJSON for the account API and chat ack/event parsing.
    REQUIRES aiwatchos_os noise_core nvs_flash esp_wifi esp_netif esp_event
             esp-tls esp_http_client json esp_timer lwip freertos
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    "-Wall" "-Wextra"
)
