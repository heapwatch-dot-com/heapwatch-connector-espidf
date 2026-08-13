# Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
#
# Pre-build: git describe for client app + connector -> heapwatch_build_metadata.h

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from heapwatch_build_metadata import (
    BUILD_IDENTIFIER_PLACEHOLDER,
    git_describe,
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
        app_version = git_describe(args.app_root)
        connector_version = git_describe(args.connector_root)
        changed = write_header(
            args.output,
            app_source_version=app_version,
            connector_source_version=connector_version,
            build_identifier=BUILD_IDENTIFIER_PLACEHOLDER,
        )
        print(
            f"Generated {args.output} "
            f"(app={app_version}, connector={connector_version}, "
            f"header {'updated' if changed else 'unchanged'})"
        )
    except Exception as err:  # noqa: BLE001 - build hook top-level
        print(f"ERROR: failed to generate build metadata header: {err}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
