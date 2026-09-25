# Copyright 2026 Heapwatch
# SPDX-License-Identifier: Apache-2.0
#
# Pre-build: git describe for client app + connector -> heapwatch_build_metadata.h

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from heapwatch_build_metadata import (
    BUILD_IDENTIFIER_PLACEHOLDER,
    resolve_source_version,
    write_header,
)
def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate heapwatch_build_metadata.h from git describe.",
    )
    parser.add_argument("--app-root", type=Path, required=True)
    parser.add_argument("--connector-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        app_version = resolve_source_version(args.app_root, "HEAPWATCH_APP_SOURCE_VERSION")
        connector_version = resolve_source_version(
            args.connector_root,
            "HEAPWATCH_CONNECTOR_SOURCE_VERSION",
        )
        changed = write_header(
            args.output,
            app_source_version=app_version,
            connector_source_version=connector_version,
            build_identifier=BUILD_IDENTIFIER_PLACEHOLDER,
        )
        app_label = app_version if app_version else "(no VCS)"
        connector_label = connector_version if connector_version else "(no VCS)"
        print(
            f"Generated {args.output} "
            f"(app={app_label}, connector={connector_label}, "
            f"header {'updated' if changed else 'unchanged'})"
        )
    except Exception as err:  # noqa: BLE001 - build hook top-level
        print(f"ERROR: failed to generate build metadata header: {err}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
