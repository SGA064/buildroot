################################################################################
#
# fplayerdemo
#
################################################################################

FPLAYERDEMO_SITE = $(FPLAYERDEMO_PKGDIR)/src
FPLAYERDEMO_SITE_METHOD = local
FPLAYERDEMO_LICENSE = GPL-2.0
FPLAYERDEMO_OVERRIDE_SRCDIR = $(FPLAYERDEMO_PKGDIR)/src
FPLAYERDEMO_DEPENDENCIES = \
	alsa-lib \
	ffmpeg \
	host-pkgconf \
	libdrm

FPLAYERDEMO_PKGS = \
	alsa \
	libavcodec \
	libavutil \
	libdrm

FPLAYERDEMO_SOURCES = \
	$(@D)/audio.c \
	$(@D)/common.c \
	$(@D)/display.c \
	$(@D)/file_io.c \
	$(@D)/fplayerdemo.c \
	$(@D)/h264.c \
	$(@D)/h264_parser.c \
	$(@D)/media.c \
	$(@D)/video_decoder.c

define FPLAYERDEMO_BUILD_CMDS
	$(HOSTCC) $(HOST_CFLAGS) -std=gnu11 -O2 -Wall -Wextra -Werror \
		-I$(@D) \
		-o $(@D)/h264-parser-test \
		$(@D)/h264_parser.c \
		$(FPLAYERDEMO_PKGDIR)/tests/h264_parser_test.c \
		$(HOST_LDFLAGS)
	$(@D)/h264-parser-test --selftest
	$(TARGET_CC) $(TARGET_CFLAGS) -O3 -DNDEBUG \
		-pthread \
		`$(PKG_CONFIG_HOST_BINARY) --cflags $(FPLAYERDEMO_PKGS)` \
		-DHAVE_LIBAVCODEC \
		-o $(@D)/fplayerdemo $(FPLAYERDEMO_SOURCES) \
		$(TARGET_LDFLAGS) \
		`$(PKG_CONFIG_HOST_BINARY) --libs $(FPLAYERDEMO_PKGS)` \
		-pthread
endef

define FPLAYERDEMO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fplayerdemo \
		$(TARGET_DIR)/usr/bin/fplayerdemo
endef

$(eval $(generic-package))
