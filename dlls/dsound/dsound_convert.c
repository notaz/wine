/* DirectSound format conversion and mixing routines
 *
 * Copyright 2007 Maarten Lankhorst
 * Copyright 2011 Owen Rudge for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* 8 bits is unsigned, the rest is signed.
 * First I tried to reuse existing stuff from alsa-lib, after that
 * didn't work, I gave up and just went for individual hacks.
 *
 * 24 bit is expensive to do, due to unaligned access.
 * In dlls/winex11.drv/dib_convert.c convert_888_to_0888_asis there is a way
 * around it, but I'm happy current code works, maybe something for later.
 *
 * The ^ 0x80 flips the signed bit, this is the conversion from
 * signed (-128.. 0.. 127) to unsigned (0...255)
 * This is only temporary: All 8 bit data should be converted to signed.
 * then when fed to the sound card, it should be converted to unsigned again.
 *
 * Sound is LITTLE endian
 */

#include "config.h"

#undef _ISOC99_SOURCE
#define _ISOC99_SOURCE
#include <stdarg.h>
#include <math.h>

#define NONAMELESSSTRUCT
#define NONAMELESSUNION
#include "windef.h"
#include "winbase.h"
#include "mmsystem.h"
#include "winternl.h"
#include "wine/debug.h"
#include "dsound.h"
#include "dsound_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dsound);

#ifdef WORDS_BIGENDIAN
#define le16(x) RtlUshortByteSwap((x))
#define le32(x) RtlUlongByteSwap((x))
#else
#define le16(x) (x)
#define le32(x) (x)
#endif

#ifdef __ARM_ARCH_7A__
 #define ssat32_to_16(v) \
  asm("ssat %0,#16,%1" : "=r" (v) : "r" (v))
#else
 #define ssat32_to_16(v) do { \
  if (v < -32768) v = -32768; \
  else if (v > 32767) v = 32767; \
 } while (0)
#endif

static int get8(const IDirectSoundBufferImpl *dsb, DWORD pos, DWORD channel)
{
    const BYTE* buf = dsb->buffer->memory;
    buf += pos + channel;
    return (short)((buf[0] - 0x80) << 8);
}

static int get16(const IDirectSoundBufferImpl *dsb, DWORD pos, DWORD channel)
{
    const BYTE* buf = dsb->buffer->memory;
    const SHORT *sbuf = (const SHORT*)(buf + pos + 2 * channel);
    return le16(*sbuf);
}

static int get24(const IDirectSoundBufferImpl *dsb, DWORD pos, DWORD channel)
{
    LONG sample;
    const BYTE* buf = dsb->buffer->memory;
    buf += pos + 3 * channel;
    /* The next expression deliberately has an overflow for buf[2] >= 0x80,
       this is how negative values are made.
     */
    sample = (buf[0] << 8) | (buf[1] << 16) | (buf[2] << 24);
    return sample >> 8;
}

static int get32(const IDirectSoundBufferImpl *dsb, DWORD pos, DWORD channel)
{
    const BYTE* buf = dsb->buffer->memory;
    const LONG *sbuf = (const LONG*)(buf + pos + 4 * channel);
    return le32(*sbuf) >> 16;
}

static int getieee32(const IDirectSoundBufferImpl *dsb, DWORD pos, DWORD channel)
{
    const BYTE* buf = dsb->buffer->memory;
    const float *sbuf = (const float*)(buf + pos + 4 * channel);
    int l = *sbuf * 32767.0f;
    ssat32_to_16(l);
    return l;
}

const bitsgetfunc getbpp[5] = {get8, get16, get24, get32, getieee32};

int get_mono(const IDirectSoundBufferImpl *dsb, DWORD pos, DWORD channel)
{
    DWORD channels = dsb->pwfx->nChannels;
    DWORD c;
    int val = 0;
    /* XXX: does Windows include LFE into the mix? */
    for (c = 0; c < channels; c++)
        val += dsb->get_aux(dsb, pos, c);
    val /= channels;
    return val;
}

static inline unsigned char f_to_8(float value)
{
    if(value <= -1.f)
        return 0;
    if(value >= 1.f * 0x7f / 0x80)
        return 0xFF;
    return lrintf((value + 1.f) * 0x80);
}

static inline SHORT f_to_16(float value)
{
    if(value <= -1.f)
        return 0x8000;
    if(value >= 1.f * 0x7FFF / 0x8000)
        return 0x7FFF;
    return le16(lrintf(value * 0x8000));
}

static inline LONG f_to_24(float value)
{
    if(value <= -1.f)
        return 0x80000000;
    if(value >= 1.f * 0x7FFFFF / 0x800000)
        return 0x7FFFFF00;
    return lrintf(value * 0x80000000U);
}

static inline LONG f_to_32(float value)
{
    if(value <= -1.f)
        return 0x80000000;
    if(value >= 1.f * 0x7FFFFFFF / 0x80000000U)  /* this rounds to 1.f */
        return 0x7FFFFFFF;
    return le32(lrintf(value * 0x80000000U));
}

void putint(const IDirectSoundBufferImpl *dsb, DWORD pos, DWORD channel, int value)
{
    BYTE *buf = (BYTE *)dsb->device->tmp_buffer;
    int *fbuf = (int*)(buf + pos + sizeof(int) * channel);
    *fbuf = value;
}

void put_mono2stereo(const IDirectSoundBufferImpl *dsb, DWORD pos, DWORD channel, int value)
{
    putint(dsb, pos, 0, value);
    putint(dsb, pos, 1, value);
}

#ifdef __ARM_NEON__
void mixint32(int *src, int *dst, unsigned samples);
asm(
".align 3\n"
".globl mixint32\n"
"mixint32:\n"
"0:\n"
"  subs      r2, #8\n"
"  blt       1f\n"
"  vld1.32   {q0,q1}, [r0, :256]!\n"
"  vld1.32   {q2,q3}, [r1, :256]\n"
"  pld       [r0, #64*2]\n"
"  pld       [r1, #64*2]\n"
"  vadd.s32  q0, q2\n"
"  vadd.s32  q1, q3\n"
"  vst1.32   {q0,q1}, [r1, :256]!\n"
"  b         0b\n"
"1:\n"
"  adds      r2, #8\n"
"  bxeq      lr\n"
"2:\n"
"  ldr       r3, [r0], #4\n"
"  ldr       r12,[r1]\n"
"  subs      r2, #1\n"
"  add       r3, r12\n"
"  str       r3, [r1], #4\n"
"  bgt       2b\n"
"  nop\n"
"  bx        lr\n"
);
#else
void mixint32(int *src, int *dst, unsigned samples)
{
    TRACE("%p - %p %d\n", src, dst, samples);
    while (samples--)
        *(dst++) += *(src++);
}
#endif

static void norm8(int *src, unsigned char *dst, unsigned len)
{
    TRACE("%p - %p %d\n", src, dst, len);
    while (len--)
    {
        int l = *src;
        *src = 0;
        ssat32_to_16(l);
        *dst = (l >> 8) + 0x80;
        ++dst;
        ++src;
    }
}

#ifdef __ARM_NEON__
void norm16(int *src, SHORT *dst, unsigned len);
asm(
".align 3\n"
"norm16:\n"
"  lsr       r2, #1\n"
"  mov       r12, #0\n"
"  vdup.32   q2, r12\n"
"  vdup.32   q3, r12\n"
"0:\n"
"  pld       [r0, #64*2]\n"
"  subs      r2, #8\n"
"  blt       1f\n"
"  vld1.32   {q0,q1}, [r0]\n"
"  vst1.32   {q2,q3}, [r0]!\n"
"  vqmovn.s32 d0, q0\n"
"  vqmovn.s32 d1, q1\n"
"  vst1.16   {q0}, [r1]!\n"
"  b         0b\n"
"1:\n"
"  adds      r2, #8\n"
"  bxeq      lr\n"
"2:\n"
"  ldr       r3, [r0]\n"
"  subs      r2, #1\n"
"  str       r12,[r0], #4\n"
"  ssat      r3, #16, r3\n"
"  strh      r3, [r1], #2\n"
"  bgt       2b\n"
"  nop\n"
"  bx        lr\n"
);
#else
static void norm16(int *src, SHORT *dst, unsigned len)
{
    TRACE("%p - %p %d\n", src, dst, len);
    len /= 2;
    while (len--)
    {
        int l = *src;
        *src = 0;
        ssat32_to_16(l);
        *dst = le16(l);
        ++dst;
        ++src;
    }
}
#endif

static void norm24(int *src, BYTE *dst, unsigned len)
{
    TRACE("%p - %p %d\n", src, dst, len);
    len /= 3;
    while (len--)
    {
        LONG t;
        int l = *src;
        *src = 0;
        ssat32_to_16(l);
        t = l << 8;;
        dst[0] = (t >> 8) & 0xFF;
        dst[1] = (t >> 16) & 0xFF;
        dst[2] = t >> 24;
        dst += 3;
        ++src;
    }
}

#ifdef __ARM_NEON__
void norm32(int *src, INT *dst, unsigned len);
asm(
".align 3\n"
"norm32:\n"
"  lsr       r2, #2\n"
"  mov       r12, #0\n"
"  vdup.32   q2, r12\n"
"  vdup.32   q3, r12\n"
"0:\n"
"  pld       [r0, #64*2]\n"
"  subs      r2, #8\n"
"  blt       1f\n"
"  vld1.32   {q0,q1}, [r0]\n"
"  vst1.32   {q2,q3}, [r0]!\n"
"  vqshl.s32 q0, q0, #16\n"
"  vqshl.s32 q1, q1, #16\n"
"  vst1.32   {q0,q1}, [r1]!\n"
"  b         0b\n"
"1:\n"
"  adds      r2, #8\n"
"  bxeq      lr\n"
"2:\n"
"  ldr       r3, [r0]\n"
"  subs      r2, #1\n"
"  str       r12,[r0], #4\n"
"  ssat      r3, #32, r3, lsl #16\n"
"  str       r3, [r1], #4\n"
"  bgt       2b\n"
"  nop\n"
"  bx        lr\n"
);
#else
static void norm32(int *src, INT *dst, unsigned len)
{
    TRACE("%p - %p %d\n", src, dst, len);
    len /= 4;
    while (len--)
    {
        int l = *src;
        *src = 0;
        ssat32_to_16(l);
        *dst = le32(l << 16);
        ++dst;
        ++src;
    }
}
#endif

static void normieee32(int *src, float *dst, unsigned len)
{
    TRACE("%p - %p %d\n", src, dst, len);
    len /= 4;
    while (len--)
    {
        int l = *src;
        *src = 0;
        ssat32_to_16(l);
        *dst = (float)l / 32768.0f;
        ++dst;
        ++src;
    }
}

const normfunc normfunctions[5] = {
    (normfunc)norm8,
    (normfunc)norm16,
    (normfunc)norm24,
    (normfunc)norm32,
    (normfunc)normieee32
};
