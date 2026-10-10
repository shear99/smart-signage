"""Synthetic tests for the selected production sync implementation."""
from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import pytest

from smart_signage.agent import AgentConfig, DesiredRelease, ReleaseInstaller, SyncAgent

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from make_sample import manifest_for, sample_payloads


async def install(installer, revision, payloads, corrupt=False):
    manifest, raw, digest = manifest_for(payloads, revision)

    async def download(_revision, record, target):
        target.write_bytes(b"broken" if corrupt else payloads[record["path"]])

    async def report(_state, _progress, _error):
        pass

    return await installer.prepare(revision, manifest, raw, digest, download, report)


@pytest.mark.asyncio
async def test_verifies_complete_bundle_before_activation(tmp_path):
    installer = ReleaseInstaller(tmp_path)
    prepared = await install(installer, 1, sample_payloads())
    assert not installer.active_path.exists()
    assert (prepared.path / ".complete").exists()
    installer.activate(prepared, 3)
    assert json.loads(installer.active_path.read_text())["generation"] == 3
    restarted = ReleaseInstaller(tmp_path)
    assert restarted.current_revision() == 1


@pytest.mark.asyncio
async def test_corrupt_download_preserves_active_pointer(tmp_path):
    installer = ReleaseInstaller(tmp_path)
    installer.activate(await install(installer, 1, sample_payloads()), 1)
    original = installer.active_path.read_bytes()
    with pytest.raises(ValueError):
        await install(installer, 2, sample_payloads(b"changed"), corrupt=True)
    assert installer.active_path.read_bytes() == original
    assert installer.current_revision() == 1


@pytest.mark.asyncio
async def test_corrupt_manifest_rejected_before_download(tmp_path):
    installer = ReleaseInstaller(tmp_path)
    manifest, raw, digest = manifest_for(sample_payloads())
    async def forbidden(*_args):
        pytest.fail("Download must not start before manifest verification")
    with pytest.raises(ValueError, match="SHA256"):
        await installer.prepare(1, manifest, raw + b" ", digest, forbidden, forbidden)


@pytest.mark.asyncio
async def test_new_generation_cancels_old_work_and_stale_event_is_ignored(tmp_path):
    agent = SyncAgent(AgentConfig("http://127.0.0.1:8765", "demo", "local-only", tmp_path))
    try:
        assert agent._offer_desired(DesiredRelease(1, 1))
        agent._install_task = asyncio.create_task(asyncio.sleep(60))
        assert agent._offer_desired(DesiredRelease(2, 2))
        assert not agent._offer_desired(DesiredRelease(1, 1))
        await asyncio.gather(agent._install_task, return_exceptions=True)
        assert agent._install_task.cancelled()
        assert agent._desired == DesiredRelease(2, 2)
    finally:
        await agent.stop()


@pytest.mark.asyncio
async def test_snapshot_and_live_event_share_the_same_desired_path(tmp_path):
    agent = SyncAgent(AgentConfig("http://127.0.0.1:8765", "demo", "local-only", tmp_path))
    messages = iter([
        {"type": "state.snapshot", "desired_revision": 1, "desired_generation": 1},
        {"type": "release.available", "revision": 2, "generation": 2},
    ])
    class WebSocket:
        async def recv(self):
            try:
                return json.dumps(next(messages))
            except StopIteration:
                agent._stop.set()
                return '{}'
    try:
        await agent._reader_loop(WebSocket())
        assert agent._desired == DesiredRelease(2, 2)
    finally:
        await agent.stop()
