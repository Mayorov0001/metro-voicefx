# MetroVoiceFX

Original, from-scratch voice-effects module for the Metro 2033 RP server, built
because VoiceBox FX's XEON DRM license check stopped working and the developer
has been inactive for ~1.5 years. This does not touch or reuse any VoiceBox FX
code or binaries - it's built on top of [gm_8bit](https://github.com/Meachamp/gm_8bit)
(LGPL-2.1, see `source/LICENSE`), an open-source GMod voice-interception module,
with new DSP effects layered on top (see `source/source/audio_effects.h` and
`source/source/voice_presets.h`).

Presets: `Radio`, `Phone`, `Stormtrooper`, `Combine`, `PA`, `Muffled`, `Masked`.
All built from generic building blocks - band-pass filtering, ring modulation,
noise, saturation, a granular pitch shifter - not from decompiling anyone's
product.

## 1. Determine your server's platform

Paste into your **server console** (Pterodactyl console or `lua_run` over
whatever admin access you have):

```
lua_run print((system.IsWindows() and "win" or "linux") .. (jit.arch == "x86" and "32" or "64"))
```

This tells you which of the four binaries you need: `win32`, `win64`,
`linux32` (called `linux` in filenames), or `linux64`.

## 2. Build

No compiled binaries are shipped here - you build them yourself so you always
have the source for whatever you're running.

**Linux** (most common for a GMod VDS): run `source/build_linux.sh` on any
Debian/Ubuntu x86_64 machine (doesn't have to be the game server itself - a
throwaway cloud VM works fine, or the VDS itself if you have apt/root there).
It installs `g++-multilib`, `premake5`, clones `garrysmod_common`, and builds
both 32 and 64-bit into `source/dist/`.

```
cd source
chmod +x build_linux.sh
./build_linux.sh
```

**Windows**: needs Visual Studio (Community is fine) with C++ build tools.
```
premake5.exe --gmcommon=garrysmod_common vs2019
```
then open `projects/windows/vs2019/metro_voicefx.sln` and build
`ReleaseWithSymbols` for both `Win32` and `x64`.

If you'd rather not set up a compiler at all, push this folder to a GitHub
repo and let GitHub Actions build it for you - `.github/workflows/c-cpp.yml`
already does both platforms on every push, you'd just download the artifacts
from the Actions tab.

## 3. Install (isolated - won't touch other addons)

1. Copy the ONE binary matching your platform from `dist/` (or the VS output)
   into `garrysmod/lua/bin/` on the server, e.g. `gmsv_metro_voicefx_linux64.dll`.
2. Copy the `addon/` folder here into `garrysmod/addons/metro_voicefx/`
   (so you end up with `garrysmod/addons/metro_voicefx/lua/autorun/server/...`).
3. Edit `garrysmod/addons/metro_voicefx/lua/metro_voicefx_config.lua` to set
   which players/teams/models/usergroups get which preset.
4. Restart the server (or `lua_openscript_serverside` the two Lua files for a
   hot-reload without a full restart, if you want to iterate faster).

Nothing here patches or overwrites any existing gamemode/addon files, so it's
safe to try alongside everything else you're running - to remove it, just
delete `garrysmod/lua/bin/gmsv_metro_voicefx_*.dll` and
`garrysmod/addons/metro_voicefx/`.

## 4. Test

As superadmin, in-game console:
```
metrovoice_test Combine
```
then talk in voice chat - you should hear the effect immediately. Switch
presets and re-test until the tuning sounds right; the exact filter/ring-mod
constants live in `source/source/voice_presets.h` if you want to adjust them
(e.g. make Combine's pitch drop stronger, add more static to Radio) - just
re-run the build after editing.

## 5. Rollback

Voice chat itself is never modified when the effect is `None`/unset - the
native hook just passes packets through untouched (see the `else` branch in
`hook_BroadcastVoiceData` in `source/source/main.cpp`). So if something
sounds wrong for a specific player, `metrovoice_test None` on them, or just
remove the two install steps above.
