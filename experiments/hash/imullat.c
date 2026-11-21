/* Latency and throughput of "imul $C, r32, r32" for several constants, measured
 * with rdtsc (reference cycles, so compare the constants with each other, not
 * to the documented latency). Build: gcc -O2 imullat.c -o imullat
 * Run pinned: taskset -c 1 ./imullat
 */
#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>
#define N 100000000UL
#define LAT(C) static uint64_t lat_##C(uint32_t x) { uint64_t t = __rdtsc(); \
	for (unsigned long i = 0; i < N; i++) \
		__asm__ volatile("imul $" #C ", %0, %0\n\timul $" #C ", %0, %0\n\timul $" #C ", %0, %0\n\timul $" #C ", %0, %0" : "+r"(x)); \
	return __rdtsc() - t; }
#define TPT(C) static uint64_t tpt_##C(uint32_t x) { uint32_t a=x,b=x+1,c=x+2,d=x+3; uint64_t t = __rdtsc(); \
	for (unsigned long i = 0; i < N; i++) \
		__asm__ volatile("imul $" #C ", %0, %0\n\timul $" #C ", %1, %1\n\timul $" #C ", %2, %2\n\timul $" #C ", %3, %3" : "+r"(a),"+r"(b),"+r"(c),"+r"(d)); \
	return __rdtsc() - t; }
#define BOTH(C) LAT(C) TPT(C)
BOTH(0x8003f) BOTH(0x100801) BOTH(0x1af42f) BOTH(0x7fffffff) BOTH(3)
int main(void)
{
	int r;
	for (r = 0; r < 3; r++) {
		printf("round %d (ref cycles per imul): latency / throughput\n", r);
		printf("  0x8003f    %.3f / %.3f\n", lat_0x8003f(r + 12345) / (4.0 * N), tpt_0x8003f(r + 12345) / (4.0 * N));
		printf("  0x100801   %.3f / %.3f\n", lat_0x100801(r + 12345) / (4.0 * N), tpt_0x100801(r + 12345) / (4.0 * N));
		printf("  0x1af42f   %.3f / %.3f\n", lat_0x1af42f(r + 12345) / (4.0 * N), tpt_0x1af42f(r + 12345) / (4.0 * N));
		printf("  0x7fffffff %.3f / %.3f\n", lat_0x7fffffff(r + 12345) / (4.0 * N), tpt_0x7fffffff(r + 12345) / (4.0 * N));
		printf("  3          %.3f / %.3f\n", lat_3(r + 12345) / (4.0 * N), tpt_3(r + 12345) / (4.0 * N));
	}
	return 0;
}
