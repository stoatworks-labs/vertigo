# Vertigo

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The dolly-zoom maths is
> verified numerically by an offline harness that drives the real plugin class in
> a headless GL context: the GLSL is measured against an independent C++
> implementation across 180 parameter combinations and agrees to within 0.9 of an
> 8-bit level, the depth-map solve is measured against the same solve in double
> across 45 more (0.8 levels), and the anchor surface is confirmed not to move at
> all — 0.0000 of travel, while the rest of the frame travels 20% of the frame
> (see [Status](#status)). It **runs in Resolume Arena** — loaded and confirmed
> working by the author on 2026-08-17 — but it has never been loaded into Resolve
> or any other OpenFX host, and no Windows build has been run. Check it in your
> own rig before trusting it in a show.

The dolly zoom — the shot where one thing holds still and the world moves — as an
[FFGL](https://github.com/resolume/ffgl) effect for [Resolume](https://resolume.com)
Arena and Avenue, and an [OpenFX](https://openeffects.org) effect for DaVinci
Resolve, Nuke, Natron and Vegas.

<!-- downloads:start -->

## Download

**[v0.1.3](https://github.com/stoatworks-labs/vertigo/releases/tag/v0.1.3)** — prebuilt for macOS and Windows. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`vertigo-0.1.3-macos-universal.dmg`](https://github.com/stoatworks-labs/vertigo/releases/download/v0.1.3/vertigo-0.1.3-macos-universal.dmg) | 206 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`vertigo-macos-universal.zip`](https://github.com/stoatworks-labs/vertigo/releases/latest/download/vertigo-macos-universal.zip) | 161 KB |
| Universal (Apple Silicon + Intel) · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`vertigo-ofx-macos-universal.zip`](https://github.com/stoatworks-labs/vertigo/releases/latest/download/vertigo-ofx-macos-universal.zip) | 237 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`vertigo-0.1.3-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/vertigo/releases/download/v0.1.3/vertigo-0.1.3-windows-x86_64-setup.exe) | 214 KB |
| x64 · .zip archive | [`vertigo-windows-x86_64.zip`](https://github.com/stoatworks-labs/vertigo/releases/latest/download/vertigo-windows-x86_64.zip) | 107 KB |
| x64 · .zip archive (OpenFX — Resolve, Vegas, Nuke) | [`vertigo-ofx-windows-x86_64.zip`](https://github.com/stoatworks-labs/vertigo/releases/latest/download/vertigo-ofx-windows-x86_64.zip) | 67 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/vertigo/releases](https://github.com/stoatworks-labs/vertigo/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## What it does

A dolly zoom is one camera move and one lens move arranged to cancel. The camera
tracks along its own axis; the focal length changes to hold one surface at
exactly the size it already was. That surface does not move, and everything at
any other distance does — which is why it reads as the ground giving way rather
than as a zoom. Hitchcock's cameraman worked it out for a stairwell in 1958 and
it has been the shorthand for vertigo ever since.

It is a *depth* effect, and a video clip has no depth. So the honest part of this
plugin is where the depth comes from, and there are two answers:

- **Radial** invents a field — near on the optical axis, falling away to the
  frame corners, or the reverse. Nothing is read out of the picture, so the
  geometry is a closed form with no failure cases, no holes, and no dependence on
  what the clip contains. It works on any footage at all, and what it gives you
  is the *look* of the shot.
- **Luma** and **Alpha** read a field out of the clip. Given real depth — a
  rendered depth pass, a matte, a generated depth map baked into a channel — this
  is the actual reprojection, and the picture comes apart in depth the way the
  real move does. Given an ordinary clip it turns brightness into geometry, which
  is either a mistake or an effect, depending on the clip.

## Install

Download the release for your platform, then:

**Resolume (FFGL)** — put `Vertigo.bundle` (macOS) or `Vertigo.dll` (Windows) in

```
~/Documents/Resolume Arena/Extra Effects
~/Documents/Resolume Avenue/Extra Effects
```

and restart Resolume. It appears under Effects as **Vertigo**.

**Resolve, Nuke, Natron, Vegas (OpenFX)** — put `Vertigo.ofx.bundle` in
`/Library/OFX/Plugins` (macOS) or `C:\Program Files\Common Files\OFX\Plugins`
(Windows). It appears under the **Stoatworks** group.

The macOS builds are Developer ID-signed and notarised, so they load with no
Gatekeeper step. The Windows builds are unsigned — see
[docs/UNSIGNED.md](docs/UNSIGNED.md) for what SmartScreen will say and what to do
about it.

## Controls

### Shot

| Control | What it does |
|---|---|
| **Dolly** | How far the camera travels. 0.5 is no move; below is a pull back, above is a push in. It is geometric in the parallax ratio between the nearest surface and infinity, so equal travel either side of the middle gives exactly reciprocal shots. |
| **Relief** | How much depth the scene has. 0.5 is flat — and a flat scene cannot be dolly-zoomed, so that is a true null however hard the dolly is driven. Below 0.5 turns the depth inside out: the middle becomes the far end, which is the stairwell shot rather than the subject shot. |
| **Anchor** | Which depth is held fixed. 1 is the nearest surface, 0 the furthest. In Radial, 1 is the optical axis and 0 the frame corners. |

### Depth

| Control | What it does |
|---|---|
| **Depth** | Radial, Luma or Alpha — see above. |
| **Falloff** | Gamma on the depth field: where between the near and far ends most of the scene sits. 0.5 is linear. |
| **Smooth** | Blurs the depth field, not the picture. Does nothing in Radial, which is already smooth; on a real depth map it is what stops a hard depth step smearing. |

### Frame

**Centre X / Centre Y** put the optical axis where the subject is — the move is
about that point. **Overscan** scales the picture to buy back the frame that a
push in gives away.

### Output

**Edges** decides what to show where a push in looks past the picture
(Transparent, Black, Clamp, Mirror, Wrap). **Quality** is supersampling — Fast is
1 sample per pixel, Good 4, Best 16. It does not change the geometry, only how it
is sampled.

### Presets

Seven factory shots, from **Vertigo** through **Stairwell** to **Slam**. Picking
one sets the shot controls; editing any of them afterwards falls back to Custom.
They deliberately leave **Depth** alone: whether a clip carries a usable depth map
is a fact about the footage, not a look.

## Two things worth knowing

**The pull-back direction has no edge artefacts.** Pulling back magnifies the
background, which means reading from inside the picture, so nothing ever reaches
the frame edge. Pushing in shrinks the background, which means reading from
*beyond* it — that is what Edges and Overscan are for. Both are the shot; only one
explains itself, which is why the default is a pull back.

**There are two nulls, and both are exact.** Dolly at the middle is no move.
Relief at the middle is a scene with no depth. Either one gives back the original
picture byte for byte (at Quality = Fast; supersampling still resamples).

## Watch it

[**Vertigo — the dolly zoom as a Resolume plugin**](https://www.youtube.com/watch?v=cWMUqKOl4fU) (48s)

Rendered rather than screen-recorded: an FFGL plugin has no window, so the
footage is real frames through the real plugin class from `vgtest --pipe`,
driven by a cue sheet that lives beside the code. Two of its beats are
checkable rather than atmospheric — the anchor ring holding on the test card,
and the null, where the dolly runs at full travel and the picture does not move.

## Try it in a browser

`demo/` is the plugin's own shader ported to WebGL2, running on clips generated
in the page with the parameters the constructor declares, live at
**https://vertigo-demo.stoatworks-labs.com**. Run it locally with no build step:

```bash
python3 -m http.server 8792 --directory demo
```

Two of its presets exist to make a claim checkable rather than to look like
anything: **Null: no depth** drives the dolly as hard as the range goes with
Relief flat, and **Null: no move** does the opposite. Both give the picture back
untouched, because a scene with no depth cannot be dolly-zoomed and no camera
move is no camera move.

It is a port and not the plugin — it says so on the page, and nothing on it
measures anything.

## Status

Everything below is measured by `tools/verify.sh`, which runs in about 20 seconds
on the development machine.

| Check | Result |
|---|---|
| `vgtest --probe` — the GLSL against the C++, over dolly × relief × anchor × falloff | 180 combinations, worst disagreement **0.9 of an 8-bit level** |
| `vgtest --anchor` — the anchor surface does not move | 55 combinations, **0.0000 of travel** on the anchor while the frame travels up to 0.25 |
| `vgtest --depth` — the GPU's fixed-point depth solve against the same solve in double | 45 combinations, worst **0.8 of a level** |
| `tools/sweep.py` — no control silently dead | all 12 reach the picture |
| registration, OpenFX plist, ad-hoc sign, `lipo` | pass |

**What has never been checked:**

- **It has been loaded into Resolume Arena and works** — the author's own
  report on 2026-08-17, the day it was released, not a session observation. So
  the FFGL side is no longer only a claim about a harness: it registers, it
  instantiates, and it renders in the host it was written for.
- **It has never been loaded into Resolve**, or any other OpenFX host. The OFX
  build is still only checked against `ofxprobe`, so real texture sizes and that
  host's premultiplication behaviour remain unconfirmed — exactly what an
  offline harness cannot tell you about, because it supplies its own textures.
- **The Windows build has never been run.** It compiles: the v0.1.0 release ships
  a Windows x64 DLL, an OpenFX bundle and an installer, all built in CI. Nobody
  has loaded any of them into a Windows host.
- **Nothing has been timed.** Best quality is 16 samples per pixel and each one
  runs the depth solve three times in the Luma and Alpha modes; nobody has
  measured what that costs at 4K.
- Every number here comes from one M4 Max and none of them from CI — hosted
  runners have no GPU, so `vgtest` cannot run there.

## Building

```bash
git clone --recursive https://github.com/stoatworks-labs/vertigo
cd vertigo
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
tools/verify.sh
```

Add `-DCMAKE_OSX_ARCHITECTURES=arm64` for a much faster dev build. `cmake
--install build` drops the bundle straight into Resolume's plugin folder.

`AGENTS.md` is the orientation doc — the model, the invariants and the traps —
and `CLAUDE.md` is the short command reference.

## Licence

MIT — see [LICENSE](LICENSE). Built on other people's work; see
[ATTRIBUTIONS.md](ATTRIBUTIONS.md).

*Vertigo* is a 1958 Paramount film. This plugin is named after the shot it made
famous and is not connected with, endorsed by, or derived from the film or its
rights holders.
