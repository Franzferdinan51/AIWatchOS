# AIWatchOS — an AI-first watch OS for the Waveshare ESP32-S3-Touch-AMOLED-2.06
# (ESP32-S3R8, 410x502 CO5300 AMOLED, FT3168 touch).
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common components/aiwatchos_os components/aiwatchos_net components/aiwatchos_weather components/noise_core components/minimp3 apps/muse apps/hermes)

# Pull in the ESP-IDF build system. Without this include, `idf.py build` only
# runs a bare CMake configure and never compiles any firmware (no IDF targets,
# no components, no project_description.json).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aiwatchos)

# Bake the UI assets (baked Inter fonts + shadow sprites from
# tools/gen_font.py, committed under assets/) into the aiwatchos_assets
# FAT partition at build time. FLASH_IN_PROJECT makes the partition
# image part of `idf.py flash`, so the fonts land with the firmware
# while the app slots stay untouched. The OS mounts the partition
# read-only at boot (font.cpp) and loads the assets into PSRAM.
fatfs_create_spiflash_image(aiwatchos_assets assets FLASH_IN_PROJECT)
