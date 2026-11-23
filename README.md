# PC-Resource-Monitor

Windows PC의 프로세스별 자원 사용량을 로컬에 기록하고, 현재 사용량을 평소 패턴과 비교해 보여 주는 팀 프로젝트입니다.

## 실행 · Windows x64

1. [최종 릴리즈의 Windows x64 ZIP](https://github.com/HighKcal/PC-Resource-Monitor/releases/download/team-submission-2026-11-23/PC-Resource-Monitor-win-x64.zip)을 내려받아 전체를 풉니다.
2. `win-x64/PcResourceMonitor.exe`를 열고 Windows 관리자 승인 창에서 **예**를 선택합니다. DLL과 리소스는 실행 파일과 함께 둡니다.

저장소를 내려받았다면 [동봉된 win-x64 폴더](final-product/bin/win-x64)에서 같은 실행 파일을 열 수 있습니다. ZIP의 SHA-256은 [릴리즈 페이지](https://github.com/HighKcal/PC-Resource-Monitor/releases/tag/team-submission-2026-11-23)에서 확인할 수 있습니다.

## 소스·빌드·시험

[최종 제품](final-product/)에 [제품 소스](final-product/src/), 빌드 스크립트와 시험이 있습니다. x64 .NET 10 SDK, Visual Studio C++ 데스크톱 빌드 도구와 Windows SDK를 설치한 뒤 저장소 루트에서 PowerShell로 실행합니다.

```powershell
.\final-product\build.ps1
.\final-product\test.ps1 -SkipBuild
```

두 번째 명령은 방금 빌드한 제품으로 대표 회귀 19종을 실행합니다. 기능별 소스 위치와 빌드 환경은 [제품 안내](final-product/README.md)에 있습니다.
