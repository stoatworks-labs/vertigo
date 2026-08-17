"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where the
effect is actually doing something, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

Three things about this plugin in particular that will fool you:

  * **The baseline must not be flat, and must not be still.** Relief at 0.5 is
    a scene with no depth, which is the identity however hard the dolly is
    driven -- and Dolly at 0.5 is no move, which is the identity however much
    depth there is. Either one alone makes every other control read dead. The
    baseline below is off both.

  * **Edges is invisible on a pull back.** Pulling back magnifies the
    background, which means reading from INSIDE the picture, so nothing ever
    reaches the frame edge and all five edge modes agree. It needs a push in to
    have anything to decide, which is what its CONTEXT entry is for.

  * **Smooth does nothing in Radial mode, correctly.** That field is a formula,
    and a formula has no noise to smooth. It only exists for a field read out of
    the picture, so its CONTEXT switches Depth to Luma.
"""
import subprocess, zlib, struct, sys, tempfile

SC = tempfile.mkdtemp(prefix="vgsweep")

# A baseline where the effect is genuinely moving something, so that nothing
# reads dead merely because the thing it modifies is switched off.
BASE = {
    "Dolly": 0.30,       # a pull back -- NOT 0.5, which is no move at all
    "Relief": 0.75,      # +1 -- NOT 0.5, which is a scene with no depth
    "Anchor": 0.60,      # a ring rather than the axis, so Falloff has somewhere to bite
    "Depth": 0,          # Radial
    "Falloff": 0.5,
    "Smooth": 0.25,
    "Centre X": 0.5,
    "Centre Y": 0.5,
    "Overscan": 0.5,
    "Edges": 2,          # clamp, so edge pixels carry picture rather than nothing
    "Quality": 1,
}

# Options are discrete; sweep them across their real element range. Everything
# else is a plain 0..1 float.
DISCRETE = {"Depth": (0, 2), "Edges": (0, 4), "Quality": (0, 2), "Preset": (0, 7)}

# A few controls need a baseline of their own to be visible at all.
CONTEXT = {
    # Reading outside the frame is the only thing an edge mode decides, and
    # only a push in ever reads outside the frame.
    "Edges": {"Dolly": 0.90, "Overscan": 0.5},
    # Supersampling only shows where the picture is being minified hard, which
    # again is the push-in direction, on the card's fine checker band.
    "Quality": {"Dolly": 0.92},
    # Smoothing a formula is a no-op. Give it a field read out of the picture.
    "Smooth": {"Depth": 1, "Dolly": 0.90},
}


def render(path, overrides):
    args = ["./build/vgtest", "--out", path, "--width", "1280", "--height", "720"]
    merged = dict(BASE)
    merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr)
        sys.exit(1)
    return open(path, "rb").read()


def pixels(png):
    i = 8
    idat = b""
    w = h = 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i + 4])[0]
        t = png[i + 4:i + 8]
        d = png[i + 8:i + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT":
            idat += d
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa)
    changed = 0
    total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / (n / 4) * 100, total / (n / 4)


names = subprocess.run(["./build/vgtest", "--list"], capture_output=True, text=True).stdout
params = [" ".join(l.split()[1:-1]) for l in names.strip().splitlines()]

# The About block is a text field and browser buttons, declared last. They
# never touch a pixel, so sweeping them only buries a real dead control.
if "About" in params:
    params = params[:params.index("About")]

print(f"{'parameter':<16} {'pixels changed':>15} {'mean delta':>11}   verdict")
dead = []
for p in params:
    lo, hi = DISCRETE.get(p, (0.0, 1.0))
    context = CONTEXT.get(p, {})
    a = render(f"{SC}/a.png", {**context, p: lo})
    b = render(f"{SC}/b.png", {**context, p: hi})
    pct, mean = diff(a, b)
    ok = pct > 0.5
    if not ok:
        dead.append(p)
    print(f"{p:<16} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

print()
if dead:
    print("DEAD CONTROLS:", ", ".join(dead))
    sys.exit(1)
print(f"all {len(params)} parameters affect the output")
