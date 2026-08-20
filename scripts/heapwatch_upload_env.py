# Copyright (c) 2026 Heapwatch. All rights reserved.
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


def missing_upload_env_keys() -> list[str]:
    missing: list[str] = []
    for key in REQUIRED_UPLOAD_ENV_KEYS:
        if not os.environ.get(key, "").strip():
            missing.append(key)
    return missing


def format_missing_upload_env_error(app_root: Path, missing: list[str]) -> str:
    dotenv_path = app_root / ".env"
    missing_lines = "\n".join(f"  - {key}" for key in missing)
    return (
        "ERROR: Heapwatch firmware upload is not configured.\n\n"
        f"Missing environment variable(s):\n{missing_lines}\n\n"
        f"Create or update the project .env file:\n  {dotenv_path}\n\n"
        "Required keys:\n"
        "  HEAPWATCH_API_BASE=https://api.heapwatch.com\n"
        "  HEAPWATCH_BUILD_TOKEN=sk_build_...\n"
        "  HEAPWATCH_TENANT_UUID=<tenant-uuid>\n"
        "  HEAPWATCH_APPLICATION_ID=<application-id>\n\n"
        "Get these values in Heapwatch → Application → Build tokens.\n"
    )


def ensure_upload_env_configured(app_root: Path) -> None:
    """
    Load project .env and fail with a actionable error when upload credentials are incomplete.
    """
    load_dotenv(app_root / ".env")
    missing = missing_upload_env_keys()
    if missing:
        die(format_missing_upload_env_error(app_root, missing))
