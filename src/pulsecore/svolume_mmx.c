/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/timeval.h>
#include <pulsecore/random.h>
#include <pulsecore/macro.h>
#include <pulsecore/g711.h>
#include <pulsecore/core-util.h>

#include "cpu-x86.h"

#include "sample-util.h"
#include "endianmacros.h"

#if 0
static void
pa_volume_u8_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  for (channel = 0; length; length--) {
    int32_t t, hi, lo;

    hi = volumes[channel] >> 16;
    lo = volumes[channel] & 0xFFFF;

    t = (int32_t) *samples - 0x80;
    t = ((t * lo) >> 16) + (t * hi);
    t = PA_CLAMP_UNLIKELY(t, -0x80, 0x7F);
    *samples++ = (uint8_t) (t + 0x80);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_alaw_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  for (channel = 0; length; length--) {
    int32_t t, hi, lo;

    hi = volumes[channel] >> 16;
    lo = volumes[channel] & 0xFFFF;

    t = (int32_t) st_alaw2linear16(*samples);
    t = ((t * lo) >> 16) + (t * hi);
    t = PA_CLAMP_UNLIKELY(t, -0x8000, 0x7FFF);
    *samples++ = (uint8_t) st_13linear2alaw((int16_t) t >> 3);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_ulaw_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  for (channel = 0; length; length--) {
    int32_t t, hi, lo;

    hi = volumes[channel] >> 16;
    lo = volumes[channel] & 0xFFFF;

    t = (int32_t) st_ulaw2linear16(*samples);
    t = ((t * lo) >> 16) + (t * hi);
    t = PA_CLAMP_UNLIKELY(t, -0x8000, 0x7FFF);
    *samples++ = (uint8_t) st_14linear2ulaw((int16_t) t >> 2);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}
#endif

/* in s: 2 int16_t samples
 * in v: 2 int32_t volumes, fixed point 16:16
 * out s: contains scaled and clamped int16_t samples.
 *
 * We calculate the high 32 bits of a 32x16 multiply which we then
 * clamp to 16 bits. The calulcation is:
 *
 *  vl = (v & 0xffff)
 *  vh = (v >> 16)
 *  s = ((s * vl) >> 16) + (s * vh);
 *
 * For the first multiply we have to do a sign correction as we need to
 * multiply a signed int with an unsigned int. Hacker's delight 8-3 gives a
 * simple formula to correct the sign of the high word after the signed
 * multiply.
 */
#define VOLUME_32x16(s,v)                  /* .. |   vh  |   vl  | */                   \
      " pxor  %%mm4, %%mm4           \n\t" /* .. |    0  |    0  | */                   \
      " punpcklwd %%mm4, "#s"        \n\t" /* .. |    0  |   p0  | */                   \
      " pcmpgtw "#v", %%mm4          \n\t" /* .. |    0  | s(vl) | */                   \
      " pand "#s", %%mm4             \n\t" /* .. |    0  |  (p0) |  (vl >> 15) & p */   \
      " movq %%mm6, %%mm5            \n\t" /* .. |  ffff |   0   | */                   \
      " pand "#v", %%mm5             \n\t" /* .. |   vh  |   0   | */                   \
      " por %%mm5, %%mm4             \n\t" /* .. |   vh  |  (p0) | */                   \
      " pmulhw "#s", "#v"            \n\t" /* .. |    0  | vl*p0 | */                   \
      " paddw %%mm4, "#v"            \n\t" /* .. |   vh  | vl*p0 | vh + sign correct */ \
      " pslld $16, "#s"              \n\t" /* .. |   p0  |    0  | */                   \
      " por %%mm7, "#s"              \n\t" /* .. |   p0  |    1  | */                   \
      " pmaddwd "#s", "#v"           \n\t" /* .. |    p0 * v0    | */                   \
      " packssdw "#v", "#v"          \n\t" /* .. | p1*v1 | p0*v0 | */

/* approximately advances %3 = (%3 + a) % b. This function requires that
 * a <= b. */
#define MOD_ADD(a,b) \
      " add "#a", %3                 \n\t" \
      " mov %3, %4                   \n\t" \
      " sub "#b", %4                 \n\t" \
      " cmp "#b", %3                 \n\t" \
      " cmovae %4, %3                \n\t" 

/* swap 16 bits */
#define SWAP_16(s) \
      " movq "#s", %%mm4             \n\t" /* .. |  h  l |  */ \
      " psrlw $8, %%mm4              \n\t" /* .. |  0  h |  */ \
      " psllw $8, "#s"               \n\t" /* .. |  l  0 |  */ \
      " por %%mm4, "#s"              \n\t" /* .. |  l  h |  */

/* swap 2 registers 16 bits for better pairing */
#define SWAP_16_2(s1,s2) \
      " movq "#s1", %%mm4            \n\t" /* .. |  h  l |  */ \
      " movq "#s2", %%mm5            \n\t"                     \
      " psrlw $8, %%mm4              \n\t" /* .. |  0  h |  */ \
      " psrlw $8, %%mm5              \n\t"                     \
      " psllw $8, "#s1"              \n\t" /* .. |  l  0 |  */ \
      " psllw $8, "#s2"              \n\t"                     \
      " por %%mm4, "#s1"             \n\t" /* .. |  l  h |  */ \
      " por %%mm5, "#s2"             \n\t"

static void
pa_volume_s16ne_mmx (int16_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  pa_reg_x86 channel, temp;

  /* the max number of samples we process at a time, this is also the max amount
   * we overread the volume array, which should have enough padding. */
  channels = MAX (4, channels);

  __asm__ __volatile__ (
    " xor %3, %3                    \n\t"
    " sar $1, %2                    \n\t" /* length /= sizeof (int16_t) */
    " pcmpeqw %%mm6, %%mm6          \n\t" /* .. |  ffff |  ffff | */
    " pcmpeqw %%mm7, %%mm7          \n\t" /* .. |  ffff |  ffff | */
    " pslld  $16, %%mm6             \n\t" /* .. |  ffff |     0 | */
    " psrld  $31, %%mm7             \n\t" /* .. |     0 |     1 | */

    " test $1, %2                   \n\t" /* check for odd samples */
    " je 2f                         \n\t" 

    " movd (%1, %3, 4), %%mm0       \n\t" /* |  v0h  |  v0l  | */
    " movw (%0), %4                 \n\t" /*     ..  |  p0   | */
    " movd %4, %%mm1                \n\t" 
    VOLUME_32x16 (%%mm1, %%mm0)
    " movd %%mm0, %4                \n\t" /*     ..  | p0*v0 | */
    " movw %4, (%0)                 \n\t" 
    " add $2, %0                    \n\t"
    MOD_ADD ($1, %5)

    "2:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 2 samples at a time */
    " test $1, %2                   \n\t" /* check for odd samples */
    " je 4f                         \n\t" 

    "3:                             \n\t" /* do samples in groups of 2 */
    " movq (%1, %3, 4), %%mm0       \n\t" /* |  v1h  |  v1l  |  v0h  |  v0l  | */
    " movd (%0), %%mm1              \n\t" /*              .. |   p1  |  p0   | */ 
    VOLUME_32x16 (%%mm1, %%mm0)
    " movd %%mm0, (%0)              \n\t" /*              .. | p1*v1 | p0*v0 | */
    " add $4, %0                    \n\t"
    MOD_ADD ($2, %5)

    "4:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 4 samples at a time */
    " cmp $0, %2                    \n\t"
    " je 6f                         \n\t"

    "5:                             \n\t" /* do samples in groups of 4 */
    " movq (%1, %3, 4), %%mm0       \n\t" /* |  v1h  |  v1l  |  v0h  |  v0l  | */ 
    " movq 8(%1, %3, 4), %%mm2      \n\t" /* |  v3h  |  v3l  |  v2h  |  v2l  | */
    " movd (%0), %%mm1              \n\t" /*              .. |   p1  |  p0   | */
    " movd 4(%0), %%mm3             \n\t" /*              .. |   p3  |  p2   | */
    VOLUME_32x16 (%%mm1, %%mm0)
    VOLUME_32x16 (%%mm3, %%mm2)
    " movd %%mm0, (%0)              \n\t" /*              .. | p1*v1 | p0*v0 | */
    " movd %%mm2, 4(%0)             \n\t" /*              .. | p3*v3 | p2*v2 | */
    " add $8, %0                    \n\t"
    MOD_ADD ($4, %5)
    " dec %2                        \n\t"
    " jne 5b                        \n\t"

    "6:                             \n\t"
    " emms                          \n\t"

    : "+r" (samples), "+r" (volumes), "+r" (length), "=D" ((pa_reg_x86)channel), "=&r" (temp)
    : "r" ((pa_reg_x86)channels)
    : "cc"
  );
}

static void
pa_volume_s16re_mmx (int16_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  pa_reg_x86 channel, temp;

  /* the max number of samples we process at a time, this is also the max amount
   * we overread the volume array, which should have enough padding. */
  channels = MAX (4, channels);

  __asm__ __volatile__ (
    " xor %3, %3                    \n\t"
    " sar $1, %2                    \n\t" /* length /= sizeof (int16_t) */
    " pcmpeqw %%mm6, %%mm6          \n\t" /* .. |  ffff |  ffff | */
    " pcmpeqw %%mm7, %%mm7          \n\t" /* .. |  ffff |  ffff | */
    " pslld  $16, %%mm6             \n\t" /* .. |  ffff |     0 | */
    " psrld  $31, %%mm7             \n\t" /* .. |     0 |     1 | */

    " test $1, %2                   \n\t" /* check for odd samples */
    " je 2f                         \n\t" 

    " movd (%1, %3, 4), %%mm0       \n\t" /* |  v0h  |  v0l  | */
    " movw (%0), %4                 \n\t" /*     ..  |  p0   | */
    " rorw $8, %4                   \n\t"
    " movd %4, %%mm1                \n\t" 
    VOLUME_32x16 (%%mm1, %%mm0)
    " movd %%mm0, %4                \n\t" /*     ..  | p0*v0 | */
    " rorw $8, %4                   \n\t"
    " movw %4, (%0)                 \n\t" 
    " add $2, %0                    \n\t"
    MOD_ADD ($1, %5)

    "2:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 2 samples at a time */
    " test $1, %2                   \n\t" /* check for odd samples */
    " je 4f                         \n\t" 

    "3:                             \n\t" /* do samples in groups of 2 */
    " movq (%1, %3, 4), %%mm0       \n\t" /* |  v1h  |  v1l  |  v0h  |  v0l  | */
    " movd (%0), %%mm1              \n\t" /*              .. |   p1  |  p0   | */ 
    SWAP_16 (%%mm1)
    VOLUME_32x16 (%%mm1, %%mm0)
    SWAP_16 (%%mm0)
    " movd %%mm0, (%0)              \n\t" /*              .. | p1*v1 | p0*v0 | */
    " add $4, %0                    \n\t"
    MOD_ADD ($2, %5)

    "4:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 4 samples at a time */
    " cmp $0, %2                    \n\t"
    " je 6f                         \n\t"

    "5:                             \n\t" /* do samples in groups of 4 */
    " movq (%1, %3, 4), %%mm0       \n\t" /* |  v1h  |  v1l  |  v0h  |  v0l  | */ 
    " movq 8(%1, %3, 4), %%mm2      \n\t" /* |  v3h  |  v3l  |  v2h  |  v2l  | */
    " movd (%0), %%mm1              \n\t" /*              .. |   p1  |  p0   | */
    " movd 4(%0), %%mm3             \n\t" /*              .. |   p3  |  p2   | */
    SWAP_16_2 (%%mm1, %%mm3)
    VOLUME_32x16 (%%mm1, %%mm0)
    VOLUME_32x16 (%%mm3, %%mm2)
    SWAP_16_2 (%%mm0, %%mm2)
    " movd %%mm0, (%0)              \n\t" /*              .. | p1*v1 | p0*v0 | */
    " movd %%mm2, 4(%0)             \n\t" /*              .. | p3*v3 | p2*v2 | */
    " add $8, %0                    \n\t"
    MOD_ADD ($4, %5)
    " dec %2                        \n\t"
    " jne 5b                        \n\t"

    "6:                             \n\t"
    " emms                          \n\t"

    : "+r" (samples), "+r" (volumes), "+r" (length), "=D" ((pa_reg_x86)channel), "=&r" (temp)
    : "r" ((pa_reg_x86)channels)
    : "cc"
  );
}

#if 0
static void
pa_volume_float32ne_mmx (float *samples, float *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (float);

  for (channel = 0; length; length--) {
    *samples++ *= volumes[channel];

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_float32re_mmx (float *samples, float *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (float);

  for (channel = 0; length; length--) {
    float t;

    t = PA_FLOAT32_SWAP(*samples);
    t *= volumes[channel];
    *samples++ = PA_FLOAT32_SWAP(t);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s32ne_mmx (int32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (int32_t);

  for (channel = 0; length; length--) {
    int64_t t;

    t = (int64_t)(*samples);
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    *samples++ = (int32_t) t;

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s32re_mmx (int32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (int32_t);

  for (channel = 0; length; length--) {
    int64_t t;

    t = (int64_t) PA_INT32_SWAP(*samples);
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    *samples++ = PA_INT32_SWAP((int32_t) t);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s24ne_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;
  uint8_t *e;

  e = samples + length;

  for (channel = 0; samples < e; samples += 3) {
    int64_t t;

    t = (int64_t)((int32_t) (PA_READ24NE(samples) << 8));
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    PA_WRITE24NE(samples, ((uint32_t) (int32_t) t) >> 8);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s24re_mmx (uint8_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;
  uint8_t *e;

  e = samples + length;

  for (channel = 0; samples < e; samples += 3) {
    int64_t t;

    t = (int64_t)((int32_t) (PA_READ24RE(samples) << 8));
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    PA_WRITE24RE(samples, ((uint32_t) (int32_t) t) >> 8);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s24_32ne_mmx (uint32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (uint32_t);

  for (channel = 0; length; length--) {
    int64_t t;

    t = (int64_t) ((int32_t) (*samples << 8));
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    *samples++ = ((uint32_t) ((int32_t) t)) >> 8;

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}

static void
pa_volume_s24_32re_mmx (uint32_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  unsigned channel;

  length /= sizeof (uint32_t);

  for (channel = 0; length; length--) {
    int64_t t;

    t = (int64_t) ((int32_t) (PA_UINT32_SWAP(*samples) << 8));
    t = (t * volumes[channel]) >> 16;
    t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
    *samples++ = PA_UINT32_SWAP(((uint32_t) ((int32_t) t)) >> 8);

    if (PA_UNLIKELY(++channel >= channels))
      channel = 0;
  }
}
#endif

#undef RUN_TEST

#ifdef RUN_TEST
#define CHANNELS 2
#define SAMPLES 1021
#define TIMES 1000
#define PADDING 16

static void run_test (void) {
  int16_t samples[SAMPLES];
  int16_t samples_ref[SAMPLES];
  int16_t samples_orig[SAMPLES];
  int32_t volumes[CHANNELS + PADDING];
  int i, j, padding;
  pa_do_volume_func_t func;
  struct timeval start, stop;

  func = pa_get_volume_func (PA_SAMPLE_S16NE);

  printf ("checking MMX %zd\n", sizeof (samples));

  pa_random (samples, sizeof (samples));
  memcpy (samples_ref, samples, sizeof (samples));
  memcpy (samples_orig, samples, sizeof (samples));

  for (i = 0; i < CHANNELS; i++)
    volumes[i] = rand() >> 1;
  for (padding = 0; padding < PADDING; padding++, i++)
    volumes[i] = volumes[padding];

  func (samples_ref, volumes, CHANNELS, sizeof (samples));
  pa_volume_s16ne_mmx (samples, volumes, CHANNELS, sizeof (samples));
  for (i = 0; i < SAMPLES; i++) {
    if (samples[i] != samples_ref[i]) {
      printf ("%d: %04x != %04x (%04x * %04x)\n", i, samples[i], samples_ref[i],
          samples_orig[i], volumes[i % CHANNELS]);
    }
  }

  pa_gettimeofday(&start);
  for (j = 0; j < TIMES; j++) {
    memcpy (samples, samples_orig, sizeof (samples));
    pa_volume_s16ne_mmx (samples, volumes, CHANNELS, sizeof (samples));
  }
  pa_gettimeofday(&stop);
  pa_log_info("MMX: %llu usec.", (long long unsigned int)pa_timeval_diff (&stop, &start));

  pa_gettimeofday(&start);
  for (j = 0; j < TIMES; j++) {
    memcpy (samples_ref, samples_orig, sizeof (samples));
    func (samples_ref, volumes, CHANNELS, sizeof (samples));
  }
  pa_gettimeofday(&stop);
  pa_log_info("ref: %llu usec.", (long long unsigned int)pa_timeval_diff (&stop, &start));
}
#endif

void pa_volume_func_init_mmx (pa_cpu_x86_flag_t flags) {
  pa_log_info("Initialising MMX optimized functions.");

#ifdef RUN_TEST
  run_test ();
#endif

  pa_set_volume_func (PA_SAMPLE_S16NE,     (pa_do_volume_func_t) pa_volume_s16ne_mmx);
  pa_set_volume_func (PA_SAMPLE_S16RE,     (pa_do_volume_func_t) pa_volume_s16re_mmx);
}
