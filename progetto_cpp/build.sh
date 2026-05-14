#!/bin/bash
# VetoAltManager build script for MSYS2 bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd -W 2>/dev/null || pwd)"

echo "=== VetoAltManager Build ==="

# Ensure output dir
mkdir -p "$SCRIPT_DIR/bin"

# Find JDK JNI headers
JAVA_HOME="/c/Program Files/Java/jdk-17"
JNI_INCLUDES="-I\"$JAVA_HOME/include\" -I\"$JAVA_HOME/include/win32\""

echo "Step 1/2: Compiling SessionChanger.dll..."
g++ -shared -o "$SCRIPT_DIR/bin/SessionChanger.dll" \
    -I"$JAVA_HOME/include" \
    -I"$JAVA_HOME/include/win32" \
    -fPIC -O2 -s -static-libgcc -static-libstdc++ \
    "$SCRIPT_DIR/src/dll.cpp" -lws2_32

if [ $? -ne 0 ]; then
    echo "ERROR: DLL compilation failed!"
    exit 1
fi
echo "  OK: bin/SessionChanger.dll ($(stat -c%s "$SCRIPT_DIR/bin/SessionChanger.dll") bytes)"

echo "Step 2/2: Compiling VetoAltManager.exe..."
g++ -o "$SCRIPT_DIR/bin/VetoAltManager.exe" \
    -mwindows -O2 -s -static-libgcc -static-libstdc++ \
    "$SCRIPT_DIR/src/injector.cpp" -lcomctl32 -ladvapi32 -lkernel32

if [ $? -ne 0 ]; then
    echo "ERROR: Injector compilation failed!"
    exit 1
fi
echo "  OK: bin/VetoAltManager.exe ($(stat -c%s "$SCRIPT_DIR/bin/VetoAltManager.exe") bytes)"

echo ""
echo "=== BUILD COMPLETE ==="
echo ""
echo "Files:"
echo "  bin/VetoAltManager.exe  (the injector GUI)"
echo "  bin/SessionChanger.dll  (the DLL payload)"
echo ""
echo "Usage:"
echo "  1. Run VetoAltManager.exe as ADMINISTRATOR"
echo "  2. Launch Minecraft (Vanilla / Lunar / Badlion)"
echo "  3. Click Refresh, select the Java process"
echo "  4. Click Inject DLL (one time only)"
echo "  5. Enter username -> Change Alt (Pipe)"
