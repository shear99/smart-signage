"""Selected production content-sync code; see docs/provenance.md for omissions."""
from __future__ import annotations

import asyncio
import errno
import hashlib
import json
import os
import random
import re
import shutil
import stat
import tempfile
import uuid
from collections.abc import Awaitable, Callable
from contextlib import suppress
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from urllib.parse import quote, urlparse, urlunparse

import httpx
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed

from . import __version__
from .storage import canonical_json_bytes, normalize_release_path

DownloadFile = Callable[[int, dict[str, Any], Path], Awaitable[None]]
StatusCallback = Callable[[str, int, str | None], Awaitable[None]]

_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_BUNDLE_ID_RE = re.compile(r"^r([1-9][0-9]*)-([0-9a-f]{16})$")
_STAGING_NAME_RE = re.compile(r"^\.r[1-9][0-9]*-[0-9a-f]{16}\.staging$")
_RETRYABLE_HTTP_STATUS = {408, 425, 429, 500, 502, 503, 504}
_DEFAULT_QUARANTINE_RETENTION = 1
_DEFAULT_STAGING_RETENTION = 1
_DEFAULT_AUXILIARY_MAX_AGE_SECONDS = 7 * 24 * 60 * 60


def _utc_now() -> str:
    return datetime.now(UTC).isoformat()








def _fsync_directory(path: Path) -> None:
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    except OSError as exc:
        if exc.errno not in {errno.EBADF, errno.EINVAL, errno.ENOTSUP}:
            raise
    finally:
        os.close(descriptor)


def _atomic_bytes(path: Path, payload: bytes, *, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temp_name = tempfile.mkstemp(prefix=f".{path.name}-", dir=path.parent)
    temp_path = Path(temp_name)
    try:
        os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temp_path, path)
        _fsync_directory(path.parent)
    finally:
        temp_path.unlink(missing_ok=True)


def _atomic_json(path: Path, value: dict[str, Any], *, mode: int = 0o600) -> None:
    _atomic_bytes(path, canonical_json_bytes(value), mode=mode)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


async def _run_blocking(
    function: Callable[..., Any],
    /,
    *args: Any,
    **kwargs: Any,
) -> Any:
    """Run filesystem work off-loop and never leave it running after cancellation."""

    worker = asyncio.create_task(asyncio.to_thread(function, *args, **kwargs))
    try:
        return await asyncio.shield(worker)
    except asyncio.CancelledError:
        while not worker.done():
            try:
                await asyncio.shield(worker)
            except asyncio.CancelledError:
                continue
            except Exception:
                break
        if worker.done() and not worker.cancelled():
            with suppress(Exception):
                worker.result()
        raise


@dataclass(frozen=True)
class AgentConfig:
    server_url: str
    device_id: str
    token: str
    runtime_root: Path
    heartbeat_seconds: int = 30
    shadow_mode: bool = False
    retry_delays: tuple[float, ...] = (5, 15, 60, 300, 900)
    retry_jitter_ratio: float = 0.2
    minimum_free_bytes: int = 64 * 1024 * 1024
    keep_releases: int = 3

    @property
    def websocket_url(self) -> str:
        parsed = urlparse(self.server_url)
        scheme = "wss" if parsed.scheme == "https" else "ws"
        return urlunparse(
            (scheme, parsed.netloc, f"/ws/device/{quote(self.device_id)}", "", "", "")
        )


@dataclass(frozen=True)
class DesiredRelease:
    revision: int
    generation: int
    manifest_sha256: str | None = None

    @classmethod
    def from_message(cls, message: dict[str, Any]) -> DesiredRelease | None:
        revision = message.get(
            "revision",
            message.get("desired_revision"),
        )
        generation = message.get(
            "generation",
            message.get("desired_generation"),
        )
        if revision is None:
            return None
        if (
            isinstance(revision, bool)
            or not isinstance(revision, int)
            or revision <= 0
            or isinstance(generation, bool)
            or not isinstance(generation, int)
            or generation <= 0
        ):
            raise ValueError("Invalid desired release identity")
        manifest_sha256 = message.get("manifest_sha256")
        if manifest_sha256 is not None:
            manifest_sha256 = str(manifest_sha256)
            if not _SHA256_RE.fullmatch(manifest_sha256):
                raise ValueError("Invalid desired manifest SHA256")
        return cls(
            revision=revision,
            generation=generation,
            manifest_sha256=manifest_sha256,
        )




@dataclass(frozen=True)
class ManifestDocument:
    value: dict[str, Any]
    content: bytes
    sha256: str


@dataclass(frozen=True)
class PreparedRelease:
    bundle_id: str
    revision: int
    manifest_sha256: str
    path: Path


class ReleaseInstaller:
    def __init__(
        self,
        runtime_root: Path,
        keep_releases: int | None = 3,
        *,
        minimum_free_bytes: int = 64 * 1024 * 1024,
        keep_quarantine: int = _DEFAULT_QUARANTINE_RETENTION,
        keep_staging: int = _DEFAULT_STAGING_RETENTION,
        auxiliary_max_age_seconds: int = _DEFAULT_AUXILIARY_MAX_AGE_SECONDS,
    ) -> None:
        self.runtime_root = runtime_root
        self.releases_dir = runtime_root / "releases"
        self.quarantine_dir = self.releases_dir / ".quarantine"
        self.data_path = runtime_root / "data"
        self.active_path = runtime_root / "active.json"
        self.state_path = runtime_root / ".sync-agent-state.json"
        self.minimum_free_bytes = minimum_free_bytes
        configured_keep_releases = 3 if keep_releases is None else keep_releases
        self.keep_releases = max(1, configured_keep_releases)
        self.keep_quarantine = max(0, keep_quarantine)
        self.keep_staging = max(0, keep_staging)
        self.auxiliary_max_age_seconds = max(0, auxiliary_max_age_seconds)
        self.startup_error: str | None = None
        self.cleanup_error: str | None = None
        self._active_state: dict[str, Any] = {}
        self._in_progress_staging: Path | None = None
        self._mutation_lock = asyncio.Lock()
        self.runtime_root.mkdir(parents=True, exist_ok=True)
        self.releases_dir.mkdir(parents=True, exist_ok=True)
        self._load_and_validate_active()

    def current_revision(self) -> int | None:
        revision = self._active_state.get("revision")
        return revision if isinstance(revision, int) else None

    def current_generation(self) -> int:
        generation = self._active_state.get("generation")
        return generation if isinstance(generation, int) else 0

    def verified_revision(self) -> int | None:
        revision = self.shadow_state().get("verified_revision")
        return revision if isinstance(revision, int) else None

    def verified_generation(self) -> int:
        generation = self.shadow_state().get("verified_generation")
        return generation if isinstance(generation, int) else 0

    def current_state(self) -> dict[str, Any]:
        return dict(self._active_state)

    def shadow_state(self) -> dict[str, Any]:
        try:
            state = json.loads(self.state_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, OSError, json.JSONDecodeError):
            return {}
        return state if isinstance(state, dict) else {}

    async def prepare(
        self,
        revision: int,
        manifest: dict[str, Any],
        manifest_content: bytes,
        manifest_sha256: str,
        download_file: DownloadFile,
        report: StatusCallback,
    ) -> PreparedRelease:
        async with self._mutation_lock:
            return await self._prepare_locked(
                revision,
                manifest,
                manifest_content,
                manifest_sha256,
                download_file,
                report,
            )

    async def _prepare_locked(
        self,
        revision: int,
        manifest: dict[str, Any],
        manifest_content: bytes,
        manifest_sha256: str,
        download_file: DownloadFile,
        report: StatusCallback,
    ) -> PreparedRelease:
        if hashlib.sha256(manifest_content).hexdigest() != manifest_sha256:
            raise ValueError("Manifest content SHA256 mismatch")
        records = self._validate_manifest(revision, manifest)
        bundle_id = self._bundle_id(revision, manifest_sha256)
        final_dir = self.releases_dir / bundle_id
        staging_dir = self.releases_dir / f".{bundle_id}.staging"
        await _run_blocking(
            self._maintain_auxiliary_storage,
            protected_staging=staging_dir,
        )

        if final_dir.exists() or final_dir.is_symlink():
            try:
                await _run_blocking(
                    self._verify_complete_release,
                    final_dir,
                    revision=revision,
                    manifest_sha256=manifest_sha256,
                    bundle_id=bundle_id,
                )
            except Exception:
                self._quarantine_release(final_dir, bundle_id)

        if not final_dir.exists():
            self._in_progress_staging = staging_dir
            try:
                await _run_blocking(self._prepare_staging_directory, staging_dir)
                await _run_blocking(
                    self._prune_completed_releases,
                    self._protected_bundle_ids(),
                    retention_limit=max(1, self.keep_releases - 1),
                )
                await _run_blocking(
                    self._preflight_disk_space,
                    staging_dir,
                    records,
                )
                total = len(records)
                for index, file_record in enumerate(records, start=1):
                    path = normalize_release_path(str(file_record["path"]))
                    destination = staging_dir / path
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    await _run_blocking(
                        self._discard_invalid_staging_file,
                        destination,
                        file_record,
                    )
                    await download_file(revision, file_record, destination)
                    try:
                        await _run_blocking(
                            self._verify_file,
                            destination,
                            file_record,
                        )
                    except Exception:
                        destination.unlink(missing_ok=True)
                        raise
                    progress = max(1, min(95, int(index * 95 / total)))
                    await report("downloading", progress, None)

                await report("verifying", 98, None)
                _atomic_bytes(
                    staging_dir / ".manifest.json",
                    manifest_content,
                    mode=0o640,
                )
                _atomic_json(
                    staging_dir / ".complete",
                    {
                        "schemaVersion": 1,
                        "bundleId": bundle_id,
                        "revision": revision,
                        "manifestSha256": manifest_sha256,
                        "completedAt": _utc_now(),
                    },
                    mode=0o640,
                )
                await _run_blocking(
                    self._verify_complete_release,
                    staging_dir,
                    revision=revision,
                    manifest_sha256=manifest_sha256,
                    bundle_id=bundle_id,
                )
                await _run_blocking(self._fsync_tree, staging_dir)
                await _run_blocking(
                    self._seal_release,
                    staging_dir,
                    seal_root=False,
                )
                os.replace(staging_dir, final_dir)
                os.chmod(final_dir, 0o550)
                _fsync_directory(self.releases_dir)
            finally:
                if self._in_progress_staging == staging_dir:
                    self._in_progress_staging = None

        return PreparedRelease(
            bundle_id=bundle_id,
            revision=revision,
            manifest_sha256=manifest_sha256,
            path=final_dir,
        )

    def activate(self, prepared: PreparedRelease, generation: int) -> None:
        expected_path = self.releases_dir / prepared.bundle_id
        if (
            prepared.path != expected_path
            or prepared.path.is_symlink()
            or not prepared.path.is_dir()
        ):
            raise ValueError("Prepared release path is no longer valid")
        active = {
            "schemaVersion": 1,
            "bundleId": prepared.bundle_id,
            "path": f"releases/{prepared.bundle_id}",
            "revision": prepared.revision,
            "generation": generation,
            "manifestSha256": prepared.manifest_sha256,
            "activatedAt": _utc_now(),
        }
        # The data symlink is a compatibility alias for the legacy display
        # process. Finish that best-effort compatibility transition first so
        # active.json remains the sole v2 commit point.
        self._switch_legacy_data_link(prepared.path)
        _atomic_json(self.active_path, active, mode=0o440)
        self._active_state = active
        self.startup_error = None

    def mark_verified(self, prepared: PreparedRelease, generation: int) -> None:
        state = self.shadow_state()
        state.update(
            {
                "schema_version": 1,
                "verified_revision": prepared.revision,
                "verified_generation": generation,
                "verified_bundle_id": prepared.bundle_id,
                "manifest_sha256": prepared.manifest_sha256,
                "verified_at": _utc_now(),
            }
        )
        _atomic_json(self.state_path, state, mode=0o600)

    async def maintain_storage(self) -> None:
        async with self._mutation_lock:
            await _run_blocking(
                self._maintain_storage,
                self._protected_bundle_ids(),
            )

    async def mark_verified_and_maintain(
        self,
        prepared: PreparedRelease,
        generation: int,
    ) -> None:
        async with self._mutation_lock:
            await _run_blocking(
                self._mark_verified_with_maintenance,
                prepared,
                generation,
            )

    def _mark_verified_with_maintenance(
        self,
        prepared: PreparedRelease,
        generation: int,
    ) -> None:
        self.mark_verified(prepared, generation)
        self._maintain_storage(self._protected_bundle_ids())

    async def install(
        self,
        revision: int,
        generation: int,
        manifest: dict[str, Any],
        download_file: DownloadFile,
        report: StatusCallback,
        *,
        activate: bool = True,
        manifest_content: bytes | None = None,
        manifest_sha256: str | None = None,
    ) -> PreparedRelease:
        content = manifest_content or canonical_json_bytes(manifest)
        actual_sha256 = hashlib.sha256(content).hexdigest()
        if manifest_sha256 is not None and actual_sha256 != manifest_sha256:
            raise ValueError("Manifest SHA256 mismatch")
        prepared = await self.prepare(
            revision,
            manifest,
            content,
            actual_sha256,
            download_file,
            report,
        )
        if activate:
            self.activate(prepared, generation)
            await self.maintain_storage()
        else:
            await self.mark_verified_and_maintain(prepared, generation)
        return prepared

    @staticmethod
    def _bundle_id(revision: int, manifest_sha256: str) -> str:
        return f"r{revision}-{manifest_sha256[:16]}"

    @staticmethod
    def _validate_manifest(
        revision: int,
        manifest: dict[str, Any],
    ) -> list[dict[str, Any]]:
        if manifest.get("schema_version") != 1 or manifest.get("revision") != revision:
            raise ValueError("Manifest schema or revision mismatch")
        files = manifest.get("files")
        if not isinstance(files, list) or not files:
            raise ValueError("Manifest has no files")
        seen: set[str] = set()
        validated: list[dict[str, Any]] = []
        for file_record in files:
            if not isinstance(file_record, dict):
                raise ValueError("Invalid file record")
            path = normalize_release_path(str(file_record.get("path", "")))
            if path in seen:
                raise ValueError(f"Duplicate manifest path: {path}")
            seen.add(path)
            size = file_record.get("size")
            sha256 = file_record.get("sha256")
            if (
                isinstance(size, bool)
                or not isinstance(size, int)
                or size < 0
                or not isinstance(sha256, str)
                or not _SHA256_RE.fullmatch(sha256)
            ):
                raise ValueError(f"Invalid manifest file metadata: {path}")
            validated.append(file_record)
        return validated

    @staticmethod
    def _verify_file(path: Path, file_record: dict[str, Any]) -> None:
        try:
            metadata = path.lstat()
        except FileNotFoundError as exc:
            raise ValueError(
                f"Downloaded file is missing: {file_record.get('path')}"
            ) from exc
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError(f"Downloaded path is not a regular file: {file_record.get('path')}")
        expected_size = int(file_record["size"])
        if metadata.st_size != expected_size:
            raise ValueError(f"File size mismatch: {file_record.get('path')}")
        if _sha256_file(path) != file_record["sha256"]:
            raise ValueError(f"SHA256 mismatch: {file_record.get('path')}")

    def _verify_complete_release(
        self,
        path: Path,
        *,
        revision: int,
        manifest_sha256: str,
        bundle_id: str,
    ) -> None:
        if path.is_symlink() or not path.is_dir():
            raise ValueError("Release bundle is not a directory")
        manifest_path = path / ".manifest.json"
        complete_path = path / ".complete"
        if manifest_path.is_symlink() or complete_path.is_symlink():
            raise ValueError("Release metadata cannot be symlinks")
        manifest_content = manifest_path.read_bytes()
        if hashlib.sha256(manifest_content).hexdigest() != manifest_sha256:
            raise ValueError("Stored manifest SHA256 mismatch")
        manifest = json.loads(manifest_content)
        if not isinstance(manifest, dict):
            raise ValueError("Stored manifest is not an object")
        records = self._validate_manifest(revision, manifest)
        complete = json.loads(complete_path.read_text(encoding="utf-8"))
        expected_marker = {
            "schemaVersion": 1,
            "bundleId": bundle_id,
            "revision": revision,
            "manifestSha256": manifest_sha256,
        }
        if not isinstance(complete, dict) or any(
            complete.get(key) != value for key, value in expected_marker.items()
        ):
            raise ValueError("Invalid release completion marker")
        for file_record in records:
            release_path = normalize_release_path(str(file_record["path"]))
            self._verify_file(path / release_path, file_record)

    def _load_and_validate_active(self) -> None:
        try:
            active = json.loads(self.active_path.read_text(encoding="utf-8"))
            if not isinstance(active, dict):
                raise ValueError("active.json is not an object")
            expected_keys = {
                "schemaVersion",
                "bundleId",
                "path",
                "revision",
                "generation",
                "manifestSha256",
                "activatedAt",
            }
            revision = active.get("revision")
            generation = active.get("generation")
            bundle_id = active.get("bundleId")
            manifest_sha256 = active.get("manifestSha256")
            activated_at = active.get("activatedAt")
            if (
                set(active) != expected_keys
                or active.get("schemaVersion") != 1
                or isinstance(revision, bool)
                or not isinstance(revision, int)
                or revision <= 0
                or isinstance(generation, bool)
                or not isinstance(generation, int)
                or generation <= 0
                or not isinstance(bundle_id, str)
                or not _BUNDLE_ID_RE.fullmatch(bundle_id)
                or not isinstance(manifest_sha256, str)
                or not _SHA256_RE.fullmatch(manifest_sha256)
                or active.get("path") != f"releases/{bundle_id}"
                or bundle_id != self._bundle_id(revision, manifest_sha256)
                or not isinstance(activated_at, str)
            ):
                raise ValueError("Invalid active.json identity")
            parsed_activated_at = datetime.fromisoformat(activated_at)
            if parsed_activated_at.tzinfo is None:
                raise ValueError("active.json activatedAt must include a timezone")
        except FileNotFoundError:
            return
        except Exception as exc:
            self.startup_error = f"{type(exc).__name__}: {exc}"
            self._active_state = {}
            return

        final_dir = self.releases_dir / bundle_id
        try:
            self._verify_complete_release(
                final_dir,
                revision=revision,
                manifest_sha256=manifest_sha256,
                bundle_id=bundle_id,
            )
        except Exception as exc:
            self.startup_error = f"{type(exc).__name__}: {exc}"
            self._active_state = {}
            if final_dir.exists() and not final_dir.is_symlink():
                self._quarantine_release(final_dir, bundle_id)
            return
        self._active_state = active

    def _quarantine_release(self, final_dir: Path, bundle_id: str) -> None:
        if final_dir.is_symlink():
            raise ValueError("Refusing to quarantine a symlink release")
        if not final_dir.exists():
            return
        self.quarantine_dir.mkdir(parents=True, exist_ok=True)
        destination = self.quarantine_dir / (
            f"{bundle_id}--{datetime.now(UTC).strftime('%Y%m%dT%H%M%S%fZ')}"
            f"--{uuid.uuid4().hex[:8]}"
        )
        # macOS requires write permission on the directory being renamed even
        # when its parent is writable. Only the bundle root is opened; the
        # immutable payload and metadata files remain read-only.
        os.chmod(final_dir, 0o750)
        try:
            os.replace(final_dir, destination)
        except Exception:
            os.chmod(final_dir, 0o550)
            raise
        os.chmod(destination, 0o550)
        _fsync_directory(self.quarantine_dir)
        _fsync_directory(self.releases_dir)

    def _protected_bundle_ids(self) -> set[str]:
        protected: set[str] = set()
        active_bundle_id = self._active_state.get("bundleId")
        if isinstance(active_bundle_id, str) and _BUNDLE_ID_RE.fullmatch(active_bundle_id):
            protected.add(active_bundle_id)
        verified_bundle_id = self.shadow_state().get("verified_bundle_id")
        if isinstance(verified_bundle_id, str) and _BUNDLE_ID_RE.fullmatch(
            verified_bundle_id
        ):
            protected.add(verified_bundle_id)
        return protected

    def _maintain_storage(self, protected_bundle_ids: set[str]) -> None:
        errors: list[str] = []
        operations = (
            lambda: self._prune_completed_releases(protected_bundle_ids),
            lambda: self._prune_auxiliary_directories(
                self.quarantine_dir,
                keep=self.keep_quarantine,
            ),
            lambda: self._prune_auxiliary_directories(
                self.releases_dir,
                keep=self.keep_staging,
                staging_only=True,
                protected=self._in_progress_staging,
            ),
        )
        for operation in operations:
            try:
                operation()
            except Exception as exc:
                errors.append(f"{type(exc).__name__}: {exc}")
        self.cleanup_error = "; ".join(errors) if errors else None

    def _maintain_auxiliary_storage(self, *, protected_staging: Path) -> None:
        errors: list[str] = []
        operations = (
            lambda: self._prune_auxiliary_directories(
                self.quarantine_dir,
                keep=self.keep_quarantine,
            ),
            lambda: self._prune_auxiliary_directories(
                self.releases_dir,
                keep=self.keep_staging,
                staging_only=True,
                protected=protected_staging,
            ),
        )
        for operation in operations:
            try:
                operation()
            except Exception as exc:
                errors.append(f"{type(exc).__name__}: {exc}")
        self.cleanup_error = "; ".join(errors) if errors else None

    def _prune_completed_releases(
        self,
        protected_bundle_ids: set[str],
        *,
        retention_limit: int | None = None,
    ) -> None:
        completed: list[tuple[float, Path]] = []
        for candidate in self.releases_dir.iterdir():
            if (
                candidate.name in protected_bundle_ids
                or candidate.is_symlink()
                or not candidate.is_dir()
                or not _BUNDLE_ID_RE.fullmatch(candidate.name)
            ):
                continue
            timestamp = self._completed_bundle_timestamp(candidate)
            if timestamp is None:
                self._quarantine_release(candidate, candidate.name)
                continue
            completed.append((timestamp, candidate))

        completed.sort(key=lambda item: item[0], reverse=True)
        limit = self.keep_releases if retention_limit is None else retention_limit
        inactive_slots = max(0, limit - len(protected_bundle_ids))
        for _timestamp, candidate in completed[inactive_slots:]:
            self._remove_directory(candidate)

    @staticmethod
    def _completed_bundle_timestamp(path: Path) -> float | None:
        marker_path = path / ".complete"
        try:
            metadata = marker_path.lstat()
            if not stat.S_ISREG(metadata.st_mode):
                return None
            marker = json.loads(marker_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, OSError, json.JSONDecodeError):
            return None
        match = _BUNDLE_ID_RE.fullmatch(path.name)
        if match is None or not isinstance(marker, dict):
            return None
        revision = marker.get("revision")
        manifest_sha256 = marker.get("manifestSha256")
        if (
            marker.get("schemaVersion") != 1
            or marker.get("bundleId") != path.name
            or isinstance(revision, bool)
            or not isinstance(revision, int)
            or str(revision) != match.group(1)
            or not isinstance(manifest_sha256, str)
            or not _SHA256_RE.fullmatch(manifest_sha256)
            or manifest_sha256[:16] != match.group(2)
        ):
            return None
        return metadata.st_mtime

    def _prune_auxiliary_directories(
        self,
        parent: Path,
        *,
        keep: int,
        staging_only: bool = False,
        protected: Path | None = None,
    ) -> None:
        if not parent.is_dir():
            return
        candidates: list[tuple[float, Path]] = []
        for candidate in parent.iterdir():
            if (
                candidate.is_symlink()
                or not candidate.is_dir()
                or (staging_only and not _STAGING_NAME_RE.fullmatch(candidate.name))
            ):
                continue
            try:
                modified_at = candidate.stat().st_mtime
            except OSError:
                continue
            candidates.append((modified_at, candidate))

        candidates.sort(key=lambda item: item[0], reverse=True)
        now = datetime.now(UTC).timestamp()
        for index, (modified_at, candidate) in enumerate(candidates):
            if protected is not None and candidate == protected:
                continue
            expired = now - modified_at > self.auxiliary_max_age_seconds
            if expired or index >= keep:
                self._remove_directory(candidate)

    @staticmethod
    def _remove_directory(path: Path) -> None:
        if path.is_symlink() or not path.is_dir():
            raise ValueError(f"Refusing to remove unsafe directory: {path}")
        for root_name, directories, _files in os.walk(path):
            root = Path(root_name)
            os.chmod(root, 0o700)
            directories[:] = [
                name for name in directories if not (root / name).is_symlink()
            ]
        parent = path.parent
        shutil.rmtree(path)
        _fsync_directory(parent)

    def _prepare_staging_directory(self, staging_dir: Path) -> None:
        if staging_dir.is_symlink():
            raise ValueError("Staging path cannot be a symlink")
        staging_dir.mkdir(parents=True, exist_ok=True)
        for root, directories, files in os.walk(staging_dir):
            root_path = Path(root)
            os.chmod(root_path, 0o750)
            for directory in directories:
                directory_path = root_path / directory
                if directory_path.is_symlink():
                    raise ValueError("Staging directory cannot contain symlinks")
                os.chmod(directory_path, 0o750)
            for file_name in files:
                file_path = root_path / file_name
                if file_path.is_symlink():
                    raise ValueError("Staging directory cannot contain symlinks")
                os.chmod(file_path, 0o640)

    def _preflight_disk_space(
        self,
        staging_dir: Path,
        records: list[dict[str, Any]],
    ) -> None:
        required = 0
        for record in records:
            destination = staging_dir / normalize_release_path(str(record["path"]))
            expected_size = int(record["size"])
            try:
                metadata = destination.lstat()
            except FileNotFoundError:
                required += expected_size
                continue
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > expected_size:
                required += expected_size
            elif metadata.st_size == expected_size:
                try:
                    self._verify_file(destination, record)
                except ValueError:
                    required += expected_size
            else:
                required += expected_size - metadata.st_size
        free = shutil.disk_usage(self.releases_dir).free
        if free < required + self.minimum_free_bytes:
            raise OSError(
                errno.ENOSPC,
                (
                    "Insufficient disk space for release bundle: "
                    f"required={required}, reserve={self.minimum_free_bytes}, free={free}"
                ),
            )

    def _discard_invalid_staging_file(
        self,
        destination: Path,
        file_record: dict[str, Any],
    ) -> None:
        try:
            metadata = destination.lstat()
        except FileNotFoundError:
            return
        expected_size = int(file_record["size"])
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > expected_size:
            destination.unlink(missing_ok=True)
            return
        if metadata.st_size == expected_size:
            try:
                self._verify_file(destination, file_record)
            except ValueError:
                destination.unlink(missing_ok=True)

    @staticmethod
    def _fsync_tree(root: Path) -> None:
        directories = [Path(path) for path, _dirs, _files in os.walk(root)]
        for directory in reversed(directories):
            _fsync_directory(directory)

    @staticmethod
    def _seal_release(root: Path, *, seal_root: bool = True) -> None:
        directories: list[Path] = []
        for directory_name, child_directories, files in os.walk(root):
            directory = Path(directory_name)
            directories.append(directory)
            for child in child_directories:
                child_path = directory / child
                if child_path.is_symlink():
                    raise ValueError("Release bundle cannot contain directory symlinks")
            for file_name in files:
                file_path = directory / file_name
                if file_path.is_symlink():
                    raise ValueError("Release bundle cannot contain file symlinks")
                os.chmod(file_path, 0o440)
        for directory in reversed(directories):
            if directory != root or seal_root:
                os.chmod(directory, 0o550)

    def _switch_legacy_data_link(self, final_dir: Path) -> None:
        if self.data_path.exists() and not self.data_path.is_symlink():
            legacy_dir = self.releases_dir / "legacy"
            if legacy_dir.exists() or legacy_dir.is_symlink():
                raise RuntimeError(
                    "Cannot bootstrap data directory because releases/legacy exists"
                )
            os.chmod(self.data_path, 0o750)
            os.replace(self.data_path, legacy_dir)
            os.chmod(legacy_dir, 0o550)
            _fsync_directory(self.releases_dir)
            _fsync_directory(self.runtime_root)

        temp_link = self.runtime_root / ".data.next"
        temp_link.unlink(missing_ok=True)
        relative_target = final_dir.relative_to(self.runtime_root)
        temp_link.symlink_to(relative_target, target_is_directory=True)
        os.replace(temp_link, self.data_path)
        _fsync_directory(self.runtime_root)

        if not self.data_path.is_symlink() or not self.data_path.resolve().samefile(final_dir):
            raise RuntimeError(f"Failed to update legacy data alias to {final_dir.name}")


class DeviceApi:
    def __init__(self, config: AgentConfig) -> None:
        self.config = config
        self.client = httpx.AsyncClient(
            base_url=config.server_url,
            headers={"Authorization": f"Bearer {config.token}"},
            timeout=httpx.Timeout(30, read=300),
            follow_redirects=False,
        )

    async def close(self) -> None:
        await self.client.aclose()

    async def manifest(
        self,
        revision: int,
        expected_sha256: str | None,
    ) -> ManifestDocument:
        response = await self.client.get(
            f"/api/device/{quote(self.config.device_id)}/releases/{revision}/manifest"
        )
        response.raise_for_status()
        content = response.content
        actual_sha256 = hashlib.sha256(content).hexdigest()
        if expected_sha256 and actual_sha256 != expected_sha256:
            raise ValueError("Manifest SHA256 mismatch")
        value = json.loads(content)
        if not isinstance(value, dict):
            raise ValueError("Manifest is not an object")
        return ManifestDocument(value=value, content=content, sha256=actual_sha256)

    async def download_file(
        self,
        revision: int,
        file_record: dict[str, Any],
        destination: Path,
    ) -> None:
        release_path = normalize_release_path(str(file_record["path"]))
        url = (
            f"/api/device/{quote(self.config.device_id)}/releases/{revision}/files/"
            f"{quote(release_path, safe='/')}"
        )
        expected_size = int(file_record["size"])
        existing_size = destination.stat().st_size if destination.exists() else 0
        if existing_size > expected_size:
            destination.unlink()
            existing_size = 0
        if existing_size == expected_size:
            return

        resumed = await self._download_once(
            url,
            file_record,
            destination,
            existing_size,
        )
        if not resumed:
            return
        resumed_ok = await _run_blocking(
            self._download_matches,
            destination,
            file_record,
        )
        if resumed_ok:
            return

        # A mismatched resumed result may be a stale/corrupt partial. Discard it
        # and retry exactly once from byte zero before treating integrity failure
        # as non-transient.
        destination.unlink(missing_ok=True)
        await self._download_once(url, file_record, destination, 0)


    async def _download_once(
        self,
        url: str,
        file_record: dict[str, Any],
        destination: Path,
        existing_size: int,
    ) -> bool:
        headers: dict[str, str] = {}
        if existing_size:
            headers = {
                "Range": f"bytes={existing_size}-",
                "If-Range": f'"{file_record["sha256"]}"',
            }
        async with self.client.stream("GET", url, headers=headers) as response:
            response.raise_for_status()
            if response.status_code == 206:
                content_range = response.headers.get("content-range", "")
                if not content_range.startswith(f"bytes {existing_size}-"):
                    raise ValueError("Invalid Content-Range from server")
                mode = "ab"
            elif response.status_code == 200:
                mode = "wb"
            else:
                raise ValueError(f"Unexpected download status: {response.status_code}")
            with destination.open(mode) as output:
                try:
                    async for chunk in response.aiter_bytes():
                        output.write(chunk)
                finally:
                    output.flush()
                    sync_descriptor = os.dup(output.fileno())
                    try:
                        await _run_blocking(os.fsync, sync_descriptor)
                    finally:
                        os.close(sync_descriptor)
        return response.status_code == 206 and existing_size > 0

    @staticmethod
    def _download_matches(path: Path, file_record: dict[str, Any]) -> bool:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            return False
        return (
            stat.S_ISREG(metadata.st_mode)
            and metadata.st_size == int(file_record["size"])
            and _sha256_file(path) == file_record["sha256"]
        )


class SyncAgent:
    def __init__(self, config: AgentConfig) -> None:
        self.config = config
        self.api = DeviceApi(config)
        self.installer = ReleaseInstaller(
            config.runtime_root,
            keep_releases=config.keep_releases,
            minimum_free_bytes=config.minimum_free_bytes,
        )
        self._stop = asyncio.Event()
        self._desired: DesiredRelease | None = None
        self._desired_event = asyncio.Event()
        self._install_task: asyncio.Task[None] | None = None

    async def stop(self) -> None:
        self._stop.set()
        if self._install_task is not None:
            self._install_task.cancel()
        await self.api.close()

    async def run(self) -> None:
        delay = 1
        while not self._stop.is_set():
            connection = asyncio.create_task(self._run_connection())
            stopping = asyncio.create_task(self._stop.wait())
            try:
                done, _pending = await asyncio.wait(
                    {connection, stopping},
                    return_when=asyncio.FIRST_COMPLETED,
                )
                if stopping in done:
                    connection.cancel()
                    await asyncio.gather(connection, return_exceptions=True)
                    return
                stopping.cancel()
                await asyncio.gather(stopping, return_exceptions=True)
                await connection
                delay = 1
            except (
                ConnectionClosed,
                OSError,
                httpx.HTTPError,
                ValueError,
                json.JSONDecodeError,
            ):
                if self._stop.is_set():
                    return
                try:
                    await asyncio.wait_for(self._stop.wait(), timeout=delay)
                    return
                except TimeoutError:
                    pass
                delay = min(delay * 2, 60)
            finally:
                if not connection.done():
                    connection.cancel()
                if not stopping.done():
                    stopping.cancel()
                await asyncio.gather(connection, stopping, return_exceptions=True)

    async def _run_connection(self) -> None:
        headers = {
            "Authorization": f"Bearer {self.config.token}",
            "X-Agent-Version": __version__,
        }
        async with connect(
            self.config.websocket_url,
            additional_headers=headers,
            open_timeout=15,
            ping_interval=20,
            ping_timeout=20,
            max_size=1024 * 1024,
        ) as websocket:
            current = self.installer.current_revision()
            if current is not None:
                await self._send_status(
                    websocket,
                    current,
                    self.installer.current_generation(),
                    "startup-validation",
                    "applied",
                    100,
                    None,
                )
            if self.config.shadow_mode and self.installer.verified_revision() is not None:
                await self._send_status(
                    websocket,
                    int(self.installer.verified_revision()),
                    self.installer.verified_generation(),
                    "startup-shadow-validation",
                    "ready",
                    100,
                    None,
                )

            if self._desired is not None and not self._target_is_current(self._desired):
                self._desired_event.set()

            reader = asyncio.create_task(self._reader_loop(websocket))
            worker = asyncio.create_task(self._deployment_worker(websocket))
            heartbeat = asyncio.create_task(self._heartbeat_loop(websocket))
            stopping = asyncio.create_task(self._stop.wait())
            tasks = {
                reader,
                worker,
                heartbeat,
                stopping,
            }
            try:
                done, _pending = await asyncio.wait(
                    tasks,
                    return_when=asyncio.FIRST_COMPLETED,
                )
                if stopping in done:
                    return
                for task in done:
                    await task
            finally:
                for task in tasks:
                    task.cancel()
                if self._install_task is not None:
                    self._install_task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                if self._install_task is not None:
                    await asyncio.gather(self._install_task, return_exceptions=True)
                    self._install_task = None

    async def _reader_loop(self, websocket: Any) -> None:
        while not self._stop.is_set():
            raw = await websocket.recv()
            message = json.loads(raw)
            if not isinstance(message, dict):
                raise ValueError("WebSocket message is not an object")
            message_type = message.get("type")
            if message_type in {"state.snapshot", "release.available"}:
                desired = DesiredRelease.from_message(message)
                if desired is not None:
                    self._offer_desired(desired)


    def _offer_desired(self, desired: DesiredRelease) -> bool:
        current = self._desired
        applied_generation = self._current_target_generation()
        newest_generation = max(
            applied_generation,
            current.generation if current is not None else 0,
        )
        if desired.generation < newest_generation:
            return False
        if desired.generation == newest_generation:
            if current is not None and (
                desired.revision == current.revision
                and desired.generation == current.generation
            ):
                if current.manifest_sha256 is None and desired.manifest_sha256 is not None:
                    self._desired = desired
                    self._desired_event.set()
                    if self._install_task is not None and not self._install_task.done():
                        self._install_task.cancel()
                    return True
                return False
            if (
                desired.revision == self._current_target_revision()
                and desired.generation == applied_generation
            ):
                return False
            return False

        self._desired = desired
        self._desired_event.set()
        if self._install_task is not None and not self._install_task.done():
            self._install_task.cancel()
        return True

    async def _deployment_worker(self, websocket: Any) -> None:
        while not self._stop.is_set():
            await self._desired_event.wait()
            self._desired_event.clear()
            desired = self._desired
            if desired is None or self._target_is_current(desired):
                continue
            task = asyncio.create_task(self._apply_with_retry(websocket, desired))
            self._install_task = task
            try:
                await task
            except asyncio.CancelledError:
                current_task = asyncio.current_task()
                if self._stop.is_set() or (
                    current_task is not None and current_task.cancelling()
                ):
                    raise
            finally:
                if self._install_task is task:
                    self._install_task = None








    async def _apply_with_retry(self, websocket: Any, desired: DesiredRelease) -> None:
        retry_index = 0
        while not self._stop.is_set() and self._target_is_latest(desired):
            attempt_id = str(uuid.uuid4())
            try:
                await self._apply_target_once(websocket, desired, attempt_id)
                return
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                await self._send_status(
                    websocket,
                    desired.revision,
                    desired.generation,
                    attempt_id,
                    "failed",
                    0,
                    f"{type(exc).__name__}: {exc}",
                )
                if not self._is_transient_error(exc) or not self._target_is_latest(desired):
                    return
                delays = self.config.retry_delays
                if not delays:
                    return
                base_delay = delays[min(retry_index, len(delays) - 1)]
                retry_index += 1
                jitter = base_delay * self.config.retry_jitter_ratio
                delay = max(0.0, base_delay + random.uniform(-jitter, jitter))
                await asyncio.sleep(delay)

    async def _apply_target_once(
        self,
        websocket: Any,
        desired: DesiredRelease,
        attempt_id: str,
    ) -> None:
        await self._send_status(
            websocket,
            desired.revision,
            desired.generation,
            attempt_id,
            "received",
            0,
            self.installer.startup_error,
        )
        document = await self.api.manifest(
            desired.revision,
            desired.manifest_sha256,
        )

        async def report(state: str, progress: int, error: str | None) -> None:
            await self._send_status(
                websocket,
                desired.revision,
                desired.generation,
                attempt_id,
                state,
                progress,
                error,
            )

        prepared = await self.installer.prepare(
            desired.revision,
            document.value,
            document.content,
            document.sha256,
            self.api.download_file,
            report,
        )
        if not self._target_is_latest(desired):
            await self._send_status(
                websocket,
                desired.revision,
                desired.generation,
                attempt_id,
                "ready",
                100,
                None,
            )
            return
        if self.config.shadow_mode:
            await self.installer.mark_verified_and_maintain(
                prepared,
                desired.generation,
            )
            await self._send_status(
                websocket,
                desired.revision,
                desired.generation,
                attempt_id,
                "ready",
                100,
                None,
            )
            return

        # No await is allowed between the latest-target check and this atomic
        # local commit, so a newer WebSocket event cannot interleave activation.
        self.installer.activate(prepared, desired.generation)
        await self.installer.maintain_storage()
        await self._send_status(
            websocket,
            desired.revision,
            desired.generation,
            attempt_id,
            "applied",
            100,
            None,
        )

    async def _heartbeat_loop(self, websocket: Any) -> None:
        while not self._stop.is_set():
            await websocket.send(json.dumps(self._heartbeat_payload()))
            try:
                await asyncio.wait_for(
                    self._stop.wait(),
                    timeout=self.config.heartbeat_seconds,
                )
            except TimeoutError:
                continue

    def _heartbeat_payload(self) -> dict[str, Any]:
        # Public snapshot excludes machine identity, software OTA, and power policy.
        return {
            "type": "heartbeat",
            "agent_version": __version__,
            "applied_revision": self.installer.current_revision(),
            "applied_generation": self.installer.current_generation(),
        }

    def _target_is_latest(self, desired: DesiredRelease) -> bool:
        return self._desired == desired

    def _target_is_current(self, desired: DesiredRelease) -> bool:
        return (
            desired.revision == self._current_target_revision()
            and desired.generation == self._current_target_generation()
        )

    def _current_target_revision(self) -> int | None:
        if self.config.shadow_mode:
            return self.installer.verified_revision()
        return self.installer.current_revision()

    def _current_target_generation(self) -> int:
        if self.config.shadow_mode:
            return self.installer.verified_generation()
        return self.installer.current_generation()

    @staticmethod
    def _is_transient_error(exc: Exception) -> bool:
        if isinstance(
            exc,
            (
                httpx.TimeoutException,
                httpx.NetworkError,
                httpx.RemoteProtocolError,
            ),
        ):
            return True
        if isinstance(exc, httpx.HTTPStatusError):
            return exc.response.status_code in _RETRYABLE_HTTP_STATUS
        return isinstance(exc, OSError) and exc.errno in {
            errno.ECONNABORTED,
            errno.ECONNREFUSED,
            errno.ECONNRESET,
            errno.EHOSTUNREACH,
            errno.ENETDOWN,
            errno.ENETUNREACH,
            errno.ETIMEDOUT,
        }

    @staticmethod
    async def _send_status(
        websocket: Any,
        revision: int,
        generation: int,
        attempt_id: str,
        state: str,
        progress: int,
        error: str | None,
    ) -> None:
        await websocket.send(
            json.dumps(
                {
                    "type": "deployment.status",
                    "revision": revision,
                    "generation": generation,
                    "attempt_id": attempt_id,
                    "status": state,
                    "progress": progress,
                    "error": error,
                }
            )
        )









