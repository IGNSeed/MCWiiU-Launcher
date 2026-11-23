<p align="center">
  <img src="../src/launcher/assets/app-icon.png" alt="MCWiiU Launcher" width="112">
</p>

<h1 align="center">MCWiiU Launcher</h1>

<p align="center">Minecraft: Wii U Edition을 하나의 런처에서.</p>

<p align="center">
  <strong>0.4.0 Stable — 공개됨</strong><br>
  Windows 10 / 11 x64 · Cemu-based · Unofficial
</p>

<p align="center">
  <a href="../README.md">日本語</a> · <a href="README.en.md">English</a> · <strong>한국어</strong> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.es.md">Español</a>
</p>

<p align="center">
  <a href="../docs/INSTALLATION.md">설치 가이드</a> · <a href="../docs/FEATURES.md">기능 가이드</a>
</p>

---

## 소개

MCWiiU Launcher는 Windows PC에서 Minecraft: Wii U Edition **v560**을 플레이하기 위한 Cemu 기반 비공식 런처입니다. 게임 설치부터 계정·월드·팩 관리, 실행과 업데이트까지 한 화면에서 이용할 수 있습니다.

## 출시 현황

**[0.4.0 Stable](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.0)이 공개되었습니다.** Foundation **1.0.2**, Updater Protocol **2**입니다. [Setup 다운로드](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.0/MCWiiU-Launcher-0.4.0-Setup-x64.exe).

배포 바이너리의 소스 Commit: [52d4fbbaddc9edac0eb7d0eb6242ca91a8bc8b7c](https://github.com/IGNSeed/MCWiiU-Launcher/commit/52d4fbbaddc9edac0eb7d0eb6242ca91a8bc8b7c).

실서비스용 Ed25519 공개 키가 포함되어 있으며, 서명된 Manifest와 배포 파일의 무결성 및 공개 다운로드를 확인했습니다. 이미지 재배포 허가, 라이선스와 대응 소스의 미확인 사항, 의존 라이브러리의 잔여 취약점, 깨끗한 Windows 10 / 11 환경에서의 전체 검증은 아직 완료되지 않았습니다.

## 주요 기능

- **Minecraft Launch / Game Data** — Native 빌드의 `PLAY`로 같은 창에서 플레이합니다. BASE, 같은 지역의 Update v560, 선택 사항인 DLC를 설치하고 관리합니다.
- **Account / Friends** — Settings에서 계정을 관리하고 서비스를 선택하며, Friends 목록과 친구 요청을 관리합니다. 서비스 이용 조건과 연결 상태에 따라 이용 가능 여부가 달라집니다.
- **World Manager** — 월드 가져오기, 이름·아이콘 변경, Account 간 이동, Trash 및 복원을 지원합니다.
- **Texture Pack / Graphic Pack Manager** — 로컬 팩을 검증·선택·설정합니다. 변경 사항은 다음 게임 실행 시 적용됩니다.
- **Session History / Discord Rich Presence** — 일·주·월·연간 플레이 시간과 날짜별 기록을 확인하고, 현재 페이지와 플레이 상태를 알립니다. Presence는 Discord의 공유 설정에도 영향을 받습니다.
- **Launcher Settings / Secure Updates** — 입력·그래픽·오디오·Overlay 설정과 사용자 승인 후 적용되는 서명된 Launcher 업데이트를 제공합니다.

플레이 중 `F10`으로 종료 확인을 열고, `ESC`로 취소하며, `F11`로 표시 모드를 전환합니다. 자세한 조작과 제한은 [FEATURES](../docs/FEATURES.md)를 확인하세요. 기능 구현이 모든 PC와 온라인 서비스에서의 동작을 보장하지는 않습니다.

## 시스템 요구 사항

| 항목 | 대상 및 조건 |
|---|---|
| OS | Windows 10 / 11 x64 |
| CPU / RAM | x64 CPU. 최소 CPU·명령어 집합·메모리 기준은 미확정 |
| GPU | Vulkan 또는 OpenGL 지원 GPU와 적절한 드라이버. Vulkan 우선 |
| Runtime | Microsoft Edge WebView2 Evergreen Runtime. 없으면 Setup이 중단되며 자동 설치하지 않음 |
| 저장 공간 | 앱·게임·MLC·세이브·팩·캐시 및 설치/업데이트 작업용 공간. 고정 최소 용량은 미확정 |

Windows 10의 일반 지원은 **2025-10-14에 종료**되었습니다. 대상 OS 표기는 OS 보안 지원을 보장하지 않습니다. [Microsoft 안내](https://www.microsoft.com/en-us/windows/end-of-support)를 확인하세요.

지금까지의 검증은 주로 하나의 Windows 11 개발 PC, 소유한 게임의 격리된 JPN v560 사본, 제한된 GPU 환경에서 이루어졌습니다. Windows 10, 깨끗한 PC, 모든 지역·입력·오디오 장치·장시간 실행에 대한 검증은 완료되지 않았습니다. Core가 없는 Standalone 개발 빌드는 게임을 실행할 수 없습니다.

## 설치

1. [INSTALLATION](../docs/INSTALLATION.md)에서 요구 사항과 안전 확인 절차를 읽으세요.
2. 공식 [GitHub Releases](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.0)에서 Setup을 받고 설치 경로를 선택하세요. Setup은 **Authenticode 서명을 사용하지 않습니다**. 가이드에 따라 배포 출처와 무결성을 독립적으로 확인하세요.
3. `MCWiiU-Launcher.exe`(Bootstrapper)를 실행하고, 소유한 게임 데이터를 설치한 뒤 Home의 `PLAY`를 선택하세요. 본체 이름은 `MCWiiU Launcher.exe`, 업데이터는 `MCWiiU-Updater.exe`입니다.

이전 개발 빌드에서의 자동 이전은 공식 지원 대상이 아닙니다. 제거 및 데이터 유지에 대해서도 설치 가이드를 참조하세요.

## 게임 데이터

**게임 본편·Update·DLC·세이브·Wii U 시스템 파일·게임 키는 포함되지 않습니다.** 본인이 소유하고 적법하게 준비한 데이터가 필요합니다.

BASE와 같은 지역의 Update **v560**을 사용하세요. DLC는 선택 사항입니다. 설치, 지역 변경, 게임 데이터만 삭제하는 방법과 세이브 유지는 [GAME_DATA](../docs/GAME_DATA.md)에서 설명합니다. 변경 전에 중요한 세이브를 백업하세요.

## 업데이트

일반 Launcher 업데이트는 사용자 승인 후 서명된 ZIP을 적용합니다. Ed25519, SHA-256, GitHub Asset의 대응 관계와 파일 목록을 검증합니다. 30초 Readiness 확인에 실패하면 검증 가능한 이전 버전으로 Rollback합니다. **실서비스용 공개 키가 포함되어 있습니다. 0.4.0에서 같은 버전으로의 업데이트는 발생하지 않으며, 이후 버전으로의 실제 업데이트 시험은 아직 수행하지 않았습니다.**

Foundation은 **수동 Setup 업데이트**를 사용하며 일반 Launcher 업데이트와 별도입니다. 자세한 내용과 복구 방법은 [UPDATING](../docs/UPDATING.md)을 참조하세요.

## 문서

아래 상세 가이드는 현재 일본어로 제공됩니다.

- [설치 및 제거](../docs/INSTALLATION.md)
- [소유 게임 데이터와 세이브](../docs/GAME_DATA.md)
- [조작 및 설정](../docs/FEATURES.md)
- [업데이트 및 복구](../docs/UPDATING.md)
- [문제 해결](../docs/TROUBLESHOOTING.md)
- [저장 데이터 및 통신](../docs/PRIVACY.md)
- [라이선스와 대응 소스](../docs/THIRD_PARTY_NOTICES.md)
- [배포 확인 및 보안 신고](../SECURITY.md)
- [변경 기록](../CHANGELOG.md)
- 개발·빌드·기여: [BUILD](../BUILD.md) · [CONTRIBUTING](../CONTRIBUTING.md)

버그 신고 창구로 GitHub Issues를 사용할 예정이지만, **현재는 비활성화되어 있으며 신고 체계가 아직 준비되지 않았습니다**. 공개 신고에 게임 키·토큰·계정 정보·세이브를 포함하지 마세요.

## 크레딧

[Cemu](https://github.com/cemu-project/Cemu)와 기여자, saucer, WebView2, Discord Social SDK 및 각 의존 라이브러리 개발자에게 감사드립니다. 원문 고지와 대응 소스는 [THIRD_PARTY_NOTICES](../docs/THIRD_PARTY_NOTICES.md)를 참조하세요.

## 라이선스

Cemu에서 파생된 코드의 **MPL-2.0** 라이선스와 저작권 고지를 유지합니다. 원문은 [LICENSE.txt](../LICENSE.txt)입니다. 타사 의존 구성 요소에는 각각의 조건이 적용됩니다. 소스 라이선스는 게임 자산이나 타사 이미지의 재배포를 허가하지 않습니다.

## 면책 사항

MCWiiU Launcher는 Cemu, Mojang, Microsoft, Nintendo의 공식 제품이 아니며, 이들이 제공하거나 승인한 제품도 아닙니다. Minecraft 및 관련 명칭과 자산의 권리는 각 권리자에게 있습니다.
