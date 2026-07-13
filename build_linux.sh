#!/usr/bin/env bash
# Builds gmsv_metro_voicefx_linux.dll (32-bit) and gmsv_metro_voicefx_linux64.dll
# (64-bit) from source. Run this on any Debian/Ubuntu x86_64 machine - it does
# NOT need to be the game server itself, just something with the same OS family.
#
# Usage: ./build_linux.sh
# Output ends up in ./dist/

set -euo pipefail
cd "$(dirname "$0")"

echo "== Installing build dependencies (needs sudo) =="
sudo apt-get update
sudo apt-get install -y g++-multilib wget git

if ! command -v premake5 >/dev/null 2>&1; then
	echo "== Installing premake5 =="
	wget -q https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-linux.tar.gz -O /tmp/premake.tar.gz
	tar -xf /tmp/premake.tar.gz -C /tmp
	chmod +x /tmp/premake5
	sudo cp /tmp/premake5 /usr/bin/premake5
fi

if [ ! -d garrysmod_common ]; then
	echo "== Cloning garrysmod_common =="
	git clone --recursive --branch x86-64-support-sourcesdk https://github.com/danielga/garrysmod_common.git garrysmod_common
fi

echo "== Generating build files =="
premake5 --gmcommon=garrysmod_common gmake

echo "== Building 32-bit =="
cd projects/linux/gmake
make config=releasewithsymbols_x86
echo "== Building 64-bit =="
make config=releasewithsymbols_x86_64
cd ../../..

mkdir -p dist
cp projects/linux/gmake/x86/ReleaseWithSymbols/gmsv_metro_voicefx_linux.dll dist/
cp projects/linux/gmake/x86_64/ReleaseWithSymbols/gmsv_metro_voicefx_linux64.dll dist/

echo ""
echo "== Done =="
echo "Binaries are in ./dist/ :"
ls -la dist/
echo ""
echo "Copy the ONE matching your server's architecture (see HOW_TO_INSTALL.md"
echo "for the lua_run command that tells you win/linux + 32/64) into:"
echo "  garrysmod/lua/bin/"
