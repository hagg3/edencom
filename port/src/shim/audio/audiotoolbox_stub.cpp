// audiotoolbox_stub.cpp — deliberately-failing AudioToolbox implementations, Stage P5 placeholder.
//
// WHY FAILING STUBS RATHER THAN NO FILE: the P1 milestone is "the engine links and runs one
// headless tick". Classes/Sound.m is ordinary ENGINE code (not seam-excluded), so it references
// AudioFileOpenURL/AudioFileReadBytes/AudioFileClose/AudioFileGetProperty and the target cannot
// link without them. Writing a CAF/WAV decoder to satisfy that would be doing Stage P5's work
// early and in the wrong place.
//
// WHY FAILING IS SAFE: the engine already checks. Sound.m reads
//     OSStatus result = AudioFileReadBytes(...);  if (result != 0) NSLog(@"cannot load effect…");
// and carries on with an empty buffer — the same path a device would take for a missing .caf.
// So a non-zero return degrades to "no sound", not to a crash or to garbage PCM. Returning
// noErr with an untouched buffer would be the dangerous choice: the engine would then hand
// uninitialized memory to alBufferData.
//
// Stage P5 replaces this file entirely; see web-port-plan.md ("CocosDenshion → Web Audio") and
// framework/AudioToolbox/AudioFile.h.
#include <AudioToolbox/AudioToolbox.h>

extern "C" {

// A distinctive non-zero OSStatus so a log line is traceable back to here rather than looking
// like a real CoreAudio error code.
static const OSStatus kEdenAudioNotImplemented = -20250719;

OSStatus AudioFileOpenURL(CFURLRef inFileRef, SInt8 inPermissions, UInt32 inFileTypeHint,
                          AudioFileID *outAudioFile) {
  (void)inFileRef;
  (void)inPermissions;
  (void)inFileTypeHint;
  if (outAudioFile) *outAudioFile = 0;
  return kEdenAudioNotImplemented;
}

OSStatus AudioFileClose(AudioFileID inAudioFile) {
  (void)inAudioFile;
  return kEdenAudioNotImplemented;
}

OSStatus AudioFileReadBytes(AudioFileID inAudioFile, Boolean inUseCache, SInt64 inStartingByte,
                            UInt32 *ioNumBytes, void *outBuffer) {
  (void)inAudioFile;
  (void)inUseCache;
  (void)inStartingByte;
  (void)outBuffer;
  // Report zero bytes read, so a caller that ignores the status at least sees an empty buffer
  // rather than believing its requested length was filled.
  if (ioNumBytes) *ioNumBytes = 0;
  return kEdenAudioNotImplemented;
}

OSStatus AudioFileGetProperty(AudioFileID inAudioFile, AudioFilePropertyID inPropertyID,
                              UInt32 *ioDataSize, void *outPropertyData) {
  (void)inAudioFile;
  (void)inPropertyID;
  (void)outPropertyData;
  if (ioDataSize) *ioDataSize = 0;
  return kEdenAudioNotImplemented;
}

OSStatus AudioFileGetPropertyInfo(AudioFileID inAudioFile, AudioFilePropertyID inPropertyID,
                                  UInt32 *outDataSize, UInt32 *isWritable) {
  (void)inAudioFile;
  (void)inPropertyID;
  if (outDataSize) *outDataSize = 0;
  if (isWritable) *isWritable = 0;
  return kEdenAudioNotImplemented;
}

}  // extern "C"
