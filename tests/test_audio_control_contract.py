#!/usr/bin/env python3
"""Regression contracts for PureFox audio-control scripts."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
STATUS_MONITOR = "/etc/init.d/S40statusmonitor"
AUDIO_SCRIPTS = (
    "ext_tree/board/luckfox/rootfs_overlay/opt/2_std.sh",
    "ext_tree/board/luckfox/rootfs_overlay/opt/2_usb.sh",
    "ext_tree/board/luckfox/rootfs_overlay/opt/usb_to_i2s.sh",
    "ext_tree/board/luckfox/rootfs_overlay/opt/usb_unlock.sh",
    "ext_tree/board/luckfox/rootfs_overlay/usr/bin/usb-audio-hotplug.sh",
)


class AudioControlContractTests(unittest.TestCase):
    def test_all_audio_transition_paths_restart_the_installed_status_monitor(self):
        for relative_path in AUDIO_SCRIPTS:
            content = (ROOT / relative_path).read_text()
            with self.subTest(path=relative_path):
                self.assertNotIn("/etc/init.d/S01statusmonitor", content)
                self.assertIn(STATUS_MONITOR, content)

    def test_output_switch_restarts_the_active_player_in_the_transition_script_only(self):
        handler = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/var/www/handle_alsa.php"
        ).read_text()

        self.assertNotIn("/etc/init.d/S95* restart", handler)
        for relative_path in (
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_std.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_usb.sh",
        ):
            with self.subTest(path=relative_path):
                self.assertIn(
                    "/etc/init.d/S95* restart",
                    (ROOT / relative_path).read_text(),
                )

    def test_nonstandard_i2s_profiles_publish_i2s_state_and_refresh_status(self):
        for relative_path in (
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_lr.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_plr.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_8ch.sh",
        ):
            content = (ROOT / relative_path).read_text()
            with self.subTest(path=relative_path):
                self.assertIn("echo I2S > /etc/output", content)
                self.assertIn(STATUS_MONITOR, content)
                self.assertIn("/etc/init.d/S95* restart", content)

        handler = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/var/www/handle_i2s.php"
        ).read_text()
        self.assertIn("Submode change requires I2S output", handler)

    def test_alsa_switch_does_not_report_success_after_a_transition_failure(self):
        handler = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/var/www/handle_alsa.php"
        ).read_text()

        self.assertIn("Output switch failed", handler)
        self.assertIn("http_response_code(500)", handler)

    def test_all_audio_transitions_share_one_lock(self):
        helper_path = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/var/www/audio_transition.php"
        )
        self.assertTrue(helper_path.exists(), "missing shared audio transition helper")
        helper = helper_path.read_text()
        self.assertIn("/tmp/purefox-audio.lock", helper)
        self.assertIn("fopen(PUREFOX_AUDIO_LOCK_FILE, 'ce')", helper)

        for relative_path in (
            "ext_tree/board/luckfox/rootfs_overlay/var/www/handle_service.php",
            "ext_tree/board/luckfox/rootfs_overlay/var/www/handle_alsa.php",
            "ext_tree/board/luckfox/rootfs_overlay/var/www/handle_i2s.php",
            "ext_tree/board/luckfox/rootfs_overlay/var/www/usb_to_i2s.php",
        ):
            with self.subTest(path=relative_path):
                self.assertIn(
                    "require_once 'audio_transition.php'",
                    (ROOT / relative_path).read_text(),
                )

        for relative_path in (
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_std.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_usb.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_lr.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_plr.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_8ch.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2pll.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2ext.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_512_pll.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_512_ext.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_1024_pll.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_1024_ext.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/usb_to_i2s.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/usb_unlock.sh",
            "ext_tree/board/luckfox/rootfs_overlay/usr/bin/usb-audio-hotplug.sh",
        ):
            with self.subTest(path=relative_path):
                self.assertIn(
                    ". /opt/audio-transition-lock.sh",
                    (ROOT / relative_path).read_text(),
                )

    def test_transition_lock_fd_is_closed_before_service_daemons_are_started(self):
        helper = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/opt/audio-transition-lock.sh"
        ).read_text()
        self.assertIn("run_without_audio_lock_fd", helper)
        self.assertIn("exec 9>&-", helper)

        for relative_path in (
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_std.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/2_usb.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/usb_to_i2s.sh",
            "ext_tree/board/luckfox/rootfs_overlay/opt/usb_unlock.sh",
            "ext_tree/board/luckfox/rootfs_overlay/usr/bin/usb-audio-hotplug.sh",
        ):
            with self.subTest(path=relative_path):
                self.assertIn(
                    "run_without_audio_lock_fd",
                    (ROOT / relative_path).read_text(),
                )

    def test_player_switch_confirms_readiness_before_notifying_clients(self):
        handler = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/var/www/handle_service.php"
        ).read_text()

        self.assertIn("$timeoutMs = 5000", handler)
        self.assertLess(
            handler.index("$confirmed = waitForProcess($proc)"),
            handler.index("/opt/dbus_notify ServiceChanged"),
        )

    def test_alsa_cache_is_mutated_only_while_the_transition_lock_is_held(self):
        handler = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/var/www/handle_alsa.php"
        ).read_text()

        self.assertLess(
            handler.index("acquireAudioTransitionLock"),
            handler.index("unlink($cache_file)"),
        )

    def test_usb_hotplug_filters_by_sysfs_bus_instead_of_card_number(self):
        mdev_config = (
            ROOT / "ext_tree/board/luckfox/rootfs_overlay/etc/mdev.conf"
        ).read_text()
        hotplug = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/usr/bin/usb-audio-hotplug.sh"
        ).read_text()

        self.assertIn("card[0-9]+", mdev_config)
        self.assertIn('readlink -f "/sys/class/sound/$MDEV/device"', hotplug)
        self.assertIn('*/usb*', hotplug)

    def test_usb_hotplug_preserves_identity_for_remove_events_after_sysfs_disappears(self):
        hotplug = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/usr/bin/usb-audio-hotplug.sh"
        ).read_text()

        self.assertIn("USB_CARD_STATE=/tmp/usb-audio-card", hotplug)
        self.assertIn('echo "$MDEV" > "$USB_CARD_STATE"', hotplug)
        self.assertIn('cat "$USB_CARD_STATE"', hotplug)

    def test_status_monitor_discovers_the_usb_dac_card_from_sysfs(self):
        source = (
            ROOT
            / "ext_tree/package/status-monitor/src/status_monitor_dbus.c"
        ).read_text()

        self.assertIn("find_usb_audio_card", source)
        self.assertNotIn('stat("/sys/class/sound/card1"', source)
        self.assertIn('"hw:%d"', source)

    def test_php_status_fallback_discovers_usb_dac_from_sysfs(self):
        source = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/var/www/status_fast.php"
        ).read_text()

        self.assertIn("/sys/class/sound/card*/device", source)
        self.assertNotIn("/sys/class/sound/card1", source)

    def test_volume_encoder_discovers_the_usb_dac_card_from_sysfs(self):
        source = (ROOT / "ext_tree/package/volume-encoder/volume-encoder.c").read_text()

        self.assertIn("find_usb_audio_card", source)
        self.assertNotIn('return "hw:1"', source)

    def test_c_usb_card_resolvers_canonicalize_sysfs_device_links(self):
        for relative_path in (
            "ext_tree/package/status-monitor/src/status_monitor_dbus.c",
            "ext_tree/package/volume-encoder/volume-encoder.c",
        ):
            source = (ROOT / relative_path).read_text()
            with self.subTest(path=relative_path):
                self.assertIn("realpath(device_link, device_path)", source)
                self.assertNotIn("readlink(device_link", source)
                self.assertIn("char device_link[PATH_MAX]", source)
                self.assertIn("char device_path[PATH_MAX]", source)

    def test_usb_output_uses_the_discovered_usb_card_for_alsa_and_qobuz(self):
        helper_path = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/opt/audio-card-utils.sh"
        )
        self.assertTrue(helper_path.exists(), "missing dynamic ALSA card helper")
        self.assertIn("find_usb_sound_card", helper_path.read_text())

        usb_switch = (
            ROOT / "ext_tree/board/luckfox/rootfs_overlay/opt/2_usb.sh"
        ).read_text()
        qobuz_service = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/etc/rc.pure/S95qobuz"
        ).read_text()
        self.assertIn("configure_usb_asound", usb_switch)
        self.assertIn("usb_audio_device", qobuz_service)

    def test_usb_mode_remains_selectable_without_a_dac_and_reconfigures_on_hotplug(self):
        helper = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/opt/audio-card-utils.sh"
        ).read_text()
        hotplug = (
            ROOT
            / "ext_tree/board/luckfox/rootfs_overlay/usr/bin/usb-audio-hotplug.sh"
        ).read_text()

        self.assertIn("ln -s /etc/asound.usb /etc/asound.conf", helper)
        self.assertIn("configure_usb_asound", hotplug)
        self.assertIn("grep -qx USB /etc/output", hotplug)


if __name__ == "__main__":
    unittest.main()
