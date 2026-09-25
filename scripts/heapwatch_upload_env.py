# Copyright 2026 Heapwatch
# SPDX-License-Identifier: Apache-2.0
#
# Validate Heapwatch CI upload credentials from the client firmware project .env.

from __future__ import annotations

import os
import sys
from pathlib import Path

REQUIRED_UPLOAD_ENV_KEYS: tuple[str, ...] = (
    "HEAPWATCH_API_BASE",
    "HEAPWATCH_BUILD_TOKEN",
    "HEAPWATCH_TENANT_UUID",
    "HEAPWATCH_APPLICATION_ID",
)


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


def missing_upload_env_keys() -> list[str]:
    missing: list[str] = []
    for key in REQUIRED_UPLOAD_ENV_KEYS:
        if not os.environ.get(key, "").strip():
            missing.append(key)
    return missing


def format_missing_upload_env_warning(app_root: Path, missing: list[str]) -> str:
    dotenv_path = app_root / ".env"
    if dotenv_path.is_file():
        where = f"{dotenv_path} is missing: {', '.join(missing)}."
    else:
        where = f"No .env file at {dotenv_path}."
    return (
        "WARNING: Heapwatch firmware upload is disabled. "
        f"{where} The build will continue without uploading.\n"
        "To enable upload, set HEAPWATCH_API_BASE, HEAPWATCH_BUILD_TOKEN, "
        "HEAPWATCH_TENANT_UUID, and HEAPWATCH_APPLICATION_ID in the project-root .env file.\n"
    )


def ensure_upload_env_configured(app_root: Path) -> bool:
    """
    Load project .env. When upload credentials are missing, warn and return False.
    The caller skips upload and the build continues.
    """
    load_dotenv(app_root / ".env")
    missing = missing_upload_env_keys()
    if missing:
        print(format_missing_upload_env_warning(app_root, missing), file=sys.stderr)
        return False
    return True
