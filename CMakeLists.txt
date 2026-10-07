# AIWatchOS — an AI-first watch OS for the Waveshare ESP32-S3-Touch-AMOLED-2.06
# (ESP32-S3R8, 410x502 CO5300 AMOLED, FT3168 touch).
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common components/aiwatchos_os apps/muse apps/hermes)

# The IDF build system requires the project() macro in the top-level CMakeLists.txt.
project(aiwatchos)

# Target chip: ESP32-S3 (dual-core 240 MHz, 8 MB octal PSRAM, 32 MB flash).
set(EXTRA_TOOLCHAIN_TARGETS "esp32s3")
