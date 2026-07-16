VOLUME_ENCODER_VERSION = 1.0
VOLUME_ENCODER_SITE_METHOD = local
VOLUME_ENCODER_SITE = $(TOPDIR)/../ext_tree/package/volume-encoder

define VOLUME_ENCODER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -O2 -s \
		-o $(@D)/volume-encoder $(@D)/volume-encoder.c
endef

define VOLUME_ENCODER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/volume-encoder $(TARGET_DIR)/usr/sbin/volume-encoder
	$(INSTALL) -D -m 0755 $(@D)/S96volume-encoder $(TARGET_DIR)/etc/init.d/S96volume-encoder
endef

$(eval $(generic-package))
