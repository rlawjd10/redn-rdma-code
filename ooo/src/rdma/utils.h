#ifndef RDMA_UTILS_H
#define RDMA_UTILS_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <inttypes.h>
#include <semaphore.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdarg.h>

// utils.c에서도 쓰이지만 connectin.c에서 외부변수로 함수들을 호출함

// 현재 스레드의 TID(Thread ID)를 가져오는 매크로
#define get_tid() syscall(__NR_gettid)

//DEBUG macros
#ifdef DEBUG
 #define debug_print(fmt, args...) fprintf(stderr, "DEBUG[tid:%lu][%s:%d]: " fmt, \
		     	 	get_tid(), __FILE__, __LINE__, ##args)
#else
 #define debug_print(fmt, args...) /*  Don't do anything in release builds */
#endif

/*
 * min()/max()/clamp() macros that also do
 * strict type-checking.. See the
 * "unnecessary" pointer comparison.
 */
#define min(a, b) ({\
		__typeof__(a) _a = a;\
		__typeof__(b) _b = b;\
		_a < _b ? _a : _b; })

#define max(a, b) ({\
		__typeof__(a) _a = a;\
		__typeof__(b) _b = b;\
		_a > _b ? _a : _b; })

// 어셈블리 명령어 pause\n을 실행해서 CPU가 wait하도록 함으로써 불필요한 CPU 자원 낭비 방지
// volatile 키워드는 컴파일러가 이 코드를 최적화하지 못하게 막음 (반드시 실행)
// CQ를 계속 확인해야 되는데 busy-waiting을 하면서 CPU낭비 or Atomic 연산할 때 lock 풀리는 거 기다리기기
#define ibw_cpu_relax() __asm__ volatile("pause\n": : :"memory")


//ibw_cmpxchg(&bitmap[i], 0, 1)
// P는 메모리 주소, O는 old 값, N은 new
#define ibw_cmpxchg(P, O, N) __sync_val_compare_and_swap((P), (O), (N))

// 사용되지 않는 변수를 명시적을 처리하는 매크로 -> 안쓰는 거 같아서 지워버림
//#define ibw_unused(expr) do { (void)(expr); } while (0)

extern unsigned int g_seed; // utils.c에서 선언한 변수

// inline
inline void set_seed(int seed) {
	g_seed = seed;
}

inline int fastrand(int seed) { 
	seed = (214013*seed+2531011); 
	return (seed>>16)&0x7FFF; 
}

static inline unsigned DIV_ROUND_UP(unsigned n, unsigned d)
{
	return (n + d - 1u) / d;
}

__attribute__((visibility ("hidden"))) 
inline int cmp_counters(uint32_t a, uint32_t b) { 
	if (a == b) // 두 wr id가 같음 (이미 완료)
		return 0;
	else if((a - b) < UINT32_MAX/2)	// a가 b보다 조금 크므로 완료되지 않음 
		return 1;
	else
		return -1;	// a가 b보다 매우 크므로 a가 오래된 wr id 
}

__attribute__((visibility ("hidden"))) 
inline int diff_counters(uint32_t a, uint32_t b) {
	if (a >= b)
		return a - b;
	else
		return b - a;
}

// bitmap에서 첫 번째 비어있는 bit를 찾고 채우기.
inline int find_first_empty_bit_and_set(int bitmap[], int n)
{
	for(int i=0; i<n; i++) {
		if(!ibw_cmpxchg(&bitmap[i], 0, 1))
			return i;
	}
	return -1;							
}

// 첫 번째로 비어있지 않은 bit index 찾기.
inline int find_first_empty_bit(int bitmap[], int n)
{
	for(int i=0; i<n; i++) {
		if(!bitmap[i])
			return i;
	}
	return -1;
}

// inx 이후 첫 번째 비어있는 비트.
inline int find_next_empty_bit(int idx, int bitmap[], int n)
{
	for(int i=idx+1; i<n; i++) {
		if(!bitmap[i])
			return i;
	}
	return -1;
}

// 첫 번째로 채워져있는 bit를 empty로
inline int find_first_set_bit_and_empty(int bitmap[], int n)
{
	for(int i=0; i<n; i++) {
		if(ibw_cmpxchg(&bitmap[i], 1, 0))
			return i;
	}
	return -1;							
}

// 첫 번째 채워져있는 bit의 index 찾기
inline int find_first_set_bit(int bitmap[], int n)
{
	for(int i=0; i<n; i++) {
		if(bitmap[i])
			return i;
	}
	return -1;
}

// inx 다음의 채워져있는 bit를 empty
inline int find_next_set_bit(int idx, int bitmap[], int n)
{
	for(int i=idx+1; i<n; i++) {
		if(bitmap[i])
			return i;
	}
	return -1;
}

// 1인 bit 개수
inline int find_bitmap_weight(int bitmap[], int n)
{
	int weight = 0;
	for(int i=0; i<n; i++) {
		weight += bitmap[i];
	}
	return weight;
}

#if 0
// core_id = 0, 1, ... n-1, where n is the system's number of cores
// 특정 thread를 특정 CPU core에 binding
__attribute__((visibility ("hidden"))) 
inline int stick_this_thread_to_core(int core_id) {
	int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
	if (core_id < 0 || core_id >= num_cores)
		return EINVAL;

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(core_id, &cpuset);

	pthread_t current_thread = pthread_self();    
	return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}
#endif

// IPv4 주소를 복사하는 함수.
// sockaddr_storage 구조체에서 sockad
__attribute__((visibility ("hidden"))) 
inline struct sockaddr_in * copy_ipv4_sockaddr(struct sockaddr_storage *in)
{
	if(in->ss_family == AF_INET) {
		struct sockaddr_in *out = (struct sockaddr_in *) calloc(1, sizeof(struct sockaddr_in));
		memcpy(out, in, sizeof(struct sockaddr_in));
		return out;
	}
	else {
		return NULL;
	}
}

#endif
