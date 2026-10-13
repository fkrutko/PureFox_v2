################################################################################
#
# scream
#
################################################################################

SCREAM_VERSION = 4.0
SCREAM_SITE = $(call github,duncanthrax,scream,$(SCREAM_VERSION))
SCREAM_LICENSE = MS-PL
SCREAM_LICENSE_FILES = LICENSE
SCREAM_SUBDIR = Receivers/unix
SCREAM_DEPENDENCIES = alsa-lib host-pkgconf

# ALSA output only: PulseAudio, JACK and pcap are not used on the device
SCREAM_CONF_OPTS = \
	-DALSA_ENABLE=ON \
	-DPULSEAUDIO_ENABLE=OFF \
	-DJACK_ENABLE=OFF \
	-DPCAP_ENABLE=OFF

$(eval $(cmake-package))
