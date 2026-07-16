################################################################################
#
# doh-proxy — minimal DNS-over-HTTPS proxy
#
################################################################################

DOH_PROXY_VERSION = 1.0
DOH_PROXY_SITE = $(BR2_EXTERNAL_ext_tree_PATH)/package/doh-proxy
DOH_PROXY_SITE_METHOD = local
DOH_PROXY_DEPENDENCIES = libcurl

define DOH_PROXY_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -s \
		$(@D)/doh-proxy.c \
		-o $(@D)/doh-proxy \
		$(TARGET_LDFLAGS) -lcurl
endef

define DOH_PROXY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/doh-proxy $(TARGET_DIR)/usr/sbin/doh-proxy
	$(INSTALL) -D -m 0755 $(DOH_PROXY_PKGDIR)/S90doh-proxy $(TARGET_DIR)/etc/init.d/S90doh-proxy
endef

$(eval $(generic-package))
