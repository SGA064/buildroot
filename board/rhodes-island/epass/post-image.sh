#!/bin/sh

set -eu

BOARD_DIR="$(dirname "$0")"
KERNEL_IMAGE="${BINARIES_DIR}/zImage"
LINUX_DTB="${BINARIES_DIR}/suniv-f1c200s-epass.dtb"
OLD_APPENDED_IMAGE="${BINARIES_DIR}/zImage.suniv-f1c200s-epass"
FLASH_SCRIPT="${BOARD_DIR}/epass-flash.sh"

if [ -f "${KERNEL_IMAGE}" ] && [ -f "${LINUX_DTB}" ]; then
	cat "${KERNEL_IMAGE}" "${LINUX_DTB}" > "${KERNEL_IMAGE}.tmp"
	mv "${KERNEL_IMAGE}.tmp" "${KERNEL_IMAGE}"
fi
rm -f "${OLD_APPENDED_IMAGE}"

if [ -f "${FLASH_SCRIPT}" ]; then
	install -m 0755 "${FLASH_SCRIPT}" "${BINARIES_DIR}/epass-flash.sh"
fi
