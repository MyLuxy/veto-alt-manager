@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ============================================================
echo  VetoAltManager — Qt6 / Project Neo Build
echo ============================================================

:: ── Paths ──────────────────────────────────────────────────────────────────
set "MSYS2=C:\msys64"
set "UCRT=%MSYS2%\ucrt64"
set "MINGW64=%MSYS2%\mingw64"
set "CMAKE=%UCRT%\bin\cmake.exe"
set "NINJA=%UCRT%\bin\ninja.exe"
set "QT6=%UCRT%"
set "MINGW=%UCRT%"

:: ── Prepend both toolchain bin dirs to PATH so cc1plus/as/ld can find their DLLs
set "PATH=%UCRT%\bin;%MINGW64%\bin;%PATH%"

:: ── Sanity checks ──────────────────────────────────────────────────────────
if not exist "%CMAKE%" (
    echo ERROR: cmake not found at %CMAKE%
    echo.
    echo  Install via MSYS2 UCRT64 shell:
    echo    pacman -S mingw-w64-ucrt-x86_64-cmake
    echo    pacman -S mingw-w64-ucrt-x86_64-ninja
    pause & exit /b 1
)

if not exist "%UCRT%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo ERROR: Qt6 not found at %QT6%
    echo.
    echo  Install via MSYS2 UCRT64 shell:
    echo    pacman -S mingw-w64-ucrt-x86_64-qt6-base
    echo    pacman -S mingw-w64-ucrt-x86_64-qt6-declarative
    echo    pacman -S mingw-w64-ucrt-x86_64-qt6-5compat
    pause & exit /b 1
)

echo  cmake  : %CMAKE%
echo  ninja  : %NINJA%
echo  Qt6    : %QT6%
echo.

:: ── Step 1: Compile SessionChanger.dll (MinGW64 — msvcrt.dll runtime only) ───
:: Use mingw64 (not ucrt64) so the DLL only depends on KERNEL32 + msvcrt which
:: are guaranteed to exist in any Windows process, including Minecraft's JVM.
set "GXX=%MINGW64%\bin\g++.exe"
set "JAVA_HOME=C:\Program Files\Java\jdk-17"

if not exist "%JAVA_HOME%\include\jni.h" (
    echo WARNING: JDK not found at %JAVA_HOME% — skipping DLL build.
    echo          If you already have the DLL in bin\ this is fine.
    goto :skip_dll
)

echo [1/3] Compiling SessionChanger.dll (zero extra DLL deps)...
if not exist bin mkdir bin
"%GXX%" -shared -o "bin\SessionChanger.dll" ^
    -I"%JAVA_HOME%\include" ^
    -I"%JAVA_HOME%\include\win32" ^
    -fPIC -O2 -s ^
    src\dll.cpp -lws2_32 ^
    -Wl,-Bstatic,-lwinpthread,-lstdc++,-lgcc_s,-lgcc,-Bdynamic
if errorlevel 1 (
    echo ERROR: DLL compilation failed!
    pause & exit /b 1
)
echo   OK: bin\SessionChanger.dll

:skip_dll

:: ── Step 2: CMake configure ────────────────────────────────────────────────
echo.
echo [2/3] CMake configure (Qt6 Release)...

if not exist build_qt mkdir build_qt

"%CMAKE%" -S . -B build_qt ^
    -G "Ninja" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT6%" ^
    -DCMAKE_C_COMPILER="%UCRT%\bin\gcc.exe" ^
    -DCMAKE_CXX_COMPILER="%UCRT%\bin\g++.exe" ^
    -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" ^
    -DCMAKE_INSTALL_PREFIX=bin_qt

if errorlevel 1 (
    echo ERROR: CMake configure failed!
    pause & exit /b 1
)
echo   OK: build_qt\ configured.

:: ── Step 3: Build ─────────────────────────────────────────────────────────
echo.
echo [3/3] Building VetoAltManager (Qt6)...

"%CMAKE%" --build build_qt --config Release -- -j%NUMBER_OF_PROCESSORS%
if errorlevel 1 (
    echo ERROR: Build failed!
    pause & exit /b 1
)

:: ── Collect output ─────────────────────────────────────────────────────────
echo.
if not exist bin_qt mkdir bin_qt

:: The exe may land in build_qt\ or build_qt\Release\ depending on generator
for %%F in (build_qt\VetoAltManager.exe build_qt\Release\VetoAltManager.exe) do (
    if exist "%%F" (
        copy /Y "%%F" "bin_qt\VetoAltManager.exe" >nul
        echo   Copied: %%F -> bin_qt\VetoAltManager.exe
    )
)

:: Copy DLL alongside the new EXE
if exist "bin\SessionChanger.dll" (
    copy /Y "bin\SessionChanger.dll" "bin_qt\SessionChanger.dll" >nul
    echo   Copied: bin\SessionChanger.dll -> bin_qt\SessionChanger.dll
)

:: ── Qt DLL deploy ──────────────────────────────────────────────────────────
echo.
echo  Deploying Qt runtime DLLs to bin_qt\...
set "WINDEPLOYQT=%UCRT%\bin\windeployqt6.exe"
if not exist "%WINDEPLOYQT%" set "WINDEPLOYQT=%UCRT%\bin\windeployqt.exe"

if exist "%WINDEPLOYQT%" (
    "%WINDEPLOYQT%" --qmldir qml --release bin_qt\VetoAltManager.exe
    echo   Done.
) else (
    echo   WARNING: windeployqt not found — copy Qt DLLs manually or run from MSYS2.
    echo   From UCRT64 shell: windeployqt6 --qmldir qml --release bin_qt\VetoAltManager.exe
)

:: ── Copy UCRT forwarder DLLs (needed on some Windows 11 builds) ───────────
if exist "C:\Windows\System32\downlevel\api-ms-win-crt-runtime-l1-1-0.dll" (
    echo  Copying UCRT downlevel DLLs...
    for %%F in (C:\Windows\System32\downlevel\api-ms-win-crt-*.dll) do (
        copy /Y "%%F" "bin_qt\" >nul
    )
)

:: ── Copy logo from img\ ───────────────────────────────────────────────────
if not exist "bin_qt\img" mkdir "bin_qt\img"
for %%F in (..\img\*.png) do (
    copy /Y "%%F" "bin_qt\img\logo.png" >nul
    echo   Logo: %%~nxF -> img\logo.png
    goto :imgdone
)
:imgdone

:: ── Also copy to parent VetoAlts\ folder ───────────────────────────────────
if exist "bin_qt\VetoAltManager.exe" (
    xcopy /Y /E /I bin_qt ..\VetoAlts\ >nul 2>&1
    echo.
    echo  Deployed to: ..\VetoAlts\
)

:: ════════════════════════════════════════════════════════════════════════════
::  INSTALLER BUILD
:: ════════════════════════════════════════════════════════════════════════════
echo.
echo ============================================================
echo  Building VetoInstaller (Qt6)...
echo ============================================================

if not exist build_installer mkdir build_installer

"%CMAKE%" -S installer -B build_installer ^
    -G "Ninja" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT6%" ^
    -DCMAKE_C_COMPILER="%UCRT%\bin\gcc.exe" ^
    -DCMAKE_CXX_COMPILER="%UCRT%\bin\g++.exe" ^
    -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" ^
    -DCMAKE_INSTALL_PREFIX=installer_bundle

if errorlevel 1 ( echo ERROR: Installer CMake configure failed! & pause & exit /b 1 )

"%CMAKE%" --build build_installer --config Release -- -j%NUMBER_OF_PROCESSORS%
if errorlevel 1 ( echo ERROR: Installer build failed! & pause & exit /b 1 )

:: ── Assemble installer_bundle\ ──────────────────────────────────────────────
:: Bundle = app files (bin_qt\) + VetoInstaller.exe + its Qt DLLs
echo.
echo  Assembling installer_bundle\...
if not exist installer_bundle mkdir installer_bundle

:: Copy all app files first
xcopy /Y /E /I bin_qt installer_bundle\ >nul

:: Copy VetoInstaller.exe into bundle
copy /Y build_installer\VetoInstaller.exe installer_bundle\VetoInstaller.exe >nul
echo   Copied: VetoInstaller.exe

:: Deploy Qt DLLs for the installer exe (may add a few new ones)
if exist "%WINDEPLOYQT%" (
    "%WINDEPLOYQT%" --qmldir installer\qml --release installer_bundle\VetoInstaller.exe >nul
    echo   windeployqt for VetoInstaller done.
)

:: ── Run NSIS ────────────────────────────────────────────────────────────────
echo.
echo  Running NSIS...
set "MAKENSIS=C:\Program Files (x86)\NSIS\makensis.exe"
if not exist "%MAKENSIS%" set "MAKENSIS=C:\Program Files\NSIS\makensis.exe"

if exist "%MAKENSIS%" (
    "%MAKENSIS%" /V2 installer.nsi
    if errorlevel 1 ( echo ERROR: NSIS failed! & pause & exit /b 1 )
    echo   OK: VetoAltManager_Setup.exe
) else (
    echo   WARNING: NSIS not found — run makensis installer.nsi manually.
)

echo.
echo ============================================================
echo  BUILD COMPLETE
echo ============================================================
echo.
echo   bin_qt\VetoAltManager.exe      — main app (run as Admin)
echo   bin_qt\SessionChanger.dll      — injection payload
echo   VetoAltManager_Setup.exe       — single-file installer
echo.
pause
