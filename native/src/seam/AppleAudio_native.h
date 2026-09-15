// AppleAudio_native.h — the two Apple-framework jobs the audio backend cannot do itself, behind a
// plain-C wall. Phase N Stage 2 (see SimpleAudioEngine_native.mm for the design this serves).
//
// *** WHY THIS IS A SEPARATE TRANSLATION UNIT AND NOT JUST MORE #imports ***
// `web/src/shim/foundation/framework/` deliberately SHADOWS <AudioToolbox/...> and
// <AVFoundation/...> with this port's own type-only stubs, on every target including native macOS.
// Read that AudioToolbox.h's header comment before touching this: the reason is measured, not
// stylistic — the real headers transitively pull in CoreServices, and CarbonCore/AIFF.h defines a
// `ChunkHeader`, which collides ~5 includes deep with the engine's own on-disk chunk record in
// almost every engine TU. Stage 1 renamed the engine's struct, but the shadow is still what keeps
// CoreServices out of ~80 translation units, and unshadowing it globally to reach two APIs would
// be a large change with a wide blast radius for a small gain.
//
// So this file's implementation (AppleAudio_native.mm) is compiled as its own CMake object
// library WITHOUT the shim's framework/ directory on its include path, and it exports only C
// primitives. Nothing Apple-shaped crosses this header.
//
// It is also, not coincidentally, exactly the surface the Linux leg has to reimplement: a decoder
// (the plan's portable CAF decoder plus an mp3 one) and a streaming player. Everything else in
// the audio backend — the voice pool, the clip cache, the five-channel model, the volume split,
// the façade — is already portable.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- decode: any format the platform reads (here: .caf incl. IMA4, .mp3, .wav, .aif) ---------
// On success returns 1 and fills the out-params; `*pcm` is malloc'd interleaved signed 16-bit and
// belongs to the caller (free with eden_apple_audio_free). On failure returns 0 and touches
// nothing.
int  eden_apple_audio_decode(const char* path, int16_t** pcm, int* frames, int* channels, int* rate);
void eden_apple_audio_free(int16_t* pcm);

// --- stream: one long file, played without being decoded into memory -------------------------
// An opaque handle. Create returns NULL if the file cannot be opened.
typedef struct EdenApplePlayer EdenApplePlayer;

EdenApplePlayer* eden_apple_player_create(const char* path);
void  eden_apple_player_destroy(EdenApplePlayer* p);
void  eden_apple_player_play(EdenApplePlayer* p);
void  eden_apple_player_pause(EdenApplePlayer* p);
void  eden_apple_player_stop(EdenApplePlayer* p);          // stop and rewind to 0
void  eden_apple_player_rewind(EdenApplePlayer* p);
void  eden_apple_player_set_loop(EdenApplePlayer* p, int loop);
void  eden_apple_player_set_volume(EdenApplePlayer* p, float v);
int   eden_apple_player_is_playing(EdenApplePlayer* p);
// Non-NULL only while the last create failed; for the one error message worth printing.
const char* eden_apple_audio_last_error(void);

#ifdef __cplusplus
}
#endif
