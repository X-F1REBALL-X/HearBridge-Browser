#!/usr/bin/env bash
# HearBridge Browser — build.
#   ./build.sh          -> dist/hearbridge-browser-<version>.elf   (PS5 payload, port 8096)
#   ./build.sh host     -> build/hearbridge-browser-host           (local test server, no console needed;
#                          HB_TEST=program ./build/hearbridge-browser-host for game-like test material)
# The PS5 build needs PS5_PAYLOAD_SDK pointing at your ps5-payload-sdk install (and its LLVM on PATH).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
ver="$(sed -n 's/^#define PSS_VERSION "\(.*\)"/\1/p' "$here/src/version.h")"
gen="$here/build/gen"
mkdir -p "$gen" "$here/dist"
python3 "$here/tools/embed.py" "$here/web/index.html" "$gen/page_html.h" PAGE_HTML PSS_PAGE_HTML_H
python3 "$here/tools/embed.py" "$here/vendor/lamejs/lame.min.js" "$gen/lame_js.h" LAME_JS HB_LAME_JS_H
inc=(-I"$gen" -I"$here/src")
common=(main.c server.c sha1.c diag.c log.c util.c notify.c)

if [[ "${1:-ps5}" == "host" ]]; then
  out="$here/build/hearbridge-browser-host"
  cc -std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE -DPSS_HOST -DPSS_STATE_DIR=\"/tmp/hearbridge-browser\" \
     "${inc[@]}" -o "$out" "${common[@]/#/$here/src/}" "$here/src/capture_host.c" -lpthread -lm
  echo "ok: $out"; exit 0
fi

if [[ -z "${PS5_PAYLOAD_SDK:-}" ]]; then
  echo "error: PS5_PAYLOAD_SDK is not set." >&2
  echo "       export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk and run again (or use ./build.sh host)." >&2
  exit 1
fi
if [[ ! -x "$PS5_PAYLOAD_SDK/bin/prospero-clang" ]]; then
  echo "error: $PS5_PAYLOAD_SDK/bin/prospero-clang not found. Is PS5_PAYLOAD_SDK set to the sdk folder?" >&2
  exit 1
fi
out="$here/dist/hearbridge-browser-$ver.elf"
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -std=c11 -Wall -Wextra -O2 "${inc[@]}" -o "$out" \
  "${common[@]/#/$here/src/}" "$here/src/creds.c" "$here/src/avcap2.c" -lpthread
echo "ok: $out"; ls -la "$out"; sha256sum "$out"
