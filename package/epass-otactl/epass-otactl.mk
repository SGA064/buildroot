################################################################################
#
# epass-otactl
#
################################################################################

EPASS_OTACTL_VERSION = 1.0
EPASS_OTACTL_SITE = $(EPASS_OTACTL_PKGDIR)
EPASS_OTACTL_SITE_METHOD = local
EPASS_OTACTL_DEPENDENCIES = mtd

define EPASS_OTACTL_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) $(@D)/otactl.c -o $(@D)/epass-otactl \
		-lmtd
endef

define EPASS_OTACTL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/epass-otactl \
		$(TARGET_DIR)/usr/sbin/epass-otactl
	$(INSTALL) -D -m 0755 $(EPASS_OTACTL_PKGDIR)/scripts/epass-ota-install \
		$(TARGET_DIR)/usr/sbin/epass-ota-install
	$(INSTALL) -D -m 0644 $(EPASS_OTACTL_PKGDIR)/scripts/epass-init-functions.sh \
		$(TARGET_DIR)/usr/lib/epass-init-functions.sh
endef

$(eval $(generic-package))
