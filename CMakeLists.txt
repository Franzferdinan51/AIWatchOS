# AIWatchOS — an AI-first watch OS for the Waveshare ESP32-S3-Touch-AMOLED-2.06
# (ESP32-S3R8, 410x502 CO5300 AMOLED, FT3168 touch).
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common components/aiwatchos_os components/noise_core components/minimp3 apps/muse apps/hermes)

# Pull in the ESP-IDF build system. Without this include, `idf.py build` only
# runs a bare CMake configure and never compiles any firmware (no IDF targets,
# no components, no project_description.json).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aiwatchos)
