#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# build.sh — Clean and Build Automation Script
# For CMake presets (e.g., linux-rel, win-rel)
# ============================================================

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_PRESET="linux-rel"
RUN_AFTER_BUILD=0
RUN_ARG_VIDEO=""
DO_CLEAN=0
JOBS="${JOBS:-$(nproc || echo 4)}"

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  -p, --preset <name>   Specify CMake preset (default: linux-rel)
  -c, --clean           Clean the build directory before compiling
  -r, --run <file|uri>  Run the player after build using the provided video file or URI
  -j, --jobs <N>        Number of parallel build jobs (default: number of cores)
  -h, --help            Show this help message

Examples:
  $0 --clean --preset linux-rel
  $0 --preset linux-rel
  $0 --preset linux-rel --run /absolute/path/video.mp4
  $0 --preset win-rel -j 12
EOF
}

log() { echo -e "\033[1;34m[build]\033[0m $*"; }
err() { echo -e "\033[1;31m[error]\033[0m $*" >&2; }

# --- Parse command-line arguments ---
while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--preset) BUILD_PRESET="${2:-}"; shift 2;;
    -c|--clean)  DO_CLEAN=1; shift;;
    -r|--run)    RUN_AFTER_BUILD=1; RUN_ARG_VIDEO="${2:-}"; shift 2;;
    -j|--jobs)   JOBS="${2:-}"; shift 2;;
    -h|--help)   usage; exit 0;;
    *) err "Unknown option: $1"; usage; exit 2;;
  esac
done

cd "$PROJECT_ROOT"

# --- Build directory path based on preset ---
BUILD_DIR="build/${BUILD_PRESET}"

# --- Optional: Clean ---
if [[ $DO_CLEAN -eq 1 ]]; then
  if [[ -d "$BUILD_DIR" ]]; then
    log "Cleaning build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
  else
    log "Nothing to clean: $BUILD_DIR does not exist"
  fi
fi

# --- CMake Configure ---
log "Configuring CMake (preset: ${BUILD_PRESET})"
cmake --preset "${BUILD_PRESET}"

# --- Build ---
log "Building project (jobs: ${JOBS})"
cmake --build --preset "${BUILD_PRESET}" -j "${JOBS}"

# --- Locate final binary ---
BIN="./${BUILD_DIR}/gst_qt_poc"
if [[ ! -x "$BIN" ]]; then
  BIN_ALT="${BUILD_DIR}/gst_qt_poc"
  if [[ -x "$BIN_ALT" ]]; then
    BIN="$BIN_ALT"
  fi
fi

log "Build completed successfully. Binary: ${BIN}"

# --- Optional: Run after build ---
if [[ $RUN_AFTER_BUILD -eq 1 ]]; then
  if [[ -z "${RUN_ARG_VIDEO}" ]]; then
    err "Missing video file or URI for --run option."
    exit 3
  fi

  # Recommended environment stability variables
  export GST_GL_NO_DMABUF="${GST_GL_NO_DMABUF:-1}"
  export GST_PLUGIN_FEATURE_RANK="${GST_PLUGIN_FEATURE_RANK:-vaapidecodebin:0,vaapih264dec:0}"

  log "Launching player with: ${RUN_ARG_VIDEO}"
  exec "${BIN}" "${RUN_ARG_VIDEO}"
fi
