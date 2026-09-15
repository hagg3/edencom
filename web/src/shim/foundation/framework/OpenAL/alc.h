// <OpenAL/alc.h> — trampoline to the target's OpenAL context API, or to nothing. See OpenAL/al.h
// for the whole reasoning, including the measurement that says nothing in this port calls OpenAL
// on any target.
#ifndef EDEN_SHIM_OPENAL_ALC_H
#define EDEN_SHIM_OPENAL_ALC_H

#if defined(__APPLE__)
#include_next <OpenAL/alc.h>

#elif defined(__EMSCRIPTEN__)
#include <AL/alc.h>

#else
// Phase N Stage 3.2 — the two names Classes/Sound.h actually needs, as opaque structs. Real
// OpenAL declares them exactly this way (incomplete types you only ever hold a pointer to), so
// anything that compiles against this stub also compiles against the real header.
typedef struct ALCdevice_struct  ALCdevice;
typedef struct ALCcontext_struct ALCcontext;
#endif

#endif
