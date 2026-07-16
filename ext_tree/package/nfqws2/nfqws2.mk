################################################################################
#
# nfqws2 — DPI bypass utility
#
################################################################################

NFQWS2_VERSION = 1.1.5
NFQWS2_SOURCE = zapret2-v1.0.2-openwrt-embedded.tar.gz
NFQWS2_SITE = https://github.com/bol-van/zapret2/releases/download/v1.0.2
NFQWS2_DL_SUBDIR = nfqws2

define NFQWS2_EXTRACT_CMDS
	$(TAR) -xzf $(DL_DIR)/$(NFQWS2_DL_SUBDIR)/$(NFQWS2_SOURCE) -C $(@D)
endef

define NFQWS2_INSTALL_TARGET_CMDS
	# Install nfqws2 binary (static, ARMv7)
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/bin
	$(INSTALL) -D -m 0755 $(@D)/zapret2-v1.0.2/binaries/linux-arm/nfqws2 \
		$(TARGET_DIR)/usr/bin/nfqws2
	$(INSTALL) -D -m 0755 $(@D)/zapret2-v1.0.2/binaries/linux-arm/ip2net \
		$(TARGET_DIR)/usr/bin/ip2net
	$(INSTALL) -D -m 0755 $(@D)/zapret2-v1.0.2/binaries/linux-arm/mdig \
		$(TARGET_DIR)/usr/bin/mdig

	# Install Lua strategies from zapret2 (decompress .gz files)
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/etc/nfqws2/lua
	cp -r $(@D)/zapret2-v1.0.2/lua/* $(TARGET_DIR)/etc/nfqws2/lua/
	gunzip -f $(TARGET_DIR)/etc/nfqws2/lua/*.gz 2>/dev/null || true

	# Install blob files (fake packet templates) from zapret2
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/etc/nfqws2/blobs
	cp -r $(@D)/zapret2-v1.0.2/files/fake/* $(TARGET_DIR)/etc/nfqws2/blobs/

	# Symlink default blobs so nfqws2 config finds them
	ln -sf quic_initial_www_google_com.bin $(TARGET_DIR)/etc/nfqws2/blobs/quic_initial.bin
	ln -sf tls_clienthello_www_google_com.bin $(TARGET_DIR)/etc/nfqws2/blobs/tls_clienthello.bin

	# Install default config
	$(INSTALL) -D -m 0644 $(NFQWS2_PKGDIR)/nfqws2.conf \
		$(TARGET_DIR)/etc/nfqws2/nfqws2.conf

	# Install lists directory
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/etc/nfqws2/lists
	$(INSTALL) -D -m 0644 $(NFQWS2_PKGDIR)/user.list \
		$(TARGET_DIR)/etc/nfqws2/lists/user.list
	touch $(TARGET_DIR)/etc/nfqws2/lists/auto.list
	$(INSTALL) -D -m 0644 $(NFQWS2_PKGDIR)/exclude.list \
		$(TARGET_DIR)/etc/nfqws2/lists/exclude.list
	touch $(TARGET_DIR)/etc/nfqws2/lists/ipset.list
	$(INSTALL) -D -m 0644 $(NFQWS2_PKGDIR)/ipset_exclude.list \
		$(TARGET_DIR)/etc/nfqws2/lists/ipset_exclude.list

	# Install init script
	$(INSTALL) -D -m 0755 $(NFQWS2_PKGDIR)/S51nfqws2 \
		$(TARGET_DIR)/etc/init.d/S51nfqws2

	# Create log directory
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/var/log
endef

$(eval $(generic-package))
