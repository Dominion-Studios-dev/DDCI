#!/bin/sh
# ============================================================================
# install.sh — DDCI single-command installer (Linux / macOS)
#
# Builds DDCI from source (Release, -O3 -march=native -flto, stripped) and
# installs the `ddci` binary. C++20 compiler, CMake 3.20+, SQLite3 and
# libcurl development headers are required (cpr is fetched and built
# automatically). On Linux the binary is fully static; on macOS it links
# the system libcurl.
#
# Usage:
#   ./install.sh                build + install to /usr/local/bin
#   ./install.sh --prefix ~/bin install to a custom directory
#   ./install.sh --help         show this usage
#
# Copyright (c) 2026 Dominion Studios. All Rights Reserved.
# ============================================================================

set -e

PREFIX="/usr/local/bin"

usage() {
    echo "Usage: ./install.sh [--prefix DIR]"
    echo
    echo "Builds DDCI (Release) and installs the 'ddci' binary."
    echo
    echo "Options:"
    echo "  --prefix DIR   Install to DIR (default: /usr/local/bin)"
    echo "  --help, -h     Show this help"
}

for arg in "$@"; do
    case "$arg" in
        --prefix)
            shift
            PREFIX="$1"
            ;;
        --prefix=*)
            PREFIX="${arg#--prefix=}"
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown argument: $arg"
            usage
            exit 2
            ;;
    esac
done

# --- Preflight: required tools -------------------------------------------
for tool in cmake c++ pkg-config sqlite3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERROR] Required tool not found: $tool"
        case "$(uname -s)" in
            Darwin)
                echo "  Install it with: brew install cmake sqlite3 pkg-config"
                ;;
            Linux)
                echo "  Debian/Ubuntu:   sudo apt install build-essential cmake pkg-config libsqlite3-dev"
                echo "  Fedora:          sudo dnf install gcc-c++ cmake pkg-config sqlite-devel"
                echo "  Arch:            sudo pacman -S base-devel cmake pkgconf sqlite"
                ;;
        esac
        exit 1
    fi
done

case "$(uname -s)" in
    Darwin)
        if ! command -v brew >/dev/null 2>&1; then
            echo "[WARNING] Homebrew not found; libcurl may be missing."
        fi
        ;;
    *)
        if ! pkg-config --exists libcurl; then
            echo "[ERROR] libcurl development headers are required."
            echo "  Debian/Ubuntu:   sudo apt install libcurl4-openssl-dev"
            echo "  Fedora:          sudo dnf install libcurl-devel"
            echo "  Arch:            sudo pacman -S curl"
            exit 1
        fi
        ;;
esac

# --- Locate the repo root (this script, or a subdirectory of it) ---------
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

echo "=== DDCI install ==="
echo "  Source:  $ROOT"
echo "  Prefix:  $PREFIX"

BUILD_DIR="$ROOT/build"
mkdir -p "$BUILD_DIR"

echo "=== Configuring (Release) ==="
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "=== Building ==="
cmake --build "$BUILD_DIR" --parallel

BINARY="$BUILD_DIR/ddci"
if [ ! -x "$BINARY" ]; then
    echo "[ERROR] Build succeeded but no binary was produced at $BINARY"
    exit 1
fi

echo "=== Installing ==="
install -m 0755 -d "$PREFIX"
install -m 0755 "$BINARY" "$PREFIX/ddci"

echo
echo "Installed: $PREFIX/ddci ($(du -h "$PREFIX/ddci" | cut -f1))"
echo
echo "Next steps:"
echo "  1. Make sure $PREFIX is on your PATH."
echo "  2. Run 'ddci' once — it will walk you through creating an API key."
echo "  3. Optional: export GROQ_API_KEY=sk-... or add it to a .env file."