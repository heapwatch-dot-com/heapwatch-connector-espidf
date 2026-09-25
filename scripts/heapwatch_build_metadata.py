# Copyright 2026 Heapwatch
# SPDX-License-Identifier: Apache-2.0
#
# Read/write generated heapwatch_build_metadata.h for client firmware builds.

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

BUILD_IDENTIFIER_PREFIX = "sha256:"
BUILD_IDENTIFIER_HEX_LEN = 64
BUILD_IDENTIFIER_PLACEHOLDER = BUILD_IDENTIFIER_PREFIX + ("0" * BUILD_IDENTIFIER_HEX_LEN)

HEAPWATCH_APP_SOURCE_VERSION_RE = re.compile(
    r'^\s*#define\s+HEAPWATCH_APP_SOURCE_VERSION\s+"([^"]*)"\s*$',
    re.MULTILINE,
)
HEAPWATCH_CONNECTOR_SOURCE_VERSION_RE = re.compile(
    r'^\s*#define\s+HEAPWATCH_CONNECTOR_SOURCE_VERSION\s+"([^"]*)"\s*$',
    re.MULTILINE,
)
HEAPWATCH_BUILD_IDENTIFIER_RE = re.compile(
    r'^\s*#define\s+HEAPWATCH_BUILD_IDENTIFIER\s+"([^"]*)"\s*$',
    re.MULTILINE,
)


def normalize_describe_version(raw: str) -> str:
    version = raw.strip()
    if version.startswith("v"):
        version = version[1:]
    return version


def git_describe(repo_root: Path) -> str:
    """
    Best-effort git-describe for a repo root.

    Returns an empty string when git is unavailable (no .git, not a checkout, etc.).
    """
    safe_directory = str(repo_root.resolve())
    for args in (
        ["git", "-c", f"safe.directory={safe_directory}", "describe", "--tags", "--always", "--dirty"],
        ["git", "-c", f"safe.directory={safe_directory}", "describe", "--always", "--dirty"],
    ):
        ret = subprocess.run(
            args,
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )
        if ret.returncode != 0:
            continue

        version = normalize_describe_version(ret.stdout)
        if version:
            return version

    return ""


def resolve_source_version(repo_root: Path, env_var: str) -> str:
    """
    Resolve a source version string for build metadata.

    Order: process env override, then git describe, else empty (no VCS).
    """
    override = os.environ.get(env_var, "").strip()
    if override:
        return normalize_describe_version(override)

    return git_describe(repo_root)


def render_header(
    *,
    app_source_version: str,
    connector_source_version: str,
    build_identifier: str,
) -> str:
    return (
        "// Auto-generated build metadata — do not edit manually.\n"
        "#ifndef HEAPWATCH_BUILD_METADATA_H\n"
        "#define HEAPWATCH_BUILD_METADATA_H\n\n"
        f'#define HEAPWATCH_APP_SOURCE_VERSION "{app_source_version}"\n'
        f'#define HEAPWATCH_CONNECTOR_SOURCE_VERSION "{connector_source_version}"\n'
        f'#define HEAPWATCH_BUILD_IDENTIFIER "{build_identifier}"\n\n'
        "#endif // HEAPWATCH_BUILD_METADATA_H\n"
    )


def write_header(
    path: Path,
    *,
    app_source_version: str,
    connector_source_version: str,
    build_identifier: str,
) -> bool:
    content = render_header(
        app_source_version=app_source_version,
        connector_source_version=connector_source_version,
        build_identifier=build_identifier,
    )

    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return False

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def parse_header(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise FileNotFoundError(f"Missing build metadata header: {path}")

    text = path.read_text(encoding="utf-8", errors="replace")

    app_match = HEAPWATCH_APP_SOURCE_VERSION_RE.search(text)
    connector_match = HEAPWATCH_CONNECTOR_SOURCE_VERSION_RE.search(text)
    build_match = HEAPWATCH_BUILD_IDENTIFIER_RE.search(text)

    if app_match is None or connector_match is None or build_match is None:
        raise ValueError(f"Invalid or incomplete build metadata header: {path}")

    return {
        "app_source_version": app_match.group(1),
        "connector_source_version": connector_match.group(1),
        "build_identifier": build_match.group(1),
    }


def update_build_identifier(path: Path, build_identifier: str) -> bool:
    parsed = parse_header(path)
    return write_header(
        path,
        app_source_version=parsed["app_source_version"],
        connector_source_version=parsed["connector_source_version"],
        build_identifier=build_identifier,
    )
