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
// <AudioToolbox/AudioToolbox.h> — umbrella header, mirroring Apple's. See AudioFile.h for what
// is actually implemented (and what deliberately is not) at this stage of the port.
#ifndef EDEN_SHIM_AUDIOTOOLBOX_AUDIOTOOLBOX_H
#define EDEN_SHIM_AUDIOTOOLBOX_AUDIOTOOLBOX_H
#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/AudioFileStream.h>
#include <AudioToolbox/ExtendedAudioFile.h>
#endif
