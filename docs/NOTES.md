# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*Vertigo — the dolly zoom as an FFGL + OpenFX plugin; PUBLIC MIT v0.1.0, all homes live, RUNS IN ARENA (confirmed 2026-08-17)*

**Vertigo** — the dolly zoom (track back, zoom in, one surface holds still) as an
FFGL 2.1 effect for Resolume (ID `VG01`) and an OpenFX plugin for
Resolve/Nuke/Natron/Vegas. C++17 + GLSL 4.1, CMake, `~/Projects/resolume/vertigo`.
Built and released 2026-08-17. **PUBLIC MIT, v0.1.2** (v0.1.0 Aug 17, v0.1.1 Aug 18,
v0.1.2 Aug 22 2026), six release assets.

**All homes live**: repo `stoatworks-labs/vertigo`, release v0.1.0, project page
`stoatworks-labs.com/software/vertigo/`, the video-plugins suite (16th member),
demo `vertigo-demo.stoatworks-labs.com`, YouTube `cWMUqKOl4fU`, Instagram reel
`DcIPqtvChgR`. **It RUNS IN RESOLUME ARENA** — Allan confirmed 2026-08-17, so the
usual "never loaded into a host" caveat is retired. **Windows has been run too**,
by an external user who reported issue #2 from it — factory presets snapping back
to Custom, a host-behaviour bug no harness here reaches, fixed in `a0fb025`; that
is one user on one machine, GPU and driver unknown. Resolve has still never
opened it (OFX only ever met `ofxprobe`), nothing is timed, and the depth modes
have only ever been fed a radial card and a linear ramp — never a real rendered
depth pass.

**The one idea:** the model is written in **disparity, not depth**.
`m = (1 - σ·d_a)/(1 - σ·d)` with the field normalised to 0..1 and σ < 1 by
construction, so the denominator is unconditionally positive. **There is no
divide-by-zero guard in the plugin and there must not be one** — the depth form
has a pole (the camera reaching a surface) and this form removes it rather than
guarding it. The Dolly control is geometric in the parallax ratio
`P = 1/(1-σ)`, so the two directions are exact reciprocals.

**Depth comes from three places, and two of them differ in KIND from the first.**
Radial *invents* a field, so it is evaluated in OUTPUT space: closed form, one
fetch, no holes, works on any clip. Luma/Alpha read a field out of the picture,
which only exists in SOURCE space — the thing being solved for — so
`solveSourcePoint()` iterates a fixed 3 times. An inverse-mapped parallax cannot
tear; it SMEARS along a depth step, and `Smooth` blurs the depth (never the
picture) to soften the step.

⚠️ **v0.1.2 (2026-08-22) fixed the first external bug report, #2: factory presets
did not stick.** Resolume does **not** consume `FF_EVENT_FLAG_VALUE` — it keeps
pushing its own pre-preset values back, and copy-based apply read that as an
operator edit and dropped to Custom instantly. A preset is an override now, and
`hostValues[]` records what the host last *sent* so an edit can be told from an
echo. `vgtest --presets` drives three host behaviours with no GL and fails on the
pre-fix code in exactly the "ignores value events" column. **The same broken
pattern is in six other plugins** — [plugin factory presets](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_plugin_factory_presets.md).

**The bug worth remembering, because `--probe` could not see it.** Relief
originally scaled the disparity field *about the anchor*, which is the tidy-looking
arrangement. With a negative relief and the anchor off-centre the whole field
slides past the near plane, clamps flat, and a flat field is the identity — so the
control silently did nothing over half its travel, and the "Stairwell" preset
rendered byte-identical to the null. **The GPU and the C++ agreed perfectly, on
the same wrong answer**, because they are the same formula. It now scales about
the middle of the range with the anchor put through the same map
(`anchorDisparity`). Caught by `--anchor` (which refuses to pass when nothing
else moved) and by rendering each preset through ofxprobe.

Verification, all in `tools/verify.sh` (~20s): `--probe` GLSL vs C++ over 180
settings (worst 0.9 of a level); `--anchor` the anchor holds at **0.0000 travel**
over 55 while the frame travels 0.25; `--depth` the GPU's fixed-point solve vs
the same solve in double over 45 (worst 0.8); `sweep.py` all 12 controls live;
plus registration, the OFX plist + ad-hoc sign, and lipo. Two nulls are exact at
Quality=Fast (0 bytes differ through ofxprobe): Dolly centred, and Relief flat.

Traps in the repo's AGENTS.md. Built from the porthole/flipbook scaffolding —
see [new plugin repo copy traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_new_plugin_repo_copy_traps.md), [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md),
[resolume demo kit](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_resolume_demo_kit.md), **release workflow** (working-practice note, kept in Claude memory).
