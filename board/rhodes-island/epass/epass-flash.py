#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""ePass FEL/DFU flasher.

Boot the ePass U-Boot image through Allwinner FEL when needed, then flash
the SPI-NAND bootloader and partitions through USB DFU.

Started without arguments this is a graphical tool. With any command line
option it behaves like the retired epass-flash.sh CLI and accepts the same
option letters, so existing commands keep working after renaming the
executable.
"""

import argparse
import glob
import os
import queue
import re
import selectors
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    from tkinter import font as tkfont
except ImportError:  # CLI-only hosts do not need python3-tk.
    tk = None

# FlashApp must not inherit from tk.Tk when tkinter is missing: evaluating the
# base class at import time would abort the whole tool, including --check and
# the CLI, on hosts without python3-tk. gui_main() refuses to instantiate it.
_TkBase = tk.Tk if tk is not None else object

DFU_VIDPID = "1f3a:1010"
# Allwinner BROM FEL mode.  This is the only VID:PID sunxi-fel matches
# (fel_lib.c), and what the F1C200s BROM reports ("sunxi SoC OTG connector in
# FEL/flashing mode").
FEL_VIDPID = "1f3a:efe8"
DFU_WAIT = 30
DFU_RETRIES = 2
TOOL_TIMEOUT = 600
VERIFY_MODES = ("none", "bootloader", "raw", "all")
VERIFY_DEFAULT = "bootloader"
FEL_TOOL_DEFAULT = "sunxi-fel"
DFU_UTIL_DEFAULT = "dfu-util"
DEFAULT_FEL_ADDR = 0x81700000

BOOTLOADER_MAX = 0x100000
BOOTLOGO_MAX = 0x080000
ENV_MAX = 0x020000
UBI_MAX = 0x7620000
KERNEL_MAX = 0x800000
ROOTFS_MAX = 0x1400000
UBOOT_OFFSET = 0x20000
NAND_PAGE_SIZE = 0x800
NAND_ERASE_SIZE = 0x20000

# DFU alt -> (option letter, size limit)
ALT_LIMITS = {
    "bootloader": BOOTLOADER_MAX,
    "bootlogo": BOOTLOGO_MAX,
    "env": ENV_MAX,
    "ubi": UBI_MAX,
    "kernel": KERNEL_MAX,
    "rootfs": ROOTFS_MAX,
}

# Flash order used by the device-side plan, for overall progress accounting.
FLASH_ORDER = ["bootloader", "bootlogo", "env", "ubi", "kernel", "rootfs"]

UBOOT_MAGIC = bytes.fromhex("27051956")

# Printed when the FEL boot command fails and no DFU device shows up.
FEL_SPL_HINT = (
    "hint: sunxi-fel runs with -v, check its output above.  If it stopped "
    "after \"Executing the SPL\" without a matching eGON.FEL handshake, the "
    "BootROM FEL handshake failed.  A warm FEL entry (U-Boot \"fel\" command, "
    "or the FEL key after a normal boot that displayed the bootlogo) leaves "
    "the display engines fetching from DRAM, which can hang the FEL-loaded "
    "SPL.  Enter FEL cold (power-cycle into the BootROM FEL), or start DFU "
    "from U-Boot (\"dfu 0\") and flash with -F."
)


class FlashError(Exception):
    pass


class _Retryable(Exception):
    """Internal marker for failed attempts that may be retried."""


def human_size(size):
    if size < 1024:
        return "%d B" % size
    for unit in ("KiB", "MiB", "GiB"):
        size /= 1024.0
        if size < 1024:
            return "%.1f %s" % (size, unit)
    return "%.1f TiB" % (size / 1024.0)


# ---- image validation ------------------------------------------------------

def read_file(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise FlashError("cannot read %s: %s" % (path, e))


def _field(data, offset, fmt, what, path):
    try:
        return struct.unpack_from(fmt, data, offset)[0]
    except struct.error:
        raise FlashError("cannot read %s: %s" % (what, path))


def check_bmp(path):
    """Validate a BMP bootlogo, mirroring the checks of epass-flash.sh."""
    data = read_file(path)
    if len(data) < 2 or data[:2] != b"BM":
        raise FlashError("bootlogo is not a BMP file: %s" % path)
    declared_size = _field(data, 2, "<I", "BMP file size", path)
    data_offset = _field(data, 10, "<I", "BMP data offset", path)
    dib_size = _field(data, 14, "<I", "BMP DIB header size", path)
    width = _field(data, 18, "<I", "BMP width", path)
    height = _field(data, 22, "<I", "BMP height", path)
    planes = _field(data, 26, "<H", "BMP plane count", path)
    bpp = _field(data, 28, "<H", "BMP bit depth", path)
    compression = _field(data, 30, "<I", "BMP compression", path)

    if declared_size != len(data):
        raise FlashError("BMP header size %d does not match file size %d: %s"
                         % (declared_size, len(data), path))
    if dib_size < 40:
        raise FlashError("unsupported BMP DIB header size %d: %s"
                         % (dib_size, path))
    if data_offset < 14 + dib_size or data_offset >= len(data):
        raise FlashError("invalid BMP data offset %d: %s" % (data_offset, path))
    if planes != 1:
        raise FlashError("BMP must contain exactly one plane: %s" % path)
    if not 0 < width <= 32768 or not 0 < height <= 32768:
        raise FlashError("unsupported BMP dimensions %dx%d: %s"
                         % (width, height, path))

    if "%d:%d" % (bpp, compression) not in (
            "1:0", "8:0", "16:0", "16:3", "24:0", "32:0", "32:3"):
        raise FlashError("unsupported BMP format: %d bpp, compression %d: %s"
                         % (bpp, compression, path))

    if compression == 3:
        if data_offset < 66:
            raise FlashError("BMP bitfields header is too short: %s" % path)
        red_mask = _field(data, 54, "<I", "BMP red mask", path)
        green_mask = _field(data, 58, "<I", "BMP green mask", path)
        blue_mask = _field(data, 62, "<I", "BMP blue mask", path)
        if (bpp, red_mask, green_mask, blue_mask) not in (
                (16, 63488, 2016, 31), (32, 16711680, 65280, 255)):
            raise FlashError("unsupported BMP channel masks: %s" % path)

    row_size = ((width * bpp + 31) // 32) * 4
    if row_size * height > len(data) - data_offset:
        raise FlashError("BMP pixel data is truncated: %s" % path)
    return "Validated bootlogo BMP: %dx%d, %d bpp, compression %d" % (
        width, height, bpp, compression)


def check_fit(path):
    data = read_file(path)
    if data[:4] != b"\xd0\x0d\xfe\xed":
        raise FlashError("kernel image is not a FIT image: %s" % path)
    declared_size = _field(data, 4, ">I", "FIT total size", path)
    if declared_size < 40 or declared_size > len(data):
        raise FlashError("invalid FIT total size %d for %d-byte file: %s"
                         % (declared_size, len(data), path))


def check_ubifs(path):
    data = read_file(path)
    if data[:4] != b"\x31\x18\x10\x06":
        raise FlashError("rootfs image is not a UBIFS image: %s" % path)


def check_bootloader(path):
    """Validate the NAND layout independently of the temporary FEL image."""
    data = read_file(path)
    if data[4:12] != b"eGON.BT0":
        raise FlashError("bootloader is not an Allwinner eGON image: %s" % path)
    spl_len = _field(data, 16, "<I", "SPL length", path)
    if not 32 <= spl_len <= min(len(data), UBOOT_OFFSET) or spl_len % 4:
        raise FlashError("invalid SPL length: %s" % path)
    expected = _field(data, 12, "<I", "SPL checksum", path)
    checksum = (sum(struct.unpack_from("<%dI" % (spl_len // 4), data))
                - expected + 0x5f0a6c39) & 0xffffffff
    if checksum != expected:
        raise FlashError("SPL checksum mismatch: %s" % path)
    header = data[UBOOT_OFFSET:UBOOT_OFFSET + 64]
    if len(header) != 64 or header[:4] != UBOOT_MAGIC:
        raise FlashError("missing U-Boot legacy header at NAND offset 0x20000: %s"
                         % path)
    header_crc = struct.unpack_from(">I", header, 4)[0]
    if zlib.crc32(header[:4] + b"\0" * 4 + header[8:]) != header_crc:
        raise FlashError("U-Boot header CRC mismatch: %s" % path)
    size = struct.unpack_from(">I", header, 12)[0]
    payload = data[UBOOT_OFFSET + 64:UBOOT_OFFSET + 64 + size]
    if not size or len(payload) != size:
        raise FlashError("U-Boot payload is empty or truncated: %s" % path)
    if zlib.crc32(payload) != struct.unpack_from(">I", header, 24)[0]:
        raise FlashError("U-Boot payload CRC mismatch: %s" % path)


def check_ubi(path):
    """Check EC/VID headers and the board's 2 KiB / 128 KiB geometry."""
    data = read_file(path)
    if not data or len(data) % NAND_ERASE_SIZE:
        raise FlashError("UBI image must contain complete 128 KiB eraseblocks: %s"
                         % path)
    image_seq = None
    for offset in range(0, len(data), NAND_ERASE_SIZE):
        header = data[offset:offset + 64]
        if header[:4] != b"UBI#" or header[4] != 1:
            raise FlashError("invalid UBI EC header at 0x%x: %s" % (offset, path))
        if (zlib.crc32(header[:60]) ^ 0xffffffff) != struct.unpack_from(">I", header, 60)[0]:
            raise FlashError("UBI EC header CRC mismatch at 0x%x: %s" % (offset, path))
        vid_offset, data_offset, seq = struct.unpack_from(">III", header, 16)
        if (vid_offset, data_offset) != (NAND_PAGE_SIZE, 2 * NAND_PAGE_SIZE):
            raise FlashError("UBI geometry does not match ePass NAND: %s" % path)
        if image_seq is not None and seq != image_seq:
            raise FlashError("inconsistent UBI image sequence: %s" % path)
        image_seq = seq
        vid = data[offset + vid_offset:offset + vid_offset + 64]
        # An EC-only block is an unused eraseblock.
        if vid == b"\xff" * 64:
            continue
        if vid[:4] != b"UBI!" or vid[4] != 1:
            raise FlashError("invalid UBI VID header at 0x%x: %s" % (offset, path))
        if (zlib.crc32(vid[:60]) ^ 0xffffffff) != struct.unpack_from(">I", vid, 60)[0]:
            raise FlashError("UBI VID header CRC mismatch at 0x%x: %s" % (offset, path))


def check_env(path):
    data = read_file(path)
    if len(data) != ENV_MAX:
        raise FlashError("environment image must be exactly 128 KiB: %s" % path)
    if zlib.crc32(data[4:]) != struct.unpack_from("<I", data)[0]:
        raise FlashError("environment CRC mismatch: %s" % path)


def check_image(alt, path):
    """Validate a selected image for every frontend; return its byte size."""
    try:
        if not path or not os.path.isfile(path):
            raise FlashError("image not found: %s" % path)
        if not os.access(path, os.R_OK):
            raise FlashError("image is not readable: %s" % path)
        size = os.path.getsize(path)
        if size == 0:
            raise FlashError("%s image is empty: %s" % (alt, path))
        if size > ALT_LIMITS[alt]:
            raise FlashError("%s image is too large: %s is %d bytes, max is %d bytes"
                             % (alt, path, size, ALT_LIMITS[alt]))
        checker = {"bootlogo": check_bmp, "kernel": check_fit,
                   "rootfs": check_ubifs, "bootloader": check_bootloader,
                   "ubi": check_ubi, "env": check_env}.get(alt)
        if checker:
            checker(path)
        return size
    except OSError as e:
        raise FlashError("cannot read image %s: %s" % (path, e)) from e


def validate_image(alt, path):
    """Format the shared validation result for GUI and --check output."""
    if not path:
        return True, UI_TEXT["not_selected"]
    try:
        size = check_image(alt, path)
    except FlashError as e:
        return False, str(e)
    return True, "%s / ≤ %s" % (human_size(size), human_size(ALT_LIMITS[alt]))


# ---- FEL boot preparation --------------------------------------------------

def prepare_fel_boot_image(image):
    """Return (boot_image, temp_path_or_None) for sunxi-fel uboot boot."""
    data = read_file(image)
    if data[4:12] != b"eGON.BT0":
        raise FlashError("FEL boot image is not an Allwinner eGON image: %s"
                         % image)
    spl_len = _field(data, 16, "<I", "eGON SPL length", image)
    if not 0 < spl_len <= len(data):
        raise FlashError("invalid SPL length %d in %s" % (spl_len, image))

    # sunxi-fel uboot loads U-Boot from max(SPL length, 0x8000).
    fel_uboot_offset = max(spl_len, 32768)
    if data[fel_uboot_offset:fel_uboot_offset + 4] == UBOOT_MAGIC:
        return image, None

    u_boot_offset = None
    for offset in (131072, spl_len, 32768):
        if offset < len(data) and data[offset:offset + 4] == UBOOT_MAGIC:
            u_boot_offset = offset
            break
    if u_boot_offset is None:
        raise FlashError("cannot find U-Boot legacy image inside %s" % image)

    fd, temp_path = tempfile.mkstemp(prefix="epass-fel-uboot.")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data[:spl_len])
            if fel_uboot_offset > spl_len:
                f.write(b"\0" * (fel_uboot_offset - spl_len))
            f.write(data[u_boot_offset:])
    except OSError:
        os.unlink(temp_path)
        raise FlashError("failed to create temporary FEL image")
    return temp_path, temp_path


def default_split_boot_files(boot_image, fel_spl, fel_uboot, fel_addr):
    boot_dir = os.path.dirname(boot_image)
    if not fel_spl:
        candidate = os.path.join(boot_dir, "spl", "sunxi-spl.bin")
        if os.path.isfile(candidate):
            fel_spl = candidate
    if not fel_uboot:
        candidate = os.path.join(boot_dir, "u-boot-dtb.bin")
        if os.path.isfile(candidate):
            fel_uboot = candidate
    if not fel_addr:
        config = os.path.join(boot_dir, ".config")
        if os.path.isfile(config):
            try:
                with open(config) as f:
                    for line in f:
                        if line.startswith("CONFIG_TEXT_BASE="):
                            fel_addr = int(line.split("=", 1)[1].strip(), 0)
                            break
            except (OSError, ValueError):
                pass
    return fel_spl, fel_uboot, fel_addr or DEFAULT_FEL_ADDR


# ---- output stream parsing ---------------------------------------------------

class StreamParser:
    """Split subprocess output into log lines and \\r progress runs.

    dfu-util redraws one progress line with repeated carriage returns;
    like a terminal, only the text after the last \\r belongs in the log.
    """

    def __init__(self, on_log, on_run):
        self.buf = ""
        self.on_log = on_log
        self.on_run = on_run

    def feed(self, text):
        self.buf += text
        while True:
            i = self.buf.find("\n")
            if i < 0:
                break
            line = self.buf[:i]
            self.buf = self.buf[i + 1:]
            runs = line.split("\r")
            for seg in runs[:-1]:
                self._run(seg)
            self._log(runs[-1])
        if "\r" in self.buf:
            runs = self.buf.split("\r")
            for seg in runs[:-1]:
                self._run(seg)
            self.buf = runs[-1]

    def _run(self, seg):
        if seg.strip():
            self.on_run(seg)

    def _log(self, line):
        if line.strip():
            self.on_log(line)


# ---- flasher -----------------------------------------------------------------

class Flasher:
    """Run the FEL/DFU flash sequence, reporting progress via events.

    Events are (kind, value) tuples passed to on_event:
      ("log", text)      - one output line
      ("progress", frac) - overall progress in 0..1 during a transfer
    and finally ("done", returncode) exactly once.
    """

    def __init__(self, dfu_dev=DFU_VIDPID, dfu_wait=DFU_WAIT,
                 fel_tool=FEL_TOOL_DEFAULT, dfu_util=DFU_UTIL_DEFAULT,
                 capture=False, on_event=None, stop_event=None,
                 tool_timeout=TOOL_TIMEOUT):
        self.dfu_dev = dfu_dev
        self.dfu_wait = dfu_wait
        self.fel_tool = fel_tool
        self.dfu_util = dfu_util
        self.tool_timeout = tool_timeout
        self.capture = capture
        self.on_event = on_event
        self.stop_event = stop_event or threading.Event()
        self.current_proc = None
        self.current_group = None
        self.list_output = ""
        self.current_alt = None
        self.sizes = {}
        self.total_bytes = 0
        self.done_bytes = 0

    # -- plumbing ------------------------------------------------------------

    def emit(self, kind, value=None):
        if self.on_event:
            self.on_event((kind, value))

    def log(self, message):
        self.emit("log", "==> " + message)

    def check_stop(self):
        if self.stop_event.is_set():
            raise FlashError("cancelled by user")

    def kill_current(self, force=False):
        proc = self.current_proc
        if proc is None or proc.poll() is not None:
            return
        if self.current_group is None:
            return
        try:
            os.killpg(self.current_group,
                      signal.SIGKILL if force else signal.SIGTERM)
        except (OSError, ProcessLookupError):
            pass

    def _popen(self, argv, on_progress=None, timeout=None):
        """Run one host tool. Returns (returncode, collected_log_lines).

        Output is always captured. In GUI mode lines are emitted as log
        events; in CLI mode they are printed to the terminal and \\r
        progress runs are redrawn like dfu-util does natively.
        """
        self.check_stop()
        timeout = self.tool_timeout if timeout is None else timeout
        deadline = time.monotonic() + timeout
        try:
            proc = subprocess.Popen(
                argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                start_new_session=True)
        except OSError as e:
            raise FlashError("cannot run %s: %s" % (argv[0], e))
        self.current_proc = proc
        # start_new_session makes the PID the process group ID, even if the
        # command exits before the parent gets a chance to inspect it.
        self.current_group = proc.pid
        lines = []

        def on_run(seg):
            if not self.capture:
                sys.stdout.write(seg + "\r")
                sys.stdout.flush()
            if on_progress:
                m = re.search(r"(\d+)%", seg)
                if m:
                    on_progress(int(m.group(1)))

        def on_log(line):
            lines.append(line)
            if self.capture:
                self.emit("log", line)
            else:
                sys.stdout.write(line + "\n")
                sys.stdout.flush()

        parser = StreamParser(on_log=on_log, on_run=on_run)
        selector = selectors.DefaultSelector()
        completed = False
        try:
            selector.register(proc.stdout, selectors.EVENT_READ)
            eof = False
            while not eof or proc.poll() is None:
                self.check_stop()
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise FlashError("%s timed out after %g seconds" %
                                     (argv[0], timeout))
                if eof:
                    self.stop_event.wait(min(0.1, remaining))
                    continue
                if selector.select(min(0.1, remaining)):
                    chunk = os.read(proc.stdout.fileno(), 4096)
                    if chunk:
                        parser.feed(chunk.decode("utf-8", "replace"))
                    else:
                        eof = True
                        selector.unregister(proc.stdout)
            if parser.buf:
                parser.feed("\n")
            self.check_stop()
            completed = True
        finally:
            selector.close()
            if not completed:
                # Also kill descendants that inherited stdout after the
                # original tool exited, otherwise the pipe can stay open.
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            try:
                proc.stdout.close()
            except OSError:
                pass
            # Always reap the child and clear the cancellation state, even if
            # reading the pipe failed; otherwise kill_current() would signal a
            # stale process later.
            rc = proc.wait()
            self.current_proc = None
            self.current_group = None
        return rc, lines

    # -- DFU helpers -----------------------------------------------------------

    def dfu_list(self, timeout=None):
        rc, lines = self._popen([self.dfu_util, "-d", self.dfu_dev, "-l"],
                                timeout=timeout)
        if rc != 0:
            return ""
        return "\n".join(lines)

    def wait_for_dfu(self):
        self.log("Waiting for USB DFU device %s..." % self.dfu_dev)
        started = time.monotonic()
        deadline = started + self.dfu_wait
        last_progress = 0
        self.list_output = ""
        while time.monotonic() < deadline:
            self.check_stop()
            output = self.dfu_list(timeout=min(self.tool_timeout,
                                              max(0, deadline - time.monotonic())))
            self.list_output = output
            elapsed = time.monotonic() - started
            if "Found DFU:" in output:
                self.log("DFU device detected after %.1fs" % elapsed)
                return
            # Show progress every 5 seconds
            if elapsed - last_progress >= 5:
                self.log("Still waiting... (%ds elapsed)" % elapsed)
                last_progress = elapsed
            self.stop_event.wait(min(1, max(0, deadline - time.monotonic())))

        # Enhanced error message with troubleshooting hints
        error_msg = "Timed out waiting for DFU device %s after %ds" % (
            self.dfu_dev, self.dfu_wait)
        hints = [
            "\n可能的原因:",
            "1. 设备未连接或 USB 线缆故障",
            "2. 设备未进入 FEL 模式（按住 FEL 键上电）",
            "3. USB 权限问题（Linux 需要 udev 规则或 sudo）",
            "4. U-Boot 启动失败（检查 bootloader 镜像）"
        ]
        if self.list_output:
            error_msg += "\n\n最后的 dfu-util 输出:\n%s" % self.list_output
        raise FlashError(error_msg + "\n".join(hints))

    def require_alt(self, alt):
        if 'name="%s"' % alt not in self.list_output:
            raise FlashError("DFU alt '%s' is not exposed by the device" % alt)

    def _run_tool(self, desc, argv, on_progress=None):
        attempt = 1
        while True:
            rc, _ = self._popen(argv, on_progress)
            if rc == 0:
                return
            self.check_stop()
            if attempt >= self.dfu_retries:
                raise FlashError("%s failed after %d attempt(s)"
                                 % (desc, self.dfu_retries))
            self.log("%s failed (attempt %d/%d), retrying..."
                     % (desc, attempt, self.dfu_retries))
            attempt += 1
            time.sleep(1)
            self.wait_for_dfu()

    # -- flashing ---------------------------------------------------------------

    def boot_fel(self, boot_method, boot_image, fel_spl, fel_uboot, fel_addr):
        tool_name = os.path.basename(self.fel_tool.replace("\\", "/"))
        if tool_name not in ("sunxi-fel", "sunxi-fel.exe"):
            raise FlashError("unsupported FEL tool for temporary U-Boot boot: %s"
                             % self.fel_tool)
        # -v is what makes sunxi-fel report how far it got (stack probe, SPL
        # execution, eGON.FEL handshake); without it a failure is a single
        # opaque usb_bulk_send() timeout.
        if boot_method == "split":
            argv = [self.fel_tool, "-v", "-p", "spl", fel_spl,
                    "write", hex(fel_addr), fel_uboot, "exe", hex(fel_addr)]
        else:
            argv = [self.fel_tool, "-v", "-p", "uboot", boot_image]
        # Deliberately no retry and no DFU wait here: this is not a DFU
        # transfer, and _run_inner() decides whether the device still made
        # it into DFU before reporting failure.
        rc, _ = self._popen(argv)
        if rc != 0:
            raise FlashError("Booting temporary ePass U-Boot over FEL failed "
                             "(exit %d)" % rc)

    def _progress_cb(self):
        def on_progress(pct):
            if self.current_alt and self.total_bytes:
                frac = (self.done_bytes + pct / 100.0 *
                        self.sizes[self.current_alt]) / self.total_bytes
                self.emit("progress", frac)
        return on_progress

    def flash(self, alt, image):
        size = os.path.getsize(image)
        self.current_alt = alt
        self.done_bytes = sum(
            self.sizes[a] for a in FLASH_ORDER
            if a in self.sizes and
            FLASH_ORDER.index(a) < FLASH_ORDER.index(alt))
        self.log("Flashing %s from %s (%s)" % (alt, image, human_size(size)))
        self._run_tool("Flashing %s" % alt,
                       [self.dfu_util, "-d", self.dfu_dev, "-a", alt,
                        "-D", image],
                       on_progress=self._progress_cb())

    def should_verify_alt(self, alt):
        # "bootloader" is a write-only profile: the retired CLI deliberately
        # refused to read the SPL/U-Boot region back over DFU
        # (epass-flash.sh: should_verify_alt bootloader -> return 1), so it
        # behaves like "none". "raw" verifies only the raw-MTD alts, "all"
        # additionally verifies the ubi/rootfs transfer.
        if self.verify_mode == "raw":
            return alt in ("bootlogo", "env", "kernel")
        return self.verify_mode == "all"

    def verify_prefix(self, alt, image, upload_size=None):
        self.current_alt = None
        compare_size = os.path.getsize(image)
        upload_size = upload_size or compare_size
        self.log("Verifying %s by DFU upload (%s, compare first %s)"
                 % (alt, human_size(upload_size), human_size(compare_size)))
        attempt = 1
        while True:
            self.check_stop()
            fd, verify_file = tempfile.mkstemp(prefix="epass-dfu-verify.")
            os.close(fd)
            try:
                rc, _ = self._popen([self.dfu_util, "-d", self.dfu_dev,
                                     "-a", alt, "-Z", str(upload_size),
                                     "-U", verify_file])
                if rc != 0:
                    raise _Retryable("%s verification upload failed" % alt)

                # Memory-efficient comparison: 64 KiB chunks
                with open(image, "rb") as f_img, open(verify_file, "rb") as f_ver:
                    remaining = compare_size
                    chunk_size = 65536
                    while remaining > 0:
                        to_read = min(chunk_size, remaining)
                        img_chunk = f_img.read(to_read)
                        ver_chunk = f_ver.read(to_read)
                        if img_chunk != ver_chunk:
                            raise FlashError("%s verification failed" % alt)
                        remaining -= to_read

            except _Retryable as e:
                if attempt >= self.dfu_retries:
                    raise FlashError(str(e))
                self.log("Verifying %s failed (attempt %d/%d), retrying..."
                         % (alt, attempt, self.dfu_retries))
                attempt += 1
                time.sleep(1)
                self.wait_for_dfu()
                continue
            finally:
                os.unlink(verify_file)
            self.log("Verified %s first %d bytes" % (alt, compare_size))
            return

    def detach(self):
        self.log("Leaving DFU mode")
        self._run_tool("Leaving DFU mode",
                       [self.dfu_util, "-d", self.dfu_dev, "-a", "0", "-e"])

    # -- top level ----------------------------------------------------------------

    def run(self, plan):
        try:
            self._run_inner(plan)
        except FlashError as e:
            self.emit("log", "error: %s" % e)
            rc = 1
        except Exception as e:  # never leave a worker without a result
            # Any unexpected host error (OSError, ValueError, ...) must still
            # produce a ("done", ...) event, otherwise the GUI waits forever.
            self.emit("log", "error: unexpected failure: %s" % e)
            rc = 1
        else:
            rc = 0
        finally:
            _unlink_quietly(plan.get("temp_image"))
        self.emit("done", rc)
        return rc

    def _log_flash_plan(self, plan, images):
        """Log the flash plan summary."""
        self.log("Flash plan")
        if plan["skip_fel"]:
            self.emit("log", "    boot: use existing USB DFU device %s"
                      % self.dfu_dev)
        elif plan["boot_method"] == "split":
            self.emit("log", "    boot: FEL SPL %s + U-Boot %s @ 0x%x"
                      % (plan["fel_spl"], plan["fel_uboot"],
                         plan["fel_addr"]))
        else:
            self.emit("log", "    boot: FEL image %s" % plan["boot_image"])
        for alt in FLASH_ORDER:
            if alt in images:
                self.emit("log", "    %s: DFU alt %s <- %s"
                          % (alt, alt, images[alt]))
        self.emit("log", "    verify: %s" % self.verify_mode)
        self.emit("log", "    dfu retries: %d" % self.dfu_retries)

    def _boot_or_use_existing_dfu(self, plan):
        """Boot via FEL or use existing DFU device, return potential FEL error."""
        if plan["skip_fel"]:
            self.log("Using already-running DFU device %s" % self.dfu_dev)
            return None

        if plan["boot_method"] == "split":
            self.log("Booting temporary ePass U-Boot over FEL with SPL %s "
                     "and U-Boot %s at 0x%x"
                     % (plan["fel_spl"], plan["fel_uboot"],
                        plan["fel_addr"]))
        else:
            self.log("Booting temporary ePass U-Boot over FEL from %s"
                     % plan["boot_image"])
        try:
            self.boot_fel(plan["boot_method"], plan["boot_image"],
                          plan["fel_spl"], plan["fel_uboot"],
                          plan["fel_addr"])
            return None
        except FlashError as e:
            # sunxi-fel also fails when the SPL does not return to the
            # BROM, even though the temporary U-Boot may have started and
            # be sitting in DFU.  Give DFU a chance before giving up.
            self.log("FEL boot reported a failure; waiting for DFU anyway")
            return e

    def _verify_dfu_ready(self, images):
        """Log available DFU alts and verify required ones are present."""
        self.log("Available DFU alt settings")
        for line in self.list_output.splitlines():
            if line.strip():
                self.emit("log", line)

        for alt in FLASH_ORDER:
            if alt in images:
                self.require_alt(alt)

    def _flash_and_verify_all(self, images):
        """Flash all images in order and verify as needed."""
        for alt in FLASH_ORDER:
            if alt not in images:
                continue
            self.flash(alt, images[alt])
            if alt == "bootlogo" and self.should_verify_alt(alt):
                self.verify_prefix(alt, images[alt], BOOTLOGO_MAX)
            elif alt == "env" and self.should_verify_alt(alt):
                self.verify_prefix(alt, images[alt], ENV_MAX)
            elif alt == "ubi" and self.should_verify_alt(alt):
                self.verify_prefix(alt, images[alt], UBI_MAX)
            elif alt in ("kernel", "rootfs") and self.should_verify_alt(alt):
                self.verify_prefix(alt, images[alt])

    def _run_inner(self, plan):
        self.dfu_retries = plan["dfu_retries"]
        self.verify_mode = plan["verify_mode"]
        images = plan["images"]
        self.sizes = dict((alt, os.path.getsize(path))
                          for alt, path in images.items())
        self.total_bytes = sum(self.sizes.values())

        self._log_flash_plan(plan, images)

        fel_error = self._boot_or_use_existing_dfu(plan)

        try:
            self.wait_for_dfu()
        except FlashError:
            if fel_error is not None:
                raise FlashError("%s\n%s" % (fel_error, FEL_SPL_HINT))
            raise

        self._verify_dfu_ready(images)

        if plan["list_only"]:
            self.log("DFU list completed.")
            return

        self._flash_and_verify_all(images)

        if plan["reset_after"]:
            self.detach()
        self.log("ePass FEL DFU flashing completed.")


def _prefix_equal(image, verify_file, count):
    with open(image, "rb") as a, open(verify_file, "rb") as b:
        return a.read(count) == b.read(count)


def _unlink_quietly(path):
    if not path:
        return
    try:
        os.unlink(path)
    except OSError:
        pass


# ---- plan validation -----------------------------------------------------------

def validate_plan(plan):
    images = plan["images"]
    for alt, path in images.items():
        check_image(alt, path)
    if "ubi" in images and "rootfs" in images:
        raise FlashError("-u cannot be combined with -R")
    if not plan["list_only"] and not images:
        raise FlashError("no images selected")
    if plan["verify_mode"] not in VERIFY_MODES:
        raise FlashError("DFU_VERIFY must be one of: %s"
                         % ", ".join(VERIFY_MODES))
    if plan["dfu_retries"] < 1:
        raise FlashError("DFU retry count must be greater than zero")


def prepare_plan(**kwargs):
    """Build and validate a flash plan from raw options.

    Mirrors the argument handling of epass-flash.sh: FEL boot image
    resolution, split boot defaults and image validation.
    """
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_boot_image = os.path.join(script_dir, "u-boot-sunxi-with-spl.bin")

    images = kwargs["images"]
    skip_fel = kwargs["skip_fel"]
    boot_image = kwargs.get("boot_image")
    fel_spl = kwargs.get("fel_spl")
    fel_uboot = kwargs.get("fel_uboot")
    fel_addr = kwargs.get("fel_addr")
    split_requested = kwargs.get("split_requested", False)

    if not skip_fel and not boot_image:
        if "bootloader" in images:
            boot_image = images["bootloader"]
        elif os.path.isfile(default_boot_image):
            boot_image = default_boot_image
        else:
            raise FlashError("no FEL boot image: pass -b or -B, or run "
                             "next to u-boot-sunxi-with-spl.bin")

    if not skip_fel:
        if not os.path.isfile(boot_image):
            raise FlashError("image not found: %s" % boot_image)

    plan = {
        "images": images,
        "skip_fel": skip_fel,
        "boot_image": boot_image,
        "fel_spl": fel_spl,
        "fel_uboot": fel_uboot,
        "fel_addr": fel_addr,
        "boot_method": None,
        "list_only": kwargs.get("list_only", False),
        "reset_after": kwargs.get("reset_after", True),
        "verify_mode": kwargs["verify_mode"],
        "dfu_retries": kwargs["dfu_retries"],
    }

    try:
        if not skip_fel:
            if not split_requested:
                boot_image, temp_path = prepare_fel_boot_image(boot_image)
                plan["boot_image"] = boot_image
                if temp_path:
                    plan["temp_image"] = temp_path
                plan["boot_method"] = "uboot"
            else:
                fel_spl, fel_uboot, fel_addr = default_split_boot_files(
                    boot_image, fel_spl, fel_uboot, fel_addr)
                if not fel_spl or not fel_uboot:
                    raise FlashError("explicit sunxi-fel spl boot needs both "
                                     "SPL (-s) and raw U-Boot proper (-p) "
                                     "images")
                for path in (fel_spl, fel_uboot):
                    if not os.path.isfile(path):
                        raise FlashError("image not found: %s" % path)
                plan["fel_spl"] = fel_spl
                plan["fel_uboot"] = fel_uboot
                plan["fel_addr"] = fel_addr
                plan["boot_method"] = "split"

        validate_plan(plan)
    except BaseException:
        # Flasher.run() only unlinks the temporary FEL image once a plan has
        # been handed to it; clean up here so a validation failure cannot
        # leak /tmp/epass-fel-uboot.* files.
        _unlink_quietly(plan.get("temp_image"))
        raise
    return plan


# ---- CLI -------------------------------------------------------------------------

def parse_fel_addr(text):
    """Parse a sunxi-fel load/entry address.

    sunxi-fel addresses are hexadecimal, and the retired CLI passed -a
    straight through to the tool, so a bare digit string means hex too.
    """
    try:
        value = int(text.strip(), 16)
    except (AttributeError, ValueError):
        value = 0
    if value <= 0:
        raise FlashError("invalid U-Boot address: %s" % text)
    return value


def positive_seconds(text):
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("timeout must be a positive integer")
    if value <= 0:
        raise argparse.ArgumentTypeError("timeout must be greater than zero")
    return value


def build_parser():
    env = os.environ
    parser = argparse.ArgumentParser(
        prog="epass-flash.py",
        description="ePass FEL/DFU flasher (GUI when started without "
                    "options).")
    parser.add_argument("-b", "--fel-boot", metavar="FILE",
                        help="combined U-Boot image used only to boot "
                             "temporary DFU over FEL")
    parser.add_argument("-B", "--bootloader", metavar="FILE",
                        help="flash complete bootloader from FILE")
    parser.add_argument("-L", "--bootlogo", metavar="FILE",
                        help="flash BMP FILE to the bootlogo partition")
    parser.add_argument("-e", "--env", metavar="FILE",
                        help="flash FILE to the env partition")
    parser.add_argument("-u", "--ubi", metavar="FILE",
                        help="flash FILE to the unified rootfs/data UBI "
                             "partition")
    parser.add_argument("-k", "--kernel", metavar="FILE",
                        help="flash FIT FILE directly to the raw kernel "
                             "partition")
    parser.add_argument("-R", "--rootfs", metavar="FILE",
                        help="flash UBIFS FILE directly to the rootfs UBI "
                             "volume")
    parser.add_argument("-d", "--dfu-dev", default=env.get("DFU_DEV") or
                        DFU_VIDPID, metavar="VID:PID",
                        help="dfu-util USB device selector")
    parser.add_argument("-t", "--dfu-wait", type=positive_seconds,
                        default=env.get("DFU_WAIT") or DFU_WAIT,
                        metavar="SEC", help="DFU wait timeout in seconds")
    parser.add_argument("--tool-timeout", type=positive_seconds,
                        default=TOOL_TIMEOUT, metavar="SEC",
                        help="timeout for each host tool (default: 600 seconds)")
    parser.add_argument("-r", "--dfu-retries", type=int,
                        default=env.get("DFU_RETRIES") or DFU_RETRIES,
                        metavar="N", help="retry each dfu-util transfer "
                        "up to N times")
    parser.add_argument("-s", "--fel-spl", metavar="FILE",
                        help="SPL image for explicit sunxi-fel "
                             "spl/write/exe boot")
    parser.add_argument("-p", "--fel-uboot", metavar="FILE",
                        help="raw U-Boot proper image for explicit "
                             "sunxi-fel spl/write/exe boot")
    parser.add_argument("-a", "--fel-addr", metavar="ADDR",
                        help="raw U-Boot proper load/entry address")
    parser.add_argument("-F", "--skip-fel", action="store_true",
                        help="skip FEL boot and use an already-running DFU "
                             "device")
    parser.add_argument("-l", "--list", action="store_true",
                        help="boot U-Boot over FEL unless -F is used, list "
                             "DFU alt settings, then exit")
    parser.add_argument("-V", dest="verify_mode", action="store_const",
                        const="raw", default=env.get("DFU_VERIFY") or
                        VERIFY_DEFAULT,
                        help="verify flashed raw alts by DFU upload")
    parser.add_argument("-N", dest="verify_mode", action="store_const",
                        const="none",
                        help="disable DFU upload verification")
    parser.add_argument("-n", "--no-reset", action="store_true",
                        help="do not request USB DFU reset after the last "
                             "transfer")
    parser.add_argument("--check", metavar="DIR", type=str,
                        help="validate default images in DIR and exit")
    parser.add_argument("--list-usb", action="store_true",
                        help="list all USB devices and exit (diagnostic)")
    parser.add_argument("--gui", action="store_true",
                        help="force the graphical interface")
    return parser


def run_check(directory):
    """Headless validation pass over the default images in a directory."""
    if not os.path.isdir(directory):
        print("error: no such image directory: %s" % directory,
              file=sys.stderr)
        return 1
    found = scan_directory(directory)
    failed = False
    print("镜像目录: %s" % directory)
    for key, label, name in ROWS:
        path = found.get(key)
        if path:
            ok, message = validate_image(key, path)
            print("  %-24s %-32s %s" % (label, name, message if ok
                                        else "无效: " + message))
            if not ok:
                failed = True
        else:
            print("  %-24s %-32s 未找到 (可选)" % (label, name))
    return 1 if failed else 0


def list_usb_devices():
    """List all USB devices for diagnostic purposes."""
    print("=== USB 设备列表 ===\n")

    # Try lsusb first (most common on Linux)
    if shutil.which("lsusb"):
        print("所有 USB 设备 (lsusb):")
        try:
            result = subprocess.run(["lsusb"], capture_output=True, text=True)
            print(result.stdout)
        except Exception as e:
            print("无法运行 lsusb: %s\n" % e)

    # Try dfu-util -l
    dfu_util = os.environ.get("DFU_UTIL") or DFU_UTIL_DEFAULT
    if shutil.which(dfu_util):
        print("\nDFU 设备 (dfu-util -l):")
        try:
            result = subprocess.run([dfu_util, "-l"], capture_output=True,
                                    text=True)
            if result.stdout:
                print(result.stdout)
            else:
                print("未发现 DFU 设备")
            if result.returncode != 0 and result.stderr:
                print("stderr:", result.stderr)
        except Exception as e:
            print("无法运行 dfu-util: %s\n" % e)
    else:
        print("\ndfu-util 未找到: %s" % dfu_util)

    # Check for Allwinner FEL devices
    print("\n=== Allwinner FEL 设备检查 ===")
    fel_devices = [
        ("1f3a:efe8", "Allwinner F1C100s/F1C200s (FEL mode)"),
        ("1f3a:1010", "Allwinner generic FEL"),
    ]

    found_fel = False
    if shutil.which("lsusb"):
        for vid_pid, desc in fel_devices:
            try:
                result = subprocess.run(["lsusb", "-d", vid_pid],
                                        capture_output=True, text=True)
                if result.stdout.strip():
                    print("✓ 发现: %s" % desc)
                    print("  %s" % result.stdout.strip())
                    found_fel = True
            except:
                pass

    if not found_fel:
        print("未发现 FEL 模式设备")
        print("\n提示:")
        print("- 确保设备已连接并按住 FEL 键上电")
        print("- Linux 用户可能需要添加 udev 规则")
        print("- 尝试: sudo lsusb | grep 1f3a")

    return 0


def cli_main(args):
    if args.list_usb:
        return list_usb_devices()

    if args.check:
        # --check is a validation-only mode; refusing to combine it with a
        # flash request keeps a mistyped command from silently checking
        # instead of flashing.
        if any((args.bootloader, args.bootlogo, args.env, args.ubi,
                args.kernel, args.rootfs, args.skip_fel, args.list)):
            print("error: --check cannot be combined with flash options",
                  file=sys.stderr)
            return 2
        sys.exit(run_check(os.path.abspath(args.check)))

    images = {}
    for alt, path in (("bootloader", args.bootloader),
                      ("bootlogo", args.bootlogo),
                      ("env", args.env),
                      ("ubi", args.ubi),
                      ("kernel", args.kernel),
                      ("rootfs", args.rootfs)):
        if path:
            images[alt] = path

    fel_tool = os.environ.get("FEL_TOOL") or FEL_TOOL_DEFAULT
    dfu_util = os.environ.get("DFU_UTIL") or DFU_UTIL_DEFAULT

    if not args.skip_fel and not shutil.which(fel_tool):
        print("error: FEL tool not found: %s" % fel_tool, file=sys.stderr)
        return 1
    if not shutil.which(dfu_util):
        print("error: dfu-util not found: %s" % dfu_util, file=sys.stderr)
        return 1

    fel_addr = None
    if args.fel_addr:
        try:
            fel_addr = parse_fel_addr(args.fel_addr)
        except FlashError as e:
            print("error: %s" % e, file=sys.stderr)
            return 2

    try:
        plan = prepare_plan(
            images=images,
            skip_fel=args.skip_fel,
            boot_image=args.fel_boot,
            fel_spl=args.fel_spl,
            fel_uboot=args.fel_uboot,
            fel_addr=fel_addr,
            split_requested=bool(args.fel_spl or args.fel_uboot or
                                 args.fel_addr),
            list_only=args.list,
            reset_after=not args.no_reset,
            verify_mode=args.verify_mode,
            dfu_retries=args.dfu_retries)
    except FlashError as e:
        # Validation failures keep the retired CLI's exit status (1);
        # argparse usage errors remain 2.
        print("error: %s" % e, file=sys.stderr)
        return 1

    flasher = Flasher(
        dfu_dev=args.dfu_dev,
        dfu_wait=args.dfu_wait,
        fel_tool=fel_tool,
        dfu_util=dfu_util,
        tool_timeout=args.tool_timeout,
        capture=False,
        on_event=_cli_event)
    return flasher.run(plan)


def _cli_event(event):
    kind, value = event
    if kind == "log":
        print(value)
        sys.stdout.flush()


# ---- GUI ------------------------------------------------------------------------

# key -> (label, default file name); option letters come from ALT_LIMITS.
ROWS = [
    ("bootloader", "引导加载器 (SPL + U-Boot)", "u-boot-sunxi-with-spl.bin"),
    ("kernel", "内核 (FIT)", "fitImage.itb"),
    ("ubi", "统一 UBI (rootfs + data)", "rootfs.ubi"),
    ("rootfs", "rootfs 卷 (UBIFS)", "rootfs.ubifs"),
    ("bootlogo", "开机图 (BMP, 可选)", "bootlogo.bmp"),
]

MUTUALLY_EXCLUSIVE = ("ubi", "rootfs")

PRESETS = {
    "full": ("完整刷写", {"bootloader", "kernel", "ubi"}),
    "bootloader": ("仅引导加载器", {"bootloader"}),
    "kernel": ("仅内核", {"kernel"}),
    "ubi": ("仅 UBI", {"ubi"}),
    "rootfs": ("仅 rootfs 卷", {"rootfs"}),
    "bootlogo": ("仅开机图", {"bootlogo"}),
    "update": ("内核 + rootfs", {"kernel", "rootfs"}),
    "custom": ("自定义", None),
}

# UI text constants
UI_TEXT = {
    "not_selected": "未选择",
    "fel_connected": "已连接 ✓",
    "fel_disconnected": "未连接",
    "dfu_connected": "已连接 ✓",
    "dfu_disconnected": "未连接",
    "writing": "正在写入 %s…",
    "verifying": "正在校验 %s…",
    "cancelling": "正在取消…",
}

VERIFY_LABELS = {
    "bootloader": "默认（不回读）",
    "none": "关闭主机回读",
    "raw": "原始分区回读",
    "all": "全部可回读目标",
}


def scan_directory(directory):
    """Map each component key to its default image inside directory."""
    found = {}
    if directory and os.path.isdir(directory):
        for key, _label, name in ROWS:
            path = os.path.join(directory, name)
            if os.path.isfile(path):
                found[key] = path
    return found


def _usb_vidpids():
    """Enumerate currently connected USB VID:PID pairs from sysfs."""
    found = set()
    for vendor in glob.glob("/sys/bus/usb/devices/*/idVendor"):
        try:
            with open(vendor) as f:
                vid = f.read().strip()
            with open(os.path.join(os.path.dirname(vendor),
                                   "idProduct")) as f:
                pid = f.read().strip()
            found.add("%s:%s" % (vid, pid))
        except OSError:
            continue
    return found


class FlashApp(_TkBase):
    def __init__(self):
        super().__init__()
        self.title("ePass 刷机工具")
        self.minsize(960, 760)
        self.geometry("1080x860")

        self.flasher = None
        self.stop_event = None
        self.out_queue = queue.Queue()
        self.cancelled = False
        self.fel_connected = False
        self.dfu_connected = False

        # Progress tracking for time estimation
        self.flash_start_time = None
        self.last_progress = 0
        self.progress_history = []  # List of (timestamp, progress_value)

        self._configure_styles()
        self._build_ui()
        self._init_defaults()
        self.after(100, self._poll_queue)
        self.device_after = self.after(500, self.refresh_devices)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---- UI construction -------------------------------------------------

    def _configure_styles(self):
        self.configure(background="#f1f5f9")
        families = set(tkfont.families(self))
        family = next((name for name in ("Noto Sans CJK SC", "Microsoft YaHei",
                                        "PingFang SC") if name in families),
                      tkfont.nametofont("TkDefaultFont").actual("family"))
        tkfont.nametofont("TkDefaultFont").configure(family=family, size=10)
        self.ui_font = family
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure(".", font=(family, 10), background="#ffffff",
                        foreground="#1e293b", bordercolor="#e2e8f0",
                        lightcolor="#ffffff", darkcolor="#e2e8f0")
        style.configure("Page.TFrame", background="#f1f5f9")
        style.configure("Card.TFrame", background="#ffffff", relief="solid",
                        borderwidth=1)
        style.configure("Title.TLabel", background="#f1f5f9",
                        font=(family, 22, "bold"), foreground="#0f172a")
        style.configure("Subtitle.TLabel", background="#f1f5f9",
                        foreground="#64748b")
        style.configure("Section.TLabel", font=(family, 11, "bold"))
        style.configure("Muted.TLabel", foreground="#64748b", font=(family, 9))
        style.configure("Hint.TLabel", foreground="#64748b", font=(family, 9))
        style.configure("TButton", padding=(10, 3), relief="flat")
        style.configure("Small.TButton", padding=(8, 1), font=(family, 9))
        style.map("TButton", background=[("active", "#e2e8f0")],
                  foreground=[("disabled", "#94a3b8")])
        style.configure("Primary.TButton", background="#2563eb", foreground="white",
                        font=(family, 10, "bold"), padding=(20, 8))
        style.map("Primary.TButton", background=[("disabled", "#cbd5e1"),
                  ("pressed", "#1e40af"), ("active", "#1d4ed8")],
                  foreground=[("disabled", "#64748b"), ("!disabled", "white")])
        style.configure("TEntry", padding=5, fieldbackground="#f8fafc")
        style.configure("Path.TEntry", font=(family, 9), padding=(6, 3))
        style.map("Path.TEntry", fieldbackground=[("readonly", "#f8fafc")],
                  foreground=[("readonly", "#64748b"), ("disabled", "#94a3b8")])
        style.configure("TCombobox", padding=5, arrowsize=12)
        style.map("TCombobox", fieldbackground=[("readonly", "#f8fafc")],
                  selectbackground=[("readonly", "#f8fafc")],
                  selectforeground=[("readonly", "#1e293b")])
        style.configure("TCheckbutton", padding=0)
        style.map("TCheckbutton", background=[("active", "#ffffff")])
        style.configure("TSeparator", background="#e2e8f0")
        style.configure("Horizontal.TProgressbar", troughcolor="#e2e8f0",
                        background="#2563eb", borderwidth=0, thickness=8)

    def _build_ui(self):
        self.main = ttk.Frame(self, padding=18, style="Page.TFrame")
        self.main.pack(fill="both", expand=True)
        self.main.columnconfigure(0, weight=1)
        self.main.rowconfigure(4, weight=1)

        header = ttk.Frame(self.main, style="Page.TFrame")
        header.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        ttk.Label(header, text="ePass", style="Title.TLabel").pack(side="left")
        ttk.Label(header, text="固件刷写工具", style="Subtitle.TLabel").pack(
            side="left", padx=14, pady=(10, 0))
        ttk.Label(header, text="F1C200S  /  SPI NAND", style="Subtitle.TLabel").pack(
            side="right", pady=(10, 0))

        directory = ttk.Frame(self.main, padding=(14, 8), style="Card.TFrame")
        directory.grid(row=1, column=0, sticky="ew", pady=(0, 12))
        directory.columnconfigure(1, weight=1)
        ttk.Label(directory, text="镜像目录", style="Section.TLabel").grid(
            row=0, column=0, padx=(0, 14))
        self.dir_var = tk.StringVar()
        self.dir_entry = ttk.Entry(directory, textvariable=self.dir_var)
        self.dir_entry.grid(row=0, column=1, sticky="ew")
        self.dir_entry.bind("<Return>", lambda e: self.rescan(force=True))
        ttk.Button(directory, text="浏览…", command=self.browse_directory).grid(
            row=0, column=2, padx=(8, 0))
        ttk.Button(directory, text="重新扫描", command=lambda: self.rescan(force=True)).grid(
            row=0, column=3, padx=(6, 0))

        body = ttk.Frame(self.main, style="Page.TFrame")
        body.grid(row=2, column=0, sticky="ew", pady=(0, 12))
        body.columnconfigure(0, weight=1)
        components = ttk.Frame(body, padding=14, style="Card.TFrame")
        components.grid(row=0, column=0, sticky="nsew", padx=(0, 12))
        components.columnconfigure(0, weight=1)
        title_row = ttk.Frame(components)
        title_row.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        ttk.Label(title_row, text="选择刷写内容", style="Section.TLabel").pack(side="left")
        self.selection_var = tk.StringVar(value="")
        ttk.Label(title_row, textvariable=self.selection_var,
                  style="Muted.TLabel").pack(side="right")
        self.row_vars, self.path_vars = {}, {}
        self.status_vars, self.status_labels = {}, {}
        self.image_sizes, self.validation_errors = {}, {}
        self.path_entries = []
        for i, (key, label, _name) in enumerate(ROWS):
            row = ttk.Frame(components)
            row.grid(row=i + 1, column=0, sticky="ew", pady=(0, 5 if i < 4 else 0))
            row.columnconfigure(0, weight=1)
            var = tk.BooleanVar()
            self.row_vars[key] = var
            ttk.Checkbutton(row, text=label, variable=var,
                            command=lambda k=key: self._toggle_row(k)).grid(
                                row=0, column=0, sticky="w", pady=(0, 3))
            status_var = tk.StringVar()
            self.status_vars[key] = status_var
            status_label = ttk.Label(row, textvariable=status_var, style="Muted.TLabel",
                                     width=26, anchor="e")
            status_label.grid(row=0, column=1, sticky="e", pady=(0, 3))
            self.status_labels[key] = status_label
            path_var = tk.StringVar(value=UI_TEXT["not_selected"])
            self.path_vars[key] = path_var
            path = ttk.Entry(row, textvariable=path_var, state="readonly",
                             style="Path.TEntry", width=12)
            path.grid(row=1, column=0, sticky="ew", padx=(0, 8))
            self.path_entries.append(path)
            ttk.Button(row, text="选择文件…", style="Small.TButton",
                       command=lambda k=key: self.browse_image(k)).grid(
                row=1, column=1, sticky="e")

        settings = ttk.Frame(body, padding=14, style="Card.TFrame")
        settings.grid(row=0, column=1, sticky="nsew")
        settings.columnconfigure(0, weight=1)
        ttk.Label(settings, text="设备与方案", style="Section.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 8))
        device = ttk.Frame(settings)
        device.grid(row=1, column=0, sticky="ew")
        device.columnconfigure(1, weight=1)
        self.fel_var = tk.StringVar(value="检测中…")
        self.dfu_var = tk.StringVar(value="检测中…")
        for i, (label, variable) in enumerate((("FEL", self.fel_var), ("DFU", self.dfu_var))):
            ttk.Label(device, text=label, style="Muted.TLabel").grid(row=i, column=0, sticky="w")
            status = ttk.Label(device, textvariable=variable, style="Muted.TLabel")
            status.grid(row=i, column=1, sticky="e")
            if i == 0:
                self.fel_label = status
            else:
                self.dfu_label = status
        ttk.Separator(settings).grid(row=2, column=0, sticky="ew", pady=10)
        ttk.Label(settings, text="刷写方案", style="Muted.TLabel").grid(row=3, column=0, sticky="w")
        self.preset_var = tk.StringVar(value="full")
        self.preset_display_var = tk.StringVar(value=PRESETS["full"][0])
        self.preset_combo = ttk.Combobox(settings, textvariable=self.preset_display_var,
                                        values=[item[0] for item in PRESETS.values()],
                                        state="readonly", width=21)
        self.preset_combo.grid(row=4, column=0, sticky="ew", pady=(4, 6))
        self.preset_combo.bind("<<ComboboxSelected>>", self._select_preset)
        self.plan_hint_var = tk.StringVar()
        self.plan_hint = ttk.Label(settings, textvariable=self.plan_hint_var,
                                   style="Hint.TLabel", wraplength=225)
        self.plan_hint.grid(row=5, column=0, sticky="w", pady=(0, 8))
        self.skip_fel_var = tk.BooleanVar()
        ttk.Checkbutton(settings, text="设备已在 DFU，跳过 FEL",
                        variable=self.skip_fel_var).grid(row=6, column=0, sticky="w")
        ttk.Separator(settings).grid(row=7, column=0, sticky="ew", pady=10)
        options = ttk.Frame(settings)
        options.grid(row=8, column=0, sticky="ew")
        options.columnconfigure(0, weight=1)
        ttk.Label(options, text="回读校验", style="Muted.TLabel").grid(row=0, column=0, sticky="w")
        self.verify_var = tk.StringVar(value=VERIFY_DEFAULT)
        self.verify_display_var = tk.StringVar(value=VERIFY_LABELS[VERIFY_DEFAULT])
        self.verify_combo = ttk.Combobox(options, textvariable=self.verify_display_var,
                                         values=list(VERIFY_LABELS.values()),
                                         state="readonly", width=17)
        self.verify_combo.grid(row=0, column=1, sticky="e")
        self.verify_combo.bind("<<ComboboxSelected>>", self._select_verify)
        ttk.Label(options, text="尝试次数", style="Muted.TLabel").grid(
            row=1, column=0, sticky="w", pady=(8, 0))
        self.retries_var = tk.IntVar(value=DFU_RETRIES)
        ttk.Spinbox(options, from_=1, to=10, width=17,
                    textvariable=self.retries_var).grid(row=1, column=1, pady=(8, 0))

        task = ttk.Frame(self.main, padding=(14, 12), style="Card.TFrame")
        task.grid(row=3, column=0, sticky="ew", pady=(0, 12))
        task.columnconfigure(0, weight=1)
        status = ttk.Frame(task)
        status.grid(row=0, column=0, sticky="ew", padx=(0, 18))
        self.state_var = tk.StringVar(value="准备就绪")
        self.state_label = ttk.Label(status, textvariable=self.state_var, style="Section.TLabel")
        self.state_label.pack(side="left")
        self.percent_var = tk.StringVar(value="0%")
        ttk.Label(status, textvariable=self.percent_var, style="Muted.TLabel").pack(side="right")
        self.progress = ttk.Progressbar(task, maximum=100, mode="determinate")
        self.progress.grid(row=1, column=0, sticky="ew", padx=(0, 18), pady=(8, 6))
        self.step_var = tk.StringVar(value="选择镜像并连接设备后开始刷写")
        details = ttk.Frame(task)
        details.grid(row=2, column=0, sticky="ew", padx=(0, 18))
        ttk.Label(details, textvariable=self.step_var, style="Muted.TLabel").pack(side="left")
        self.timing_var = tk.StringVar(value="")
        ttk.Label(details, textvariable=self.timing_var, style="Muted.TLabel").pack(side="right")
        actions = ttk.Frame(task)
        actions.grid(row=0, column=1, rowspan=3, sticky="e")
        self.cancel_btn = ttk.Button(actions, text="取消", command=self.cancel, state="disabled")
        self.cancel_btn.pack(side="left", padx=(0, 8))
        self.start_btn = ttk.Button(actions, text="开始刷写", command=self.start, style="Primary.TButton")
        self.start_btn.pack(side="left")

        log_frame = ttk.Frame(self.main, padding=12, style="Card.TFrame")
        log_frame.grid(row=4, column=0, sticky="nsew")
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(1, weight=1)
        toolbar = ttk.Frame(log_frame)
        toolbar.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 8))
        ttk.Label(toolbar, text="运行日志", style="Section.TLabel").pack(side="left")
        self.follow_log_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(toolbar, text="跟随输出", variable=self.follow_log_var).pack(side="left", padx=14)
        self.log_buttons = []
        for label, command in (("保存…", self._save_log), ("复制", self._copy_log),
                               ("清空", self._clear_log)):
            button = ttk.Button(toolbar, text=label, command=command, style="Small.TButton")
            button.pack(side="right", padx=(4, 0))
            if label != "清空":
                self.log_buttons.append(button)
        self.log = tk.Text(log_frame, height=5, width=40, state="disabled",
                           wrap="none", relief="flat", borderwidth=0, padx=10, pady=8,
                           font="TkFixedFont", background="#0f172a", foreground="#cbd5e1",
                           selectbackground="#334155", selectforeground="#ffffff")
        self.log.grid(row=1, column=0, sticky="nsew")
        yscroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        yscroll.grid(row=1, column=1, sticky="ns")
        xscroll = ttk.Scrollbar(log_frame, orient="horizontal", command=self.log.xview)
        xscroll.grid(row=2, column=0, sticky="ew")
        self.log.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.log.tag_configure("info", foreground="#cbd5e1")
        self.log.tag_configure("ok", foreground="#6ee7b7")
        self.log.tag_configure("error", foreground="#fda4af")
        self.log_menu = tk.Menu(self.log, tearoff=0)
        self.log_menu.add_command(label="复制全部", command=self._copy_log)
        self.log_menu.add_command(label="保存日志", command=self._save_log)
        self.log_menu.add_separator()
        self.log_menu.add_command(label="清空", command=self._clear_log)
        self.log.bind("<Button-3>", self._show_log_menu)

    def _init_defaults(self):
        # Default to the directory holding this script and the images.
        here = os.path.dirname(os.path.abspath(__file__))
        # When run from the board directory, fall back to the build output.
        repo_images = os.path.join(os.path.dirname(os.path.dirname(
            os.path.dirname(here))), "output", "images")
        if os.path.isfile(os.path.join(here, "fitImage.itb")):
            self.dir_var.set(here)
        elif os.path.isdir(repo_images):
            self.dir_var.set(repo_images)
        else:
            self.dir_var.set(here)
        self.rescan(force=True)
        self._apply_preset()

    # ---- image selection ---------------------------------------------------

    def browse_directory(self):
        directory = filedialog.askdirectory(
            initialdir=self.dir_var.get() or ".")
        if directory:
            self.dir_var.set(os.path.normpath(directory))
            self.rescan(force=True)

    def browse_image(self, key):
        names = dict((row[0], row[2]) for row in ROWS)
        labels = dict((row[0], row[1]) for row in ROWS)
        path = filedialog.askopenfilename(
            title="选择 %s 镜像" % labels[key],
            initialdir=self.dir_var.get() or ".",
            initialfile=names[key] if os.path.isfile(
                os.path.join(self.dir_var.get() or ".", names[key])) else "",
            filetypes=[("所有文件", "*.*")])
        if path:
            self.path_vars[key].set(path)
            self.preset_var.set("custom")
            self._refresh_row_status(key)
            self._refresh_selection()

    def rescan(self, force=False):
        """Fill in default image paths from the current directory."""
        found = scan_directory(self.dir_var.get())
        for key in self.path_vars:
            current = self.path_vars[key].get()
            if current and current != UI_TEXT["not_selected"] and os.path.isfile(current) \
                    and not force:
                continue
            self.path_vars[key].set(found.get(key, UI_TEXT["not_selected"]))
        for key in self.path_vars:
            self._refresh_row_status(key)
        self._refresh_selection()

    def _refresh_row_status(self, key):
        path = self.path_vars[key].get()
        if path == UI_TEXT["not_selected"]:
            path = None
        ok, message = validate_image(key, path)
        self.validation_errors[key] = "" if ok else message
        self.image_sizes[key] = 0
        if path and ok:
            try:
                self.image_sizes[key] = os.path.getsize(path)
            except OSError:
                pass
        self.status_vars[key].set(message if ok else "校验失败，请更换镜像")
        self.status_labels[key].configure(
            foreground="#64748b" if not path else ("#15803d" if ok else "#be123c"))
        return ok

    def _select_preset(self, _event=None):
        label = self.preset_display_var.get()
        for key, (text, _selection) in PRESETS.items():
            if text == label:
                self.preset_var.set(key)
                self._apply_preset()
                break

    def _select_verify(self, _event=None):
        label = self.verify_display_var.get()
        for key, text in VERIFY_LABELS.items():
            if text == label:
                self.verify_var.set(key)
                break

    def _refresh_selection(self):
        selected = [key for key, var in self.row_vars.items() if var.get()]
        size = sum(self.image_sizes.get(key, 0) for key in selected)
        self.selection_var.set("已选 %d 项 · %s" % (len(selected), human_size(size)))
        self.preset_display_var.set(PRESETS[self.preset_var.get()][0])
        if "ubi" in selected:
            self.plan_hint_var.set("重建 rootfs 与 data 卷，设备原有数据将被清除。")
            self.plan_hint.configure(foreground="#b45309")
        else:
            self.plan_hint_var.set("仅更新勾选的组件；rootfs 卷更新保留 data 卷。")
            self.plan_hint.configure(foreground="#64748b")

    def _toggle_row(self, key):
        var = self.row_vars[key]
        if key in MUTUALLY_EXCLUSIVE and var.get():
            for other in MUTUALLY_EXCLUSIVE:
                if other != key and self.row_vars[other].get():
                    self.row_vars[other].set(False)
        self.preset_var.set("custom")
        self._refresh_selection()

    def _apply_preset(self):
        _text, selection = PRESETS[self.preset_var.get()]
        if selection is not None:
            for key in self.row_vars:
                self.row_vars[key].set(key in selection)
        self._refresh_selection()

    # ---- device probing ------------------------------------------------------

    def refresh_devices(self):
        """Check FEL/DFU presence by enumerating USB devices via sysfs.

        Running sunxi-fel/dfu-util here could hang or fight with an active
        flashing session over the USB device; reading sysfs has no side
        effects.
        """
        if getattr(self, "device_after", None) is not None:
            self.after_cancel(self.device_after)
            self.device_after = None
        if self.flasher:
            return
        found = _usb_vidpids()
        self._update_device_status(FEL_VIDPID in found, DFU_VIDPID in found)

    def _update_device_status(self, fel_ok, dfu_ok):
        self.fel_connected = fel_ok
        self.dfu_connected = dfu_ok
        self.fel_var.set(UI_TEXT["fel_connected"] if fel_ok else UI_TEXT["fel_disconnected"])
        self.dfu_var.set(UI_TEXT["dfu_connected"] if dfu_ok else UI_TEXT["dfu_disconnected"])
        self.fel_label.configure(foreground="#0a7a2f" if fel_ok
                                 else "#808080")
        self.dfu_label.configure(foreground="#0a7a2f" if dfu_ok
                                 else "#808080")
        if not self.flasher:
            self.device_after = self.after(3000, self.refresh_devices)

    # ---- flashing ---------------------------------------------------------------

    def _set_busy(self, busy):
        def walk(frame):
            for child in frame.winfo_children():
                if child == self.cancel_btn or child in self.log_buttons:
                    continue
                if isinstance(child, ttk.Checkbutton) and str(child.cget("variable")) == str(self.follow_log_var):
                    continue
                if isinstance(child, (ttk.Button, ttk.Entry, ttk.Checkbutton,
                                      ttk.Radiobutton, ttk.Combobox,
                                      ttk.Spinbox)):
                    restore = "readonly" if isinstance(child, ttk.Combobox) or child in self.path_entries else "normal"
                    child.configure(state="disabled" if busy else restore)
                walk(child)

        walk(self.main)
        self.cancel_btn.configure(state="normal" if busy else "disabled")

    def start(self):
        if self.flasher:
            return
        if self.row_vars["ubi"].get() and self.row_vars["rootfs"].get():
            messagebox.showerror(
                "组件冲突",
                "统一 UBI 和 rootfs 卷不能同时刷写 (对应 -u 和 -R)。\n"
                "完整刷写请使用统一 UBI, 单独更新 rootfs 请取消勾选统一 UBI。")
            return
        fel_tool = os.environ.get("FEL_TOOL") or FEL_TOOL_DEFAULT
        if not self.skip_fel_var.get() and not shutil.which(fel_tool):
            messagebox.showerror(
                "缺少工具",
                "未在 PATH 中找到 %s。\n若设备已停在新版 U-Boot 的 DFU 模式, "
                "可勾选\"设备已在 DFU 模式 (跳过 FEL)\"。" % fel_tool)
            return
        dfu_util = os.environ.get("DFU_UTIL") or DFU_UTIL_DEFAULT
        if not shutil.which(dfu_util):
            messagebox.showerror("缺少工具",
                                 "未在 PATH 中找到 %s。" % dfu_util)
            return

        labels = dict((row[0], row[1]) for row in ROWS)
        images = {}
        invalid = []
        for key in self.row_vars:
            if not self.row_vars[key].get():
                continue
            path = self.path_vars[key].get()
            if path == UI_TEXT["not_selected"] or not path:
                invalid.append("%s: 未选择镜像文件" % labels[key])
                continue
            if not self._refresh_row_status(key):
                invalid.append("%s: %s" % (labels[key],
                                           self.validation_errors[key]))
            images[key] = path
        if invalid:
            messagebox.showerror("镜像无效", "\n".join(invalid))
            return
        if not images:
            messagebox.showwarning("未选择组件", "请至少勾选一个要刷写的组件。")
            return

        # Check device connection
        found = _usb_vidpids()
        self.fel_connected = FEL_VIDPID in found
        self.dfu_connected = DFU_VIDPID in found
        need_fel = not self.skip_fel_var.get()
        if need_fel and not self.fel_connected:
            messagebox.showerror("设备未连接",
                                "FEL 设备未连接，请:\n"
                                "1. 按住 FEL 按键并连接 USB\n"
                                "2. 或勾选「跳过 FEL」(设备已在 DFU 模式)")
            return
        if self.skip_fel_var.get() and not self.dfu_connected:
            messagebox.showerror("设备未连接",
                                "DFU 设备未连接，请确保设备已进入 DFU 模式。")
            return

        if not self._ask_confirm(images):
            return

        try:
            retries = int(self.retries_var.get())
        except (tk.TclError, ValueError):
            retries = DFU_RETRIES
        try:
            plan = prepare_plan(
                images=images,
                skip_fel=self.skip_fel_var.get(),
                boot_image=images.get("bootloader") or os.path.join(
                    self.dir_var.get(), "u-boot-sunxi-with-spl.bin"),
                fel_spl=None,
                fel_uboot=None,
                fel_addr=None,
                split_requested=False,
                list_only=False,
                reset_after=True,
                verify_mode=self.verify_var.get(),
                dfu_retries=retries)
        except FlashError as e:
            messagebox.showerror("刷写计划无效", str(e))
            return

        self._clear_log()
        self.cancelled = False
        self.stop_event = threading.Event()
        self.flasher = Flasher(
            fel_tool=fel_tool,
            dfu_util=dfu_util,
            capture=True,
            on_event=self.out_queue.put,
            stop_event=self.stop_event)

        self._log_line("== 开始刷写: %s ==" % ", ".join(
            sorted(images, key=FLASH_ORDER.index)), "info")
        self._set_busy(True)
        self.flash_start_time = time.monotonic()
        self.last_progress = 0
        self.progress_history = []
        self.percent_var.set("0%")
        self.timing_var.set("")
        self.state_var.set("刷写中…")
        self.state_label.configure(foreground="#222222")
        self.progress.configure(mode="indeterminate")
        self.progress.start(40)
        self.step_var.set("启动刷写…")

        threading.Thread(target=self.flasher.run, args=(plan,),
                         daemon=True).start()

    def _ask_confirm(self, images):
        labels = dict((row[0], row[1]) for row in ROWS)
        names = [labels[k] for k in
                 sorted(images, key=FLASH_ORDER.index)]
        if "ubi" in images:
            extra = "\n\n注意: 统一 UBI 会重建全部卷, 设备上的数据将被清除。"
        else:
            extra = ""
        return messagebox.askyesno(
            "确认刷写",
            "即将刷写以下组件:\n  - %s%s\n\n刷写过程中请勿断开设备电源或 USB 连接。"
            % ("\n  - ".join(names), extra))

    def _poll_queue(self):
        """Process flasher events with batched log updates for performance."""
        log_buffer = []
        try:
            while True:
                event = self.out_queue.get_nowait()
                kind = event[0]
                if kind == "log":
                    log_buffer.append(event[1])
                elif kind == "progress":
                    # Flush any pending logs before updating progress
                    if log_buffer:
                        self._flush_log_buffer(log_buffer)
                        log_buffer = []
                    self.progress.configure(mode="determinate")
                    self.progress.stop()
                    progress_val = min(100, event[1] * 100)
                    self.progress["value"] = progress_val
                    self.percent_var.set("%.1f%%" % progress_val)
                    self._update_time_estimate(progress_val)
                elif kind == "done":
                    # Flush remaining logs before finishing
                    if log_buffer:
                        self._flush_log_buffer(log_buffer)
                        log_buffer = []
                    self._finish(event[1])
        except queue.Empty:
            # Flush any accumulated logs before next poll
            if log_buffer:
                self._flush_log_buffer(log_buffer)
        self.after(50, self._poll_queue)

    def _update_time_estimate(self, progress_val):
        """Update ETA based on progress history."""
        now = time.monotonic()

        # Record progress
        if self.flash_start_time is None:
            self.flash_start_time = now
            self.last_progress = 0
            self.progress_history = [(now, progress_val)]
            return

        # Only update if progress increased significantly
        if progress_val > self.last_progress + 1:
            self.progress_history.append((now, progress_val))
            self.last_progress = progress_val

            # Keep only recent history (last 10 samples)
            if len(self.progress_history) > 10:
                self.progress_history.pop(0)

        # Calculate ETA from linear regression of recent history
        if len(self.progress_history) >= 2 and progress_val > 5:
            elapsed = now - self.flash_start_time

            # Simple average rate from history
            first_t, first_p = self.progress_history[0]
            last_t, last_p = self.progress_history[-1]

            if last_t > first_t and last_p > first_p:
                rate = (last_p - first_p) / (last_t - first_t)  # percent/sec
                if rate > 0:
                    remaining = 100 - progress_val
                    eta_seconds = remaining / rate

                    if eta_seconds < 120:
                        eta_str = "%d 秒" % eta_seconds
                    else:
                        eta_str = "%d 分 %d 秒" % (eta_seconds // 60, eta_seconds % 60)

                    elapsed_str = "%d 秒" % elapsed if elapsed < 120 else \
                                  "%d 分 %d 秒" % (elapsed // 60, elapsed % 60)

                    self.timing_var.set("已用 %s · 预计传输剩余 %s" %
                                        (elapsed_str, eta_str))
                    return

        # Fallback: just show elapsed time
        if self.flash_start_time:
            elapsed = now - self.flash_start_time
            elapsed_str = "%d 秒" % elapsed if elapsed < 120 else \
                         "%d 分 %d 秒" % (elapsed // 60, elapsed % 60)
            self.timing_var.set("已用 %s" % elapsed_str)

    def _flush_log_buffer(self, lines):
        """Batch insert multiple log lines for better performance."""
        if not lines:
            return
        self.log.configure(state="normal")
        for line in lines:
            if not line.strip():
                continue
            tag = "error" if line.startswith("error") else "info"
            if line.startswith("==> ") and (
                    "completed" in line or "Verified" in line or
                    "Validated" in line):
                tag = "ok"
            self.log.insert("end", line.rstrip("\r") + "\n", tag)
            self._handle_step(line)
        if self.follow_log_var.get():
            self.log.see("end")
        self.log.configure(state="disabled")

    def _log_line(self, line, tag=None):
        """Single line log insertion (for non-queue logging)."""
        if not line.strip() and tag is None:
            return
        if tag is None:
            tag = "error" if line.startswith("error") else "info"
            if line.startswith("==> ") and (
                    "completed" in line or "Verified" in line or
                    "Validated" in line):
                tag = "ok"
        self.log.configure(state="normal")
        self.log.insert("end", line.rstrip("\r") + "\n", tag)
        if self.follow_log_var.get():
            self.log.see("end")
        self.log.configure(state="disabled")
        self._handle_step(line)

    def _handle_step(self, line):
        """Track high-level phases from flasher log lines."""
        m = re.match(r"==> Flashing (\S+)", line)
        if m:
            self.progress.configure(mode="determinate")
            self.progress.stop()
            # Byte-weighted progress spans all selected components.
            self.progress["value"] = self.last_progress
            self.progress_history = []
            label = next((label for key, label, _name in ROWS if key == m.group(1)), m.group(1))
            self.step_var.set("正在写入 %s…" % label)
            return
        if line.startswith("==> Verifying"):
            self.progress.configure(mode="indeterminate")
            self.progress.stop()
            self.progress.start(40)
            self.step_var.set("正在校验 %s…" % line.split()[2])
            return
        if "Booting temporary ePass U-Boot" in line:
            self.step_var.set("FEL 启动临时 U-Boot…")
        elif "Waiting for USB DFU" in line:
            self.step_var.set("等待设备进入 DFU 模式…")
        elif "flashing completed" in line:
            self.step_var.set("刷写完成")

    def _finish(self, rc):
        self.progress.stop()
        self.flasher = None
        self._set_busy(False)

        # Reset time tracking
        self.flash_start_time = None
        self.progress_history = []

        if self.cancelled:
            self.percent_var.set("已取消")
            self.step_var.set("刷写已中止，设备可能需要重新刷写")
            self.progress.configure(mode="determinate", value=0)
            self.state_var.set("已取消")
            self.state_label.configure(foreground="#c02020")
            self._log_line("== 已取消 ==", "error")
        elif rc == 0:
            self.percent_var.set("100%")
            self.step_var.set("所选组件已刷写完成")
            self.progress.configure(mode="determinate", value=100)
            self.state_var.set("刷写完成")
            self.state_label.configure(foreground="#0a7a2f")
            self._log_line("== 刷写完成 ==", "ok")
        else:
            self.percent_var.set("失败")
            self.step_var.set("请查看日志中的错误信息后重试")
            self.progress.configure(mode="determinate", value=0)
            self.state_var.set("失败 (退出码 %s)" % rc)
            self.state_label.configure(foreground="#c02020")
            self._log_line("== 刷写失败, 退出码 %s ==" % rc, "error")
        self.refresh_devices()

    def _show_log_menu(self, event):
        try:
            self.log_menu.tk_popup(event.x_root, event.y_root)
        finally:
            self.log_menu.grab_release()

    def _copy_log(self):
        log_text = self.log.get("1.0", "end-1c")
        self.clipboard_clear()
        self.clipboard_append(log_text)

    def _save_log(self):
        log_text = self.log.get("1.0", "end-1c")
        if not log_text.strip():
            messagebox.showinfo("空日志", "当前没有日志可保存")
            return
        filename = filedialog.asksaveasfilename(
            defaultextension=".log",
            filetypes=[("日志文件", "*.log"), ("文本文件", "*.txt"),
                       ("所有文件", "*.*")],
            initialfile="epass-flash-%s.log" %
                        time.strftime("%Y%m%d-%H%M%S"))
        if filename:
            try:
                with open(filename, "w", encoding="utf-8") as f:
                    f.write(log_text)
                messagebox.showinfo("保存成功", "日志已保存到:\n%s" % filename)
            except Exception as e:
                messagebox.showerror("保存失败", str(e))

    def _clear_log(self):
        if self.flasher:
            messagebox.showwarning("正在刷写", "刷写过程中无法清空日志")
            return
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def cancel(self):
        if not self.flasher:
            return
        self.cancelled = True
        self.state_var.set("正在取消…")
        self._log_line("== 用户取消 ==", "error")
        self.stop_event.set()
        self.flasher.kill_current()

        def force_kill():
            if self.flasher:
                self.flasher.kill_current(force=True)
        self.after(2000, force_kill)

    def _on_close(self):
        if self.flasher and not messagebox.askokcancel(
                "退出", "刷写正在进行, 确定要退出吗?"):
            return
        if self.flasher:
            self.stop_event.set()
            self.flasher.kill_current()
        self.destroy()


def gui_main():
    if tk is None:
        print("error: the graphical interface needs Python tkinter "
              "(python3-tk)", file=sys.stderr)
        return 1
    FlashApp().mainloop()
    return 0


def main():
    # Bare invocation opens the GUI; any option selects CLI mode.
    if len(sys.argv) == 1:
        sys.exit(gui_main())

    parser = build_parser()
    args = parser.parse_args()
    if args.gui:
        sys.exit(gui_main())
    sys.exit(cli_main(args))


if __name__ == "__main__":
    main()
