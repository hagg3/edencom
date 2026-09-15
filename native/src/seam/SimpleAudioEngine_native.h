// SimpleAudioEngine_native.h — the three host hooks the audio backend needs from the frame loop.
// Everything else about it is reached through CocosDenshion::SimpleAudioEngine, which the engine
// already calls; see SimpleAudioEngine_native.mm's header for the design and for what it does
// NOT share with the web twin.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// OFF for the scripted/headless modes. Must be called before the engine's first sound, i.e.
// before eden_seam_main(). Same role eden_native_gl_set_headless() plays for the renderer.
void eden_native_audio_set_enabled(int on);

// Once per frame: reaps drained voices and re-feeds looping ones. Cheap when nothing is playing
// (an early return on "no device"), so the frame loop can call it unconditionally.
void eden_native_audio_tick(void);

// Closes the device and drops every voice. Reached from SimpleAudioEngine::end() as well.
void eden_native_audio_shutdown(void);

// --- probes, for --audio-selftest --------------------------------------------------------------
// Decode `name` (through the same cache a real play uses) and report what came out. Returns 1 on
// success. This is the only way to assert "the decoder handled this format" without a listener,
// which is the whole reason --audio-selftest can exist as a regression test rather than a demo.
int eden_native_audio_probe(const char* name, int* frames, int* channels, int* rate);

// How many effect voices are currently bound to the device. A sound that decoded but never
// reached the mixer reports 0 here while everything else looks healthy.
int eden_native_audio_voice_count(void);

// The volume a streaming channel is ACTUALLY at (userVolume x engineFade), or -1 if nothing is
// loaded on it, and the file it is playing. A channel that is "playing" at volume 0 is the one
// ambience failure mode that looks identical to a channel that never started.
float eden_native_audio_channel_volume(int ch);
const char* eden_native_audio_channel_name(int ch);

#ifdef __cplusplus
}
#endif
