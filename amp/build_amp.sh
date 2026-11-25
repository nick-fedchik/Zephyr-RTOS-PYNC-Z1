#!/usr/bin/env bash
set -euo pipefail

AMP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="$(cd "$AMP_DIR/.." && pwd)"
VENV_DIR="$WORK_DIR/.venv"
TOOLS_DIR="$WORK_DIR/.tools"
ZEPHYR_WS="$WORK_DIR/zephyr-ws"
OUT_DIR="$AMP_DIR/build"
SDK_VER="${SDK_VER:-1.0.1}"
SDK_DIR="$TOOLS_DIR/zephyr-sdk-${SDK_VER}"

mkdir -p "$OUT_DIR"

echo "=== [1/3] Compiling Linux DT overlay (openamp.dts -> openamp.dtbo) ==="
dtc -@ -I dts -O dtb -o "$OUT_DIR/openamp.dtbo" "$AMP_DIR/openamp.dts"
ls -lh "$OUT_DIR/openamp.dtbo"

if [[ -f "$WORK_DIR/sd_backup/image.ub" ]] && command -v fdtoverlay >/dev/null 2>&1; then
	echo "=== Validating openamp.dtbo against sd_backup/image.ub fdt-0 ==="
	dumpimage -T flat_dt -p 1 -o "$OUT_DIR/base_fdt.dtb" "$WORK_DIR/sd_backup/image.ub" >/dev/null
	fdtoverlay -i "$OUT_DIR/base_fdt.dtb" -o "$OUT_DIR/pynq-amp.dtb" "$OUT_DIR/openamp.dtbo"
	rm -f "$OUT_DIR/base_fdt.dtb"
	echo ">>> Overlay applied cleanly -> $OUT_DIR/pynq-amp.dtb"
fi

echo "=== [2/3] Building Zephyr AMP firmware for CPU1 (0x1e000000) ==="
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$SDK_DIR"
# shellcheck disable=SC1091
source "$ZEPHYR_WS/zephyr/zephyr-env.sh"

west build -p always -b pynq_z1 "$AMP_DIR/zephyr_cpu1" -d "$OUT_DIR/zephyr_build" -- \
	-DBOARD_ROOT="$WORK_DIR"

cp "$OUT_DIR/zephyr_build/zephyr/zephyr.elf" "$OUT_DIR/zephyr_amp.elf"

echo "=== [3/3] Verifying ELF load address and .resource_table ==="
READELF="$SDK_DIR/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-readelf"
"$READELF" -lW "$OUT_DIR/zephyr_amp.elf" | grep -E 'Type|LOAD'
"$READELF" -SW "$OUT_DIR/zephyr_amp.elf" | grep -E 'Name|resource_table|nocache'

echo "================================================================="
echo "AMP artifacts ready in $OUT_DIR/:"
ls -lh "$OUT_DIR/openamp.dtbo" "$OUT_DIR/zephyr_amp.elf"
echo "================================================================="
