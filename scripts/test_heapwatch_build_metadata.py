# Copyright 2026 Heapwatch
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import json
import os
import tempfile
import unittest
from io import StringIO
from pathlib import Path
from unittest.mock import patch

from heapwatch_build_metadata import git_describe, resolve_source_version
from heapwatch_multipart_upload import encode_multipart_form_data
from heapwatch_upload_env import (
    ensure_upload_env_configured,
    format_missing_upload_env_warning,
    missing_upload_env_keys,
)
from post_build_upload_firmware import (
    api_version_fields,
    ensure_artifacts_match_successful_build,
    format_firmware_upload_success,
    fail_on_upload_error,
    https_ssl_context,
    normalize_optional_version,
    stale_sources_newer_than,
    upload_firmware_bundle,
)


class HeapwatchBuildMetadataTest(unittest.TestCase):
    def test_git_describe_returns_empty_without_git(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(git_describe(Path(tmp)), "")

    def test_resolve_source_version_env_override(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            import os

            os.environ["HEAPWATCH_APP_SOURCE_VERSION"] = "v2.3.4"
            try:
                self.assertEqual(resolve_source_version(Path(tmp), "HEAPWATCH_APP_SOURCE_VERSION"), "2.3.4")
            finally:
                del os.environ["HEAPWATCH_APP_SOURCE_VERSION"]

    def test_normalize_optional_version(self) -> None:
        self.assertIsNone(normalize_optional_version(None))
        self.assertIsNone(normalize_optional_version(""))
        self.assertIsNone(normalize_optional_version("   "))
        self.assertEqual(normalize_optional_version(" 1.0.0 "), "1.0.0")

    def test_api_version_fields_with_git_describe(self) -> None:
        version, source = api_version_fields("1.2.3-4-gabc", "sha256:" + ("a" * 64))
        self.assertEqual(version, "v1.2.3")
        self.assertEqual(source, "1.2.3-4-gabc")

    def test_api_version_fields_without_vcs(self) -> None:
        build_identifier = "sha256:" + ("b" * 64)
        version, source = api_version_fields("", build_identifier)
        self.assertEqual(version, "v" + ("b" * 12))
        self.assertIsNone(source)

    def test_format_firmware_upload_success_summarizes_payload(self) -> None:
        body = json.dumps(
            {
                "success": True,
                "code": 201,
                "data": {
                    "id": 4,
                    "version": "v4957a2dfc8de",
                    "build_identifier": "sha256:abc",
                    "firmware_binary": {"public_url": "https://example.test/huge.bin"},
                },
            }
        )
        message = format_firmware_upload_success(201, body)
        self.assertEqual(
            message,
            "Firmware upload OK (HTTP 201): id=4, version=v4957a2dfc8de, build_identifier=sha256:abc",
        )
        self.assertNotIn("public_url", message)

    def test_format_firmware_upload_success_already_exists(self) -> None:
        body = '{"success":true,"code":200,"data":{"id":2,"version":"v1"},"message":"already_exists"}'
        message = format_firmware_upload_success(200, body)
        self.assertEqual(
            message,
            "Firmware already uploaded (HTTP 200): id=2, version=v1",
        )

    def test_multipart_upload_encodes_file_fields_for_ci_endpoint(self) -> None:
        body, content_type = encode_multipart_form_data(
            {"version": "v1"},
            {
                "flasher_args": ("flasher_args.json", b"{}", "application/json"),
                "flash_files[0x10000]": ("lysos-wifi-tester.bin", b"BIN", "application/octet-stream"),
            },
        )
        self.assertIn("multipart/form-data; boundary=", content_type)
        self.assertIn(b'name="flasher_args"; filename="flasher_args.json"', body)
        self.assertIn(b'name="flash_files[0x10000]"; filename="lysos-wifi-tester.bin"', body)
        self.assertIn(b"BIN", body)

    def test_missing_upload_env_lists_all_keys(self) -> None:
        import os

        saved = {key: os.environ.pop(key, None) for key in (
            "HEAPWATCH_API_BASE",
            "HEAPWATCH_BUILD_TOKEN",
            "HEAPWATCH_TENANT_UUID",
            "HEAPWATCH_APPLICATION_ID",
        )}
        try:
            missing = missing_upload_env_keys()
            self.assertEqual(
                missing,
                [
                    "HEAPWATCH_API_BASE",
                    "HEAPWATCH_BUILD_TOKEN",
                    "HEAPWATCH_TENANT_UUID",
                    "HEAPWATCH_APPLICATION_ID",
                ],
            )
            with tempfile.TemporaryDirectory() as tmp:
                message = format_missing_upload_env_warning(Path(tmp), missing)
                self.assertIn("WARNING: Heapwatch firmware upload is disabled", message)
                self.assertIn("HEAPWATCH_BUILD_TOKEN", message)
                self.assertIn(str(Path(tmp) / ".env"), message)
        finally:
            for key, value in saved.items():
                if value is not None:
                    os.environ[key] = value

    def test_ensure_upload_env_reads_dotenv(self) -> None:
        import os

        saved = {key: os.environ.pop(key, None) for key in (
            "HEAPWATCH_API_BASE",
            "HEAPWATCH_BUILD_TOKEN",
            "HEAPWATCH_TENANT_UUID",
            "HEAPWATCH_APPLICATION_ID",
        )}
        try:
            with tempfile.TemporaryDirectory() as tmp:
                app_root = Path(tmp)
                (app_root / ".env").write_text(
                    "\n".join(
                        [
                            "HEAPWATCH_API_BASE=https://api.example.test",
                            "HEAPWATCH_BUILD_TOKEN=sk_build_test",
                            "HEAPWATCH_TENANT_UUID=tenant-uuid",
                            "HEAPWATCH_APPLICATION_ID=42",
                        ]
                    ),
                    encoding="utf-8",
                )
                self.assertTrue(ensure_upload_env_configured(app_root))
                self.assertEqual(os.environ["HEAPWATCH_APPLICATION_ID"], "42")
        finally:
            for key, value in saved.items():
                if value is not None:
                    os.environ[key] = value
                else:
                    os.environ.pop(key, None)

    def test_ensure_upload_env_warns_when_incomplete(self) -> None:
        import os

        saved = {key: os.environ.pop(key, None) for key in (
            "HEAPWATCH_API_BASE",
            "HEAPWATCH_BUILD_TOKEN",
            "HEAPWATCH_TENANT_UUID",
            "HEAPWATCH_APPLICATION_ID",
        )}
        try:
            with tempfile.TemporaryDirectory() as tmp:
                stderr = StringIO()
                with patch("sys.stderr", stderr):
                    ready = ensure_upload_env_configured(Path(tmp))
                self.assertFalse(ready)
                self.assertIn("WARNING: Heapwatch firmware upload is disabled", stderr.getvalue())
        finally:
            for key, value in saved.items():
                if value is not None:
                    os.environ[key] = value

    def test_stale_sources_newer_than_elf(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "main.cpp"
            source.write_text("int main() { return 0; }\n", encoding="utf-8")
            older = 1_700_000_000.0
            newer = older + 60
            os.utime(source, (newer, newer))
            stale = stale_sources_newer_than(artifact_mtime=older, roots=(root,))
            self.assertEqual(stale, [source])

    def test_ensure_artifacts_refuse_stale_elf(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            app_root = Path(tmp) / "app"
            connector_root = Path(tmp) / "connector"
            build_dir = app_root / "build"
            app_root.mkdir()
            connector_root.mkdir()
            build_dir.mkdir()

            elf_path = build_dir / "crasher.elf"
            bin_path = build_dir / "crasher.bin"
            stamp_path = build_dir / ".bin_timestamp"
            source = app_root / "main.cpp"
            source.write_text("int main() { return 1; }\n", encoding="utf-8")
            for path in (elf_path, bin_path, stamp_path):
                path.write_bytes(b"x")

            older = 1_700_000_000.0
            newer = older + 120
            for path in (elf_path, bin_path, stamp_path):
                os.utime(path, (older, older))
            os.utime(source, (newer, newer))

            with self.assertRaises(RuntimeError) as ctx:
                ensure_artifacts_match_successful_build(
                    build_dir=build_dir,
                    project_name="crasher",
                    app_root=app_root,
                    connector_root=connector_root,
                )
            self.assertIn("source files are newer than the ELF", str(ctx.exception))

    def test_ensure_artifacts_accept_fresh_elf(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            app_root = Path(tmp) / "app"
            connector_root = Path(tmp) / "connector"
            build_dir = app_root / "build"
            app_root.mkdir()
            connector_root.mkdir()
            build_dir.mkdir()

            elf_path = build_dir / "crasher.elf"
            bin_path = build_dir / "crasher.bin"
            stamp_path = build_dir / ".bin_timestamp"
            source = app_root / "main.cpp"
            source.write_text("int main() { return 0; }\n", encoding="utf-8")
            for path in (elf_path, bin_path, stamp_path):
                path.write_bytes(b"x")

            older = 1_700_000_000.0
            newer = older + 120
            os.utime(source, (older, older))
            for path in (elf_path, bin_path, stamp_path):
                os.utime(path, (newer, newer))

            ensure_artifacts_match_successful_build(
                build_dir=build_dir,
                project_name="crasher",
                app_root=app_root,
                connector_root=connector_root,
            )

    def test_https_ssl_context_lab_uses_bundled_caddy_ca(self) -> None:
        saved = os.environ.pop("HEAPWATCH_TLS_CA_FILE", None)
        try:
            ctx = https_ssl_context("https://api.heapwatch.local")
            self.assertIsNotNone(ctx)
            bundled = Path(__file__).resolve().parent.parent / "certs" / "heapwatch_local_ca.pem"
            self.assertTrue(bundled.is_file(), f"missing lab CA {bundled}")
        finally:
            if saved is not None:
                os.environ["HEAPWATCH_TLS_CA_FILE"] = saved

    def test_fail_on_upload_error_defaults_true(self) -> None:
        saved = os.environ.pop("HEAPWATCH_FAIL_ON_UPLOAD_ERROR", None)
        try:
            self.assertTrue(fail_on_upload_error())
        finally:
            if saved is not None:
                os.environ["HEAPWATCH_FAIL_ON_UPLOAD_ERROR"] = saved

    def test_fail_on_upload_error_opt_out(self) -> None:
        saved = os.environ.pop("HEAPWATCH_FAIL_ON_UPLOAD_ERROR", None)
        os.environ["HEAPWATCH_FAIL_ON_UPLOAD_ERROR"] = "0"
        try:
            self.assertFalse(fail_on_upload_error())
        finally:
            if saved is not None:
                os.environ["HEAPWATCH_FAIL_ON_UPLOAD_ERROR"] = saved
            else:
                os.environ.pop("HEAPWATCH_FAIL_ON_UPLOAD_ERROR", None)

    def test_upload_uses_passed_partitions_csv_path(self) -> None:
        saved = {key: os.environ.pop(key, None) for key in (
            "HEAPWATCH_API_BASE",
            "HEAPWATCH_BUILD_TOKEN",
            "HEAPWATCH_TENANT_UUID",
            "HEAPWATCH_APPLICATION_ID",
        )}
        os.environ["HEAPWATCH_API_BASE"] = "https://api.example.test"
        os.environ["HEAPWATCH_BUILD_TOKEN"] = "sk_build_test"
        os.environ["HEAPWATCH_TENANT_UUID"] = "tenant-uuid"
        os.environ["HEAPWATCH_APPLICATION_ID"] = "42"
        try:
            with tempfile.TemporaryDirectory() as tmp:
                app_root = Path(tmp)
                build_dir = app_root / "build"
                build_dir.mkdir()
                (build_dir / "flasher_args.json").write_text("{}", encoding="utf-8")
                custom_csv = app_root / "partition_unsecured_16MB.csv"
                stderr = StringIO()
                with patch("sys.stderr", stderr), self.assertRaises(SystemExit) as ctx:
                    upload_firmware_bundle(
                        build_dir=build_dir,
                        metadata_header=app_root / "heapwatch_build_metadata.h",
                        project_name="remoted-node-gps",
                        partitions_csv=custom_csv,
                        app_root=app_root,
                    )
                self.assertEqual(ctx.exception.code, 1)
                self.assertIn(str(custom_csv), stderr.getvalue())
                self.assertNotIn(str(app_root / "partitions.csv"), stderr.getvalue())
        finally:
            for key, value in saved.items():
                if value is not None:
                    os.environ[key] = value
                else:
                    os.environ.pop(key, None)


if __name__ == "__main__":
    unittest.main()
