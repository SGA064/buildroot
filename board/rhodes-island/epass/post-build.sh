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
# utilities. bluetoothd and bluetoothctl only require libglib and libdbus.
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
