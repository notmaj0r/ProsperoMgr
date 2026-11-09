#!/usr/bin/env bash
# Build ProsperoMgr.elf — wraps this project's actual pipeline
# (frontend -> embed -> make). PS5-only; there is no host/dev build.
#
# Usage:
#   ./build.sh                  # frontend build + embed + PS5 build
#   ./build.sh --skip-frontend  # skip npm build (backend-only iteration)
#   ./build.sh --clean          # wipe build/ first
#   ./build.sh --debug-net      # compile in the /api/debug/net-* probes
#
# --debug-net implies --clean: the flag only changes CXXFLAGS, which make
# cannot see, so objects would otherwise be reused.
#
# Flags may be combined: ./build.sh --clean --skip-frontend
#
# PS5_PAYLOAD_SDK must point at the SDK root (env var, or one of the
# fallback locations below).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT/build"
JOBS="$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)"

OPT_CLEAN=0
OPT_SKIP_FRONTEND=0
OPT_DEBUG_NET=0

for arg in "$@"; do
  case "$arg" in
    --clean)          OPT_CLEAN=1 ;;
    --skip-frontend)  OPT_SKIP_FRONTEND=1 ;;
    --debug-net)      OPT_DEBUG_NET=1; OPT_CLEAN=1 ;;
    *) echo "Unknown argument: $arg"; exit 1 ;;
  esac
done

# ------------------------------------------------------------------ output styling
if [[ -t 1 ]]; then
  IS_TTY=1
  C_RESET=$'\033[0m';  C_BOLD=$'\033[1m'
  C_RED=$'\033[31m';   C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'; C_CYAN=$'\033[36m'; C_DIM=$'\033[2m'
else
  # Piped to a file/log — the live-updating compile counter falls back to a
  # plain scrolling list instead, so a saved/pasted log still reads cleanly.
  IS_TTY=0
  C_RESET=""; C_BOLD=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_CYAN=""; C_DIM=""
fi

fail()    { echo "${C_RED}${C_BOLD}✗ ERROR:${C_RESET} $*" >&2; exit 1; }
warn()    { echo "${C_YELLOW}⚠ WARNING:${C_RESET} $*" >&2; }
info()    { echo "${C_DIM}  ·${C_RESET} $*"; }
ok()      { echo "${C_GREEN}✓${C_RESET} $*"; }
section() { echo; echo "${C_CYAN}${C_BOLD}── $* ──${C_RESET}"; }

BUILD_LOG=""
on_exit() {
  local rc=$?
  [[ -n "$BUILD_LOG" ]] && rm -f "$BUILD_LOG"
  if (( rc != 0 )); then
    echo >&2
    echo "${C_RED}${C_BOLD}✗ BUILD FAILED${C_RESET} (exit $rc) — see the errors above." >&2
  fi
}
trap on_exit EXIT

BUILD_START=$(date +%s)
ELF="$BUILD_DIR/ProsperoMgr.elf"

# ------------------------------------------------------------------ clean
# DEBUG_NET only changes CXXFLAGS, which make cannot see. Without this, turning
# the flag back off would silently reuse objects that still carry the probes.
DEBUG_NET_STAMP="$BUILD_DIR/.debug-net"
if [[ -f "$DEBUG_NET_STAMP" ]]; then
  [[ "$OPT_DEBUG_NET" != "$(cat "$DEBUG_NET_STAMP")" ]] && {
    OPT_CLEAN=1
    info "DEBUG_NET changed since the last build — forcing a clean rebuild."
  }
elif [[ -d "$BUILD_DIR" ]]; then
  # Objects exist but predate the stamp, so their flags are unknown.
  OPT_CLEAN=1
  info "Existing objects have no DEBUG_NET record — forcing a clean rebuild."
fi

if [[ "$OPT_CLEAN" -eq 1 && -d "$BUILD_DIR" ]]; then
  section "Cleaning"
  info "Removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

# ------------------------------------------------------------------ SDK resolve
section "Resolving PS5 payload SDK"

SDK_CANDIDATES=(
  "${PS5_PAYLOAD_SDK:-}"
  "$ROOT/ps5-payload-sdk"
  "/opt/ps5-payload-sdk"
)

SDK_ROOT=""
for candidate in "${SDK_CANDIDATES[@]}"; do
  [[ -z "$candidate" ]] && continue
  if [[ -f "$candidate/toolchain/prospero.mk" ]]; then
    SDK_ROOT="$candidate"
    break
  fi
done

if [[ -z "$SDK_ROOT" ]]; then
  fail "PS5 payload SDK not found. Expected one of:
  - env var PS5_PAYLOAD_SDK
  - $ROOT/ps5-payload-sdk
  - /opt/ps5-payload-sdk"
fi

export PS5_PAYLOAD_SDK="$SDK_ROOT"
ok "SDK: $SDK_ROOT"

for lib in libmicrohttpd libSceSystemService libSceUserService; do
  if ! compgen -G "$SDK_ROOT/target/lib/$lib.*" > /dev/null && \
     ! compgen -G "$SDK_ROOT/target/user/homebrew/lib/$lib.*" > /dev/null; then
    warn "Expected SDK library not found anywhere under $SDK_ROOT: $lib"
  fi
done

command -v make &>/dev/null || fail "make is not installed or not in PATH."
command -v python3 &>/dev/null || fail "python3 is not installed or not in PATH (needed for the asset-embed step)."

# ------------------------------------------------------------------ frontend
if [[ "$OPT_SKIP_FRONTEND" -eq 0 ]]; then
  section "Building frontend"
  command -v npm &>/dev/null || fail "npm is not installed or not in PATH."
  # Drops npm's funding nag and vite's per-asset table (gen_assets.py's own
  # summary line covers that). stderr merges in so vite's warnings stay in order.
  make --no-print-directory -C "$ROOT" frontend 2>&1 | awk '
    /^make(\[[0-9]+\])?: (Entering|Leaving) directory/ { next }
    /packages are looking for funding/ { skip_blank = 1; next }
    /run `npm fund`/ { next }
    skip_blank && /^[[:space:]]*$/ { skip_blank = 0; next }
    { skip_blank = 0 }
    /^dist\// { next }
    { print }
  ' | sed "s/^/${C_DIM}  ·${C_RESET} /"
  ok "Frontend built and embedded into backend/src/assets.cpp"

  section "Validating locales"
  command -v node &>/dev/null || fail "node is not installed or not in PATH."
  if out=$(node "$ROOT/source/frontend/scripts/check-locales.mjs" 2>&1); then
    ok "$(echo "$out" | grep -v '^[[:space:]]*$' | tail -1)"
  else
    echo "$out" >&2
    fail "locale validation failed — the locales are compiled into the ELF, so this would ship."
  fi
else
  info "Skipping frontend build (--skip-frontend) — using whatever assets.cpp already has embedded."
fi

# ------------------------------------------------------------------ bundled payloads
if ls "$ROOT"/source/payloads/*.elf &>/dev/null; then
  section "Embedding bundled payloads"
  make --no-print-directory -C "$ROOT" payloads | sed "s/^/${C_DIM}  ·${C_RESET} /"
  ok "Bundled payloads embedded"
fi

# ------------------------------------------------------------------ PS5 build
section "Building ProsperoMgr.elf"
if [[ "$OPT_DEBUG_NET" -eq 1 ]]; then
  warn "DEBUG_NET build: /api/debug/net-sink and net-source are unauthenticated. Do not ship this."
fi
# Removed up front so a failed build cannot leave the previous ELF sitting
# here looking deployable.
rm -f "$ELF"

# How many files this invocation will actually compile — a dry run, so an
# main.cpp carries the build time shown in the startup log, so it is rebuilt every run.
touch "$ROOT/source/backend/src/main.cpp"
# incremental build shows its real (smaller) total instead of every source file.
TOTAL_UNITS=$(PS5_PAYLOAD_SDK="$SDK_ROOT" DEBUG_NET="$OPT_DEBUG_NET" \
              make -n --no-print-directory -C "$ROOT" 2>/dev/null | grep -c -- '-c -o build/obj-ps5/' || true)

BUILD_LOG="$(mktemp)"
# -j compiles independent translation units in parallel; --output-sync=target
# keeps each file's output from interleaving with whatever else is running.
PS5_PAYLOAD_SDK="$SDK_ROOT" DEBUG_NET="$OPT_DEBUG_NET" make -j"$JOBS" --output-sync=target --no-print-directory -C "$ROOT" 2>&1 | tee "$BUILD_LOG" | awk -v dim="$C_DIM" -v rst="$C_RESET" -v total="$TOTAL_UNITS" -v tty="$IS_TTY" '
  /^make(\[[0-9]+\])?: (Entering|Leaving) directory/ { next }
  /\\$/ { next } # continuation line of a multi-line recipe — the next line has the real command
  / -c -o build\/obj-ps5\/[^ ]+\.o / && ($NF ~ /\.cpp$/ || $NF ~ /\.c$/) {
    n++
    split($NF, parts, "/")
    line = sprintf("%s  · [%d/%d] compiling %-40s%s", dim, n, total, parts[length(parts)], rst)
    if (tty) { printf "\r%s", line; fflush(); progress = 1 }
    else print line
    next
  }
  / -o build\/ProsperoMgr\.elf / && $0 !~ / -c / {
    if (tty && progress) printf "\r%60s\r", ""
    printf "%s  · linking ProsperoMgr.elf%s\n", dim, rst
    progress = 0
    next
  }
  {
    if (progress) { printf "\n"; progress = 0 }
    print
  }
  END { if (progress) printf "\n" }
'

PS5_WARNINGS=$(grep -c 'warning:' "$BUILD_LOG" || true)
section "Summary"
if [[ "$PS5_WARNINGS" -gt 0 ]]; then
  warn "$PS5_WARNINGS compiler warning(s):"
  grep 'warning:' "$BUILD_LOG" | sed "s/^/  /"
else
  ok "No compiler warnings."
fi
mkdir -p "$BUILD_DIR"
echo "$OPT_DEBUG_NET" > "$DEBUG_NET_STAMP"

BUILD_ELAPSED=$(( $(date +%s) - BUILD_START ))
if [[ -f "$ELF" ]]; then
  size=$(( $(wc -c < "$ELF") / 1024 ))
  ok "Built $ELF (${size} KiB) in ${BUILD_ELAPSED}s"
else
  warn "Build finished, but ProsperoMgr.elf was not found in build directory."
fi
