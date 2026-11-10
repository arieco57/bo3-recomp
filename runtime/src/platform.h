#pragma once
// Platform choices of the build (Android: on-screen options button, physical gamepad only).
// BO3_TOUCH_UI=1 / BO3_GAMEPAD_ONLY=1 enable them on the desktop for testing.
#include <cstdlib>

inline bool PlatformTouchUi()
{
#ifdef __ANDROID__
    return true;
#else
    static const bool on = getenv("BO3_TOUCH_UI") != nullptr;
    return on;
#endif
}

inline bool PlatformGamepadOnly()
{
#ifdef __ANDROID__
    return true;
#else
    static const bool on = getenv("BO3_GAMEPAD_ONLY") != nullptr;
    return on;
#endif
}
