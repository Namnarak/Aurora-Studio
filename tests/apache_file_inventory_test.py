#!/usr/bin/env python3
"""Validate Aurora Studio's explicit mixed-license file inventory."""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"Apache file inventory contract failed: {message}")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: apache_file_inventory_test.py SOURCE_ROOT")
    root = Path(sys.argv[1]).resolve()
    inventory = root / "LICENSES/APACHE-FILES.txt"
    hash_re = re.compile(r"^[0-9a-f]{40}$")
    sha_re = re.compile(r"^[0-9a-f]{64}$")
    rows: list[tuple[str, str, str, str, str, str]] = []

    for line_number, line in enumerate(inventory.read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 6:
            fail(f"line {line_number} has {len(fields)} columns, expected 6")
        path, origin, revisions, source_path, license_name, digest = fields
        if license_name != "Apache-2.0":
            fail(f"{path} is not marked Apache-2.0")
        if not sha_re.fullmatch(digest):
            fail(f"{path} has a malformed SHA-256")
        if not source_path or source_path.startswith("/") or ".." in Path(source_path).parts:
            fail(f"{path} has an unsafe source path")
        revs = re.findall(r"(?<![0-9a-f])[0-9a-f]{40}(?![0-9a-f])", revisions)
        if not revs or any(not hash_re.fullmatch(rev) for rev in revs):
            fail(f"{path} does not identify a full 40-character source revision")
        if origin.startswith("https://github.com/komaruworld/mocktail/blob/"):
            match = re.fullmatch(
                r"https://github\.com/komaruworld/mocktail/blob/([0-9a-f]{40})/(.+)",
                origin,
            )
            if not match:
                fail(f"{path} has a non-pinned Mocktail source URL")
            if match.group(1) not in revs or match.group(2) != source_path:
                fail(f"{path} URL does not match its revision/source path columns")
        elif origin != "NamKrub-authored local source checkout":
            fail(f"{path} has an unrecognized provenance origin")
        file_path = root / path
        if not file_path.is_file():
            fail(f"listed file is missing: {path}")
        actual_digest = hashlib.sha256(file_path.read_bytes()).hexdigest()
        if actual_digest != digest:
            fail(f"listed SHA-256 does not match candidate file: {path}")
        if path.startswith("third_party/"):
            fail(f"third-party file is incorrectly in the first-party inventory: {path}")
        candidate_text = file_path.read_text(errors="ignore")
        spdx = re.search(
            r"(?m)^\s*(?://|#|/\*|\*)?\s*SPDX-License-Identifier:\s*([^\s*]+)",
            candidate_text,
        )
        if spdx and spdx.group(1) != "Apache-2.0":
            fail(f"listed candidate has a conflicting SPDX license: {path}")
        rows.append(tuple(fields))

    paths = [row[0] for row in rows]
    if len(paths) != len(set(paths)):
        fail("candidate paths are duplicated")
    if not rows:
        fail("inventory has no file rows")

    # SPDX-marked Apache files are first-party license claims and must be mapped.
    for path in root.rglob("*"):
        if not path.is_file() or "third_party" in path.relative_to(root).parts:
            continue
        try:
            content = path.read_text(errors="ignore")
        except OSError:
            continue
        if re.search(
            r"(?m)^\s*(?://|#|/\*|\*)?\s*SPDX-License-Identifier:\s*Apache-2\.0\b",
            content,
        ):
            relative = path.relative_to(root).as_posix()
            if relative not in set(paths):
                fail(f"Apache SPDX file is not mapped: {relative}")

    provenance = (root / "LICENSES/PROVENANCE.md").read_text()
    if "2451264696d83bdb91d1517a7107273213178aca" not in provenance:
        fail("provenance document lacks the full Mocktail snapshot")
    if "https://github.com/komaruworld/mocktail/commit/2451264696d83bdb91d1517a7107273213178aca" not in provenance:
        fail("provenance document lacks the pinned Mocktail commit URL")
    if "https://github.com/vinegarhq/vinegar" not in provenance:
        fail("provenance document lacks the Vinegar source URL")
    if "does not claim a specific Vinegar source revision" not in provenance:
        fail("provenance document must disclose that no Vinegar revision is verified")
    print(f"Apache inventory contract passed: {len(rows)} unique file rows")


if __name__ == "__main__":
    main()
