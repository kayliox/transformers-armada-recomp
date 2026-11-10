#!/usr/bin/env bash
# Plays the game (Linux: Play.bat's job). In the game: F1 = the menu (settings, saves), F11 = full screen.
# First start: settings.ini is made from settings.example.ini (overlay/settings.example.ini when there is one).
# For Steam: add this script as a non-Steam game.
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" || exit 1
if [ ! -f settings.ini ]; then
    for example in overlay/settings.example.ini settings.example.ini; do
        [ -f "$example" ] && cp "$example" settings.ini && break
    done
fi
exec ./out/build/transformers/TransformersRecomp "$@"
