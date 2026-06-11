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
	gst1-plugins-bad \
	gstreamer1 \
	host-pkgconf \
	libdrm

FPLAYERDEMO_PKGS = \
	alsa \
	gstreamer-codecparsers-1.0 \
	gstreamer-video-1.0 \
	libavcodec \
	libavformat \
	libavutil \
	libdrm

define FPLAYERDEMO_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O3 -DNDEBUG -DGST_USE_UNSTABLE_API \
		-pthread \
		`$(PKG_CONFIG_HOST_BINARY) --cflags $(FPLAYERDEMO_PKGS)` \
		-DHAVE_LIBAVCODEC \
		-o $(@D)/fplayerdemo $(@D)/fplayerdemo.c \
		$(TARGET_LDFLAGS) \
		`$(PKG_CONFIG_HOST_BINARY) --libs $(FPLAYERDEMO_PKGS)` \
		-pthread
endef

define FPLAYERDEMO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fplayerdemo \
		$(TARGET_DIR)/usr/bin/fplayerdemo
endef

$(eval $(generic-package))
