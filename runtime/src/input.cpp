// XInput: one gamepad on port 0. Host code (window/SDL layer, later) updates g_pad; the guest
// reads it through XamInputGetState.
//
// X_INPUT_GAMEPAD (12 bytes): u16 buttons, u8 left_trigger, u8 right_trigger, s16 lx, ly, rx, ry
// X_INPUT_STATE: u32 packet_number + gamepad
// X_INPUT_CAPABILITIES: u8 type, u8 sub_type, u16 flags, gamepad (max values), vibration (u16 x2)
#include "guest.h"
#include "input.h"
#include "stubs.h"

#include <mutex>

HostPadState g_pad;
static std::mutex s_padLock;
static uint32_t s_packet = 0;

void InputSetPad(const HostPadState& s)
{
    std::lock_guard lock(s_padLock);
    if (memcmp(&s, &g_pad, sizeof(s)) != 0) s_packet++;
    g_pad = s;
}

static bool PadConnected(uint32_t user) { return user == 0 || user == 0xFF; }

static void WriteGamepad(uint32_t p, const HostPadState& s)
{
    GuestWrite16(p + 0, s.buttons);
    GuestWrite8(p + 2, s.leftTrigger);
    GuestWrite8(p + 3, s.rightTrigger);
    GuestWrite16(p + 4, uint16_t(s.lx));
    GuestWrite16(p + 6, uint16_t(s.ly));
    GuestWrite16(p + 8, uint16_t(s.rx));
    GuestWrite16(p + 10, uint16_t(s.ry));
}

// XamInputGetCapabilities(user, flags, caps*)
GUEST_IMPORT(XamInputGetCapabilities)
{
    if (!PadConnected(ARG(3))) RETURN(ERROR_DEVICE_NOT_CONNECTED);
    uint32_t c = ARG(5);
    memset(g_base + c, 0, 0x14);
    GuestWrite8(c + 0, 1);   // XINPUT_DEVTYPE_GAMEPAD
    GuestWrite8(c + 1, 1);   // XINPUT_DEVSUBTYPE_GAMEPAD
    HostPadState full{ 0xF3FF, 0xFF, 0xFF, -1, -1, -1, -1 };
    WriteGamepad(c + 4, full);
    GuestWrite16(c + 0x10, 0xFFFF);
    GuestWrite16(c + 0x12, 0xFFFF);
    RETURN(ERROR_SUCCESS);
}

// XamInputGetState(user, flags, state*)
GUEST_IMPORT(XamInputGetState)
{
    if (!PadConnected(ARG(3))) RETURN(ERROR_DEVICE_NOT_CONNECTED);
    std::lock_guard lock(s_padLock);
    GuestWrite32(ARG(5), s_packet);
    WriteGamepad(ARG(5) + 4, g_pad);
    RETURN(ERROR_SUCCESS);
}

// XamInputSetState(user, flags, vibration*)
GUEST_IMPORT(XamInputSetState)
{
    RETURN(PadConnected(ARG(3)) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED);
}
