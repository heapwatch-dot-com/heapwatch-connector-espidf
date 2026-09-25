# Copyright 2026 Heapwatch
# SPDX-License-Identifier: Apache-2.0
#
# Build multipart/form-data bodies for Heapwatch firmware CI upload.

from __future__ import annotations

import mimetypes
import uuid
from typing import Mapping


def encode_multipart_form_data(
    fields: Mapping[str, str],
    files: Mapping[str, tuple[str, bytes, str]],
) -> tuple[bytes, str]:
    """
    Encode multipart form fields and file parts.

    File map keys are used as the multipart field names (e.g. flash_files[0x0]).
  """
    boundary = uuid.uuid4().hex
    lines: list[bytes] = []

    for name, value in fields.items():
        lines.append(f"--{boundary}\r\n".encode("ascii"))
        lines.append(f"Content-Disposition: form-data; name=\"{name}\"\r\n\r\n".encode("ascii"))
        lines.append(value.encode("utf-8"))
        lines.append(b"\r\n")

    for field_name, (filename, content, mime_type) in files.items():
        lines.append(f"--{boundary}\r\n".encode("ascii"))
        lines.append(
            (
                f"Content-Disposition: form-data; name=\"{field_name}\"; "
                f"filename=\"{filename}\"\r\n"
            ).encode("ascii")
        )
        lines.append(f"Content-Type: {mime_type}\r\n\r\n".encode("ascii"))
        lines.append(content)
        lines.append(b"\r\n")

    lines.append(f"--{boundary}--\r\n".encode("ascii"))

    body = b"".join(lines)
    content_type = f"multipart/form-data; boundary={boundary}"

    return body, content_type


def guess_mime_type(filename: str) -> str:
    guessed, _ = mimetypes.guess_type(filename)

    if guessed:
        return guessed

    if filename.endswith(".bin") or filename.endswith(".elf"):
        return "application/octet-stream"

    if filename.endswith(".json"):
        return "application/json"

    if filename.endswith(".csv"):
        return "text/csv"

    return "application/octet-stream"
