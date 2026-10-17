################################################################################
#
# aprenderer
#
################################################################################

APRENDERER_SOURCE = aprenderer-arm32.tar.gz
APRENDERER_SITE = https://albumplayer.ru/linux
APRENDERER_DL_SUBDIR = aprenderer

define APRENDERER_EXTRACT_CMDS
    $(TAR) -xzf $(DL_DIR)/$(APRENDERER_DL_SUBDIR)/$(APRENDERER_SOURCE) -C $(@D)
endef

define APRENDERER_INSTALL_TARGET_CMDS
    $(INSTALL) -d -m 0755 $(TARGET_DIR)/usr
    $(TAR) -xzf $(DL_DIR)/$(APRENDERER_DL_SUBDIR)/$(APRENDERER_SOURCE) -C $(TARGET_DIR)/usr
endef

# PureFox: блокировка настроек в веб-интерфейсе
define APRENDERER_PATCH_WEBUI
    $(SED) 's|id="radio_fm" type="radio" name="pm"|id="radio_fm" type="radio" name="pm" disabled|' $(TARGET_DIR)/usr/aprenderer/renderer.html
    $(SED) 's|id="check_preload" type="checkbox"> Full preloading|id="check_preload" type="checkbox" disabled> Full preloading|' $(TARGET_DIR)/usr/aprenderer/renderer.html
    $(SED) 's|id="mmap" name="mmap" type="radio" checked|id="mmap" name="mmap" type="radio" disabled|' $(TARGET_DIR)/usr/aprenderer/renderer.html
    $(SED) 's|id="rw" name="mmap" type="radio"|id="rw" name="mmap" type="radio" checked|' $(TARGET_DIR)/usr/aprenderer/renderer.html
    $(SED) 's|id="check_memory" type="checkbox" checked>|id="check_memory" type="checkbox" disabled>|' $(TARGET_DIR)/usr/aprenderer/renderer.html
    $(SED) 's|id="cores0" type="radio" name="cores" checked|id="cores0" type="radio" name="cores" disabled|' $(TARGET_DIR)/usr/aprenderer/renderer.html
    $(SED) 's|id="cores2" type="radio" name="cores"|id="cores2" type="radio" name="cores" disabled|' $(TARGET_DIR)/usr/aprenderer/renderer.html
    $(SED) 's|id="CardNum" style="width:24px"|id="CardNum" style="width:24px" disabled|' $(TARGET_DIR)/usr/aprenderer/renderer.html
endef
APRENDERER_POST_INSTALL_TARGET_HOOKS += APRENDERER_PATCH_WEBUI

$(eval $(generic-package))
