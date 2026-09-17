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

# Only settings that cannot work on this board are disabled here:
#   * Bluetooth and SLE are not built at all (no bluez, no BT in the kernel).
#   * WIFI_WOW arms wake-on-wireless through system suspend, but the kernel is
#     built with CONFIG_SUSPEND=n and wow.bin is never loaded at runtime, so the
#     whole WOW_OFFLOAD/DYNAMIC_OFFLOAD path is dead code.
# The remaining optional features in ws73_default.config (CSI/TWT/MBO/11KVR/
# PROMISC/... ) are deliberately left alone: the vendor "light" profile is not a
# reduced feature set (it only swaps SNIFFER/WPA3/WAPI/MFG_TEST in), and there is
# no evidence on which of them are safe to drop, so trimming them would be an
# unverified change.
define WS73_RADIO_CONFIGURE_CMDS
	$(SED) \
		's/^WIFI_BTCOEX=y/# WIFI_BTCOEX is not set/' \
		-e 's/^BT_EM_BUFFER_CALI_SUPPORT=y/# BT_EM_BUFFER_CALI_SUPPORT is not set/' \
		-e 's/^WSCFG_BLE_COMPILE_BY_DEFAULT=y/# WSCFG_BLE_COMPILE_BY_DEFAULT is not set/' \
		-e 's/^WSCFG_SLE_COMPILE_BY_DEFAULT=y/# WSCFG_SLE_COMPILE_BY_DEFAULT is not set/' \
		-e 's/^WIFI_WOW=y/# WIFI_WOW is not set/' \
		$(@D)/build/config/ws73_default.config
endef

define WS73_RADIO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		KERNEL_DIR=$(LINUX_DIR) \
		CROSS_COMPILE=$(TARGET_CROSS) \
		TARGET_ARCH=$(KERNEL_ARCH) \
		wifi
endef

define WS73_RADIO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/output/bin/plat_soc.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION_PROBED)/extra/plat_soc.ko
	$(INSTALL) -D -m 0644 $(@D)/output/bin/wifi_soc.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION_PROBED)/extra/wifi_soc.ko
	find $(TARGET_DIR)/lib/modules -type f \
		\( -name 'ble_soc.ko' -o -name 'sle_soc.ko' \) -delete
	$(INSTALL) -D -m 0644 $(@D)/output/bin/ws73_cfg.ini \
		$(TARGET_DIR)/etc/ws73_cfg.ini
	$(INSTALL) -d $(TARGET_DIR)/etc/ws73
	rm -f $(TARGET_DIR)/etc/ws73/btc_cali.bin $(TARGET_DIR)/etc/ws73/wow.bin
	$(INSTALL) -m 0644 \
		$(@D)/firmware/us/wifi_cali.bin \
		$(@D)/firmware/us/ws73.bin \
		$(TARGET_DIR)/etc/ws73/
	$(INSTALL) -D -m 0755 $(WS73_RADIO_PKGDIR)/ws73-switch \
		$(TARGET_DIR)/usr/sbin/ws73-switch
endef

$(eval $(generic-package))
