#!/bin/sh
# Linux runner: build/desktop/mw_desktop. Needs scripts/translate.sh first.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
CC=clang CXX=clang++ cmake -S "$R" -B "$R/build/desktop" -G Ninja
ninja -C "$R/build/desktop" mw_desktop
