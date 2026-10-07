set(COMPONENT_SRCS app_main.cpp)

set(COMPONENT_ADD_INCLUDEDIRS .)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS .
    REQUIRES aiwatchos_os aiwatchos_muse aiwatchos_hermes
)
