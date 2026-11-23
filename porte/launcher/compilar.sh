#!/bin/sh
# compila o launcher (Win32 puro) -> bin/Pokemon Alola PC.exe   (uso: sh compilar.sh)
cd "$(dirname "$0")"; export PATH="/c/msys64/ucrt64/bin:$PATH"
windres launcher.rc -O coff -o launcher_rc.o && \
g++ -O2 -std=c++17 -municode -mwindows -static -s launcher.cpp launcher_rc.o -o "../../bin/Pokemon Alola PC.exe" -lcomdlg32 -lshell32 -lgdi32 -luser32 && \
rm -f launcher_rc.o && ls -la "../../bin/Pokemon Alola PC.exe"
