#!/bin/sh
# Builds ReferenceScaler_Data/ReferenceScaler64.dll with mingw-w64
# (apt-get install mingw-w64). Statically linked: needs only Windows' own GDI+.
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-g++ -O2 -shared -static -static-libgcc -static-libstdc++ \
	-o ../ReferenceScaler_Data/ReferenceScaler64.dll ReferenceScaler.cpp \
	-lgdiplus -Wl,--kill-at
