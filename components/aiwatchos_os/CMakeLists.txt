set(COMPONENT_SRCS
    src/app.cpp
    src/hal.cpp
    src/display_driver.cpp
    src/touch_driver.cpp
    src/audio_driver.cpp
    src/power_manager.cpp
    src/app_manager.cpp
    src/launcher_ui.cpp
    src/clock_face.cpp
    src/battery_display.cpp
    src/settings_page.cpp
    src/notifications.cpp
    src/weather_app.cpp
)

set(COMPONENT_ADD_INCLUDEDIRS include)

# NOTE: test/app_registry_test.cpp is a host-side unit test built with plain
# g++ (see its header comment), not an IDF unit-test-app case, so it must not
# be passed to idf_component_register (unknown args break component setup).
idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS include
    # IDF 5.x drivers live in esp_driver_* components (legacy `driver` shim is
    # gone); these make driver/gpio.h, driver/spi_master.h, driver/i2c_master.h
    # and the esp_lcd panel headers visible to the HAL drivers. esp_driver_i2c
    # is public because hal.hpp exposes the shared-bus accessor to dependents.
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_lcd esp_driver_i2s esp_timer
    REQUIRES esp_driver_i2c
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wall" "-Wextra")
