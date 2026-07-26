################################################################################
#
# ws73-radio
#
################################################################################

WS73_RADIO_SITE = $(TOPDIR)/../Ai-WS1-CBS-CBE_Linux
WS73_RADIO_SITE_METHOD = local
WS73_RADIO_OVERRIDE_SRCDIR = $(WS73_RADIO_SITE)
WS73_RADIO_DEPENDENCIES = linux host-python3
WS73_RADIO_LICENSE = GPL-2.0 (driver), PROPRIETARY (firmware)

define WS73_RADIO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		KERNEL_DIR=$(LINUX_DIR) \
		CROSS_COMPILE=$(TARGET_CROSS) \
		TARGET_ARCH=$(KERNEL_ARCH) \
		wifi ble sle
endef

define WS73_RADIO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/output/bin/plat_soc.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION_PROBED)/extra/plat_soc.ko
	$(INSTALL) -D -m 0644 $(@D)/output/bin/wifi_soc.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION_PROBED)/extra/wifi_soc.ko
	$(INSTALL) -D -m 0644 $(@D)/output/bin/ble_soc.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION_PROBED)/extra/ble_soc.ko
	$(INSTALL) -D -m 0644 $(@D)/output/bin/sle_soc.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION_PROBED)/extra/sle_soc.ko
	$(INSTALL) -D -m 0644 $(@D)/output/bin/ws73_cfg.ini \
		$(TARGET_DIR)/etc/ws73_cfg.ini
	$(INSTALL) -d $(TARGET_DIR)/etc/ws73
	$(INSTALL) -m 0644 $(@D)/firmware/us/*.bin $(TARGET_DIR)/etc/ws73/
	$(INSTALL) -D -m 0755 $(WS73_RADIO_PKGDIR)/ws73-switch \
		$(TARGET_DIR)/usr/sbin/ws73-switch
endef

$(eval $(generic-package))
