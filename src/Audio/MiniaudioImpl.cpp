// The single translation unit that compiles miniaudio and stb_vorbis.
//
// Nothing else in the mod may define MINIAUDIO_IMPLEMENTATION. Other files
// include "miniaudio.h" for the declarations only, the same arrangement
// Branding.cpp uses for stb_image.
//
// Ordering matters: miniaudio only compiles its Vorbis decoder when
// STB_VORBIS_INCLUDE_STB_VORBIS_H is already defined (see the guard at the top
// of its vorbis section), so stb_vorbis has to be included first. Without this
// include .ogg silently stops being a supported format.

// stb_vorbis is a .c file written for C89. These are the warnings it trips
// when compiled as C++ in this project's warning configuration; none of them
// indicate a real defect and none are ours to fix.
#pragma warning(push)
#pragma warning(disable: 4244) // conversion, possible loss of data
#pragma warning(disable: 4245) // signed/unsigned mismatch
#pragma warning(disable: 4456) // declaration hides previous local
#pragma warning(disable: 4457) // declaration hides function parameter
#pragma warning(disable: 4701) // potentially uninitialized local
#include "stb_vorbis.c"
#pragma warning(pop)

// stb_vorbis defines L, C and R for its channel-mapping table and never undefines
// them. Left in place they rewrite identifiers inside the Windows SDK headers
// that miniaudio includes below, and the build dies a hundred lines deep in
// threadpoolapiset.h with errors that name neither file. Do not remove these.
#undef L
#undef C
#undef R

// We use miniaudio for exactly two things: decoding user files to PCM, and
// playing a preview out of the default output device. Everything else it can do
// is switched off — this header is 4 MB and the unused subsystems are pure
// compile time.
#define MA_NO_ENCODING          // we never write audio files; the XACT converter does that
#define MA_NO_GENERATION        // no waveform/noise generators
#define MA_NO_ENGINE            // no high-level engine
#define MA_NO_NODE_GRAPH        // no node graph (implied by MA_NO_ENGINE, stated for clarity)
#define MA_NO_RESOURCE_MANAGER  // we manage our own buffers

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
