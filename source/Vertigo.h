#pragma once

#include <FFGLSDK.h>

#include "Presets.h"
#include "StoatworksAboutParams.h"

/**
    Vertigo -- the dolly zoom, as an effect for Resolume.

    The shot is one camera move and one lens move arranged to cancel: the
    camera tracks along its own axis while the focal length changes to hold one
    surface at exactly the size it already was. That surface stays put and
    everything at every other distance moves, which is why it reads as the
    ground giving way rather than as a zoom. Hitchcock's cameraman worked it out
    for a stairwell in 1958 and it has been the shorthand for vertigo ever
    since.

    It is a depth effect, and a video clip has no depth -- so the honest part of
    this plugin is where the depth comes from. There are two answers here and
    they are different in kind:

    - **Radial** invents a field: near on the optical axis, falling away to the
      frame corners, or the reverse. Nothing is read out of the picture, so the
      geometry is a closed form with no failure cases, no holes, and no
      dependence on what the clip happens to contain. It works on any footage
      at all, and what it produces is the *look* of the shot rather than the
      shot.
    - **Luma** and **Alpha** read a field out of the clip. Given real depth --
      a rendered depth pass, a matte, an AI depth map baked into a channel --
      this is the actual reprojection, and the picture comes apart in depth the
      way the real move does. Given an ordinary clip it turns brightness into
      geometry, which is either a mistake or an effect, depending on the clip.

    Everything else is common to both, because both end up as a disparity in
    0..1 and the maths downstream of that does not care where it came from. See
    Dolly.h, which is where the model is written down.

    Three properties are load-bearing rather than decorative:

    - **The anchor does not move**, at any dolly setting. That is the whole
      definition of the shot, and `vgtest --anchor` measures it directly.
    - **Relief at flat is the identity.** A scene with no depth variation
      cannot be dolly-zoomed; the maths says so without being told, and the
      null for the whole effect is either that or Dolly at centre.
    - **The two dolly directions are reciprocal**, so equal travel either side
      of centre is equal strength in opposite senses.

    There is deliberately no wet/dry mix. Cross-fading two geometries
    double-exposes the picture rather than easing between them.

    See AGENTS.md for the traps.
*/
class Vertigo : public CFFGLPlugin
{
public:
	Vertigo();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

private:
	/// The order the host shows them in: the shot, then where the depth comes
	/// from, then how the frame is fitted to the result.
	enum ParamID : FFUInt32
	{
		//Shot
		PT_DOLLY,
		PT_RELIEF,
		PT_ANCHOR,

		//Depth
		PT_DEPTH,
		PT_FALLOFF,
		PT_SMOOTH,

		//Frame
		PT_CENTRE_X,
		PT_CENTRE_Y,
		PT_OVERSCAN,

		//Output
		PT_EDGES,
		PT_QUALITY,

		//Preset. Declared after the real controls so their IDs — which a saved
		//composition refers to — do not shift under existing users.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ vertigo::presets::kParamCount ] = {
		PT_DOLLY, PT_RELIEF, PT_ANCHOR, PT_FALLOFF, PT_SMOOTH, PT_EDGES
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	void applyPreset( int presetIndex );

	/// The active preset's value for `id`, or -1 if this preset does not cover
	/// it. Preset values are all 0..1, so a negative is an unambiguous "no".
	float presetValue( int presetIndex, unsigned int id ) const;

	/// What the effect should actually render `id` with. A parameter a live
	/// preset covers takes the preset's value; everything else takes the
	/// host's. See the note on hostValues for why the preset cannot simply
	/// overwrite params[] and expect it to stay overwritten.
	float effective( unsigned int id ) const;

	/// What the HOST last sent for each parameter, which is not the same thing
	/// as what the plugin is rendering with.
	///
	/// FFGL's host owns parameter state. It may push its own values back down
	/// at any time, and nothing obliges it to act on the value events a plugin
	/// raises when it changes one itself. So a plugin that applies a preset by
	/// writing params[] and trusting the host to follow is relying on
	/// behaviour the specification does not promise -- and when the host
	/// instead restates the values it still believes in, the "an edit means
	/// the operator has taken over" rule sees a change and drops straight back
	/// to Custom. That is issue #2: the preset appears not to stick.
	///
	/// Keeping the host's own last word separately is what tells the two apart.
	/// An operator's edit differs from what the host last sent; the host
	/// restating itself does not.
	float hostValues[ PT_COUNT ];

	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	float params[ PT_COUNT ];

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
