<p align="center">
  <img src="assets/logo.svg" width="200" alt="drs logo: an F1 car with its DRS flap open and color-wheel rims">
</p>

<h1 align="center">drs</h1>

<p align="center"><b>D</b>aVinci <b>R</b>esolve <b>S</b>tuff — DCTLs and plugins for DaVinci Resolve.<br>Flap open, grade faster. 🏎️💨</p>

## Usage

Uses [Task](https://taskfile.dev), pinned in `aqua.yaml`.

- macOS / Linux: install [aqua](https://aquaproj.github.io/docs/install), then run `aqua i -l` in this repo
- Windows: `winget install Task.Task`

```sh
task            # list tasks
task lint       # make sure every DCTL is ASCII outside slider labels (runs before install)
task install    # copy dctl/ into Resolve's LUT/drs (also use this to reinstall)
task status     # show what is installed and what is outdated
task uninstall  # remove LUT/drs
task open       # open the LUT folder
```

After installing, restart Resolve or use Project Settings > Color Management > **Update Lists**.
The DCTLs then show up as `drs/...` in the DCTL list (ResolveFX DCTL).

| OS      | Install location                                                      |
| ------- | --------------------------------------------------------------------- |
| Windows | `C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\LUT\drs`    |
| macOS   | `/Library/Application Support/Blackmagic Design/DaVinci Resolve/LUT/drs` (use `sudo task install` if not writable) |
| Linux   | `/opt/resolve/LUT/drs`                                                |

> [!WARNING]
> Keep DCTL files **ASCII**, comments included. Non-ASCII text in code or comments (e.g. Japanese comments) makes the AMD OpenCL compiler fail and Resolve crashes with `Insufficient Kernel Work Group Size for globalKernel`.
> The one exception is the **label** of a `DEFINE_UI_PARAMS` line: Resolve only uses it for the UI, so emoji like 🟥🟧🟩⬜🟦🟪✚ are fine there and give the sliders color chips. `task lint` checks exactly this.

### OFX plugins

OFX plugins are C++ and need a compiler and CMake:

- Windows: `winget install Kitware.CMake` and Visual Studio 2022 Build Tools with the C++ workload
- macOS: Xcode Command Line Tools + `brew install cmake`
- Linux: `g++` + `cmake`

```sh
task ofx:build      # build + run the pixel-math tests (OpenFX 1.5.1 is fetched by CMake)
task ofx:install    # build, then copy drs_*.ofx.bundle into the OFX folder (close Resolve first; asks for admin)
task ofx:uninstall  # remove drs_*.ofx.bundle
```

They show up under Effects > OpenFX > **drs**.

| OS      | Install location                              |
| ------- | --------------------------------------------- |
| Windows | `C:\Program Files\Common Files\OFX\Plugins`   |
| macOS   | `/Library/OFX/Plugins`                        |
| Linux   | `/usr/OFX/Plugins`                            |

## Contents

### dctl/

| File                    | Description |
| ----------------------- | ----------- |
| `Film_Print.dctl`       | Negative → print film model, per channel in stops from 18% grey. Interimage (film saturation), dye density (saturated colors get darker), printer lights in real printer points (+ = more of that color), per-layer contrast for color shifts between shadows and highlights, print black depth and tint, and Mix. 18% grey stays put; output stays scene-referred. |
| `HueVec_Density.dctl`, `HueVec_Sat.dctl`, `HueVec_Hue.dctl` | Six-vector look development (Red, Yellow, Green, Cyan, Blue, Magenta on DWG primaries; neighbors blend seamlessly). **Density**: deeper, darker saturated colors like print film (neutrals untouched). **Sat**: per-hue saturation that keeps luminance and never leaves the gamut; **Hue Range** < 1 isolates a color (a yellow taxi without the skin next to it). **Hue**: shift each color toward a neighbor, up to 30°. All have **Deep** to keep highlights clean (**Highlight** shows what is excluded) — use the node's Global Blend to mix. Chain them Density → Sat → Hue. Note: in DWG, foliage sits near the Yellow vector. |
| `Split_Tone.dctl`       | Split toning: one hue into the shadows, another into the highlights (default teal / orange). Added in Oklab, so brightness is untouched. Hue sliders in Oklab degrees (60 orange, 200 teal, 250 blue...), Balance moves the split in stops from 18% grey, Softness sets the blend, Show Ranges paints where each color goes, Protect Black and White keeps the extremes clean. |
| `WB_Linear.dctl`        | White balance in scene-linear (Bradford adaptation), like a raw developer. Temperature = the light to neutralize (6504 K = no change; lower = cooler), Tint + = magenta / − = green. Place it early, right after the input CST or `SLog3_Normalize`. |
| `Contrast_Map.dctl`     | Makes contrast visible. **Local Contrast**: a heat map of how many stops the luminance varies around each pixel (blue = flat → red = punchy); small Radius shows texture, large Radius shows light/shadow separation. **Zones**: luminance posterized into stop bands around 18% grey (mid grey in green) with lines between zones. Turn it off before rendering. |
| `Exposure_Check.dctl`   | Clipping warning (red = all channels clipped, orange = one channel, blue = crushed, with optional zebra stripes) or false color by stops from 18% grey. Right after the CST it shows sensor clipping; on the last node it shows your grade. Turn it off before rendering. |
| `SLog3_Normalize.dctl`  | Normalizes S-Log3 footage (e.g. PP8) toward the look of a normalized still. Assumes a DaVinci Wide Gamut / DaVinci Intermediate working space; place it right after the input CST (S-Gamut3.Cine/S-Log3 → DWG/DI). Controls: exposure, contrast, highlight compression, saturation. |

### ofx/

| Plugin          | Description |
| --------------- | ----------- |
| `Look Explorer` | Find a look by picking, not tweaking. The viewer shows your frame as a 3×3 contact sheet of looks (round 0: Warm Print, Teal Orange, Faded Matte, Slide Film, Neutral, Bleach, Pastel, Moody Green, Nordic). Set **Pick** to the one you like and press **Next Round** to get 9 variations around it; they narrow down each round. **Back** / **Start Over** to undo, **View: Result** to see it full frame, then fine-tune with **Amount**, **Warmth** and **Punch**. Under the hood: per-channel film curve, interimage, dye density, split toning and hue shifts for skin, greens and blues. DWG / DI working space. CPU render. `look_preview` (built with the plugins) writes a BMP of the starting looks on a test chart. |
| `Color Anchors` | Up to 6 "From → To" color pairs picked with color swatches; similar colors follow smoothly. Matching is done in Oklab on lightness-normalized chroma, so a color is found at any exposure. **Match Lightness** limits an anchor to similar brightness, so shadows and highlights of the same hue can go different ways. Protect Neutrals, Mix and Show Mask included. DWG / DI working space. CPU render. |
