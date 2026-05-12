#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Boot the ePass U-Boot image through Allwinner FEL when needed, then flash
# the SPI-NAND bootloader and partitions through USB DFU.

set -eu

default_boot_image="u-boot-sunxi-with-spl.bin"

bootloader_max=$((0x100000))
uboot_max=$((0x0e0000))
env_max=$((0x020000))
kernel_max=$((0x800000))
ubi_max=$((0x76e0000))

dfu_dev="${DFU_DEV:-1f3a:1010}"
dfu_wait="${DFU_WAIT:-30}"
dfu_retries="${DFU_RETRIES:-2}"
verify_mode="${DFU_VERIFY:-bootloader}"
fel_tool="${FEL_TOOL:-sunxi-fel}"
dfu_util="${DFU_UTIL:-dfu-util}"

boot_image=
bootloader_image=
bootloader_uboot_image=
bootloader_uboot_temp=
env_image=
kernel_image=
ubi_image=
dfu_list_file=
dfu_verify_file=
fel_boot_image=
fel_spl_image=
fel_uboot_image=
fel_uboot_addr=
fel_boot_method=
fel_temp_image=
split_boot_requested=0
list_only=0
reset_after=1
skip_fel=0

usage()
{
	cat <<EOF
Usage:
  epass-flash.sh [options] -k zImage -u rootfs.ubi
  epass-flash.sh [options] -B u-boot-sunxi-with-spl.bin -k zImage -u rootfs.ubi

Options:
  -b FILE     Combined U-Boot image used only to boot temporary DFU over FEL.
              When -b is omitted, FEL boot uses -B FILE if supplied;
              otherwise it uses ${default_boot_image}.
  -B FILE     Flash complete bootloader from FILE, max 0x100000 bytes.
              The device DFU alt bootloader writes SPL in the BootROM
              SPI-NAND split-page layout and writes U-Boot proper.
  -e FILE     Flash FILE to the env partition, max 0x20000 bytes
  -k FILE     Flash FILE to the kernel partition, max 0x800000 bytes
  -u FILE     Flash FILE to the ubi partition, max 0x76e0000 bytes
  -d VID:PID  dfu-util USB device selector, default: ${dfu_dev}
  -t SEC      DFU wait timeout in seconds, default: ${dfu_wait}
  -r N        Retry each dfu-util transfer up to N times, default: ${dfu_retries}
  -s FILE     SPL image for explicit sunxi-fel spl/write/exe boot
  -p FILE     Raw U-Boot proper image for explicit sunxi-fel spl/write/exe boot
  -a ADDR     Raw U-Boot proper load/entry address,
              default: CONFIG_TEXT_BASE from .config, or 0x81700000
  -F          Skip FEL boot and use an already-running DFU device
  -l          Boot U-Boot over FEL unless -F is used, list DFU alt settings,
              then exit
  -V          Verify flashed raw alts by DFU upload: u-boot/env/kernel.
              UBI is verified only when DFU_VERIFY=all is set explicitly.
  -N          Disable DFU upload verification
  -n          Do not request USB DFU reset after the last transfer
  -h          Show this help

Environment:
  FEL_TOOL    FEL loader command, default: sunxi-fel
  DFU_UTIL    DFU host command, default: dfu-util
  DFU_DEV     USB VID:PID selector, default: 1f3a:1010
  DFU_WAIT    DFU wait timeout in seconds, default: 30
  DFU_RETRIES dfu-util transfer attempts, default: 2
  DFU_VERIFY  Verification mode: none, bootloader, raw, all.
              default: bootloader

Examples:
  epass-flash.sh -l
  epass-flash.sh -B u-boot-sunxi-with-spl.bin
  epass-flash.sh -k zImage -u rootfs.ubi
  epass-flash.sh -B u-boot-sunxi-with-spl.bin -k zImage -u rootfs.ubi
  epass-flash.sh -F -V -k zImage
EOF
}

die()
{
	echo "error: $*" >&2
	exit 1
}

cleanup()
{
	[ -z "$dfu_list_file" ] || rm -f "$dfu_list_file"
	[ -z "$dfu_verify_file" ] || rm -f "$dfu_verify_file"
	[ -z "$fel_temp_image" ] || rm -f "$fel_temp_image"
	[ -z "$bootloader_uboot_temp" ] || rm -f "$bootloader_uboot_temp"
}

info()
{
	echo "==> $*"
}

file_size()
{
	wc -c < "$1" | tr -d ' '
}

hex_at()
{
	dd if="$1" bs=1 skip="$2" count="$3" 2>/dev/null |
		od -An -tx1 | tr -d ' \n'
}

le32_at()
{
	set -- $(dd if="$1" bs=1 skip="$2" count=4 2>/dev/null |
		od -An -tu1)

	[ "$#" -eq 4 ] || return 1
	echo $(($1 + ($2 * 256) + ($3 * 65536) + ($4 * 16777216)))
}

check_file()
{
	image="$1"

	[ -n "$image" ] || return 0
	[ -f "$image" ] || die "image not found: $image"
	[ -r "$image" ] || die "image is not readable: $image"
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

check_uint()
{
	name="$1"
	value="$2"

	case "$value" in
	''|*[!0-9]*)
		die "$name must be a decimal integer: $value"
		;;
	esac
}

check_verify_mode()
{
	case "$verify_mode" in
	none|bootloader|raw|all)
		;;
	*)
		die "DFU_VERIFY must be one of: none, bootloader, raw, all"
		;;
	esac
}

prepare_fel_boot_image()
{
	image="$1"
	image_size="$(file_size "$image")"
	spl_len="$(le32_at "$image" 16)" ||
		die "cannot read eGON SPL length from $image"
	fel_uboot_offset="$spl_len"

	if [ "$(hex_at "$image" 4 8)" != "65474f4e2e425430" ]; then
		die "FEL boot image is not an Allwinner eGON image: $image"
	fi

	if [ "$spl_len" -le 0 ] || [ "$spl_len" -gt "$image_size" ]; then
		die "invalid SPL length $spl_len in $image"
	fi

	# sunxi-fel uboot loads U-Boot from max(SPL length, 0x8000).
	if [ "$fel_uboot_offset" -lt 32768 ]; then
		fel_uboot_offset=32768
	fi

	if [ "$fel_uboot_offset" -lt "$image_size" ] &&
	   [ "$(hex_at "$image" "$fel_uboot_offset" 4)" = "27051956" ]; then
		fel_boot_image="$image"
		return
	fi

	u_boot_offset=
	for offset in 131072 "$spl_len" 32768; do
		if [ "$offset" -lt "$image_size" ] &&
		   [ "$(hex_at "$image" "$offset" 4)" = "27051956" ]; then
			u_boot_offset="$offset"
			break
		fi
	done

	if [ -z "$u_boot_offset" ]; then
		die "cannot find U-Boot legacy image inside $image"
	fi

	fel_temp_image="${TMPDIR:-/tmp}/epass-fel-uboot.$$"
	{
		dd if="$image" bs=1 count="$spl_len" 2>/dev/null
		if [ "$fel_uboot_offset" -gt "$spl_len" ]; then
			dd if=/dev/zero bs=1 count=$((fel_uboot_offset - spl_len)) 2>/dev/null
		fi
		dd if="$image" bs=1 skip="$u_boot_offset" 2>/dev/null
	} > "$fel_temp_image" ||
		die "failed to create temporary FEL image"

	fel_boot_image="$fel_temp_image"
	info "Created temporary FEL image from $image: SPL length $(printf '0x%x' "$spl_len"), U-Boot source offset $(printf '0x%x' "$u_boot_offset"), FEL U-Boot offset $(printf '0x%x' "$fel_uboot_offset")"
}

set_default_split_boot_files()
{
	boot_dir="$(dirname "$boot_image")"

	if [ -z "$fel_spl_image" ] && [ -f "$boot_dir/spl/sunxi-spl.bin" ]; then
		fel_spl_image="$boot_dir/spl/sunxi-spl.bin"
	fi

	if [ -z "$fel_uboot_image" ] && [ -f "$boot_dir/u-boot-dtb.bin" ]; then
		fel_uboot_image="$boot_dir/u-boot-dtb.bin"
	fi

	if [ -z "$fel_uboot_addr" ]; then
		if [ -f "$boot_dir/.config" ]; then
			fel_uboot_addr="$(sed -n 's/^CONFIG_TEXT_BASE=//p' "$boot_dir/.config" | sed -n '1p')"
		fi
		: "${fel_uboot_addr:=0x81700000}"
	fi
}

prepare_fel_boot()
{
	if [ "$split_boot_requested" -eq 0 ]; then
		prepare_fel_boot_image "$boot_image"
		fel_boot_method=uboot
		return
	fi

	set_default_split_boot_files
	if [ -n "$fel_spl_image" ] && [ -n "$fel_uboot_image" ]; then
		check_file "$fel_spl_image"
		check_file "$fel_uboot_image"
		fel_boot_method=split
		return
	fi

	die "explicit sunxi-fel spl boot needs both SPL (-s) and raw U-Boot proper (-p) images"
}

prepare_bootloader_uboot_image()
{
	src_image="$1"
	image_size="$(file_size "$src_image")"
	spl_len="$(le32_at "$src_image" 16)" ||
		die "cannot read eGON SPL length from $src_image"
	u_boot_offset=

	if [ "$(hex_at "$src_image" 4 8)" != "65474f4e2e425430" ]; then
		die "bootloader image is not an Allwinner eGON image: $src_image"
	fi

	for offset in 131072 "$spl_len" 32768; do
		if [ "$offset" -lt "$image_size" ] &&
		   [ "$(hex_at "$src_image" "$offset" 4)" = "27051956" ]; then
			u_boot_offset="$offset"
			break
		fi
	done

	if [ -z "$u_boot_offset" ]; then
		die "cannot find U-Boot legacy image inside $src_image"
	fi

	bootloader_uboot_temp="${TMPDIR:-/tmp}/epass-uboot-proper.$$"
	if [ $((u_boot_offset % 512)) -eq 0 ]; then
		dd if="$src_image" of="$bootloader_uboot_temp" bs=512 \
			skip=$((u_boot_offset / 512)) 2>/dev/null ||
			die "failed to extract U-Boot proper from $src_image"
	else
		dd if="$src_image" of="$bootloader_uboot_temp" bs=1 \
			skip="$u_boot_offset" 2>/dev/null ||
			die "failed to extract U-Boot proper from $src_image"
	fi

	bootloader_uboot_image="$bootloader_uboot_temp"
	check_size "u-boot proper" "$bootloader_uboot_image" "$uboot_max"
	info "Extracted U-Boot proper for NAND offset 0x20000 from $src_image offset $(printf '0x%x' "$u_boot_offset")"
}

trap cleanup EXIT HUP INT TERM

while getopts "b:B:e:k:u:d:t:r:s:p:a:FVNlnh" opt; do
	case "$opt" in
	b)
		boot_image="$OPTARG"
		;;
	B)
		bootloader_image="$OPTARG"
		;;
	e)
		env_image="$OPTARG"
		;;
	k)
		kernel_image="$OPTARG"
		;;
	u)
		ubi_image="$OPTARG"
		;;
	d)
		dfu_dev="$OPTARG"
		;;
	t)
		dfu_wait="$OPTARG"
		;;
	r)
		dfu_retries="$OPTARG"
		;;
	s)
		fel_spl_image="$OPTARG"
		split_boot_requested=1
		;;
	p)
		fel_uboot_image="$OPTARG"
		split_boot_requested=1
		;;
	a)
		fel_uboot_addr="$OPTARG"
		split_boot_requested=1
		;;
	F)
		skip_fel=1
		;;
	V)
		verify_mode=raw
		;;
	N)
		verify_mode=none
		;;
	l)
		list_only=1
		;;
	n)
		reset_after=0
		;;
	h)
		usage
		exit 0
		;;
	*)
		usage >&2
		exit 2
		;;
	esac
done

if [ "$skip_fel" -eq 0 ] && [ -z "$boot_image" ]; then
	if [ -n "$bootloader_image" ]; then
		boot_image="$bootloader_image"
	elif [ -f "$default_boot_image" ]; then
		boot_image="$default_boot_image"
	else
		usage >&2
		exit 2
	fi
fi

check_uint "DFU wait timeout" "$dfu_wait"
check_uint "DFU retry count" "$dfu_retries"
[ "$dfu_retries" -gt 0 ] || die "DFU retry count must be greater than zero"
check_verify_mode
[ "$skip_fel" -eq 1 ] || check_file "$boot_image"
check_file "$bootloader_image"
check_file "$env_image"
check_file "$kernel_image"
check_file "$ubi_image"

[ -n "$bootloader_image" ] && check_size "bootloader" "$bootloader_image" "$bootloader_max"
[ -n "$env_image" ] && check_size "env" "$env_image" "$env_max"
[ -n "$kernel_image" ] && check_size "kernel" "$kernel_image" "$kernel_max"
[ -n "$ubi_image" ] && check_size "ubi" "$ubi_image" "$ubi_max"

if [ "$list_only" -eq 0 ] &&
   [ -z "$bootloader_image" ] &&
   [ -z "$env_image" ] &&
   [ -z "$kernel_image" ] &&
   [ -z "$ubi_image" ]; then
	usage >&2
	exit 2
fi

if [ "$skip_fel" -eq 0 ] && ! command -v "$fel_tool" >/dev/null 2>&1; then
	die "FEL tool not found: $fel_tool"
fi

if ! command -v "$dfu_util" >/dev/null 2>&1; then
	die "dfu-util not found: $dfu_util"
fi

[ "$skip_fel" -eq 1 ] || prepare_fel_boot
[ -n "$bootloader_image" ] && prepare_bootloader_uboot_image "$bootloader_image"

dfu_list_file="${TMPDIR:-/tmp}/epass-dfu-list.$$"

dfu_list()
{
	"$dfu_util" -d "$dfu_dev" -l
}

dfu_device_found()
{
	grep "Found DFU:" "$dfu_list_file" >/dev/null 2>&1
}

require_alt()
{
	alt="$1"

	if ! grep "name=\"$alt\"" "$dfu_list_file" >/dev/null 2>&1; then
		die "DFU alt '$alt' is not exposed by the device"
	fi
}

wait_for_dfu()
{
	i=0

	info "Waiting for USB DFU device $dfu_dev..."
	while [ "$i" -lt "$dfu_wait" ]; do
		dfu_list > "$dfu_list_file" 2>&1 || true
		if dfu_device_found; then
			return 0
		fi
		i=$((i + 1))
		sleep 1
	done

	[ ! -f "$dfu_list_file" ] || cat "$dfu_list_file" >&2 || true
	die "timed out waiting for DFU device $dfu_dev after ${dfu_wait}s"
}

run_dfu_util()
{
	desc="$1"
	shift
	attempt=1

	while :; do
		if "$@"; then
			return 0
		fi

		if [ "$attempt" -ge "$dfu_retries" ]; then
			die "$desc failed after $dfu_retries attempt(s)"
		fi

		info "$desc failed (attempt $attempt/$dfu_retries), retrying..."
		attempt=$((attempt + 1))
		sleep 1
		wait_for_dfu
	done
}

boot_fel()
{
	fel_tool_name="${fel_tool##*/}"
	fel_tool_name="${fel_tool_name##*\\}"

	case "$fel_tool_name" in
	sunxi-fel|sunxi-fel.exe)
		if [ "$fel_boot_method" = split ]; then
			"$fel_tool" -p \
				spl "$fel_spl_image" \
				write "$fel_uboot_addr" "$fel_uboot_image" \
				exe "$fel_uboot_addr"
		else
			"$fel_tool" -p uboot "$fel_boot_image"
		fi
		;;
	*)
		die "unsupported FEL tool for temporary U-Boot boot: $fel_tool"
		;;
	esac
}

dfu_flash()
{
	alt="$1"
	image="$2"
	size="$(file_size "$image")"

	info "Flashing $alt from $image ($size bytes)"
	run_dfu_util "Flashing $alt" \
		"$dfu_util" -d "$dfu_dev" -a "$alt" -D "$image"
}

should_verify_alt()
{
	alt="$1"

	case "$verify_mode" in
	none)
		return 1
		;;
	bootloader)
		[ "$alt" = "u-boot" ] && [ -n "$bootloader_image" ]
		;;
	raw)
		[ "$alt" = "u-boot" ] || [ "$alt" = "env" ] ||
			[ "$alt" = "kernel" ]
		;;
	all)
		return 0
		;;
	esac
}

dfu_verify_prefix()
{
	alt="$1"
	image="$2"
	compare_size="$(file_size "$image")"
	upload_size="${3:-$compare_size}"
	dfu_verify_file="${TMPDIR:-/tmp}/epass-dfu-verify.$$"
	attempt=1

	info "Verifying $alt by DFU upload ($upload_size bytes, compare first $compare_size)"
	while :; do
		rm -f "$dfu_verify_file"
		if "$dfu_util" -d "$dfu_dev" -a "$alt" -Z "$upload_size" -U "$dfu_verify_file"; then
			break
		fi

		if [ "$attempt" -ge "$dfu_retries" ]; then
			die "$alt verification upload failed"
		fi

		info "Verifying $alt failed (attempt $attempt/$dfu_retries), retrying..."
		attempt=$((attempt + 1))
		sleep 1
		wait_for_dfu
	done

	if cmp -n "$compare_size" "$image" "$dfu_verify_file" >/dev/null 2>&1; then
		info "Verified $alt first $compare_size bytes"
	else
		die "$alt verification failed"
	fi
}

dfu_detach()
{
	info "Leaving DFU mode"
	run_dfu_util "Leaving DFU mode" "$dfu_util" -d "$dfu_dev" -a 0 -e
}

print_plan()
{
	info "Flash plan"
	if [ "$skip_fel" -eq 1 ]; then
		echo "    boot: use existing USB DFU device $dfu_dev"
	elif [ "$fel_boot_method" = split ]; then
		echo "    boot: FEL SPL $fel_spl_image + U-Boot $fel_uboot_image @ $fel_uboot_addr"
	else
		echo "    boot: FEL image $fel_boot_image"
	fi

	if [ "$list_only" -eq 1 ]; then
		echo "    action: list DFU alt settings only"
	else
		[ -n "$bootloader_image" ] &&
			echo "    bootloader: DFU alt bootloader <- $bootloader_image"
		[ -n "$env_image" ] &&
			echo "    env: DFU alt env <- $env_image"
		[ -n "$kernel_image" ] &&
			echo "    kernel: DFU alt kernel <- $kernel_image"
		[ -n "$ubi_image" ] &&
			echo "    ubi: DFU alt ubi <- $ubi_image"
		echo "    verify: $verify_mode"
		echo "    dfu retries: $dfu_retries"
	fi
}

print_plan

if [ "$skip_fel" -eq 1 ]; then
	info "Using already-running DFU device $dfu_dev"
elif [ "$fel_boot_method" = split ]; then
	info "Booting temporary ePass U-Boot over FEL with SPL $fel_spl_image and U-Boot $fel_uboot_image at $fel_uboot_addr"
	boot_fel
else
	info "Booting temporary ePass U-Boot over FEL from $fel_boot_image"
	boot_fel
fi
wait_for_dfu

info "Available DFU alt settings"
cat "$dfu_list_file"

[ -n "$bootloader_image" ] && require_alt bootloader
[ -n "$bootloader_image" ] && should_verify_alt u-boot && require_alt u-boot
[ -n "$env_image" ] && require_alt env
[ -n "$kernel_image" ] && require_alt kernel
[ -n "$ubi_image" ] && require_alt ubi

if [ "$list_only" -eq 1 ]; then
	info "DFU list completed."
	exit 0
fi

if [ -n "$bootloader_image" ]; then
	dfu_flash bootloader "$bootloader_image"
	should_verify_alt u-boot && dfu_verify_prefix u-boot "$bootloader_uboot_image" "$uboot_max"
fi
if [ -n "$env_image" ]; then
	dfu_flash env "$env_image"
	should_verify_alt env && dfu_verify_prefix env "$env_image" "$env_max"
fi
if [ -n "$kernel_image" ]; then
	dfu_flash kernel "$kernel_image"
	should_verify_alt kernel && dfu_verify_prefix kernel "$kernel_image" "$kernel_max"
fi
if [ -n "$ubi_image" ]; then
	dfu_flash ubi "$ubi_image"
	should_verify_alt ubi && dfu_verify_prefix ubi "$ubi_image" "$ubi_max"
fi

[ "$reset_after" -eq 1 ] && dfu_detach

info "ePass FEL DFU flashing completed."
