set(COMPONENT_SRCS src/hermes_adapter.cpp)

set(COMPONENT_ADD_INCLUDEDIRS include)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS include
    REQUIRES aiwatchos_os
)

# The HermesGadget project is integrated as a submodule. Its core (hg::App,
# hg::Hal interfaces) lives in firmware/core/. This adapter wraps the App lifecycle:
#   - begin(): generates device key, loads settings, starts connection phase
#   - tick(): processes WebSocket I/O, audio streaming, display updates
# The HAL bridge is implemented above via delegation to aiwatchos::hal().
