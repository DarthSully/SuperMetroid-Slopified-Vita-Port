#!/usr/bin/env bash
# Generate the release profile locally; players never run a compiler.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-$(command -v python3 || command -v python)}"
exec "$PYTHON" "$ROOT/tools/regen.py" "$@"
