# Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
#
# Runs after the application .bin exists:
#  1. Compute composite HEAPWATCH_BUILD_IDENTIFIER and refresh heapwatch_build_metadata.h
#  2. Re-link once when the fingerprint header changed
#  3. Upload firmware bundle to Heapwatch (non-fatal locally unless HEAPWATCH_UPLOAD_STRICT=1)

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from compute_build_identifier import compute_build_identifier
from heapwatch_build_metadata import update_build_identifier
from post_build_upload_firmware import upload_firmware_bundle


def is_strict_upload() -> bool:
    return os.environ.get("HEAPWATCH_UPLOAD_STRICT", "").strip().lower() in {
        "1",
        "true",
        "yes",
        "on",
    }


def rebuild_project(build_dir: Path, project_name: str) -> None:
    """
    Re-link the ELF after HEAPWATCH_BUILD_IDENTIFIER changes, then regenerate the .bin.

    Upload ships the .bin (not the ELF). Building only `{project}.elf` leaves a
    stale .bin whose embedded build identifier no longer matches metadata — OTA
    then boots the new slot but reports the old fingerprint and fails identity match.
    """
    ret = subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", f"{project_name}.elf"],
        check=False,
    )
    if ret.returncode != 0:
        raise RuntimeError(
            f"Rebuild after fingerprint update failed with exit code {ret.returncode}"
        )

    ret = subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "gen_project_binary"],
        check=False,
    )
    if ret.returncode != 0:
        raise RuntimeError(
            "Regenerating application .bin after fingerprint update failed with exit code "
            f"{ret.returncode}"
        )


def finalize_fingerprint(
    build_dir: Path,
    project_name: str,
    metadata_header: Path,
    sdkconfig: Path,
) -> str:
    elf_path = build_dir / f"{project_name}.elf"
    bin_path = build_dir / f"{project_name}.bin"
    partition_path = build_dir / "partition_table" / "partition-table.bin"

    for path, label in (
        (elf_path, "ELF"),
        (bin_path, "application .bin"),
        (partition_path, "partition-table.bin"),
        (sdkconfig, "sdkconfig"),
    ):
        if not path.is_file():
            raise FileNotFoundError(f"Missing {label}: {path}")

    build_identifier = compute_build_identifier(elf_path, partition_path, sdkconfig)
    if update_build_identifier(metadata_header, build_identifier):
        rebuild_project(build_dir, project_name)

        for path, label in ((elf_path, "ELF"), (bin_path, "application .bin")):
            if not path.is_file():
                raise FileNotFoundError(f"Missing {label} after rebuild: {path}")

        build_identifier = compute_build_identifier(elf_path, partition_path, sdkconfig)
        update_build_identifier(metadata_header, build_identifier)
        rebuild_project(build_dir, project_name)

    bin_bytes = bin_path.read_bytes()
    if build_identifier.encode("ascii") not in bin_bytes:
        raise RuntimeError(
            "Application .bin does not embed HEAPWATCH_BUILD_IDENTIFIER "
            f"{build_identifier!r} — refusing upload (stale .bin after fingerprint refresh)."
        )

    return build_identifier


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Finalize HEAPWATCH_BUILD_IDENTIFIER after .bin exists and upload firmware.",
    )
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--metadata-header", type=Path, required=True)
    parser.add_argument("--sdkconfig", type=Path, required=True)
    parser.add_argument("--project-name", type=str, required=True)
    args = parser.parse_args()

    try:
        build_identifier = finalize_fingerprint(
            args.build_dir,
            args.project_name,
            args.metadata_header,
            args.sdkconfig,
        )
        upload_firmware_bundle(
            build_dir=args.build_dir,
            metadata_header=args.metadata_header,
            project_name=args.project_name,
            build_identifier=build_identifier,
        )
    except FileNotFoundError as err:
        print(f"ERROR: {err}", file=sys.stderr)
        sys.exit(1)
    except RuntimeError as err:
        print(f"ERROR: {err}", file=sys.stderr)
        sys.exit(1)
    except Exception as err:  # noqa: BLE001 - top-level build hook
        print(f"ERROR: {err}", file=sys.stderr)
        sys.exit(1 if is_strict_upload() else 0)


if __name__ == "__main__":
    main()
