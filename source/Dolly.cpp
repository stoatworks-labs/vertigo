#include "Dolly.h"

#include <algorithm>
#include <cmath>

namespace vertigo
{
namespace
{
/// Two stops either side of unity, for every geometric control here. Written
/// once because they have to agree: the Dolly and the Overscan are read against
/// each other constantly -- a push in of P shrinks infinity by 1/P, and the
/// operator's next move is to overscan it back.
constexpr double kStops = 4.0;

double geometric( float value )
{
	return std::pow( 2.0, ( static_cast< double >( value ) - 0.5 ) * kStops );
}
} // namespace

double referenceRadius( double aspect )
{
	return 0.5 * std::sqrt( aspect * aspect + 1.0 );
}

double radialBase( double rhoHat )
{
	// Linear in radius, so that with the default gamma the near plane is the
	// optical axis and the far plane is the frame corner exactly. Everything
	// interesting about the shape of the field is the gamma's job; keeping this
	// straight means the gamma control has one clear meaning rather than
	// modifying something already curved.
	return 1.0 - std::clamp( rhoHat, 0.0, 1.0 );
}

double disparity( double base, double gamma, double relief )
{
	return anchorDisparity( std::pow( std::clamp( base, 0.0, 1.0 ), gamma ), relief );
}

double anchorDisparity( double anchorLevel, double relief )
{
	// Scaled about the middle of the range, not about the anchor. Both put the
	// anchor at a fixed point -- see the note in Dolly.h for why only this one
	// leaves the field reachable when relief goes negative.
	const double scaled = 0.5 + relief * ( std::clamp( anchorLevel, 0.0, 1.0 ) - 0.5 );

	// The clamp is load-bearing, not defensive. m() below divides by
	// 1 - sigma*delta, and sigma is under 1 by construction, so a disparity
	// held inside 0..1 makes that denominator unconditionally positive. A
	// relief past 1 pushes the field outside 0..1 and this is where it lands:
	// as a near plane and a far plane, which is what a real camera has anyway.
	return std::clamp( scaled, 0.0, 1.0 );
}

double magnification( double delta, double sigma, double anchorDelta )
{
	return ( 1.0 - sigma * anchorDelta ) / ( 1.0 - sigma * delta );
}

double radialAnchorRadius( double anchorLevel, double gamma )
{
	// Invert radialBase() through the gamma: (1 - rhoHat)^gamma == anchorLevel.
	// Independent of relief on purpose -- relief scales the field *about* the
	// anchor, so it cannot move it.
	const double a = std::clamp( anchorLevel, 0.0, 1.0 );
	return std::clamp( 1.0 - std::pow( a, 1.0 / gamma ), 0.0, 1.0 );
}

double sigmaFromParam( float value )
{
	// sigma = 1 - 1/P. Derived from the parallax ratio rather than stated
	// directly so that the slider is symmetric in the thing an operator sees:
	// P and 1/P are equal distances from the middle, and sigma's own wild
	// asymmetry (0.75 one way, -3 the other) never has to be looked at.
	return 1.0 - 1.0 / parallaxFromParam( value );
}

double parallaxFromParam( float value )
{
	return geometric( value );
}

double reliefFromParam( float value )
{
	// Signed, and zero in the middle. Positive is the reading everybody means
	// by a dolly zoom -- a subject near the axis with the world behind it.
	// Negative turns the field inside out into the other famous shot, the one
	// down a stairwell, where the middle is the far end and the walls are
	// close. Past +-1 the field over-extends and clamps, which is a near plane
	// and a far plane appearing, not a failure.
	return ( static_cast< double >( value ) - 0.5 ) * 4.0;
}

double gammaFromParam( float value )
{
	return geometric( value );
}

double overscanFromParam( float value )
{
	return geometric( value );
}

double smoothFromParam( float value )
{
	// 4% of the picture at full travel. This blurs the *depth*, never the
	// picture, and the reason it exists is that an inverse-mapped parallax has
	// no holes to fill -- it smears instead, along whichever edge the depth
	// steps across. Softening the step is the whole repair.
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 ) * 0.04;
}

int tapsFromParam( float value )
{
	const int option = static_cast< int >( std::lround( value ) );
	switch( option )
	{
	case 0:
		return 1;//Fast: one sample, aliases where the dolly minifies hard
	case 2:
		return 4;//Best: 16 samples
	case 1:
	default:
		return 2;//Good: 4 samples
	}
}

DepthSource depthSourceFromParam( float value )
{
	const int option = static_cast< int >( std::lround( value ) );
	switch( option )
	{
	case 1:
		return DepthSource::Luma;
	case 2:
		return DepthSource::Alpha;
	case 0:
	default:
		return DepthSource::Radial;
	}
}

bool depthSourceIsSampled( DepthSource source )
{
	return source != DepthSource::Radial;
}

} // namespace vertigo
