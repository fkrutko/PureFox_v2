################################################################################
#
# ymusic
#
################################################################################

YMUSIC_VERSION = 1.0
YMUSIC_SITE_METHOD = local
YMUSIC_SITE = $(BR2_EXTERNAL_ext_tree_PATH)/package/ymusic
YMUSIC_LICENSE = GPL-2.0+, MIT (cJSON)
YMUSIC_DEPENDENCIES = libcurl openssl

define YMUSIC_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -O2 -s \
		-o $(@D)/ymusicd $(@D)/ymusicd.c $(@D)/cJSON.c -lcurl -lcrypto -lpthread
endef

define YMUSIC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ymusicd $(TARGET_DIR)/usr/bin/ymusicd
endef

$(eval $(generic-package))
