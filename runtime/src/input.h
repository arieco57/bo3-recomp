#pragma once
#include <cstdint>

// XINPUT_GAMEPAD button bits
enum : uint16_t
{
    PAD_DPAD_UP = 0x0001, PAD_DPAD_DOWN = 0x0002, PAD_DPAD_LEFT = 0x0004, PAD_DPAD_RIGHT = 0x0008,
    PAD_START = 0x0010, PAD_BACK = 0x0020, PAD_LTHUMB = 0x0040, PAD_RTHUMB = 0x0080,
    PAD_LB = 0x0100, PAD_RB = 0x0200, PAD_GUIDE = 0x0400,
    PAD_A = 0x1000, PAD_B = 0x2000, PAD_X = 0x4000, PAD_Y = 0x8000,
};

struct HostPadState
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// Called by the host input layer.
void InputSetPad(const HostPadState& s);
