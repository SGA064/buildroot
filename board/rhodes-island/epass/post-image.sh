#!/bin/sh

set -eu

BOARD_DIR="$(dirname "$0")"
KERNEL_IMAGE="${BINARIES_DIR}/zImage"
LINUX_DTB="${BINARIES_DIR}/suniv-f1c200s-epass.dtb"
OLD_APPENDED_IMAGE="${BINARIES_DIR}/zImage.suniv-f1c200s-epass"
FIT_SOURCE="${BOARD_DIR}/epass.its"
FIT_WORK="${BINARIES_DIR}/epass.its"
FIT_IMAGE="${BINARIES_DIR}/fitImage.itb"
FLASH_TOOL="${BOARD_DIR}/epass-flash.py"
UBINIZE_SOURCE="${BOARD_DIR}/ubinize.cfg"
UBINIZE_WORK="${BUILD_DIR}/epass-ubinize.cfg"
UBIFS_IMAGE="${BINARIES_DIR}/rootfs.ubifs"
UBI_IMAGE="${BINARIES_DIR}/rootfs.ubi"
DATA_DIR="${BUILD_DIR}/epass-data-root"
DATA_UBIFS_IMAGE="${BUILD_DIR}/epass-data.ubifs"
MKIMAGE="${HOST_DIR}/bin/mkimage"
DUMPIMAGE="${HOST_DIR}/bin/dumpimage"
UBINIZE="${HOST_DIR}/sbin/ubinize"
MKFS_UBIFS="${HOST_DIR}/sbin/mkfs.ubifs"
# KERNEL_MAX and ROOTFS_MAX mirror KERNEL_MAX_SIZE and ROOTFS_MAX_SIZE in
# package/epass-otactl/otactl.c, which is what the running system enforces via
# `epass-otactl status` (and what epass-ota-install reads back).  These two are
# the build-time pre-flight check of the same limits; keep them in sync.
KERNEL_MAX=$((0x800000))
ROOTFS_MAX=$((20 * 1024 * 1024))
UBI_MAX=$((0x7620000))
DATA_MAX_LEB_COUNT=753

die()
{
	echo "error: $*" >&2
	exit 1
}

file_size()
{
	# With stdin input wc prints the bare count: no padding and no file name
	# to strip, so this needs the same treatment as epass-ota-install's
	# get_file_size().
	wc -c < "$1"
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

[ -f "${FIT_IMAGE}" ] || die "FIT image was not generated: ${FIT_IMAGE}"
[ -f "${UBIFS_IMAGE}" ] || die "UBIFS image not found: ${UBIFS_IMAGE}"
check_size "rootfs UBIFS" "${UBIFS_IMAGE}" "${ROOTFS_MAX}"
[ -x "${UBINIZE}" ] || die "ubinize not found: enable host-mtd"
[ -x "${MKFS_UBIFS}" ] || die "mkfs.ubifs not found: enable host-mtd"
rm -rf "${DATA_DIR}"
rm -f "${DATA_UBIFS_IMAGE}"
mkdir -p "${DATA_DIR}"
"${MKFS_UBIFS}" -d "${DATA_DIR}" -e 0x1f000 \
	-c "${DATA_MAX_LEB_COUNT}" -m 0x800 -x lzo \
	-o "${DATA_UBIFS_IMAGE}"
sed \
	-e "s;BR2_ROOTFS_UBIFS_PATH;${UBIFS_IMAGE};g" \
	-e "s;DATA_UBIFS_PATH;${DATA_UBIFS_IMAGE};g" \
	"${UBINIZE_SOURCE}" > "${UBINIZE_WORK}"
rm -f "${UBI_IMAGE}" \
	"${BINARIES_DIR}/data.ubifs" "${BINARIES_DIR}/epass.ubi" \
	"${BINARIES_DIR}/system.ubi" "${BINARIES_DIR}/data.ubi"
"${UBINIZE}" -o "${UBI_IMAGE}" -m 0x800 -p 0x20000 \
	"${UBINIZE_WORK}"
rm -f "${UBINIZE_WORK}"
rm -rf "${DATA_DIR}"
rm -f "${DATA_UBIFS_IMAGE}"
check_size "ePass UBI" "${UBI_IMAGE}" "${UBI_MAX}"

# Drop flasher file names from earlier layouts, then install the tool.
rm -f "${BINARIES_DIR}/epass-flash.sh" "${BINARIES_DIR}/epass-flash-gui.py"
if [ -f "${FLASH_TOOL}" ]; then
	install -m 0755 "${FLASH_TOOL}" "${BINARIES_DIR}/epass-flash.py"
fi
