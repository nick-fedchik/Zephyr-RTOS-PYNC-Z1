#!/usr/bin/env bash
set -euo pipefail

WORK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$WORK_DIR/.venv"
TOOLS_DIR="$WORK_DIR/.tools"
ZEPHYR_WS="$WORK_DIR/zephyr-ws"
BUILD_DIR="$WORK_DIR/build"
SD_DIR="$WORK_DIR/sd_card_files"

if [[ -f "$WORK_DIR/local.conf" ]]; then
	# shellcheck disable=SC1091
	source "$WORK_DIR/local.conf"
fi

ZEPHYR_VER="${ZEPHYR_VER:-v4.5.0}"
SDK_VER="${SDK_VER:-1.0.1}"
PYNQ_ZIP_URL="${PYNQ_ZIP_URL:-https://download.amd.com/opendownload/pynq/pynq_z1_v3.1.1.zip}"
PYNQ_IMAGE_PATH="${PYNQ_IMAGE_PATH:-}"
SKIP_PYNQ_IMAGE=0

# Optional TFTP netboot (disabled by default; configure via flags, env, or local.conf)
TFTP_SERVER_IP="${TFTP_SERVER_IP:-}"
BOARD_IP="${BOARD_IP:-}"
BOARD_NETMASK="${BOARD_NETMASK:-255.255.255.0}"
BOARD_GW="${BOARD_GW:-}"
BOARD_MAC="${BOARD_MAC:-02:50:59:4e:51:01}"
TFTP_PREFIX="${TFTP_PREFIX:-pynq1}"
TFTP_DIR="${TFTP_DIR:-/srv/tftp/${TFTP_PREFIX}}"

usage() {
	cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --pynq-image <path>    Use a local PYNQ-Z1 .zip or .img file instead of downloading
  --skip-pynq-image      Skip downloading/extracting BOOT.BIN from PYNQ-Z1 image
  --tftp-server <ip>     Enable two-stage TFTP netboot in boot.scr (with SD fallback)
  -h, --help             Show this help message

Environment overrides (can also be placed in git-ignored local.conf):
  ZEPHYR_VER             Zephyr git tag/branch (default: v4.5.0)
  SDK_VER                Zephyr SDK version (default: 1.0.1)
  PYNQ_ZIP_URL           PYNQ-Z1 image zip URL (default: v3.1.1 on download.amd.com)
  TFTP_SERVER_IP         TFTP server IP; if unset, boot.scr boots directly from SD
  BOARD_IP               Optional static IP for U-Boot (if unset, U-Boot uses DHCP)
  BOARD_NETMASK          Netmask when BOARD_IP is set (default: 255.255.255.0)
  BOARD_GW               Optional gateway IP when BOARD_IP is set
  BOARD_MAC              Locally administered MAC for U-Boot (default: 02:50:59:4e:51:01)
  TFTP_PREFIX            Subdirectory under TFTP root (default: pynq1)
  TFTP_DIR               Local staging dir (default: /srv/tftp/\$TFTP_PREFIX)
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--pynq-image)
		[[ $# -ge 2 ]] || { echo "ERROR: --pynq-image requires a path" >&2; exit 1; }
		PYNQ_IMAGE_PATH="$2"
		shift 2
		;;
	--skip-pynq-image)
		SKIP_PYNQ_IMAGE=1
		shift
		;;
	--tftp-server)
		[[ $# -ge 2 ]] || { echo "ERROR: --tftp-server requires an IP address" >&2; exit 1; }
		TFTP_SERVER_IP="$2"
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "ERROR: Unknown option: $1" >&2
		usage >&2
		exit 1
		;;
	esac
done

HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
x86_64 | aarch64) ;;
*)
	echo "ERROR: Unsupported host architecture: $HOST_ARCH" >&2
	exit 1
	;;
esac

missing_cmds=()
for cmd in python3 cmake ninja dtc gperf wget tar xz mkimage; do
	if ! command -v "$cmd" >/dev/null 2>&1; then
		missing_cmds+=("$cmd")
	fi
done

if [[ "$SKIP_PYNQ_IMAGE" -eq 0 && ! -f "$SD_DIR/BOOT.BIN" ]]; then
	if ! command -v unzip >/dev/null 2>&1; then
		missing_cmds+=("unzip")
	fi
fi

if [[ ${#missing_cmds[@]} -gt 0 ]]; then
	echo "ERROR: Missing required host tools: ${missing_cmds[*]}" >&2
	echo "Install prerequisites on Debian/Ubuntu with:" >&2
	echo "  sudo apt install -y git cmake ninja-build gperf ccache device-tree-compiler wget xz-utils unzip u-boot-tools mtools python3-venv picocom" >&2
	exit 1
fi

LOOP_DEV=""
MNT_DIR=""
cleanup() {
	if [[ -n "$MNT_DIR" ]] && mountpoint -q "$MNT_DIR" 2>/dev/null; then
		sudo umount "$MNT_DIR" || true
	fi
	if [[ -n "$LOOP_DEV" ]]; then
		sudo losetup -d "$LOOP_DEV" 2>/dev/null || true
	fi
}
trap cleanup EXIT

mkdir -p "$TOOLS_DIR" "$SD_DIR"

echo "=== [1/5] Preparing local Python venv and Zephyr workspace ($ZEPHYR_VER) ==="
if [[ ! -d "$VENV_DIR" ]]; then
	python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"

if ! python3 -c "import west" >/dev/null 2>&1; then
	pip install -q --upgrade pip west
fi

if [[ ! -d "$ZEPHYR_WS/.west" ]]; then
	mkdir -p "$ZEPHYR_WS"
	west init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_VER" "$ZEPHYR_WS"
fi

pushd "$ZEPHYR_WS" >/dev/null
west config manifest.project-filter -- "-.*,+cmsis,+picolibc,+libmetal,+open-amp"
west update
pip install -q -r "$ZEPHYR_WS/zephyr/scripts/requirements-base.txt"
popd >/dev/null

if [[ -d "$WORK_DIR/patches" ]]; then
	for patch_file in "$WORK_DIR"/patches/*.patch; do
		[[ -f "$patch_file" ]] || continue
		if git -C "$ZEPHYR_WS/zephyr" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
			continue
		fi
		echo ">>> Applying Zephyr patch: $(basename "$patch_file")"
		git -C "$ZEPHYR_WS/zephyr" apply "$patch_file"
	done
fi

echo "=== [2/5] Installing Zephyr SDK ($SDK_VER, arm-zephyr-eabi) into .tools ==="
SDK_DIR="$TOOLS_DIR/zephyr-sdk-${SDK_VER}"
if [[ ! -d "$SDK_DIR/gnu/arm-zephyr-eabi" && ! -d "$SDK_DIR/arm-zephyr-eabi" ]]; then
	SDK_TAR="zephyr-sdk-${SDK_VER}_linux-${HOST_ARCH}_minimal.tar.xz"
	SDK_URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VER}/${SDK_TAR}"
	if [[ ! -d "$SDK_DIR" ]]; then
		wget -q --show-progress -O "$TOOLS_DIR/$SDK_TAR" "$SDK_URL"
		tar -xf "$TOOLS_DIR/$SDK_TAR" -C "$TOOLS_DIR"
		rm -f "$TOOLS_DIR/$SDK_TAR"
	fi
	"$SDK_DIR/setup.sh" -t arm-zephyr-eabi
fi

export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$SDK_DIR"
# shellcheck disable=SC1091
source "$ZEPHYR_WS/zephyr/zephyr-env.sh"

echo "=== [3/5] Building Zephyr (native board: pynq_z1) ==="
west build -p always -b pynq_z1 "$WORK_DIR" -d "$BUILD_DIR" -- \
	-DBOARD_ROOT="$WORK_DIR"

READELF="$SDK_DIR/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-readelf"
if [[ ! -x "$READELF" ]]; then
	READELF="$SDK_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi-readelf"
fi
LOAD_ADDR=$("$READELF" -lW "$BUILD_DIR/zephyr/zephyr.elf" | awk '$1 == "LOAD" {print $4; exit}')
ENTRY_ADDR=$("$READELF" -h "$BUILD_DIR/zephyr/zephyr.elf" | awk '/Entry point address:/ {print $4}')

if [[ -z "$LOAD_ADDR" || -z "$ENTRY_ADDR" ]]; then
	echo "ERROR: Failed to extract LOAD_ADDR or ENTRY_ADDR from zephyr.elf" >&2
	exit 1
fi

echo ">>> Built: $BUILD_DIR/zephyr/zephyr.bin"
echo ">>> LOAD_ADDR=$LOAD_ADDR  ENTRY_ADDR=$ENTRY_ADDR"

echo "=== [4/5] Preparing PYNQ-Z1 BOOT.BIN ==="
if [[ -f "$SD_DIR/BOOT.BIN" ]]; then
	echo ">>> Reusing existing $SD_DIR/BOOT.BIN"
elif [[ "$SKIP_PYNQ_IMAGE" -eq 1 ]]; then
	echo ">>> Skipping PYNQ-Z1 image download (--skip-pynq-image). Place BOOT.BIN into $SD_DIR/ manually."
else
	PYNQ_CACHE="$TOOLS_DIR/pynq_img"
	mkdir -p "$PYNQ_CACHE"
	IMG_FILE=""

	if [[ -n "$PYNQ_IMAGE_PATH" ]]; then
		if [[ ! -f "$PYNQ_IMAGE_PATH" ]]; then
			echo "ERROR: File not found: $PYNQ_IMAGE_PATH" >&2
			exit 1
		fi
		case "$PYNQ_IMAGE_PATH" in
		*.zip)
			unzip -oq "$PYNQ_IMAGE_PATH" -d "$PYNQ_CACHE"
			;;
		*.img)
			IMG_FILE="$PYNQ_IMAGE_PATH"
			;;
		*)
			echo "ERROR: --pynq-image must be a .zip or .img file" >&2
			exit 1
			;;
		esac
	else
		ZIP_FILE="$PYNQ_CACHE/pynq-z1.zip"
		if [[ ! -f "$ZIP_FILE" ]]; then
			wget -q --show-progress -O "$ZIP_FILE" "$PYNQ_ZIP_URL"
		fi
		if ! find "$PYNQ_CACHE" -maxdepth 2 -name '*.img' -print -quit | grep -q .; then
			unzip -oq "$ZIP_FILE" -d "$PYNQ_CACHE"
		fi
	fi

	if [[ -z "$IMG_FILE" ]]; then
		IMG_FILE="$(find "$PYNQ_CACHE" -maxdepth 2 -name '*.img' -print | head -n 1)"
	fi
	if [[ -z "$IMG_FILE" || ! -f "$IMG_FILE" ]]; then
		echo "ERROR: No .img file found after unpacking PYNQ-Z1 archive" >&2
		exit 1
	fi

	if command -v mcopy >/dev/null 2>&1; then
		PART_OFFSET="$(python3 - "$IMG_FILE" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
entry = mbr[446:462]
lba_start = struct.unpack("<I", entry[8:12])[0]
if lba_start == 0:
    sys.exit(1)
print(lba_start * 512)
PY
)"
		mcopy -o -i "${IMG_FILE}@@${PART_OFFSET}" ::/BOOT.BIN "$SD_DIR/BOOT.BIN"
		mcopy -o -i "${IMG_FILE}@@${PART_OFFSET}" ::/u-boot.img "$SD_DIR/u-boot.img" 2>/dev/null || true
	else
		LOOP_DEV="$(sudo losetup --find --show --read-only --partscan "$IMG_FILE")"
		MNT_DIR="$PYNQ_CACHE/mnt"
		mkdir -p "$MNT_DIR"
		sudo mount -o ro "${LOOP_DEV}p1" "$MNT_DIR"
		cp "$MNT_DIR/BOOT.BIN" "$SD_DIR/BOOT.BIN"
		if [[ -f "$MNT_DIR/u-boot.img" ]]; then
			cp "$MNT_DIR/u-boot.img" "$SD_DIR/u-boot.img"
		fi
		sudo umount "$MNT_DIR"
		sudo losetup -d "$LOOP_DEV"
		LOOP_DEV=""
	fi
fi

echo "=== [5/5] Generating boot scripts and staging files ==="
cp "$BUILD_DIR/zephyr/zephyr.bin" "$SD_DIR/zephyr.bin"

if [[ -n "$TFTP_SERVER_IP" ]]; then
	cat <<EOF >"$TOOLS_DIR/netboot.cmd"
echo "=== TFTP Netboot: Zephyr RTOS on PYNQ-Z1 ==="
if tftpboot ${LOAD_ADDR} ${TFTP_PREFIX}/zephyr.bin; then
	dcache off
	icache off
	go ${ENTRY_ADDR}
fi
EOF

	mkimage -A arm -O linux -T script -C none -a 0 -e 0 -n "Zephyr PYNQ-Z1 Netboot" \
		-d "$TOOLS_DIR/netboot.cmd" "$SD_DIR/netboot.scr"

	{
		echo "echo \"=== PYNQ-Z1 Boot: trying TFTP (${TFTP_SERVER_IP}) ===\""
		echo "setenv ethaddr ${BOARD_MAC}"
		if [[ -n "$BOARD_IP" ]]; then
			echo "setenv ipaddr ${BOARD_IP}"
			echo "setenv netmask ${BOARD_NETMASK}"
			if [[ -n "$BOARD_GW" ]]; then
				echo "setenv gatewayip ${BOARD_GW}"
			fi
		else
			echo "setenv autoload no"
			echo "dhcp"
		fi
		echo "setenv serverip ${TFTP_SERVER_IP}"
		echo "setenv tftptimeout 1000"
		echo "setenv tftptimeoutcountmax 3"
		echo "if tftpboot 0x03100000 ${TFTP_PREFIX}/netboot.scr; then"
		echo "	source 0x03100000"
		echo "fi"
		echo "echo \"=== TFTP unavailable, falling back to SD card ===\""
		echo "fatload mmc 0:1 ${LOAD_ADDR} zephyr.bin"
		echo "dcache off"
		echo "icache off"
		echo "go ${ENTRY_ADDR}"
	} >"$TOOLS_DIR/boot.cmd"

	if [[ -d "$TFTP_DIR" && -w "$TFTP_DIR" ]]; then
		cp "$SD_DIR/netboot.scr" "$SD_DIR/zephyr.bin" "$TFTP_DIR/"
		chmod 644 "$TFTP_DIR/netboot.scr" "$TFTP_DIR/zephyr.bin"
		echo ">>> Staged TFTP netboot files in: $TFTP_DIR/"
	fi
else
	rm -f "$SD_DIR/netboot.scr"
	cat <<EOF >"$TOOLS_DIR/boot.cmd"
echo "=== Starting Zephyr RTOS on PYNQ-Z1 ==="
fatload mmc 0:1 ${LOAD_ADDR} zephyr.bin
dcache off
icache off
go ${ENTRY_ADDR}
EOF
fi

mkimage -A arm -O linux -T script -C none -a 0 -e 0 -n "Zephyr PYNQ-Z1" \
	-d "$TOOLS_DIR/boot.cmd" "$SD_DIR/boot.scr"

echo "================================================================="
echo "Done. Copy these files to the root of a FAT32 microSD card:"
ls -lh "$SD_DIR"
echo "================================================================="
