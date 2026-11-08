<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo/stereo_loom_dark_256.png">
  <img src="assets/logo/stereo_loom_light_256.png" width="96" alt="Stereo Loom logo">
</picture>

# Stereo Loom

Part of [Stereopticon](https://stereopticon.org): open-source stereoscopic 3D,
for every display.

<img width="210" height="276" alt="image" src="https://github.com/user-attachments/assets/9c5132f2-5673-4d70-82b9-5ea284c76aad" /> <img width="210" height="276" alt="image" src="https://github.com/user-attachments/assets/d8dcd576-908e-433a-bbd5-f5998231f833" />

A lightweight Windows tray app that shows stereo 3D on 3D displays: it weaves
for **Simulated Reality** displays (Samsung Odyssey 3D, Acer SpatialLabs, and
other LeiaSR / Dimenco panels), drives **Zondision** displays (the Zimo M1), and
can output anaglyph, side by side, top and bottom, interleaved, HDMI frame-packed or
3D Vision 3D for glasses,
3D TVs and projectors on any display. Point it at your screen, a window, or a
floating "looking glass", pick the stereo format your content is in, and it
converts and shows it in 3D.

> **Stereo Loom was called SR Loom** (and `SRLoom.exe`) up to v3.1. It was
> renamed when it learnt to drive displays other than Leia SR ones. Nothing is
> lost in the move: settings, profiles and the log are kept where SR Loom kept
> them (`HKCU\Software\SRLoom`, `%LOCALAPPDATA%\SRLoom` and `srweaver.log` beside
> the exe), so an existing setup carries straight over.

> **v3.1** (as SR Loom) was a single self-contained `SRLoom.exe`, needing a
> Simulated Reality display and the SR Platform runtime. New in 3.1: a Direct3D 12
> weaver option, anti-crosstalk controls, RGB + Depth pictures, an experimental
> light-field mode with no eye tracking, experimental HDR, faster and steadier
> Recovered Colour for anaglyphs, and custom anaglyph colours. See
> `CHANGELOG.md` for the full list.

## What it does

The LeiaSR weaver takes a **full side-by-side (SBS)** texture and produces the
lenticular, eye-tracked output. Stereo Loom captures your source, converts whatever
stereo layout it is in to SBS with a GPU shader, then weaves:

```
capture (screen / window)  ->  convert any stereo format to SBS  ->  weave  ->  present
```

### Stereo input formats
- **Side-by-Side** (Full / Half)
- **Top-and-Bottom** (Full / Half)
- **Interleaved** (Row / Column)
- **Checkerboard**
- **Anaglyph**, with several decode modes (Recovered Colour, filtered, half
  colour, mono) and a Custom colour pair you can pick from the picture
- **Frame Sequential** (temporal)
- **Pulfrich Effect** (time-delay or ND-filter)
- **Frame Packing**
- **Quilt** (a Looking Glass grid of views)
- **360°** VR video and pictures, 180° or 360°, Top-and-Bottom or Side-by-Side
- **RGB + Depth**: a picture beside its depth map. Each eye's view is drawn from
  the depth, and moving your head looks around the scene
- **Lytro Light Field** files
- **Katanga**: frames sent by a game or bridge over the Katanga shared-texture
  protocol
- **Automatic Detection** (experimental): Stereo Loom looks at the picture and works
  out the layout itself

### Display modes
- **Fullscreen**: weaves the whole SR display. The taskbar, the pointer and
  windows in front show through as cut-outs.
- **Window**: pick a window; the weave overlays and follows it.
- **Make active window 3D**: weave whatever window you are using (Ctrl+Alt+C).
- **Looking Glass**: a floating, draggable and resizable 3D viewport.

### Display outputs
Chosen in the Display Output section, below Stereo 3D Input. Everything before the output (the capture,
the stereo format, Gaussian Splats) works the same whichever is chosen.
- **Leia SR** (the default): the SR weave, eye tracked, for SR displays.
- **Zondision**: side by side on a Zondision display (the Zimo M1), which weaves
  it with its own chip; Stereo Loom switches the display's 3D on and off through
  Zondision's SDK, and its eye tracker drives Gaussian Splats. Needs Zondision's
  3DS software installed.
- **Anaglyph**: red / cyan, green / magenta or amber / blue glasses, on any
  display. Optimised (Dubois' matrices, least ghosting with red / cyan), Colour,
  Half Colour or Grey.
- **Side-by-Side** and **Top-and-Bottom**: for a 3D TV or projector in its own 3D
  mode, or for VR viewers that take a side by side picture.
- **Row Interleaved**, **Column Interleaved** and **Checkerboard**: for passive
  (polarised) and DLP 3D displays.
- **HDMI Frame Packing**: for 3D TVs and projectors that take frame-packed 3D.
  Stereo Loom switches the display to 1920 x 2205 (or 1280 x 1470 for 720p)
  while the 3D is on, leaving the refresh rate to the driver, and puts it back
  after. If the display does not offer that size, add it as a custom resolution
  (NVIDIA Control Panel, AMD Software or CRU).
- **3D Vision**: NVIDIA's shutter glasses, through
  [NV3D-Lib](https://github.com/oneup03/NV3D-Lib) as wiz3D does it. Needs the 3D
  Vision glasses, emitter and a 120 Hz panel, an NVIDIA driver with 3D Vision
  (R425 or older, or a newer one with
  [Vision Restoration](https://github.com/oscardelatorre/VisionRestoration)), and
  "Set up stereoscopic 3D" on in the NVIDIA Control Panel.

Each non-SR output can go to any display (automatic: the Zondision display, or
the primary), and **Swap Eyes** turns the eyes round for glasses or a display
that starts with the other eye. Without the SR Platform installed, Stereo Loom
starts with anaglyph (or Zondision, where a Zondision display is found).

### Weaving options
- **Weaver**: the SR SDK's weaver on Direct3D 11 (the default) or on Direct3D 12
  (experimental). With Direct3D 12 a slow conversion no longer holds the 3D back:
  the picture for your eyes is redrawn every refresh.
- **Anti-Crosstalk**: a strength slider under Convergence reduces ghosting
  between the eyes. The method (the display's default, Off, Static or Dynamic) is
  in Settings, Advanced.
- **Straight to the display**: while the weave covers the whole screen with
  nothing showing through, Stereo Loom bypasses Windows' compositor, which shortens
  the time from the weave to the display. It switches back by itself when a
  cut-out is needed.
- **Light field** (experimental, for Quilts and RGB + Depth): shows many views at
  once with no eye tracking, so the camera stays off and you move your head to
  look around. The picture is softer than tracked 3D, and the area you can move
  in is set by the lens.
- **HDR** (experimental): when Windows has HDR switched on for the 3D display,
  the pass-through layouts (Side-by-Side, Top-and-Bottom, Interleaved,
  Checkerboard) are captured, converted and woven in 16-bit float, so
  brighter-than-white picture is kept.

### Profiles

Stereo Loom can remember settings per game, per program and per media file. Focus a
game that has a saved profile and Stereo Loom switches to the right stereo format
automatically; load an image or video and it remembers what you last set.
Profiles live in `%LOCALAPPDATA%\SRLoom\profiles.ini`, which is hand-editable.
The top of the file lists every setting, and Stereo Loom reads the file again by
itself when you change it.

### Head tracking output to games

Stereo Loom forwards the SR display's head pose to PC games as head-look input, so
flight sims, racing titles, and any TrackIR / FreeTrack / OpenTrack-aware game
gets head tracking from the SR camera with no extra hardware. Three output
protocols are broadcast together by default:

- **OpenTrack UDP** (`127.0.0.1:4242`): modern indie sims (some MSFS plugins,
  flight sims that support OpenTrack natively).
- **FreeTrack 2.0 Enhanced**: DCS World, Falcon BMS, IL-2, MSFS, X-Plane,
  Project CARS, Elite Dangerous, Star Citizen and others.
- **TrackIR (NaturalPoint NPClient)**: iRacing, FSX/P3D, ArmA, Codemasters
  racers and most serious sims. The NPClient.dll is embedded in StereoLoom.exe and
  extracted to `%LOCALAPPDATA%\SRLoom\NPClient\` on first launch, so no OpenTrack
  install is required. If OpenTrack is already installed and its registry entry
  points at its own NPClient, Stereo Loom defers to it.

The pipeline: filtered SR eye-pair midpoint (position) + SR head pose
orientation → OneEuro + OpenTrack-style Accela on rotation → per-axis
sensitivity / invert / offset → output fan-out. The pose is also masked to the
set of axes you choose (XYZ + Yaw/Pitch, XYZ only, Yaw/Pitch, 6DOF, or
Yaw/Pitch/Roll).

**The default mode is XYZ + Yaw/Pitch**: position plus look direction, the
format the broadest range of titles use (sims, racing, action games with
head-look). Position comes from the SR runtime's *filtered* eye-pair stream
(`SR::EyeTracker::create()`), not the raw head-pose tracker. That is the same
data SR-native head-tracked titles use (*The First Berserker: Khazan*, *Lies of
P*, *Stellar Blade*, *Hell is Us*), so the smoothness matches.

There is one small inherent quirk. The eye-pair midpoint sits a few centimetres
in front of the head's rotation pivot, so turning your head produces a small
position drift as the midpoint traces an arc. This is geometric (SR-native games
show the same artefact) and cannot be cleanly subtracted without bringing
rotation noise back. The XYZ-only mode is there for the rare case where this
matters.

Right-click the tray icon → **Head Tracking** to toggle the master switch, pick
individual protocols, and pick the output mode. The compact panel has a small
on/off toggle for one-click "release the camera": when all outputs are off, SR
Loom's SR context is destroyed entirely and the head-pose camera goes off
(unless the weaver itself is running, which keeps the camera engaged).

## Controls

Left-click the tray icon for the control panel; right-click for a quick menu.

- **Ctrl+Alt+W**: toggle weaving on/off
- **Ctrl+Alt+F**: switch between Fullscreen ⇄ Looking Glass
- **Ctrl+Alt+C**: make the active window 3D (press again to turn it off)
- **Ctrl+Alt+R**: recalibrate head tracking (snap the current head pose to centre)
- **Ctrl+Alt+[** / **Ctrl+Alt+]**: Convergence down / up a step
- **Ctrl+Alt+-** / **Ctrl+Alt+=**: Anti-Crosstalk down / up 5%

The last two pairs show the new value in the corner of the 3D picture for a
moment, so both can be set by eye with a picture filling the screen.

The panel has a compact mode (just the on/off switch and a status line) and an
expanded mode with the display, stereo-input and depth controls (Convergence,
Anti-Crosstalk), profiles and settings. The theme button in the header offers
Auto (follows Windows), Light or Dark, and remembers your choice.

## Requirements

- **64-bit Windows 10 (1903+) or Windows 11.** Older builds lack the
  screen-capture APIs Stereo Loom relies on.
- A Simulated Reality display and a **current SR Platform runtime** installed
  (the SR Service must be running: it provides the `SimulatedReality*.dll`s and
  the eye tracking). An out-of-date runtime can be missing the modern weaver API.

**Note on primary monitor:** Samsung and Acer both tell users to make the SR
display Windows-primary. That guidance exists because their *launchers*
(Reality Hub, SpatialLabs Go / TrueGame) open games on whichever display
Windows considers primary. **Stereo Loom does NOT need the SR display to be
primary.** It targets the SR panel itself through the LeiaSR SDK's display
rect, so you can run with the SR display as a secondary monitor (recommended on
Windows 11). See `docs/sr-non-primary-research.md` for the full rationale and
SDK-level evidence.

## Troubleshooting

- **A "fullscreen" game shows a frozen 3D image, doesn't update, or has no input.**
  Stereo Loom captures and overlays via Windows' screen-capture API, which can't see
  or draw over a game in **exclusive fullscreen**. Run the game in **borderless**
  (or windowed) instead. Most modern games default to borderless-flip anyway; only
  true exclusive fullscreen is unsupported. Games that gate their own 3D mode
  behind exclusive fullscreen can't be driven yet.
- **`CreateDX11Weaver ... could not be located`:** your **SR Platform runtime is
  out of date**. Update it (and make sure the SR Service is running).
- **`CreateDirect3D11DeviceFromDXGIDevice ... could not be located`:** your
  **Windows is too old**; update to a current Windows 10/11.
- **Windows Defender / SmartScreen flags it.** It is an unsigned app that
  captures the screen and draws overlays, which trips antivirus heuristics. This
  is a false positive. The source is here for review; you can "Allow on device",
  and the detection has been reported to Microsoft.
- **A yellow border around the captured area.** That is Windows' capture
  indicator. Stereo Loom asks the OS to hide it, but Windows only grants that to
  packaged apps, so it may remain.
- **Variable refresh rate (G-Sync / FreeSync) causes judder.** Stereo Loom captures
  the source and re-presents to the SR display, so the SR display's VRR ends up
  synced to *Stereo Loom's* present cadence and not the source game's. If the game
  runs below the SR display's refresh rate, you'll see judder. This is a
  fundamental limitation of capture-based mirroring on Windows: Sunshine,
  ShaderGlass, OBS and every other tool in this category have the same
  constraint, and the DXGI API that could fix it (`SetPresentDuration`) is
  explicitly restricted by Microsoft to internal panels. For best results, cap
  the game at (or just below) the SR display's refresh rate so VRR isn't engaged,
  or disable VRR on the SR display and run a clean integer multiple
  (e.g. 60 fps source on a 120 Hz panel).
- **RTSS (RivaTuner Statistics Server) causes stutter or drops.** RTSS hooks
  `IDXGISwapChain::Present` in every Direct3D app, which interferes with SR
  Loom's output. If you use RTSS, **add `StereoLoom.exe` to its exclusion list**
  (RTSS Settings → "Application detection level" → exclude, or per-process
  exclusion). OBS users use the same trick for the same reason.
- **Stutter you can't explain.** Switch on **Perf Log** in Settings, Advanced,
  reproduce it, and attach `srweaver.log` (next to `StereoLoom.exe`) to a report. The
  log records frame timings and what the SR runtime reported at the time.

## Releasing / running

The build is a **self-contained `StereoLoom.exe`**. The UI fonts are embedded
and the MSVC runtime is statically linked. The SR Platform runtime (which an SR
display's owner already has) is loaded only when present, so the exe also starts
on PCs without it. Shipped beside the exe:
- `ZimoAdapter.dll`, `3DS_SDK.dll` and `jsoncpp.dll`, for Zondision displays
  (built and copied by CMake when Zondision's SDK is in `lib/Zondision`).
- the `neural` folder (ONNX Runtime, DirectML and the depth models), for
  Gaussian Splats.

## Building

Requires Visual Studio 2022/2026 (Desktop C++) and CMake ≥ 3.21. The build is
**x64 only** (the SR Platform runtime only installs on 64-bit Windows).

> The `lib/` folder (the Leia SR SDK) is **not** tracked in git. Place the
> LeiaSR SDK locally at `lib/Simulated Reality/LeiaSR-SDK-1.36.2-win64`.
> For Zondision support, unpack Zondision's C SDK (from developer.zondision.com)
> into `lib/Zondision/3DS_CSDK_V<version>`. ZimoAdapter.dll is then built with
> Zondision's evaluation key, which expires; a build with a key of your own reads
> it from `adapters/zimo/ZimoKey.private.h` (see `ZimoKey.example.h`), which is
> never committed. Zondision ask that app keys are kept inside the binary rather
> than published as text.
> For the 3D Vision output, put [NV3D-Lib](https://github.com/oneup03/NV3D-Lib)'s
> `include/NV3D.hpp`, `lib/x64/Release-MT/NV3DLib-mt.lib` and
> `external/nvapi/amd64/nvapi64.lib` in `lib/NV3D-Lib`; without them the build
> leaves 3D Vision out.

```powershell
cmake -B build/x64 -A x64
cmake --build build/x64 --config Release
# -> build/x64/Release/StereoLoom.exe
```

## Credits

- **[markleoryan79](https://github.com/markleoryan79)**: brought Katanga back into
  Stereo Loom's format list and built the
  [3D Slicer → Stereo Loom bridge](https://github.com/markleoryan79/3D-Slicer-Stereoscopic-Display-Extension/tree/SRLoomBridge),
  which sends stereo views of medical scans into Stereo Loom over Katanga. His work
  shaped Stereo Loom's Katanga receiver and auto-receive support.
- **[AlonBorodAHub](https://github.com/AlonBorodAHub)**: their
  [Odyssey Stereo Controls](https://github.com/AlonBorodAHub/Odyssey-Stereo-Controls)
  (MIT) worked out how to ask Samsung Odyssey 3D Hub to turn off its fullscreen
  "convert to 3D" popup and to put the preference back. Stereo Loom's Samsung 3D Popup
  switch (Settings, Samsung Odyssey 3D) works the same way.
- **[oneup03](https://github.com/oneup03)**: [NV3D-Lib](https://github.com/oneup03/NV3D-Lib)
  (LGPL 2.1) drives the 3D Vision output.
- **[oscardelatorre](https://github.com/oscardelatorre)**:
  [Vision Restoration](https://github.com/oscardelatorre/VisionRestoration) brings
  3D Vision back on current NVIDIA drivers.

## Licence

MIT (see `LICENSE`). Third-party parts (Dear ImGui, NPClient, the 1 Euro
Filter, the **Inter** font, stb_image, Odyssey Stereo Controls, NV3D-Lib, NVAPI) are listed with their licences in
`THIRD_PARTY_NOTICES.txt`. Links against Leia's **SR SDK** (MIT); the SR
Platform runtime must be installed separately. Zondision display support uses
Zondision's 3DS SDK, redistributed under the Zondision SDK and API Licence
Agreement (developer.zondision.com); it is not part of this repository.
