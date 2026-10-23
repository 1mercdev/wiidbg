#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build and push the stub (or target) to a Wii waiting in the Homebrew Channel.
#
# Usage:  WIILOAD=tcp:192.168.1.50 tools/deploy.sh [stub|target]
#
# How it works: the Homebrew Channel listens on TCP port 4299; `wiiload`
# streams a .dol to it, so you never have to move the SD card between builds.
set -eu
what="${1:-stub}"
case "$what" in stub|target) ;; *) echo "usage: $0 [stub|target]" >&2; exit 2 ;; esac
: "${WIILOAD:?set WIILOAD, e.g. WIILOAD=tcp:192.168.1.50}"
make -C "$(dirname "$0")/../$what" deploy
