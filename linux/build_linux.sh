#!/usr/bin/env bash
set -euo pipefail

# Isolated build of minimal Mainline Linux 7.3.6 (PREEMPT_RT + FPGA Manager +
# ported zynq_remoteproc + RPMsg) for Digilent PYNQ-Z1.
#
# Usage:
#   ./linux/build_linux.sh
#   ./linux/build_linux.sh --tftp-dir /srv/tftp/pynq1

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX_VER="${LINUX_VER:-7.3.6}"
LINUX_MAJOR="${LINUX_VER%%.*}"
WORK_DIR="${ROOT_DIR}/.tools/linux"
SRC_DIR="${WORK_DIR}/linux-${LINUX_VER}"
TARBALL="${WORK_DIR}/linux-${LINUX_VER}.tar.xz"
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

if ! command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
  echo "ERROR: arm-linux-gnueabihf-gcc not found. Install gcc-arm-linux-gnueabihf." >&2
  exit 1
fi

mkdir -p "${WORK_DIR}"

if [[ ! -d "${SRC_DIR}" ]]; then
  if [[ ! -f "${TARBALL}" ]]; then
    echo ">>> Downloading Mainline Linux ${LINUX_VER}..."
    curl -fL "https://cdn.kernel.org/pub/linux/kernel/v${LINUX_MAJOR}.x/linux-${LINUX_VER}.tar.xz" \
      -o "${TARBALL}"
  fi
  echo ">>> Extracting ${TARBALL}..."
  tar -xf "${TARBALL}" -C "${WORK_DIR}"
fi

echo ">>> Installing pynq_z1_min_defconfig and zynq_remoteproc..."
cp "${ROOT_DIR}/linux/pynq_z1_min_defconfig" "${SRC_DIR}/arch/arm/configs/pynq_z1_min_defconfig"
cp "${ROOT_DIR}/linux/zynq_remoteproc.c" "${SRC_DIR}/drivers/remoteproc/zynq_remoteproc.c"

if ! grep -q 'CONFIG_ZYNQ_REMOTEPROC' "${SRC_DIR}/drivers/remoteproc/Makefile"; then
  printf 'obj-$(CONFIG_ZYNQ_REMOTEPROC)\t\t+= zynq_remoteproc.o\n' \
    >> "${SRC_DIR}/drivers/remoteproc/Makefile"
fi

if ! grep -q 'config ZYNQ_REMOTEPROC' "${SRC_DIR}/drivers/remoteproc/Kconfig"; then
  python3 - "${SRC_DIR}/drivers/remoteproc/Kconfig" << 'PYEOF'
import sys
p = sys.argv[1]
txt = open(p).read()
snippet = """
config ZYNQ_REMOTEPROC
\ttristate "Support Zynq-7000 remoteproc"
\tdepends on ARCH_ZYNQ && SMP
\tselect RPMSG_VIRTIO
\thelp
\t  Say y or m here to support Zynq-7000 CPU1 remote processor via the
\t  remoteproc framework.

endif # REMOTEPROC"""
assert "endif # REMOTEPROC" in txt
open(p, "w").write(txt.replace("endif # REMOTEPROC", snippet, 1))
PYEOF
fi

echo ">>> Configuring and building zImage..."
make -C "${SRC_DIR}" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- pynq_z1_min_defconfig
make -C "${SRC_DIR}" -j"$(nproc)" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage

ZIMAGE="${SRC_DIR}/arch/arm/boot/zImage"
echo ">>> Built: ${ZIMAGE} ($(du -h "${ZIMAGE}" | cut -f1))"

if [[ -n "${TFTP_DIR}" && -d "${TFTP_DIR}" ]]; then
  cp "${ZIMAGE}" "${TFTP_DIR}/zImage-${LINUX_VER}"
  echo ">>> Staged to ${TFTP_DIR}/zImage-${LINUX_VER}"
fi
