# Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
#
# Composite build fingerprint for Heapwatch firmware uploads and coredump pairing.
#
# HEAPWATCH_BUILD_IDENTIFIER = "sha256:" + sha256(normalize(elf) + partition_table + sdkconfig)
#
# ELF normalization replaces any embedded sha256:<64-hex> token with a fixed-width
# placeholder so the identifier stays stable across the post-link header refresh.

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

from heapwatch_build_metadata import (
    BUILD_IDENTIFIER_PLACEHOLDER,
    BUILD_IDENTIFIER_PREFIX,
    update_build_identifier,
)

BUILD_IDENTIFIER_BYTES_RE = re.compile(
    rb"sha256:[0-9a-f]{64}",
    flags=re.IGNORECASE,
)


def normalize_elf_bytes(elf_bytes: bytes) -> bytes:
    return BUILD_IDENTIFIER_BYTES_RE.sub(
        BUILD_IDENTIFIER_PLACEHOLDER.encode("ascii"),
        elf_bytes,
    )


def compute_build_identifier(
    elf_path: Path,
    partition_path: Path,
    sdkconfig_path: Path,
) -> str:
    for path, label in (
        (elf_path, "ELF"),
        (partition_path, "partition table"),
        (sdkconfig_path, "sdkconfig"),
    ):
        if not path.is_file():
            raise FileNotFoundError(f"Missing {label}: {path}")

    digest = hashlib.sha256()
    digest.update(normalize_elf_bytes(elf_path.read_bytes()))
    digest.update(partition_path.read_bytes())
    digest.update(sdkconfig_path.read_bytes())
    return BUILD_IDENTIFIER_PREFIX + digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compute Heapwatch composite HEAPWATCH_BUILD_IDENTIFIER.",
    )
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--partition", type=Path, required=True)
    parser.add_argument("--sdkconfig", type=Path, required=True)
    parser.add_argument(
        "--metadata-header",
        type=Path,
        help="Optional path to generated/heapwatch_build_metadata.h",
    )
    args = parser.parse_args()

    try:
        build_identifier = compute_build_identifier(
            args.elf,
            args.partition,
            args.sdkconfig,
        )
    except FileNotFoundError as err:
        print(f"ERROR: {err}", file=sys.stderr)
        sys.exit(1)

    if args.metadata_header is not None:
        changed = update_build_identifier(args.metadata_header, build_identifier)
        print(
            f"HEAPWATCH_BUILD_IDENTIFIER={build_identifier} "
            f"(header {'updated' if changed else 'unchanged'})"
        )
    else:
        print(build_identifier)


if __name__ == "__main__":
    main()
