"""Public harness: run the selected real sync agent against the loopback example."""
import asyncio
from pathlib import Path

from smart_signage.agent import AgentConfig, SyncAgent


async def main() -> None:
    agent = SyncAgent(AgentConfig(
        server_url="http://127.0.0.1:8765", device_id="demo-screen",
        token="local-demo-only", runtime_root=Path("runtime/demo-device"),
    ))
    try:
        await agent.run()
    finally:
        await agent.stop()


if __name__ == "__main__":
    asyncio.run(main())
