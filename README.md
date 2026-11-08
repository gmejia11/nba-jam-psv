<h1 align="center">
<img align="center" src="extras/screenshots/banner.png" width="50%"><br>
NBA Jam · PSVita Port
</h1>
<p align="center">
  <a href="#setup-instructions-for-players">How to install</a> •
  <a href="#controls">Controls</a> •
  <a href="#whats-changed-in-this-fork">Changes</a> •
  <a href="#building">Building</a> •
  <a href="#known-issues">Known issues</a> •
  <a href="#credits">Credits</a> •
  <a href="#license">License</a>
</p>

Jam with your favorite stars from all 30 NBA teams in over-the-top, high-flying,
2-on-2 arcade basketball just like you remember it – and like you’ve never seen
before!

> **This is a fork of [elliencode/nba-jam-psv][upstream]** focused on loading
> times. Starting a match now takes **~10 seconds** (was ~2 minutes) and quitting
> back to the main menu takes **~4 seconds** (was ~25–30 seconds with an app
> relaunch). See [What's changed in this fork](#whats-changed-in-this-fork).
> Fork maintained by **DominicanNY**. All credit for the original port goes to
> its author, **elliencode**.

This repository contains a loader for **the Android release of NBA Jam v.4.00.80**,
based on the [Android SO Loader by TheFloW][gtasa]. The loader provides
a tailored, minimal Android-like environment to run the official ARMv7
game executable on the PS Vita.

Disclaimer
----------------

**NBA Jam** is copyright &copy; 2011-2020 Electronic Arts Inc.
EA, EA SPORTS and the EA SPORTS logo are trademarks of Electronic Arts Inc.

The NBA and individual NBA member team identifications reproduced on this
product are trademarks and copyrighted designs, and/or other forms of 
intellectual property, that are the exclusive property of NBA Properties, Inc,
and the respective NBA member teams and may not be used, in whole or in part,
without the prior written consent of NBA Properties, Inc. &copy; 2011-2020
NBA Properties, Inc. All rights reserved.

The work presented in this repository is not "official" or produced or
sanctioned by the owner(s) of the aforementioned trademark(s) or any other
registered trademark mentioned in this repository.

This software does not contain the original code, executables, or other
non-redistributable parts of the original game product. The authors of
this work do not promote or condone piracy in any way. To launch and play
the game on their PS Vita devices, users must possess their own legally obtained
copy of the game in the form of .apk and .obb files.

Setup Instructions (For Players)
----------------

To properly install the game, follow these steps precisely:

- Install or update [kubridge][kubridge] and [FdFix][fdfix] by copying
  `kubridge.skprx` and `fd_fix.skprx` to your taiHEN plugins folder
  (usually `ur0:tai`) and adding the following two entries
  to your `config.txt` under `*KERNEL`:

```
  *KERNEL
  ur0:tai/kubridge.skprx
  ur0:tai/fd_fix.skprx
```

```diff
! ⚠️ Don't install `fd_fix.skprx` if you're using the rePatch plugin!
```

- Make sure you have `libshacccg.suprx` in the `ur0:/data/` folder on your
  console. If you don't, use [ShaRKBR33D][shrkbrd] to get it quickly and easily.

- <u>Legally</u> obtain your copy of NBA Jam for Android in the form
  of `.apk` and `.obb` files. This port is tailored for the v.4.00.80 (latest)
  version of the game. Other versions may not work.

    - If you have it installed on your phone, you can
      [get all the required files directly from it][unpack-on-phone]
      or by using any APK extractor that you can find on Google Play.

> ℹ️ Verify that your build is the correct version using **sha1sum** (can also
> be found as an online tool). The sha1sum for `lib/armeabi-v7a/libnbajam.so`
> **must** be `7D230A3A59B0F382DE055B9AF15BC2F3D87E010F`

- Open the `.apk` with any ZIP explorer (like [7-Zip](https://www.7-zip.org/))
  and extract the file `lib/armeabi-v7a/libnbajam.so`
  into `ux0:data/com.eamobile.nbajam_row_wf/` on your Vita. The correct resulting path:
  `ux0:data/com.eamobile.nbajam_row_wf/libnbajam.so`

- Open `main.40080.com.eamobile.nbajam_row_wf.obb` with any ZIP explorer (like [7-Zip](https://www.7-zip.org/))
  and extract all of its context into a separate folder on your computer.

- Install [Total Commander](https://www.ghisler.com/download.htm) and its [PSARC plugin](http://totalcmd.net/plugring/PSARC.html).

- Launch Total Commander and navigate up to the folder you just created.

- Select all the files and directories inside the folder, click on File -> Pack.

- Set `psarc` as Compressor and then click on `Configure` button right below.

- Set `PSARC Version` to `1.3`, `Compression` to `ZLIB` and `Ratio` to `0` and press `OK`

- Press `OK` to launch the compression, it will create a file in `C:\ArchiveName.psarc`. (If you get an error, manually change the location in the command line string `psarc: DESTINATIONFOLDER\ArchiveName.psarc`).

- Rename the newly created psarc archive to `data.psarc` and transfer it to `ux0:data/com.eamobile.nbajam_row_wf/`.

- Install `com.eamobile.nbajam_row_wf.vpk` (from [Releases][latest-release]).

Controls
-----------------

The port features support for physical/gamepad controls. In-game help menu contains
information and tutorials on using them.

What's changed in this fork
-----------------

Measured on real PS Vita hardware with the same `data.psarc`:

| | Original | This fork |
|---|---|---|
| Player select → match starts | ~2 minutes | **~10 seconds** |
| Quit match → main menu | ~25–30 seconds (app relaunch) | **~4 seconds** |

**Buffered PSARC reader** (`source/reimpl/io.c`)
- Every `fread()` on a file inside `data.psarc` used to be a synchronous FIOS
  request. Loading a match issues ~270,000 tiny reads (~75 bytes on average).
  Each open file now has a 256 KB read-ahead buffer, so those reads are served
  from RAM with only ~1,100 real FIOS requests. Large reads still go directly
  to FIOS without an extra copy.
- File sizes come from `sceFiosFHGetSize()` and reads use `sceFiosFHPreadSync()`.
- Fixed stdio semantics for PSARC files: `fread()` returns the number of items
  actually read, `feof()` and `fgetc()` work, and `fgets()`, `getc()` and
  `ungetc()` are now supported instead of being routed to the wrong libc.

**Shader cache**
- vitaGL is built with `HAVE_SHADER_CACHE=1`, so GLSL shaders are translated
  and compiled only once and then loaded from
  `ux0:data/shader_cache/<TITLEID>/` on later boots and matches.

**No more app relaunch when quitting to the main menu**
- The original port intercepted `StateMachine::Clear()` and relaunched the
  whole app behind a fake loading screen. With the other fixes in place, the
  game's real `StateMachine::Clear()` works fine (tested with several matches
  in a row), so returning to the menu no longer reloads everything.
- If the relaunch workaround is still needed, it is used as before, but the
  fake loading screen now ends as soon as the main menu has loaded
  (`MainMenuState::MenuManagerLoadedMenu`) instead of always lasting 20 seconds.

**Missing imports**
- Added `posix_memalign` (newlib declares it but does not implement it),
  `log2f`, `strtoll_l`, `strtoull_l`, `strtold_l` and `readlink`.

**Build changes**
- Uses upstream [vitaGL][vitagl] (the previous vitaGL submodule was not
  publicly accessible). It is built with `SOFTFP_ABI=1 HAVE_GLSL_SUPPORT=1
  NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1`.
- Links `mpg123`, `mp3lame` and `opus`, which newer `libsndfile` requires.
- Submodule URLs switched to HTTPS so the repo can be cloned without SSH keys.

### Optional switch files

Create an empty file with one of these names in
`ux0:data/com.eamobile.nbajam_row_wf/` to change the behaviour. Nothing is
needed for normal use.

| File | Effect |
|---|---|
| `use_relaunch` | Go back to the original "relaunch the app" workaround when quitting to the menu |
| `debug_log` | Write diagnostics to `psarc_io.log` (I/O statistics, blocked frames) and `relaunch_debug.log` |
| `psarc_legacy` | Use the original unbuffered PSARC reader |

Building
-----------------

This port **must** be built with the **softfp** flavour of VitaSDK, because the
Android `.so` uses the softfp ABI. A regular (hardfp) VitaSDK fails at link time
with `uses VFP register arguments` errors.

```bash
# 1. Install VitaSDK softfp into its own folder
git clone https://github.com/vitasdk-softfp/vdpm
cd vdpm
export VITASDK=$HOME/vitasdk-softfp
export PATH=$VITASDK/bin:$PATH
./bootstrap-vitasdk.sh
./install-all.sh

# 2. Clone and build
git clone --recurse-submodules https://github.com/gmejia11/nba-jam-psv
cd nba-jam-psv
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The resulting `com.eamobile.nbajam_row_wf.vpk` is in `build/`.

Known issues
-----------------

* The first boot and the first match after installing are slower, because
  shaders are compiled and stored in the shader cache.
* This port does not run in Vita3K: the emulator cannot allocate memory at the
  fixed address the `.so` loader needs.
* Multiplayer doesn't work.

Credits
----------------

- [elliencode][upstream] for the original NBA Jam PS Vita port this fork is based on.
- [The FloW][flow] for the original .so loader.
- [Rinnegatamante][rinne] for VitaGL.
- [gl33ntwine][gl33ntwine] for SoLoBoP.

License
----------------

This software may be modified and distributed under the terms of
the MIT license. See the [LICENSE](LICENSE) file for details.

[cross]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/cross.svg "Cross"
[circl]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/circle.svg "Circle"
[squar]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/square.svg "Square"
[trian]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/triangle.svg "Triangle"
[joysl]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/joystick-left.svg "Left Joystick"
[joysr]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/joystick-right.svg "Left Joystick"
[dpadh]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/dpad-left-right.svg "D-Pad Left/Right"
[dpadv]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/dpad-top-down.svg "D-Pad Up/Down"
[dpadu]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/dpad-up.svg "D-Pad Up"
[dpadd]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/dpad-down.svg "D-Pad Down"
[dpadl]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/dpad-left.svg "D-Pad Left"
[dpadr]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/dpad-right.svg "D-Pad Right"
[selec]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/dpad-select.svg "Select"
[start]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/dpad-start.svg "Start"
[trigl]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/trigger-left.svg "Left Trigger"
[trigr]: https://raw.githubusercontent.com/v-atamanenko/mc3-vita/master/extras/icons/trigger-right.svg "Right Trigger"

[gtasa]: https://github.com/TheOfficialFloW/gtasa_vita
[upstream]: https://github.com/elliencode/nba-jam-psv
[vitagl]: https://github.com/Rinnegatamante/vitaGL
[kubridge]: https://github.com/bythos14/kubridge/releases/
[fdfix]: https://github.com/TheOfficialFloW/FdFix/releases/
[unpack-on-phone]: https://stackoverflow.com/questions/11012976/how-do-i-get-the-apk-of-an-installed-app-without-root-access
[shrkbrd]: https://github.com/Rinnegatamante/ShaRKBR33D/releases/latest
[latest-release]: https://github.com/gmejia11/nba-jam-psv/releases/latest

[flow]: https://github.com/TheOfficialFloW/
[rinne]: https://github.com/Rinnegatamante/
[gl33ntwine]: https://github.com/v-atamanenko/
