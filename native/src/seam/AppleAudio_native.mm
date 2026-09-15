// AppleAudio_native.mm — the Apple half of Stage 2's audio, isolated. Read AppleAudio_native.h
// first: it explains why this is its own translation unit (the port shadows <AudioToolbox/...>
// and <AVFoundation/...> on purpose) and why the wall between here and the rest of the backend is
// plain C.
//
// This file must NOT include anything from Classes/, web/src/shim/, or the GL shim. It sees the
// real SDK and nothing else, which is the whole point — its CMake object library is configured
// without the shim's framework/ directory on the include path.

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include "AppleAudio_native.h"

#include <stdlib.h>
#include <string.h>

static char g_lastError[256] = {0};

static void set_error(const char* s) {
    if (!s) { g_lastError[0] = 0; return; }
    strncpy(g_lastError, s, sizeof(g_lastError) - 1);
    g_lastError[sizeof(g_lastError) - 1] = 0;
}

const char* eden_apple_audio_last_error(void) { return g_lastError[0] ? g_lastError : NULL; }

// ---------------------------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------------------------
// ExtAudioFile, with a CLIENT format of packed interleaved signed-16 at the file's own rate: it
// runs the AudioConverter for us, so one code path covers raw LPCM .caf, Apple IMA4 .caf (428 of
// the 436 effects), .mp3, .wav and .aif. Channels are clamped to stereo — nothing in this library
// is multichannel, and an unclamped >2ch file would size the output buffer wrong rather than fail.
int eden_apple_audio_decode(const char* path, int16_t** outPcm, int* outFrames,
                            int* outChannels, int* outRate) {
    if (!path || !outPcm || !outFrames || !outChannels || !outRate) return 0;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
    if (!url) { set_error("bad path"); return 0; }

    ExtAudioFileRef af = NULL;
    OSStatus st = ExtAudioFileOpenURL(url, &af);
    CFRelease(url);
    if (st != noErr || !af) { set_error("ExtAudioFileOpenURL failed"); return 0; }

    AudioStreamBasicDescription src;
    memset(&src, 0, sizeof(src));
    UInt32 sz = sizeof(src);
    if (ExtAudioFileGetProperty(af, kExtAudioFileProperty_FileDataFormat, &sz, &src) != noErr) {
        ExtAudioFileDispose(af);
        set_error("no file format");
        return 0;
    }

    const int channels = (src.mChannelsPerFrame >= 2) ? 2 : 1;
    AudioStreamBasicDescription client;
    memset(&client, 0, sizeof(client));
    client.mSampleRate       = (src.mSampleRate > 0) ? src.mSampleRate : 44100.0;
    client.mFormatID         = kAudioFormatLinearPCM;
    client.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    client.mChannelsPerFrame = (UInt32)channels;
    client.mBitsPerChannel   = 16;
    client.mFramesPerPacket  = 1;
    client.mBytesPerFrame    = (UInt32)(channels * 2);
    client.mBytesPerPacket   = client.mBytesPerFrame;
    if (ExtAudioFileSetProperty(af, kExtAudioFileProperty_ClientDataFormat,
                                sizeof(client), &client) != noErr) {
        ExtAudioFileDispose(af);
        set_error("client format refused");
        return 0;
    }

    // FileLengthFrames counts the FILE's frames; when the client rate differs it is only an
    // estimate, so it sizes the first allocation and the read loop decides where the file ends.
    SInt64 estFrames = 0;
    sz = sizeof(estFrames);
    ExtAudioFileGetProperty(af, kExtAudioFileProperty_FileLengthFrames, &sz, &estFrames);
    if (estFrames <= 0 || estFrames > (1 << 26)) estFrames = 1 << 16;

    size_t cap = (size_t)estFrames * (size_t)channels + 4096;
    int16_t* buf = (int16_t*)malloc(cap * sizeof(int16_t));
    if (!buf) { ExtAudioFileDispose(af); set_error("out of memory"); return 0; }

    const UInt32 kChunkFrames = 4096;
    size_t used = 0;
    for (;;) {
        if (used + (size_t)kChunkFrames * (size_t)channels > cap) {
            size_t ncap = cap * 2;
            int16_t* nbuf = (int16_t*)realloc(buf, ncap * sizeof(int16_t));
            if (!nbuf) break;                        // keep what we have rather than lose it all
            buf = nbuf;
            cap = ncap;
        }
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = (UInt32)channels;
        abl.mBuffers[0].mDataByteSize =
            (UInt32)(kChunkFrames * (UInt32)channels * sizeof(int16_t));
        abl.mBuffers[0].mData = buf + used;
        UInt32 frames = kChunkFrames;
        if (ExtAudioFileRead(af, &frames, &abl) != noErr) break;
        if (frames == 0) break;                      // clean EOF
        used += (size_t)frames * (size_t)channels;
    }
    ExtAudioFileDispose(af);

    if (used == 0) { free(buf); set_error("decoded 0 frames"); return 0; }

    *outPcm      = buf;
    *outFrames   = (int)(used / (size_t)channels);
    *outChannels = channels;
    *outRate     = (int)client.mSampleRate;
    set_error(NULL);
    return 1;
}

void eden_apple_audio_free(int16_t* pcm) { free(pcm); }

// ---------------------------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------------------------
// AVAudioPlayer, one per channel. It is the direct counterpart of the web backend's <audio>
// element — decodes lazily as it plays, so a 4-minute mp3 costs a buffer rather than the ~40 MB
// of PCM the decode path above would produce for it.
struct EdenApplePlayer {
    AVAudioPlayer* player;
};

EdenApplePlayer* eden_apple_player_create(const char* path) {
    if (!path || !*path) return NULL;
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        NSError* err = nil;
        AVAudioPlayer* p = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&err];
        if (!p) {
            set_error(err ? [[err localizedDescription] UTF8String] : "AVAudioPlayer init failed");
            return NULL;
        }
        [p prepareToPlay];
        EdenApplePlayer* h = (EdenApplePlayer*)calloc(1, sizeof(EdenApplePlayer));
        if (!h) { [p release]; set_error("out of memory"); return NULL; }
        h->player = p;                 // retained by the alloc; released in destroy
        set_error(NULL);
        return h;
    }
}

void eden_apple_player_destroy(EdenApplePlayer* h) {
    if (!h) return;
    if (h->player) { [h->player stop]; [h->player release]; }
    free(h);
}

void eden_apple_player_play(EdenApplePlayer* h)   { if (h && h->player) [h->player play]; }
void eden_apple_player_pause(EdenApplePlayer* h)  { if (h && h->player) [h->player pause]; }
void eden_apple_player_stop(EdenApplePlayer* h) {
    if (!h || !h->player) return;
    [h->player stop];
    h->player.currentTime = 0;
}
void eden_apple_player_rewind(EdenApplePlayer* h) {
    if (h && h->player) h->player.currentTime = 0;
}
// -1 is AVAudioPlayer's "loop forever"; 0 is "play once".
void eden_apple_player_set_loop(EdenApplePlayer* h, int loop) {
    if (h && h->player) h->player.numberOfLoops = loop ? -1 : 0;
}
void eden_apple_player_set_volume(EdenApplePlayer* h, float v) {
    if (!h || !h->player) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    h->player.volume = v;
}
int eden_apple_player_is_playing(EdenApplePlayer* h) {
    return (h && h->player && h->player.playing) ? 1 : 0;
}
