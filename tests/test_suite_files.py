"""Frame Dock's service, desktop entry and install scripts.

The scripts change the user's systemd units and SteamVR, so they are read, parsed (sh -n), and run only against a
fake home with fake systemctl / steamvr / pkill on PATH (InstallBehaviourTests): nothing outside a temporary folder
is touched, and no real service or SteamVR is started, stopped or restarted.
"""

from pathlib import Path
import configparser
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CONTRIB = ROOT / "contrib"
# FrameColor's own install script before the merge: the layer files must be written as it wrote them
ORIGINAL_INSTALL = ("3f0c894", "contrib/install.sh")


def read(name):
    return (CONTRIB / name).read_text(encoding="utf-8")


class DockFilesTests(unittest.TestCase):
    def test_scripts_parse(self):
        for name in ("install.sh", "uninstall.sh", "package.sh", "framedockctl", "framecolor-guard"):
            with self.subTest(name=name):
                subprocess.run(["sh", "-n", str(CONTRIB / name)], check=True)

    def test_service_is_modelled_on_perf(self):
        unit = configparser.ConfigParser(strict=False, interpolation=None)
        unit.optionxform = str
        unit.read_string(read("framedock.service"))
        self.assertEqual(unit["Unit"]["After"], "steamvr.service")
        self.assertEqual(unit["Unit"]["PartOf"], "steamvr.service")
        # A crash at every start doesn't loop forever (with RestartSec=5 systemd's default limit never trips)
        self.assertEqual(unit["Unit"]["StartLimitIntervalSec"], "300")
        self.assertEqual(unit["Unit"]["StartLimitBurst"], "10")
        self.assertEqual(unit["Service"]["ExecStart"], "%h/.local/bin/framedock")
        self.assertEqual(unit["Service"]["Restart"], "always")
        self.assertEqual(unit["Service"]["RestartPreventExitStatus"], "3")
        self.assertEqual(unit["Install"]["WantedBy"], "steamvr.service")

    def test_autostart_and_lock_use_the_same_name(self):
        autostart = (ROOT / "perf/headset/autostart.h").read_text(encoding="utf-8")
        self.assertIn('kUnitName = "framedock.service"', autostart)
        paths = (ROOT / "perf/headset/paths.h").read_text(encoding="utf-8")
        self.assertIn('kAppName = "framedock"', paths)

    def test_desktop_entry(self):
        desktop = read("framedock.desktop")
        self.assertIn("Exec=@BINARY@", desktop)
        self.assertIn("Icon=framedock", desktop)
        for size in (48, 128, 256):
            self.assertTrue((CONTRIB / "icons" / f"framedock-{size}.png").is_file())
        validate = shutil.which("desktop-file-validate")
        if validate:
            subprocess.run([validate, str(CONTRIB / "framedock.desktop")], check=True)

    def test_install_migrates_both_apps(self):
        install = read("install.sh")
        self.assertIn("for fs_old in framecolor frame-perf-overlay; do", install)
        self.assertIn('systemctl --user disable --now "$fs_old.service"', install)
        for path in ('"$fs_unit_dir/framecolor.service"', '"$fs_bin_dir/framecolor"',
                     '"$fs_unit_dir/frame-perf-overlay.service"', '"$fs_bin_dir/frame-perf-overlay"',
                     '"$fs_data_root/applications/frame-perf-overlay.desktop"'):
            with self.subTest(path=path):
                self.assertIn(path, install)
        # The config directories stay: they are what Frame Dock reads
        self.assertNotIn('rm -r', install)
        self.assertNotIn('"$fc_config_dir/current.txt" "$fc_config_dir/presets.txt"', install)

    def test_layer_install_matches_framecolor(self):
        install = read("install.sh")
        for line in ('"$fc_layer_binary" "$fs_stage/libvkbasalt.so"',
                     '"$fs_unit_dir/steamvr.service.d/90-framecolor.conf"',
                     "printf '0 0 0 1 1 1 1 1 1 1 1 1 0 0 0\\n'",
                     'generate_vulkan_lut.py',
                     "VK_ADD_IMPLICIT_LAYER_PATH=%s/layer",
                     "ENABLE_VKBASALT=1"):
            with self.subTest(line=line):
                self.assertIn(line, install)

    def test_layer_manifest_matches_the_original_install_script(self):
        # The manifest, vkBasalt.conf and vulkan.env as FrameColor's install.sh wrote them (only the folder the
        # files are first written to differs: the staging folder)
        try:
            original = subprocess.run(["git", "-C", str(ROOT), "show", ":".join(ORIGINAL_INSTALL)], check=True,
                                      capture_output=True, text=True).stdout
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("the original install.sh is not in this checkout's history")
        install = read("install.sh")

        def manifest_block(text):
            match = re.search(r"^manifest = \{\n.*?^\}\n", text, re.S | re.M)
            self.assertIsNotNone(match)
            return match.group(0)

        self.assertEqual(manifest_block(install), manifest_block(original))
        self.assertIn('json.dumps(manifest, indent=2) + "\\n"', install)
        self.assertIn("f\"lutFile = {root / 'framecolor.cube'}\\n\"", install)
        for line in ("VK_ADD_IMPLICIT_LAYER_PATH=%s/layer\\n", "ENABLE_VKBASALT=1\\n",
                     "VKBASALT_CONFIG_FILE=%s/vkBasalt.conf\\n", "VKBASALT_LOG_LEVEL=warn\\n"):
            with self.subTest(line=line):
                self.assertIn(line, original)
                self.assertIn(line, install)

    def test_framedockctl_runs_framedock(self):
        ctl = read("framedockctl")
        self.assertIn('fc_app="$HOME/.local/bin/framedock"', ctl)
        self.assertIn('exec "$fc_app" --bench "$fc_seconds"', ctl)
        self.assertIn('exec "$fc_app" --bench-compare "$3" "$4" "$fc_pairs"', ctl)
        self.assertNotIn("framecolor.service", ctl)
        # enable turns the layer on; autostart is the dashboard's switch
        self.assertNotIn("systemctl --user enable", ctl)

    def test_bench_runs_before_the_signal_handlers(self):
        # The overlay's SIGINT/SIGTERM handlers only set a flag the bench never reads: plain --bench must run with the
        # default handling (Ctrl-C ends it), --bench-compare installs its own (they restore current.txt)
        main = (ROOT / "perf/headset/main.cpp").read_text(encoding="utf-8")
        body = main[main.index("int main(int argc, char** argv) {"):]
        self.assertLess(body.index("framecolor::runBenchCommand(argc, argv)"), body.index("installSignalHandlers();"))
        bench = (ROOT / "src/bench.cpp").read_text(encoding="utf-8")
        self.assertIn("std::signal(s, compareStopHandler)", bench)

    def test_no_updater_left(self):
        # Perf's self-updater is gone. config.json's "update_check" key now turns the notify-only new version check
        # (version_notice.h, 2026-11-14) on and off, and install.sh deletes the old frame-update.sh, so only the
        # updater's code is looked for here
        self.assertFalse((ROOT / "perf/vendor").exists())
        self.assertFalse((ROOT / "perf/install.sh").exists())
        for path in list((ROOT / "perf/headset").glob("*")) + list((ROOT / "src").glob("*")) + \
                [CONTRIB / "install.sh", ROOT / "CMakeLists.txt"]:
            with self.subTest(path=path.name):
                text = path.read_text(encoding="utf-8").lower()
                self.assertNotIn("frame-updater", text)
                self.assertNotIn("update_check.h", text)
                self.assertNotIn("preview-update", text)


# A stand-in for `systemctl --user`: unit states are files under $FS_TEST_STATE/{enabled,active}, every call is
# logged to $FS_TEST_STATE/systemctl.log
FAKE_SYSTEMCTL = """\
import os, sys
from pathlib import Path
state = Path(os.environ["FS_TEST_STATE"])
args = sys.argv[1:]
with open(state / "systemctl.log", "a") as log:
    log.write(" ".join(args) + "\\n")
if args and args[0] == "--user":
    args = args[1:]
command, rest = args[0], args[1:]
quiet = "--quiet" in rest
now = "--now" in rest
units = [a for a in rest if not a.startswith("-") and a not in ("MainPID",)]
enabled, active = state / "enabled", state / "active"
def has(folder, unit):
    return (folder / unit).exists()
if command in ("is-enabled", "is-active"):
    folder = enabled if command == "is-enabled" else active
    ok = all(has(folder, u) for u in units)
    if not quiet:
        print(("enabled" if ok else "disabled") if command == "is-enabled" else ("active" if ok else "inactive"))
    sys.exit(0 if ok else 1)
for unit in units:
    if command == "enable":
        (enabled / unit).touch()
        if now: (active / unit).touch()
    elif command == "disable":
        (enabled / unit).unlink(missing_ok=True)
        if now: (active / unit).unlink(missing_ok=True)
    elif command in ("start", "restart"):
        (active / unit).touch()
    elif command == "stop":
        (active / unit).unlink(missing_ok=True)
if command == "show":
    print("0")
"""


@unittest.skipUnless(Path("/opt/steamvr").is_dir() and shutil.which("cmp"),
                     "install.sh runs only where /opt/steamvr exists (the Frame)")
class InstallBehaviourTests(unittest.TestCase):
    """install.sh / uninstall.sh run from a fake release archive into a fake home."""

    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="framedock-install-test-"))
        self.addCleanup(shutil.rmtree, self.temp)
        self.home = self.temp / "home"
        self.state = self.temp / "state"
        self.fakebin = self.temp / "fakebin"
        self.release = self.temp / "release"
        for folder in (self.home, self.state / "enabled", self.state / "active", self.fakebin, self.temp / "run"):
            folder.mkdir(parents=True)
        # The release archive's layout (see package.sh), with stand-ins for the two binaries
        shutil.copytree(CONTRIB, self.release / "contrib")
        (self.release / "tools").mkdir()
        shutil.copy(ROOT / "tools/generate_vulkan_lut.py", self.release / "tools")
        self.write_exec(self.release / "bin/framedock", "#!/bin/sh\necho 'framedock 0.1.0'\n")
        self.set_layer_library(b"layer build 1\n")
        self.write_exec(self.fakebin / "systemctl", f"#!{sys.executable}\n" + FAKE_SYSTEMCTL)
        self.write_exec(self.fakebin / "steamvr", '#!/bin/sh\n[ "$1" = path ] && echo /opt/steamvr\n')
        for name in ("pkill", "update-desktop-database", "gtk-update-icon-cache"):
            self.write_exec(self.fakebin / name, f'#!/bin/sh\necho "{name} $*" >> "$FS_TEST_STATE/other.log"\n')
        if platform.machine() != "aarch64":
            self.write_exec(self.fakebin / "uname", "#!/bin/sh\necho aarch64\n")
        self.env = dict(os.environ, HOME=str(self.home), XDG_RUNTIME_DIR=str(self.temp / "run"),
                        FS_TEST_STATE=str(self.state), PATH=f"{self.fakebin}:{os.environ['PATH']}")
        self.env.pop("XDG_CONFIG_HOME", None)
        self.env.pop("XDG_DATA_HOME", None)

    # ---- helpers ----
    @staticmethod
    def write_exec(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(path.stat().st_mode | stat.S_IXUSR)

    def set_layer_library(self, data):
        (self.release / "lib").mkdir(exist_ok=True)
        (self.release / "lib/libvkbasalt.so").write_bytes(data)

    def h(self, relative):
        return self.home / relative

    def unit_state(self, unit, enabled=None, active=None):
        for folder, on in (("enabled", enabled), ("active", active)):
            if on is not None:
                path = self.state / folder / unit
                path.touch() if on else path.unlink(missing_ok=True)

    def is_enabled(self, unit):
        return (self.state / "enabled" / unit).exists()

    def log(self):
        path = self.state / "systemctl.log"
        return path.read_text().splitlines() if path.exists() else []

    def clear_log(self):
        (self.state / "systemctl.log").unlink(missing_ok=True)

    def run_script(self, name, *args, check=True):
        result = subprocess.run(["sh", str(self.release / "contrib" / name), *args], env=self.env,
                                capture_output=True, text=True)
        if check and result.returncode != 0:
            self.fail(f"{name} {' '.join(args)} failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
        return result

    def old_apps(self, framecolor_enabled, perf_enabled):
        """Both old apps installed, as their own install scripts left them."""
        for name, enabled in (("framecolor", framecolor_enabled), ("frame-perf-overlay", perf_enabled)):
            self.h(".config/systemd/user").mkdir(parents=True, exist_ok=True)
            self.h(f".config/systemd/user/{name}.service").write_text("[Service]\n")
            self.write_exec(self.h(f".local/bin/{name}"), "#!/bin/sh\n")
            self.unit_state(f"{name}.service", enabled=enabled, active=enabled)
        # FrameColor's control script, which framedockctl replaces
        self.write_exec(self.h(".local/bin/framecolorctl"), "#!/bin/sh\n# the old one\n")

    def enable_layer(self):
        manifest = self.h(".local/share/framecolor/layer/vkBasalt.json")
        (manifest.parent / "vkBasalt.json.disabled").rename(manifest)

    # ---- tests ----
    def test_fresh_install_replaces_both_apps(self):
        self.old_apps(framecolor_enabled=False, perf_enabled=True)
        self.unit_state("steamvr.service", active=True)
        out = self.run_script("install.sh", "--replace-old-apps").stdout
        for gone in (".config/systemd/user/framecolor.service", ".local/bin/framecolor",
                     ".config/systemd/user/frame-perf-overlay.service", ".local/bin/frame-perf-overlay",
                     ".local/bin/framecolorctl"):
            self.assertFalse(self.h(gone).exists(), gone)
        self.assertTrue(os.access(self.h(".local/bin/framedock"), os.X_OK))
        self.assertIn("framedock", self.h(".local/bin/framedockctl").read_text())
        self.assertTrue(self.h(".local/share/applications/framedock.desktop").is_file())
        self.assertTrue(self.h(".local/share/icons/hicolor/256x256/apps/framedock.png").is_file())
        # Perf started with SteamVR, so Frame Dock does
        self.assertTrue(self.is_enabled("framedock.service"))
        # A new layer is installed disabled: no SteamVR restart, the colour stays off until framedockctl enable
        self.assertTrue(self.h(".local/share/framecolor/layer/vkBasalt.json.disabled").is_file())
        self.assertFalse(self.h(".local/share/framecolor/layer/vkBasalt.json").exists())
        self.assertNotIn("--user restart steamvr.service", self.log())
        self.assertIn("framedockctl enable", out)
        self.assertEqual(self.h(".config/framecolor/current.txt").read_text(), "0 0 0 1 1 1 1 1 1 1 1 1 0 0 0\n")
        # Nothing left of the staging folder
        self.assertEqual([p.name for p in self.h(".local/share").iterdir() if p.name.startswith(".framedock")], [])

    def test_old_apps_are_kept_unless_the_user_says_so(self):
        # No terminal to ask and no --replace-old-apps: both stay installed but no longer start with SteamVR (they
        # share Frame Dock's settings file), and the script says how to remove them
        self.old_apps(framecolor_enabled=True, perf_enabled=True)
        self.unit_state("steamvr.service", active=True)
        out = self.run_script("install.sh").stdout
        for kept in (".config/systemd/user/framecolor.service", ".local/bin/framecolor",
                     ".config/systemd/user/frame-perf-overlay.service", ".local/bin/frame-perf-overlay",
                     ".local/bin/framecolorctl"):
            self.assertTrue(self.h(kept).exists(), kept)
        self.assertFalse(self.is_enabled("framecolor.service") or self.is_enabled("frame-perf-overlay.service"))
        self.assertTrue([line for line in self.log() if "disable --now framecolor" in line])
        self.assertIn("Kept: framecolor frame-perf-overlay", out)
        self.assertIn("--replace-old-apps", out)
        self.assertTrue(os.access(self.h(".local/bin/framedock"), os.X_OK))
        # The option then removes them
        self.run_script("install.sh", "--replace-old-apps")
        self.assertFalse(self.h(".local/bin/framecolor").exists())
        self.assertFalse(self.h(".config/systemd/user/frame-perf-overlay.service").exists())

    def test_no_frame_suite_migration_left(self):
        for name in ("install.sh", "uninstall.sh"):
            with self.subTest(script=name):
                self.assertNotIn("frame-suite", read(name))

    def test_autostart_follows_the_old_apps(self):
        cases = ((False, False, False), (True, False, True), (False, True, True), (True, True, True))
        for framecolor_enabled, perf_enabled, expected in cases:
            with self.subTest(framecolor=framecolor_enabled, perf=perf_enabled):
                self.setUp()
                self.old_apps(framecolor_enabled, perf_enabled)
                self.run_script("install.sh")
                self.assertEqual(self.is_enabled("framedock.service"), expected)
                # A reinstall keeps whatever it is now
                self.run_script("install.sh")
                self.assertEqual(self.is_enabled("framedock.service"), expected)
        # Without the old apps: on, unless --no-autostart
        self.setUp()
        self.run_script("install.sh")
        self.assertTrue(self.is_enabled("framedock.service"))
        self.setUp()
        self.run_script("install.sh", "--no-autostart")
        self.assertFalse(self.is_enabled("framedock.service"))

    def test_no_restart_leaves_steamvr_running_and_says_how(self):
        self.unit_state("steamvr.service", active=True)
        self.run_script("install.sh")
        self.enable_layer()
        self.set_layer_library(b"layer build 2\n")
        self.clear_log()
        out = self.run_script("install.sh", "--no-restart").stdout
        self.assertNotIn("--user restart steamvr.service", self.log())
        self.assertIn("--user restart framedock.service", self.log())
        self.assertIn("systemctl --user restart steamvr.service", out)
        self.assertEqual(self.h(".local/share/framecolor/lib/libvkbasalt.so").read_bytes(), b"layer build 2\n")
        # Without it, a clear warning comes before the restart
        self.set_layer_library(b"layer build 3\n")
        self.clear_log()
        out = self.run_script("install.sh").stdout
        self.assertIn("WARNING: restarting SteamVR now", out)
        self.assertEqual(self.log().count("--user restart steamvr.service"), 1)

    def test_home_with_sed_special_characters(self):
        # The desktop entry's Exec is the binary's absolute path, whatever characters the home folder's name has
        odd = self.temp / "odd|home&x\\y"
        odd.mkdir()
        self.home = odd
        self.env["HOME"] = str(odd)
        self.run_script("install.sh", "--app-only")
        desktop = self.h(".local/share/applications/framedock.desktop").read_text()
        self.assertIn("Exec=" + str(odd / ".local/bin/framedock"), desktop)

    def test_switch_over_ignores_hup_and_int(self):
        text = read("install.sh")
        ignore = text.index("trap '' HUP INT")
        self.assertLess(ignore, text.index('systemctl --user disable --now "$fs_old.service"'))
        restore = text.index("trap 'exit 1' HUP INT", ignore)
        self.assertGreater(restore, text.index('put "$fs_stage/vkBasalt.json" "$fc_manifest_target"'))
        self.assertLess(restore, text.index("systemctl --user daemon-reload"))

    def test_reinstall_restarts_steamvr_only_for_a_changed_enabled_layer(self):
        self.unit_state("steamvr.service", active=True)
        self.run_script("install.sh")
        self.enable_layer()
        self.clear_log()
        # Unchanged layer: the app is restarted, SteamVR isn't
        self.run_script("install.sh")
        self.assertNotIn("--user restart steamvr.service", self.log())
        self.assertIn("--user restart framedock.service", self.log())
        # After its failed state is cleared: a start limit hit by an earlier bad build doesn't keep it from starting
        self.assertLess(self.log().index("--user reset-failed framedock.service"),
                        self.log().index("--user restart framedock.service"))
        self.assertTrue(self.h(".local/share/framecolor/layer/vkBasalt.json").is_file())
        self.assertFalse(self.h(".local/share/framecolor/layer/vkBasalt.json.disabled").exists())
        # A new layer build: SteamVR restarts once
        self.set_layer_library(b"layer build 2\n")
        self.clear_log()
        self.run_script("install.sh")
        self.assertEqual(self.log().count("--user restart steamvr.service"), 1)
        self.assertEqual(self.h(".local/share/framecolor/lib/libvkbasalt.so").read_bytes(), b"layer build 2\n")
        # SteamVR not running: nothing to restart
        self.set_layer_library(b"layer build 3\n")
        self.unit_state("steamvr.service", active=False)
        self.clear_log()
        self.run_script("install.sh")
        self.assertNotIn("--user restart steamvr.service", self.log())
        # A changed but disabled layer: no restart either
        self.unit_state("steamvr.service", active=True)
        manifest = self.h(".local/share/framecolor/layer/vkBasalt.json")
        manifest.rename(manifest.with_name("vkBasalt.json.disabled"))
        self.set_layer_library(b"layer build 4\n")
        self.clear_log()
        self.run_script("install.sh")
        self.assertNotIn("--user restart steamvr.service", self.log())

    def test_app_only_install_leaves_the_layer_alone(self):
        self.unit_state("steamvr.service", active=True)
        self.run_script("install.sh")
        self.enable_layer()
        library = self.h(".local/share/framecolor/lib/libvkbasalt.so")
        self.set_layer_library(b"layer build 2\n")
        self.clear_log()
        out = self.run_script("install.sh", "--app-only").stdout
        self.assertEqual(library.read_bytes(), b"layer build 1\n")
        self.assertNotIn("--user restart steamvr.service", self.log())
        self.assertIn("--app-only", out)
        self.assertTrue(self.h(".local/bin/framedockctl").is_file())
        self.assertTrue(self.h(".local/share/applications/framedock.desktop").is_file())

    def test_uninstall_removes_the_grip_manifest_under_xdg_data_home(self):
        # The app writes it under $XDG_DATA_HOME when that is absolute (dock_manifest.cpp), else ~/.local/share
        self.run_script("install.sh")
        data = self.temp / "xdg-data"
        manifest = data / "framedock/input"
        manifest.mkdir(parents=True)
        (manifest / "actions.json").write_text("{}\n")
        (manifest / "bindings_frame_controller.json").write_text("{}\n")
        (data / "keep.txt").write_text("someone else's\n")
        self.env["XDG_DATA_HOME"] = str(data)
        self.run_script("uninstall.sh", "--app-only")
        self.assertFalse((data / "framedock").exists())
        self.assertTrue((data / "keep.txt").is_file())
        # A relative XDG_DATA_HOME is ignored, as the app ignores it: ~/.local/share
        home_manifest = self.h(".local/share/framedock/input")
        home_manifest.mkdir(parents=True)
        (home_manifest / "actions.json").write_text("{}\n")
        self.env["XDG_DATA_HOME"] = "relative/data"
        self.run_script("uninstall.sh", "--app-only")
        self.assertFalse(self.h(".local/share/framedock").exists())

    def test_uninstall_app_only_keeps_the_layer_and_framedockctl(self):
        self.unit_state("steamvr.service", active=True)
        self.run_script("install.sh")
        self.enable_layer()
        # What the app writes at start: SteamVR Input's manifest for the Dock's grip
        manifest = self.h(".local/share/framedock/input")
        manifest.mkdir(parents=True)
        (manifest / "actions.json").write_text("{}\n")
        (manifest / "bindings_frame_controller.json").write_text("{}\n")
        self.clear_log()
        out = self.run_script("uninstall.sh", "--app-only").stdout
        for gone in (".local/bin/framedock", ".config/systemd/user/framedock.service",
                     ".local/share/applications/framedock.desktop", ".local/share/framedock"):
            self.assertFalse(self.h(gone).exists(), gone)
        for kept in (".local/bin/framedockctl", ".local/libexec/framecolor-guard",
                     ".config/systemd/user/steamvr.service.d/90-framecolor.conf",
                     ".local/share/framecolor/layer/vkBasalt.json", ".local/share/framecolor/lib/libvkbasalt.so",
                     ".config/framecolor/vulkan.env"):
            self.assertTrue(self.h(kept).exists(), kept)
        self.assertNotIn("--user restart steamvr.service", self.log())
        self.assertIn("framedockctl disable", out)
        # The kept ctl still turns the layer off
        self.clear_log()
        subprocess.run(["sh", str(self.h(".local/bin/framedockctl")), "disable"], env=self.env, check=True,
                       capture_output=True)
        self.assertTrue(self.h(".local/share/framecolor/layer/vkBasalt.json.disabled").is_file())
        self.assertEqual(self.log(), ["--user restart steamvr.service"])

    def test_uninstall_keeps_the_browser_profile_and_names_it(self):
        self.run_script("install.sh", "--app-only")
        # What the browser panel writes: its own Chrome profile and log, next to the grip manifest
        profile = self.h(".local/share/framedock/browser/profile")
        profile.mkdir(parents=True)
        (profile / "Cookies").write_text("x\n")
        (self.h(".local/share/framedock/input")).mkdir(parents=True)
        (self.h(".local/share/framedock/input/actions.json")).write_text("{}\n")
        out = self.run_script("uninstall.sh", "--app-only").stdout
        self.assertTrue((profile / "Cookies").is_file())
        self.assertFalse(self.h(".local/share/framedock/input").exists())
        self.assertIn('rm -r "%s"' % self.h(".local/share/framedock/browser"), out)

    def test_uninstall_restarts_only_a_running_steamvr_with_the_layer_enabled(self):
        for steamvr_active, layer_enabled, expected in ((False, True, 0), (True, False, 0), (True, True, 1)):
            with self.subTest(steamvr_active=steamvr_active, layer_enabled=layer_enabled):
                self.setUp()
                self.run_script("install.sh")
                if layer_enabled:
                    self.enable_layer()
                self.unit_state("steamvr.service", active=steamvr_active)
                self.clear_log()
                self.run_script("uninstall.sh")
                self.assertEqual(self.log().count("--user restart steamvr.service"), expected)
                for gone in (".local/bin/framedock", ".local/bin/framedockctl", ".local/share/framecolor",
                             ".config/systemd/user/steamvr.service.d/90-framecolor.conf"):
                    self.assertFalse(self.h(gone).exists(), gone)
                self.assertTrue(self.h(".config/framecolor/current.txt").is_file())  # settings stay

    def test_a_damaged_current_txt_doesnt_stop_the_install(self):
        # Out-of-range or unreadable values: the LUT is made from clamped (or neutral) values, with a warning
        self.h(".config/framecolor").mkdir(parents=True)
        self.h(".config/framecolor/current.txt").write_text("not numbers\n")
        result = self.run_script("install.sh")
        self.assertIn("neutral look", result.stderr)
        self.assertTrue(self.h(".local/share/framecolor/framecolor.cube").is_file())
        self.h(".config/framecolor/current.txt").write_text("9 9 9 9 9 9 9 9 9 9 9 9 9 9 9\n")
        result = self.run_script("install.sh")
        self.assertIn("clamped", result.stderr)

    def test_a_failure_before_the_switch_over_keeps_the_old_apps(self):
        self.old_apps(framecolor_enabled=True, perf_enabled=True)
        self.h(".config/framecolor").mkdir(parents=True)
        self.h(".config/framecolor/current.txt").write_text("0 0 0 1 1 1 1 1 1 1 1 1 0 0 0\n")
        (self.release / "tools/generate_vulkan_lut.py").unlink()  # the LUT can't be made
        result = self.run_script("install.sh", check=False)
        self.assertNotEqual(result.returncode, 0)
        for kept in (".config/systemd/user/framecolor.service", ".local/bin/framecolor",
                     ".config/systemd/user/frame-perf-overlay.service", ".local/bin/frame-perf-overlay"):
            self.assertTrue(self.h(kept).exists(), kept)
        self.assertTrue(self.is_enabled("framecolor.service") and self.is_enabled("frame-perf-overlay.service"))
        self.assertFalse([line for line in self.log() if "disable" in line or "restart" in line])
        self.assertFalse(self.h(".local/bin/framedock").exists())
        self.assertIn("the old one", self.h(".local/bin/framecolorctl").read_text())
        self.assertFalse(self.h(".local/bin/framedockctl").exists())
        self.assertEqual([p.name for p in self.h(".local/share").iterdir() if p.name.startswith(".framedock")], [])
        # A missing icon fails the same way
        self.h(".config/framecolor/current.txt").write_text("0 0 0 1 1 1 1 1 1 1 1 1 0 0 0\n")
        (self.release / "contrib/icons/framedock-128.png").unlink()
        self.assertNotEqual(self.run_script("install.sh", check=False).returncode, 0)
        self.assertTrue(self.h(".config/systemd/user/framecolor.service").exists())


if __name__ == "__main__":
    unittest.main()
