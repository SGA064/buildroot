#!/bin/sh

set -eu

BOARD_DIR="$(dirname "$0")"
KERNEL_IMAGE="${BINARIES_DIR}/zImage"
LINUX_DTB="${BINARIES_DIR}/suniv-f1c200s-epass.dtb"
OLD_APPENDED_IMAGE="${BINARIES_DIR}/zImage.suniv-f1c200s-epass"
FIT_SOURCE="${BOARD_DIR}/epass.its"
FIT_WORK="${BINARIES_DIR}/epass.its"
FIT_IMAGE="${BINARIES_DIR}/fitImage.itb"
FLASH_SCRIPT="${BOARD_DIR}/epass-flash.sh"
MKIMAGE="${HOST_DIR}/bin/mkimage"
DUMPIMAGE="${HOST_DIR}/bin/dumpimage"
KERNEL_MAX=$((0x800000))

die()
{
	echo "error: $*" >&2
	exit 1
}

file_size()
{
	wc -c < "$1" | tr -d ' '
}

check_size()
{
	name="$1"
	image="$2"
	max="$3"
	size="$(file_size "$image")"

	if [ "$size" -gt "$max" ]; then
		die "$name image is too large: $image is $size bytes, max is $max bytes"
	fi
}

if [ -f "${KERNEL_IMAGE}" ] && [ -f "${LINUX_DTB}" ]; then
	[ -x "${MKIMAGE}" ] ||
		die "mkimage not found: enable BR2_PACKAGE_HOST_UBOOT_TOOLS"
	cp "${FIT_SOURCE}" "${FIT_WORK}"
	(
		cd "${BINARIES_DIR}"
		"${MKIMAGE}" -q -f "$(basename "${FIT_WORK}")" "$(basename "${FIT_IMAGE}")"
	)
	check_size "FIT kernel" "${FIT_IMAGE}" "${KERNEL_MAX}"
	if [ -x "${DUMPIMAGE}" ]; then
		"${DUMPIMAGE}" -l "${FIT_IMAGE}"
	fi
fi
rm -f "${OLD_APPENDED_IMAGE}"

if [ -f "${FLASH_SCRIPT}" ]; then
	install -m 0755 "${FLASH_SCRIPT}" "${BINARIES_DIR}/epass-flash.sh"
fi
