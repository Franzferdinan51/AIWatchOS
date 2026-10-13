set(COMPONENT_SRCS src/muse_adapter.cpp src/muse_adpcm.c)

set(COMPONENT_ADD_INCLUDEDIRS include)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS include
    REQUIRES aiwatchos_os
    # aiwatchos_net: the transport behind muse_outbox_drain's send seam
    # (device builds; the adapter registers its pump under __ESPRESSIF_IDF__).
    PRIV_REQUIRES minimp3 aiwatchos_net
)

# The muse-gadget-206 project is integrated as a submodule. Its components
# (muse/, noise_core/, minimp3/) are registered via EXTRA_COMPONENT_DIRS in the
# top-level CMakeLists.txt. This adapter component wraps muse_app_run() and
# muse_voice_start()/muse_voice_stop() from the original firmware.
