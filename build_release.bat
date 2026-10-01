@echo off
rem ---------------------------------------------------------------------------
rem AviCap Studio - Windows release build (MSVC + vcpkg + Inno Setup).
rem
rem   build_release.bat [--skip-tests] [--skip-installer]
rem
rem Produces in release\:
rem   AviCapStudio.exe (+ DLLs, runnable in place)
rem   AviCapStudio-Setup.exe
rem   AviCap-Studio-Windows-Portable.zip
rem   SHA256SUMS.txt
rem
rem Prerequisites: run setup_dev.ps1 once (MSVC Build Tools, CMake, Ninja,
rem Inno Setup, vcpkg + FFmpeg). On Linux use tools/package_windows.sh.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build\msvc-release"
set "RELEASE=%ROOT%\release"
set "STAGE=%RELEASE%\stage"
set SKIP_TESTS=0
set SKIP_INSTALLER=0
:args
if "%~1"=="" goto argsdone
if /I "%~1"=="--skip-tests" set SKIP_TESTS=1
if /I "%~1"=="--skip-installer" set SKIP_INSTALLER=1
shift
goto args
:argsdone

rem ---- MSVC environment
if not defined VCINSTALLDIR (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "!VSWHERE!" ( echo [ERROR] Visual Studio not found. Run setup_dev.ps1 first. & exit /b 1 )
  for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
  if not defined VSPATH ( echo [ERROR] MSVC C++ tools not found. Run setup_dev.ps1 first. & exit /b 1 )
  call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
if not defined VCPKG_ROOT if exist "%ROOT%\tools\vcpkg\vcpkg.exe" set "VCPKG_ROOT=%ROOT%\tools\vcpkg"
if not defined VCPKG_ROOT ( echo [ERROR] VCPKG_ROOT is not set. Run setup_dev.ps1 first. & exit /b 1 )

for /f "usebackq tokens=*" %%v in (`powershell -NoProfile -Command "(Select-String -Path '%ROOT%\CMakeLists.txt' -Pattern '^\s*VERSION\s+([0-9.]+)').Matches[0].Groups[1].Value"`) do set "VERSION=%%v"
echo == AviCap Studio %VERSION%: Windows release (MSVC)

rem ---- configure + build
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_INSTALLED_DIR="%ROOT%\build\vcpkg_installed" ^
  -DAVICAP_BUILD_APP=ON -DAVICAP_BUILD_TESTS=ON || exit /b 1
cmake --build "%BUILD%" || exit /b 1

if %SKIP_TESTS%==0 (
  echo == Running unit tests
  ctest --test-dir "%BUILD%" -E stress --output-on-failure || exit /b 1
)

rem ---- stage
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\licenses" || exit /b 1
copy /y "%BUILD%\bin\AviCapStudio.exe" "%STAGE%\" >nul || exit /b 1
copy /y "%BUILD%\bin\avicap-cli.exe" "%STAGE%\" >nul || exit /b 1
copy /y "%BUILD%\bin\*.dll" "%STAGE%\" >nul
rem Visual C++ runtime, app-local (redistributable per the VC++ license).
if defined VCToolsRedistDir (
  for /d %%d in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do copy /y "%%d\*.dll" "%STAGE%\" >nul
)
set "SHARE=%ROOT%\build\vcpkg_installed\x64-windows\share"
for %%p in (ffmpeg dav1d zlib libvpl) do (
  if exist "%SHARE%\%%p\copyright" copy /y "%SHARE%\%%p\copyright" "%STAGE%\licenses\%%p-copyright.txt" >nul
)
copy /y "%ROOT%\third_party\imgui\LICENSE.txt" "%STAGE%\licenses\DearImGui-MIT.txt" >nul
copy /y "%ROOT%\installer\LICENSE-SUMMARY.txt" "%STAGE%\licenses\" >nul
copy /y "%ROOT%\installer\FFMPEG-SOURCE.txt" "%STAGE%\licenses\" >nul
if exist "%ROOT%\THIRD_PARTY_LICENSES.md" copy /y "%ROOT%\THIRD_PARTY_LICENSES.md" "%STAGE%\licenses\" >nul
powershell -NoProfile -Command "(Get-Content -Raw -Encoding UTF8 '%ROOT%\installer\README.txt').Replace('@VERSION@','%VERSION%') | Set-Content -Encoding UTF8 '%STAGE%\README.txt'"

rem ---- release folder
if not exist "%RELEASE%" mkdir "%RELEASE%"
del /q "%RELEASE%\*.exe" "%RELEASE%\*.dll" "%RELEASE%\*.zip" "%RELEASE%\SHA256SUMS.txt" 2>nul
copy /y "%STAGE%\*.exe" "%RELEASE%\" >nul
copy /y "%STAGE%\*.dll" "%RELEASE%\" >nul
xcopy /e /i /y /q "%STAGE%\licenses" "%RELEASE%\licenses" >nul
copy /y "%STAGE%\README.txt" "%RELEASE%\" >nul

rem ---- portable zip (portable.txt keeps user data next to the exe)
set "PTMP=%RELEASE%\portable-tmp\AviCap Studio"
if exist "%RELEASE%\portable-tmp" rmdir /s /q "%RELEASE%\portable-tmp"
mkdir "%PTMP%"
xcopy /e /i /y /q "%STAGE%" "%PTMP%" >nul
echo AviCap Studio portable mode: user data is stored in the UserData folder next to AviCapStudio.exe.> "%PTMP%\portable.txt"
powershell -NoProfile -Command "Compress-Archive -Path '%RELEASE%\portable-tmp\AviCap Studio' -DestinationPath '%RELEASE%\AviCap-Studio-Windows-Portable.zip' -CompressionLevel Optimal -Force" || exit /b 1
rmdir /s /q "%RELEASE%\portable-tmp"

rem ---- installer
if %SKIP_INSTALLER%==0 (
  set "ISCC="
  for %%x in (ISCC.exe) do if not "%%~$PATH:x"=="" set "ISCC=%%~$PATH:x"
  if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
  if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
  if not defined ISCC ( echo [ERROR] Inno Setup 6 ^(ISCC.exe^) not found. Run setup_dev.ps1 or use --skip-installer. & exit /b 1 )
  "!ISCC!" /Q "/DAppVersion=%VERSION%" "/DStageDir=%STAGE%" "/O%RELEASE%" "%ROOT%\installer\AviCapStudio.iss" || exit /b 1
)

rem ---- checksums
powershell -NoProfile -Command "Get-ChildItem '%RELEASE%' -File | Where-Object { $_.Name -in @('AviCapStudio.exe','AviCapStudio-Setup.exe','AviCap-Studio-Windows-Portable.zip') } | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name } | Set-Content -Encoding ASCII '%RELEASE%\SHA256SUMS.txt'"

echo == Done:
dir /b "%RELEASE%\*.exe" "%RELEASE%\*.zip" "%RELEASE%\SHA256SUMS.txt"
endlocal
