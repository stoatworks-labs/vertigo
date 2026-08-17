#pragma once

/**
    The one shader pass.

    Vertigo is a resampler: every output pixel works out how far away the scene
    is at that point, asks the dolly-zoom magnification where that surface came
    from before the camera moved, and fetches it. Nothing accumulates between
    frames and no pixel depends on any other, so this is a single pass with no
    intermediate buffers at all.

    Five things in it are worth knowing about before editing:

    - **The dolly maths is a mirror of Dolly.cpp.** Two copies of one formula is
      a liability, and the answer to it is `vgtest --probe`, which measures what
      the GPU actually did against what the C++ predicts.

    - **The Radial field is evaluated in OUTPUT space and the sampled fields in
      SOURCE space, and that is the whole difference between them.** An invented
      field can be asked about the pixel being written, so the source point is a
      closed form and there is exactly one texture fetch. A field carried by the
      picture can only be asked about the pixel being read -- which is the thing
      being solved for. `solveSourcePoint()` iterates a fixed three times.

    - **Everything happens in picture space, 0..1, and MaxUV is applied at the
      last possible moment.** FFGL can hand over a texture larger than the
      picture in it. This effect samples wherever it likes and routinely wants
      source from beyond the frame edge, so treating the texture edge as the
      picture edge fetches undrawn padding.

    - **The supersample grid is spread using dFdx/dFdy of the output UV**, not a
      resolution uniform, so it is right whatever the host renders at.

    - **Uniform names have to match the C++ exactly.** A mismatch is not an
      error anywhere: glGetUniformLocation returns -1 and glUniform on -1 is a
      documented no-op, so the control is simply dead. `tools/sweep.py` is the
      only thing that catches it.
*/
namespace vertigo
{

/// Passes UV through untouched, in 0..1 picture space. Deliberately *not*
/// pre-multiplied by MaxUV the way a simple filter's vertex shader would --
/// the fragment shader needs to do its geometry in picture space and scale
/// only the final fetch.
extern const char* const kVertexShader;

extern const char* const kFragmentShader;

} // namespace vertigo
