"""Check Debug/Release SDK staging with synthetic files, without game content."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("staging", ROOT / "tools/stage_renderer_sdk.py")
staging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(staging)

# Install artifact names and locations differ per platform (the script's
# artifact_names() is the single source of truth; see tools/stage_renderer_sdk.py).
RELEASE_NAMES = staging.artifact_names("Release")
DEBUG_NAMES = staging.artifact_names("Debug")


def install_rel(name):
    """Install location of an artifact name inside the install prefix."""
    if name.endswith((".lib", ".so", ".dylib")):
        return f"lib/{name}"
    return f"bin/{name}"  # .dll / .exe / the bare rexglue tool


SHARED_FILES = ["include/rex/version.h", "lib/cmake/rexglue/rexglueConfig.cmake"]
RELEASE_FILES = SHARED_FILES + [install_rel(n) for n in
                                (RELEASE_NAMES["tool"], RELEASE_NAMES["runtime"], RELEASE_NAMES["plugin"],
                                 *([RELEASE_NAMES["runtime_lib"], RELEASE_NAMES["plugin_lib"]]
                                   if RELEASE_NAMES["runtime_lib"] else ()))]
# Only Release installs the codegen tool (rexglue / rexglue.exe).
DEBUG_FILES = SHARED_FILES + [install_rel(n) for n in
                              (DEBUG_NAMES["runtime"], DEBUG_NAMES["plugin"],
                               *([DEBUG_NAMES["runtime_lib"], DEBUG_NAMES["plugin_lib"]]
                                 if DEBUG_NAMES["runtime_lib"] else ()))]


def make_install(output, names):
    """Write each install file with its own content, so a hash of the wrong file shows up."""
    for name in names:
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("built-" + name, encoding="utf-8")


def run_staging(source, output, *options):
    """Run the staging script on a fake install and return the provenance record it writes."""
    argv = ["stage_renderer_sdk.py", str(source), str(output), *options]
    with (
        patch("sys.argv", argv),
        patch.object(staging.subprocess, "check_output", return_value="synthetic-revision\n"),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        staging.main()
    return json.loads((output / "renderer-sdk-provenance.json").read_text(encoding="utf-8"))


def sha256_of(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class RuntimeStagingTests(unittest.TestCase):
    def stage(self, configuration, missing=None):
        names = DEBUG_NAMES if configuration == "Debug" else RELEASE_NAMES
        with tempfile.TemporaryDirectory(prefix="fable2-sdk-test-") as directory:
            base = Path(directory)
            source, output = base / "source", base / "output"
            if sys.platform == "win32":
                stems = names["stem"]
                fixtures = [f"{stem}.{extension}" for stem in stems
                            for extension in ("dll", "lib", "pdb")]
                if configuration == "Release":
                    fixtures.append(names["tool"])
            else:
                # ELF/Mach-O builds carry their debug info in the libraries
                # themselves: no import libs, no separate symbol files.
                fixtures = [names["runtime"], names["plugin"]]
                if configuration == "Release":
                    fixtures.append(names["tool"])
            files = {
                output / "include/rex/version.h": "version",
                output / "lib/cmake/rexglue/rexglueConfig.cmake": "config",
            }
            for name in fixtures:
                if name == missing:
                    continue
                if name.endswith(".pdb"):
                    files[source / "out" / staging.sdk_platform() / name] = "symbols-" + name
                else:
                    files[output / install_rel(name)] = "built-" + name
            for path, value in files.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(value)
            args = ["stage_renderer_sdk.py", str(source), str(output), "--configuration", configuration]
            with patch("sys.argv", args), patch.object(staging.subprocess, "check_output",
                                                       return_value="synthetic-revision\n"):
                if missing:
                    with self.assertRaisesRegex(SystemExit, "Missing build input"):
                        staging.main()
                    self.assertFalse((output / "renderer-sdk-provenance.json").exists())
                    return
                staging.main()
            if sys.platform == "win32":
                for stem in names["stem"]:
                    name = f"{stem}.pdb"
                    self.assertEqual((output / "bin" / name).read_text(), "symbols-" + name)
            provenance = json.loads((output / "renderer-sdk-provenance.json").read_text())
            self.assertEqual(provenance["configuration"], configuration)
            self.assertEqual(provenance["sdk_commit"], "synthetic-revision")

    def test_new_tool_clears_codegen_stamp(self):
        with tempfile.TemporaryDirectory(prefix="fable2-sdk-test-") as directory:
            base = Path(directory)
            source, output, stamp = base / "source", base / "output", base / "codegen.stamp"
            make_install(output, RELEASE_FILES)
            args = ["stage_renderer_sdk.py", str(source), str(output), "--codegen-stamp", str(stamp)]

            def run():
                with patch("sys.argv", args), patch.object(staging.subprocess, "check_output",
                                                           return_value="synthetic-revision\n"):
                    staging.main()

            run()
            stamp.write_text("fingerprint")
            run()
            self.assertTrue(stamp.exists())
            (output / install_rel(RELEASE_NAMES["tool"])).write_text("rebuilt")
            run()
            self.assertFalse(stamp.exists())

    def test_debug_uses_debug_pair_without_tool(self):
        self.stage("Debug")

    def test_release_uses_release_pair(self):
        self.stage("Release")

    def test_missing_release_tool_fails(self):
        self.stage("Release", missing=RELEASE_NAMES["tool"])

    def test_missing_debug_runtime_fails(self):
        self.stage("Debug", missing=DEBUG_NAMES["runtime"])

    def test_missing_debug_plugin_artifact_fails(self):
        # Windows stages the plugin import lib separately; other platforms only
        # ship the plugin library itself.
        self.stage("Debug", missing=DEBUG_NAMES["plugin_lib"] or DEBUG_NAMES["plugin"])


class RuntimeStagingRecordTests(unittest.TestCase):
    def test_release_record_hashes_each_file(self):
        with tempfile.TemporaryDirectory(prefix="fable2-sdk-test-") as directory:
            source, output = Path(directory) / "source", Path(directory) / "output"
            make_install(output, RELEASE_FILES)
            record = run_staging(source, output)
            runtime_hash = sha256_of(output / install_rel(RELEASE_NAMES["runtime"]))
            plugin_hash = sha256_of(output / install_rel(RELEASE_NAMES["plugin"]))
            tool_hash = sha256_of(output / install_rel(RELEASE_NAMES["tool"]))

        self.assertEqual(record["runtime_sha256"], runtime_hash)
        self.assertEqual(record["plugin_sha256"], plugin_hash)
        self.assertEqual(record["codegen_tool_sha256"], tool_hash)

    def test_debug_record_hashes_the_debug_files(self):
        with tempfile.TemporaryDirectory(prefix="fable2-sdk-test-") as directory:
            source, output = Path(directory) / "source", Path(directory) / "output"
            make_install(output, DEBUG_FILES)
            record = run_staging(source, output, "--configuration", "Debug")
            runtime_hash = sha256_of(output / install_rel(DEBUG_NAMES["runtime"]))
            plugin_hash = sha256_of(output / install_rel(DEBUG_NAMES["plugin"]))

        self.assertEqual(record["runtime_sha256"], runtime_hash)
        self.assertEqual(record["plugin_sha256"], plugin_hash)

    def test_debug_record_has_no_tool_hash(self):
        with tempfile.TemporaryDirectory(prefix="fable2-sdk-test-") as directory:
            source, output = Path(directory) / "source", Path(directory) / "output"
            make_install(output, DEBUG_FILES)
            record = run_staging(source, output, "--configuration", "Debug")

        self.assertNotIn("codegen_tool_sha256", record)

    def test_staging_works_without_symbols(self):
        # PDBs are optional: the source folder has no out/<platform> build output at all.
        with tempfile.TemporaryDirectory(prefix="fable2-sdk-test-") as directory:
            source, output = Path(directory) / "source", Path(directory) / "output"
            make_install(output, RELEASE_FILES)
            record = run_staging(source, output)
            symbols = list((output / "bin").glob("*.pdb"))

        self.assertEqual(record["configuration"], "Release")
        self.assertEqual(symbols, [])

    def test_debug_staging_keeps_the_codegen_stamp(self):
        # Debug and Release share one stamp, and only Release installs the tool, so a
        # Debug run must leave the stamp alone even if the earlier record looks out of date.
        with tempfile.TemporaryDirectory(prefix="fable2-sdk-test-") as directory:
            base = Path(directory)
            source, output, stamp = base / "source", base / "output", base / "codegen.stamp"
            make_install(output, DEBUG_FILES)
            (output / "renderer-sdk-provenance.json").write_text(
                json.dumps({"codegen_tool_sha256": "old"}), encoding="utf-8")
            stamp.write_text("fingerprint", encoding="utf-8")
            run_staging(source, output, "--configuration", "Debug", "--codegen-stamp", str(stamp))
            stamp_exists = stamp.exists()

        self.assertTrue(stamp_exists)

    def test_first_release_staging_clears_the_codegen_stamp(self):
        # With no earlier record there is no proof the tool is unchanged, so the stamp goes.
        with tempfile.TemporaryDirectory(prefix="fable2-sdk-test-") as directory:
            base = Path(directory)
            source, output, stamp = base / "source", base / "output", base / "codegen.stamp"
            make_install(output, RELEASE_FILES)
            stamp.write_text("fingerprint", encoding="utf-8")
            run_staging(source, output, "--codegen-stamp", str(stamp))
            stamp_exists = stamp.exists()

        self.assertFalse(stamp_exists)


if __name__ == "__main__":
    unittest.main()
