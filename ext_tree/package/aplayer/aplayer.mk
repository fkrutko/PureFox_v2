################################################################################
#
# aplayer
#
################################################################################

APLAYER_SOURCE = aplayer-arm32.tar.gz
APLAYER_SITE = https://albumplayer.ru/linux
APLAYER_DL_SUBDIR = aplayer

define APLAYER_EXTRACT_CMDS
    $(TAR) -xzf $(DL_DIR)/$(APLAYER_DL_SUBDIR)/$(APLAYER_SOURCE) -C $(@D)
endef

define APLAYER_INSTALL_TARGET_CMDS
    $(INSTALL) -d -m 0755 $(TARGET_DIR)/usr
    $(TAR) -xzf $(DL_DIR)/$(APLAYER_DL_SUBDIR)/$(APLAYER_SOURCE) -C $(TARGET_DIR)/usr
endef

# PureFox: блокировка настроек в веб-интерфейсе
define APLAYER_PATCH_WEBUI
    # aplayer.html
    $(SED) 's|id="radio_fm" type="radio" name="pm"|id="radio_fm" type="radio" name="pm" disabled|' $(TARGET_DIR)/usr/aplayer/aplayer.html
    $(SED) 's|id="check_preload" type="checkbox"> Full preloading|id="check_preload" type="checkbox" disabled> Full preloading|' $(TARGET_DIR)/usr/aplayer/aplayer.html
    $(SED) 's|id="mmap" name="mmap" type="radio" checked|id="mmap" name="mmap" type="radio" disabled|' $(TARGET_DIR)/usr/aplayer/aplayer.html
    $(SED) 's|id="rw" name="mmap" type="radio"|id="rw" name="mmap" type="radio" checked|' $(TARGET_DIR)/usr/aplayer/aplayer.html
    $(SED) 's|id="check_memory" type="checkbox" checked>|id="check_memory" type="checkbox" disabled>|' $(TARGET_DIR)/usr/aplayer/aplayer.html
    $(SED) 's|id="cores0" type="radio" name="cores" checked|id="cores0" type="radio" name="cores" disabled|' $(TARGET_DIR)/usr/aplayer/aplayer.html
    $(SED) 's|id="cores2" type="radio" name="cores"|id="cores2" type="radio" name="cores" disabled|' $(TARGET_DIR)/usr/aplayer/aplayer.html
    $(SED) 's|id="CardNum" style="width:24px"|id="CardNum" style="width:24px" disabled|' $(TARGET_DIR)/usr/aplayer/aplayer.html
    # API поля (одна строка с \n — без multi-line)
    $(SED) '/id="radio_pict"/s|(1-10)</td></tr>|(1-10)  \&nbsp;\&nbsp; API cx \&nbsp;<input id="api_cx" style="width:150px" type="text"></td></tr>\n        <tr><td>API key \&nbsp; <input id="api_key" style="width:340px" type="text"></td></tr>|' $(TARGET_DIR)/usr/aplayer/aplayer.html
    # dimas/aplayer.html
    $(SED) 's|id="radio_fm" type="radio" name="pm"|id="radio_fm" type="radio" name="pm" disabled|' $(TARGET_DIR)/usr/aplayer/dimas/aplayer.html
    $(SED) 's|id="check_preload" type="checkbox"|id="check_preload" type="checkbox" disabled|' $(TARGET_DIR)/usr/aplayer/dimas/aplayer.html
    $(SED) 's|id="mmap" name="mmap" type="radio" checked|id="mmap" name="mmap" type="radio" disabled|' $(TARGET_DIR)/usr/aplayer/dimas/aplayer.html
    $(SED) 's|id="rw" name="mmap" type="radio"|id="rw" name="mmap" type="radio" checked|' $(TARGET_DIR)/usr/aplayer/dimas/aplayer.html
    $(SED) 's|id="check_memory" type="checkbox" checked|id="check_memory" type="checkbox" disabled|' $(TARGET_DIR)/usr/aplayer/dimas/aplayer.html
    $(SED) 's|id="cores0" type="radio" name="cores" checked|id="cores0" type="radio" name="cores" disabled|' $(TARGET_DIR)/usr/aplayer/dimas/aplayer.html
    $(SED) 's|id="cores2" type="radio" name="cores"|id="cores2" type="radio" name="cores" disabled|' $(TARGET_DIR)/usr/aplayer/dimas/aplayer.html
    $(SED) 's|id="CardNum" style="width:24px"|id="CardNum" style="width:24px" disabled|' $(TARGET_DIR)/usr/aplayer/dimas/aplayer.html
endef
APLAYER_POST_INSTALL_TARGET_HOOKS += APLAYER_PATCH_WEBUI

$(eval $(generic-package))
