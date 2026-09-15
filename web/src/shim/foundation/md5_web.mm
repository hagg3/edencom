// md5_web.mm — portable replacement for the seam-excluded Classes/md5.c.
//
// WHY REPLACED RATHER THAN SHIMMED: the original is 100 lines of Apple-specific plumbing around
// 3 lines of hashing. It reads the file through CFReadStreamCreateWithFile/CFReadStreamRead,
// builds its path with CFURLCreateWithFileSystemPath, formats its result with
// CFStringCreateWithCString, and hashes with CommonCrypto's CC_MD5_*. Shimming that would mean
// implementing a CFReadStream and a CFString factory purely to serve ONE call site — versus
// re-expressing the same function over stdio, which is what this file does. This follows the
// same "reuse the original .h, replace only the .{c,mm}" pattern as the rest of the seam:
// Classes/md5.h is untouched and still declares the signature everyone calls.
//
// The MD5 itself is a real implementation (RFC 1321), not a stub — the one caller,
// Util.mm's screenshot path, feeds the digest to FileManager::setImageHash, which the world-
// sharing upload uses to tell whether a preview image changed. A fake or empty digest there
// would look like it worked and quietly break dedupe at Stage P6.
//
// MD5 is used here as a CONTENT FINGERPRINT, not for security. That is the original's choice and
// it is preserved deliberately: the value is compared against hashes stored by the existing
// edengame.net service, so changing the algorithm would break compatibility with worlds and
// previews already uploaded by the shipped iOS app.
#import "NSString.h"

// Reaches into the parent tree by relative path on purpose — the same pattern the seam files use
// (see src/seam/EAGLView_web.mm): quoted includes resolve relative to THIS file, so no -I ordering
// is involved and there is no chance of picking up a different md5.h.
#include "../../../../Classes/md5.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>   // uint32_t/uint64_t — see NSObject.mm (Phase N Stage 3.2)

namespace {

struct MD5Context {
  uint32_t state[4];
  uint64_t bitCount;
  unsigned char buffer[64];
};

inline uint32_t rotateLeft(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

const uint32_t kSine[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

const int kShift[64] = {
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

void md5Init(MD5Context *ctx) {
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xefcdab89;
  ctx->state[2] = 0x98badcfe;
  ctx->state[3] = 0x10325476;
  ctx->bitCount = 0;
}

void md5Transform(MD5Context *ctx, const unsigned char block[64]) {
  uint32_t m[16];
  for (int i = 0; i < 16; i++) {
    // Little-endian assembly done byte-wise rather than by casting to uint32_t*: wasm is
    // little-endian so a cast would work, but the block pointer has no alignment guarantee.
    m[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
           ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
  }

  uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];

  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    const uint32_t temp = d;
    d = c;
    c = b;
    b = b + rotateLeft(a + f + kSine[i] + m[g], kShift[i]);
    a = temp;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
}

void md5Update(MD5Context *ctx, const unsigned char *data, size_t length) {
  size_t bufferFill = (size_t)((ctx->bitCount / 8) % 64);
  ctx->bitCount += (uint64_t)length * 8;

  for (size_t i = 0; i < length; i++) {
    ctx->buffer[bufferFill++] = data[i];
    if (bufferFill == 64) {
      md5Transform(ctx, ctx->buffer);
      bufferFill = 0;
    }
  }
}

void md5Final(MD5Context *ctx, unsigned char digest[16]) {
  const uint64_t bitCount = ctx->bitCount;
  size_t bufferFill = (size_t)((bitCount / 8) % 64);

  unsigned char padding[64];
  padding[0] = 0x80;
  memset(padding + 1, 0, sizeof(padding) - 1);
  // Pad to 56 mod 64, leaving 8 bytes for the length.
  md5Update(ctx, padding, (bufferFill < 56) ? (56 - bufferFill) : (120 - bufferFill));

  unsigned char lengthBytes[8];
  for (int i = 0; i < 8; i++) lengthBytes[i] = (unsigned char)((bitCount >> (8 * i)) & 0xff);
  // Appended directly rather than through md5Update, which would fold it back into bitCount.
  memcpy(ctx->buffer + 56, lengthBytes, 8);
  md5Transform(ctx, ctx->buffer);

  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      digest[i * 4 + j] = (unsigned char)((ctx->state[i] >> (8 * j)) & 0xff);
    }
  }
}

}  // namespace

extern "C" {

// Signature and ownership contract come from the unmodified Classes/md5.h. "Create" in the name
// means the caller owns the result — but note the ONE caller (Util.mm) has its CFRelease
// commented out, so this leaks exactly as it does on device. That is audit-backlog territory
// (the L-cluster), not something to silently change while porting: per web-port-plan.md
// principle #1, port the behavior first, fix it as a visible patch afterwards.
CFStringRef FileMD5HashCreateWithPath(CFStringRef filePath, size_t chunkSizeForReadingData) {
  if (!filePath) return NULL;

  // Toll-free bridged, as everywhere else in this tree: the caller passes an NSString* cast to
  // CFStringRef (Util.mm: `FileMD5HashCreateWithPath((CFStringRef)file_name, …)`).
  NSString *path = (NSString *)filePath;
  const char *cPath = [path UTF8String];
  if (!cPath) return NULL;

  FILE *file = fopen(cPath, "rb");
  if (!file) return NULL;

  MD5Context ctx;
  md5Init(&ctx);

  // The original streams the file in chunkSizeForReadingData-sized reads specifically so a large
  // world preview never lands in memory whole; that behavior is preserved rather than replaced
  // with a slurp.
  size_t chunkSize = chunkSizeForReadingData;
  if (chunkSize == 0 || chunkSize > 65536) chunkSize = FileHashDefaultChunkSizeForReadingData;

  unsigned char buffer[65536];
  for (;;) {
    const size_t read = fread(buffer, 1, chunkSize, file);
    if (read == 0) break;
    md5Update(&ctx, buffer, read);
  }
  fclose(file);

  unsigned char digest[16];
  md5Final(&ctx, digest);

  // Lowercase hex, matching CommonCrypto-plus-%02x formatting in the original.
  char hex[33];
  for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  hex[32] = '\0';

  return (CFStringRef)[NSString stringWithUTF8String:hex];
}

}  // extern "C"
