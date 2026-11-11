#!/bin/sh
# Runs the game from the folder that holds the AppImage: "instalar/", the installed game data and
# bo3recomp.ini live there (the first run installs the game from the copy in instalar/).
HERE="$(dirname "$(readlink -f "$0")")"
cd "$(dirname "${APPIMAGE:-$0}")" || exit 1
exec "$HERE/usr/bin/BIN" "$@"
