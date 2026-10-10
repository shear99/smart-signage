# 공개 snapshot 로컬 검증

검증일: **2026-10-10**. 대상은 이 공개 저장소이며 원본 현장 서비스가 아닙니다.

| 검사 | 결과 | 확인한 범위 |
|---|---|---|
| Python pytest | **6 passed** | 검증 전 비활성, 손상 시 기존 포인터 보존, 최신 목표, 이벤트 처리, loopback HTTP/WebSocket |
| C++ 코어 build | **성공** | Qt Core/Gui 기반 파서·watcher·controller·재생/일정/화면 선택 |
| CTest | **2/2 passed** | display-core 회귀 테스트, Python compiler–C++ parser 계약 |
| Ruff | **통과** | 공개 프로젝트에 명시된 E4/E7/E9/F/I 규칙 |
| 전체 Qt GUI build | **미검증** | 이 Mac에 Qt6Multimedia 개발 모듈이 없어 configure 중단 |
| 실제 Pi / HDMI / Wayland / 디코딩 | **이번 작업에서 실행하지 않음** | 현장 상태·영상 성능·사용자 수 실적을 의미하지 않음 |

환경: macOS arm64, AppleClang 17, Python 3.14.7. 의존성은 uv.lock에 고정했습니다.

## 실제 loopback 테스트

`test_live_protocol.py`는 임시 127.0.0.1 TCP listener와 Uvicorn 예제 서버에 실제 SyncAgent를 연결합니다. 초기 snapshot 적용 → 온라인 generation 변경 → 에이전트 종료 중 새 배정 → 재접속 snapshot 복구를 확인합니다. 공개용 합성 데이터만 사용하며 외부 서버에 연결하지 않습니다.

## 해석 범위

C++ 코어 테스트는 표시 상태와 파일 계약을 검증합니다. GUI 전체 빌드·영상 디코딩·물리 화면 검증을 대체하지 않습니다. contract-only 미디어는 합성 바이트이며 재생 성능 지표가 아닙니다. 원본 운영 환경의 성과 수치를 이 결과로 추정하지 않습니다.
