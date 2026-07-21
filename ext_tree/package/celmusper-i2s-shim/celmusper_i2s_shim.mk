################################################################################
#
# celmusper-i2s-shim
#
################################################################################

CELMUSPER_I2S_SHIM_VERSION = 1.0
CELMUSPER_I2S_SHIM_SITE = $(BR2_EXTERNAL_ext_tree_PATH)/package/celmusper-i2s-shim
CELMUSPER_I2S_SHIM_SITE_METHOD = local
CELMUSPER_I2S_SHIM_LICENSE = MIT
CELMUSPER_I2S_SHIM_DEPENDENCIES = alsa-lib

define CELMUSPER_I2S_SHIM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -shared -fPIC -Wall -Wextra -Werror \
		-o $(@D)/libcelmusper_i2s.so $(@D)/celmusper_i2s_shim.c \
		-ldl -lasound
endef

define CELMUSPER_I2S_SHIM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 755 $(@D)/libcelmusper_i2s.so \
		$(TARGET_DIR)/usr/lib/libcelmusper_i2s.so
endef

$(eval $(generic-package))
