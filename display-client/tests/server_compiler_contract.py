from __future__ import annotations

import argparse
import hashlib
import subprocess
import tempfile
from datetime import UTC, datetime
from pathlib import Path

from smart_signage.content import (
    AudioPolicy,
    ContentSpec,
    DisplayConfig,
    PlaylistConfig,
    PlaylistItem,
    SafeInsets,
    ScheduleConfig,
    ScheduleEvent,
    SubtitleConfig,
    SubtitleLine,
    SubtitleMessage,
    SubtitleSpan,
    compile_content_bundle,
)
from smart_signage.storage import canonical_json_bytes


def record(path: str, content: bytes) -> dict[str, object]:
    return {
        "path": path,
        "sha256": hashlib.sha256(content).hexdigest(),
        "size": len(content),
        "media_type": "application/octet-stream",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=Path, required=True)
    args = parser.parse_args()

    media_path = "media/welcome.mp4"
    media = b"compiler-contract-video"
    spec = ContentSpec(
        display=DisplayConfig(
            layoutMode="dashboard",
            defaultMediaFit="fit",
            safeInsets=SafeInsets(left=0.02, top=0.02, right=0.02, bottom=0.02),
            audio=AudioPolicy(muted=False, volume=0.8),
        ),
        playlist=PlaylistConfig(
            items=[
                PlaylistItem(
                    id="welcome-video",
                    order=10,
                    path=media_path,
                )
            ]
        ),
        schedule=ScheduleConfig(
            events=[
                ScheduleEvent(
                    id="friday-event",
                    title="금요일 행사",
                    location="4층",
                    daysOfWeek=[5],
                    startTime="21:00",
                    endTime="23:00",
                    priority=10,
                )
            ]
        ),
        subtitle=SubtitleConfig(
            schemaVersion=2,
            messages=[
                SubtitleMessage(
                    id="welcome",
                    durationMs=5000,
                    lines=[
                        SubtitleLine(
                            spans=[
                                SubtitleSpan(
                                    text="안내 센터",
                                    size="large",
                                    marks=["bold", "underline"],
                                ),
                                SubtitleSpan(text="에 오신 여러분을"),
                            ]
                        ),
                        SubtitleLine(
                            spans=[
                                SubtitleSpan(
                                    text="환영합니다.",
                                    marks=["italic"],
                                )
                            ]
                        ),
                    ],
                )
            ]
        ),
    )
    compiled = compile_content_bundle(spec, {media_path})
    payloads = dict(compiled)
    payloads[media_path] = media
    revision = 1
    generation = 1
    manifest = {
        "schema_version": 1,
        "revision": revision,
        "files": [
            record(path, content)
            for path, content in sorted(payloads.items())
        ],
    }
    manifest_bytes = canonical_json_bytes(manifest)
    manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
    bundle_id = f"r{revision}-{manifest_sha256[:16]}"
    now = datetime.now(UTC).isoformat()

    with tempfile.TemporaryDirectory(prefix="signage-display-contract-") as temp:
        root = Path(temp)
        release = root / "releases" / bundle_id
        release.mkdir(parents=True)
        for path, content in payloads.items():
            destination = release / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)
        (release / ".manifest.json").write_bytes(manifest_bytes)
        (release / ".complete").write_bytes(
            canonical_json_bytes(
                {
                    "schemaVersion": 1,
                    "bundleId": bundle_id,
                    "revision": revision,
                    "manifestSha256": manifest_sha256,
                    "completedAt": now,
                }
            )
        )
        (root / "active.json").write_bytes(
            canonical_json_bytes(
                {
                    "schemaVersion": 1,
                    "bundleId": bundle_id,
                    "path": f"releases/{bundle_id}",
                    "revision": revision,
                    "generation": generation,
                    "manifestSha256": manifest_sha256,
                    "activatedAt": now,
                }
            )
        )
        completed = subprocess.run(
            [str(args.checker), str(root)],
            check=False,
        )
        return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
