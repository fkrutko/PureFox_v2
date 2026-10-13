################################################################################
#
# linux510 — second kernel: Rockchip BSP Linux 5.10 (as in PureFox 1.x)
#
# Built next to the main 6.1 kernel. Its zImage and DTBs are installed to
# /data/boot/5.10/ and written to the boot partition by /opt/kernel.sh when
# the user selects kernel 5.10. The I2S driver and the dummy codec are copied
# from the patched 6.1 tree, so both kernels run the same driver code.
#
################################################################################

LINUX510_VERSION = 8eb2a2355deb4335f5814bfa496fe646301fbed0
LINUX510_SITE = $(call github,ppy2,ale-linux-rv1106,$(LINUX510_VERSION))
LINUX510_LICENSE = GPL-2.0
LINUX510_DEPENDENCIES = linux host-bison host-flex

LINUX510_CONFIG = $(BR2_EXTERNAL_ext_tree_PATH)/board/luckfox/config/linux510.config
LINUX510_DTS_DIR = $(BR2_EXTERNAL_ext_tree_PATH)/board/luckfox/dts_max
LINUX510_BOOT_DIR = $(TARGET_DIR)/data/boot/5.10

# Driver files shared with the 6.1 kernel (version guards in the source)
LINUX510_SHARED_FILES = \
	sound/soc/rockchip/rockchip_i2s_tdm.c \
	sound/soc/rockchip/rockchip_i2s_tdm.h \
	sound/soc/codecs/dummy-codec.c

# U-Boot reads 0x360000 bytes of kernel from the boot partition (bootcmd)
LINUX510_KERNEL_MAX = 3538944

LINUX510_MAKE_FLAGS = \
	HOSTCC="$(HOSTCC) $(subst -I/,-isystem /,$(subst -I /,-isystem /,$(HOST_CFLAGS))) $(HOST_LDFLAGS)" \
	ARCH=arm \
	KCFLAGS="-Wno-attribute-alias" \
	CROSS_COMPILE="$(TARGET_CROSS)" \
	REGENERATE_PARSERS=1

define LINUX510_CONFIGURE_CMDS
	cp $(LINUX510_CONFIG) $(@D)/.config
	$(TARGET_MAKE_ENV) $(BR2_MAKE) $(LINUX510_MAKE_FLAGS) HOSTCC="$(HOSTCC_NOCCACHE)" -C $(@D) olddefconfig
endef

# Copy only what changed, so an unchanged driver is not rebuilt.
# The USB controller runs in host mode, as in PureFox 1.x: the 6.1 DTS sets
# "otg" for the USBtoI2S gadget, which this kernel does not support.
define LINUX510_BUILD_CMDS
	for f in $(LINUX510_SHARED_FILES); do \
		cmp -s $(LINUX_DIR)/$$f $(@D)/$$f || cp $(LINUX_DIR)/$$f $(@D)/$$f || exit 1; \
	done
	for f in $(LINUX510_DTS_DIR)/*; do \
		dst=$(@D)/arch/arm/boot/dts/$${f##*/}; \
		sed 's/dr_mode = "otg";/dr_mode = "host";/' $$f > $$dst.tmp || exit 1; \
		if cmp -s $$dst.tmp $$dst; then rm $$dst.tmp; else mv $$dst.tmp $$dst; fi; \
	done
	$(TARGET_MAKE_ENV) $(BR2_MAKE) $(LINUX510_MAKE_FLAGS) -C $(@D) zImage \
		rv1106_ext.dtb rv1106_pll.dtb rv1106_512_ext.dtb
	size=$$(wc -c < $(@D)/arch/arm/boot/zImage); \
	if [ $$size -gt $(LINUX510_KERNEL_MAX) ]; then \
		echo "Kernel 5.10 zImage is $$size bytes, U-Boot loads $(LINUX510_KERNEL_MAX)" >&2; \
		exit 1; \
	fi
endef

define LINUX510_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/arch/arm/boot/zImage $(LINUX510_BOOT_DIR)/zImage
	$(INSTALL) -D -m 0644 $(@D)/arch/arm/boot/dts/rv1106_ext.dtb $(LINUX510_BOOT_DIR)/1024_ext.dtb
	$(INSTALL) -D -m 0644 $(@D)/arch/arm/boot/dts/rv1106_pll.dtb $(LINUX510_BOOT_DIR)/1024_pll.dtb
	$(INSTALL) -D -m 0644 $(@D)/arch/arm/boot/dts/rv1106_512_ext.dtb $(LINUX510_BOOT_DIR)/512_ext.dtb
endef

$(eval $(generic-package))
