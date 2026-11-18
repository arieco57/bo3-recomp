// First-run installation from the player's copy of the game (disc image, Title Update, DLC): see
// setup.cpp. Returns false when the game cannot start (the player was told what is missing).
#pragma once
bool SetupGameData();

#include <string>
#include <utility>
#include <vector>
// status text + percent (0-100, or -1 for a message) for the Android installer screen
using SetupReport = void (*)(const char* text, int percent);
// everything installed (game, Title Update, patched executables, Compatibility Packs 1 and 2)?
bool SetupInstalled();
// installs from explicitly chosen files ({ path, display name }); current directory = data folder
bool SetupGameDataFrom(const std::vector<std::pair<std::string, std::string>>& files, SetupReport report);

// what a chosen file or folder holds (for the installer screens' checks)
struct SetupProbe { int discs = 0, updates = 0, dlcs = 0; bool cp1 = false, cp2 = false; };
SetupProbe SetupProbePath(const std::string& path);
// game, Title Update, Compatibility Pack 1, Compatibility Pack 2 already in place
void SetupInstalledParts(bool out[4]);
// everything still missing is in the folders the installer looks in ("instalar/"...): no screen needed
bool SetupLocalComplete();

// Linux: the installer window (installer_ui.cpp). 1 = installed, 0 = closed by the player,
// -1 = no window possible (no font / renderer): the dialog wizard instead.
int InstallerUiRun(bool zombies);
