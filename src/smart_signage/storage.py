from __future__ import annotations

import asyncio
import hashlib
import json
import os
import tempfile
import uuid
from collections.abc import AsyncIterable
from pathlib import Path, PurePosixPath
from typing import Any


def normalize_release_path(raw_path: str) -> str:
    if not raw_path or "\x00" in raw_path or "\\" in raw_path:
        raise ValueError("Invalid release path")
    path = PurePosixPath(raw_path)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise ValueError("Release path must be a safe relative POSIX path")
    normalized = path.as_posix()
    if normalized.startswith("."):
        raise ValueError("Hidden release paths are not allowed")
    return normalized


def canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


class BlobStore:
    def __init__(self, root: Path, max_file_bytes: int) -> None:
        self.root = root
        self.blob_dir = root / "blobs"
        self.manifest_dir = root / "manifests"
        self.tmp_dir = root / "tmp"
        self.max_file_bytes = max_file_bytes
        self._upload_locks: dict[str, asyncio.Lock] = {}
        for path in (self.blob_dir, self.manifest_dir, self.tmp_dir):
            path.mkdir(parents=True, exist_ok=True)

    def blob_path(self, sha256: str) -> Path:
        if len(sha256) != 64 or any(char not in "0123456789abcdef" for char in sha256):
            raise ValueError("Invalid SHA256")
        return self.blob_dir / sha256[:2] / sha256

    async def put_stream(self, chunks: AsyncIterable[bytes]) -> tuple[str, int]:
        digest = hashlib.sha256()
        size = 0
        fd, temp_name = tempfile.mkstemp(prefix="upload-", dir=self.tmp_dir)
        temp_path = Path(temp_name)
        try:
            with os.fdopen(fd, "wb") as output:
                async for chunk in chunks:
                    if not chunk:
                        continue
                    size += len(chunk)
                    if size > self.max_file_bytes:
                        raise ValueError("File exceeds configured size limit")
                    digest.update(chunk)
                    output.write(chunk)
                output.flush()
                os.fsync(output.fileno())
            sha256 = digest.hexdigest()
            destination = self.blob_path(sha256)
            destination.parent.mkdir(parents=True, exist_ok=True)
            self._promote_blob(temp_path, destination, sha256, size)
            return sha256, size
        except Exception:
            temp_path.unlink(missing_ok=True)
            raise

    def put_file(self, source: Path) -> tuple[str, int]:
        if source.is_symlink() or not source.is_file():
            raise ValueError("Source file is missing or unsafe")
        size = source.stat().st_size
        if size > self.max_file_bytes:
            raise ValueError("File exceeds configured size limit")
        digest = hashlib.sha256()
        fd, temp_name = tempfile.mkstemp(prefix="local-", dir=self.tmp_dir)
        temp_path = Path(temp_name)
        try:
            with source.open("rb") as input_file, os.fdopen(fd, "wb") as output:
                for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
                    digest.update(chunk)
                    output.write(chunk)
                output.flush()
                os.fsync(output.fileno())
            sha256 = digest.hexdigest()
            destination = self.blob_path(sha256)
            destination.parent.mkdir(parents=True, exist_ok=True)
            self._promote_blob(temp_path, destination, sha256, size)
            return sha256, size
        except Exception:
            temp_path.unlink(missing_ok=True)
            raise

    def verify_blob(self, sha256: str, size: int) -> bool:
        return self._blob_matches(self.blob_path(sha256), sha256, size)

    def write_manifest(self, revision: int, manifest: dict[str, Any]) -> tuple[bytes, str]:
        content = canonical_json_bytes(manifest)
        digest = hashlib.sha256(content).hexdigest()
        destination = self.manifest_dir / f"{revision}.json"
        fd, temp_name = tempfile.mkstemp(
            prefix=f"manifest-{revision}-", dir=self.tmp_dir
        )
        temp_path = Path(temp_name)
        try:
            with os.fdopen(fd, "wb") as output:
                output.write(content)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temp_path, destination)
        finally:
            temp_path.unlink(missing_ok=True)
        return content, digest

    def read_manifest(self, revision: int) -> bytes:
        return (self.manifest_dir / f"{revision}.json").read_bytes()

    def create_upload(
        self,
        revision: int,
        path: str,
        size: int,
        media_type: str | None,
        expected_sha256: str | None,
    ) -> dict[str, Any]:
        if size < 0 or size > self.max_file_bytes:
            raise ValueError("Invalid upload size")
        if expected_sha256 is not None:
            self.blob_path(expected_sha256)
        upload_id = uuid.uuid4().hex
        metadata = {
            "upload_id": upload_id,
            "revision": revision,
            "path": path,
            "size": size,
            "media_type": media_type,
            "expected_sha256": expected_sha256,
            "complete": False,
        }
        self._upload_part_path(upload_id).touch(exist_ok=False)
        self._write_upload_metadata(upload_id, metadata)
        return {**metadata, "offset": 0}

    def get_upload(self, upload_id: str) -> dict[str, Any]:
        metadata = self._read_upload_metadata(upload_id)
        if metadata.get("complete"):
            return {**metadata, "offset": int(metadata["size"])}
        part_path = self._upload_part_path(upload_id)
        if not part_path.is_file():
            raise FileNotFoundError("Upload data not found")
        offset = part_path.stat().st_size
        if offset > int(metadata["size"]):
            raise ValueError("Upload data exceeds declared size")
        return {**metadata, "offset": offset}

    async def append_upload(
        self,
        upload_id: str,
        expected_offset: int,
        chunks: AsyncIterable[bytes],
    ) -> dict[str, Any]:
        lock = self._upload_locks.setdefault(upload_id, asyncio.Lock())
        async with lock:
            metadata = self.get_upload(upload_id)
            if metadata.get("complete"):
                if expected_offset == int(metadata["size"]):
                    return metadata
                raise ValueError("Upload is already complete")
            if expected_offset != int(metadata["offset"]):
                raise ValueError(f"Upload offset mismatch: expected {metadata['offset']}")

            part_path = self._upload_part_path(upload_id)
            declared_size = int(metadata["size"])
            output = part_path.open("ab")
            try:
                async for chunk in chunks:
                    if not chunk:
                        continue
                    if output.tell() + len(chunk) > declared_size:
                        raise ValueError("Upload exceeds declared size")
                    output.write(chunk)
            finally:
                output.flush()
                os.fsync(output.fileno())
                output.close()

            offset = part_path.stat().st_size
            if offset < declared_size:
                return {**metadata, "offset": offset}

            digest = hashlib.sha256()
            with part_path.open("rb") as input_file:
                for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
                    digest.update(chunk)
            sha256 = digest.hexdigest()
            expected_sha256 = metadata.get("expected_sha256")
            if expected_sha256 and sha256 != expected_sha256:
                part_path.unlink(missing_ok=True)
                self._upload_metadata_path(upload_id).unlink(missing_ok=True)
                raise ValueError("Upload SHA256 mismatch")

            destination = self.blob_path(sha256)
            destination.parent.mkdir(parents=True, exist_ok=True)
            self._promote_blob(part_path, destination, sha256, declared_size)
            completed = {
                **metadata,
                "sha256": sha256,
                "complete": True,
            }
            self._write_upload_metadata(upload_id, completed)
            return {**completed, "offset": declared_size}

    @staticmethod
    def _blob_matches(path: Path, expected_sha256: str, expected_size: int) -> bool:
        if path.is_symlink() or not path.is_file():
            return False
        if path.stat().st_size != expected_size:
            return False
        digest = hashlib.sha256()
        with path.open("rb") as input_file:
            for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest() == expected_sha256

    def _promote_blob(
        self,
        source: Path,
        destination: Path,
        expected_sha256: str,
        expected_size: int,
    ) -> None:
        if self._blob_matches(destination, expected_sha256, expected_size):
            source.unlink()
            return
        os.replace(source, destination)

    def _upload_metadata_path(self, upload_id: str) -> Path:
        self._validate_upload_id(upload_id)
        return self.tmp_dir / f"upload-{upload_id}.json"

    def _upload_part_path(self, upload_id: str) -> Path:
        self._validate_upload_id(upload_id)
        return self.tmp_dir / f"upload-{upload_id}.part"

    @staticmethod
    def _validate_upload_id(upload_id: str) -> None:
        if len(upload_id) != 32 or any(
            character not in "0123456789abcdef" for character in upload_id
        ):
            raise FileNotFoundError("Upload not found")

    def _read_upload_metadata(self, upload_id: str) -> dict[str, Any]:
        try:
            value = json.loads(
                self._upload_metadata_path(upload_id).read_text(encoding="utf-8")
            )
        except (FileNotFoundError, json.JSONDecodeError, OSError) as exc:
            raise FileNotFoundError("Upload not found") from exc
        if not isinstance(value, dict) or value.get("upload_id") != upload_id:
            raise ValueError("Invalid upload metadata")
        return value

    def _write_upload_metadata(
        self,
        upload_id: str,
        metadata: dict[str, Any],
    ) -> None:
        destination = self._upload_metadata_path(upload_id)
        fd, temp_name = tempfile.mkstemp(
            prefix=f"upload-{upload_id}-",
            dir=self.tmp_dir,
        )
        temp_path = Path(temp_name)
        try:
            with os.fdopen(fd, "wb") as output:
                output.write(canonical_json_bytes(metadata))
                output.flush()
                os.fsync(output.fileno())
            os.replace(temp_path, destination)
        finally:
            temp_path.unlink(missing_ok=True)
