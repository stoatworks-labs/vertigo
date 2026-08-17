#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   --probe        the GLSL copy of the dolly maths against the C++ copy, over
#                  the parameter space. Catches a typo in either.
#   --anchor       the model's own defining claim: one surface does not move.
#                  Catches a formula that is self-consistently wrong, which the
#                  probe by construction cannot.
#   --depth        the fixed-point solve the sampled-depth path runs on the GPU,
#                  against the same solve in double here. Covers the half of the
#                  plugin that has no closed form.
#   sweep.py       that no control is silently dead.
#   registration   that the bundle contains a plugin at all.
#   OFX plist      that the OpenFX bundle will codesign -- the release step,
#                  run here where it is cheap instead of after the tag.
#   lipo           that the macOS build really is universal.
#
# The probe and the anchor test both decline to answer where the picture cannot
# support the question -- a source radius that falls outside the frame, or an
# anchor sitting on the optical axis, which is a point and cannot move. Those
# are reported as SKIP, not as passes.
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() {
	printf '\n\033[1m== %s\033[0m\n' "$1"
}

if [ ! -x "$BUILD/vgtest" ]; then
	echo "$BUILD/vgtest not found. Run:"
	echo "  cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

# ---------------------------------------------------------------------------
# The GLSL against the C++.
# ---------------------------------------------------------------------------
step "probe: the GPU against Dolly.cpp, over dolly x relief x anchor x falloff"
probe_pass=0; probe_fail=0
for dolly in 0.10 0.30 0.50 0.70 0.90; do
	for relief in 0.25 0.60 0.75 1.00; do
		for anchor in 0.25 0.60 1.00; do
			for falloff in 0.30 0.50 0.70; do
				if "$BUILD/vgtest" --probe \
					--set "Dolly=$dolly" --set "Relief=$relief" \
					--set "Anchor=$anchor" --set "Falloff=$falloff" >/dev/null 2>&1; then
					probe_pass=$((probe_pass + 1))
				else
					probe_fail=$((probe_fail + 1))
					printf '\033[31mFAILED: probe dolly=%s relief=%s anchor=%s falloff=%s\033[0m\n' \
						"$dolly" "$relief" "$anchor" "$falloff"
					failures=$((failures + 1))
				fi
			done
		done
	done
done
printf '   %d passed, %d failed\n' "$probe_pass" "$probe_fail"

# ---------------------------------------------------------------------------
# The claim the whole effect is named after.
#
# Anchor is deliberately never 1.0 here: at the near end the anchor IS the
# optical axis, a single point, and a point cannot move. vgtest reports that as
# SKIP (exit 2) rather than as a pass, and a sweep made only of skips would look
# like a clean run.
# ---------------------------------------------------------------------------
step "anchor: one surface does not move, while the rest of the frame does"
anchor_pass=0; anchor_fail=0; anchor_skip=0
for anchor in 0.20 0.40 0.60 0.80; do
	# 0.25 and 0.35 are the INVERTED half of the relief control, and they are in
	# this list because that is where the model was wrong once. Scaling the
	# field about the anchor rather than about the middle of the range sends the
	# whole field off the end as soon as relief goes negative and the anchor is
	# not centred -- the field clamps flat, and a flat field is the identity, so
	# the effect silently stops. The probe could not see it: the GPU and the C++
	# agreed perfectly, on the same wrong answer.
	for relief in 0.25 0.35 0.65 0.75 0.90; do
		for falloff in 0.40 0.50 0.60; do
			"$BUILD/vgtest" --anchor \
				--set "Anchor=$anchor" --set "Relief=$relief" --set "Falloff=$falloff" >/dev/null 2>&1
			case $? in
				0) anchor_pass=$((anchor_pass + 1)) ;;
				2) anchor_skip=$((anchor_skip + 1)) ;;
				*)
					anchor_fail=$((anchor_fail + 1))
					printf '\033[31mFAILED: anchor anchor=%s relief=%s falloff=%s\033[0m\n' \
						"$anchor" "$relief" "$falloff"
					failures=$((failures + 1))
					;;
			esac
		done
	done
done
printf '   %d passed, %d failed, %d not measurable at those settings\n' \
	"$anchor_pass" "$anchor_fail" "$anchor_skip"
if [ "$anchor_pass" -eq 0 ]; then
	printf '\033[31mFAILED: the anchor test never actually ran\033[0m\n'
	failures=$((failures + 1))
fi

# ---------------------------------------------------------------------------
# The sampled-depth path, which has no closed form to check against.
# ---------------------------------------------------------------------------
step "depth: the GPU's fixed-point solve against the same solve in double"
depth_pass=0; depth_fail=0
for dolly in 0.15 0.30 0.45 0.60 0.75; do
	for relief in 0.60 0.75 0.90; do
		for anchor in 0.40 0.70 1.00; do
			if "$BUILD/vgtest" --depth \
				--set "Dolly=$dolly" --set "Relief=$relief" --set "Anchor=$anchor" >/dev/null 2>&1; then
				depth_pass=$((depth_pass + 1))
			else
				depth_fail=$((depth_fail + 1))
				printf '\033[31mFAILED: depth dolly=%s relief=%s anchor=%s\033[0m\n' \
					"$dolly" "$relief" "$anchor"
				failures=$((failures + 1))
			fi
		done
	done
done
printf '   %d passed, %d failed\n' "$depth_pass" "$depth_fail"

# ---------------------------------------------------------------------------
# A dead control is invisible to the compiler.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# The demo's copy of the shader.
#
# demo/plugin.js cannot include a C++ file, so the browser demo carries a second
# copy of the GLSL and nothing about the build says a word when they diverge.
# That matters more for this plugin than for most: the demo carries two presets
# that invite a visitor to check the claim that one surface does not move, and a
# demo running a stale shader is evidence about a shader that no longer exists.
# ---------------------------------------------------------------------------
if [ -f demo/tools/check_shaders.py ]; then
	step "the demo runs the plugin's shader"
	if python3 demo/tools/check_shaders.py; then
		:
	else
		printf '\033[31mFAILED: demo/plugin.js has drifted from source/Shaders.cpp\033[0m\n'
		failures=$((failures + 1))
	fi
fi

step "sweep: no control silently dead"
if python3 tools/sweep.py > "${TMPDIR:-/tmp}/vertigo-sweep.txt" 2>&1; then
	echo "   all parameters affect the output"
else
	printf '\033[31mFAILED: dead controls, see %svertigo-sweep.txt\033[0m\n' "${TMPDIR:-/tmp}/"
	tail -4 "${TMPDIR:-/tmp}/vertigo-sweep.txt"
	failures=$((failures + 1))
fi

# ---------------------------------------------------------------------------
# Registration.
#
# The failure this catches is specific and silent: CFFGLPluginInfo registers
# itself from a file-scope constructor and nothing references it by name, so a
# linker that drops the translation unit gives a bundle which loads, exports
# plugMain, and reports that it contains no plugins. Resolume shows an empty
# effects list and no error.
#
# Read once into variables rather than piping into `grep -q`. `grep -q` exits
# the instant it matches, which closes the pipe under the still-running nm or
# strings; they take SIGPIPE and exit 141, and with `set -o pipefail` the
# pipeline is then a failure however well the grep went.
# ---------------------------------------------------------------------------
step "the bundle contains its plugin"
binary="$BUILD/Vertigo.bundle/Contents/MacOS/Vertigo"
if [ ! -f "$binary" ]; then
	printf '\033[31mFAILED: %s not built\033[0m\n' "$binary"
	failures=$((failures + 1))
else
	symbols=$(nm -gU "$binary" 2>/dev/null)
	literals=$(strings "$binary" 2>/dev/null)

	if ! grep -q plugMain <<<"$symbols"; then
		printf '\033[31mFAILED: Vertigo exports no plugMain\033[0m\n'
		failures=$((failures + 1))
	elif ! grep -qx "VG01" <<<"$literals"; then
		# The plugin id is a four-character literal in the registration, so it
		# is in the binary's strings if and only if that translation unit
		# survived the link.
		printf '\033[31mFAILED: Vertigo does not carry its own id VG01\033[0m\n'
		failures=$((failures + 1))
	else
		printf 'ok   Vertigo exports plugMain and carries VG01\n'
	fi
fi

# ---------------------------------------------------------------------------
# The OpenFX bundle's plist.
#
# This check exists because it went wrong elsewhere in the fleet.
# cmake/InfoOFX.plist.in is one of the files copied from repo to repo when a
# new plugin starts, and the version it was copied from had the PREVIOUS
# plugin's name hardcoded into CFBundleExecutable. NOTHING caught it: the
# bundle assembles, the binary is universal, the OFX entry point exports, and a
# probe host loads it and renders a correct frame. It fails only at release
# time, in codesign, with a message that names a "subcomponent" and never
# mentions the plist.
#
# So the check is the release step itself, run here where it is cheap. On a
# copy of the bundle, so a verify run never leaves a signature on the build
# tree that the release job did not put there.
# ---------------------------------------------------------------------------
if [ -d "$BUILD/Vertigo.ofx.bundle" ]; then
	step "the OpenFX bundle signs"

	plist="$BUILD/Vertigo.ofx.bundle/Contents/Info.plist"
	named=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" 2>/dev/null)
	if [ ! -f "$BUILD/Vertigo.ofx.bundle/Contents/MacOS/$named" ]; then
		printf '\033[31mFAILED: Info.plist names "%s", which is not in Contents/MacOS\033[0m\n' "$named"
		failures=$((failures + 1))
	else
		scratch="${TMPDIR:-/tmp}/vertigo-signcheck.ofx.bundle"
		rm -rf "$scratch"
		cp -R "$BUILD/Vertigo.ofx.bundle" "$scratch"
		if codesign --force --sign - --timestamp=none "$scratch" >/dev/null 2>&1; then
			printf 'ok   CFBundleExecutable is %s, and the bundle ad-hoc signs\n' "$named"
		else
			printf '\033[31mFAILED: the OpenFX bundle will not codesign\033[0m\n'
			codesign --force --sign - --timestamp=none "$scratch" 2>&1 | sed 's/^/       /'
			failures=$((failures + 1))
		fi
		rm -rf "$scratch"
	fi

	ofxSymbols=$(nm -gU "$BUILD/Vertigo.ofx.bundle/Contents/MacOS/Vertigo.ofx" 2>/dev/null)
	if ! grep -q OfxGetPlugin <<<"$ofxSymbols"; then
		printf '\033[31mFAILED: the OpenFX bundle exports no OfxGetPlugin\033[0m\n'
		failures=$((failures + 1))
	else
		printf 'ok   the OpenFX bundle exports OfxGetPlugin\n'
	fi
fi

# ---------------------------------------------------------------------------
# Universal.
#
# CMake latches CMAKE_OSX_ARCHITECTURES when the first target is created, so
# setting it late is silently ignored and the build log still says success. The
# only honest answer comes from lipo. Skipped when the developer asked for a
# single-architecture build on purpose.
# ---------------------------------------------------------------------------
step "the macOS build is universal"
if grep -q "CMAKE_OSX_ARCHITECTURES:.*arm64;x86_64" "$BUILD/CMakeCache.txt" 2>/dev/null; then
	for candidate in "$BUILD/Vertigo.bundle/Contents/MacOS/Vertigo" \
	                 "$BUILD/Vertigo.ofx.bundle/Contents/MacOS/Vertigo.ofx"; do
		[ -f "$candidate" ] || continue
		arches=$(lipo -archs "$candidate" 2>/dev/null)
		case "$arches" in
			*arm64*x86_64* | *x86_64*arm64*)
				printf 'ok   %s: %s\n' "$(basename "$candidate")" "$arches" ;;
			*)
				printf '\033[31mFAILED: %s is %s, not universal\033[0m\n' \
					"$(basename "$candidate")" "${arches:-missing}"
				failures=$((failures + 1)) ;;
		esac
	done
else
	echo "skipped: this build was configured for one architecture"
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit "$failures"
