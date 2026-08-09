/*
 * Copyright (C) 2013-2015 Willy Tarreau <w@1wt.eu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _SLZ_PRV_H
#define _SLZ_PRV_H

/* We have two macros UNALIGNED_LE_OK and UNALIGNED_FASTER. The latter indicates
 * that using unaligned data is faster than a simple shift. On x86 32-bit at
 * least it is not the case as the per-byte access is 30% faster. A core2-duo on
 * x86_64 is 7% faster to read one byte + shifting by 8 than to read one word,
 * but a core i5 is 7% faster doing the unaligned read, so we privilege more
 * recent implementations here.
 */
#if defined(__x86_64__)
#define UNALIGNED_LE_OK
#define UNALIGNED_FASTER
#define HAVE_FAST_MULT
#elif defined(__i386__) || defined(__i486__) || defined(__i586__) || defined(__i686__)
#define UNALIGNED_LE_OK
//#define UNALIGNED_FASTER
#elif defined(__ARMEL__) && defined(__ARM_ARCH_7A__)
#define UNALIGNED_LE_OK
#define UNALIGNED_FASTER
#elif defined(__ARM_ARCH_8A) || defined(__ARM_FEATURE_UNALIGNED)
#define UNALIGNED_LE_OK
#define UNALIGNED_FASTER
#define HAVE_FAST_MULT
#endif

/* returns the max of <a> and <b>, must be constant since evaluated twice */
#define MIN_C(a, b) ({				\
	(a < b) ? a : b;			\
})

/* returns the min of <a> and <b>; both must be of compatible types and are
 * evaluated exactly once.
 */
#define MIN(a, b) ({				\
	typeof(a) _a = (a);			\
	typeof(a) _b = (b);			\
	(_a < _b) ? _a : _b;			\
})

/* returns the min of <a> and <b>; both cast to type <t>, are evaluated exactly
 * once.
 */
#define MIN_T(t, a, b) ({			\
	t _a = (t)(a);				\
	t _b = (t)(b);				\
	(_a < _b) ? _a : _b;			\
})

/* returns the max of <a> and <b>, must be constant since evaluated twice */
#define MAX_C(a, b) ({				\
	(a > b) ? a : b;			\
})

/* returns the max of <a> and <b>; both must be of compatible types and are
 * evaluated exactly once.
 */
#define MAX(a, b) ({				\
	typeof(a) _a = (a);			\
	typeof(a) _b = (b);			\
	(_a > _b) ? _a : _b;			\
})

/* returns the max of <a> and <b>; both cast to type <t>, are evaluated exactly
 * once.
 */
#define MAX_T(t, a, b) ({			\
	t _a = (t)(a);				\
	t _b = (t)(b);				\
	(_a > _b) ? _a : _b;			\
})


/* writes an unaligned 32-bit word in little endian order */
static inline void write_le32(void *p, uint32_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	union {  uint32_t u32; } __attribute__((packed)) *u = p;
	u->u32 = v;
#else
	uint8_t *u8 = p;
	u8[0] = v;
	u8[1] = v >> 8;
	u8[2] = v >> 16;
	u8[3] = v >> 24;
#endif
}

/* writes an unaligned 16-bit word in little order */
static inline void write_le16(void *p, uint16_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	union {  uint16_t u16; } __attribute__((packed)) *u = p;
	u->u16 = v;
#else
	uint8_t *u8 = p;
	u8[0] = v;
	u8[1] = v >> 8;
#endif
}

/* uses the most suitable crc32 function to update crc on <buf, len> */
static inline uint32_t update_crc(uint32_t crc, const void *buf, int len)
{
	return slz_crc32_by4(crc, buf, len);
}

/* helper function to reverse bits in <input> which is expected to be
 * <len> bits long.
 */
static inline short rev_short(short input, int len)
{
	short ret = 0;

	while (len) {
		ret <<= 1;
		ret |= (input & 1);
		input >>= 1;
		len--;
	}

	return ret;
}

#endif
