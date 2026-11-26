#!/usr/bin/env bash
set -euo pipefail

# Isolated build of minimal Embedded Linux rootfs (Buildroot + BusyBox +
# Dropbear SSH + rt-tests + i2c-tools + ethtool + rpmsg-cli + Zephyr AMP)
# for Digilent PYNQ-Z1.
#
# Usage:
#   ./buildroot/build_buildroot.sh
#   ./buildroot/build_buildroot.sh --tftp-dir /srv/tftp/pynq1

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BR_VER="${BR_VER:-2026.08.2}"
WORK_DIR="${ROOT_DIR}/.tools/buildroot"
BR_DIR="${WORK_DIR}/buildroot-${BR_VER}"
OUT_DIR="${WORK_DIR}/output"
OVERLAY_STAGE="${WORK_DIR}/overlay-stage"
TARBALL="${WORK_DIR}/buildroot-${BR_VER}.tar.xz"
TFTP_DIR="${TFTP_DIR:-}"

if [[ -f "${ROOT_DIR}/local.conf" ]]; then
  # shellcheck disable=SC1091
  source "${ROOT_DIR}/local.conf"
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tftp-dir) TFTP_DIR="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 [--tftp-dir /path/to/tftp/subdir]"
      exit 0
      ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

mkdir -p "${WORK_DIR}"

if [[ ! -d "${BR_DIR}" ]]; then
  if [[ ! -f "${TARBALL}" ]]; then
    echo ">>> Downloading Buildroot ${BR_VER}..."
    curl -fL "https://buildroot.org/downloads/buildroot-${BR_VER}.tar.xz" -o "${TARBALL}"
  fi
  echo ">>> Extracting ${TARBALL}..."
  tar -xf "${TARBALL}" -C "${WORK_DIR}"
fi

# Prepare staging rootfs overlay (init script, zephyr_amp.elf, optional SSH key)
rm -rf "${OVERLAY_STAGE}"
mkdir -p "${OVERLAY_STAGE}/etc/init.d" "${OVERLAY_STAGE}/lib/firmware" \
         "${OVERLAY_STAGE}/usr/bin" "${OVERLAY_STAGE}/root/.ssh"
cp "${ROOT_DIR}/buildroot/rootfs-overlay/etc/init.d/S90zephyr-amp" \
   "${OVERLAY_STAGE}/etc/init.d/S90zephyr-amp"
chmod 0755 "${OVERLAY_STAGE}/etc/init.d/S90zephyr-amp"

# Include Zephyr CPU1 AMP ELF if available
ZEPHYR_ELF=""
for candidate in \
  "${ROOT_DIR}/amp/build/zephyr_amp.elf" \
  "${ROOT_DIR}/amp/build/zephyr_build/zephyr/zephyr.elf" \
  "${TFTP_DIR:+${TFTP_DIR}/zephyr_amp.elf}"; do
  if [[ -n "${candidate}" && -f "${candidate}" ]]; then
    ZEPHYR_ELF="${candidate}"
    break
  fi
done
if [[ -n "${ZEPHYR_ELF}" ]]; then
  cp "${ZEPHYR_ELF}" "${OVERLAY_STAGE}/lib/firmware/zephyr_amp.elf"
  echo ">>> Embedded ${ZEPHYR_ELF} -> /lib/firmware/zephyr_amp.elf"
else
  echo "WARNING: zephyr_amp.elf not found; build amp/ first to bundle it in initramfs"
fi

# Include host SSH public key for passwordless root login if present
chmod 0700 "${OVERLAY_STAGE}/root/.ssh"
for pub in "${HOME}/.ssh/id_ed25519.pub" "${HOME}/.ssh/id_rsa.pub"; do
  if [[ -f "${pub}" ]]; then
    cat "${pub}" >> "${OVERLAY_STAGE}/root/.ssh/authorized_keys"
  fi
done
if [[ -f "${OVERLAY_STAGE}/root/.ssh/authorized_keys" ]]; then
  chmod 0600 "${OVERLAY_STAGE}/root/.ssh/authorized_keys"
fi

# Configure Buildroot
cp "${ROOT_DIR}/buildroot/pynq_z1_defconfig" "${WORK_DIR}/pynq_z1_defconfig"
printf 'BR2_ROOTFS_OVERLAY="%s"\n' "${OVERLAY_STAGE}" >> "${WORK_DIR}/pynq_z1_defconfig"

make -C "${BR_DIR}" O="${OUT_DIR}" defconfig BR2_DEFCONFIG="${WORK_DIR}/pynq_z1_defconfig"

# Build toolchain first so we can cross-compile rpmsg-cli with the exact target libc
echo ">>> Building Buildroot toolchain + packages..."
make -C "${BR_DIR}" O="${OUT_DIR}" -j"$(nproc)" toolchain

TARGET_CC="${OUT_DIR}/host/bin/arm-linux-gcc"
echo ">>> Compiling rpmsg-cli with ${TARGET_CC}..."
"${TARGET_CC}" -O2 -Wall -Wextra "${ROOT_DIR}/buildroot/rpmsg-cli.c" \
  -o "${OVERLAY_STAGE}/usr/bin/rpmsg-cli"

echo ">>> Building rootfs.cpio.uboot..."
make -C "${BR_DIR}" O="${OUT_DIR}" -j"$(nproc)"

ROOTFS_IMG="${OUT_DIR}/images/rootfs.cpio.uboot"
echo ">>> Built: ${ROOTFS_IMG} ($(du -h "${ROOTFS_IMG}" | cut -f1))"

if [[ -n "${TFTP_DIR}" && -d "${TFTP_DIR}" ]]; then
  cp "${ROOTFS_IMG}" "${TFTP_DIR}/rootfs.cpio.uboot"
  echo ">>> Staged to ${TFTP_DIR}/rootfs.cpio.uboot"
fi
