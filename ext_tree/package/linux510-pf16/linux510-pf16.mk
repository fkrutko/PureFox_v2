################################################################################
#
# linux510-pf16 — third kernel choice: Linux 5.10 as in PureFox 1.6
#
# Same kernel source and config as linux510, but with the I2S driver, dummy
# codec and device trees of PureFox 1.6 (DSD512 max, the 1.6 clock setup).
# Installed to /data/boot/1.6/ and selected with /opt/kernel.sh 1.6.
#
################################################################################

# Same source tarball as linux510 (this file is included before linux510.mk)
LINUX510_PF16_VERSION = 8eb2a2355deb4335f5814bfa496fe646301fbed0
LINUX510_PF16_SITE = $(call github,ppy2,ale-linux-rv1106,$(LINUX510_PF16_VERSION))
LINUX510_PF16_SOURCE = linux510-$(LINUX510_PF16_VERSION).tar.gz
LINUX510_PF16_DL_SUBDIR = linux510
LINUX510_PF16_LICENSE = GPL-2.0
LINUX510_PF16_DEPENDENCIES = host-bison host-flex

LINUX510_PF16_DTS_DIR = $(BR2_EXTERNAL_ext_tree_PATH)/board/luckfox/dts_pf16
LINUX510_PF16_BOOT_DIR = $(TARGET_DIR)/data/boot/1.6

# "uname -r" tells the two 5.10 builds apart
define LINUX510_PF16_CONFIGURE_CMDS
	sed 's/^CONFIG_LOCALVERSION=.*/CONFIG_LOCALVERSION="-pf16"/' $(LINUX510_CONFIG) > $(@D)/.config
	$(TARGET_MAKE_ENV) $(BR2_MAKE) $(LINUX510_MAKE_FLAGS) HOSTCC="$(HOSTCC_NOCCACHE)" -C $(@D) olddefconfig
endef

define LINUX510_PF16_BUILD_CMDS
	for f in $(LINUX510_PF16_DTS_DIR)/*; do \
		cmp -s $$f $(@D)/arch/arm/boot/dts/$${f##*/} || cp $$f $(@D)/arch/arm/boot/dts/ || exit 1; \
	done
	$(TARGET_MAKE_ENV) $(BR2_MAKE) $(LINUX510_MAKE_FLAGS) -C $(@D) zImage \
		rv1106_ext.dtb rv1106_pll.dtb rv1106_512_ext.dtb
	size=$$(wc -c < $(@D)/arch/arm/boot/zImage); \
	if [ $$size -gt $(LINUX510_KERNEL_MAX) ]; then \
		echo "Kernel 5.10 (1.6) zImage is $$size bytes, U-Boot loads $(LINUX510_KERNEL_MAX)" >&2; \
		exit 1; \
	fi
endef

define LINUX510_PF16_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/arch/arm/boot/zImage $(LINUX510_PF16_BOOT_DIR)/zImage
	$(INSTALL) -D -m 0644 $(@D)/arch/arm/boot/dts/rv1106_ext.dtb $(LINUX510_PF16_BOOT_DIR)/1024_ext.dtb
	$(INSTALL) -D -m 0644 $(@D)/arch/arm/boot/dts/rv1106_pll.dtb $(LINUX510_PF16_BOOT_DIR)/1024_pll.dtb
	$(INSTALL) -D -m 0644 $(@D)/arch/arm/boot/dts/rv1106_512_ext.dtb $(LINUX510_PF16_BOOT_DIR)/512_ext.dtb
endef

$(eval $(generic-package))
