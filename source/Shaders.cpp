#include "Shaders.h"

namespace vertigo
{

const char* const kVertexShader = R"(#version 410 core

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
)";

const char* const kFragmentShader = R"(#version 410 core

uniform sampler2D InputTexture;

uniform vec2 MaxUV;        //the part of the input texture that is really picture
uniform vec2 HalfTexel;    //half an input texel, in picture space
uniform vec2 Centre;       //offset of the optical axis from the middle of frame
uniform float Aspect;      //picture width / height
uniform float Sigma;       //the dolly, t/z_anchor; always below 1
uniform float AnchorLevel; //the disparity that is held fixed
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
// `vgtest --probe`, which exists to catch exactly this pair drifting apart.
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

float disparity( float base )
{
	float field = pow( clamp( base, 0.0, 1.0 ), Gamma );

	//Scaled about the anchor, which is what makes Relief = 0 the identity: the
	//whole scene collapses onto the one surface that by definition does not
	//move. The clamp is what keeps the magnification below free of poles.
	return clamp( AnchorLevel + Relief * ( field - AnchorLevel ), 0.0, 1.0 );
}

float magnification( float delta )
{
	//Sigma < 1 by construction and delta is in 0..1, so the denominator is
	//unconditionally positive. There is nothing to guard here and nothing that
	//can produce a NaN, which is why this effect has no equivalent of the
	//"no source for this pixel" case a lens warp needs.
	return ( 1.0 - Sigma * AnchorLevel ) / ( 1.0 - Sigma * delta );
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

/// Where the pixel at output point `p` came from.
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
)";

} // namespace vertigo
