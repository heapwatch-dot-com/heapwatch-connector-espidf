# Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
#
# Upload firmware metadata + binaries to Heapwatch CI endpoint.
# Called from finalize_and_upload_firmware.py after the application .bin exists.

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

SCRIPTS_DIR = Path(__file__).resolve().parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from heapwatch_build_metadata import parse_header

IDF_VERSION_MAJOR_RE = re.compile(r"^\s*set\(IDF_VERSION_MAJOR\s+(\d+)\)\s*$")
IDF_VERSION_MINOR_RE = re.compile(r"^\s*set\(IDF_VERSION_MINOR\s+(\d+)\)\s*$")
IDF_VERSION_PATCH_RE = re.compile(r"^\s*set\(IDF_VERSION_PATCH\s+(\d+)\)\s*$")
CONFIG_IDF_TARGET_RE = re.compile(r'^\s*CONFIG_IDF_TARGET="([^"]+)"\s*$', re.MULTILINE)


def die(msg: str, code: int = 1) -> None:
    print(msg, file=sys.stderr)
    sys.exit(code)


def load_dotenv(dotenv_path: Path) -> None:
    """
    Load KEY=VALUE pairs from .env into process environment.
    Existing process vars win over .env values.
    """
    if not dotenv_path.is_file():
        return

    for line in dotenv_path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "=" not in stripped:
            continue
        key, value = stripped.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key:
            continue
        if (value.startswith('"') and value.endswith('"')) or (
            value.startswith("'") and value.endswith("'")
        ):
            value = value[1:-1]
        if key not in os.environ:
            os.environ[key] = value


def require_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        die(f"ERROR: required environment variable {name} is not set.")
    return value


def is_strict_upload() -> bool:
    return os.environ.get("HEAPWATCH_UPLOAD_STRICT", "").strip().lower() in {
        "1",
        "true",
        "yes",
        "on",
    }


def upload_firmware_bundle(
    *,
    build_dir: Path,
    metadata_header: Path,
    project_name: str,
    app_root: Path | None = None,
    build_identifier: str | None = None,
) -> None:
    resolved_app_root = app_root or build_dir.parent
    dotenv_default = resolved_app_root / ".env"
    load_dotenv(dotenv_default)

    base = require_env("HEAPWATCH_API_BASE").rstrip("/")
    build_token = require_env("HEAPWATCH_BUILD_TOKEN")
    tenant_id = require_env("HEAPWATCH_TENANT_ID")
    app_id = require_env("HEAPWATCH_APPLICATION_ID")
    build_environment = os.environ.get("HEAPWATCH_BUILD_ENV", "development").strip()
    description = os.environ.get("HEAPWATCH_FIRMWARE_DESCRIPTION", "").strip() or None

    bin_path = build_dir / f"{project_name}.bin"
    elf_path = build_dir / f"{project_name}.elf"
    part_path = build_dir / "partition_table" / "partition-table.bin"
    partitions_csv_path = resolved_app_root / "partitions.csv"
    sdkconfig_path = resolved_app_root / "sdkconfig"

    for path, label in (
        (bin_path, "application .bin"),
        (elf_path, "ELF"),
        (part_path, "partition-table.bin"),
        (partitions_csv_path, "partitions.csv"),
        (metadata_header, "heapwatch_build_metadata.h"),
    ):
        if not path.is_file():
            die(f"ERROR: missing {label}: {path}")

    metadata = parse_header(metadata_header)
    app_source_version = metadata["app_source_version"]
    connector_source_version = metadata["connector_source_version"]
    version, _ = api_version_fields(app_source_version)
    source_version = app_source_version
    resolved_build_identifier = build_identifier or metadata["build_identifier"]
    build_toolchain_version = detect_build_toolchain_version()
    build_toolchain_type = "esp_idf"
    git_commit = detect_git_commit(resolved_app_root)
    partitions_csv = partitions_csv_path.read_text(encoding="utf-8", errors="replace")
    target_chip = detect_target_chip(build_dir=build_dir, sdkconfig_path=sdkconfig_path)

    firmware_url = f"{base}/api/applications/{app_id}/firmwares/ci-upload"
    post_body: dict[str, Any] = {
        "version": version,
        "source_version": source_version,
        "connector_source_version": connector_source_version,
        "build_identifier": resolved_build_identifier,
        "target_chip": target_chip,
        "build_environment": build_environment,
        "build_toolchain_version": build_toolchain_version,
        "build_toolchain_type": build_toolchain_type,
        "firmware_binary": file_to_data_uri(bin_path),
        "elf_file": file_to_data_uri(elf_path),
        "partition_data": file_to_data_uri(part_path),
        "partitions_csv": partitions_csv,
    }
    if description:
        post_body["description"] = description
    if git_commit:
        post_body["git_commit"] = git_commit

    req = urllib.request.Request(
        firmware_url,
        data=json.dumps(post_body).encode("utf-8"),
        method="POST",
    )
    req.add_header("Accept", "application/json")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", f"Bearer {build_token}")
    req.add_header("X-Tenant-Id", tenant_id)

    try:
        with urllib.request.urlopen(req, timeout=600) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            code = resp.getcode() or 200
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        code = e.code
        message = f"ERROR: firmware POST failed HTTP {code}: {body!r}"
        if is_strict_upload():
            die(message)
        print(f"WARNING: {message}", file=sys.stderr)
        return
    except urllib.error.URLError as e:
        message = f"ERROR: network failure: {e}"
        if is_strict_upload():
            die(message)
        print(f"WARNING: {message}", file=sys.stderr)
        return

    if code not in (200, 201):
        message = f"ERROR: firmware POST failed HTTP {code}: {body!r}"
        if is_strict_upload():
            die(message)
        print(f"WARNING: {message}", file=sys.stderr)
        return

    if code == 200 and "already_exists" in body:
        print(f"Firmware already uploaded (HTTP 200 already_exists): {body!r}")
    else:
        print(f"Firmware upload OK (HTTP {code}): {body!r}")


def detect_target_chip(*, build_dir: Path, sdkconfig_path: Path) -> str:
    """
    ESP-IDF chip target for OTA compatibility (Heapwatch firmware.target_chip).

    Read from the build output only — never a manual env override.
    """
    project_desc = build_dir / "project_description.json"
    if project_desc.is_file():
        try:
            data = json.loads(project_desc.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
        else:
            target = data.get("target") if isinstance(data, dict) else None
            if isinstance(target, str) and target.strip():
                return target.strip()

    idf_target = os.environ.get("IDF_TARGET", "").strip()
    if idf_target:
        return idf_target

    if not sdkconfig_path.is_file():
        die(
            "ERROR: cannot detect target_chip: missing sdkconfig "
            f"({sdkconfig_path}) and no build/project_description.json target."
        )

    text = sdkconfig_path.read_text(encoding="utf-8", errors="replace")
    match = CONFIG_IDF_TARGET_RE.search(text)
    if not match:
        die(
            "ERROR: cannot detect target_chip from build output. "
            f"Expected target in {build_dir / 'project_description.json'}, "
            f"IDF_TARGET in the build environment, or CONFIG_IDF_TARGET in {sdkconfig_path}."
        )

    return match.group(1)


def detect_build_toolchain_version() -> str:
    override = os.environ.get("HEAPWATCH_BUILD_TOOLCHAIN_VERSION", "").strip()
    if override:
        return override

    idf_ver = os.environ.get("IDF_VER", "").strip()
    if idf_ver:
        return idf_ver if idf_ver.startswith("v") else f"v{idf_ver}"

    idf_path = os.environ.get("IDF_PATH", "").strip()
    if not idf_path:
        die(
            "ERROR: build_toolchain_version is required. "
            "Set IDF_PATH/IDF_VER or HEAPWATCH_BUILD_TOOLCHAIN_VERSION."
        )

    version_cmake = Path(idf_path) / "tools" / "cmake" / "version.cmake"
    if not version_cmake.is_file():
        die(f"ERROR: cannot detect ESP-IDF version, missing file: {version_cmake}")

    major = None
    minor = None
    patch = None
    for line in version_cmake.read_text(encoding="utf-8", errors="replace").splitlines():
        if major is None:
            match = IDF_VERSION_MAJOR_RE.match(line)
            if match:
                major = match.group(1)
                continue
        if minor is None:
            match = IDF_VERSION_MINOR_RE.match(line)
            if match:
                minor = match.group(1)
                continue
        if patch is None:
            match = IDF_VERSION_PATCH_RE.match(line)
            if match:
                patch = match.group(1)
                continue

    if major is None or minor is None or patch is None:
        die(f"ERROR: cannot parse ESP-IDF version from {version_cmake}")

    return f"v{major}.{minor}.{patch}"


def detect_git_commit(app_root: Path) -> str | None:
    """
    Best-effort full git SHA (40 hex) for the firmware upload payload.
    Returns None if unset, invalid, or git is unavailable.
    """
    override = os.environ.get("HEAPWATCH_GIT_COMMIT", "").strip()
    if override:
        normalized = override.lower()
        if re.fullmatch(r"[0-9a-f]{40}", normalized):
            return normalized
        return None

    try:
        ret = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=app_root,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
        return None

    if ret.returncode != 0:
        return None

    sha = ret.stdout.strip().lower()
    if re.fullmatch(r"[0-9a-f]{40}", sha):
        return sha
    return None


def api_version_fields(app_source_version: str) -> tuple[str, str]:
    """API display version (e.g. v1.2.3) from HEAPWATCH_APP_SOURCE_VERSION first segment."""
    tag_part = app_source_version.split("-", 1)[0].strip()
    if not tag_part:
        die(f"ERROR: invalid HEAPWATCH_APP_SOURCE_VERSION: {app_source_version!r}")
    display = tag_part if tag_part.startswith("v") else f"v{tag_part}"
    return display, app_source_version


def file_to_data_uri(path: Path) -> str:
    raw = path.read_bytes()
    encoded = base64.standard_b64encode(raw).decode("ascii")
    return f"data:application/octet-stream;base64,{encoded}"


def main() -> None:
    parser = argparse.ArgumentParser(description="Upload firmware bundle after ESP-IDF build.")
    parser.add_argument("--build-dir", type=Path, required=True, help="CMake binary dir (build/)")
    parser.add_argument(
        "--metadata-header",
        type=Path,
        required=True,
        help="Path to generated/heapwatch_build_metadata.h",
    )
    parser.add_argument(
        "--project-name",
        type=str,
        required=True,
        help="CMAKE_PROJECT_NAME (basename of .elf / .bin)",
    )
    parser.add_argument(
        "--app-root",
        type=Path,
        default=None,
        help="Client firmware project root (defaults to build-dir parent)",
    )
    args = parser.parse_args()

    upload_firmware_bundle(
        build_dir=args.build_dir,
        metadata_header=args.metadata_header,
        project_name=args.project_name,
        app_root=args.app_root,
    )


if __name__ == "__main__":
    main()
