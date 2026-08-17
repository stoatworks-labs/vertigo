#pragma once

/**
    The dolly zoom, in C++.

    A dolly zoom is one camera move and one lens move, arranged to cancel. The
    camera tracks along its own axis; the focal length changes to hold one
    surface at exactly the size it already was. That surface does not move, and
    everything at any other distance does -- which is the whole effect, and the
    reason it reads as the ground giving way rather than as a zoom.

    Written out: a point at depth z projects to image radius r = f*X/z. Dolly by
    t and change the focal length to f' and the same point lands at
    f'*X/(z - t). Requiring the anchor surface at z_a to be unmoved fixes
    f' = f*(z_a - t)/z_a, and the magnification of everything else follows:

        m(z) = [ z (z_a - t) ] / [ z_a (z - t) ]

    That form has a pole -- the camera reaches the surface at z = t -- and it is
    written in depth, which is not what a depth map holds. Both problems go away
    at once in **disparity**, d = z_a/z, where d = 1 is the anchor, d = 0 is
    infinitely far, and d > 1 is nearer than the anchor:

        m(d) = ( 1 - sigma*d_a ) / ( 1 - sigma*d )        sigma = t/z_a

    The pole is now at d = 1/sigma, and this plugin normalises the disparity
    field into 0..1 by construction, so a sigma below 1 can never reach it. No
    clamp, no guard, no special case: the geometry is unconditionally defined
    for every setting the host can produce, which is why nothing in here has to
    test for one.

    The remaining quantity worth naming is what an operator is actually
    reaching for. The ratio between how much the nearest surface grows and how
    much infinity does is

        P = m(1) / m(0) = 1 / ( 1 - sigma )

    -- one number for the strength of the whole shot, so **that** is what the
    Dolly control is, geometrically, in stops. P = 1 is no shot at all.

    Three properties follow from this rather than being arranged, and breaking
    any of them means the model is wrong rather than the look being off:

    - **The anchor does not move.** m(d_a) = 1 identically, for every dolly,
      every relief, every depth field. `vgtest --anchor` measures it.
    - **A flat scene is the identity.** With no depth variation every pixel is
      the anchor, so the picture is untouched however hard the dolly is driven.
      A dolly zoom on a painted backdrop is nothing, and the maths says so
      without being told.
    - **The two directions are reciprocal.** Equal travel either side of the
      centre of the Dolly control gives parallax ratios P and 1/P.

    This file is the canonical statement of it. `Shaders.cpp` carries a GLSL
    mirror, because it has to run per pixel on the GPU, and `vgtest --probe`
    measures the GPU against these functions to catch the two copies drifting
    apart. Change one, change the other, then run the probe.
*/
namespace vertigo
{

/// Where the disparity field comes from.
///
/// Radial is a field this plugin invents; the other two read one out of the
/// clip. They differ in more than taste -- see `solveSourcePoint()` in
/// Shaders.cpp: the invented field is a closed-form function of the *output*
/// point and resolves in one step, and a field carried by the picture has to be
/// solved for, because which depth applies is not known until it is known where
/// the sample came from.
enum class DepthSource
{
	Radial = 0,//!< Invented: near in the middle, far at the corners (or the reverse).
	Luma   = 1,//!< The clip's own brightness read as a depth map.
	Alpha  = 2 //!< The clip's alpha read as a depth map.
};

/// Rec.709 luma weights, for reading a picture as a depth map.
inline constexpr double kLumaR = 0.2126;
inline constexpr double kLumaG = 0.7152;
inline constexpr double kLumaB = 0.0722;

/// How many times the depth-map solve iterates. See solveSourcePoint().
inline constexpr int kDepthIterations = 3;

/// Half the frame diagonal, in "height units" -- the space in which x has been
/// multiplied by the aspect ratio, so that the radial field is circular in
/// pixels rather than circular in UV. Radii are quoted as fractions of this
/// everywhere, which puts the frame corner at exactly 1.
///
/// Fixed, unlike the four-way Fit control a lens warp needs: what matters here
/// is only that the field reaches its far end somewhere sensible, and the
/// corner is the one choice that leaves no part of the frame outside the field.
double referenceRadius( double aspect );

/// The raw radial field, before gamma: 1 at the optical axis falling to 0 at
/// `rhoHat` = 1, which is the frame corner. Output space, not source space.
double radialBase( double rhoHat );

/// The raw field turned into the disparity that is actually used.
///
/// `base` is 0..1 from radialBase() or from the picture; `gamma` redistributes
/// it; `relief` scales it about the anchor level, so relief 0 collapses the
/// field to a constant and relief below 0 turns the scene inside out. The
/// clamp to 0..1 is what keeps m() free of poles, and it means physically that
/// nothing is nearer than the near plane and nothing is beyond infinity.
double disparity( double base, double gamma, double relief, double anchorLevel );

/// The magnification a surface at disparity `delta` gets. 1 at the anchor.
double magnification( double delta, double sigma, double anchorLevel );

/// The radius, in units of the reference radius, that the Radial field puts at
/// disparity `anchorLevel` -- i.e. the ring that will not move, whatever the
/// dolly does. Independent of relief, which scales the field *about* the
/// anchor and so cannot shift it. This is what `vgtest --anchor` aims at.
double radialAnchorRadius( double anchorLevel, double gamma );

//---------------------------------------------------------------------------
// Parameter mapping.
//
// Every parameter this plugin exposes to the host is a plain 0..1 float, and
// these turn them into the physical quantities above. That is not laziness:
// CFFGLPluginManager::SetParamInfo clamps an FF_TYPE_STANDARD default into
// 0..1 *before* SetParamRange can widen the range (SDK b1afaf9), so a
// parameter declared in stops cannot state a default in stops. Keeping the
// host side unitless sidesteps it, and the conversion lives here where the
// harness can print both.
//---------------------------------------------------------------------------

/// Dolly slider -> sigma. 0.5 is no shot at all. Geometric in the parallax
/// ratio P, so 0.25 and 0.75 are exact reciprocals of each other: one pushes
/// in as hard as the other pulls back.
double sigmaFromParam( float value );

/// Dolly slider -> the parallax ratio P itself, which is the number the shot is
/// really described by and the one the harness prints.
double parallaxFromParam( float value );

/// Relief slider -> the signed depth gain. 0.5 is flat, and flat is the
/// identity whatever the dolly is doing.
double reliefFromParam( float value );

/// Falloff slider -> the gamma on the raw field. 0.5 is 1.0, geometric either
/// side, so the control is symmetric in the way a gamma actually behaves.
double gammaFromParam( float value );

/// Overscan slider -> a linear scale on the source read, 0.5 being 1:1.
/// Geometric like the dolly. Its job is real here rather than cosmetic: a push
/// in shrinks the background, which means reading from beyond the frame, and
/// this is what buys the picture back.
double overscanFromParam( float value );

/// Smooth slider -> the radius, in picture-space units, that the depth map is
/// blurred over before it is used. Zero for the Radial field, which is smooth
/// by construction.
double smoothFromParam( float value );

/// Quality option -> the grid order n, giving n*n samples per output pixel.
int tapsFromParam( float value );

/// Depth option -> which field.
DepthSource depthSourceFromParam( float value );

/// True for the two sources that read the field out of the picture, and so
/// need the iterative solve rather than a closed form.
bool depthSourceIsSampled( DepthSource source );

} // namespace vertigo
