/**
 * Vertigo — browser demo.
 *
 * `VERTEX` and `FRAGMENT` are `kVertexShader` and `kFragmentShader` from
 * `source/Shaders.cpp`, copied across unedited. `demo/tools/check_shaders.py`
 * compares them character for character and is run by `tools/verify.sh`,
 * because nothing else can: plugin.js cannot include a C++ file.
 *
 * The conversions below are ports of `source/Dolly.cpp` — the CPU half of the
 * model, which is what turns the host's 0..1 into the uniforms the shader
 * wants. They are ported rather than re-derived for the same reason the plugin
 * keeps them in one file: the model already exists twice on purpose (C++ for
 * readability, GLSL because it runs per pixel), and `vgtest --probe` is what
 * keeps those two honest. A third, invented copy here would have nothing
 * checking it at all.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through. The usual FFGL vertex shader folds MaxUV in here, but
	//this effect has to do its geometry in picture space and scale only at the
	//fetch.
	uv = vUV;
}
`;

const FRAGMENT = `#version 410 core

uniform sampler2D InputTexture;

uniform vec2 MaxUV;        //the part of the input texture that is really picture
uniform vec2 HalfTexel;    //half an input texel, in picture space
uniform vec2 Centre;       //offset of the optical axis from the middle of frame
uniform float Aspect;      //picture width / height
uniform float Sigma;       //the dolly, t/z_anchor; always below 1
uniform float AnchorDelta; //where the anchor level landed after Relief
uniform float Relief;      //signed gain on the depth field; 0 is flat
uniform float Gamma;       //redistributes the raw field
uniform float DepthMode;   //0 radial, 1 luma, 2 alpha
uniform float DepthSmooth; //blur radius for a sampled field, picture-space
uniform float Overscan;
uniform float EdgeMode;    //0 transparent, 1 black, 2 clamp, 3 mirror, 4 wrap
uniform float Taps;        //grid order n, so n*n samples per output pixel

in vec2 uv;
out vec4 fragColor;

//The solve below is a fixed number of steps rather than a convergence test:
//uniform control flow is cheaper on every GPU this runs on, and three steps is
//past the point where a fourth moves a sample by a visible amount at any
//setting this plugin can reach. Mirror of kDepthIterations in Dolly.h.
const int kDepthIterations = 3;

const vec3 kLumaWeights = vec3( 0.2126, 0.7152, 0.0722 );

//---------------------------------------------------------------------------
// The dolly zoom. Mirror of Dolly.cpp -- change both, then run
// \`vgtest --probe\`, which exists to catch exactly this pair drifting apart.
//---------------------------------------------------------------------------
float referenceRadius()
{
	//Half the frame diagonal, measured with x scaled by the aspect ratio, so
	//the radial field is circular in pixels rather than circular in UV and the
	//frame corner lands at exactly 1.
	return 0.5 * sqrt( Aspect * Aspect + 1.0 );
}

float rhoHat( vec2 p )
{
	vec2 c = p - ( vec2( 0.5 ) + Centre );
	c.x *= Aspect;
	return length( c ) / referenceRadius();
}

float radialBase( float rho )
{
	return 1.0 - clamp( rho, 0.0, 1.0 );
}

/// Scaled about the middle of the range, not about the anchor -- the anchor is
/// held by putting it through this same map (see AnchorDelta, set on the C++
/// side by anchorDisparity()). Scaling about the anchor instead would slide the
/// whole field off the end of the range whenever Relief went negative and the
/// anchor was not at 0.5, which reads as the control doing nothing.
float reliefScaled( float level )
{
	//The clamp is what keeps magnification() free of poles.
	return clamp( 0.5 + Relief * ( clamp( level, 0.0, 1.0 ) - 0.5 ), 0.0, 1.0 );
}

float disparity( float base )
{
	return reliefScaled( pow( clamp( base, 0.0, 1.0 ), Gamma ) );
}

float magnification( float delta )
{
	//Sigma < 1 by construction and delta is in 0..1, so the denominator is
	//unconditionally positive. There is nothing to guard here and nothing that
	//can produce a NaN, which is why this effect has no equivalent of the
	//"no source for this pixel" case a lens warp needs.
	return ( 1.0 - Sigma * AnchorDelta ) / ( 1.0 - Sigma * delta );
}

//---------------------------------------------------------------------------
// Fetching.
//---------------------------------------------------------------------------
float mirrorCoord( float x )
{
	//GLSL mod() is x - y*floor(x/y), so this is already correct for negatives.
	float m = mod( x, 2.0 );
	return ( m > 1.0 ) ? ( 2.0 - m ) : m;
}

/// Never sample nearer than half a texel to the picture edge. The input texture
/// may be bigger than the picture -- MaxUV is the fraction of it that was
/// actually drawn -- so a linear fetch right at the edge takes half its weight
/// from padding that contains nothing.
vec4 fetchInside( vec2 p )
{
	p = clamp( p, HalfTexel, vec2( 1.0 ) - HalfTexel );
	return texture( InputTexture, p * MaxUV );
}

vec4 fetch( vec2 p )
{
	bool outside = any( lessThan( p, vec2( 0.0 ) ) ) || any( greaterThan( p, vec2( 1.0 ) ) );

	if( EdgeMode < 0.5 )
	{
		if( outside )
			return vec4( 0.0 );//transparent, and already premultiplied
	}
	else if( EdgeMode < 1.5 )
	{
		if( outside )
			return vec4( 0.0, 0.0, 0.0, 1.0 );//opaque black
	}
	else if( EdgeMode < 2.5 )
	{
		p = clamp( p, vec2( 0.0 ), vec2( 1.0 ) );
	}
	else if( EdgeMode < 3.5 )
	{
		p = vec2( mirrorCoord( p.x ), mirrorCoord( p.y ) );
	}
	else
	{
		p = fract( p );
	}

	return fetchInside( p );
}

//---------------------------------------------------------------------------
// The depth field.
//---------------------------------------------------------------------------
float rawDepth( vec2 p )
{
	//Deliberately NOT routed through the edge mode. Off the frame the depth
	//extends from the edge whatever the picture is doing there, because the
	//alternative -- Transparent, say -- would read as "infinitely far" and put
	//a ring of runaway magnification just outside every frame.
	vec4 texel = fetchInside( p );

	if( DepthMode < 1.5 )
	{
		//A premultiplied clip reads its transparent regions as far away, which
		//is the honest answer: there is nothing there to be near.
		return dot( texel.rgb, kLumaWeights );
	}
	return texel.a;
}

float depthAt( vec2 p )
{
	if( DepthSmooth <= 0.0 )
		return rawDepth( p );

	//A cheap 5-tap, and crude on purpose: this is softening a control field,
	//not the picture. What it is for is the one artefact an inverse-mapped
	//parallax has. Nothing can leave a hole here -- every output pixel fetches
	//something -- so a depth step does not tear, it SMEARS along the step, and
	//turning the step into a gradient is the whole repair.
	vec2 r = vec2( DepthSmooth );
	float sum = 2.0 * rawDepth( p );
	sum += rawDepth( p + vec2( r.x, r.y ) );
	sum += rawDepth( p + vec2( r.x, -r.y ) );
	sum += rawDepth( p + vec2( -r.x, r.y ) );
	sum += rawDepth( p + vec2( -r.x, -r.y ) );
	return sum / 6.0;
}

/// Where the pixel at output point \`p\` came from.
///
/// The two branches are the same geometry asked in two different places, and
/// the difference is not an implementation detail:
///
///   * The Radial field is invented, so it can be evaluated at the pixel being
///     WRITTEN. The magnification is then known before anything is fetched and
///     the answer is closed form.
///   * A field read out of the picture only exists at the pixel being READ,
///     and which pixel that is, is the thing being solved for. So: guess the
///     output point, look up the depth there, follow the magnification to a
///     better guess, repeat. It converges quickly because the displacement is
///     small and smooth nearly everywhere; where it is neither -- a hard depth
///     step under a hard dolly -- it lands a step late, and that is one of the
///     two things Smooth is for.
vec2 solveSourcePoint( vec2 p )
{
	vec2 axis   = vec2( 0.5 ) + Centre;
	vec2 offset = p - axis;

	if( DepthMode < 0.5 )
	{
		float m = magnification( disparity( radialBase( rhoHat( p ) ) ) );
		return axis + offset / ( m * Overscan );
	}

	vec2 src = p;
	for( int i = 0; i < kDepthIterations; ++i )
	{
		float m = magnification( disparity( depthAt( src ) ) );
		src     = axis + offset / ( m * Overscan );
	}
	return src;
}

void main()
{
	//The output pixel's size in picture-space units, from the rasteriser. Doing
	//it this way rather than from a resolution uniform means the supersample
	//grid is right whatever size the host is rendering at, and it has to happen
	//in uniform control flow, so it happens first.
	vec2 pixel = vec2( dFdx( uv.x ), dFdy( uv.y ) );

	int n     = int( Taps + 0.5 );
	float inv = 1.0 / float( n );

	vec4 sum = vec4( 0.0 );

	for( int j = 0; j < n; ++j )
	{
		for( int i = 0; i < n; ++i )
		{
			vec2 o = ( vec2( float( i ), float( j ) ) + 0.5 ) * inv - 0.5;

			//Rotate the grid off the pixel axes. An axis-aligned grid samples
			//every horizontal edge at the same few heights, which is where a
			//regular supersample still shows stair-stepping; 26.6 degrees is
			//the standard dodge.
			o = vec2( o.x * 0.8944272 - o.y * 0.4472136,
			          o.x * 0.4472136 + o.y * 0.8944272 );

			sum += fetch( solveSourcePoint( uv + o * pixel ) );
		}
	}

	vec4 result = sum / float( n * n );

	//Premultiplied in, premultiplied out. Averaging premultiplied samples is
	//the correct filter -- it is unpremultiplied averaging that goes wrong at a
	//transparent edge -- so there is nothing to undo here. Just hold the
	//invariant the engine expects.
	result.rgb = clamp( result.rgb, vec3( 0.0 ), vec3( result.a ) );

	fragColor = result;
}
`;

//---------------------------------------------------------------------------
// Ports of source/Dolly.cpp
//---------------------------------------------------------------------------

const clamp01 = (v) => Math.min(1, Math.max(0, v));

/// Two stops either side of unity, for every geometric control here.
const geometric = (v) => Math.pow(2, (v - 0.5) * 4);

/// The parallax ratio P: how much more the nearest surface grows than infinity
/// does. One number for the strength of the whole shot, and what the Dolly
/// control actually is.
const parallaxFromParam = geometric;

/// sigma = 1 - 1/P. Derived from the ratio rather than stated directly so the
/// slider is symmetric in the thing an operator sees; sigma's own asymmetry
/// (0.75 one way, -3 the other) never has to be looked at.
const sigmaFromParam = (v) => 1 - 1 / parallaxFromParam(v);

/// Signed, zero in the middle. Positive is a subject on the axis with the world
/// behind it; negative turns that inside out into the stairwell shot.
const reliefFromParam = (v) => (v - 0.5) * 4;

const gammaFromParam = geometric;
const overscanFromParam = geometric;

/// 4% of the picture at full travel. Blurs the DEPTH, never the picture.
const smoothFromParam = (v) => clamp01(v) * 0.04;

/// Where the anchor level lands once the same relief has been applied to it.
/// This is what makes the anchor hold — see the note in source/Dolly.h about
/// why relief scales about the middle of the range and not about the anchor.
const anchorDisparity = (level, relief) =>
  clamp01(0.5 + relief * (clamp01(level) - 0.5));

function tapsFromParam(v) {
  switch (Math.round(v)) {
    case 0: return 1; // Fast: one sample
    case 2: return 4; // Best: 16 samples
    default: return 2; // Good: 4 samples
  }
}

/// The radius the Radial field puts at a given anchor level — the ring that
/// will not move. Independent of relief, which scales the field *about* the
/// anchor and so cannot shift it.
const radialAnchorRadius = (level, gamma) =>
  clamp01(1 - Math.pow(clamp01(level), 1 / gamma));

//---------------------------------------------------------------------------
// The renderer: one pass, exactly as ProcessOpenGL does it.
//---------------------------------------------------------------------------

function createRenderer(gl, quad) {
  const shader = new Program(gl, VERTEX, FRAGMENT, 'vertigo');

  return {
    render({ input, params }) {
      shader.use();
      bindTexture(gl, 0, input.texture);
      shader.setSampler('InputTexture', 0);

      shader.set('MaxUV', 1, 1);
      shader.set('HalfTexel', 0.5 / input.width, 0.5 / input.height);
      shader.set('Aspect', input.width / input.height);

      const relief = reliefFromParam(params.get('relief'));
      shader.set('Sigma', sigmaFromParam(params.get('dolly')));
      shader.set('Relief', relief);
      // The anchor goes to the GPU already through the relief map, so the
      // shader never has to know the two are the same function.
      shader.set('AnchorDelta', anchorDisparity(params.get('anchor'), relief));

      shader.set('DepthMode', Math.round(params.get('depth')));
      shader.set('Gamma', gammaFromParam(params.get('falloff')));
      shader.set('DepthSmooth', smoothFromParam(params.get('smooth')));

      shader.set('Centre', params.get('centreX') - 0.5, params.get('centreY') - 0.5);
      shader.set('Overscan', overscanFromParam(params.get('overscan')));

      shader.set('EdgeMode', Math.round(params.get('edges')));
      shader.set('Taps', tapsFromParam(params.get('quality')));

      quad.draw();
    },
  };
}

//---------------------------------------------------------------------------

const signed = (v) => (v >= 0 ? '+' : '') + v.toFixed(2);

function dollyDisplay(v) {
  const p = parallaxFromParam(v);
  if (Math.abs(v - 0.5) < 1e-6) return 'no move';
  return p > 1
    ? `push in ${p.toFixed(2)}:1`
    : `pull back 1:${(1 / p).toFixed(2)}`;
}

function reliefDisplay(v) {
  const r = reliefFromParam(v);
  if (Math.abs(r) < 1e-6) return 'flat — the identity';
  return r > 0 ? `${signed(r)} subject` : `${signed(r)} inverted`;
}

mountDemo({
  name: 'Vertigo',
  pluginId: 'VG01',
  tagline:
    'The dolly zoom: the camera tracks along its own axis while the focal length changes to hold one surface exactly the size it already was. That surface does not move and everything at any other distance does, which is why it reads as the ground giving way rather than as a zoom.',
  repo: 'https://github.com/stoatworks-labs/vertigo',
  // The kit renders "Project page and downloads" unconditionally — unlike
  // `video`, which it guards — so leaving this out puts href="undefined" in the
  // header. There is no project page yet, and the releases are in fact where the
  // downloads are, so this points there until the website entry exists. The
  // kit's own missing-link handling is the better fix; see demo/README.md.
  page: 'https://github.com/stoatworks-labs/vertigo/releases',

  showBackdrop: true,

  params: [
    {
      id: 'dolly', name: 'Dolly', type: 'standard', default: 0.3, group: 'Shot',
      display: dollyDisplay,
      hint: 'How far the camera travels, as the ratio between how much the nearest surface grows and how much infinity does. Geometric, so equal travel either side of the middle gives exactly reciprocal shots. 0.5 is a true null.',
    },
    {
      id: 'relief', name: 'Relief', type: 'standard', default: 0.75, group: 'Shot',
      display: reliefDisplay,
      hint: 'How much depth the scene has. Flat is the other true null — a scene with no depth cannot be dolly-zoomed, however hard the dolly is driven. Below the middle turns the depth inside out: the centre becomes the far end.',
    },
    {
      id: 'anchor', name: 'Anchor', type: 'standard', default: 1.0, group: 'Shot',
      display: (v) => (v >= 0.999 ? 'the axis' : `ring at ${radialAnchorRadius(v, 1).toFixed(2)}`),
      hint: 'Which depth is held fixed: 1 is the nearest surface, 0 the furthest. In Radial that is the optical axis and the frame corners. The readout assumes Falloff at the middle.',
    },

    {
      id: 'depth', name: 'Depth', type: 'option', default: 0, group: 'Depth',
      elements: ['Radial', 'Luma', 'Alpha'],
      hint: 'Where the depth comes from. Radial invents a field and is a closed form with no failure cases; Luma and Alpha read one out of the clip and are solved for, three iterations per sample.',
    },
    {
      id: 'falloff', name: 'Falloff', type: 'standard', default: 0.5, group: 'Depth',
      display: (v) => `\u03b3 ${gammaFromParam(v).toFixed(2)}`,
      hint: 'Gamma on the depth field: where between the near and far ends most of the scene sits.',
    },
    {
      id: 'smooth', name: 'Smooth', type: 'standard', default: 0.25, group: 'Depth',
      display: (v) => `${(smoothFromParam(v) * 100).toFixed(1)}%`,
      hint: 'Blurs the depth field, not the picture. Nothing in Radial, which is already smooth. An inverse-mapped parallax cannot leave a hole — it smears along a depth step instead — and softening the step is the repair.',
    },

    {
      id: 'centreX', name: 'Centre X', type: 'standard', default: 0.5, group: 'Frame',
      display: (v) => signed(v - 0.5),
      hint: 'The optical axis: what the move is about.',
    },
    {
      id: 'centreY', name: 'Centre Y', type: 'standard', default: 0.5, group: 'Frame',
      display: (v) => signed(v - 0.5),
      hint: 'The optical axis: what the move is about.',
    },
    {
      id: 'overscan', name: 'Overscan', type: 'standard', default: 0.5, group: 'Frame',
      display: (v) => `${overscanFromParam(v).toFixed(2)}\u00d7`,
      hint: 'Scales the picture to buy back the frame a push in gives away.',
    },

    {
      id: 'edges', name: 'Edges', type: 'option', default: 2, group: 'Output',
      elements: ['Transparent', 'Black', 'Clamp', 'Mirror', 'Wrap'],
      hint: 'What to show where a push in looks past the picture. A pull back never does, so on this side of the Dolly control all five agree.',
    },
    {
      id: 'quality', name: 'Quality', type: 'option', default: 1, group: 'Output',
      elements: ['Fast', 'Good', 'Best'],
      display: (v) => `${tapsFromParam(v) ** 2} taps`,
      hint: 'Samples per output pixel on a rotated grid. It does not change the geometry, only how it is sampled.',
    },
  ],

  sources: ['grid', 'scene', 'detail', 'spot', 'alpha', 'bars'],

  // The factory presets, from source/Presets.h, plus two that exist to make a
  // claim checkable rather than to look like anything.
  presets: {
    'Vertigo': { dolly: 0.3, relief: 0.75, anchor: 1.0, falloff: 0.5, smooth: 0.25, edges: 2 },
    'Vertigo (push in)': { dolly: 0.7, relief: 0.75, anchor: 1.0, falloff: 0.5, smooth: 0.25, edges: 2 },
    'Stairwell': { dolly: 0.66, relief: 0.25, anchor: 1.0, falloff: 0.55, smooth: 0.25, edges: 2 },
    'Creeping unease': { dolly: 0.42, relief: 0.62, anchor: 1.0, falloff: 0.5, smooth: 0.25, edges: 2 },
    'Slam': { dolly: 0.05, relief: 0.9, anchor: 1.0, falloff: 0.4, smooth: 0.25, edges: 2 },
    'Mid anchor — a ring holds, not the axis': { dolly: 0.3, relief: 0.75, anchor: 0.5, falloff: 0.5, smooth: 0.25, edges: 2 },
    'Deep field': { dolly: 0.35, relief: 0.75, anchor: 1.0, falloff: 0.82, smooth: 0.35, edges: 2 },
    'Null: no depth (drive the dolly, nothing happens)': { dolly: 0.95, relief: 0.5, quality: 0 },
    'Null: no move': { dolly: 0.5, relief: 1.0, quality: 0 },
    'Depth from the clip (Luma)': { depth: 1, dolly: 0.72, relief: 0.8, anchor: 0.9, smooth: 0.3, edges: 2 },
  },

  differences: [
    'Two of the plugin\u2019s claims are checkable here rather than taken on trust. The two "Null" presets set Quality to Fast and one of the two nulls: the picture comes back untouched, at any dolly, because a scene with no depth cannot be dolly-zoomed. And whatever else you change, the anchor surface does not move.',
    'Quality must be Fast for a null to be exact. The supersample grid still runs when the geometry is the identity, so at Good or Best the picture is resampled — very slightly softened — even though nothing moved.',
    'The Luma and Alpha depth modes are reading brightness or alpha out of a generated clip, not a real depth pass. That is the same thing the plugin does with an ordinary clip, and it is worth knowing that the plugin has never been given a rendered depth map either.',
    'The plugin\u2019s numerical proof — its GLSL measured against an independent C++ implementation over 180 combinations, the anchor measured at 0.0000 of travel, and the depth solve measured against the same solve in double — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
