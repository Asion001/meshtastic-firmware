#!/usr/bin/env bash
# Run the Meshtastic BaseUI in a 240x135 desktop window that mimics the M5Stack Cardputer-Adv.
#
# Usage:
#   bin/cardputer-sim.sh                 # build native-tft (if needed) and open the window
#   bin/cardputer-sim.sh --no-build      # reuse the existing build
#   bin/cardputer-sim.sh --fresh         # wipe the simulated flash (fresh node) before starting
#   bin/cardputer-sim.sh --screenshot out.png [--wait 15]
#                                        # headless: run under Xvfb, save a 3x screenshot, exit
#
# Env: SIM_NAME (default "cardputer") picks the virtual filesystem under ~/.portduino/,
#      SIM_MAC   (default AA:BB:CC:DD:EE:01) is the node's MAC, so several sims can run side by side
#      SIM_PORT  (default 4403) is the API port, for the Python CLI / web client / meshtastic-mcp.
#
# Keys in the window: arrows = navigate, Enter = select. Close the window to quit.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_NAME="native-tft"
BIN="$ROOT/.pio/build/$ENV_NAME/meshtasticd"
CONFIG="$ROOT/bin/cardputer-sim.yaml"
SIM_NAME="${SIM_NAME:-cardputer}"
SIM_MAC="${SIM_MAC:-AA:BB:CC:DD:EE:01}"
SIM_PORT="${SIM_PORT:-4403}"
FSDIR="${HOME}/.portduino/${SIM_NAME}"

build=1
fresh=0
shot=""
wait_s=15
while [[ $# -gt 0 ]]; do
    case "$1" in
    --no-build) build=0 ;;
    --fresh) fresh=1 ;;
    --screenshot)
        shot="$2"
        shift
        ;;
    --wait)
        wait_s="$2"
        shift
        ;;
    -h | --help)
        sed -n '2,17p' "$0"
        exit 0
        ;;
    *)
        echo "unknown option: $1" >&2
        exit 2
        ;;
    esac
    shift
done

if [[ $build -eq 1 || ! -x $BIN ]]; then
    (cd "$ROOT" && pio run -e "$ENV_NAME")
fi
[[ $fresh -eq 1 ]] && rm -rf "$FSDIR"
mkdir -p "$FSDIR"

args=(-c "$CONFIG" -d "$FSDIR" -h "$SIM_MAC" -p "$SIM_PORT")

if [[ -z $shot ]]; then
    exec "$BIN" "${args[@]}"
fi

# Headless screenshot mode (CI / agents): needs xvfb-run and ImageMagick's `import`.
for tool in xvfb-run import convert; do
    command -v "$tool" >/dev/null || {
        echo "--screenshot needs $tool" >&2
        exit 2
    }
done
shot="$(realpath -m "$shot")"
xvfb-run -a -s "-screen 0 640x480x24" bash -c '
    "$1" "${@:4}" >/dev/null 2>&1 &
    pid=$!
    sleep "$2"
    import -window root /tmp/cardputer-sim-root.$$.png
    geom=$(xwininfo -root -tree | grep -m1 "LGFX" | grep -oE "[0-9]+x[0-9]+\+[0-9]+\+[0-9]+" | head -1)
    convert /tmp/cardputer-sim-root.$$.png -crop "$geom" +repage -filter point -resize 300% "$3"
    rm -f /tmp/cardputer-sim-root.$$.png
    kill -9 "$pid" 2>/dev/null || true
' _ "$BIN" "$wait_s" "$shot" "${args[@]}"
echo "saved $shot"
