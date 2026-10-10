"""Public synthetic content generator; not part of the field server."""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import shutil
import subprocess
import tempfile
from pathlib import Path

from smart_signage.agent import ReleaseInstaller
from smart_signage.content import ContentSpec, compile_content_bundle
from smart_signage.storage import canonical_json_bytes


def sample_payloads(media: bytes = b"synthetic-contract-fixture") -> dict[str, bytes]:
    spec = ContentSpec.model_validate({
        "playlist": {"items": [{"id": "demo", "order": 0, "path": "media/demo.mp4"}]},
        "schedule": {"events": [{
            "id": "demo-event", "title": "공개 예제 행사", "location": "4층",
            "daysOfWeek": [1, 2, 3, 4, 5, 6, 7], "startTime": "09:00", "endTime": "18:00",
        }]},
        "subtitle": {"messages": [{"id": "demo-message", "text": "합성 콘텐츠 예제입니다.", "durationMs": 5000}]},
    })
    return {**compile_content_bundle(spec, {"media/demo.mp4"}), "media/demo.mp4": media}


def manifest_for(payloads: dict[str, bytes], revision: int = 1) -> tuple[dict, bytes, str]:
    manifest = {"schema_version": 1, "revision": revision, "files": [
        {"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
         "media_type": "application/octet-stream"}
        for name, data in sorted(payloads.items())
    ]}
    raw = canonical_json_bytes(manifest)
    return manifest, raw, hashlib.sha256(raw).hexdigest()


async def install_sample(root: Path, payloads: dict[str, bytes]) -> None:
    manifest, raw, digest = manifest_for(payloads)
    installer = ReleaseInstaller(root)

    async def download(_revision, record, target):
        target.write_bytes(payloads[record["path"]])

    async def report(_state, _progress, _error):
        pass

    prepared = await installer.prepare(1, manifest, raw, digest, download, report)
    installer.activate(prepared, 1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("display-client/examples/content-root"))
    parser.add_argument("--contract-only", action="store_true",
                        help="Use non-playable bytes for contract/QML-binding tests; no ffmpeg needed")
    args = parser.parse_args()
    media = b"synthetic-contract-fixture"
    if not args.contract_only:
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            parser.error("ffmpeg is required for generated video; use --contract-only for tests")
        with tempfile.TemporaryDirectory(prefix="signage-synthetic-") as temp:
            output = Path(temp) / "demo.mp4"
            subprocess.run([ffmpeg, "-loglevel", "error", "-f", "lavfi", "-i",
                            "testsrc2=size=640x360:rate=15", "-t", "3", "-c:v", "libx264",
                            "-pix_fmt", "yuv420p", str(output)], check=True)
            media = output.read_bytes()
    asyncio.run(install_sample(args.root, sample_payloads(media)))
    print(f"Generated synthetic content at {args.root}")


if __name__ == "__main__":
    main()
