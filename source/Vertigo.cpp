#include "Vertigo.h"

#include "Diag.h"
#include "Dolly.h"
#include "Presets.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace vertigo;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Vertigo >,                      // Create method
	"VG01",                                        // Plugin unique ID of maximum length 4.
	"Vertigo",                                     // Plugin name
	2,                                             // API major version number
	1,                                             // API minor version number
	0,                                             // Plugin major version number
	1,                                             // Plugin minor version number
	FF_EFFECT,                                     // Plugin type
	"Dolly zoom: the shot where the world moves",  // Plugin description
	"Vertigo FFGL effect"                          // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

Vertigo::Vertigo()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	// The About block's slots are never assigned below -- they are a text line
	// and four buttons, none of which holds a value -- and GetFloatParameter
	// hands whatever is in the array straight to the host. Zeroing first means
	// it hands back 0 rather than whatever was on the stack.
	for( float& value : params )
		value = 0.0f;

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// Set to a real shot rather than to nothing. An effect that does nothing
	// until three sliders have been found is an effect nobody discovers is any
	// good -- and the null here is Dolly at centre, which is one drag away.
	//
	// The default is a PULL BACK and not a push in, which is not a matter of
	// taste. A push in shrinks the background, and a shrunk background is read
	// from beyond the frame edge, so the first thing a new user would see is
	// whatever Edges does at the border. A pull back magnifies the background
	// and reads from inside the picture, so it cannot produce an edge artefact
	// at all. Both are the shot; only one of them explains itself.
	//---------------------------------------------------------------------
	params[ PT_DOLLY ]    = 0.30f;//pull back, parallax ratio about 1:1.7
	params[ PT_RELIEF ]   = 0.75f;//+1: a subject on the axis, the world behind it
	params[ PT_ANCHOR ]   = 1.00f;//lock the nearest surface, which in Radial is the axis

	params[ PT_DEPTH ]    = 0.0f; //Radial: invents its field, so it works on any clip
	params[ PT_FALLOFF ]  = 0.5f; //gamma 1
	params[ PT_SMOOTH ]   = 0.25f;//no effect on Radial; a sane starting point for a map

	params[ PT_CENTRE_X ] = 0.5f; //0.5 is on axis
	params[ PT_CENTRE_Y ] = 0.5f;
	params[ PT_OVERSCAN ] = 0.5f; //0.5 is 1:1

	params[ PT_EDGES ]    = 2.0f; //clamp: the picture continues rather than stopping
	params[ PT_QUALITY ]  = 1.0f; //good

	params[ PT_PRESET ]   = 0.0f; //Custom: the sliders are the truth

	// The host has not said anything yet, so its last word is the defaults it
	// is about to be told about.
	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every parameter is a plain 0..1 float even where it stands for stops or
	// a gamma. SetParamRange exists, but SetParamInfo clamps an
	// FF_TYPE_STANDARD default into 0..1 *before* a range can be attached
	// (SDK b1afaf9), so a parameter declared in stops cannot declare a default
	// in stops. The conversions live in Dolly.cpp.
	//---------------------------------------------------------------------
	SetParamInfof( PT_DOLLY, "Dolly", FF_TYPE_STANDARD );
	SetParamInfof( PT_RELIEF, "Relief", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANCHOR, "Anchor", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_DEPTH, "Depth", 3, params[ PT_DEPTH ] );
	SetParamElementInfo( PT_DEPTH, 0, "Radial", 0.0f );
	SetParamElementInfo( PT_DEPTH, 1, "Luma", 1.0f );
	SetParamElementInfo( PT_DEPTH, 2, "Alpha", 2.0f );

	SetParamInfof( PT_FALLOFF, "Falloff", FF_TYPE_STANDARD );
	SetParamInfof( PT_SMOOTH, "Smooth", FF_TYPE_STANDARD );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, "" );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	SetParamInfof( PT_CENTRE_X, "Centre X", FF_TYPE_STANDARD );
	SetParamInfof( PT_CENTRE_Y, "Centre Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_OVERSCAN, "Overscan", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_EDGES, "Edges", 5, params[ PT_EDGES ] );
	SetParamElementInfo( PT_EDGES, 0, "Transparent", 0.0f );
	SetParamElementInfo( PT_EDGES, 1, "Black", 1.0f );
	SetParamElementInfo( PT_EDGES, 2, "Clamp", 2.0f );
	SetParamElementInfo( PT_EDGES, 3, "Mirror", 3.0f );
	SetParamElementInfo( PT_EDGES, 4, "Wrap", 4.0f );

	SetOptionParamInfo( PT_QUALITY, "Quality", 3, params[ PT_QUALITY ] );
	SetParamElementInfo( PT_QUALITY, 0, "Fast", 0.0f );
	SetParamElementInfo( PT_QUALITY, 1, "Good", 1.0f );
	SetParamElementInfo( PT_QUALITY, 2, "Best", 2.0f );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the shot parameters and raises value events so the
	// host re-reads the sliders. Editing a covered slider flips back to Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + vertigo::presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < vertigo::presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, vertigo::presets::kPresets[ i ].name, float( 1 + i ) );

	//Eleven parameters is well past the point where an ungrouped list in
	//somebody else's inspector stops being readable.
	SetParamGroup( PT_DOLLY, "Shot" );
	SetParamGroup( PT_RELIEF, "Shot" );
	SetParamGroup( PT_ANCHOR, "Shot" );

	SetParamGroup( PT_DEPTH, "Depth" );
	SetParamGroup( PT_FALLOFF, "Depth" );
	SetParamGroup( PT_SMOOTH, "Depth" );

	SetParamGroup( PT_CENTRE_X, "Frame" );
	SetParamGroup( PT_CENTRE_Y, "Frame" );
	SetParamGroup( PT_OVERSCAN, "Frame" );

	SetParamGroup( PT_EDGES, "Output" );
	SetParamGroup( PT_QUALITY, "Output" );

	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created Vertigo effect" );

	vertigo::diag::init();
}

FFResult Vertigo::InitGL( const FFGLViewportStruct* vp )
{
	// The GL strings first, and unconditionally: when a shader will not compile
	// it is almost always the driver or the GL version, and knowing which
	// machine reported what is most of the diagnosis.
	vertigo::diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	                     + " renderer=" + glStringOrUnknown( GL_RENDERER )
	                     + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !shader.Compile( kVertexShader, kFragmentShader ) )
	{
		// Returning FF_FAIL here is invisible to the operator: the effect
		// simply does nothing in Resolume, with no message anywhere. This line
		// is the only record that it was the shader.
		vertigo::diag::error( "shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Vertigo: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		vertigo::diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Vertigo: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	vertigo::diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult Vertigo::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//FFGL requires the context to be left in a default state on return, so use
	//the scoped bindings for everything touched here.
	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	ScopedSamplerActivation activateSampler( 0 );
	Scoped2DTextureBinding textureBinding( picture.Handle );

	shader.Set( "InputTexture", 0 );

	//The input texture can be larger than the picture inside it, and its
	//dimensions can change from frame to frame, so both of these are uniforms
	//rather than anything baked into the geometry.
	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
	shader.Set( "MaxUV", maxCoords.s, maxCoords.t );

	//Half an input texel, expressed in picture space. The shader keeps every
	//fetch this far inside the picture so a linear tap at the edge cannot reach
	//into the texture's undrawn padding.
	shader.Set( "HalfTexel",
	            0.5f / static_cast< float >( picture.Width ),
	            0.5f / static_cast< float >( picture.Height ) );

	const float aspect = static_cast< float >( picture.Width ) / static_cast< float >( picture.Height );
	shader.Set( "Aspect", aspect );

	// effective(), not params[], for everything a preset can cover: the host
	// may still be pushing its own idea of those values down at us.
	const double relief = reliefFromParam( effective( PT_RELIEF ) );
	shader.Set( "Sigma", static_cast< float >( sigmaFromParam( effective( PT_DOLLY ) ) ) );
	shader.Set( "Relief", static_cast< float >( relief ) );
	//The anchor goes to the GPU already through the relief map, so the shader
	//never has to know that the two are the same function.
	shader.Set( "AnchorDelta", static_cast< float >( anchorDisparity( effective( PT_ANCHOR ), relief ) ) );

	shader.Set( "DepthMode", params[ PT_DEPTH ] );
	shader.Set( "Gamma", static_cast< float >( gammaFromParam( effective( PT_FALLOFF ) ) ) );
	shader.Set( "DepthSmooth", static_cast< float >( smoothFromParam( effective( PT_SMOOTH ) ) ) );

	shader.Set( "Centre", params[ PT_CENTRE_X ] - 0.5f, params[ PT_CENTRE_Y ] - 0.5f );
	shader.Set( "Overscan", static_cast< float >( overscanFromParam( params[ PT_OVERSCAN ] ) ) );

	shader.Set( "EdgeMode", effective( PT_EDGES ) );
	shader.Set( "Taps", static_cast< float >( tapsFromParam( params[ PT_QUALITY ] ) ) );

	quad.Draw();

	return FF_SUCCESS;
}

FFResult Vertigo::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();

	return FF_SUCCESS;
}

FFResult Vertigo::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = int( std::lround( value ) );
		if( chosen != int( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	//Deliberately not logged. A parameter change is not a diagnostic event: the
	//host already shows the value, and an operator animating a slider would put
	//a line in the log every frame. This log exists for the shader that will not
	//compile, and it is worth nothing if it is buried.
	const float lastFromHost = hostValues[ index ];
	hostValues[ index ]      = value;
	params[ index ]          = value;

	// A slider moved while a preset is active means the operator has taken
	// over, and the dropdown falls back to Custom. Two things that are NOT an
	// operator moving a slider arrive through this same call, and reading
	// either as an edit is what makes a preset look like it will not stick:
	//
	//   - a host that honours the value events raised by applyPreset reads the
	//     new values and hands them straight back;
	//   - a host that does not simply carries on pushing the values it still
	//     believes in, which are the ones from before the preset.
	//
	// So neither is judged by whether the value changed -- both changed
	// something -- but by what the value IS. Matching the preset is the first
	// case, matching what the host last sent is the second, and anything else
	// is a person turning a knob.
	const int active = int( std::lround( params[ PT_PRESET ] ) );
	if( active <= 0 )
		return FF_SUCCESS;

	const float covered = presetValue( active, index );
	if( covered < 0.0f )
		return FF_SUCCESS;//not a parameter this preset has an opinion about

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters as anything shorter than a float -- or that round-trips
	// them through a UI, a MIDI value or a saved composition -- hands back a
	// number near ours rather than ours, and 1e-4 was tight enough to read
	// that as an edit.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - covered ) <= kSame )
		return FF_SUCCESS;

	if( std::fabs( value - lastFromHost ) <= kSame )
		return FF_SUCCESS;

	// Logged, unlike an ordinary parameter change: this one is a state change
	// an operator can be surprised by, it happens once rather than per frame,
	// and issue #2 needed a code read precisely because nothing said it had
	// happened.
	vertigo::diag::info( "preset dropped to Custom: parameter " + std::to_string( index )
	            + " moved to " + std::to_string( value )
	            + " (preset says " + std::to_string( covered )
	            + ", host last said " + std::to_string( lastFromHost ) + ")" );

	params[ PT_PRESET ] = 0.0f;
	RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );

	return FF_SUCCESS;
}

float Vertigo::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex <= 0 || presetIndex > vertigo::presets::kCount )
		return -1.0f;

	const vertigo::presets::Preset& preset = vertigo::presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < vertigo::presets::kParamCount; ++j )
		if( kPresetParamIDs[ j ] == id )
			return preset.v[ j ];

	return -1.0f;
}

float Vertigo::effective( unsigned int id ) const
{
	const float fromPreset = presetValue( int( std::lround( params[ PT_PRESET ] ) ), id );
	return fromPreset >= 0.0f ? fromPreset : params[ id ];
}

void Vertigo::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = float( presetIndex );

	if( presetIndex <= 0 || presetIndex > vertigo::presets::kCount )
	{
		vertigo::diag::info( "preset: Custom" );
		return;//Custom: the sliders keep whatever they said
	}

	vertigo::diag::info( std::string( "preset: " ) + vertigo::presets::kPresets[ presetIndex - 1 ].name );

	// hostValues is deliberately NOT written here. It is the record of what the
	// host has said, and the host has not said anything -- if this wrote to it,
	// the host's next restatement of its own values would read as an operator
	// edit and drop the preset on the spot.

	const vertigo::presets::Preset& preset = vertigo::presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < vertigo::presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Vertigo::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	// The effective value, so a host that re-reads its sliders after a preset
	// -- on a value event, or when saving the composition -- gets the numbers
	// the effect is actually rendering with rather than the ones it happened
	// to send last.
	return effective( index );
}

FFResult Vertigo::SetTextParameter( unsigned int index, const char* )
{
	// The About text is generated on read and never stored — but a set of it
	// must SUCCEED. The SDK's instantiateGL pushes every parameter's default
	// into a fresh instance and destroys it on the first FF_FAIL, and the
	// base SetTextParameter returns FF_FAIL — so without this the plugin
	// fails FF_INSTANTIATE_GL in the host, with no message anywhere.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, nullptr );
}

char* Vertigo::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}
