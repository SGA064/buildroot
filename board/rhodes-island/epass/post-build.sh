#!/bin/sh

set -eu

target_dir=${1:-}

case "${target_dir}" in
	/*) ;;
	*)
		echo "epass post-build: invalid target directory: ${target_dir}" >&2
		exit 1
		;;
esac

if [ ! -f "${target_dir}/THIS_IS_NOT_YOUR_ROOT_FILESYSTEM" ]; then
	echo "epass post-build: refusing to prune unmarked directory: ${target_dir}" >&2
	exit 1
fi

libdir="${target_dir}/usr/lib"

# Board policy: these scripts are installed by enabled packages or replaced
# by the late-init coordinator, including on a clean build.
rm -f -- \
	"${target_dir}/etc/init.d/S01seedrng" \
	"${target_dir}/etc/init.d/S02sysctl" \
	"${target_dir}/etc/init.d/S50dropbear"

# Migration only: remove obsolete overlay entry points from incremental trees.
rm -f -- \
	"${target_dir}/etc/init.d/S11modules" \
	"${target_dir}/etc/init.d/S12data" \
	"${target_dir}/etc/init.d/S13ota-cleanup" \
	"${target_dir}/etc/init.d/S14seedrng" \
	"${target_dir}/etc/init.d/S15umtprd" \
	"${target_dir}/etc/init.d/S45umtprd"

# Disabled packages/applets can still exist in an incremental target tree.
rm -f -- \
	"${target_dir}/etc/init.d/S50crond" \
	"${target_dir}/usr/bin/crontab" \
	"${target_dir}/usr/sbin/crond" \
	"${target_dir}/usr/sbin/ubiattach" \
	"${target_dir}/usr/sbin/ubidetach"
rm -rf -- "${target_dir}/etc/cron"

# Bluetooth, SLE, and D-Bus are disabled. Prune files left by incremental builds.
rm -f -- \
	"${target_dir}/etc/init.d/S30dbus-daemon" \
	"${target_dir}/etc/init.d/S40bluetoothd" \
	"${target_dir}/usr/bin/bluetoothctl" \
	"${target_dir}/usr/bin/dbus-cleanup-sockets" \
	"${target_dir}/usr/bin/dbus-daemon" \
	"${target_dir}/usr/bin/dbus-launch" \
	"${target_dir}/usr/bin/dbus-monitor" \
	"${target_dir}/usr/bin/dbus-run-session" \
	"${target_dir}/usr/bin/dbus-send" \
	"${target_dir}/usr/bin/dbus-test-tool" \
	"${target_dir}/usr/bin/dbus-update-activation-environment" \
	"${target_dir}/usr/bin/dbus-uuidgen" \
	"${target_dir}/usr/libexec/dbus-daemon-launch-helper" \
	"${libdir}/libbluetooth.so"* \
	"${libdir}/libdbus-1.so"*
rm -rf -- \
	"${target_dir}/etc/bluetooth" \
	"${target_dir}/etc/dbus-1" \
	"${target_dir}/run/dbus" \
	"${target_dir}/usr/libexec/bluetooth" \
	"${target_dir}/usr/share/dbus-1" \
	"${target_dir}/usr/share/xml/dbus-1" \
	"${target_dir}/var/lib/bluetooth" \
	"${target_dir}/var/lib/dbus"

# The WS73 driver is cfg80211/nl80211 only, and wireless_tools is no longer
# selected. Prune its binaries from incremental target trees.
rm -f -- \
	"${target_dir}/sbin/ifrename" \
	"${target_dir}/sbin/iwconfig" \
	"${target_dir}/sbin/iwgetid" \
	"${target_dir}/sbin/iwlist" \
	"${target_dir}/sbin/iwspy"

# Buildroot groups modetest with the other libdrm test programs. Keep the KMS
# diagnostic used on this board and discard the unrelated test binaries.
for program in drmdevice modeprint proptest vbltest; do
	rm -f -- "${target_dir}/usr/bin/${program}"
done

player="${target_dir}/usr/bin/fplayerdemo"
if [ -f "${player}" ] &&
   LC_ALL=C grep -a -q 'libgst[a-zA-Z0-9_-]*-1\.0\.so' "${player}"; then
	echo "epass post-build: rebuild ${player##*/} before pruning GStreamer" >&2
	exit 1
fi

# The current board image has no GStreamer consumer. Remove plugins and core
# libraries as well, so an incremental Buildroot tree produces the same target
# contents as a clean build.
rm -rf -- "${libdir}/gstreamer-1.0"
for library in \
	libgstadaptivedemux-1.0 \
	libgstallocators-1.0 \
	libgstanalytics-1.0 \
	libgstapp-1.0 \
	libgstaudio-1.0 \
	libgstbadaudio-1.0 \
	libgstbase-1.0 \
	libgstbasecamerabinsrc-1.0 \
	libgstcodecparsers-1.0 \
	libgstcodecs-1.0 \
	libgstcontroller-1.0 \
	libgstcuda-1.0 \
	libgstfft-1.0 \
	libgstinsertbin-1.0 \
	libgstisoff-1.0 \
	libgstmpegts-1.0 \
	libgstmse-1.0 \
	libgstnet-1.0 \
	libgstpbutils-1.0 \
	libgstphotography-1.0 \
	libgstplay-1.0 \
	libgstplayer-1.0 \
	libgstriff-1.0 \
	libgstrtp-1.0 \
	libgstrtsp-1.0 \
	libgstsctp-1.0 \
	libgstsdp-1.0 \
	libgsttag-1.0 \
	libgsttranscoder-1.0 \
	libgsturidownloader-1.0 \
	libgstvideo-1.0 \
	libgstwebrtc-1.0 \
	libgstreamer-1.0
do
	rm -f -- "${libdir}/${library}.so"*
done

# No target program uses GIO, GObject Introspection, or the GLib command-line
# utilities.
for library in libgio-2.0 libgirepository-2.0; do
	rm -f -- "${libdir}/${library}.so"*
done
for program in \
	gapplication \
	gdbus \
	gst-transcoder-1.0 \
	gi-compile-repository \
	gi-decompile-typelib \
	gi-inspect-typelib \
	gio \
	gio-querymodules \
	gresource \
	gsettings \
	pcre2grep \
	pcre2test
do
	rm -f -- "${target_dir}/usr/bin/${program}"
done

# C++ is disabled in the board defconfig. Remove stale libraries left by an
# incremental build; the GPIO tools use the C binding.
rm -f -- "${libdir}/libgpiodcxx.so"* "${libdir}/libstdc++.so"*

# fplayerdemo does not link libavfilter.
rm -f -- "${libdir}/libavfilter.so"*

authorized_keys="${target_dir}/root/.ssh/authorized_keys"
if [ -f "${authorized_keys}" ]; then
	chmod 0700 -- "${target_dir}/root/.ssh"
	chmod 0600 -- "${authorized_keys}"
fi
