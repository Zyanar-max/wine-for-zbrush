# Wine for ZBrush

Fixes for running **ZBrush** on Linux in **Bottles**. Install ZBrush in a bottle, run one script, and you're done.

> **I made this for my own setup and I'm sharing it as is. There's no support, so please don't contact me about it.** If it doesn't work on your system, `sh install.sh --uninstall` puts everything back. Use it at your own risk.
>
> Vibe-coded with [Claude](https://claude.com/claude-code) (Claude Code): the Wine bugs were tracked down and patched, and the file dialog and installer were written in conversation with Claude.

| # | Problem | Fix |
|---|---------|-----|
| 1 | Pen pressure stops working after you touch the mouse | Wine patch (winex11) |
| 2 | Pen pressure stops working after a dialog (e.g. Import) | Wine patch (wintab32) |
| 3 | Tearing near the top of the canvas while rotating | Wine patch (winex11) |
| 4 | Ugly Windows-style Open/Import/Save/Export windows | Uses your desktop's own file chooser (GNOME/KDE) |
| 5 | Decimation Master hangs at ~90% or crashes | Checks for Microsoft `vcomp140` and the DLL override |
| 6 | *"ZBrush is not responding"* after 5 s while loading | Optional: GNOME waits 20 s instead |
| 7 | .ztl / .zpr files show a blank page and don't open | ZBrush's own file icons, and double-click opens them in ZBrush |

## Requirements

- [Bottles](https://usebottles.com) (Flatpak or native)
- Runner **soda-11.0-10** installed in Bottles (*Preferences → Runners*)
- ZBrush installed in a bottle (tested: ZBrush 2023.2)
- An X11 desktop or XWayland. Tested on Rocky Linux 9 with GNOME on X11.

## Install

1. Close ZBrush and Bottles.
2. Run:
   ```sh
   sh install.sh
   ```
   The script finds the bottle that has `ZBrush.exe` in it. If you have more than one, use `sh install.sh --bottle NAME`.
3. Start ZBrush from Bottles.

**Don't change the bottle's runner in Bottles afterwards.** If you do, fixes 1–3 are lost; just run the script again.

## Set these in ZBrush yourself (after installing)

The script doesn't change these, because they're ZBrush settings and everyone wants them a bit different.

### Pen pressure: Use Tablet must be ON

**Preferences → Tablet → Use Tablet** must be turned on. On Windows, some tablet drivers (e.g. Xencelabs) need it turned off, so if you bring your ZBrush config over from Windows, it may come in switched off. Then the pen draws like a mouse, with no pressure, even though Wine is passing the pressure through.

### UI size (HiDPI / 4K)

Use ZBrush's own setting. **Don't use Wine's DPI / scaling** (`LogPixels` or Bottles' DPI option): it causes heavy display glitches in ZBrush.

1. **Preferences → Interface → UI → Button Size**: raise it. On a 4K screen, 70–75 is about 150%.
2. Save it with **Ctrl+Shift+I** (Store UI) and restart ZBrush.

### Sharp canvas

By default the document can be smaller than the canvas area. ZBrush then stretches it, so the model looks blurry, and it gets cut off at the edges while you rotate.

1. **Document → turn Pro off.** If it's on, ZBrush keeps the old aspect ratio when you resize.
2. Set **Width / Height** to the size of your canvas area (e.g. 3240 × 1880 on 4K), then **Resize**. If the canvas is cleared, drag your model out again and press **T**.
3. Press **0** (Actual size) so it's shown 1:1.
4. **Document → Save As Startup Doc**, so it starts like this next time.

A larger document means more pixels to redraw, so rotation gets a bit slower. If it's too slow, choose a smaller size.

## Uninstall

```sh
sh install.sh --uninstall
```

This restores the old runner, the original `wintab32.dll` and Wine's file dialogs. The DLL overrides and the GNOME timeout are left as they are.

## What it changes

- Creates the runner **soda-11.0-10-zbrush**. It's a copy of soda-11.0-10 made with hard links, so it takes no extra space, with 3 small binary patches. The original runner is not touched. Before patching, the script checks the SHA-256 of the original files and the exact bytes at each patch location; if anything differs, it changes nothing.
- Points the bottle to that runner (`bottle.yml`; a backup is saved as `bottle.yml.bak-wine-for-zbrush`).
- Copies the patched `wintab32.dll` into the bottle's `system32` (the original is kept as `wintab32.dll.orig`).
- Installs `zbfiledlg.dll` and `zbfiledlg.py` into `system32` and registers them for the Windows *FileOpenDialog / FileSaveDialog* (`system.reg`; a backup is saved as `system.reg.bak-wine-for-zbrush`).
- Sets `vcomp140`/`vcomp120`/`vcomp110`/`vcomp100` to `native,builtin` in `user.reg`.
- File types `application/x-zbrush-tool` (.ztl) and `application/x-zbrush-project` (.zpr), with icons taken from ZBrush's own `ZData/*_extension_icons.ico` (in `~/.local/share/icons/hicolor`, plus your icon theme if it lives in `~/.local/share/icons`, e.g. Papirus). Also a hidden "ZBrush" handler (`wine-for-zbrush-open.desktop` + `~/.local/bin/wine-for-zbrush-open`) that opens double-clicked files in the bottle. Files must be on a drive the bottle can see.

### The Wine bugs

1. **winex11 `update_device_mapping`** (staging/Proton patch) runs every time X switches between mouse and pen. It calls `XOpenDevice` → `XGetDeviceButtonMapping` → `XCloseDevice`, and `XCloseDevice` removes the client's XInput1 event selections for the pen. After that, no more pressure events arrive. Patch: skip the `XCloseDevice` call.
2. **wintab32 `WTPacket`**: the packet queue only holds 10 packets. If the app doesn't read it, for example while a modal dialog is open, Wine drops packets but keeps posting `WT_PACKET` messages. The app then asks for serial numbers that no longer exist and never gets pressure again. Patch: when a serial number isn't found, flush the queue.
3. **winex11 `x11drv_surface_flush`** calls `XShmPutImage` and then only `XFlush`. The app keeps drawing into the same shared memory before the X server has copied it, so the X server sometimes shows half of the new frame. Patch: use `XSync` instead of `XFlush`.

### File dialog

`zbfiledlg.dll` is a small COM wrapper around Wine's own dialog object. Every call is passed on to Wine, except `Show()`, which opens the desktop's file chooser through **xdg-desktop-portal** (the same one Flatpak apps use). The selected file is then handed back to ZBrush. If the portal can't be reached, the old Wine dialog appears. For a debug log, start ZBrush with `ZBFILEDLG_DEBUG=1`; the log is written to `system32\zbfiledlg.log`.

Files in your home folder work too, even if the bottle has no access to your home folder: the portal grants access to that one file only.

Source: `src/`. To build: `x86_64-w64-mingw32-gcc -O2 -s -shared -municode -o zbfiledlg.dll zbfiledlg.c zbfiledlg.def -lole32 -lshell32 -luuid -luser32 -static-libgcc`

## Tested on

**Software**

| | Version |
|---|---|
| ZBrush | 2023.2 (perpetual) |
| Bottles | 67.4 (Flatpak, GNOME Platform 50 runtime) |
| Wine runner | soda-11.0-10 (Wine 11.0) → patched copy `soda-11.0-10-zbrush` |
| Bottle | Application environment, Windows 10, DXVK 3.1.1, VKD3D-Proton 3.0.1, sync: fsync |
| OS | Rocky Linux 9.8, kernel 5.14.0-687 |
| Desktop | GNOME 40.10 on **X11** (Xorg 1.20.11), xdg-desktop-portal-gnome 41.2 |
| Tablet driver | xorg-x11-drv-libinput 1.0.1 (libinput 1.19.3) |
| GPU driver | NVIDIA 580.178.04 (open kernel module) |

**Hardware**

| | |
|---|---|
| CPU | AMD Ryzen 9 5900X (12 cores / 24 threads) |
| RAM | 128 GB |
| GPU | NVIDIA GeForce RTX 3090 |
| Display | 2 × 4K (3840 × 2160) 60 Hz |
| Tablet | Xencelabs Pen Tablet Medium + Quick Keys (wireless dongle) |

**Wacom** should work as well, because the bugs are in Wine's generic tablet code, but it hasn't been tested. Wayland (XWayland), KDE and other distros haven't been tested either.

## Known limitations

- The open dialog starts in *Recent* instead of the last folder (the GNOME portal on older systems ignores the start folder).
- The patches only work with **soda-11.0-10**. Other runners are refused (the script checks).
