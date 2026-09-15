// SimpleAudioEngine_native.mm — Phase N Stage 2's audio row. Replaces the inert stub that lived
// in seam_link_stubs_native.mm (that section is deleted; keeping it would be a duplicate symbol),
// and is the native twin of web/src/seam/SimpleAudioEngine_web.mm. Read that file first: the
// SHAPE here is deliberately identical to it, because the engine's audio contract is the same on
// both and any divergence would be a second behaviour model rather than a second backend.
//
//   * EFFECTS are short, overlap, and need low latency -> decoded once, cached by name, and mixed
//     as SDL_AudioStreams bound to one playback device. Same role Web Audio's AudioBuffers play.
//   * The FIVE CHANNELS (0 = music, 1-4 = ambience beds/proximity layers) are minutes long ->
//     streamed by AVAudioPlayer, which is the role the <audio> element plays on web. Decoding a
//     4-minute mp3 into PCM would cost ~40 MB per channel, which is half of Stage 1's entire
//     in-world RSS budget.
//
// The engine passes BARE FILENAMES with no directory ("explosion.caf", "Eden_title.mp3", and one
// with a space in it — "block break_leaves_1.caf"). On iOS those resolved against the flat app
// bundle; here they resolve against the executable's directory, where CMake symlinks the media/
// tree flat for exactly this reason. That is why this file needs no equivalent of web's
// public/audio-manifest.json.
//
// ---------------------------------------------------------------------------------------------
// WHERE THIS DEPARTS FROM THE PLAN, AND WHY
// ---------------------------------------------------------------------------------------------
// native-migration-plan-2026-09-04.md says to port SimpleAudioEngine_web.mm's hand-rolled LPCM
// .caf decoder, "portable C", behind miniaudio or SDL_audio. Two facts make that the wrong first
// move on THIS leg, both established by looking at what the engine actually asks for:
//
//   1. **The hand-rolled decoder would not be enough.** It exists on web because Chrome's
//      decodeAudioData refuses LPCM CAF — a browser constraint, not a format problem. But the
//      sound library is 436 .caf (428 of them Apple IMA4, not LPCM), 16 .wav, 1 .aif, and
//      **104 .mp3** (every creature voice, every ambience bed, all 8 music tracks). Porting the
//      CAF half would leave the mp3s — i.e. all music and all creature sound — silent, and mp3
//      needs a decoder this tree does not vendor.
//   2. **This leg already runs on Apple's real frameworks**, which was itself forced (Stage 1's
//      finding: no GNU ObjC ABI lowers to Mach-O). AudioToolbox's ExtAudioFile decodes all five
//      formats, including IMA4 CAF, in ~40 lines and with no vendored third-party code.
//
// So decoding sits behind ONE function, `decode_file()`, whose contract is "path in, interleaved
// 16-bit PCM + rate + channels out". That is the seam the Linux leg plugs into — and when it
// does, the plan's portable CAF decoder plus an mp3 decoder go THERE, with the mixer, the voice
// pool, the channel model and the whole façade below already written and exercised. The mixer is
// SDL, so that half is already portable today.
//
// ---------------------------------------------------------------------------------------------
// WHAT HAS NO EQUIVALENT HERE (all of it web-host policy, not engine behaviour)
// ---------------------------------------------------------------------------------------------
// The autoplay gesture gate, the AudioContext resume dance, the mediaSession key handlers, and
// the pause-on-visibility-hidden logic are all answers to browser rules. A desktop app has none
// of them: the device opens when the engine first asks for a sound and stays open.

#include "../../../Classes/SimpleAudioEngine.h"
#include "SimpleAudioEngine_native.h"
// The Apple frameworks are reached ONLY through this C wall — see AppleAudio_native.h for why
// they cannot be #imported here (the port shadows <AudioToolbox/...> and <AVFoundation/...> on
// purpose, and unshadowing them would pull CoreServices into ~80 engine translation units).
#include "AppleAudio_native.h"

#import <Foundation/Foundation.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------
// Enable/disable
// ---------------------------------------------------------------------------------------------
// Off for the scripted/headless modes, the same way eden_native_gl_set_headless() takes the GL
// backend out. Not just noise-avoidance: --stage1 and --input-selftest run thousands of frames in
// a few seconds, so every sound the engine fires would be decoded and queued at ~50x real time.
bool g_enabled = true;
bool g_deviceTried = false;
SDL_AudioDeviceID g_device = 0;
SDL_AudioSpec g_deviceSpec = {SDL_AUDIO_F32, 2, 48000};

// ---------------------------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------------------------
// `[[NSBundle mainBundle] resourcePath]` for a non-.app executable IS the executable's directory
// (Stage 1's finding, and why the assets are symlinked flat next to the binary). Bare filenames
// therefore resolve with a plain join — pathForResource: would also work but silently returns nil
// for a name containing a space, and one of the sound files has one.
std::string resolve_path(const char* name) {
    if (!name || !*name) return std::string();
    NSString* root = [[NSBundle mainBundle] resourcePath];
    if (!root) return std::string();
    NSString* full = [root stringByAppendingPathComponent:[NSString stringWithUTF8String:name]];
    return std::string([full UTF8String]);
}

// ---------------------------------------------------------------------------------------------
// Decode — THE SEAM (see this file's header)
// ---------------------------------------------------------------------------------------------
struct Clip {
    std::vector<Sint16> pcm;   // interleaved
    int channels = 0;
    int rate = 0;
    bool ok() const { return !pcm.empty() && channels > 0 && rate > 0; }
};

// path in, interleaved signed-16 PCM out. The platform half lives behind AppleAudio_native.h;
// the Linux leg replaces THAT and leaves everything below untouched.
bool decode_file(const std::string& path, Clip* out) {
    Sint16* pcm = nullptr;
    int frames = 0, channels = 0, rate = 0;
    if (!eden_apple_audio_decode(path.c_str(), &pcm, &frames, &channels, &rate)) return false;
    out->pcm.assign(pcm, pcm + (size_t)frames * (size_t)channels);
    eden_apple_audio_free(pcm);
    out->channels = channels;
    out->rate = rate;
    return out->ok();
}

// ---------------------------------------------------------------------------------------------
// Effects: cache + voice pool
// ---------------------------------------------------------------------------------------------
std::unordered_map<std::string, Clip> g_clips;   // name -> decoded PCM; a failed decode caches an
                                                 // empty Clip so it is attempted exactly once
float g_effectsVolume = 1.0f;

struct Voice {
    unsigned int id = 0;
    SDL_AudioStream* stream = nullptr;
    const Clip* clip = nullptr;
    bool loop = false;
    bool paused = false;
};
std::vector<Voice> g_voices;
unsigned int g_nextVoiceId = 1;

// A ceiling on simultaneous voices. The engine fires a sound per broken block and per creature
// noise with no throttle of its own, and a TNT chain reaction can ask for dozens in one frame.
constexpr size_t kMaxVoices = 48;

// Defined with the channel model further down; declared here because shutdown has to reach it.
void destroy_all_channels();

bool ensure_device() {
    if (!g_enabled) return false;
    if (g_device) return true;
    if (g_deviceTried) return false;
    g_deviceTried = true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[eden-audio] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return false;
    }
    g_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!g_device) {
        std::fprintf(stderr, "[eden-audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    // The device picks its own format; every stream converts into it. Asking rather than assuming
    // matters because the conversion is per-stream and gets it wrong silently otherwise.
    SDL_GetAudioDeviceFormat(g_device, &g_deviceSpec, NULL);
    std::fprintf(stderr, "[eden-audio] device open: %d Hz, %d ch\n",
                 g_deviceSpec.freq, g_deviceSpec.channels);
    return true;
}

const Clip* clip_for(const char* name) {
    if (!name || !*name) return nullptr;
    std::string key(name);
    auto it = g_clips.find(key);
    if (it != g_clips.end()) return it->second.ok() ? &it->second : nullptr;

    Clip c;
    const std::string path = resolve_path(name);
    if (path.empty() || !decode_file(path, &c)) {
        std::fprintf(stderr, "[eden-audio] cannot decode '%s'\n", name);
        c = Clip();                       // negative-cached: never retried
    }
    auto ins = g_clips.emplace(std::move(key), std::move(c));
    return ins.first->second.ok() ? &ins.first->second : nullptr;
}

int clip_bytes(const Clip& c) { return (int)(c.pcm.size() * sizeof(Sint16)); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Host hooks (declared in SimpleAudioEngine_native.h)
// ---------------------------------------------------------------------------------------------
extern "C" void eden_native_audio_set_enabled(int on) {
    g_enabled = (on != 0);
}

// Called once per frame by the host loop. Two jobs, both of which the browser did for us: reap
// voices that have drained, and re-feed the looping ones. A voice is finished when its stream has
// no bytes left to hand the device — SDL keeps the stream alive and silent otherwise, so without
// this the pool fills up and the 48-voice ceiling starts refusing new sounds after a few minutes.
extern "C" void eden_native_audio_tick(void) {
    if (!g_device) return;
    for (size_t i = 0; i < g_voices.size();) {
        Voice& v = g_voices[i];
        const int avail = SDL_GetAudioStreamAvailable(v.stream);
        if (v.loop && v.clip && avail < clip_bytes(*v.clip)) {
            // Top up to keep at least one whole copy queued ahead, so the loop point never
            // underruns into a click.
            SDL_PutAudioStreamData(v.stream, v.clip->pcm.data(), clip_bytes(*v.clip));
        } else if (!v.loop && avail <= 0) {
            SDL_DestroyAudioStream(v.stream);       // unbinds from the device as it goes
            g_voices.erase(g_voices.begin() + (long)i);
            continue;
        }
        ++i;
    }
}

extern "C" int eden_native_audio_probe(const char* name, int* frames, int* channels, int* rate) {
    const Clip* c = clip_for(name);
    if (!c) return 0;
    if (frames)   *frames   = (int)(c->pcm.size() / (size_t)c->channels);
    if (channels) *channels = c->channels;
    if (rate)     *rate     = c->rate;
    return 1;
}

extern "C" int eden_native_audio_voice_count(void) { return (int)g_voices.size(); }


extern "C" void eden_native_audio_shutdown(void) {
    for (Voice& v : g_voices) SDL_DestroyAudioStream(v.stream);
    g_voices.clear();
    destroy_all_channels();
    if (g_device) { SDL_CloseAudioDevice(g_device); g_device = 0; }
}

// ---------------------------------------------------------------------------------------------
// The five streaming channels (0 = music, 1-4 = ambience), AVAudioPlayer-backed
// ---------------------------------------------------------------------------------------------
// The volume split is web's, exactly: `userVolume` is the settings slider, `engineFade` is the
// per-frame crossfade Resources::update computes, and the player's volume is their product. They
// are kept apart because the engine writes one of them 60 times a second and the player writes
// the other once — folding them would make each stomp the other.
namespace {

constexpr int kChannelCount = 5;
EdenApplePlayer* g_players[kChannelCount] = {nullptr, nullptr, nullptr, nullptr, nullptr};
std::string g_channelName[kChannelCount];
float g_channelUserVolume[kChannelCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
float g_channelFade[kChannelCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

void apply_channel_volume(int ch) {
    if (ch < 0 || ch >= kChannelCount || !g_players[ch]) return;
    eden_apple_player_set_volume(g_players[ch],
                                 clamp01(g_channelUserVolume[ch]) * clamp01(g_channelFade[ch]));
}

void play_channel(int ch, const char* name, bool loop) {
    if (!g_enabled || ch < 0 || ch >= kChannelCount || !name || !*name) return;
    // Same as the web twin: asking for the track that is already on this channel is a resume, not
    // a restart. Resources::update re-asserts the current ambience bed on every frame it is in
    // range, so restarting here would retrigger the file 60 times a second.
    if (g_players[ch] && g_channelName[ch] == name) {
        eden_apple_player_set_loop(g_players[ch], loop ? 1 : 0);
        if (!eden_apple_player_is_playing(g_players[ch])) eden_apple_player_play(g_players[ch]);
        return;
    }
    const std::string path = resolve_path(name);
    if (path.empty()) return;
    EdenApplePlayer* p = eden_apple_player_create(path.c_str());
    if (!p) {
        const char* e = eden_apple_audio_last_error();
        std::fprintf(stderr, "[eden-audio] channel %d cannot open '%s': %s\n",
                     ch, name, e ? e : "?");
        return;
    }
    if (g_players[ch]) eden_apple_player_destroy(g_players[ch]);
    g_players[ch] = p;
    g_channelName[ch] = name;
    eden_apple_player_set_loop(p, loop ? 1 : 0);
    apply_channel_volume(ch);
    eden_apple_player_play(p);
    // Worth a line: which file a channel switched to is the only externally visible trace of the
    // engine's ambience-bed selection, and a bed that never starts and a bed that starts silently
    // look identical from outside.
    std::fprintf(stderr, "[eden-audio] channel %d -> %s (loop=%d, vol=%.2f)\n",
                 ch, name, loop ? 1 : 0,
                 clamp01(g_channelUserVolume[ch]) * clamp01(g_channelFade[ch]));
}

void stop_channel(int ch) {
    if (ch < 0 || ch >= kChannelCount || !g_players[ch]) return;
    eden_apple_player_stop(g_players[ch]);
}

void destroy_all_channels() {
    for (int ch = 0; ch < kChannelCount; ++ch) {
        if (!g_players[ch]) continue;
        eden_apple_player_destroy(g_players[ch]);
        g_players[ch] = nullptr;
        g_channelName[ch].clear();
    }
}

}  // namespace

// Channel probes, defined here because they read the channel model just above.
extern "C" float eden_native_audio_channel_volume(int ch) {
    if (ch < 0 || ch >= kChannelCount || !g_players[ch]) return -1.0f;
    return clamp01(g_channelUserVolume[ch]) * clamp01(g_channelFade[ch]);
}

extern "C" const char* eden_native_audio_channel_name(int ch) {
    if (ch < 0 || ch >= kChannelCount) return "";
    return g_channelName[ch].c_str();
}

// ---------------------------------------------------------------------------------------------
// The façade the engine actually calls
// ---------------------------------------------------------------------------------------------
namespace CocosDenshion {

static SimpleAudioEngine* g_sharedEngine = nullptr;

SimpleAudioEngine::SimpleAudioEngine() {}
SimpleAudioEngine::~SimpleAudioEngine() {}

// Returns a real singleton rather than null: callers message the result without checking
// (unchanged from the stub this replaces, and from the web twin).
SimpleAudioEngine* SimpleAudioEngine::sharedEngine() {
    if (!g_sharedEngine) g_sharedEngine = new SimpleAudioEngine();
    return g_sharedEngine;
}

void SimpleAudioEngine::end() { eden_native_audio_shutdown(); }

unsigned int SimpleAudioEngine::playEffect(const char* pszFilePath, bool bLoop) {
    if (!g_enabled || !pszFilePath) return 0;
    if (!ensure_device()) return 0;
    const Clip* c = clip_for(pszFilePath);
    if (!c) return 0;
    if (g_voices.size() >= kMaxVoices) return 0;    // drop rather than stall; see kMaxVoices

    SDL_AudioSpec srcSpec;
    srcSpec.format = SDL_AUDIO_S16;
    srcSpec.channels = c->channels;
    srcSpec.freq = c->rate;
    SDL_AudioStream* s = SDL_CreateAudioStream(&srcSpec, &g_deviceSpec);
    if (!s) return 0;
    SDL_SetAudioStreamGain(s, clamp01(g_effectsVolume));
    SDL_PutAudioStreamData(s, c->pcm.data(), clip_bytes(*c));
    if (bLoop) SDL_PutAudioStreamData(s, c->pcm.data(), clip_bytes(*c));   // one copy of headroom
    if (!SDL_BindAudioStream(g_device, s)) { SDL_DestroyAudioStream(s); return 0; }

    Voice v;
    v.id = g_nextVoiceId++;
    v.stream = s;
    v.clip = c;
    v.loop = bLoop;
    g_voices.push_back(v);
    return v.id;
}

void SimpleAudioEngine::stopEffect(unsigned int nSoundId) {
    for (size_t i = 0; i < g_voices.size(); ++i) {
        if (g_voices[i].id != nSoundId) continue;
        SDL_DestroyAudioStream(g_voices[i].stream);
        g_voices.erase(g_voices.begin() + (long)i);
        return;
    }
}

void SimpleAudioEngine::stopAllEffects() {
    for (Voice& v : g_voices) SDL_DestroyAudioStream(v.stream);
    g_voices.clear();
}

// Pause/resume are per-voice on iOS. SDL has no per-stream pause, so this is expressed as "stop
// feeding the device" — SDL_UnbindAudioStream leaves the queued data intact, and rebinding
// resumes from exactly where it stopped.
void SimpleAudioEngine::pauseEffect(unsigned int nSoundId) {
    for (Voice& v : g_voices)
        if (v.id == nSoundId && !v.paused) { SDL_UnbindAudioStream(v.stream); v.paused = true; }
}
void SimpleAudioEngine::resumeEffect(unsigned int nSoundId) {
    for (Voice& v : g_voices)
        if (v.id == nSoundId && v.paused) { SDL_BindAudioStream(g_device, v.stream); v.paused = false; }
}
void SimpleAudioEngine::pauseAllEffects() {
    for (Voice& v : g_voices)
        if (!v.paused) { SDL_UnbindAudioStream(v.stream); v.paused = true; }
}
void SimpleAudioEngine::resumeAllEffects() {
    for (Voice& v : g_voices)
        if (v.paused) { SDL_BindAudioStream(g_device, v.stream); v.paused = false; }
}

// Decode now so the first play does not pay for it. The engine's own preload loop is commented
// out in Classes/Resources.mm (loadGameAssets), so in practice every clip is decoded lazily on
// its first play — a sub-millisecond hitch for a one-second .caf, but a real one for a long .mp3.
void SimpleAudioEngine::preloadEffect(const char* pszFilePath) {
    if (!g_enabled || !pszFilePath) return;
    if (!ensure_device()) return;
    clip_for(pszFilePath);
}

void SimpleAudioEngine::unloadEffect(const char* pszFilePath) {
    if (pszFilePath) g_clips.erase(std::string(pszFilePath));
}

float SimpleAudioEngine::getEffectsVolume() { return g_effectsVolume; }
void SimpleAudioEngine::setEffectsVolume(float volume) {
    g_effectsVolume = clamp01(volume);
    for (Voice& v : g_voices) SDL_SetAudioStreamGain(v.stream, g_effectsVolume);
}

// --- channel 0: music ---
void SimpleAudioEngine::preloadBackgroundMusic(const char* pszFilePath) { (void)pszFilePath; }
void SimpleAudioEngine::playBackgroundMusic(const char* pszFilePath, bool bLoop) {
    play_channel(0, pszFilePath, bLoop);
}
void SimpleAudioEngine::stopBackgroundMusic(bool bReleaseData) { (void)bReleaseData; stop_channel(0); }
void SimpleAudioEngine::pauseBackgroundMusic()  { if (g_players[0]) eden_apple_player_pause(g_players[0]); }
void SimpleAudioEngine::resumeBackgroundMusic() { if (g_players[0]) eden_apple_player_play(g_players[0]); }
void SimpleAudioEngine::rewindBackgroundMusic() { if (g_players[0]) eden_apple_player_rewind(g_players[0]); }
bool SimpleAudioEngine::willPlayBackgroundMusic() { return true; }
bool SimpleAudioEngine::isBackgroundMusicPlaying() {
    return g_players[0] && eden_apple_player_is_playing(g_players[0]);
}
// Music has one volume knob, not the user/fade split the ambience channels get — the settings
// slider and Resources::update's song crossfade both call this, last write wins. Same as web.
float SimpleAudioEngine::getBackgroundMusicVolume() { return g_channelUserVolume[0]; }
void SimpleAudioEngine::setBackgroundMusicVolume(float volume) {
    g_channelUserVolume[0] = clamp01(volume);
    apply_channel_volume(0);
}

// --- channels 1-4: ambience ---
void SimpleAudioEngine::playAmbience(int layer, const char* pszFilePath, bool bLoop) {
    play_channel(layer + 1, pszFilePath, bLoop);
}
void SimpleAudioEngine::stopAmbience(int layer) { stop_channel(layer + 1); }
bool SimpleAudioEngine::isAmbiencePlaying(int layer) {
    const int ch = layer + 1;
    return ch >= 0 && ch < kChannelCount && g_players[ch] &&
           eden_apple_player_is_playing(g_players[ch]) != 0;
}
void SimpleAudioEngine::setAmbienceFade(int layer, float fade) {
    const int ch = layer + 1;
    if (ch < 0 || ch >= kChannelCount) return;
    g_channelFade[ch] = fade;
    apply_channel_volume(ch);
}
float SimpleAudioEngine::getAmbienceVolume() { return g_channelUserVolume[1]; }
void SimpleAudioEngine::setAmbienceVolume(float v) {
    for (int ch = 1; ch < kChannelCount; ++ch) {
        g_channelUserVolume[ch] = clamp01(v);
        apply_channel_volume(ch);
    }
}

}  // namespace CocosDenshion
