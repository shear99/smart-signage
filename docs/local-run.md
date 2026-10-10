# 로컬 실행

## Python

Python 3.11+와 uv:

```bash
uv sync --dev --frozen
uv run pytest -q
uv run ruff check --config pyproject.toml src tools tests
```

## Qt 코어와 계약

Qt 6.4+ Core, Gui, Test, CMake 3.21+, Ninja가 필요합니다. 플랫폼에 따라 CMAKE_PREFIX_PATH에 Qt 경로를 지정합니다.

```bash
uv run python tools/make_sample.py --contract-only
cmake -S display-client -B build -G Ninja -DSIGNAGE_BUILD_APP=OFF -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

contract-only는 재생용 영상이 아닌 계약 검사 바이트입니다. uv가 PATH에 있으면 Python compiler–C++ parser 검사가 CTest에 추가됩니다.

## 전체 Qt 앱

추가로 Qml, Quick, Multimedia가 필요합니다. Linux Pi의 물리 DRM 화면 선택이 기본 동작입니다.

```bash
# ffmpeg를 사용해 3초 합성 test pattern을 생성합니다.
uv run python tools/make_sample.py --root runtime/playable
cmake -S display-client -B build-app -G Ninja -DSIGNAGE_BUILD_APP=ON -DBUILD_TESTING=ON
cmake --build build-app --parallel
./build-app/signage-display-client --content-root "$PWD/runtime/playable"
```

기존 Linux graphical session에서 실행합니다. 배포·자동시작·하드웨어 디코더 설치 스크립트는 포함하지 않습니다.

## loopback 동기화 예제

공개용 protocol harness이며 실제 관리 웹이 아닙니다. 계약 바이트를 전달하므로 영상 재생 실습과는 별도입니다. 127.0.0.1:8765에만 bind합니다.

터미널 A:

```bash
uv run python tools/demo_server.py
```

터미널 B:

```bash
uv run python tools/demo_agent.py
```

snapshot에 따라 runtime/demo-device/active.json이 생성됩니다. 새 generation 알림:

```bash
curl -X POST http://127.0.0.1:8765/demo/publish -H 'Authorization: Bearer local-demo-only'
```

에이전트를 다시 실행하면 snapshot으로 현재 배정을 확인합니다. 예제 서버는 in-memory이며 서버 재시작 시 초기화됩니다. 실제 서버의 내구성 저장소를 대신하지 않습니다. 각 터미널에서 Ctrl-C로 종료합니다. demo 인증 문자열에는 실제 권한이 없습니다.
