#pragma once

/**
    Factory presets: named shots an operator can reach in one gesture.

    Both hosts already let a user save their own presets, so what belongs in
    the plugin is the curated set -- each entry here is a shot somebody has
    actually seen, not a random collection of slider positions.

    The values live in the same 0..1 parameter space both builds expose (the
    FFGL and OFX builds deliberately share it), so ONE table drives both and a
    preset looks identical in Resolume and Resolve. Kept as a header of plain
    data: no logic here, the application machinery lives with each host's glue.

    Element 0 of the host-facing dropdown is "Custom" and is not in this
    table: it is not a preset, it means "the sliders are the truth".

    Three things a preset deliberately does not touch:

    - **Depth.** Whether a clip carries a usable depth map in its luma or its
      alpha is a fact about the footage, not a look. A preset that switched a
      plain clip to Luma would turn its brightness into geometry and produce
      something unrecognisable; every preset here is set up to read correctly
      whichever field is selected.
    - **Centre X/Y**, which is framing -- where the subject happens to be.
    - **Overscan and Quality**, which are the operator's calls about how much
      frame to keep and how much GPU to spend.
*/

namespace vertigo
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kDolly,
	kRelief,
	kAnchor,
	kFalloff,
	kSmooth,
	kEdges,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Values are host-parameter values, all 0..1: Dolly 0.5 is no shot and is
// geometric in the parallax ratio either side; Relief 0.5 is flat, above is a
// subject in front of a world and below turns that inside out; Anchor is the
// disparity held fixed, 1 being the nearest surface; Falloff 0.5 is gamma 1;
// option params hold the element index. See Dolly.cpp for the mappings.
//
// Note which side of centre the Dolly values sit on. A push IN shrinks the
// background, which means reading from beyond the frame edge and living with
// whatever Edges says; a pull BACK magnifies it and reads from inside the
// picture, so it cannot show an edge artefact at all. Both are real shots, but
// the ones that need no explanation are on the pull-back side, and that is
// where the default and most of this table sit.
inline constexpr Preset kPresets[] = {
	//                        Dolly Relief Anchor Fall  Smooth Edges
	{ "Vertigo",           { 0.30f, 0.75f, 1.00f, 0.50f, 0.25f, 2.0f } },//the shot: subject locked, world closing in
	{ "Vertigo (Push In)", { 0.70f, 0.75f, 1.00f, 0.50f, 0.25f, 2.0f } },//the same shot the other way, world falling away
	{ "Stairwell",         { 0.66f, 0.25f, 1.00f, 0.55f, 0.25f, 2.0f } },//relief inverted: the middle is the far end
	{ "Creeping Unease",   { 0.42f, 0.62f, 1.00f, 0.50f, 0.25f, 2.0f } },//slow enough to be felt rather than seen
	{ "Slam",              { 0.05f, 0.90f, 1.00f, 0.40f, 0.25f, 2.0f } },//as hard as the range goes
	{ "Mid Anchor",        { 0.30f, 0.75f, 0.50f, 0.50f, 0.25f, 2.0f } },//locks a ring, not the axis: middle ground holds
	{ "Deep Field",        { 0.35f, 0.75f, 1.00f, 0.18f, 0.35f, 2.0f } },//gamma pushes the depth out: most of the frame is far
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace vertigo
