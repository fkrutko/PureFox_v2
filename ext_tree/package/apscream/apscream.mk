################################################################################
#
# apscream
#
################################################################################

# asioscream4.zip is replaced in place upstream: the hash pins the receiver
# version, update both when a new one comes out.
APSCREAM_VERSION = 2.9
APSCREAM_SITE = https://albumplayer.ru
APSCREAM_SOURCE = asioscream4.zip

define APSCREAM_EXTRACT_CMDS
	$(UNZIP) -j $(APSCREAM_DL_DIR)/$(APSCREAM_SOURCE) \
		"asioscream4/LinuxReceiver/Arm32/apscream-arm32.tar.gz" -d $(@D)
	$(TAR) -xf $(@D)/apscream-arm32.tar.gz -C $(@D)
endef

define APSCREAM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/apscream $(TARGET_DIR)/usr/apscream/apscream
	$(INSTALL) -D -m 0644 $(@D)/config.txt $(TARGET_DIR)/usr/apscream/config.txt
endef

$(eval $(generic-package))
