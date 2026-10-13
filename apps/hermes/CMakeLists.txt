# Hermes Gadget Protocol v1 client app.
#
# Sources:
#   hermes_adapter.cpp — OS app surface (render/touch/health) + client owner
#   hermes_proto.cpp   — portable protocol state machine (host-testable)
#   hermes_crypto.cpp  — portable SHA-256/HMAC/base64 (published vectors)
#   hermes_link.cpp    — ESP-IDF transport: NVS identity, WebSocket,
#                        OTA flash backend, pump task (host: stubs).
#                        WiFi is observed from aiwatchos_net, never owned.
#   cJSON.c            — vendored JSON parser (see PROVENANCE.txt)
idf_component_register(
    SRCS src/hermes_adapter.cpp src/hermes_proto.cpp src/hermes_crypto.cpp
         src/hermes_link.cpp src/cJSON.c
    INCLUDE_DIRS include
    REQUIRES aiwatchos_os aiwatchos_net esp_event nvs_flash app_update
             esp_websocket_client
)
