#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
echo "Building frontend..."
cd web && pnpm build && cd ..
echo "Building backend..."
cmake --build build
echo "Done."
