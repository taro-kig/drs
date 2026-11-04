<p align="center">
  <img src="assets/logo.svg" width="200" alt="drs logo: a rear wing with the DRS flap open, over three color wheels">
</p>

<h1 align="center">drs</h1>

<p align="center"><b>D</b>aVinci <b>R</b>esolve <b>S</b>tuff — DCTLs and plugins for DaVinci Resolve.<br>Flap open, grade faster. 🏎️💨</p>

## Usage

Uses [Task](https://taskfile.dev) (Windows: `winget install Task.Task` / macOS: `brew install go-task`).

```sh
task            # list tasks
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

## Contents

### dctl/

| File                    | Description |
| ----------------------- | ----------- |
| `PP8_to_StillBase.dctl` | Brings S-Log3 / PP8 footage closer to the look of a normalized still. Assumes a DaVinci Wide Gamut / DaVinci Intermediate working space; place it right after the input CST (S-Gamut3.Cine/S-Log3 → DWG/DI). Controls: exposure, contrast, highlight compression, saturation. |
