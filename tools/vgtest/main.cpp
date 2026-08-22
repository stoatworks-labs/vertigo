/**
    vgtest -- render Vertigo offline, and measure what it did.

    A dolly zoom is judged on where the pixels went, and that is a measurable
    thing rather than a matter of taste. So this harness is not only a
    previewer. It builds a headless GL 4.1 core context, drives the real Vertigo
    class through the real FFGL entry sequence, and offers three measurements,
    each answering a question none of the others can:

        vgtest --out /tmp/frame.png    a picture, on a depth card
        vgtest --probe                 does the GPU agree with Dolly.cpp?
        vgtest --anchor                does the anchor surface really not move?
        vgtest --depth                 does the depth-map solve land where it should?

    `--probe` is the one that guards the duplication. The dolly maths exists
    twice -- in C++ in Dolly.cpp and in GLSL in Shaders.cpp -- because it has to
    run per pixel on the GPU but also has to be readable and testable on the
    CPU. Two copies of one formula drift. So the probe feeds in a picture whose
    brightness *is* the normalised radius, reads the output back, and reports
    the source radius the GPU actually sampled from beside the one the C++
    predicts.

    `--anchor` is different in kind: it does not check an implementation against
    another implementation, it checks the model's defining claim. A dolly zoom
    is the shot in which one surface does not move. So render the same picture
    at several dolly settings and measure how far the anchor ring moved -- and,
    just as importantly, how far the rest of the frame moved, because a test
    that passes because nothing moved anywhere is not a test.

    `--depth` covers the half of the plugin the other two cannot reach. When the
    field is read out of the picture rather than invented, the source point is
    the solution of a fixed-point iteration rather than a closed form, and the
    only way to know the shader is solving it correctly is to solve it here too,
    in double, and compare.
*/

#include "Dolly.h"
#include "Vertigo.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace vertigo;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Test pictures. Each one exists to make a particular kind of wrong answer
// visible; none of them is meant to look nice.
//---------------------------------------------------------------------------
/// Writes with y measured from the top, which is how the patterns below are
/// described and how the PNG will be read. GL puts row zero at the bottom of a
/// texture, so the flip happens here rather than somewhere in the middle of the
/// chain where it would be a permanent trap.
///
/// The consequence worth remembering: **the buffers these builders return are
/// bottom-up**, ready for glTexImage2D. readBack() returns top-down. Comparing
/// one against the other index by index silently compares row y with row
/// height-1-y, so anything that does that has to flipRows() first.
void setPixel( std::vector< unsigned char >& image, int width, int height, int x, int y,
               float r, float g, float b, float a = 1.0f )
{
	const size_t i = ( static_cast< size_t >( height - 1 - y ) * width + x ) * 4;
	auto byte      = []( float v ) {
        return static_cast< unsigned char >( std::lround( std::fmin( std::fmax( v, 0.0f ), 1.0f ) * 255.0f ) );
	};
	image[ i + 0 ] = byte( r );
	image[ i + 1 ] = byte( g );
	image[ i + 2 ] = byte( b );
	image[ i + 3 ] = byte( a );
}

/// Normalised radius of a pixel centre, in reference-radius units, about the
/// middle of the frame. The same quantity `rhoHat()` computes in the shader.
double rhoAt( int x, int y, int width, int height, double aspect )
{
	const double cx = ( ( x + 0.5 ) / width - 0.5 ) * aspect;
	const double cy = ( y + 0.5 ) / height - 0.5;
	return std::sqrt( cx * cx + cy * cy ) / referenceRadius( aspect );
}

/// The card. A dolly zoom moves things by an amount that depends on where they
/// are, so the card is made of things whose position can be read off: a grid
/// whose spacing shows the local magnification, rings at known radii to check
/// which of them held still, and a band of fine detail out where the effect
/// compresses hardest so that the Quality control has something to fail at.
///
/// The greys are chosen so the card doubles as a depth map: brightness falls
/// with radius, so switching Depth to Luma on this picture gives roughly the
/// same field the Radial mode invents, and the two can be compared by eye.
std::vector< unsigned char > buildDepthCard( int width, int height, double aspect )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	const double R         = referenceRadius( aspect );
	const double gridPitch = 1.0 / 16.0;//in height units

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const double cx  = ( ( x + 0.5 ) / width - 0.5 ) * aspect;
			const double cy  = ( y + 0.5 ) / height - 0.5;
			const double rho = std::sqrt( cx * cx + cy * cy ) / R;

			//The base is the depth: bright in the middle, dark at the corners,
			//so that Depth = Luma on this card reads as very nearly the field
			//Depth = Radial invents.
			const float depth = static_cast< float >( std::clamp( 1.0 - rho, 0.0, 1.0 ) );
			float r = depth * 0.35f, g = depth * 0.35f, b = depth * 0.40f;

			//Grid. Lines land on exact fractions of the height, so the change
			//in their spacing is the magnification, directly.
			const double gx = std::fabs( std::fmod( std::fabs( cx ) + gridPitch * 0.5, gridPitch ) - gridPitch * 0.5 );
			const double gy = std::fabs( std::fmod( std::fabs( cy ) + gridPitch * 0.5, gridPitch ) - gridPitch * 0.5 );
			const double lineHalfWidth = 0.6 / height;
			if( gx < lineHalfWidth || gy < lineHalfWidth )
			{
				r = g = b = 0.30f + depth * 0.60f;
			}

			//Rings at quarters of the frame diagonal. One of these is the
			//anchor at any given setting, and it is the one that did not move.
			for( int ring = 1; ring <= 4; ++ring )
			{
				const double target = ring * 0.25;
				if( std::fabs( rho - target ) < ( 1.2 / height ) / R )
				{
					const bool outer = ring == 4;
					r                = outer ? 1.0f : 0.10f;
					g                = outer ? 0.55f : 0.85f;
					b                = outer ? 0.10f : 1.0f;
				}
			}

			//A band of fine detail two thirds of the way out, at a frequency
			//that will alias the moment the effect minifies it. This is what
			//the Quality control is for, and what it looks like when it is off.
			if( rho > 0.60 && rho < 0.72 )
			{
				const int checker = ( x / 2 + y / 2 ) & 1;
				r = g = b = checker ? 0.95f : 0.05f;
			}

			setPixel( image, width, height, x, y, r, g, b );
		}
	}

	return image;
}

/// Brightness *is* the normalised radius. Feeding this in means the output
/// pixel at radius rho reports, as a number, the radius the shader sampled
/// from -- which is the whole mechanism of --probe.
std::vector< unsigned char > buildRadialRamp( int width, int height, double aspect )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float v = static_cast< float >( std::fmin( rhoAt( x, y, width, height, aspect ), 1.0 ) );
			setPixel( image, width, height, x, y, v, v, v );
		}

	return image;
}

/// Brightness *is* the horizontal position, which makes this picture its own
/// depth map: it is both the field the shader reads and the readout of where
/// the shader read it from. That is what makes --depth possible without a
/// second input.
std::vector< unsigned char > buildHorizontalRamp( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			//Equal weights sum to one, so the Rec.709 luma of this pixel is
			//exactly v -- the depth the shader will read is the position.
			const float v = static_cast< float >( ( x + 0.5 ) / width );
			setPixel( image, width, height, x, y, v, v, v );
		}

	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without a
	//GPU, where it will at least prove the shader compiles.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

bool runPass( Vertigo& plugin, GLuint source, GLuint targetFBO, int width, int height )
{
	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle                              = source;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = targetFBO;

	glBindFramebuffer( GL_FRAMEBUFFER, targetFBO );
	glViewport( 0, 0, width, height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

std::vector< unsigned char > readBack( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );

	//GL hands back bottom-up; everything above is written top-down.
	std::vector< unsigned char > flipped( pixels.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             pixels.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameter automation for --pipe.
//
// A plain text file of `frame  Parameter Name  value` lines. Values are held
// before the first key and after the last, and linearly interpolated between,
// which is all a demo reel needs and keeps the whole automation of a video
// readable in one screen of text:
//
//     0     Dolly   0.5
//     120   Dolly   0.9
//     180   Dolly   0.5
//
// Same format as the rest of the fleet's harnesses on purpose: two harnesses
// that read the same file can share a build script.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

bool readExactly( void* into, size_t bytes )
{
	unsigned char* p = static_cast< unsigned char* >( into );
	size_t got       = 0;
	while( got < bytes )
	{
		const size_t n = fread( p + got, 1, bytes - got, stdin );
		if( n == 0 )
			return false;//clean EOF, or a short final frame we cannot use
		got += n;
	}
	return true;
}


//---------------------------------------------------------------------------
/// Prove a factory preset survives whatever the host does next.
///
/// FFGL's host owns parameter state and is free to push it back down at any
/// time, and nothing in the specification obliges it to act on the value
/// events a plugin raises when it changes a parameter itself. So there are
/// three hosts to survive, and the plugin cannot tell which one it is talking
/// to:
///
///   - one that honours the events and hands the new values straight back;
///   - one that ignores them and carries on restating the values it still
///     believes in, which are the ones from before the preset;
///   - one that honours them but keeps its parameters shorter than a float, so
///     what comes back is near the preset rather than equal to it.
///
/// All three arrive as SetFloatParameter calls carrying a changed value, which
/// is why "the value changed, so the operator must have taken over" is the
/// wrong test and why issue #2's reporter saw the dropdown snap back to Custom
/// the instant they chose anything.
///
/// No GL here: this is the parameter plumbing, not the picture.
//---------------------------------------------------------------------------
int runPresetTest()
{
	using namespace vertigo::presets;

	// The display names of the parameters a preset covers, in presets::Param
	// order. Looked up through the plugin's own declaration rather than by
	// index, so a reordering cannot leave this quietly driving the wrong one.
	static const char* const kCoveredNames[ kParamCount ] = {
		"Dolly", "Relief", "Anchor", "Falloff", "Smooth", "Edges"
	};

	enum class Host
	{
		Honours,
		Ignores,
		Quantises
	};
	struct HostCase
	{
		Host kind;
		const char* name;
	};
	const HostCase hosts[] = {
		{ Host::Honours, "honours value events" },
		{ Host::Ignores, "ignores value events" },
		{ Host::Quantises, "honours, 1/1000 steps" },
	};

	int failures = 0;

	for( const HostCase& host : hosts )
	{
		for( int preset = 1; preset <= kCount; ++preset )
		{
			Vertigo plugin;

			auto indexOf = [ & ]( const char* name ) -> int {
				for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
				{
					const char* declared = plugin.GetParamName( i );
					if( declared != nullptr && std::strcmp( declared, name ) == 0 )
						return int( i );
				}
				return -1;
			};

			const int presetIndex = indexOf( "Preset" );
			int covered[ kParamCount ];
			bool namesOk = presetIndex >= 0;
			for( int j = 0; j < kParamCount; ++j )
			{
				covered[ j ] = indexOf( kCoveredNames[ j ] );
				namesOk      = namesOk && covered[ j ] >= 0;
			}
			if( !namesOk )
			{
				std::fprintf( stderr, "presets: a covered parameter is not declared under the name this test expects\n" );
				return 1;
			}

			// What the host thinks the sliders say before the operator reaches
			// for the dropdown.
			float hostOwn[ kParamCount ];
			for( int j = 0; j < kParamCount; ++j )
				hostOwn[ j ] = plugin.GetFloatParameter( unsigned( covered[ j ] ) );

			// The operator picks a preset.
			plugin.SetFloatParameter( unsigned( presetIndex ), float( preset ) );

			// And now the host says its piece.
			for( int j = 0; j < kParamCount; ++j )
			{
				float back = 0.0f;
				switch( host.kind )
				{
				case Host::Honours:
					back = plugin.GetFloatParameter( unsigned( covered[ j ] ) );
					break;
				case Host::Ignores:
					back = hostOwn[ j ];
					break;
				case Host::Quantises:
					back = std::round( plugin.GetFloatParameter( unsigned( covered[ j ] ) ) * 1000.0f ) / 1000.0f;
					break;
				}
				plugin.SetFloatParameter( unsigned( covered[ j ] ), back );
			}

			const int still = int( std::lround( plugin.GetFloatParameter( unsigned( presetIndex ) ) ) );
			bool ok         = still == preset;

			// Still selected is not enough -- it has to be what renders.
			for( int j = 0; j < kParamCount; ++j )
			{
				const float want = kPresets[ preset - 1 ].v[ j ];
				const float got  = plugin.GetFloatParameter( unsigned( covered[ j ] ) );
				ok               = ok && std::fabs( got - want ) <= 1e-4f;
			}

			if( !ok )
			{
				std::printf( "presets %-22s %-20s FAILED (shows %d)\n", host.name, kPresets[ preset - 1 ].name, still );
				++failures;
				continue;
			}

			// An operator turning a covered knob must still drop to Custom --
			// a preset that cannot be left is no better than one that will not
			// stick.
			const float moved = kPresets[ preset - 1 ].v[ 0 ] > 0.5f ? 0.10f : 0.90f;
			plugin.SetFloatParameter( unsigned( covered[ 0 ] ), moved );
			const int after = int( std::lround( plugin.GetFloatParameter( unsigned( presetIndex ) ) ) );
			if( after != 0 )
			{
				std::printf( "presets %-22s %-20s FAILED (an edit left it on %d)\n", host.name, kPresets[ preset - 1 ].name, after );
				++failures;
				continue;
			}

			std::printf( "presets %-22s %-20s ok\n", host.name, kPresets[ preset - 1 ].name );
		}
	}

	std::printf( "%s\n", failures == 0 ? "presets: all ok" : "presets: FAILURES" );
	return failures == 0 ? 0 : 1;
}

void usage()
{
	std::printf(
		"vgtest -- render and measure the Vertigo dolly zoom\n"
		"\n"
		"  --out PATH        where to write (default /tmp/vertigo.png)\n"
		"  --width N         output width (default 1280)\n"
		"  --height N        output height (default 720)\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1\n"
		"  --alpha           keep the alpha channel instead of compositing on black\n"
		"  --ramp            render the horizontal ramp instead of the depth card\n"
		"  --measure         print the mean RGB of the middle of the picture\n"
		"  --probe           measure the radial map on the GPU against the C++\n"
		"  --anchor          measure how far the anchor surface moved (it should not)\n"
		"  --depth           measure the depth-map solve against the same solve in C++\n"
		"  --list            print every parameter and its default, then exit\n"
		"  --presets         every factory preset survives every host behaviour\n"
		"  --pipe            read raw RGBA frames from stdin, write them to stdout,\n"
		"                    so real footage can be put through the real shader:\n"
		"                        ffmpeg ... -f rawvideo -pix_fmt rgba - \\\n"
		"                          | vgtest --pipe --width 1920 --height 1080 \\\n"
		"                          | ffmpeg -f rawvideo -pix_fmt rgba ...\n"
		"  --script PATH     parameter automation for --pipe (see loadScript)\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/vertigo.png";
	int width              = 1280;
	int height             = 720;
	bool keepAlpha         = false;
	bool listOnly          = false;
	bool measure           = false;
	bool probe             = false;
	bool anchor            = false;
	bool depth             = false;
	bool ramp              = false;
	bool presetCheck       = false;
	bool pipeMode          = false;
	std::string scriptPath;
	std::vector< std::pair< std::string, float > > overrides;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--out" )
			outputPath = next();
		else if( arg == "--width" )
			width = std::atoi( next().c_str() );
		else if( arg == "--height" )
			height = std::atoi( next().c_str() );
		else if( arg == "--alpha" )
			keepAlpha = true;
		else if( arg == "--measure" )
			measure = true;
		else if( arg == "--probe" )
			probe = true;
		else if( arg == "--anchor" )
			anchor = true;
		else if( arg == "--depth" )
			depth = true;
		else if( arg == "--ramp" )
			ramp = true;
		else if( arg == "--list" )
			listOnly = true;
		else if( arg == "--presets" )
			presetCheck = true;
		else if( arg == "--pipe" )
			pipeMode = true;
		else if( arg == "--script" )
			scriptPath = next();
		else if( arg == "--set" )
		{
			const std::string assignment = next();
			const size_t equals          = assignment.rfind( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "vgtest: --set wants Name=Value, got '%s'\n", assignment.c_str() );
				return 2;
			}
			overrides.emplace_back( assignment.substr( 0, equals ),
			                        std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			std::fprintf( stderr, "vgtest: unknown argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	// Before the size check and before any GL: the parameter plumbing has
	// nothing to do with either, and a self-test that needed a GPU would not
	// run on a CI box without one.
	if( presetCheck )
		return runPresetTest();

	if( width <= 0 || height <= 0 )
	{
		std::fprintf( stderr, "vgtest: width and height must both be positive\n" );
		return 2;
	}

	Vertigo plugin;

	//Names come from the plugin's own declaration rather than from a table
	//here, so a parameter that is renamed or reordered cannot leave the harness
	//quietly setting the wrong one.
	auto indexOfParameter = [ & ]( const std::string& name ) -> int {
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
				return static_cast< int >( i );
		}
		return -1;
	};
	auto setParameter = [ & ]( const std::string& name, float value ) -> bool {
		const int index = indexOfParameter( name );
		if( index < 0 )
			return false;
		plugin.SetFloatParameter( static_cast< unsigned int >( index ), value );
		return true;
	};
	auto getParameter = [ & ]( const std::string& name ) -> float {
		const int index = indexOfParameter( name );
		return index < 0 ? 0.0f : plugin.GetFloatParameter( static_cast< unsigned int >( index ) );
	};

	if( listOnly )
	{
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			std::printf( "%2u  %-16s %.3f\n", i, plugin.GetParamName( i ), plugin.GetFloatParameter( i ) );
		return 0;
	}

	for( const auto& override : overrides )
	{
		if( !setParameter( override.first, override.second ) )
		{
			std::fprintf( stderr, "vgtest: no parameter named '%s' (try --list)\n", override.first.c_str() );
			return 2;
		}
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "vgtest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	//In pipe mode stdout carries the video, so everything conversational has to
	//go to stderr or it lands in the middle of a frame.
	std::fprintf( pipeMode ? stderr : stdout, "GL %s / %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	const double aspect = static_cast< double >( width ) / static_cast< double >( height );
	const double R      = referenceRadius( aspect );

	FFGLViewportStruct viewport = { 0, 0, static_cast< FFUInt32 >( width ), static_cast< FFUInt32 >( height ) };
	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "vgtest: InitGL failed -- see the diagnostics log\n" );
		return 1;
	}

	auto shutdown = [ & ]( int code ) -> int {
		plugin.DeInitGL();
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return code;
	};

	//-----------------------------------------------------------------------
	// --pipe: real footage through the real shader.
	//
	// The plugin has no window of its own, so the only honest way to show it
	// working on real pictures without driving Resolume is to push frames
	// through the shipped shader chain here.
	//-----------------------------------------------------------------------
	if( pipeMode )
	{
		std::map< std::string, Track > tracks;
		if( !scriptPath.empty() )
		{
			std::string error;
			tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "vgtest: %s\n", error.c_str() );
				return shutdown( 2 );
			}
			//Fail on a name that does not exist rather than silently animating
			//nothing for the length of the reel.
			for( const auto& entry : tracks )
			{
				if( indexOfParameter( entry.first ) < 0 )
				{
					std::fprintf( stderr, "vgtest: script names '%s', which is not a parameter (try --list)\n",
					              entry.first.c_str() );
					return shutdown( 2 );
				}
			}
		}

		const size_t frameBytes = static_cast< size_t >( width ) * height * 4;
		const size_t rowBytes   = static_cast< size_t >( width ) * 4;
		std::vector< unsigned char > in( frameBytes );
		std::vector< unsigned char > flip( frameBytes );
		std::vector< unsigned char > out( frameBytes );

		GLuint sourceTexture = makeTexture( width, height, nullptr );
		GLuint targetTexture = makeTexture( width, height, nullptr );
		GLuint targetFBO     = makeFramebuffer( targetTexture );

		long frame = 0;
		while( readExactly( in.data(), frameBytes ) )
		{
			//ffmpeg hands over top-down rows; GL wants the bottom row first.
			//The two flips do not cancel out -- the map is radial and
			//symmetric, but the picture going through it is not, so getting
			//this wrong returns a vertically mirrored image that still looks
			//like a plausible dolly zoom.
			for( int y = 0; y < height; ++y )
				std::memcpy( flip.data() + static_cast< size_t >( y ) * rowBytes,
				             in.data() + static_cast< size_t >( height - 1 - y ) * rowBytes, rowBytes );

			glBindTexture( GL_TEXTURE_2D, sourceTexture );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flip.data() );
			glBindTexture( GL_TEXTURE_2D, 0 );

			for( const auto& entry : tracks )
				plugin.SetFloatParameter( static_cast< unsigned int >( indexOfParameter( entry.first ) ),
				                          valueAt( entry.second, static_cast< int >( frame ) ) );

			if( !runPass( plugin, sourceTexture, targetFBO, width, height ) )
			{
				std::fprintf( stderr, "vgtest: ProcessOpenGL failed on frame %ld\n", frame );
				return shutdown( 1 );
			}

			glPixelStorei( GL_PACK_ALIGNMENT, 1 );
			glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flip.data() );
			for( int y = 0; y < height; ++y )
				std::memcpy( out.data() + static_cast< size_t >( y ) * rowBytes,
				             flip.data() + static_cast< size_t >( height - 1 - y ) * rowBytes, rowBytes );

			if( fwrite( out.data(), 1, frameBytes, stdout ) != frameBytes )
			{
				std::fprintf( stderr, "vgtest: short write on frame %ld\n", frame );
				return shutdown( 1 );
			}
			++frame;
		}

		std::fflush( stdout );
		std::fprintf( stderr, "vgtest: %ld frames\n", frame );
		return shutdown( 0 );
	}

	//-----------------------------------------------------------------------
	// --probe: what radius did the GPU actually sample from?
	//-----------------------------------------------------------------------
	if( probe )
	{
		//Everything that would move a sample for a reason other than the radial
		//map is switched off: Radial depth (this measures the closed form),
		//no supersampling (nothing to average), no recentring, and clamped
		//edges so nothing reads as transparent. Overscan is left alone, because
		//it is part of the map and belongs in the prediction.
		setParameter( "Depth", 0.0f );
		setParameter( "Quality", 0.0f );
		setParameter( "Centre X", 0.5f );
		setParameter( "Centre Y", 0.5f );
		setParameter( "Edges", 2.0f );

		const double sigma    = sigmaFromParam( getParameter( "Dolly" ) );
		const double relief   = reliefFromParam( getParameter( "Relief" ) );
		const double gamma    = gammaFromParam( getParameter( "Falloff" ) );
		const double overscan = overscanFromParam( getParameter( "Overscan" ) );
		const double anchorLv    = getParameter( "Anchor" );
		const double anchorDelta = anchorDisparity( anchorLv, relief );

		const std::vector< unsigned char > picture = buildRadialRamp( width, height, aspect );
		const GLuint sourceTexture                 = makeTexture( width, height, picture.data() );
		const GLuint targetTexture                 = makeTexture( width, height, nullptr );
		const GLuint targetFBO                     = makeFramebuffer( targetTexture );

		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			std::fprintf( stderr, "vgtest: framebuffer incomplete\n" );
			return shutdown( 1 );
		}
		if( !runPass( plugin, sourceTexture, targetFBO, width, height ) )
		{
			std::fprintf( stderr, "vgtest: ProcessOpenGL failed\n" );
			return shutdown( 1 );
		}

		const std::vector< unsigned char > out = readBack( targetFBO, width, height );

		std::printf( "\nparallax ratio %.3f  sigma %+.4f  relief %+.2f  gamma %.3f  anchor %.2f\n",
		             parallaxFromParam( getParameter( "Dolly" ) ), sigma, relief, gamma, anchorLv );
		std::printf( "%8s %12s %12s %10s\n", "rho out", "GPU", "C++", "delta" );

		//The probe reads along the +x axis, so it can only see source radii
		//that exist on that axis inside the frame, and the ramp itself is
		//capped at 1. Past either ceiling the fetch clamps and the reading
		//flattens out -- which looks exactly like a broken map if it is not
		//accounted for. A push in reaches that ceiling quickly, because
		//shrinking the background genuinely does want content from outside the
		//frame.
		const double edgeOfFrame = ( 0.5 * aspect ) / R;
		const double ceiling     = std::fmin( 1.0, edgeOfFrame ) - 0.01;

		double worst    = 0.0;
		int measured    = 0;
		int unreachable = 0;

		for( int step = 1; step <= 9; ++step )
		{
			const double rho = step * 0.1;

			//Where that radius falls along the +x axis through the centre.
			const double dx = rho * R / aspect;
			const int px    = static_cast< int >( std::lround( ( 0.5 + dx ) * width - 0.5 ) );
			const int py    = height / 2;
			if( px < 0 || px >= width )
			{
				//Not a failure: the probe reads along +x, and on a wide frame
				//the outer radii of the diagonal only exist near the corners.
				std::printf( "%8.2f %12s %12s %10s\n", rho, "-", "-", "off axis" );
				continue;
			}

			const double m       = magnification( disparity( radialBase( rho ), gamma, relief ),
			                                      sigma, anchorDelta );
			const double predRho = rho / ( m * overscan );

			if( predRho > ceiling )
			{
				std::printf( "%8.2f %12s %12.4f %10s\n", rho, "-", predRho, "off frame" );
				++unreachable;
				continue;
			}

			const size_t i      = ( static_cast< size_t >( py ) * width + px ) * 4;
			const double gpuRho = out[ i ] / 255.0;//brightness is the radius it sampled
			const double delta  = gpuRho - predRho;
			if( std::fabs( delta ) > worst )
				worst = std::fabs( delta );
			++measured;

			std::printf( "%8.2f %12.4f %12.4f %+10.4f\n", rho, gpuRho, predRho, delta );
		}

		if( measured == 0 )
		{
			std::printf( "\nnothing measurable: every radius wanted source from off the frame.\n"
			             "Pull back instead of pushing in, or raise Overscan.\n" );
			return shutdown( 1 );
		}
		if( unreachable > 0 )
			std::printf( "\n%d of %d radii wanted source from beyond the frame edge and were skipped.\n",
			             unreachable, measured + unreachable );

		//One 8-bit level is 0.0039, and the ramp is quantised twice over, so
		//anything under a couple of levels is the measurement rather than the
		//maths.
		const bool ok = worst < 0.012;
		std::printf( "\nworst |delta| = %.4f (%.1f levels)  %s\n",
		             worst, worst * 255.0, ok ? "OK" : "*** GPU AND C++ DISAGREE ***" );

		return shutdown( ok ? 0 : 1 );
	}

	//-----------------------------------------------------------------------
	// --anchor: the shot's defining claim.
	//
	// A dolly zoom is the move in which exactly one surface does not change
	// size. Everything else in this repository is an implementation checked
	// against another implementation; this is the model checked against what it
	// says about itself.
	//-----------------------------------------------------------------------
	if( anchor )
	{
		setParameter( "Depth", 0.0f );
		setParameter( "Quality", 0.0f );//one sample, so a held pixel is held exactly
		setParameter( "Centre X", 0.5f );
		setParameter( "Centre Y", 0.5f );
		setParameter( "Edges", 2.0f );
		setParameter( "Overscan", 0.5f );//1:1; an overscan moves everything, anchor included

		const double gamma      = gammaFromParam( getParameter( "Falloff" ) );
		const double relief     = reliefFromParam( getParameter( "Relief" ) );
		const double anchorLv   = getParameter( "Anchor" );
		const double anchorDelta = anchorDisparity( anchorLv, relief );
		const double ringRadius = radialAnchorRadius( anchorLv, gamma );

		//Measured on the radial ramp and NOT on the depth card. The obvious
		//test -- render the card at two dolly settings and difference the
		//pixels near the anchor radius -- does not work, and the way it fails
		//is worth knowing. The magnification is exactly 1 only exactly on the
		//ring; a band of any usable width contains radii where it is not, and
		//those pixels carry grid lines and ring markings, so a shift of a
		//fraction of a pixel there comes back as tens of levels. That number
		//is real, and it is measuring the card's contrast rather than the
		//model's claim.
		//
		//The ramp states the answer directly instead: the brightness of an
		//output pixel IS the radius the shader sampled from, so on the anchor
		//ring it must read back its own radius, at every dolly setting, with
		//no resampling in the argument at all.
		const std::vector< unsigned char > picture = buildRadialRamp( width, height, aspect );
		const GLuint sourceTexture                 = makeTexture( width, height, picture.data() );
		const GLuint targetTexture                 = makeTexture( width, height, nullptr );
		const GLuint targetFBO                     = makeFramebuffer( targetTexture );

		std::printf( "\nanchor level %.2f, gamma %.3f, relief %+.2f -> the ring at rho %.3f must hold\n",
		             anchorLv, gamma, relief, ringRadius );

		//An anchor at the very near end puts the ring on the optical axis,
		//which is one pixel and holds still whatever the effect does. There is
		//nothing to measure and saying so is better than passing.
		if( ringRadius < 0.05 )
		{
			std::printf( "\nSKIPPED -- that anchor is the optical axis itself, which cannot move.\n"
			             "Measure with an Anchor below 1 so the ring has a radius.\n" );
			return shutdown( 2 );
		}

		const double edgeOfFrame = ( 0.5 * aspect ) / R;
		const double ceiling     = std::fmin( 1.0, edgeOfFrame ) - 0.01;

		//Witnesses either side of the ring. Without these the test would pass
		//just as happily on a plugin that never moved anything at all.
		const double radii[]  = { 0.2 * ringRadius, ringRadius, 0.5 * ( ringRadius + ceiling ), 0.95 * ceiling };
		const double dollies[] = { 0.20, 0.35, 0.50, 0.65, 0.80 };
		constexpr int kRadii   = 4;
		constexpr int kDollies = 5;

		double lowest[ kRadii ], highest[ kRadii ];
		int readings[ kRadii ] = { 0, 0, 0, 0 };
		for( int r = 0; r < kRadii; ++r )
		{
			lowest[ r ]  = 1e9;
			highest[ r ] = -1e9;
		}

		for( int d = 0; d < kDollies; ++d )
		{
			setParameter( "Dolly", float( dollies[ d ] ) );
			if( !runPass( plugin, sourceTexture, targetFBO, width, height ) )
				return shutdown( 1 );
			const std::vector< unsigned char > out = readBack( targetFBO, width, height );

			const double sigma = sigmaFromParam( float( dollies[ d ] ) );

			for( int r = 0; r < kRadii; ++r )
			{
				const double rho = radii[ r ];
				const int px     = static_cast< int >( std::lround( ( 0.5 + rho * R / aspect ) * width - 0.5 ) );
				if( px < 0 || px >= width )
					continue;

				//Same gate as --probe: a source radius from beyond the frame
				//edge is clamped by the fetch, and a clamped reading is the
				//edge value rather than a measurement.
				const double m = magnification( disparity( radialBase( rho ), gamma, relief ),
				                                sigma, anchorDelta );
				if( rho / m > ceiling )
					continue;

				const size_t i      = ( static_cast< size_t >( height / 2 ) * width + px ) * 4;
				const double gpuRho = out[ i ] / 255.0;
				lowest[ r ]         = std::fmin( lowest[ r ], gpuRho );
				highest[ r ]        = std::fmax( highest[ r ], gpuRho );
				++readings[ r ];
			}
		}

		std::printf( "\nover dolly %.2f..%.2f, how far each radius travelled:\n",
		             dollies[ 0 ], dollies[ kDollies - 1 ] );
		std::printf( "%10s %8s %10s %10s\n", "rho", "reads", "travel", "" );

		double ringTravel = -1.0;
		double mostTravel = 0.0;

		for( int r = 0; r < kRadii; ++r )
		{
			if( readings[ r ] < 2 )
			{
				std::printf( "%10.3f %8d %10s %10s\n", radii[ r ], readings[ r ], "-", "off frame" );
				continue;
			}
			const double travel = highest[ r ] - lowest[ r ];
			const bool isRing   = ( r == 1 );
			std::printf( "%10.3f %8d %10.4f %10s\n", radii[ r ], readings[ r ], travel,
			             isRing ? "<- anchor" : "" );
			if( isRing )
				ringTravel = travel;
			else
				mostTravel = std::fmax( mostTravel, travel );
		}

		if( ringTravel < 0.0 )
		{
			std::printf( "\nINCONCLUSIVE -- the anchor ring itself was never measurable here.\n" );
			return shutdown( 2 );
		}

		//Two conditions, and the second is what stops this passing for the
		//wrong reason: the ring must hold, AND something else must not have.
		const bool held    = ringTravel < 0.008;//two 8-bit levels of the ramp
		const bool didMove = mostTravel > 0.05 && mostTravel > 10.0 * std::fmax( ringTravel, 0.001 );

		if( !didMove )
		{
			std::printf( "\nINCONCLUSIVE -- nothing else moved much either, so a still anchor\n"
			             "says nothing. Raise Relief, or widen the dolly range.\n" );
			return shutdown( 2 );
		}

		std::printf( "\nanchor travelled %.4f (%.1f levels) while the rest of the frame travelled %.4f\n",
		             ringTravel, ringTravel * 255.0, mostTravel );
		std::printf( "%s\n", held ? "OK -- the anchor held while the frame moved around it"
		                          : "*** THE ANCHOR MOVED ***" );
		return shutdown( held ? 0 : 1 );
	}

	//-----------------------------------------------------------------------
	// --depth: the iterative solve, against the same solve in double.
	//
	// The picture is a horizontal ramp, so its luma at any point IS its
	// horizontal position. That makes it both the depth field the shader reads
	// and the readout of where the shader read from: the output brightness at a
	// pixel states, as a number, the x it sampled.
	//-----------------------------------------------------------------------
	if( depth )
	{
		setParameter( "Depth", 1.0f );//Luma
		setParameter( "Quality", 0.0f );
		setParameter( "Centre X", 0.5f );
		setParameter( "Centre Y", 0.5f );
		setParameter( "Edges", 2.0f );
		//Zero, so the C++ mirror below can be the plain solve. Smoothing is a
		//5-tap in the shader and mirroring it here would be mirroring the
		//blur rather than the geometry, which is not what this measures.
		setParameter( "Smooth", 0.0f );

		const double sigma    = sigmaFromParam( getParameter( "Dolly" ) );
		const double relief   = reliefFromParam( getParameter( "Relief" ) );
		const double gamma    = gammaFromParam( getParameter( "Falloff" ) );
		const double overscan = overscanFromParam( getParameter( "Overscan" ) );
		const double anchorLv = getParameter( "Anchor" );
		const double anchorDelta = anchorDisparity( anchorLv, relief );

		const std::vector< unsigned char > picture = buildHorizontalRamp( width, height );
		const GLuint sourceTexture                 = makeTexture( width, height, picture.data() );
		const GLuint targetTexture                 = makeTexture( width, height, nullptr );
		const GLuint targetFBO                     = makeFramebuffer( targetTexture );

		if( !runPass( plugin, sourceTexture, targetFBO, width, height ) )
		{
			std::fprintf( stderr, "vgtest: ProcessOpenGL failed\n" );
			return shutdown( 1 );
		}

		const std::vector< unsigned char > out = readBack( targetFBO, width, height );

		std::printf( "\ndepth from luma, on a picture whose luma is its own x position\n" );
		std::printf( "sigma %+.4f  relief %+.2f  gamma %.3f  anchor %.2f  overscan %.3f\n",
		             sigma, relief, gamma, anchorLv, overscan );
		std::printf( "%8s %12s %12s %10s\n", "x out", "GPU", "C++", "delta" );

		//Along the middle row the offset from the axis is purely horizontal, so
		//the solved source point is a pure shift in x and the ramp reads it
		//back directly.
		const int py = height / 2;

		double worst = 0.0;
		int measured = 0;
		int skipped  = 0;

		for( int step = 1; step <= 9; ++step )
		{
			const double px = step * 0.1;
			const int x     = static_cast< int >( std::lround( px * width - 0.5 ) );
			if( x < 0 || x >= width )
				continue;

			//The same fixed-point iteration the shader runs, in double, with
			//the field read analytically instead of out of a texture. The two
			//are independent implementations of one definition; that is the
			//whole point of the comparison.
			const double offset = px - 0.5;
			double src          = px;
			for( int it = 0; it < kDepthIterations; ++it )
			{
				const double base = std::clamp( src, 0.0, 1.0 );//luma == x, clamped at the frame
				const double m    = magnification( disparity( base, gamma, relief ), sigma, anchorDelta );
				src               = 0.5 + offset / ( m * overscan );
			}

			//Outside the frame the shader's own fetch clamps, and the reading
			//would be the edge value rather than the solve.
			if( src < 0.01 || src > 0.99 )
			{
				std::printf( "%8.2f %12s %12.4f %10s\n", px, "-", src, "off frame" );
				++skipped;
				continue;
			}

			const size_t i     = ( static_cast< size_t >( py ) * width + x ) * 4;
			const double gpuX  = out[ i ] / 255.0;
			const double delta = gpuX - src;
			if( std::fabs( delta ) > worst )
				worst = std::fabs( delta );
			++measured;

			std::printf( "%8.2f %12.4f %12.4f %+10.4f\n", px, gpuX, src, delta );
		}

		if( measured == 0 )
		{
			std::printf( "\nnothing measurable: every sample came from outside the frame.\n" );
			return shutdown( 1 );
		}
		if( skipped > 0 )
			std::printf( "\n%d of %d positions solved to somewhere off the frame and were skipped.\n",
			             skipped, measured + skipped );

		//Looser than --probe: the shader's field comes from a bilinear fetch of
		//an 8-bit ramp and the C++ one is exact, so the two iterations start
		//from inputs that differ by up to half a level and the difference is
		//amplified by however steep the magnification is there.
		const bool ok = worst < 0.02;
		std::printf( "\nworst |delta| = %.4f (%.1f levels)  %s\n",
		             worst, worst * 255.0, ok ? "OK" : "*** THE SOLVE DISAGREES ***" );

		return shutdown( ok ? 0 : 1 );
	}

	//-----------------------------------------------------------------------
	// Plain render.
	//-----------------------------------------------------------------------
	const std::vector< unsigned char > picture =
		ramp ? buildHorizontalRamp( width, height ) : buildDepthCard( width, height, aspect );

	const GLuint sourceTexture = makeTexture( width, height, picture.data() );
	const GLuint targetTexture = makeTexture( width, height, nullptr );
	const GLuint targetFBO     = makeFramebuffer( targetTexture );

	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		std::fprintf( stderr, "vgtest: output framebuffer is incomplete\n" );
		return shutdown( 1 );
	}

	if( !runPass( plugin, sourceTexture, targetFBO, width, height ) )
	{
		std::fprintf( stderr, "vgtest: ProcessOpenGL failed\n" );
		return shutdown( 1 );
	}

	std::vector< unsigned char > out = readBack( targetFBO, width, height );

	const GLenum error = glGetError();
	if( error != GL_NO_ERROR )
		std::fprintf( stderr, "vgtest: GL error 0x%04x during render\n", error );

	//The plugin outputs premultiplied alpha, so the colour is already the
	//over-black composite. Flattening is just a matter of forcing alpha opaque.
	if( !keepAlpha )
	{
		for( size_t i = 3; i < out.size(); i += 4 )
			out[ i ] = 255;
	}

	if( measure )
	{
		double sum[ 3 ] = { 0.0, 0.0, 0.0 };
		size_t counted  = 0;
		for( int y = height / 4; y < height * 3 / 4; ++y )
		{
			for( int x = width / 4; x < width * 3 / 4; ++x )
			{
				const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
				sum[ 0 ] += out[ i + 0 ];
				sum[ 1 ] += out[ i + 1 ];
				sum[ 2 ] += out[ i + 2 ];
				++counted;
			}
		}
		const double n = static_cast< double >( counted ) * 255.0;
		std::printf( "mean RGB %.4f %.4f %.4f\n", sum[ 0 ] / n, sum[ 1 ] / n, sum[ 2 ] / n );
	}

	if( !writePng( outputPath, width, height, out ) )
	{
		std::fprintf( stderr, "vgtest: could not write %s\n", outputPath.c_str() );
		return shutdown( 1 );
	}

	std::printf( "wrote %s (%dx%d)\n", outputPath.c_str(), width, height );

	glDeleteFramebuffers( 1, &targetFBO );
	glDeleteTextures( 1, &targetTexture );
	glDeleteTextures( 1, &sourceTexture );
	return shutdown( 0 );
}
