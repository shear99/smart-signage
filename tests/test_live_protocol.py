"""A real loopback HTTP/WebSocket test of the public harness and selected agent."""
from __future__ import annotations

import asyncio
import socket
import sys
from pathlib import Path

import httpx
import pytest
import uvicorn

from smart_signage.agent import AgentConfig, SyncAgent

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
import demo_server


async def eventually(predicate, seconds: float = 5.0):
    deadline = asyncio.get_running_loop().time() + seconds
    while not predicate():
        if asyncio.get_running_loop().time() >= deadline:
            pytest.fail("Timed out waiting for synthetic protocol convergence")
        await asyncio.sleep(0.02)


@pytest.mark.asyncio
async def test_live_snapshot_event_and_reconnect_converge(tmp_path):
    demo_server.generation = 1
    demo_server.sockets.clear()
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(16)
    port = listener.getsockname()[1]
    server = uvicorn.Server(uvicorn.Config(demo_server.app, log_level="error"))
    server_task = asyncio.create_task(server.serve(sockets=[listener]))
    agent = None
    agent_task = None
    try:
        await eventually(lambda: server.started)
        config = AgentConfig(f"http://127.0.0.1:{port}", "demo-screen",
                             "local-demo-only", tmp_path, minimum_free_bytes=0)
        agent = SyncAgent(config)
        agent_task = asyncio.create_task(agent.run())
        await eventually(lambda: agent.installer.current_generation() == 1)
        async with httpx.AsyncClient(base_url=config.server_url,
                                     headers={"Authorization": "Bearer local-demo-only"}) as client:
            assert (await client.post("/demo/publish")).status_code == 200
            await eventually(lambda: agent.installer.current_generation() == 2)
            await agent.stop()
            await asyncio.wait_for(agent_task, 5)
            await eventually(lambda: not demo_server.sockets)
            assert (await client.post("/demo/publish")).status_code == 200
        agent = SyncAgent(config)
        agent_task = asyncio.create_task(agent.run())
        await eventually(lambda: agent.installer.current_generation() == 3)
        assert agent.installer.current_revision() == 1
    finally:
        if agent is not None:
            await agent.stop()
        if agent_task is not None:
            await asyncio.wait_for(agent_task, 5)
        server.should_exit = True
        await asyncio.wait_for(server_task, 5)
        listener.close()
