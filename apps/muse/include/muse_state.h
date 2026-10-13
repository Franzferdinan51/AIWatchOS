/*
 * Trimmed stand-in for muse-gadget-206's muse_state.h: muse_pixel.h
 * only needs the mode enum. The gadget's full state machine is not
 * ported; AIWatchOS drives the pose from its own link/agent state.
 */
#pragma once

typedef enum {
    MUSE_MODE_BOOT = 0,
    MUSE_MODE_IDLE,
    MUSE_MODE_LISTENING,
    MUSE_MODE_THINKING,
    MUSE_MODE_SPEAKING,
    MUSE_MODE_ERROR,
    MUSE_MODE_OFF,       /* powering down */
    MUSE_MODE_COUNT,
} muse_mode_t;
