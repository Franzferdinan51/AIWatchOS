set(COMPONENT_SRCS
    src/hal.cpp
    src/display_driver.cpp
    src/touch_driver.cpp
    src/audio_driver.cpp
    src/power_manager.cpp
    src/app_manager.cpp
    src/launcher_ui.cpp
    src/clock_face.cpp
    src/battery_display.cpp
)

set(COMPONENT_ADD_INCLUDEDIRS include)

# Tests are compiled separately via the test runner.
set(TEST_SRCS
    test/app_registry_test.cpp
)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS include
    TESTS ${TEST_SRCS}
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wall" "-Wextra")
