#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
${CC:-cc} -std=c11 -g -O1 ${SANITIZE:+-fsanitize=address,undefined -fno-omit-frame-pointer} \
  -Itests/host/stubs -Icomponents/seeed_epaper \
  tests/host/test_epaper.c components/seeed_epaper/epaper_panel.c \
  components/seeed_epaper/driver/{ssd1677,ssd2677,uc8179}.c -o "$build/test_epaper"
"$build/test_epaper"
