# 코드 출처와 공개용 변경

2026-10-10 사용자 소유 개발 작업 폴더에서 소스를 선별했습니다. 원본 HEAD는 `4fa365be89b10181467470ca6940d97c736f15ce`이며 **HEAD 파일만의 snapshot은 아닙니다.** 선택 시점 agent.py에는 미커밋 변경이 있었습니다. 원본 파일은 수정하지 않았고 Git 이력·운영 설정도 가져오지 않았습니다.

[source-snapshot.json](source-snapshot.json)에 선택 전 파일 SHA-256과 공개 경로를 기록했습니다. 원본 패키지명은 PRIVATE_PACKAGE로 일반화했습니다. 이 hash는 소스 선택 기준이며 성능·운영 검증 증거가 아닙니다.

## 선별한 원본 구현

C++/Qt 표시 코어·앱·QML·schema·Qt 회귀 테스트, Python 콘텐츠 compiler·storage, agent의 설치·검증·활성화·HTTP·WebSocket·최신 목표·재시도 로직을 포함합니다.

## 공개용 변경

1. namespace·패키지·표시명과 예제 텍스트를 일반화했습니다.
2. agent의 OTA·전원 관리·기기별 예외·운영 CLI를 제거했습니다. 연관 config·worker·heartbeat 항목도 제외했습니다.
3. 현장 SVG를 일반 SVG로 교체하고 시스템 폰트를 사용합니다. 기본 경로를 현재 폴더의 runtime/content로 바꿨습니다.
4. schema ID를 example.invalid로, 테스트의 현장 텍스트를 합성 명칭으로 바꿨습니다.
5. 공개용 의존성, tools의 loopback harness·합성 콘텐츠 생성기, Python 테스트를 추가했습니다. 새 예제 서버는 원본 관리 웹 소스가 아닙니다.

원본 구현과 공개용 검증 도구를 구분하여 소개합니다.
