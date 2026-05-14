@echo off
:: Build via MSYS2 bash
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

C:\msys64\usr\bin\bash.exe -l -c "cd '%ROOT%' && echo '=== Building DLL ===' && g++ -shared -o bin/SessionChanger.dll -I'/c/Program Files/Java/jdk-17/include' -I'/c/Program Files/Java/jdk-17/include/win32' -fPIC -O2 -s -static-libgcc -static-libstdc++ src/dll.cpp -lws2_32 2>&1 && echo 'OK: DLL' && echo '=== Building Injector ===' && g++ -o bin/VetoAltManager.exe -mwindows -O2 -s -static-libgcc -static-libstdc++ src/injector.cpp -lcomctl32 -ladvapi32 -lkernel32 2>&1 && echo 'OK: Injector' && echo '=== ALL DONE ==='"

pause
