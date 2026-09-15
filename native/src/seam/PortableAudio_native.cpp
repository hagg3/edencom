// PortableAudio_native.cpp — the non-Apple implementation of AppleAudio_native.h's wall.
// Phase N Stage 3.5 (WORKING/phase-n-stage3plus-plan-2026-09-05.md).
//
// AppleAudio_native.h was written in Stage 2 with this file in mind: it names exactly the two jobs
// the audio backend cannot do without a platform ("decode any format" and "stream one long file")
// and nothing else. Everything above it — the voice pool, the clip cache, the five-channel model,
// the user/fade volume split, the SimpleAudioEngine façade — is in
// src/seam/SimpleAudioEngine_native.mm, is already portable, and is UNCHANGED by this file's
// existence. That is the whole point of the wall, and it is why the biggest item in Stage 3 is
// one new file rather than a second audio backend.
//
// WHAT THE LIBRARY ACTUALLY IS — measured, not assumed, because Stage 2 already learned that
// guessing here is expensive (the plan's "port the LPCM decoder" advice would have left every
// creature voice and all music silent):
//
//     428  .caf   Apple IMA4 ADPCM, mono, 44100 Hz, 34-byte / 64-frame packets
//       8  .caf   LPCM, mono, 16-bit little-endian, 44100 Hz
//     104  .mp3   music (8 tracks) + ambience beds + creature voices
//      16  .wav   PCM, mono and stereo, 44100 and 22050 Hz, 16-bit
//       1  .aif   AIFF-C-free plain AIFF, mono, 16-bit big-endian, 44100 Hz  (media/sound/Grab.aif)
//
// Four containers, two of which have a vendored decoder and two of which are ~60 lines here.
// The census is reproducible:
//     python3 - <<'PY'  ... struct.unpack('>d4sIIIII', header[20:52]) over media/**/*.caf ... PY
//
// DECODER CHOICES:
//   * mp3 → dr_mp3, wav → dr_wav (vendored, public domain / MIT-0, single header each). mp3 is
//     not something to hand-write, and dr_wav's format coverage is free insurance against a
//     modder's stereo/24-bit/float .wav that this library happens not to contain today.
//   * caf → HAND-WRITTEN, here. There is no small public-domain CAF library, the container is a
//     trivial type/size chunk walk, and Apple IMA4 is ordinary IMA ADPCM in a 34-byte packet.
//     Writing it is smaller than vendoring something that would also drag in a format zoo.
//   * aif → HAND-WRITTEN, here, for one file. Plain uncompressed AIFF is a chunk walk plus a
//     byte swap; pulling in a library for a single 22 KB sound would be the wrong trade.
//
// AND THE ONE THING THIS FILE DELIBERATELY DOES NOT DO: resample or mix. Both already happen
// upstream — SimpleAudioEngine_native.mm hands every clip to an SDL_AudioStream whose source spec
// is the clip's own rate and channel count, and SDL converts into whatever the device chose. So
// "44100 vs 22050" is not this file's problem, and a decoder here must report the file's true rate
// rather than normalising it.

#include "AppleAudio_native.h"

#include <SDL3/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Vendored decoders, at their defaults. Both are single-header libraries whose implementation is
// pulled in exactly once, here — putting them in any other translation unit would duplicate ~15
// kLOC of code with no caller. Vendored rather than depended on because they are public
// domain / MIT-0 and because a Windows CI runner has no package manager this build trusts.
#define DR_WAV_IMPLEMENTATION
#include "../shim/vendor/dr_wav.h"

#define DR_MP3_IMPLEMENTATION
#include "../shim/vendor/dr_mp3.h"

namespace {

char g_lastError[512] = {0};

void set_error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_lastError, sizeof(g_lastError), fmt, ap);
  va_end(ap);
}

// ---------------------------------------------------------------------------------------------
// Container sniffing
// ---------------------------------------------------------------------------------------------
// BY MAGIC BYTES, NOT BY EXTENSION, and that is worth a sentence: the engine passes bare
// filenames ("explosion.caf", "Eden_title.mp3") that the iOS bundle's flat layout made meaningful,
// and this port's own Stage 2 pass found three ambience beds whose NAMES did not match any file at
// all. A container that lies about its extension would be one more of the same class of bug, and
// the check costs 12 bytes of a file that is about to be read anyway.
enum Container { CONTAINER_UNKNOWN, CONTAINER_CAF, CONTAINER_WAV, CONTAINER_AIFF, CONTAINER_MP3 };

Container sniff(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return CONTAINER_UNKNOWN;
  unsigned char h[12] = {0};
  const size_t got = fread(h, 1, sizeof(h), f);
  fclose(f);
  if (got < 12) return CONTAINER_UNKNOWN;

  if (!memcmp(h, "caff", 4)) return CONTAINER_CAF;
  if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) return CONTAINER_WAV;
  if (!memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4)))
    return CONTAINER_AIFF;
  // MP3 has no container magic. An ID3v2 tag or a frame sync in the first two bytes is the
  // conventional test and covers 99 of this library's 104 mp3s.
  if (!memcmp(h, "ID3", 3)) return CONTAINER_MP3;
  if (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) return CONTAINER_MP3;

  // ...and the other two start with a run of zero bytes before the first frame, which is legal and
  // which no first-12-bytes test can catch (media/music/Eden_1.mp3 is one; it is the title track,
  // so getting this wrong is silent music on the very first screen). Fall back to the extension
  // rather than growing the sniff into a frame scanner: magic-first is still the rule, this is only
  // what happens when the magic says nothing at all.
  const char* dot = strrchr(path, '.');
  if (dot) {
    if (!SDL_strcasecmp(dot, ".mp3")) return CONTAINER_MP3;
    if (!SDL_strcasecmp(dot, ".caf")) return CONTAINER_CAF;
    if (!SDL_strcasecmp(dot, ".wav")) return CONTAINER_WAV;
    if (!SDL_strcasecmp(dot, ".aif") || !SDL_strcasecmp(dot, ".aiff")) return CONTAINER_AIFF;
  }
  return CONTAINER_UNKNOWN;
}

// Whole file into memory. Every sound this decodes is at most a few MB (the streaming path never
// comes through here), so a read-it-all decoder is simpler and no slower than a seeking one.
bool slurp(const char* path, std::vector<unsigned char>* out) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return false; }
  out->resize((size_t)n);
  const size_t got = fread(out->data(), 1, (size_t)n, f);
  fclose(f);
  out->resize(got);
  return got > 0;
}

inline uint32_t be32(const unsigned char* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint64_t be64(const unsigned char* p) {
  return ((uint64_t)be32(p) << 32) | be32(p + 4);
}
inline int16_t le16(const unsigned char* p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
inline int16_t be16s(const unsigned char* p) { return (int16_t)(((uint16_t)p[0] << 8) | p[1]); }

// ---------------------------------------------------------------------------------------------
// Apple IMA4 (kAudioFormatAppleIMA4)
// ---------------------------------------------------------------------------------------------
// Ordinary IMA ADPCM in Apple's packet framing: per channel, 34 bytes hold 64 frames as a 2-byte
// big-endian preamble plus 32 data bytes of two 4-bit nibbles each, LOW NIBBLE FIRST.
//
// *** THE PREDICTOR CARRIES ACROSS PACKETS. THE PREAMBLE'S PREDICTOR IS USED ONCE, ON THE FIRST
// PACKET, AND IGNORED AFTERWARDS. *** This is the one thing about this format that the obvious
// reading gets wrong, and it was MEASURED here rather than reasoned about — decoded output was
// compared sample-for-sample against macOS `afconvert -f WAVE -d LEI16` over 40 files drawn at
// random from this library:
//
//     re-seed the predictor from every packet's preamble   -> max |error| 127, on all 40 files
//     seed from packet 0's preamble, then carry            -> max |error| 0,   on all 40 files
//
// The reason is in the encoding: the preamble has 16 bits for both fields, so the predictor keeps
// only its top 9 and the low 7 are lost. Re-seeding therefore injects up to +-127 of DC at every
// packet boundary — 1.45 ms apart — which is a quiet buzz, not an obvious failure, and would have
// shipped. The preamble predictor is a RESYNC HINT for a decoder that starts mid-stream; a decoder
// that starts at the beginning must not use it, because the encoder's own state was continuous.
// (Every file here has a first-packet preamble predictor of 0, so the distinction is only visible
// from packet 2 onward — which is exactly why the first packet matching is not evidence.)
const int kImaStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

const int kImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

inline int16_t ima_step(int nibble, int* predictor, int* index) {
  const int step = kImaStepTable[*index];
  int diff = step >> 3;
  if (nibble & 1) diff += step >> 2;
  if (nibble & 2) diff += step >> 1;
  if (nibble & 4) diff += step;
  if (nibble & 8) diff = -diff;

  int p = *predictor + diff;
  if (p > 32767) p = 32767;
  if (p < -32768) p = -32768;
  *predictor = p;

  int i = *index + kImaIndexTable[nibble];
  if (i < 0) i = 0;
  if (i > 88) i = 88;
  *index = i;

  return (int16_t)p;
}

// `data` is the packet stream; channels' packets are interleaved one packet at a time, each
// covering the SAME 64 frames. Output is interleaved 16-bit. Predictor/step state is PER CHANNEL
// and lives across the whole stream — see the block above for the measurement behind that.
bool decode_ima4(const unsigned char* data, size_t bytes, int channels, std::vector<int16_t>* out,
                 int* frames) {
  const size_t kPacket = 34, kFramesPerPacket = 64;
  if (channels <= 0 || channels > 8) return false;
  const size_t groupBytes = kPacket * (size_t)channels;
  const size_t groups = bytes / groupBytes;
  if (groups == 0) return false;

  *frames = (int)(groups * kFramesPerPacket);
  out->assign(groups * kFramesPerPacket * (size_t)channels, 0);

  int predictor[8] = {0}, index[8] = {0};

  for (size_t g = 0; g < groups; ++g) {
    for (int ch = 0; ch < channels; ++ch) {
      const unsigned char* pkt = data + g * groupBytes + (size_t)ch * kPacket;
      const uint16_t preamble = (uint16_t)((pkt[0] << 8) | pkt[1]);
      // The step index comes from every packet. The predictor comes from the FIRST one only.
      if (g == 0) predictor[ch] = (int16_t)(preamble & 0xFF80);
      index[ch] = preamble & 0x007F;
      if (index[ch] > 88) index[ch] = 88;

      int16_t* dst = out->data() + (g * kFramesPerPacket) * (size_t)channels + ch;
      for (size_t b = 0; b < 32; ++b) {
        const unsigned char byte = pkt[2 + b];
        *dst = ima_step(byte & 0x0F, &predictor[ch], &index[ch]);   // low nibble first
        dst += channels;
        *dst = ima_step((byte >> 4) & 0x0F, &predictor[ch], &index[ch]);
        dst += channels;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// CAF
// ---------------------------------------------------------------------------------------------
// 8-byte file header ('caff', u16 version, u16 flags), then chunks of a 4-byte type and a
// BIG-ENDIAN SIGNED 64-bit size. `desc` is always first and always 32 bytes. `data`'s first 4
// bytes are an edit count, not audio — reading them as samples is a click at the start of every
// sound, which is the kind of bug that gets blamed on the mixer.
//
// `pakt` matters here for a reason that is easy to miss: IMA4's packets are a fixed 64 frames, so
// the LAST one is zero-padded, and its header — not the payload length — is what says how many of
// those frames are real. Measured against afconvert over 40 files, ignoring it leaves 1 to 64
// frames of silence welded to the end of every effect. Inaudible on its own; audible as a gap when
// a short sound is looped, which is what the ambience beds do.
bool decode_caf(const std::vector<unsigned char>& buf, std::vector<int16_t>* pcm, int* frames,
                int* channels, int* rate) {
  size_t off = 8;
  double sampleRate = 0;
  char formatID[5] = {0};
  uint32_t formatFlags = 0, bytesPerPacket = 0, framesPerPacket = 0, chans = 0, bits = 0;
  const unsigned char* audio = nullptr;
  size_t audioBytes = 0;
  bool haveDesc = false;
  int64_t validFrames = -1;     // from `pakt`; -1 = absent, use the whole decoded payload
  int32_t primingFrames = 0;

  while (off + 12 <= buf.size()) {
    const unsigned char* h = buf.data() + off;
    const int64_t size = (int64_t)be64(h + 4);
    const size_t body = off + 12;
    // A size of -1 means "to the end of the file" (a live-recording convention CAF allows).
    const size_t avail = buf.size() - body;
    const size_t len = (size < 0 || (uint64_t)size > avail) ? avail : (size_t)size;

    if (!memcmp(h, "desc", 4)) {
      if (len < 32) { set_error("caf: desc chunk is %zu bytes, want 32", len); return false; }
      const unsigned char* d = buf.data() + body;
      uint64_t srBits = be64(d);
      memcpy(&sampleRate, &srBits, 8);       // CAF stores the rate as a big-endian IEEE double
      memcpy(formatID, d + 8, 4);
      formatFlags     = be32(d + 12);
      bytesPerPacket  = be32(d + 16);
      framesPerPacket = be32(d + 20);
      chans           = be32(d + 24);
      bits            = be32(d + 28);
      haveDesc = true;
    } else if (!memcmp(h, "data", 4)) {
      if (len < 4) { set_error("caf: data chunk too short"); return false; }
      audio = buf.data() + body + 4;        // skip mEditCount
      audioBytes = len - 4;
    } else if (!memcmp(h, "pakt", 4) && len >= 24) {
      // CAFPacketTableHeader: i64 mNumberPackets, i64 mNumberValidFrames, i32 mPrimingFrames,
      // i32 mRemainderFrames. (A variable-bitrate packet-size table follows for formats that need
      // one; IMA4 is constant-bitrate, so there is nothing after the header to read.)
      const unsigned char* d = buf.data() + body;
      validFrames = (int64_t)be64(d + 8);
      primingFrames = (int32_t)be32(d + 16);
    }
    off = body + len;
  }

  if (!haveDesc || !audio || chans == 0) { set_error("caf: no usable desc/data chunk"); return false; }

  *channels = (int)chans;
  *rate = (int)(sampleRate > 0 ? sampleRate : 44100);

  if (!memcmp(formatID, "ima4", 4)) {
    std::vector<int16_t> out;
    if (!decode_ima4(audio, audioBytes, (int)chans, &out, frames)) {
      set_error("caf: ima4 payload is %zu bytes, not a whole number of packets", audioBytes);
      return false;
    }
    // Trim the last packet's zero padding, and drop any encoder priming at the front. Both are
    // no-ops when `pakt` is absent or says nothing interesting, so this is not a special case.
    if (validFrames >= 0) {
      const int64_t first = primingFrames > 0 ? primingFrames : 0;
      const int64_t last = first + validFrames;
      if (first > 0 || last < (int64_t)*frames) {
        const int64_t clampedLast = last < (int64_t)*frames ? last : (int64_t)*frames;
        if (clampedLast > first) {
          out.erase(out.begin() + (long)(clampedLast * (int64_t)chans), out.end());
          out.erase(out.begin(), out.begin() + (long)(first * (int64_t)chans));
          *frames = (int)(clampedLast - first);
        }
      }
    }
    *pcm = out;
    return true;
  }

  if (!memcmp(formatID, "lpcm", 4)) {
    // Flags: bit 0 = float, bit 1 = little-endian. This library's 8 LPCM files are 16-bit LE
    // integer; anything else is reported rather than silently mangled.
    const bool isFloat = (formatFlags & 1) != 0;
    const bool littleEndian = (formatFlags & 2) != 0;
    if (isFloat || bits != 16) {
      set_error("caf: lpcm is %u-bit %s, only 16-bit integer is implemented",
                bits, isFloat ? "float" : "integer");
      return false;
    }
    const size_t n = audioBytes / 2;
    pcm->resize(n);
    for (size_t i = 0; i < n; ++i)
      (*pcm)[i] = littleEndian ? le16(audio + i * 2) : be16s(audio + i * 2);
    *frames = (int)(n / chans);
    return true;
  }

  (void)bytesPerPacket;
  (void)framesPerPacket;
  set_error("caf: format '%.4s' is not implemented (only lpcm and ima4)", formatID);
  return false;
}

// ---------------------------------------------------------------------------------------------
// AIFF
// ---------------------------------------------------------------------------------------------
// 12-byte FORM header, then chunks of a 4-byte type and a big-endian u32 size, PADDED TO EVEN.
// COMM gives channels / frames / bit depth / an 80-bit extended sample rate; SSND's first 8 bytes
// are an offset and a block size, not audio. One file in this library uses it.
bool decode_aiff(const std::vector<unsigned char>& buf, std::vector<int16_t>* pcm, int* frames,
                 int* channels, int* rate) {
  size_t off = 12;
  int chans = 0, bits = 0;
  uint32_t numFrames = 0, sr = 0;
  const unsigned char* audio = nullptr;
  size_t audioBytes = 0;

  while (off + 8 <= buf.size()) {
    const unsigned char* h = buf.data() + off;
    uint32_t size = be32(h + 4);
    const size_t body = off + 8;
    if (body + size > buf.size()) size = (uint32_t)(buf.size() - body);

    if (!memcmp(h, "COMM", 4) && size >= 18) {
      const unsigned char* d = buf.data() + body;
      chans = (int)((d[0] << 8) | d[1]);
      numFrames = be32(d + 2);
      bits = (int)((d[6] << 8) | d[7]);
      // 80-bit IEEE 754 extended: 1 sign + 15 exponent (bias 16383) + 64 explicit mantissa.
      // Every rate that matters is a small integer, so the integer part is all that is needed.
      const int exponent = (int)(((d[8] & 0x7F) << 8) | d[9]) - 16383;
      uint64_t mantissa = 0;
      for (int i = 0; i < 8; ++i) mantissa = (mantissa << 8) | d[10 + i];
      sr = (exponent >= 0 && exponent < 64) ? (uint32_t)(mantissa >> (63 - exponent)) : 0;
    } else if (!memcmp(h, "SSND", 4) && size >= 8) {
      audio = buf.data() + body + 8;
      audioBytes = size - 8;
    }
    off = body + size + (size & 1);        // chunks are word-aligned
  }

  if (chans <= 0 || !audio) { set_error("aiff: no usable COMM/SSND chunk"); return false; }
  if (bits != 16) { set_error("aiff: %d-bit is not implemented (only 16)", bits); return false; }

  const size_t n = audioBytes / 2;
  pcm->resize(n);
  for (size_t i = 0; i < n; ++i) (*pcm)[i] = be16s(audio + i * 2);   // AIFF is big-endian
  *channels = chans;
  *rate = sr ? (int)sr : 44100;
  *frames = numFrames ? (int)numFrames : (int)(n / (size_t)chans);
  return true;
}

}  // namespace

// =============================================================================================
// The wall
// =============================================================================================
extern "C" {

int eden_apple_audio_decode(const char* path, int16_t** pcm, int* frames, int* channels,
                            int* rate) {
  if (!path || !pcm || !frames || !channels || !rate) return 0;
  g_lastError[0] = '\0';

  const Container kind = sniff(path);

  if (kind == CONTAINER_WAV) {
    unsigned int ch = 0, sr = 0;
    drwav_uint64 total = 0;
    drwav_int16* s = drwav_open_file_and_read_pcm_frames_s16(path, &ch, &sr, &total, nullptr);
    if (!s) { set_error("wav: dr_wav could not open '%s'", path); return 0; }
    *pcm = (int16_t*)s;                    // drwav's allocator is malloc by default; see _free
    *frames = (int)total;
    *channels = (int)ch;
    *rate = (int)sr;
    return 1;
  }

  if (kind == CONTAINER_MP3) {
    drmp3_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    drmp3_uint64 total = 0;
    drmp3_int16* s = drmp3_open_file_and_read_pcm_frames_s16(path, &cfg, &total, nullptr);
    if (!s) { set_error("mp3: dr_mp3 could not open '%s'", path); return 0; }
    *pcm = (int16_t*)s;
    *frames = (int)total;
    *channels = (int)cfg.channels;
    *rate = (int)cfg.sampleRate;
    return 1;
  }

  if (kind == CONTAINER_CAF || kind == CONTAINER_AIFF) {
    std::vector<unsigned char> buf;
    if (!slurp(path, &buf)) { set_error("cannot read '%s'", path); return 0; }
    std::vector<int16_t> out;
    const bool ok = (kind == CONTAINER_CAF) ? decode_caf(buf, &out, frames, channels, rate)
                                            : decode_aiff(buf, &out, frames, channels, rate);
    if (!ok || out.empty()) return 0;
    // malloc, not new[]: eden_apple_audio_free is a plain C free on the Apple implementation too,
    // and the header's contract says the buffer belongs to the caller.
    const size_t bytes = out.size() * sizeof(int16_t);
    int16_t* mem = (int16_t*)malloc(bytes);
    if (!mem) { set_error("out of memory decoding '%s'", path); return 0; }
    memcpy(mem, out.data(), bytes);
    *pcm = mem;
    return 1;
  }

  set_error("unrecognised audio container in '%s'", path);
  return 0;
}

void eden_apple_audio_free(int16_t* pcm) { free(pcm); }

const char* eden_apple_audio_last_error(void) { return g_lastError[0] ? g_lastError : nullptr; }

}  // extern "C"

// =============================================================================================
// The streaming player
// =============================================================================================
// AVAudioPlayer's replacement, and structurally a much smaller thing than the name suggests: five
// of these ever exist (music plus four ambience beds), each plays one file, and the only states
// are playing / paused / stopped / finished.
//
// SDL_OpenAudioDeviceStream gives a LOGICAL device with a pull callback, which is exactly the
// AVAudioPlayer shape — SDL asks for more bytes, this decodes some. It is deliberately NOT bound
// to the effects device SimpleAudioEngine_native.mm opens: SDL3 multiplexes logical devices onto
// one physical device itself, and keeping them separate means a music track cannot be affected by
// the effects voice pool's stream churn.
//
// THREADING: the callback runs on SDL's audio thread. Every field below is touched from both it
// and the main thread, so every public entry point holds SDL_LockAudioStream, which is documented
// to exclude the callback. The decoders are not thread-safe and do not need to be — nothing
// touches one without that lock.
namespace {

// One SDL callback's worth of decode. 4096 frames is ~93 ms at 44.1 kHz: comfortably more than
// any device's buffer, so the callback never needs two passes, and small enough that a stop takes
// effect promptly.
const int kStreamChunkFrames = 4096;

struct StreamSource {
  enum Kind { NONE, MP3, WAV, MEMORY } kind = NONE;
  drmp3 mp3;
  drwav wav;
  std::vector<int16_t> mem;      // caf/aif: decoded up front, see open()
  size_t memFrame = 0;
  int channels = 0;
  int rate = 0;
};

}  // namespace

struct EdenApplePlayer {
  SDL_AudioStream* stream = nullptr;
  StreamSource src;
  std::vector<int16_t> scratch;
  bool loop = false;
  bool playing = false;
  bool finished = false;
};

namespace {

void source_close(StreamSource* s) {
  if (s->kind == StreamSource::MP3) drmp3_uninit(&s->mp3);
  if (s->kind == StreamSource::WAV) drwav_uninit(&s->wav);
  s->kind = StreamSource::NONE;
}

bool source_open(StreamSource* s, const char* path) {
  const Container kind = sniff(path);
  if (kind == CONTAINER_MP3) {
    if (!drmp3_init_file(&s->mp3, path, nullptr)) { set_error("mp3: cannot open '%s'", path); return false; }
    s->kind = StreamSource::MP3;
    s->channels = (int)s->mp3.channels;
    s->rate = (int)s->mp3.sampleRate;
    return true;
  }
  if (kind == CONTAINER_WAV) {
    if (!drwav_init_file(&s->wav, path, nullptr)) { set_error("wav: cannot open '%s'", path); return false; }
    s->kind = StreamSource::WAV;
    s->channels = (int)s->wav.channels;
    s->rate = (int)s->wav.sampleRate;
    return true;
  }
  // caf/aif have no streaming reader here, and do not need one: the five long channels are mp3
  // and wav in this library. Decoding one up front is correct rather than a fallback — it is
  // what "stream" means for a format whose decoder is already whole-file.
  int16_t* pcm = nullptr;
  int frames = 0, ch = 0, rate = 0;
  if (!eden_apple_audio_decode(path, &pcm, &frames, &ch, &rate)) return false;
  s->mem.assign(pcm, pcm + (size_t)frames * (size_t)ch);
  free(pcm);
  s->kind = StreamSource::MEMORY;
  s->channels = ch;
  s->rate = rate;
  s->memFrame = 0;
  return true;
}

void source_rewind(StreamSource* s) {
  switch (s->kind) {
    case StreamSource::MP3:    drmp3_seek_to_pcm_frame(&s->mp3, 0); break;
    case StreamSource::WAV:    drwav_seek_to_pcm_frame(&s->wav, 0); break;
    case StreamSource::MEMORY: s->memFrame = 0; break;
    default: break;
  }
}

// Returns frames actually produced; 0 means end of file.
size_t source_read(StreamSource* s, int16_t* dst, size_t frames) {
  switch (s->kind) {
    case StreamSource::MP3:
      return (size_t)drmp3_read_pcm_frames_s16(&s->mp3, frames, dst);
    case StreamSource::WAV:
      return (size_t)drwav_read_pcm_frames_s16(&s->wav, frames, dst);
    case StreamSource::MEMORY: {
      const size_t total = s->channels ? s->mem.size() / (size_t)s->channels : 0;
      const size_t n = (s->memFrame >= total) ? 0 : ((total - s->memFrame < frames)
                                                         ? total - s->memFrame : frames);
      if (n) memcpy(dst, s->mem.data() + s->memFrame * (size_t)s->channels,
                    n * (size_t)s->channels * sizeof(int16_t));
      s->memFrame += n;
      return n;
    }
    default:
      return 0;
  }
}

void SDLCALL stream_callback(void* userdata, SDL_AudioStream* stream, int additional, int total) {
  (void)total;
  EdenApplePlayer* p = (EdenApplePlayer*)userdata;
  if (!p || additional <= 0 || !p->playing || p->finished) return;

  const int frameBytes = p->src.channels * (int)sizeof(int16_t);
  if (frameBytes <= 0) return;
  int wanted = (additional + frameBytes - 1) / frameBytes;

  while (wanted > 0) {
    const size_t ask = (size_t)(wanted < kStreamChunkFrames ? wanted : kStreamChunkFrames);
    if (p->scratch.size() < ask * (size_t)p->src.channels)
      p->scratch.resize(ask * (size_t)p->src.channels);

    const size_t got = source_read(&p->src, p->scratch.data(), ask);
    if (got == 0) {
      if (!p->loop) { p->finished = true; p->playing = false; return; }
      source_rewind(&p->src);
      // A file that yields nothing even after a rewind is empty or broken; stop rather than spin.
      const size_t retry = source_read(&p->src, p->scratch.data(), ask);
      if (retry == 0) { p->finished = true; p->playing = false; return; }
      SDL_PutAudioStreamData(stream, p->scratch.data(), (int)(retry * (size_t)frameBytes));
      wanted -= (int)retry;
      continue;
    }
    SDL_PutAudioStreamData(stream, p->scratch.data(), (int)(got * (size_t)frameBytes));
    wanted -= (int)got;
  }
}

}  // namespace

extern "C" {

EdenApplePlayer* eden_apple_player_create(const char* path) {
  if (!path) return nullptr;
  g_lastError[0] = '\0';

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    set_error("SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
    return nullptr;
  }

  EdenApplePlayer* p = new EdenApplePlayer();
  if (!source_open(&p->src, path) || p->src.channels <= 0 || p->src.rate <= 0) {
    delete p;
    return nullptr;
  }

  SDL_AudioSpec spec;
  spec.format = SDL_AUDIO_S16;
  spec.channels = p->src.channels;
  spec.freq = p->src.rate;

  p->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                        stream_callback, p);
  if (!p->stream) {
    set_error("SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
    source_close(&p->src);
    delete p;
    return nullptr;
  }
  // Opened paused, like AVAudioPlayer: the caller sets loop and volume before -play.
  return p;
}

void eden_apple_player_destroy(EdenApplePlayer* p) {
  if (!p) return;
  if (p->stream) SDL_DestroyAudioStream(p->stream);   // unbinds and joins the callback first
  source_close(&p->src);
  delete p;
}

void eden_apple_player_play(EdenApplePlayer* p) {
  if (!p || !p->stream) return;
  SDL_LockAudioStream(p->stream);
  if (p->finished) { source_rewind(&p->src); p->finished = false; }
  p->playing = true;
  SDL_UnlockAudioStream(p->stream);
  SDL_ResumeAudioStreamDevice(p->stream);
}

void eden_apple_player_pause(EdenApplePlayer* p) {
  if (!p || !p->stream) return;
  SDL_PauseAudioStreamDevice(p->stream);
  SDL_LockAudioStream(p->stream);
  p->playing = false;
  SDL_UnlockAudioStream(p->stream);
}

void eden_apple_player_stop(EdenApplePlayer* p) {
  if (!p || !p->stream) return;
  SDL_PauseAudioStreamDevice(p->stream);
  SDL_LockAudioStream(p->stream);
  p->playing = false;
  p->finished = false;
  source_rewind(&p->src);
  SDL_UnlockAudioStream(p->stream);
  // Drop whatever was already queued, or a later -play resumes with stale audio from before the
  // rewind. AVAudioPlayer's -stop has no such buffer; this is the one place the two differ.
  SDL_ClearAudioStream(p->stream);
}

void eden_apple_player_rewind(EdenApplePlayer* p) {
  if (!p || !p->stream) return;
  SDL_LockAudioStream(p->stream);
  source_rewind(&p->src);
  p->finished = false;
  SDL_UnlockAudioStream(p->stream);
  SDL_ClearAudioStream(p->stream);
}

void eden_apple_player_set_loop(EdenApplePlayer* p, int loop) {
  if (!p || !p->stream) return;
  SDL_LockAudioStream(p->stream);
  p->loop = (loop != 0);
  SDL_UnlockAudioStream(p->stream);
}

void eden_apple_player_set_volume(EdenApplePlayer* p, float v) {
  if (!p || !p->stream) return;
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  SDL_SetAudioStreamGain(p->stream, v);
}

int eden_apple_player_is_playing(EdenApplePlayer* p) {
  if (!p || !p->stream) return 0;
  SDL_LockAudioStream(p->stream);
  const int r = (p->playing && !p->finished) ? 1 : 0;
  SDL_UnlockAudioStream(p->stream);
  return r;
}

}  // extern "C"
