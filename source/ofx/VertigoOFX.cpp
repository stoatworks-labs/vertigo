/// The OpenFX build of Vertigo, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// Same effect as the FFGL build: the dolly-zoom model lives once, in
/// Dolly.cpp, and this file links it rather than copying it. What *is* mirrored
/// here is the per-pixel machinery of Shaders.cpp -- the radial decomposition,
/// the fixed-point depth solve, the edge modes, the rotated supersample grid,
/// the 5-tap depth blur -- because the GPU did that work per fragment and here
/// it runs on the CPU. When editing the fragment shader's pixel machinery, edit
/// this too; the dolly maths itself has only the one home.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../Dolly.h"
#include "../Presets.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.vertigo";
constexpr const char* kPluginName       = "Vertigo";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"The dolly zoom: the shot where one thing holds still and the world "
	"moves.\n\n"
	"The camera tracks along its own axis while the focal length changes to "
	"hold one surface exactly the size it already was. Radial invents the "
	"depth -- near on the axis, far at the corners, or the reverse -- and "
	"works on any clip. Luma and Alpha read a real depth field out of the "
	"picture, which is the actual reprojection when the clip carries one.\n\n"
	"Relief at the middle is a scene with no depth, and a scene with no depth "
	"cannot be dolly-zoomed, so that is the null.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamPreset   = "preset";
constexpr const char* kParamDolly    = "dolly";
constexpr const char* kParamRelief   = "relief";
constexpr const char* kParamAnchor   = "anchor";
constexpr const char* kParamDepth    = "depth";
constexpr const char* kParamFalloff  = "falloff";
constexpr const char* kParamSmooth   = "smooth";
constexpr const char* kParamCentreX  = "centreX";
constexpr const char* kParamCentreY  = "centreY";
constexpr const char* kParamOverscan = "overscan";
constexpr const char* kParamEdges    = "edges";
constexpr const char* kParamQuality  = "quality";

enum class EdgeMode
{
	Transparent = 0,
	Black       = 1,
	Clamp       = 2,
	Mirror      = 3,
	Wrap        = 4
};

/// Everything the effect needs, in the physical units Dolly.h works in. Filled
/// once per render from the 0..1 parameters via the same conversion functions
/// the FFGL build uses.
struct DollySettings
{
	double sigma       = 0.0;
	double relief      = 0.0;
	double anchorLevel = 1.0;
	double gamma       = 1.0;
	vertigo::DepthSource depth = vertigo::DepthSource::Radial;
	double smooth      = 0.0;
	double centreX     = 0.0;//!< offset of the optical axis from frame centre
	double centreY     = 0.0;
	double overscan    = 1.0;
	EdgeMode edges     = EdgeMode::Clamp;
	int taps           = 2;  //!< grid order n, n*n samples per output pixel
};

/// GLSL mod(): x - y*floor(x/y), correct for negatives, which std::fmod isn't.
inline double glslMod( double x, double y )
{
	return x - y * std::floor( x / y );
}

inline double mirrorCoord( double x )
{
	const double m = glslMod( x, 2.0 );
	return ( m > 1.0 ) ? ( 2.0 - m ) : m;
}

class VertigoProcessorBase : public OFX::ImageProcessor
{
public:
	explicit VertigoProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setup( OFX::Image* src, const DollySettings& settings, bool premultipliedValue )
	{
		srcImg        = src;
		dolly         = settings;
		premultiplied = premultipliedValue;

		const OfxRectI b = src->getBounds();
		srcW             = b.x2 - b.x1;
		srcH             = b.y2 - b.y1;

		const double par = src->getPixelAspectRatio() > 0.0 ? src->getPixelAspectRatio() : 1.0;
		aspect           = double( srcW ) * par / double( srcH );
		refRadius        = vertigo::referenceRadius( aspect );
	}

protected:
	OFX::Image* srcImg = nullptr;
	DollySettings dolly;
	bool premultiplied = false;
	int srcW           = 0;
	int srcH           = 0;
	double aspect      = 1.0;
	double refRadius   = 0.5;
};

template<class PIX, int nComponents, int maxValue>
class VertigoProcessor : public VertigoProcessorBase
{
public:
	explicit VertigoProcessor( OFX::ImageEffect& effect ) :
		VertigoProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI dstBounds = _dstImg->getBounds();
		const double invW        = 1.0 / double( dstBounds.x2 - dstBounds.x1 );
		const double invH        = 1.0 / double( dstBounds.y2 - dstBounds.y1 );

		const int n          = std::max( dolly.taps, 1 );
		const double invTaps = 1.0 / double( n );

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast<PIX*>( _dstImg->getPixelAddress( window.x1, y ) );

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				double sum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };

				for( int j = 0; j < n; ++j )
				{
					for( int i = 0; i < n; ++i )
					{
						// The rotated supersample grid, exactly as in the
						// shader: 26.6 degrees off the pixel axes so a regular
						// grid doesn't sample every horizontal edge at the
						// same few heights.
						const double ox = ( i + 0.5 ) * invTaps - 0.5;
						const double oy = ( j + 0.5 ) * invTaps - 0.5;
						const double rx = ox * 0.8944272 - oy * 0.4472136;
						const double ry = ox * 0.4472136 + oy * 0.8944272;

						const double px = ( x - dstBounds.x1 + 0.5 + rx ) * invW;
						const double py = ( y - dstBounds.y1 + 0.5 + ry ) * invH;

						double s[ 4 ];
						double sx, sy;
						solveSourcePoint( px, py, sx, sy );
						fetch( sx, sy, s );
						for( int c = 0; c < 4; ++c )
							sum[ c ] += s[ c ];
					}
				}

				const double scale = 1.0 / double( n * n );
				double r = sum[ 0 ] * scale;
				double g = sum[ 1 ] * scale;
				double b = sum[ 2 ] * scale;
				double a = sum[ 3 ] * scale;

				// Samples were averaged premultiplied (the correct filter at a
				// transparent edge). Premultiplied output just keeps the
				// invariant rgb <= a; straight output unpremultiplies again.
				if( premultiplied || nComponents == 3 )
				{
					r = std::min( r, a );
					g = std::min( g, a );
					b = std::min( b, a );
				}
				else if( a > 0.0 )
				{
					r /= a;
					g /= a;
					b /= a;
				}

				dstPix[ 0 ] = quantise( r );
				dstPix[ 1 ] = quantise( g );
				dstPix[ 2 ] = quantise( b );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( a );
			}
		}
	}

private:
	/// Mirror of solveSourcePoint() in Shaders.cpp, including the reason the
	/// two branches differ: the Radial field is a function of the point being
	/// written, so it is closed form; a field read out of the picture is a
	/// function of the point being read, which is what is being solved for.
	void solveSourcePoint( double px, double py, double& outX, double& outY ) const
	{
		const double axisX  = 0.5 + dolly.centreX;
		const double axisY  = 0.5 + dolly.centreY;
		const double offX   = px - axisX;
		const double offY   = py - axisY;

		if( dolly.depth == vertigo::DepthSource::Radial )
		{
			const double m = magnificationAt( vertigo::radialBase( rhoHat( px, py ) ) );
			outX           = axisX + offX / ( m * dolly.overscan );
			outY           = axisY + offY / ( m * dolly.overscan );
			return;
		}

		outX = px;
		outY = py;
		for( int i = 0; i < vertigo::kDepthIterations; ++i )
		{
			const double m = magnificationAt( depthAt( outX, outY ) );
			outX           = axisX + offX / ( m * dolly.overscan );
			outY           = axisY + offY / ( m * dolly.overscan );
		}
	}

	double rhoHat( double px, double py ) const
	{
		const double cx = ( px - ( 0.5 + dolly.centreX ) ) * aspect;
		const double cy = py - ( 0.5 + dolly.centreY );
		return std::sqrt( cx * cx + cy * cy ) / refRadius;
	}

	double magnificationAt( double base ) const
	{
		return vertigo::magnification(
			vertigo::disparity( base, dolly.gamma, dolly.relief, dolly.anchorLevel ),
			dolly.sigma, dolly.anchorLevel );
	}

	/// The raw field, read out of the picture. Deliberately NOT routed through
	/// the edge mode -- off the frame the depth extends from the edge whatever
	/// the picture does there, because Transparent would otherwise read as
	/// "infinitely far" and ring every frame with runaway magnification.
	double rawDepth( double px, double py ) const
	{
		double texel[ 4 ];
		fetchInside( px, py, texel );

		if( dolly.depth == vertigo::DepthSource::Luma )
			return texel[ 0 ] * vertigo::kLumaR + texel[ 1 ] * vertigo::kLumaG + texel[ 2 ] * vertigo::kLumaB;
		return texel[ 3 ];
	}

	/// The same crude 5-tap the shader uses. Crude on purpose: it is softening
	/// a control field, not the picture.
	double depthAt( double px, double py ) const
	{
		if( dolly.smooth <= 0.0 )
			return rawDepth( px, py );

		const double r = dolly.smooth;
		double sum     = 2.0 * rawDepth( px, py );
		sum += rawDepth( px + r, py + r );
		sum += rawDepth( px + r, py - r );
		sum += rawDepth( px - r, py + r );
		sum += rawDepth( px - r, py - r );
		return sum / 6.0;
	}

	/// Fetch at picture-space p, premultiplied RGBA out. Mirrors the shader:
	/// edge mode first, then a bilinear tap kept inside the picture.
	void fetch( double px, double py, double out[ 4 ] ) const
	{
		const bool outside = px < 0.0 || py < 0.0 || px > 1.0 || py > 1.0;

		switch( dolly.edges )
		{
		case EdgeMode::Transparent:
			if( outside )
			{
				out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
				return;
			}
			break;
		case EdgeMode::Black:
			if( outside )
			{
				out[ 0 ] = out[ 1 ] = out[ 2 ] = 0.0;
				out[ 3 ]                       = 1.0;
				return;
			}
			break;
		case EdgeMode::Clamp:
			px = std::clamp( px, 0.0, 1.0 );
			py = std::clamp( py, 0.0, 1.0 );
			break;
		case EdgeMode::Mirror:
			px = mirrorCoord( px );
			py = mirrorCoord( py );
			break;
		case EdgeMode::Wrap:
			px = glslMod( px, 1.0 );
			py = glslMod( py, 1.0 );
			break;
		}

		fetchInside( px, py, out );
	}

	/// A bilinear tap, kept half a texel inside the picture so it never
	/// weights anything beyond the edge row.
	void fetchInside( double px, double py, double out[ 4 ] ) const
	{
		double fx = px * srcW - 0.5;
		double fy = py * srcH - 0.5;
		fx        = std::clamp( fx, 0.0, double( srcW - 1 ) );
		fy        = std::clamp( fy, 0.0, double( srcH - 1 ) );

		const int x0    = int( fx );
		const int y0    = int( fy );
		const int x1    = std::min( x0 + 1, srcW - 1 );
		const int y1    = std::min( y0 + 1, srcH - 1 );
		const double tx = fx - x0;
		const double ty = fy - y0;

		double p00[ 4 ], p10[ 4 ], p01[ 4 ], p11[ 4 ];
		texel( x0, y0, p00 );
		texel( x1, y0, p10 );
		texel( x0, y1, p01 );
		texel( x1, y1, p11 );

		for( int c = 0; c < 4; ++c )
		{
			const double top    = p00[ c ] + ( p10[ c ] - p00[ c ] ) * tx;
			const double bottom = p01[ c ] + ( p11[ c ] - p01[ c ] ) * tx;
			out[ c ]            = top + ( bottom - top ) * ty;
		}
	}

	/// One texel, premultiplied RGBA in 0..1. Straight-alpha input is
	/// premultiplied here so the averaging above filters correctly -- and so
	/// that a luma depth reading matches the FFGL build, where the host always
	/// hands over premultiplied pixels.
	void texel( int x, int y, double out[ 4 ] ) const
	{
		const OfxRectI b  = srcImg->getBounds();
		const PIX* srcPix = static_cast<const PIX*>( srcImg->getPixelAddress( b.x1 + x, b.y1 + y ) );
		if( !srcPix )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
			return;
		}

		out[ 0 ] = srcPix[ 0 ] / double( maxValue );
		out[ 1 ] = srcPix[ 1 ] / double( maxValue );
		out[ 2 ] = srcPix[ 2 ] / double( maxValue );
		out[ 3 ] = nComponents == 4 ? srcPix[ 3 ] / double( maxValue ) : 1.0;

		if( !premultiplied && nComponents == 4 )
		{
			out[ 0 ] *= out[ 3 ];
			out[ 1 ] *= out[ 3 ];
			out[ 2 ] *= out[ 3 ];
		}
	}

	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

class VertigoPlugin : public OFX::ImageEffect
{
public:
	explicit VertigoPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip  = fetchClip( kOfxImageEffectOutputClipName );
		srcClip  = fetchClip( kOfxImageEffectSimpleSourceClipName );
		preset   = fetchChoiceParam( kParamPreset );
		dolly    = fetchDoubleParam( kParamDolly );
		relief   = fetchDoubleParam( kParamRelief );
		anchor   = fetchDoubleParam( kParamAnchor );
		depth    = fetchChoiceParam( kParamDepth );
		falloff  = fetchDoubleParam( kParamFalloff );
		smooth   = fetchDoubleParam( kParamSmooth );
		centreX  = fetchDoubleParam( kParamCentreX );
		centreY  = fetchDoubleParam( kParamCentreY );
		overscan = fetchDoubleParam( kParamOverscan );
		edges    = fetchChoiceParam( kParamEdges );
		quality  = fetchChoiceParam( kParamQuality );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr<OFX::Image> dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr<OFX::Image> src( srcClip->fetchImage( args.time ) );

		const DollySettings settings = settingsAtTime( args.time );
		const bool premultiplied     = srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum bitDepth    = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		switch( bitDepth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run<VertigoProcessor<unsigned char, 4, 255>>( args, dst.get(), src.get(), settings, premultiplied )
				: run<VertigoProcessor<unsigned char, 3, 255>>( args, dst.get(), src.get(), settings, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run<VertigoProcessor<unsigned short, 4, 65535>>( args, dst.get(), src.get(), settings, premultiplied )
				: run<VertigoProcessor<unsigned short, 3, 65535>>( args, dst.get(), src.get(), settings, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run<VertigoProcessor<float, 4, 1>>( args, dst.get(), src.get(), settings, premultiplied )
				: run<VertigoProcessor<float, 3, 1>>( args, dst.get(), src.get(), settings, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		using namespace vertigo::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset — same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			setIfChanged( dolly, p.v[ kDolly ] );
			setIfChanged( relief, p.v[ kRelief ] );
			setIfChanged( anchor, p.v[ kAnchor ] );
			setIfChanged( falloff, p.v[ kFalloff ] );
			setIfChanged( smooth, p.v[ kSmooth ] );
			setIfChanged( edges, p.v[ kEdges ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back
		// to the sliders. Judged by value, not by the change reason: hosts are
		// not consistent about reasons, but "still equal to the preset" is
		// unambiguous and also absorbs the host echoing our own setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p    = kPresets[ active - 1 ];
		const bool covered =
			( paramName == kParamDolly && differs( dolly, p.v[ kDolly ] ) ) ||
			( paramName == kParamRelief && differs( relief, p.v[ kRelief ] ) ) ||
			( paramName == kParamAnchor && differs( anchor, p.v[ kAnchor ] ) ) ||
			( paramName == kParamFalloff && differs( falloff, p.v[ kFalloff ] ) ) ||
			( paramName == kParamSmooth && differs( smooth, p.v[ kSmooth ] ) ) ||
			( paramName == kParamEdges && differs( edges, p.v[ kEdges ] ) );

		if( covered )
		{
			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
		}
	}

	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime ) override
	{
		// Two independent nulls, both exact rather than approximate: no dolly
		// is no move, and no relief is a scene with no depth, which cannot be
		// dolly-zoomed however hard the camera runs. Either way nothing is
		// magnified — provided the overscan is not separately reframing it.
		const double d = dolly->getValueAtTime( args.time );
		const double r = relief->getValueAtTime( args.time );
		const double o = overscan->getValueAtTime( args.time );

		if( std::abs( o - 0.5 ) < 1e-9 && ( std::abs( d - 0.5 ) < 1e-9 || std::abs( r - 0.5 ) < 1e-9 ) )
		{
			identityClip = srcClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

private:
	// The preset table is plain floats; these give each param type its reading
	// of one. Option values are element indices.
	static void setIfChanged( OFX::DoubleParam* p, float v )
	{
		if( differs( p, v ) )
			p->setValue( double( v ) );
	}
	static void setIfChanged( OFX::ChoiceParam* p, float v )
	{
		if( differs( p, v ) )
			p->setValue( int( std::lround( v ) ) );
	}
	static bool differs( OFX::DoubleParam* p, float v )
	{
		double current = 0.0;
		p->getValue( current );
		return std::fabs( current - double( v ) ) > 1e-4;
	}
	static bool differs( OFX::ChoiceParam* p, float v )
	{
		int current = 0;
		p->getValue( current );
		return current != int( std::lround( v ) );
	}

	DollySettings settingsAtTime( double t ) const
	{
		DollySettings s;
		s.sigma       = vertigo::sigmaFromParam( float( dolly->getValueAtTime( t ) ) );
		s.relief      = vertigo::reliefFromParam( float( relief->getValueAtTime( t ) ) );
		s.anchorLevel = anchor->getValueAtTime( t );
		s.gamma       = vertigo::gammaFromParam( float( falloff->getValueAtTime( t ) ) );
		s.smooth      = vertigo::smoothFromParam( float( smooth->getValueAtTime( t ) ) );

		int depthValue = 0, edgesValue = 2, qualityValue = 1;
		depth->getValueAtTime( t, depthValue );
		edges->getValueAtTime( t, edgesValue );
		quality->getValueAtTime( t, qualityValue );

		s.depth    = vertigo::depthSourceFromParam( float( depthValue ) );
		s.centreX  = centreX->getValueAtTime( t ) - 0.5;
		s.centreY  = centreY->getValueAtTime( t ) - 0.5;
		s.overscan = vertigo::overscanFromParam( float( overscan->getValueAtTime( t ) ) );
		s.edges    = EdgeMode( edgesValue );
		s.taps     = vertigo::tapsFromParam( float( qualityValue ) );
		return s;
	}

	template<class Processor>
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src,
			  const DollySettings& settings, bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setup( src, settings, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip         = nullptr;
	OFX::Clip* srcClip         = nullptr;
	OFX::ChoiceParam* preset   = nullptr;
	OFX::DoubleParam* dolly    = nullptr;
	OFX::DoubleParam* relief   = nullptr;
	OFX::DoubleParam* anchor   = nullptr;
	OFX::ChoiceParam* depth    = nullptr;
	OFX::DoubleParam* falloff  = nullptr;
	OFX::DoubleParam* smooth   = nullptr;
	OFX::DoubleParam* centreX  = nullptr;
	OFX::DoubleParam* centreY  = nullptr;
	OFX::DoubleParam* overscan = nullptr;
	OFX::ChoiceParam* edges    = nullptr;
	OFX::ChoiceParam* quality  = nullptr;

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
										  const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( VertigoPluginFactory, {}, {} );

void VertigoPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// This samples anywhere in the source -- and, in the depth modes, at a
	// place it does not know until it has read the picture -- so it cannot
	// render from tiles. Frames are still independent of each other and of
	// render order.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void VertigoPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults as the FFGL build, so
	// the two inspectors read identically and the docs cover both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	// Custom is not a preset: it means the sliders are the truth.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Factory shots. Picking one sets the shot controls; "
	                      "editing any of them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < vertigo::presets::kCount; ++i )
		presetParam->appendOption( vertigo::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label itself does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	OFX::GroupParamDescriptor* shot = desc.defineGroupParam( "Shot" );
	shot->setLabels( "Shot", "Shot", "Shot" );

	defineSlider( desc, page, kParamDolly, "Dolly",
				  "How far the camera travels, as the ratio between how much the "
				  "nearest surface grows and how much infinity does. 0.5 is no "
				  "move; below is a pull back, above is a push in.",
				  0.30 )
		->setParent( *shot );
	defineSlider( desc, page, kParamRelief, "Relief",
				  "How much depth the scene has. 0.5 is flat, and a flat scene "
				  "cannot be dolly-zoomed. Below 0.5 turns the depth inside out: "
				  "the middle becomes the far end.",
				  0.75 )
		->setParent( *shot );
	defineSlider( desc, page, kParamAnchor, "Anchor",
				  "Which depth is held fixed: 1 is the nearest surface, 0 the "
				  "furthest. In Radial, 1 is the optical axis and 0 the corners.",
				  1.00 )
		->setParent( *shot );

	OFX::GroupParamDescriptor* depthGroup = desc.defineGroupParam( "Depth" );
	depthGroup->setLabels( "Depth", "Depth", "Depth" );

	OFX::ChoiceParamDescriptor* depthParam = desc.defineChoiceParam( kParamDepth );
	depthParam->setLabels( "Depth", "Depth", "Depth" );
	depthParam->setHint( "Where the depth comes from. Radial invents it and works on "
	                     "any clip; Luma and Alpha read it out of the picture." );
	depthParam->appendOption( "Radial" );
	depthParam->appendOption( "Luma" );
	depthParam->appendOption( "Alpha" );
	depthParam->setDefault( 0 );
	depthParam->setParent( *depthGroup );
	page->addChild( *depthParam );

	defineSlider( desc, page, kParamFalloff, "Falloff",
				  "Gamma on the depth field: where between the near and far ends "
				  "most of the scene sits. 0.5 is linear.",
				  0.50 )
		->setParent( *depthGroup );
	defineSlider( desc, page, kParamSmooth, "Smooth",
				  "Blurs the depth field, not the picture. Nothing in Radial, "
				  "which is already smooth; on a real depth map it is what stops "
				  "a hard depth step smearing.",
				  0.25 )
		->setParent( *depthGroup );

	OFX::GroupParamDescriptor* frame = desc.defineGroupParam( "Frame" );
	frame->setLabels( "Frame", "Frame", "Frame" );

	defineSlider( desc, page, kParamCentreX, "Centre X",
				  "The optical axis: what the move is about. 0.5 is centred.", 0.5 )
		->setParent( *frame );
	defineSlider( desc, page, kParamCentreY, "Centre Y",
				  "The optical axis: what the move is about. 0.5 is centred.", 0.5 )
		->setParent( *frame );
	defineSlider( desc, page, kParamOverscan, "Overscan",
				  "Scales the picture to buy back the frame a push in gives away. "
				  "0.5 is 1:1; two stops either side.",
				  0.5 )
		->setParent( *frame );

	OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
	output->setLabels( "Output", "Output", "Output" );

	OFX::ChoiceParamDescriptor* edgesParam = desc.defineChoiceParam( kParamEdges );
	edgesParam->setLabels( "Edges", "Edges", "Edges" );
	edgesParam->setHint( "What to show where a push in looks past the picture." );
	edgesParam->appendOption( "Transparent" );
	edgesParam->appendOption( "Black" );
	edgesParam->appendOption( "Clamp" );
	edgesParam->appendOption( "Mirror" );
	edgesParam->appendOption( "Wrap" );
	edgesParam->setDefault( 2 );
	edgesParam->setParent( *output );
	page->addChild( *edgesParam );

	OFX::ChoiceParamDescriptor* qualityParam = desc.defineChoiceParam( kParamQuality );
	qualityParam->setLabels( "Quality", "Quality", "Quality" );
	qualityParam->setHint( "Supersampling: Fast is 1 sample, Good 4, Best 16. It does "
	                       "not change the geometry, only how it is sampled." );
	qualityParam->appendOption( "Fast" );
	qualityParam->appendOption( "Good" );
	qualityParam->appendOption( "Best" );
	qualityParam->setDefault( 1 );
	qualityParam->setParent( *output );
	page->addChild( *qualityParam );
}

OFX::ImageEffect* VertigoPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new VertigoPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static VertigoPluginFactory* factory =
		new VertigoPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
