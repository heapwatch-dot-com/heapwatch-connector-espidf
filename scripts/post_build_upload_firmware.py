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
from heapwatch_upload_env import ensure_upload_env_configured

IDF_VERSION_MAJOR_RE = re.compile(r"^\s*set\(IDF_VERSION_MAJOR\s+(\d+)\)\s*$")
IDF_VERSION_MINOR_RE = re.compile(r"^\s*set\(IDF_VERSION_MINOR\s+(\d+)\)\s*$")
IDF_VERSION_PATCH_RE = re.compile(r"^\s*set\(IDF_VERSION_PATCH\s+(\d+)\)\s*$")
CONFIG_IDF_TARGET_RE = re.compile(r'^\s*CONFIG_IDF_TARGET="([^"]+)"\s*$', re.MULTILINE)


def die(msg: str, code: int = 1) -> None:
    print(msg, file=sys.stderr)
    sys.exit(code)


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
    ensure_upload_env_configured(resolved_app_root)

    base = os.environ["HEAPWATCH_API_BASE"].strip().rstrip("/")
    build_token = os.environ["HEAPWATCH_BUILD_TOKEN"].strip()
    tenant_uuid = os.environ["HEAPWATCH_TENANT_UUID"].strip()
    app_id = os.environ["HEAPWATCH_APPLICATION_ID"].strip()
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
    app_source_version_raw = metadata["app_source_version"]
    connector_source_version = normalize_optional_version(metadata["connector_source_version"])
    resolved_build_identifier = build_identifier or metadata["build_identifier"]
    version, source_version = api_version_fields(app_source_version_raw, resolved_build_identifier)
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
    req.add_header("X-Tenant-Id", tenant_uuid)

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

    print(format_firmware_upload_success(code, body))


def format_firmware_upload_success(code: int, body: str) -> str:
    """One-line success summary — avoid dumping the full API payload (binaries, URLs, …)."""
    try:
        payload = json.loads(body)
    except json.JSONDecodeError:
        return f"Firmware upload OK (HTTP {code})"

    if not isinstance(payload, dict):
        return f"Firmware upload OK (HTTP {code})"

    data = payload.get("data")
    if not isinstance(data, dict):
        if code == 200 and "already_exists" in body:
            return "Firmware already uploaded (HTTP 200 already_exists)"
        return f"Firmware upload OK (HTTP {code})"

    firmware_id = data.get("id")
    version = data.get("version")
    build_identifier = data.get("build_identifier")

    parts: list[str] = []
    if firmware_id is not None:
        parts.append(f"id={firmware_id}")
    if isinstance(version, str) and version:
        parts.append(f"version={version}")
    if isinstance(build_identifier, str) and build_identifier:
        parts.append(f"build_identifier={build_identifier}")

    if code == 200 and "already_exists" in body:
        prefix = "Firmware already uploaded"
    else:
        prefix = "Firmware upload OK"

    if parts:
        return f"{prefix} (HTTP {code}): {', '.join(parts)}"
    return f"{prefix} (HTTP {code})"


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


def normalize_optional_version(value: str | None) -> str | None:
    if value is None:
        return None
    stripped = value.strip()
    return stripped if stripped else None


def api_version_fields(app_source_version: str, build_identifier: str) -> tuple[str, str | None]:
    """
    API display version and optional source_version for firmware upload.

    When git describe is unavailable, source_version is None and version is derived
    from the trailing body of build_identifier (build fingerprint is still required).
    """
    tag_part = app_source_version.split("-", 1)[0].strip()
    if tag_part:
        display = tag_part if tag_part.startswith("v") else f"v{tag_part}"
        normalized_source = app_source_version.strip() or None
        return display, normalized_source

    body = build_identifier.strip()
    if body.lower().startswith("sha256:"):
        body = body[7:].strip()
    suffix = body[-12:] if len(body) >= 12 else (body or "unknown")
    return f"v{suffix}", None


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
