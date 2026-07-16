################################################################################
#
# rinetd — simple TCP redirector
#
################################################################################

RINETD_VERSION = 1.0
RINETD_SITE = $(BR2_EXTERNAL_ext_tree_PATH)/package/rinetd
RINETD_SITE_METHOD = local

define RINETD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -s \
		$(@D)/rinetd.c -o $(@D)/rinetd
endef

define RINETD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/rinetd $(TARGET_DIR)/usr/sbin/rinetd
endef

$(eval $(generic-package))
