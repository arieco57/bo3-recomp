#pragma once
struct SDL_Window;

// Loads bo3recomp.ini and applies the saved resolution / fullscreen setting.
void VideoOptionsInit(SDL_Window* w);
// F1: options dialog (resolution, fullscreen on/off). Main thread.
void VideoOptionsDialog(SDL_Window* w);
bool VideoOptionsFullscreen();
void VideoOptionsSetFullscreen(SDL_Window* w, bool on);
void VideoOptionsCycleOverlay();   // F2: performance overlay off / FPS / detailed
// Field of view override (0 = the game's own); read by the Dvar_GetInt hooks (mp/zm hooks.cpp).
int VideoOptionsFov();
