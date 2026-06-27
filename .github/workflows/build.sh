#!/usr/bin/env bash
# ==========================================================================
#  build.sh — one-command KEXT build with an existing osxcross install
#
#  Usage:
#    ./build.sh                          # auto-detect osxcross in PATH
#    ./build.sh /path/to/osxcross        # specify osxcross root
#    ./build.sh --native                 # build with Xcode clang (macOS)
#
#  Produces:  ./build/IntelXeDriver.kext
# ==========================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ---- Defaults ----
OSXCROSS_ROOT=""
TARGET_TRIPLE="x86_64-apple-darwin22"
NATIVE=0
CLEAN=0
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

# ---- Argument parsing ----
for arg in "$@"; do
    case "$arg" in
        --native)  NATIVE=1 ;;
        --clean)   CLEAN=1 ;;
        -j*)       JOBS="${arg#-j}" ;;
        *)
            if [[ -d "$arg/target/bin" ]]; then
                OSXCROSS_ROOT="$(cd "$arg" && pwd)"
            else
                echo "ERROR: '$arg' is not a valid osxcross directory" >&2
                echo "Usage: $0 [--native] [--clean] [-jN] [osxcross_path]" >&2
                exit 1
            fi
            ;;
    esac
done

# ---- Detect or validate toolchain ----
if [[ $NATIVE -eq 1 ]]; then
    echo ">>> Native macOS build (xcrun)"
    if ! command -v xcrun &>/dev/null; then
        echo "ERROR: xcrun not found — are you on macOS with Xcode?" >&2
        exit 1
    fi
    CXX="$(xcrun --find clang++)"
    MAKE_ARGS=""
else
    # Try to find osxcross
    if [[ -z "$OSXCROSS_ROOT" ]]; then
        # Check common locations
        for candidate in \
            "$HOME/osxcross" \
            "$HOME/Build/osxcross" \
            "/opt/osxcross" \
            "/usr/local/osxcross"; do
            if [[ -d "$candidate/target/bin" ]]; then
                OSXCROSS_ROOT="$candidate"
                break
            fi
        done
    fi

    # Check PATH as last resort
    if [[ -z "$OSXCROSS_ROOT" ]]; then
        FOUND_CC="$(command -v "${TARGET_TRIPLE}-clang++" 2>/dev/null || true)"
        if [[ -n "$FOUND_CC" ]]; then
            OSXCROSS_ROOT="$(cd "$(dirname "$FOUND_CC")/.." && pwd)"
        fi
    fi

    if [[ -z "$OSXCROSS_ROOT" ]] || [[ ! -x "${OSXCROSS_ROOT}/target/bin/${TARGET_TRIPLE}-clang++" ]]; then
        echo "ERROR: Cannot find osxcross installation." >&2
        echo "" >&2
        echo "  Install osxcross first:" >&2
        echo "    git clone https://github.com/tpoechtrager/osxcross.git" >&2
        echo "    cd osxcross && TARGET=x86_64-apple-darwin22 ./build.sh" >&2
        echo "" >&2
        echo "  Then re-run: $0 /path/to/osxcross" >&2
        echo "  Or use:      $0 --native   (on macOS with Xcode)" >&2
        exit 1
    fi

    export PATH="${OSXCROSS_ROOT}/target/bin:${PATH}"
    CXX="${TARGET_TRIPLE}-clang++"
    MAKE_ARGS="CROSS_CXX=${CXX}"
    echo ">>> Cross-build with osxcross ($OSXCROSS_ROOT)"
fi

# ---- Clean if requested ----
if [[ $CLEAN -eq 1 ]]; then
    echo ">>> Cleaning previous build..."
    make clean 2>/dev/null || true
fi

# ---- Build ----
echo ">>> Compiler: $CXX"
$CXX --version 2>&1 | head -1
echo ""

make -j"$JOBS" $MAKE_ARGS

# ---- Result ----
KEXT="build/IntelXeDriver.kext"
if [[ -f "$KEXT/Contents/MacOS/IntelXeDriver" ]]; then
    echo ""
    echo "=========================================="
    echo "  BUILD SUCCESSFUL"
    echo "=========================================="
    echo "  Output:  $KEXT"
    echo "  Binary:  $(file "$KEXT/Contents/MacOS/IntelXeDriver" 2>/dev/null || echo 'unknown')"
    echo ""
    echo "  Contents:"
    find "$KEXT" -type f | sort | sed 's/^/    /'
    echo ""
    echo "  To load (on macOS):"
    echo "    sudo kextutil $KEXT"
    echo ""
    echo "  To check loaded:"
    echo "    kextstat | grep IntelXe"
    echo ""
else
    echo "ERROR: Build completed but KEXT binary not found" >&2
    exit 1
fi