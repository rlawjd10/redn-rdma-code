#include <time.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h> 
#include <sys/mman.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include "time_stat.h"
#include "agent.h"

#define OFFLOAD_COUNT 50000

#define BUFFER_SIZE (2 * 1024 * 1024)  // 2MB HugePage
#define HUGEPAGE_PATH "/dev/hugepages" // HugePage 경로

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))


#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)

#ifdef __cplusplus
#define ALIGN(x, a)  ALIGN_MASK((x), ((__typeof__(x))(a) - 1))
#else
#define ALIGN(x, a)  ALIGN_MASK((x), ((typeof(x))(a) - 1))
#endif
#define ALIGN_MASK(x, mask)	(((x) + (mask)) & ~(mask))

#if __BIG_ENDIAN__
    #define htonll(x)   (x)
    #define ntohll(x)   (x)
#else
    #define htonll(x)   ((((uint64_t)htonl(x&0xFFFFFFFF)) << 32) + htonl(x >> 32))
    #define ntohll(x)   ((((uint64_t)ntohl(x&0xFFFFFFFF)) << 32) + ntohl(x >> 32))
#endif

enum region_type {
	MR_DATA = 0,
	MR_BUFFER,
	MR_COUNT
};

enum sock_type {
	SOCK_MASTER = 2,
	SOCK_CLIENT,
	SOCK_WORKER
};

int master_sock = 0;
int client_sock = 2;
int worker_sock = 3;

struct mr_context regions[MR_COUNT];

char *portno = "12345";
char *intf = "enp3s0f0";

int isClient = 0;
typedef uintptr_t addr_t;

unit64_t A[3] = {0, 1, 2};  


void *allocate_physical_memory(size_t size) {
    void *addr;

    for (int i = 0; i < MR_COUNT; i++) {
        addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (addr == MAP_FAILED) {
            perror("mmap failed");
            return NULL;
        }
        
        // 메모리 락 (스왑 방지)
        if (mlock(addr, size) != 0) {
            perror("mlock failed");
            munmap(addr, size);
            return NULL;
        }
        
        memset(addr, 0, size); // 초기화

        regions[i].type = i;
        regions[i].length = size;
        regions[i].addr = (uintptr_t) addr;
        
        printf("Allocated MR[%d]: addr=%p, size=%lu bytes\n", i, addr, size);
    }

    return addr;
}

void free_physical_memory(void *addr, size_t size) {
    if (addr) {
        munlock(addr, size);
        munmap(addr, size);
    }
}

int main(int argc, char **argv) {
    
    int iters;


    // check parameter
    if (argc != 1 && argc != 3) {
        fprintf(stderr, "usage: %s <peer-address> <iters> [-p <portno>] [-e <sge count>] [-b <batch size>]  (note: run without args to use as server)\n", argv[0]);
        return 1;
    }

    if (argc > 1) { isClient = 1; }

    // allocate dram region
    if (allocate_physical_memory(BUFFER_SIZE) == NULL) {
        fprintf(stderr, "Failed to allocate hugepage memory\n");
        return EXIT_FAILURE;
    }

    if (isClient) {
        iters = atoi(argv[2]);
        char *server_ip = atoi(argv[1]);
        
        if (iters > OFFLOAD_COUNT) {
            return 1;
        }
        
        // sockfd를 반환
        master_sock = add_connection(server_ip, portno, SOCK_MASTER, 1, 0);
    }

    // rdma connection (server & client)
    init_rdma_agent(portno, regions, MR_COUNT, 2, NULL, NULL, NULL);

    printf("Starting benchmark ...\n");


}