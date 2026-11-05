# dctl/legacy

Retired DCTLs, kept so old projects still open the same way. `task install` does **not** install them.

| File | Replaced by |
| ---- | ----------- |
| `HueVec_Density.dctl`, `HueVec_Sat.dctl`, `HueVec_Hue.dctl` | OFX **Hue Vectors** (`ofx/HueVectors`): the three in one node, run in the same order (Density → Sat → Hue), with a Richness master knob and the per-hue sliders in folded groups. |

If a project still uses them, run `task install:legacy` (installs everything plus these, under the same `drs/...` names as before). The next plain `task install` removes them again.

Not ported back: Hue Vectors also fixes a hard edge in very saturated violets/blues, where pushing toward the DWG blue primary takes luminance to zero and the DCTLs' luminance restore switched off abruptly. These files keep the old behavior so existing grades don't change.
