@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ========================================
echo  VetoAltManager - C++ Build Script
echo ========================================

:: Find MinGW-w64
set "MINGW=C:\msys64\ucrt64"
set "GXX=%MINGW%\bin\g++.exe"
if not exist "%GXX%" (
    echo ERROR: g++ not found at %GXX%
    echo Install MSYS2 + MinGW-w64 first
    exit /b 1
)
echo Compiler: %GXX%

:: Find JDK for JNI headers
set "JAVA_HOME=C:\Program Files\Java\jdk-17"
if not exist "%JAVA_HOME%\include\jni.h" (
    echo ERROR: JDK not found at %JAVA_HOME%
    exit /b 1
)
echo JDK: %JAVA_HOME%

:: Create output directory
if not exist bin mkdir bin

:: Step 1: Compile the DLL
echo.
echo [1/2] Compiling SessionChanger.dll...
"%GXX%" -shared -o "bin\SessionChanger.dll" ^
    -I"%JAVA_HOME%\include" ^
    -I"%JAVA_HOME%\include\win32" ^
    -fPIC -O2 -s -static -static-libgcc -static-libstdc++ ^
    src\dll.cpp -lws2_32
if errorlevel 1 (
    echo ERROR: DLL compilation failed!
    exit /b 1
)
echo   OK: bin\SessionChanger.dll

:: Step 2: Compile the Injector
echo.
echo [2/2] Compiling VetoAltManager.exe...
"%GXX%" -o "bin\VetoAltManager.exe" ^
    -mwindows -O2 -s -static-libgcc -static-libstdc++ ^
    src\injector.cpp -lcomctl32 -ladvapi32 -lkernel32 -lpsapi -static
if errorlevel 1 (
    echo ERROR: Injector compilation failed!
    exit /b 1
)
echo   OK: bin\VetoAltManager.exe

:: Step 3: Copy files to bin
echo.
echo [3/3] Copying files...
copy /Y "bin\SessionChanger.dll" "..\VetoAlts\" >nul 2>&1
copy /Y "bin\VetoAltManager.exe" "..\VetoAlts\" >nul 2>&1
echo   Copied to ..\VetoAlts\

echo.
echo ========================================
echo  BUILD COMPLETE!
echo ========================================
echo.
echo  Files:
echo    bin\VetoAltManager.exe  (the injector GUI)
echo    bin\SessionChanger.dll  (the DLL payload)
echo.
echo  Usage:
echo    1. Run VetoAltManager.exe as ADMINISTRATOR
echo    2. Launch Minecraft (Vanilla / Lunar / Badlion)
echo    3. Click Refresh, select the Java process
echo    4. Click "Inject DLL" (one time only)
echo    5. Enter username -> "Change Alt (Pipe)"
echo.
