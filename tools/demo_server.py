"""Loopback-only, in-memory protocol harness, not the production management server."""
from __future__ import annotations

import uvicorn
from fastapi import FastAPI, Header, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import Response
from make_sample import manifest_for, sample_payloads

app = FastAPI(title="Synthetic signage protocol harness")
payloads = sample_payloads()
revision = 1
generation = 1
sockets: set[WebSocket] = set()
TOKEN = "local-demo-only"


def require_auth(authorization: str | None) -> None:
    if authorization != f"Bearer {TOKEN}":
        raise HTTPException(401)


@app.get("/api/device/{device_id}/releases/{requested}/manifest")
async def manifest(device_id: str, requested: int, authorization: str | None = Header(None)):
    require_auth(authorization)
    if requested != revision:
        raise HTTPException(404)
    return Response(manifest_for(payloads, revision)[1], media_type="application/json")


@app.get("/api/device/{device_id}/releases/{requested}/files/{name:path}")
async def file(device_id: str, requested: int, name: str, request: Request,
               authorization: str | None = Header(None)):
    require_auth(authorization)
    if requested != revision or name not in payloads:
        raise HTTPException(404)
    data = payloads[name]
    byte_range = request.headers.get("range")
    if byte_range:
        try:
            offset = int(byte_range.removeprefix("bytes=").removesuffix("-"))
        except ValueError:
            raise HTTPException(416) from None
        if offset < 0 or offset >= len(data):
            raise HTTPException(416)
        return Response(data[offset:], status_code=206,
                        headers={"Content-Range": f"bytes {offset}-{len(data)-1}/{len(data)}"})
    return Response(data)


def state(kind: str) -> dict:
    return {"type": kind, "revision": revision, "generation": generation,
            "manifest_sha256": manifest_for(payloads, revision)[2]}


@app.websocket("/ws/device/{device_id}")
async def device(websocket: WebSocket, device_id: str):
    if websocket.headers.get("authorization") != f"Bearer {TOKEN}":
        await websocket.close(code=1008)
        return
    await websocket.accept()
    sockets.add(websocket)
    try:
        await websocket.send_json(state("state.snapshot"))
        while True:
            await websocket.receive_json()
    except WebSocketDisconnect:
        pass
    finally:
        sockets.discard(websocket)


@app.post("/demo/publish")
async def publish(authorization: str | None = Header(None)):
    """Reassign the same synthetic content; demonstrates generation updates."""
    require_auth(authorization)
    global generation
    generation += 1
    for websocket in tuple(sockets):
        await websocket.send_json(state("release.available"))
    return state("release.available")


if __name__ == "__main__":
    uvicorn.run(app, host="127.0.0.1", port=8765)
