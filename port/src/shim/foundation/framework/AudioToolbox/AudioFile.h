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
// <AudioToolbox/AudioFile.h> — declarations only, for the handful of AudioFile entry points this
// engine calls (Classes/Sound.m loads .caf effect data; CDOpenALSupport.m loads .wav/.caf).
//
// *** THESE ARE STAGE P5 STUBS: the implementations in src/shim/audio/audiotoolbox_stub.cpp all
// FAIL, returning a non-zero OSStatus. *** That is deliberate and matches the engine's own error
// handling (`if (result != 0) NSLog(@"cannot load effect: %@", fileName)`) — audio is simply
// absent until P5 decides between Emscripten's OpenAL and a Web Audio layer. Making these link
// is what lets the P1 headless-link milestone happen without dragging a CAF/WAV decoder in first.
#ifndef EDEN_SHIM_AUDIOTOOLBOX_AUDIOFILE_H
#define EDEN_SHIM_AUDIOTOOLBOX_AUDIOFILE_H

#include <CoreAudio/CoreAudioTypes.h>
#include <CoreFoundation/CoreFoundation.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OpaqueAudioFileID *AudioFileID;
typedef UInt32 AudioFilePropertyID;

enum {
  kAudioFileReadPermission      = 0x01,
  kAudioFileWritePermission     = 0x02,
  kAudioFileReadWritePermission = 0x03
};

enum {
  kAudioFilePropertyDataFormat            = 'dfmt',
  kAudioFilePropertyAudioDataByteCount    = 'bcnt',
  kAudioFilePropertyAudioDataPacketCount  = 'pcnt',
  kAudioFilePropertyMaximumPacketSize     = 'psze'
};

// `fsRdPerm` is the pre-iOS spelling; Sound.m still has it behind its `#if TARGET_OS_IPHONE`
// else-branch, which this build does not take — defined anyway so the dead branch parses.
#define fsRdPerm 1

OSStatus AudioFileOpenURL(CFURLRef inFileRef, SInt8 inPermissions, UInt32 inFileTypeHint,
                          AudioFileID *outAudioFile);
OSStatus AudioFileClose(AudioFileID inAudioFile);
OSStatus AudioFileReadBytes(AudioFileID inAudioFile, Boolean inUseCache, SInt64 inStartingByte,
                            UInt32 *ioNumBytes, void *outBuffer);
OSStatus AudioFileGetProperty(AudioFileID inAudioFile, AudioFilePropertyID inPropertyID,
                              UInt32 *ioDataSize, void *outPropertyData);
OSStatus AudioFileGetPropertyInfo(AudioFileID inAudioFile, AudioFilePropertyID inPropertyID,
                                  UInt32 *outDataSize, UInt32 *isWritable);

#ifdef __cplusplus
}
#endif

#endif
