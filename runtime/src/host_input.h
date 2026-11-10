#pragma once
#include <cstdint>
union SDL_Event;
struct SDL_Window;
void HostInputInit();
void HostInputEvent(const SDL_Event& e);
void HostInputUpdate();   // call regularly on the main thread
void HostInputSetWindow(SDL_Window* w);   // for relative mouse mode
void HostInputReleaseMouse();   // leave relative mouse mode (dialogs)
void HostInputNeutral();        // pad 0 back to rest (nothing pressed)
// Mouse look as view-angle deltas (degrees), applied by the game hook that computes the view
// angles each frame (mp/zm hooks.cpp). Returns false when there is nothing to apply.
bool HostInputTakeMouseAngles(float& dyaw, float& dpitch);
// Adds the mouse deltas to the user command angles (`cmd`) of the client state `clientState`.
void HostInputApplyMouseLook(uint32_t clientState, uint32_t cmd);
void HostInputSetMouseSens(float sens);
void HostInputMouseLookTrace(uint32_t clientState, uint32_t cmd);   // diagnostics
void HostInputApplyLookRates(uint32_t client);   // mouse look through the client look rates
void HostInputSetMouseRatio(float horizontalOverVertical);
