Unicode True
Name          "VetoAltManager Setup"
OutFile       "VetoAltManager_Setup.exe"
RequestExecutionLevel user

; LZMA maximum compression
SetCompressor /SOLID lzma
SetCompressorDictSize 64

; Use $PLUGINSDIR as temp extraction dir — NSIS auto-cleans it on exit
SilentInstall  silent
ShowInstDetails nevershow

Section
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    File /r "installer_bundle\"
    ExecWait '"$PLUGINSDIR\VetoInstaller.exe"'
SectionEnd
