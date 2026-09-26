#include <cstring>
#include <iostream>
#include <vector>
#include "main.hpp"

thread_local uint32 seed32_1, seed32_2;

namespace {

// SIMD helpers built on GCC vector extensions: they compile to AVX2 (or 2x
// SSE2) on x86-64 and 2x NEON on aarch64 without any intrinsics.
typedef unsigned int vsimd __attribute__((vector_size(32)));
enum { VLanes = sizeof(vsimd) / sizeof(uint32) };
static_assert((1 << 16) % VLanes == 0, "scan size must be a multiple of the lane count");

inline vsimd vbcast(uint32 x)
{
	vsimd r;
	for (unsigned i = 0; i < VLanes; ++i)
		r[i] = x;
	return r;
}

inline vsimd FF(vsimd b, vsimd c, vsimd d) { return d ^ (b & (c ^ d)); }
inline vsimd GG(vsimd b, vsimd c, vsimd d) { return c ^ (d & (b ^ c)); }
inline vsimd HH(vsimd b, vsimd c, vsimd d) { return b ^ c ^ d; }
inline vsimd II(vsimd b, vsimd c, vsimd d) { return c ^ (b | ~d); }
inline vsimd RL(vsimd x, unsigned n) { return (x << n) | (x >> (32-n)); }
inline vsimd RR(vsimd x, unsigned n) { return (x >> n) | (x << (32-n)); }

} // namespace

void find_block0(uint32 block[], const uint32 IV[])
{
	uint32 Q[68] = { IV[0], IV[3], IV[2], IV[1] };

	// The mask tables are constant and shared read-only by all searches.
	static const std::vector<uint32> q4mask = [] {
		std::vector<uint32> m(1<<4);
		for (unsigned k = 0; k < m.size(); ++k)
			m[k] = ((k<<2) ^ (k<<26)) & 0x38000004;
		return m; }();

	static const std::vector<uint32> q9q10mask = [] {
		std::vector<uint32> m(1<<3);
		for (unsigned k = 0; k < m.size(); ++k)
			m[k] = ((k<<13) ^ (k<<4)) & 0x2060;
		return m; }();

	static const std::vector<uint32> q9mask = [] {
		std::vector<uint32> m(1<<16);
		for (unsigned k = 0; k < m.size(); ++k)
			m[k] = ((k<<1) ^ (k<<2) ^ (k<<5) ^ (k<<7) ^ (k<<8) ^ (k<<10) ^ (k<<11) ^ (k<<13)) & 0x0eb94f16;
		return m; }();

	while (true)
	{
		check_abort();
		Q[Qoff + 1] = xrng64();
		Q[Qoff + 3] = (xrng64() & 0xfe87bc3f) | 0x017841c0;
		Q[Qoff + 4] = (xrng64() & 0x44000033) | 0x000002c0 | (Q[Qoff + 3] & 0x0287bc00);
		Q[Qoff + 5] = 0x41ffffc8 | (Q[Qoff + 4] & 0x04000033);
		Q[Qoff + 6] = 0xb84b82d6;
		Q[Qoff + 7] = (xrng64() & 0x68000084) | 0x02401b43;
		Q[Qoff + 8] = (xrng64() & 0x2b8f6e04) | 0x005090d3 | (~Q[Qoff + 7] & 0x40000000);
		Q[Qoff + 9] = 0x20040068 | (Q[Qoff + 8] & 0x00020000) | (~Q[Qoff + 8] & 0x40000000);
		Q[Qoff + 10] = (xrng64() & 0x40000000) | 0x1040b089;
		Q[Qoff + 11] = (xrng64() & 0x10408008) | 0x0fbb7f16 | (~Q[Qoff + 10] & 0x40000000);
		Q[Qoff + 12] = (xrng64() & 0x1ed9df7f) | 0x00022080 | (~Q[Qoff + 11] & 0x40200000);
		Q[Qoff + 13] = (xrng64() & 0x5efb4f77) | 0x20049008;
		Q[Qoff + 14] = (xrng64() & 0x1fff5f77) | 0x0000a088 | (~Q[Qoff + 13] & 0x40000000);
		Q[Qoff + 15] = (xrng64() & 0x5efe7ff7) | 0x80008000 | (~Q[Qoff + 14] & 0x00010000);
		Q[Qoff + 16] = (xrng64() & 0x1ffdffff) | 0xa0000000 | (~Q[Qoff + 15] & 0x40020000);

		MD5_REVERSE_STEP(0, 0xd76aa478, 7);
		MD5_REVERSE_STEP(6, 0xa8304613, 17);
		MD5_REVERSE_STEP(7, 0xfd469501, 22);
		MD5_REVERSE_STEP(11, 0x895cd7be, 22);
		MD5_REVERSE_STEP(14, 0xa679438e, 17);
		MD5_REVERSE_STEP(15, 0x49b40821, 22);

		const uint32 tt1 = FF(Q[Qoff + 1], Q[Qoff + 0], Q[Qoff - 1]) + Q[Qoff - 2] + 0xe8c7b756;
		const uint32 tt17 = GG(Q[Qoff + 16], Q[Qoff + 15], Q[Qoff + 14]) + Q[Qoff + 13] + 0xf61e2562;
		const uint32 tt18 = Q[Qoff + 14] + 0xc040b340 + block[6];
		const uint32 tt19 = Q[Qoff + 15] + 0x265e5a51 + block[11];
		const uint32 tt20 = Q[Qoff + 16] + 0xe9b6c7aa + block[0];
		const uint32 tt5 = RR(Q[Qoff + 6] - Q[Qoff + 5], 12) - FF(Q[Qoff + 5], Q[Qoff + 4], Q[Qoff + 3]) - 0x4787c62a;

		// change q17 until conditions are met on q18, q19 and q20
		unsigned counter = 0;
		while (counter < (1 << 7))
		{
			const uint32 q16 = Q[Qoff + 16];
			uint32 q17 = ((xrng64() & 0x3ffd7ff7) | (q16&0xc0008008)) ^ 0x40000000;
			++counter;

			uint32 q18 = GG(q17, q16, Q[Qoff + 15]) + tt18;
			q18 = RL(q18, 9); q18 += q17;
			if (0x00020000 != ((q18^q17)&0xa0020000))
				continue;

			uint32 q19 = GG(q18, q17, q16) + tt19;
			q19 = RL(q19, 14); q19 += q18;
			if (0x80000000 != (q19 & 0x80020000))
				continue;

			uint32 q20 = GG(q19, q18, q17) + tt20;
			q20 = RL(q20, 20); q20 += q19;
			if (0x00040000 != ((q20^q19) & 0x80040000))
				continue;

			block[1] = q17-q16; block[1] = RR(block[1], 5); block[1] -= tt17;
			uint32 q2 = block[1] + tt1; q2 = RL(q2, 12); q2 += Q[Qoff + 1];
			block[5] = tt5 - q2;

			Q[Qoff + 2] = q2;
			Q[Qoff + 17] = q17;
			Q[Qoff + 18] = q18;
			Q[Qoff + 19] = q19;
			Q[Qoff + 20] = q20;
			MD5_REVERSE_STEP(2, 0x242070db, 17);

			counter = 0;
			break;
		}
		if (counter != 0)
			continue;

		const uint32 q4 = Q[Qoff + 4];
		const uint32 q9backup = Q[Qoff + 9];
		const uint32 tt21 = GG(Q[Qoff+20], Q[Qoff+19], Q[Qoff+18]) + Q[Qoff+17] + 0xd62f105d;

		// iterate over possible changes of q4
		// while keeping all conditions on q1-q20 intact
		// this changes m3, m4, m5 and m7
		unsigned counter2 = 0;
		while (counter2 < (1<<4))
		{
			Q[Qoff+4] = q4 ^ q4mask[counter2];
			++counter2;
			MD5_REVERSE_STEP(5, 0x4787c62a, 12);
			uint32 q21 = tt21 + block[5];
			q21 = RL(q21,5); q21 += Q[Qoff+20];
			if (0 != ((q21^Q[Qoff+20]) & 0x80020000))
				continue;

			Q[Qoff + 21] = q21;
			MD5_REVERSE_STEP(3, 0xc1bdceee, 22);
			MD5_REVERSE_STEP(4, 0xf57c0faf, 7);
			MD5_REVERSE_STEP(7, 0xfd469501, 22);

			const uint32 tt22 = GG(Q[Qoff + 21], Q[Qoff + 20], Q[Qoff + 19]) + Q[Qoff + 18] + 0x02441453;
			const uint32 tt23 = Q[Qoff + 19] + 0xd8a1e681 + block[15];
			const uint32 tt24 = Q[Qoff + 20] + 0xe7d3fbc8 + block[4];

			const uint32 tt9 = Q[Qoff + 6] + 0x8b44f7af;
			const uint32 tt10 = Q[Qoff + 7] + 0xffff5bb1;
			const uint32 tt8 = FF(Q[Qoff + 8], Q[Qoff + 7], Q[Qoff + 6]) + Q[Qoff + 5] + 0x698098d8;
			const uint32 tt12 = RR(Q[Qoff+13]-Q[Qoff+12],7) - 0x6b901122;
			const uint32 tt13 = RR(Q[Qoff+14]-Q[Qoff+13],12) - FF(Q[Qoff+13],Q[Qoff+12],Q[Qoff+11]) - 0xfd987193;

			// iterate over possible changes of q9 and q10
			// while keeping conditions on q1-q21 intact
			// this changes m8, m9, m10, m12 and m13 (and not m11!)
			// the possible changes of q9 that also do not change m10 are used below
			for (unsigned counter3 = 0; counter3 < (1<<3);)
			{
				uint32 q10 = Q[Qoff+10] ^ (q9q10mask[counter3] & 0x60);
				Q[Qoff + 9] = q9backup ^ (q9q10mask[counter3] & 0x2000);
				++counter3;
				uint32 m10 = RR(Q[Qoff+11]-q10,17);
				m10 -= FF(q10, Q[Qoff+9], Q[Qoff+8]) + tt10;

				uint32 aa = Q[Qoff + 21];
				uint32 dd = tt22+m10; dd = RL(dd, 9) + aa;
				if (0x80000000 != (dd & 0x80000000)) continue;

				uint32 bb = Q[Qoff + 20];
				uint32 cc = tt23 + GG(dd, aa, bb);
				if (0 != (cc & 0x20000)) continue;
				cc = RL(cc, 14) + dd;
				if (0 != (cc & 0x80000000)) continue;

				bb = tt24 + GG(cc, dd, aa); bb = RL(bb, 20) + cc;
				if (0 == (bb & 0x80000000)) continue;

				block[10] = m10;
				block[13] = tt13 - q10;

				// iterate over possible changes of q9
				// while keeping intact conditions on q1-q24
				// this changes m8, m9 and m12 (but not m10!)
				//
				// The scan below evaluates 4 candidates at a time in SIMD
				// lanes. Lanes failing an intermediate condition keep computing
				// (harmlessly) and are filtered with an alive mask; survivors
				// continue in the scalar II chain, which rarely goes past its
				// first condition.
				const uint32 base12 = tt12 - FF(Q[Qoff + 12], Q[Qoff + 11], q10);
				const vsimd vq9base = vbcast(Q[Qoff + 9]);
				const vsimd vq10 = vbcast(q10);
				const vsimd vq8 = vbcast(Q[Qoff + 8]);
				const vsimd vq7 = vbcast(Q[Qoff + 7]);
				const vsimd vtt8 = vbcast(tt8);
				const vsimd vtt9 = vbcast(tt9);
				const vsimd vbase12 = vbcast(base12);
				const vsimd vaa = vbcast(aa), vbb = vbcast(bb), vcc = vbcast(cc), vdd = vbcast(dd);
				const vsimd vb0 = vbcast(block[0]), vb1 = vbcast(block[1]), vb2 = vbcast(block[2]);
				const vsimd vb3 = vbcast(block[3]), vb4 = vbcast(block[4]), vb5 = vbcast(block[5]);
				const vsimd vb6 = vbcast(block[6]), vb7 = vbcast(block[7]), vb10 = vbcast(block[10]);
				const vsimd vb11 = vbcast(block[11]), vb13 = vbcast(block[13]), vb14 = vbcast(block[14]);
				const vsimd vb15 = vbcast(block[15]);
				const vsimd vone = vbcast(1);
				const vsimd vzero = vbcast(0);

				for (unsigned base4 = 0; base4 < (1<<16); base4 += VLanes)
				{
					if ((base4 & 0xfff) == 0)
						check_abort();

					vsimd vmask;
					std::memcpy(&vmask, &q9mask[base4], sizeof(vmask));
					const vsimd vq9 = vq9base ^ vmask;

					const vsimd vb12 = vbase12 - vq9;
					const vsimd vb8 = RR(vq9 - vq8, 7) - vtt8;
					const vsimd vb9 = RR(vq10 - vq9, 12) - FF(vq9, vq8, vq7) - vtt9;

					vsimd va = vaa, vb = vbb, vc = vcc, vd = vdd;
					MD5_STEP(GG, va, vb, vc, vd, vb9, vbcast(0x21e1cde6), 5);
					MD5_STEP(GG, vd, va, vb, vc, vb14, vbcast(0xc33707d6), 9);
					MD5_STEP(GG, vc, vd, va, vb, vb3, vbcast(0xf4d50d87), 14);
					MD5_STEP(GG, vb, vc, vd, va, vb8, vbcast(0x455a14ed), 20);
					MD5_STEP(GG, va, vb, vc, vd, vb13, vbcast(0xa9e3e905), 5);
					MD5_STEP(GG, vd, va, vb, vc, vb2, vbcast(0xfcefa3f8), 9);
					MD5_STEP(GG, vc, vd, va, vb, vb7, vbcast(0x676f02d9), 14);
					MD5_STEP(GG, vb, vc, vd, va, vb12, vbcast(0x8d2a4c8a), 20);
					MD5_STEP(HH, va, vb, vc, vd, vb5, vbcast(0xfffa3942), 4);
					MD5_STEP(HH, vd, va, vb, vc, vb8, vbcast(0x8771f681), 11);

					vc += HH(vd, va, vb) + vb11 + vbcast(0x6d9d6122);
					// lanes still alive: bit 15 of c must be clear
					vsimd alive = ((vc >> 15) & vone) - vone;
					vc = (vc<<16 | vc>>16) + vd;

					MD5_STEP(HH, vb, vc, vd, va, vb14, vbcast(0xfde5380c), 23);
					MD5_STEP(HH, va, vb, vc, vd, vb1, vbcast(0xa4beea44), 4);
					MD5_STEP(HH, vd, va, vb, vc, vb4, vbcast(0x4bdecfa9), 11);
					MD5_STEP(HH, vc, vd, va, vb, vb7, vbcast(0xf6bb4b60), 16);
					MD5_STEP(HH, vb, vc, vd, va, vb10, vbcast(0xbebfbc70), 23);
					MD5_STEP(HH, va, vb, vc, vd, vb13, vbcast(0x289b7ec6), 4);
					MD5_STEP(HH, vd, va, vb, vc, vb0, vbcast(0xeaa127fa), 11);
					MD5_STEP(HH, vc, vd, va, vb, vb3, vbcast(0xd4ef3085), 16);
					MD5_STEP(HH, vb, vc, vd, va, vb6, vbcast(0x04881d05), 23);
					MD5_STEP(HH, va, vb, vc, vd, vb9, vbcast(0xd9d4d039), 4);
					MD5_STEP(HH, vd, va, vb, vc, vb12, vbcast(0xe6db99e5), 11);
					MD5_STEP(HH, vc, vd, va, vb, vb15, vbcast(0x1fa27cf8), 16);
					MD5_STEP(HH, vb, vc, vd, va, vb2, vbcast(0xc4ac5665), 23);
					// and bit 31 of b^d must be clear
					alive &= (((vb ^ vd) >> 31) & vone) - vone;

					// The first four II steps stay vectorized as well: only
					// ~2^-6 of the candidates survive them, so the branches
					// around the scalar tail below predict almost perfectly.
					MD5_STEP(II, va, vb, vc, vd, vb0, vbcast(0xf4292244), 6);
					alive &= (((va ^ vc) >> 31) & vone) - vone;
					MD5_STEP(II, vd, va, vb, vc, vb7, vbcast(0x432aff97), 10);
					alive &= vzero - (((vb ^ vd) >> 31) & vone);
					MD5_STEP(II, vc, vd, va, vb, vb14, vbcast(0xab9423a7), 15);
					alive &= (((va ^ vc) >> 31) & vone) - vone;
					MD5_STEP(II, vb, vc, vd, va, vb5, vbcast(0xfc93a039), 21);
					alive &= (((vb ^ vd) >> 31) & vone) - vone;

					uint32 am[VLanes], ta[VLanes], tb[VLanes], tc[VLanes], td[VLanes], tq[VLanes];
					std::memcpy(am, &alive, sizeof(alive));
					uint32 any = 0;
					for (unsigned l = 0; l < VLanes; ++l)
						any |= am[l];
					if (!any)
						continue;
					std::memcpy(ta, &va, sizeof(ta));
					std::memcpy(tb, &vb, sizeof(tb));
					std::memcpy(tc, &vc, sizeof(tc));
					std::memcpy(td, &vd, sizeof(td));
					std::memcpy(tq, &vq9, sizeof(tq));

					for (unsigned l = 0; l < VLanes; ++l)
					{
						if (!am[l])
							continue;

						const uint32 q9 = tq[l];
						block[12] = base12 - q9;
						const uint32 m8 = q9 - Q[Qoff + 8];
						block[8] = RR(m8, 7) - tt8;
						const uint32 m9 = q10 - q9;
						block[9] = RR(m9, 12) - FF(q9, Q[Qoff + 8], Q[Qoff + 7]) - tt9;

						uint32 a = ta[l], b = tb[l], c = tc[l], d = td[l];
						MD5_STEP(II, a, b, c, d, block[12], 0x655b59c3, 6);
						if (0 != ((a^c) >> 31)) continue;
						MD5_STEP(II, d, a, b, c, block[3], 0x8f0ccc92, 10);
						if (0 != ((b^d) >> 31)) continue;
						MD5_STEP(II, c, d, a, b, block[10], 0xffeff47d, 15);
						if (0 != ((a^c) >> 31)) continue;
						MD5_STEP(II, b, c, d, a, block[1], 0x85845dd1, 21);
						if (0 != ((b^d) >> 31)) continue;
						MD5_STEP(II, a, b, c, d, block[8], 0x6fa87e4f, 6);
						if (0 != ((a^c) >> 31)) continue;
						MD5_STEP(II, d, a, b, c, block[15], 0xfe2ce6e0, 10);
						if (0 != ((b^d) >> 31)) continue;
						MD5_STEP(II, c, d, a, b, block[6], 0xa3014314, 15);
						if (0 != ((a^c) >> 31)) continue;
						MD5_STEP(II, b, c, d, a, block[13], 0x4e0811a1, 21);
						if (0 == ((b^d) >> 31)) continue;
						MD5_STEP(II, a, b, c, d, block[4], 0xf7537e82, 6);
						if (0 != ((a^c) >> 31)) continue;
						MD5_STEP(II, d, a, b, c, block[11], 0xbd3af235, 10);
						if (0 != ((b^d) >> 31)) continue;
						MD5_STEP(II, c, d, a, b, block[2], 0x2ad7d2bb, 15);
						if (0 != ((a^c) >> 31)) continue;
						MD5_STEP(II, b, c, d, a, block[9], 0xeb86d391, 21);

						uint32 IHV1 = b + IV[1];
						uint32 IHV2 = c + IV[2];
						uint32 IHV3 = d + IV[3];

						bool wang = true;
						if (0x02000000 != ((IHV2^IHV1) & 0x86000000)) wang = false;
						if (0 != ((IHV1^IHV3) & 0x82000000)) wang = false;
						if (0 != (IHV1 & 0x06000020)) wang = false;

						bool stevens = true;
						if ( ((IHV1^IHV2)>>31)!=0 || ((IHV1^IHV3)>>31)!= 0 ) stevens = false;
						if ( (IHV3&(1<<25))!=0 || (IHV2&(1<<25))!=0 || (IHV1&(1<<25))!=0
							|| ((IHV2^IHV1)&1)!=0) stevens = false;

						if (!(wang || stevens)) continue;

						std::cout << "." << std::flush;

						uint32 IV1[4], IV2[4];
						for (int t = 0; t < 4; ++t)
							IV2[t] = IV1[t] = IV[t];

						uint32 block2[16];
						for (int t = 0; t < 16; ++t)
							block2[t] = block[t];
						block2[4] += 1<<31;
						block2[11] += 1<<15;
						block2[14] += 1<<31;

						md5_compress(IV1, block);
						md5_compress(IV2, block2);
							if (	   (IV2[0] == IV1[0] + (1<<31))
									&& (IV2[1] == IV1[1] + (1<<31) + (1<<25))
									&& (IV2[2] == IV1[2] + (1<<31) + (1<<25))
									&& (IV2[3] == IV1[3] + (1<<31) + (1<<25)))
								return;

						if (IV2[0] != IV1[0] + (1<<31))
							std::cout << "!" << std::flush;
					}
				}
			}
		}
	}
}
