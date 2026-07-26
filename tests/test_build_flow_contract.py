#!/usr/bin/env python3
"""Regression contracts for the PureFox build entrypoints."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BuildFlowContractTests(unittest.TestCase):
    def test_build_entrypoint_is_strict_and_repo_local(self):
        content = (ROOT / "build.sh").read_text()

        self.assertIn("set -eu", content)
        self.assertIn("SCRIPT_DIR=", content)
        self.assertNotIn("git config --global", content)

    def test_build_entrypoint_passes_a_configurable_output_directory_to_make(self):
        content = (ROOT / "build.sh").read_text()

        self.assertIn('OUTPUT_DIR=${O:-"$BUILDROOT_DIR/output"}', content)
        self.assertIn('make O="$OUTPUT_DIR"', content)

    def test_build_entrypoint_keeps_default_builds_incremental(self):
        content = (ROOT / "build.sh").read_text()

        self.assertIn("--clean", content)
        self.assertIn('make O="$OUTPUT_DIR" clean', content)
        self.assertNotIn('rm -rf "$OUTPUT_DIR/target"', content)
        self.assertNotIn('.stamp_target_installed', content)

    def test_build_entrypoint_makes_host_package_installation_explicit(self):
        content = (ROOT / "build.sh").read_text()

        self.assertIn("--bootstrap", content)
        self.assertIn('if [ "$BOOTSTRAP" -eq 1 ]', content)
        self.assertIn('command -v "$CLANG_PATH"', content)

    def test_build_entrypoint_uses_a_whitespace_safe_path_for_buildroot(self):
        content = (ROOT / "build.sh").read_text()

        self.assertIn("BUILDROOT_PATH=", content)
        self.assertIn('PATH="$BUILDROOT_PATH"', content)

    def test_post_build_uses_buildroot_hook_paths_and_checks_boot_layout(self):
        content = (
            ROOT / "ext_tree/board/luckfox/scripts/post-build.sh"
        ).read_text()

        self.assertIn("TARGET_DIR=$1", content)
        self.assertIn('LINUX_DIR="$BUILD_DIR/linux-custom"', content)
        self.assertIn("DTB_OFFSET=3932160", content)
        self.assertIn("zImage overlaps DTB", content)

    def test_post_build_uses_declared_host_squashfs_without_network_access(self):
        post_build = (
            ROOT / "ext_tree/board/luckfox/scripts/post-build.sh"
        ).read_text()
        defconfig = (
            ROOT / "ext_tree/configs/luckfox_pico_max_defconfig"
        ).read_text()

        self.assertIn("BR2_PACKAGE_HOST_SQUASHFS=y", defconfig)
        self.assertIn('"$HOST_DIR/bin/mksquashfs"', post_build)
        self.assertNotIn("wget https://curl.se/ca/cacert.pem", post_build)

    def test_post_build_publishes_boot_image_atomically(self):
        content = (
            ROOT / "ext_tree/board/luckfox/scripts/post-build.sh"
        ).read_text()

        self.assertIn("BOOT_IMAGE_TMP=", content)
        self.assertIn('mv "$BOOT_IMAGE_TMP" "$BINARIES_DIR/boot.img"', content)
        self.assertIn('rm -f "$BOOT_IMAGE_TMP"', content)

    def test_post_image_requires_expected_release_inputs(self):
        content = (
            ROOT / "buildroot/board/luckfox-pico/common/post-image.sh"
        ).read_text()

        self.assertIn('"$BINARIES_DIR/rootfs.ubi"', content)
        self.assertIn('"$BINARIES_DIR/uboot-env.bin"', content)
        self.assertNotIn("2>/dev/null", content)

    def test_build_entrypoint_repairs_missing_uboot_environment_artifact(self):
        content = (ROOT / "build.sh").read_text()

        self.assertIn('"$OUTPUT_DIR/images/uboot-env.bin"', content)
        self.assertIn("host-uboot-tools-rebuild", content)

    def test_build_entrypoint_rebuilds_modified_local_audio_packages(self):
        content = (ROOT / "build.sh").read_text()

        self.assertIn("status-monitor-rebuild", content)
        self.assertIn("volume-encoder-rebuild", content)

    def test_readmes_do_not_reference_deleted_images(self):
        deleted_images = (
            "images/2026-05-24-09-29-43-image.png",
            "images/2026-05-24-10-02-10-image.png",
        )
        for readme in (ROOT / "README.md", ROOT / "README_EN.md"):
            content = readme.read_text()
            for image in deleted_images:
                with self.subTest(readme=readme.name, image=image):
                    self.assertNotIn(image, content)

    def test_post_build_fails_if_required_external_toolchain_strip_is_missing(self):
        content = (
            ROOT / "ext_tree/board/luckfox/scripts/post-build.sh"
        ).read_text()

        self.assertIn("Host strip binary not found", content)
        self.assertNotIn("-exec $STRIP_BIN", content)

    def test_patch_webui_uses_a_repo_relative_overlay_default(self):
        content = (
            ROOT / "ext_tree/board/luckfox/scripts/patch-webui.sh"
        ).read_text()

        self.assertIn("SCRIPT_DIR=", content)
        self.assertIn("$SCRIPT_DIR/../rootfs_overlay", content)
        self.assertNotIn("/mnt/sdb/PureFox_v2", content)

    def test_patch_webui_stages_all_pages_before_publishing(self):
        content = (
            ROOT / "ext_tree/board/luckfox/scripts/patch-webui.sh"
        ).read_text()

        self.assertIn("PUBLISH_PENDING=0", content)
        self.assertIn("rollback_publish", content)
        self.assertIn("stage_and_publish", content)
        self.assertIn("validate_patched_pages", content)

    def test_branch_sync_restores_the_source_branch_after_an_error(self):
        content = (ROOT / "sync_max_to_ultra.sh").read_text()

        self.assertIn("trap cleanup EXIT", content)
        self.assertIn("git checkout \"$SRC_BRANCH\"", content)
        self.assertIn("SYNC_SUCCESS=1", content)
        self.assertIn("git status --porcelain=v1 --untracked-files=all", content)
        self.assertIn('git reset --hard "$DST_HEAD"', content)


if __name__ == "__main__":
    unittest.main()
