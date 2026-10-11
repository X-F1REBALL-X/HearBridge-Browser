#!/usr/bin/env bash
# Cross-compile the HearBridge Browser Windows app from Linux (or build on Windows with Git Bash).
set -euo pipefail
cd "$(dirname "$0")"
VER=1.0.0
GOWINRES=${GOWINRES:-$(go env GOPATH)/bin/go-winres}
[ -x "$GOWINRES" ] || go install github.com/tc-hib/go-winres@latest
# icon, version info and manifest -> rsrc_windows_amd64.syso (not committed)
(cd winres && "$GOWINRES" make --in winres.json --arch amd64 --out ../rsrc)
mkdir -p dist
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -trimpath \
  -ldflags "-s -w -H=windowsgui" -o "dist/HearBridge-Browser-$VER-windows-x64.exe" .
ls -la dist
