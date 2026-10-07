set(COMPONENT_SRCS app_main.cpp)

set(COMPONENT_ADD_INCLUDEDIRS .)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS .
    # Component names come from their directory names: apps/muse -> muse,
    # apps/hermes -> hermes (not aiwatchos_muse / aiwatchos_hermes).
    REQUIRES aiwatchos_os muse hermes
)
