# AGENTS.md — bringing an LLM up to speed on Vertigo

Orientation for an AI assistant (or a new human) picking this project up cold.
`CLAUDE.md` holds the short command reference; this file explains the model and
the traps.

---

## 1. What this is

The **dolly zoom** as an FFGL effect for Resolume Arena / Avenue and an OpenFX
effect for Resolve, Nuke, Natron and Vegas. C++/GLSL, CMake, public MIT.

The one idea to internalise before changing anything:

> **It is a perspective reprojection, not a warp that looks like one.** The
> camera moves along its own axis and the focal length changes to hold one
> surface fixed. Everything else follows from that, including which controls
> exist.

### Written down

A point at depth `z` projects to image radius `r = f X / z`. Dolly by `t` and
change the focal length to `f'` and it lands at `f' X / (z - t)`. Requiring the
anchor surface at `z_a` to be unmoved fixes `f' = f (z_a - t) / z_a`, so

```
m(z) = [ z (z_a - t) ] / [ z_a (z - t) ]
```

That form has a pole — the camera reaching `z = t` — and it is written in depth,
which is not what a depth map holds. Both problems go away at once in
**disparity**, `d = z_a / z`:

```
m(d) = ( 1 - sigma*d_a ) / ( 1 - sigma*d )        sigma = t / z_a
```

The disparity field is normalised into `0..1` by construction and `sigma` is
below 1 by construction, so the denominator is unconditionally positive. **There
is no guard anywhere in this codebase against a divide-by-zero in the
magnification, and there should not be one** — if you find yourself wanting to
add one, something upstream has stopped holding an invariant and that is the bug.

The quantity an operator is actually reaching for is the ratio between how much
the nearest surface grows and how much infinity does:

```
P = m(1) / m(0) = 1 / ( 1 - sigma )
```

so **that** is what the Dolly control is, geometrically, in stops. `P = 1` is no
shot at all.

### Three properties fall out

Break one of these and you have broken the model, not the look:

- **The anchor does not move.** `m(d_a) = 1` identically, for every dolly, every
  relief, every depth field. `vgtest --anchor` measures it and gets 0.0000.
- **A flat scene is the identity.** With no depth variation every surface is the
  anchor, so the picture is untouched however hard the dolly is driven. A dolly
  zoom on a painted backdrop is nothing, and the maths says so without being
  told.
- **The two directions are reciprocal.** Equal travel either side of the middle
  of the Dolly control gives parallax ratios `P` and `1/P`.

There is deliberately **no wet/dry mix**. Cross-fading two geometries
double-exposes the picture rather than easing between them. The nulls are Dolly
at centre and Relief at centre, and both are exact.

## 2. Where the depth comes from

This is the part that is actually interesting, because a video clip has no depth.

```
Radial   an invented field: near on the axis, far at the corners, or reversed
Luma     the clip's own brightness, read as a depth map
Alpha    the clip's alpha, read as a depth map
```

They are not three settings of one thing. **Radial is evaluated in OUTPUT space
and the sampled fields in SOURCE space, and that is the whole difference**:

- An invented field can be asked about the pixel being *written*, so the
  magnification is known before anything is fetched, the source point is a closed
  form, and there is exactly one texture fetch. No holes, no failure cases.
- A field carried by the picture only exists at the pixel being *read* — and
  which pixel that is, is the thing being solved for. So `solveSourcePoint()`
  iterates: guess, look up the depth, follow the magnification to a better guess,
  repeat. Three times, fixed, in uniform control flow.

**An inverse-mapped parallax cannot leave a hole.** Every output pixel fetches
something. What it does instead at a depth discontinuity is *smear* along the
step, and softening the step is the whole repair — that is what `Smooth` is for,
and it blurs the depth, never the picture.

## 3. The shape of it

One shader pass. Nothing accumulates between frames and no pixel depends on any
other, so there are no intermediate buffers at all — which sidesteps both
`FFGLFBO` bugs in the fleet's SDK notes without having to think about them.

```
source/Dolly.{h,cpp}     the model, and the 0..1 -> physical mapping
source/Shaders.{h,cpp}   the GLSL: a mirror of the above, plus the depth solve,
                         edge modes and supersampling
source/Vertigo.{h,cpp}   FFGL host glue and the parameter declarations
source/ofx/VertigoOFX.cpp  the OpenFX build: links Dolly.cpp, mirrors the pixel
                         machinery on the CPU
source/Presets.h         one preset table, read by BOTH builds
source/Diag.{h,cpp}      a log file, for the shader that will not compile
tools/vgtest/            headless render and three measurements
tools/sweep.py           no control is silently dead
tools/verify.sh          all of the above, in one go
```

### The maths exists twice, on purpose

It has to run per-pixel on the GPU, and it has to be readable and testable on the
CPU. Two copies of one formula drift apart — so **`vgtest --probe` measures one
against the other** and is the reason the duplication is safe. Change one copy,
change the other, then run the probe.

The OpenFX build is a third copy of the *pixel machinery* but **not** of the
model: it links `Dolly.cpp` straight from source. Edit the shader's pixel
machinery — edge modes, the supersample grid, the 5-tap depth blur, the solve
loop — and edit `VertigoOFX.cpp` too. The model itself has one home.

## 4. Traps

### The one that already bit: scaling relief about the anchor

`disparity()` scales the field about **the middle of the range**, and
`anchorDisparity()` puts the anchor level through that same map. The obvious
alternative — `anchor + relief*(field - anchor)`, which holds the anchor fixed by
construction and looks tidier — is wrong, and wrong in a way that is very hard to
see:

As soon as relief goes negative and the anchor is not at 0.5, the whole field
slides off the end of the range. With the anchor at the near end, an inverted
field lands entirely past the near plane, clamps to a constant, and a constant
field is the identity. **The control silently does nothing over half its travel.**

The reason it is worth this much comment: **`--probe` could not see it.** The GPU
and the C++ agreed perfectly — on the same wrong answer, because both were the
same formula. It was caught by `--anchor`, which refuses to report a pass when
nothing else in the frame moved, and by rendering a preset through `ofxprobe` and
noticing that "Stairwell" came back byte-identical to the flat null. Both of those
checks exist because a self-consistent mirror proves consistency and nothing else.

`tools/verify.sh` now sweeps the inverted half of the relief range explicitly.

### The macOS one that will get you

**`CMAKE_OSX_ARCHITECTURES` must be set before the first target is created.** Set
it later and CMake silently ignores it — you get an arm64-only binary that the
build log calls a success, and an Intel Resolume that quietly fails to load the
plugin.

**Always verify the artefact, never the log:**

```bash
lipo -archs build/Vertigo.bundle/Contents/MacOS/Vertigo
```

### A ranged parameter cannot have a ranged default

`SetParamRange` exists, and Resolume honours it. But
`CFFGLPluginManager::SetParamInfo` **clamps an `FF_TYPE_STANDARD` default into
0..1** before returning, and `SetParamRange` can only be called *afterwards* (it
looks the parameter up by ID, so the parameter has to exist first). There is no
`SetParamDefault`. So a parameter declared in stops cannot declare a default in
stops.

Hence: **every parameter here is a plain 0..1 float**, and the conversion to
stops, gammas and tap counts lives in `Dolly.cpp`. (SDK `b1afaf9`,
`FFGLPluginManager.cpp`.)

### A display-only TEXT parameter needs `SetTextParameter` overridden

The SDK's `instantiateGL` pushes *every* parameter's default into a fresh
instance and destroys the instance on the first `FF_FAIL` — and the base
`CFFGLPlugin::SetTextParameter` is a stub that returns `FF_FAIL`. So declaring
the About text line without overriding the setter means **no real host can
instantiate the plugin at all**, and the harness cannot see it, because it drives
the class directly and bypasses `plugMain`. `Vertigo.cpp` overrides it.

### MaxUV: the texture is bigger than the picture

FFGL hands over a texture that may be larger than the image in it, with `MaxUV`
describing the fraction actually drawn. A filter that samples where it was told
never notices. **This effect samples wherever it likes and routinely wants source
from beyond the frame edge, so it does.**

- All geometry happens in **picture space, 0..1**, and `MaxUV` is applied at the
  very last moment, in `fetchInside()`. The vertex shader deliberately does *not*
  pre-multiply UV by MaxUV the way the SDK's example filters do.
- Every fetch is clamped to at least **half a texel inside** the picture.
  `GL_LINEAR` exactly at the picture edge takes half its weight from padding that
  contains nothing.

### The depth fetch is NOT routed through the edge mode

`rawDepth()` uses `fetchInside()` and not `fetch()`, deliberately. Off the frame
the depth extends from the edge whatever the picture is doing there — because
Transparent, say, would read as alpha 0 and luma 0, which is "infinitely far",
and that puts a ring of runaway magnification just outside every frame.

### GLSL reserved words

`smooth` is one, which is why the uniform is `DepthSmooth` — and `flat`,
`active`, `filter`, `input`, `output`, `sample`, `common` and a long tail of
others. The failure mode is nasty: the shader fails to compile at *runtime*,
`InitGL` returns `FF_FAIL`, and Resolume shows an effect that silently does
nothing. That is what `source/Diag.cpp` is for.

### The plugin registers itself from a static constructor

`CFFGLPluginInfo` is a file-scope object in `Vertigo.cpp` that nothing references
by name. That is why `vertigo_core` is an **OBJECT** library and not a **STATIC**
one: in an archive the linker is entitled to drop the whole translation unit, and
you get a bundle that loads, exports `plugMain`, and reports that it contains no
plugins. Do not "tidy" it to STATIC. `verify.sh` checks for the literal `VG01` in
the binary's strings, which is present if and only if that translation unit
survived.

### `vcpkg.json` is invisible from the CMakeLists

`find_package(GLEW REQUIRED)` is guarded by `if(NOT APPLE)`, and GLEW arrives
through the vcpkg manifest — which nothing in `CMakeLists.txt` mentions. Delete
it and every local build and every macOS CI job stays green while the Windows job
fails at *configure*.

### `cmake/InfoOFX.plist.in` is parameterised, and must stay that way

The version this was copied from in another repo had the previous plugin's name
hardcoded into `CFBundleExecutable`. That fails nothing — the bundle assembles,
the binary is universal, `nm` finds `OfxGetPlugin`, and a probe host renders a
correct frame — until `codesign` in the release job, after the tag, with a
message about a "subcomponent" that never mentions the plist. `verify.sh` runs
that exact step locally.

## 5. Testing

There is no unit test rig and there cannot usefully be one — the output is a
picture. But a *reprojection* is unusually testable, because where a pixel went
is a number rather than a matter of taste.

```bash
tools/verify.sh          # everything below, about 20 seconds
```

### `--probe` — the GPU against the C++

Feeds in a picture whose brightness **is** the normalised radius, so the value
that comes back out of an output pixel says, as a number, which source radius the
shader sampled from. That is compared against `Dolly.cpp`.

```bash
./build/vgtest --probe --set "Dolly=0.75" --set "Relief=0.9"
```

Agreement is under 0.9 of an 8-bit level over 180 combinations, which is the
quantisation of the ramp rather than the maths.

**What it cannot see:** anything wrong with the model itself. Both copies are the
same formula, so both can be wrong together — see the relief trap above.

### `--anchor` — the model against its own definition

Renders at five dolly settings and measures how far each radius travelled.
The anchor ring must not have moved; **something else must have**. It reports
INCONCLUSIVE rather than a pass when nothing moved, which is the check that
caught the relief bug.

It measures on the radial ramp and not on the depth card, and that is not an
arbitrary choice. The obvious version — difference two card renders near the
anchor radius — does not work: the magnification is exactly 1 only exactly on the
ring, a band of any usable width contains radii where it is not, and those pixels
carry grid lines, so a shift of a fraction of a pixel comes back as tens of
levels. That number is real, and it is measuring the card's contrast rather than
the model's claim.

An Anchor of 1.0 puts the ring on the optical axis, which is a point and cannot
move; that is reported as SKIP, not as a pass.

### `--depth` — the solve against the same solve in double

Covers the half of the plugin with no closed form. The picture is a horizontal
ramp, so its luma at any point **is** its horizontal position — which makes it
both the depth field the shader reads and the readout of where the shader read
from. The C++ runs the same fixed-point iteration analytically and the two are
compared. Under 0.8 of a level over 45 combinations.

Looser tolerance than `--probe` on purpose: the shader's field comes from a
bilinear fetch of an 8-bit ramp and the C++ one is exact, so the two iterations
start from inputs that differ by up to half a level.

### A dead control is invisible to the compiler

```bash
python3 tools/sweep.py
```

A uniform name that does not match between the C++ and the GLSL is silently
ignored — `glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented
no-op — so a control can be stone dead while everything compiles, links, loads
and renders. Nothing else here catches that.

**The baseline must be off both nulls.** Relief at 0.5 is a scene with no depth
and Dolly at 0.5 is no move; either one alone makes every other control read
dead. Three controls also need a context of their own: Edges is invisible on a
pull back (nothing reaches the frame edge), Quality needs something being
minified hard, and Smooth does nothing in Radial because a formula has no noise
to smooth.

### The harness's own orientation trap

The test-picture builders return **bottom-up** buffers, ready for `glTexImage2D`.
`readBack()` returns **top-down**. Comparing one against the other index by index
silently compares row `y` with row `height-1-y` — which does not look like a bug,
because it produces a large, plausible and completely *constant* error that does
not change when the effect does.

### The OpenFX side

`ofxprobe` lives in the sibling `resolume-ofx-bridge` repo and is not wired into
`verify.sh`, because it may not be built:

```bash
../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.vertigo
```

Two things worth running there, both of which have found real bugs:

- **The nulls, at Quality = Fast**: `--set relief=0.5 --set quality=0` and
  `--set dolly=0.5 --set quality=0` must each report **0 bytes differ**. At any
  other quality the supersampling still resamples, so the picture changes even
  though the geometry does not.
- **Every preset, and its hand-set equivalent**: `--edit preset=N` (which
  delivers a real user edit, so the preset logic actually runs) rendered against
  the same values set by hand must be byte-identical — and no preset should come
  back matching the null.

## 6. What has never been checked

- **It has never been loaded into Resolume, or into Resolve.** Not once.
  Parameter groups, the dropdowns, the hosts' real texture sizes and their
  premultiplication behaviour are all unconfirmed, and those are exactly what the
  offline harness cannot tell you about, because it supplies its own textures.
  `cmake --install` puts the bundle where Arena looks.
- **The Windows build has never been run**, or compiled — only the workflow that
  would do it exists. A `workflow_dispatch` run of `release.yml` builds both
  platforms and publishes nothing, which is the cheap way to find out.
- **Nothing has been timed at all.** Best quality is 16 samples per pixel, and in
  the Luma and Alpha modes each of those runs the depth solve three times, each
  of which is a 5-tap when Smooth is up. Nobody has measured what that costs at
  4K.
- **The depth-map modes have only ever been fed synthetic fields** — a radial
  card and a linear ramp. No rendered depth pass and no generated depth map has
  been through this.
- Everything here comes from one M4 Max, never from CI — hosted macOS runners
  have no GPU, so `vgtest` cannot run there. The CI job compiles and checks the
  registration, and deliberately claims nothing else.

## 7. Conventions

- Public repo. "Commit" means commit **and** push.
- Standard AI disclaimer at the top of the README — see the fleet's disclaimer
  scope. The last sentence of it is a factual claim about what has been verified,
  and it rots like any other; update it when the status section changes.
- `source/StoatworksAbout.h` is normally generated by the backend's
  `sync-about.py` from the website's `projects.json`. This repo's copy is
  hand-written in the generated shape because there is no website entry yet, and
  `guide` and `page` are empty strings on purpose — the About block leaves a
  missing link out rather than showing a button that opens a 404. The next sync
  should overwrite it once those pages exist.
- `ATTRIBUTIONS.md` is likewise generated from the backend; edit it there.

## Diagnostics

`source/Diag.{h,cpp}` is a small member of the fleet's `diag` family: a log file
only. No crash handler (a plugin has no business installing a process-wide signal
handler inside Resolume) and no bundle command (there is no UI to hang one off).

It covers the failure that actually happens — `InitGL` returning `FF_FAIL`
because the shader would not compile, which from the operator's side looks like
"the effect does nothing" with no message anywhere. The GL vendor/renderer/
version strings sit next to it because with one shader stage the driver is nearly
always the rest of the answer.

```
~/Library/Logs/vertigo/vertigo.<date>.log       # macOS
%LOCALAPPDATA%\vertigo\logs\                    # Windows
```

`VERTIGO_LOG_DIR` overrides the location.
