// ---------------------------------------------------------------------------------------------
// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): this one stays THIS PORT'S OWN on
// every target, including native macOS where a real framework of the same name exists. Two
// measured reasons, both worth knowing before "improving" it into an #include_next:
//   * <AudioToolbox/...> and <AVFoundation/...> transitively pull in CoreServices, whose
//     CarbonCore/AIFF.h defines `ChunkHeader` — and so does Classes/FileManager.h, the engine's
//     on-disk chunk record. That is a typedef redefinition in every engine TU that reaches the
//     audio headers, i.e. most of them, reported ~5 includes deep from anything this project owns.
//   * <QuartzCore/QuartzCore.h> on macOS has no CAEAGLLayer, which is the only thing Classes/
//     Util.mm reaches it for.
// Native audio is stubbed for Stage 1 (native/src/seam/seam_link_stubs_native.mm) and Stage 2 gives
// it a real backend behind SimpleAudioEngine.h's own ~13-call contract — so nothing is lost by
// these headers only having to PARSE here.
// <CoreAudio/CoreAudioTypes.h> — Apple's scalar typedefs plus the one audio struct this engine
// names. Force-included into most of the tree via Classes/Sound.h (World.h → Terrain.h →
// Resources.h → Graphics.h → Sound.h), so it has to parse everywhere, not just in audio files.
//
// This is a TYPE shim, not a functional one: Stage P5 decides whether audio rides Emscripten's
// OpenAL or a hand-written Web Audio layer, and nothing here prejudges that. See
// AudioToolbox/AudioFile.h for the function side.
//
// Widths target wasm32, matching the original 32-bit armv7 build (same reasoning as
// objc/objc.h's NSInteger note — these types reach on-disk and memcpy'd data).
#ifndef EDEN_SHIM_COREAUDIO_COREAUDIOTYPES_H
#define EDEN_SHIM_COREAUDIO_COREAUDIOTYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Apple's <MacTypes.h> names, which CoreAudio/AudioToolbox headers spell everywhere and which
// Classes/Sound.m and Classes/CocosDenshion.m use directly in their own declarations.
//
// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): on a target with a real
// <MacTypes.h> these must DEFER, not duplicate. `noErr` is the one that actually breaks: it is a
// MACRO here and an ENUMERATOR in Apple's MacTypes.h, so our `#define noErr 0` turns
// `enum { noErr = 0 };` into `enum { 0 = 0 };` and the build dies with a bare
// "expected identifier" inside MacTypes.h. Real CoreFoundation is reached from this same header
// on that target, so MacTypes is already in scope by the time anything here is used.
#if defined(EDEN_USE_SYSTEM_FOUNDATION)
#include <MacTypes.h>
#else
#ifndef EDEN_SHIM_MACTYPES_DEFINED
#define EDEN_SHIM_MACTYPES_DEFINED
typedef uint8_t  UInt8;
typedef int8_t   SInt8;
typedef uint16_t UInt16;
typedef int16_t  SInt16;
typedef uint32_t UInt32;
typedef int32_t  SInt32;
typedef uint64_t UInt64;
typedef int64_t  SInt64;
typedef float    Float32;
typedef double   Float64;
typedef int32_t  OSStatus;
typedef uint32_t OSType;
typedef unsigned char Boolean;

// `noErr` is the success value every OSStatus check in this tree compares against.
#define noErr 0
#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION

// Named by CocosDenshion/CDOpenALSupport when describing decoded PCM. Field order matches
// Apple's — these structs are passed to AudioFile* by address, so if P5 ever backs those with a
// real decoder the layout is already the expected one.
typedef struct AudioStreamBasicDescription {
  Float64 mSampleRate;
  UInt32  mFormatID;
  UInt32  mFormatFlags;
  UInt32  mBytesPerPacket;
  UInt32  mFramesPerPacket;
  UInt32  mBytesPerFrame;
  UInt32  mChannelsPerFrame;
  UInt32  mBitsPerChannel;
  UInt32  mReserved;
} AudioStreamBasicDescription;

typedef struct AudioBuffer {
  UInt32 mNumberChannels;
  UInt32 mDataByteSize;
  void  *mData;
} AudioBuffer;

typedef struct AudioBufferList {
  UInt32      mNumberBuffers;
  AudioBuffer mBuffers[1];
} AudioBufferList;

enum {
  kAudioFormatLinearPCM = 'lpcm'
};

enum {
  kAudioFormatFlagIsFloat          = (1 << 0),
  kAudioFormatFlagIsBigEndian      = (1 << 1),
  kAudioFormatFlagIsSignedInteger  = (1 << 2),
  kAudioFormatFlagIsPacked         = (1 << 3),
  kAudioFormatFlagIsNonInterleaved = (1 << 5)
};

#ifdef __cplusplus
}
#endif

#endif
