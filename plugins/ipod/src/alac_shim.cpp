#include "alac_shim.h"

#include <cstdlib>
#include <cstring>
#include <new>

#include "ALACAudioTypes.h"
#include "ALACBitUtilities.h"
#include "ALACDecoder.h"
#include "ALACEncoder.h"

struct ipod_alac_enc {
  ALACEncoder enc;
  AudioFormatDescription in, out;
  uint32_t channels, bits, max_packet;
  uint8_t *packed; /* 24-bit input repacked to three bytes per sample */
};

extern "C" ipod_alac_enc *ipod_alac_enc_new(uint32_t sample_rate, uint32_t channels, uint32_t bits) {
  if ((channels != 1 && channels != 2) || (bits != 16 && bits != 24) || sample_rate == 0) return nullptr;
  ipod_alac_enc *e = new (std::nothrow) ipod_alac_enc;
  if (!e) return nullptr;
  e->channels = channels;
  e->bits = bits;
  e->packed = nullptr;
  std::memset(&e->in, 0, sizeof e->in);
  std::memset(&e->out, 0, sizeof e->out);
  e->in.mSampleRate = sample_rate;
  e->in.mFormatID = kALACFormatLinearPCM;
  e->in.mFormatFlags = kALACFormatFlagIsSignedInteger | kALACFormatFlagIsPacked; /* little endian */
  e->in.mFramesPerPacket = 1;
  e->in.mChannelsPerFrame = channels;
  e->in.mBitsPerChannel = bits;
  e->in.mBytesPerPacket = e->in.mBytesPerFrame = (bits / 8) * channels;
  e->out.mSampleRate = sample_rate;
  e->out.mFormatID = kALACFormatAppleLossless;
  e->out.mFormatFlags = bits == 16 ? 1 : 3; /* 16- or 24-bit source data */
  e->out.mFramesPerPacket = IPOD_ALAC_FRAMES_PER_PACKET;
  e->out.mChannelsPerFrame = channels;
  e->enc.SetFrameSize(IPOD_ALAC_FRAMES_PER_PACKET);
  if (e->enc.InitializeEncoder(e->out) != 0) {
    delete e;
    return nullptr;
  }
  e->max_packet = IPOD_ALAC_FRAMES_PER_PACKET * e->in.mBytesPerFrame + kALACMaxEscapeHeaderBytes;
  if (bits == 24) {
    e->packed = static_cast<uint8_t *>(std::malloc(IPOD_ALAC_FRAMES_PER_PACKET * e->in.mBytesPerFrame));
    if (!e->packed) {
      delete e;
      return nullptr;
    }
  }
  return e;
}

extern "C" void ipod_alac_enc_free(ipod_alac_enc *e) {
  if (!e) return;
  std::free(e->packed);
  delete e;
}

extern "C" uint32_t ipod_alac_enc_cookie(ipod_alac_enc *e, uint8_t *buf, uint32_t cap) {
  uint32_t size = e->enc.GetMagicCookieSize(e->channels);
  if (size > cap) return 0;
  e->enc.GetMagicCookie(buf, &size);
  return size;
}

extern "C" uint32_t ipod_alac_enc_max_packet(const ipod_alac_enc *e) { return e->max_packet; }

extern "C" uint32_t ipod_alac_enc_packet(ipod_alac_enc *e, const void *pcm, uint32_t frames, uint8_t *out) {
  if (frames == 0 || frames > IPOD_ALAC_FRAMES_PER_PACKET) return 0;
  unsigned char *in = (unsigned char *)pcm;
  if (e->bits == 24) {
    const int32_t *s = static_cast<const int32_t *>(pcm);
    uint32_t n = frames * e->channels;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t v = static_cast<uint32_t>(s[i]);
      e->packed[3 * i] = static_cast<uint8_t>(v);
      e->packed[3 * i + 1] = static_cast<uint8_t>(v >> 8);
      e->packed[3 * i + 2] = static_cast<uint8_t>(v >> 16);
    }
    in = e->packed;
  }
  int32_t bytes = static_cast<int32_t>(frames * e->in.mBytesPerFrame);
  if (e->enc.Encode(e->in, e->out, in, out, &bytes) != 0 || bytes <= 0) return 0;
  return static_cast<uint32_t>(bytes);
}

struct ipod_alac_dec {
  ALACDecoder dec;
};

extern "C" ipod_alac_dec *ipod_alac_dec_new(const uint8_t *cookie, uint32_t len) {
  ipod_alac_dec *d = new (std::nothrow) ipod_alac_dec;
  if (!d) return nullptr;
  if (d->dec.Init(const_cast<uint8_t *>(cookie), len) != 0) {
    delete d;
    return nullptr;
  }
  return d;
}

extern "C" void ipod_alac_dec_free(ipod_alac_dec *d) { delete d; }

extern "C" int32_t ipod_alac_dec_packet(ipod_alac_dec *d, const uint8_t *pkt, uint32_t len, uint8_t *out,
                                        uint32_t channels) {
  BitBuffer bits;
  BitBufferInit(&bits, const_cast<uint8_t *>(pkt), len);
  uint32_t frames = 0;
  if (d->dec.Decode(&bits, out, IPOD_ALAC_FRAMES_PER_PACKET, channels, &frames) != 0) return -1;
  return static_cast<int32_t>(frames);
}
