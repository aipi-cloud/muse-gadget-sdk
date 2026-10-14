// Copyright (c) Meta Platforms, Inc. and affiliates.

// AIPI's own two-frame face (blink): original geometric art, not the
// jollybot.gif data this file used to hold (FIRMWARE_CONTRACT.md section 5).
#pragma once

#include <stdint.h>
#include "sdkconfig.h"

#define HAPPY_ANIM_WIDTH     56
#define HAPPY_ANIM_HEIGHT    59
#define HAPPY_ANIM_FRAMES    2
#define HAPPY_ANIM_FRAME_MS  600

#if CONFIG_HOMEHUB_DISPLAY
extern const uint16_t happy_anim_palette[6];
extern const uint8_t happy_anim_frames[HAPPY_ANIM_FRAMES][HAPPY_ANIM_HEIGHT * HAPPY_ANIM_WIDTH];
#endif
