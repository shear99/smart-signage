# 코드 읽는 순서

## 계약

- [content.py](../src/smart_signage/content.py): `ContentSpec`, `compile_content_bundle()` 입력 검증과 JSON 출력.
- [schemas](../display-client/schemas): 콘텐츠 계약. ID는 공개용 `example.invalid`입니다.
- [storage.py](../src/smart_signage/storage.py): 상대 경로 검사, canonical JSON, blob 해시·업로드 보조.

## 전달과 활성화

[agent.py](../src/smart_signage/agent.py)에서 다음 순서로 읽을 수 있습니다.

1. `DesiredRelease`: revision/generation 검사.
2. `DeviceApi`: manifest 수신, HTTP Range 재개 다운로드.
3. `ReleaseInstaller.prepare()` / `activate()`: staging·검증·완성·원자 교체.
4. `SyncAgent._reader_loop()` / `_offer_desired()`: snapshot/event와 최신 목표.
5. `_apply_target_once()`: 준비·최신 목표 확인·활성화·상태 응답.

## C++ / QML

- [active_bundle_watcher.cpp](../display-client/src/active_bundle_watcher.cpp): 디렉터리 이벤트와 debounce.
- [bundle_parser.cpp](../display-client/src/bundle_parser.cpp): identity, 참조 경로, JSON 구조 검사.
- [display_controller.cpp](../display-client/src/display_controller.cpp): 마지막 정상 snapshot, 설정 갱신, 재생 경계.
- [playlist_navigator.cpp](../display-client/src/playlist_navigator.cpp): 다음 항목과 오류 backoff.
- [schedule_projection.cpp](../display-client/src/schedule_projection.cpp): 반복·날짜별 일정 projection.
- [screen_selector.cpp](../display-client/src/screen_selector.cpp): 물리 화면과 EDID 우선순위.
- [Main.qml](../display-client/qml/Main.qml): 미디어·자막·시계·일정 표현.

## 테스트와 공개용 도구

- [test_sync.py](../tests/test_sync.py): 완료 전 비활성, 손상 시 보존, 최신 generation, 이벤트 경로.
- [test_display_core.cpp](../display-client/tests/test_display_core.cpp): 마지막 정상 화면, atomic 교체, 재생 경계, 일정·화면.
- [server_compiler_contract.py](../display-client/tests/server_compiler_contract.py): Python 출력–C++ 파서 계약.
- `tools/`: 공개용 synthetic harness. 원본 관리 웹 구현이 아닙니다.
