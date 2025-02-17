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

// SOCK_WORKER
#define REDN_SINGLE 1
//#define REDN_PARALLEL 1   // SOCK_CLIENT
//#define REDN_SEQUENTIAL 1

#define REDN defined(REDN_SINGLE) || defined(REDN_PARALLEL) || defined(REDN_SEQUENTIAL)

// SOCK_MASTER
//#define ONE_SIDED 1
//#define TWO_SIDED 1

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
	MR_DATA = 0,    // 실제 데이터 저장하는 영역 
	MR_BUFFER,      // RDMA 통신을 위한 영역 
	MR_COUNT        // 메모리 영역 총 개수 
};

enum sock_type {
	SOCK_MASTER = 2,    // server
	SOCK_CLIENT,        // client
	SOCK_WORKER         // server에서 rdma 연산 수행 (확인필요)
};

int master_sock = 0;
int client_sock = 2;
int worker_sock = 3;

struct mr_context regions[MR_COUNT];

char *portno = "12345";
char *intf = "enp3s0f0";

pthread_spinlock_t sock_lock;

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

// app_connect
void add_peer_socket(int sockfd)
{
    // 소켓 타입 확인 
	int sock_type = rc_connection_meta(sockfd);

    // 소켓 프린트문 출력해서 확인할 필요가 있음음
	printf("ADDING PEER SOCKET %d (type: %d)\n", sockfd, sock_type);

    // 100개의 IBV_RECEIVE_IMM을 호출하는 이유를 알 수 없음 
    // one-sided 조건인건가? -> 맞으면 원래 코드로 바꾸던가 분석해보던가 
	if(isClient || sock_type != SOCK_CLIENT) {
		IBV_RECEIVE_IMM(sockfd);
		
        return;
	}

#if defined(REDN)
    printf("add_peer_socket RedN일 때 TEST");

	pthread_spin_lock(&sock_lock);

    // IBV_EXP_QP_CREATE_MANAGED_SEND --> send QP 
    // REDN 시나리오 --> 새로운 워커 노드를 생서으으
	int worker = add_connection(host, portno, SOCK_WORKER, 1, IBV_EXP_QP_CREATE_MANAGED_SEND);

	int id = n_client++;

	thread_arg[id] = id;

	client_sock[id] = sockfd;
	worker_sock[id] = worker;

	printf("input id %d to offload_hash\n", id);

    // offlaod_hash 바꿔야 함.
    // REDN 시나리오에서는 스레드로 offload_hash를 실행
	pthread_create(&offload_thread[id], NULL, offload_hash, &thread_arg[id]);

	printf("Setting sockfds [client: %d worker: %d]\n", client_sock[id], worker_sock[id]);

	pthread_spin_unlock(&sock_lock);
#elif defined(TWO_SIDED)

	client_sock[0] = sockfd;
	addr_t base_addr = mr_local_addr(sockfd, MR_BUFFER);

	// set up RECV for client inputs
	struct rdma_metadata *recv_meta =  (struct rdma_metadata *)
		calloc(1, sizeof(struct rdma_metadata) + 2 * sizeof(struct ibv_sge));

	recv_meta->sge_entries[0].addr = base_addr;
	recv_meta->sge_entries[0].length = 3;
	recv_meta->sge_entries[1].addr = base_addr + 4;
	recv_meta->sge_entries[1].length = 8;
	recv_meta->length = 11;
	recv_meta->sge_count = 2;

	IBV_RECEIVE_SG(sockfd, recv_meta, mr_local_key(sockfd, MR_BUFFER));

#endif
	
	return;
}

// app_disconnect -> 제대로 코드가 작성되어 있지 않음음
void remove_peer_socket(int sockfd)
{
	;
}

// app_receive
void test_callback(struct app_context *msg)
{
	//ibw_cpu_relax();
	if(!isClient) {	

		//printf("posting receive imm\n");
		int sock_type = rc_connection_meta(msg->sockfd);

		//XXX do not post receives on master lock socket
		if(sock_type != SOCK_CLIENT)
			IBV_RECEIVE_IMM(msg->sockfd);

		if(sock_type == SOCK_CLIENT)
			n_hash_req++;

#if REDN
		print_seg_data();

#elif defined(TWO_SIDED)

	int sockfd = msg->sockfd;
	addr_t base_addr = mr_local_addr(sockfd, MR_BUFFER);
	addr_t remote_addr = mr_remote_addr(sockfd, MR_BUFFER);


	uint8_t *param1 = (uint8_t*) base_addr;
	uint64_t *param2 = (uint64_t*)(base_addr + 4);

	struct hash_bucket *bucket = (struct hash_bucket *) ntohll(*param2);

	printf("received req: key %u addr %lu\n", param1[2], ntohll(*param2));
	//printf("key required %u has %u\n", param1[2], bucket->key[0]);
	if(param1[2] == bucket->key[0]) {
		post_hash_response(sockfd, bucket, remote_addr, msg->id);
		IBV_TRIGGER(master_sock, sockfd, 0);
	}
	else
		printf("Key doesn't exist!\n");

	// set up RECV for client inputs
	struct rdma_metadata *recv_meta =  (struct rdma_metadata *)
		calloc(1, sizeof(struct rdma_metadata) + 2 * sizeof(struct ibv_sge));

	recv_meta->sge_entries[0].addr = base_addr;
	recv_meta->sge_entries[0].length = 3;
	recv_meta->sge_entries[1].addr = base_addr + 4;
	recv_meta->sge_entries[1].length = 8;
	recv_meta->length = 11;
	recv_meta->sge_count = 2;

	IBV_RECEIVE_SG(sockfd, recv_meta, mr_local_key(sockfd, MR_BUFFER));

#endif

	}
	printf("Received response with id %d (n_req %d)\n", msg->id, n_hash_req);
}























int main(int argc, char **argv) {
    
    int iters;
    //void *ptr;
	//int shm_fd;
	//int shm_ret;

    // timer (latency, 성능 측정)

    pthread_spin_init(&sock_lock, PTHREAD_PROCESS_PRIVATE);

    // 공유메모리 관련 코드 작성해야 됨
    // 인자 관리 코드 작성해야 됨됨

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
    init_rdma_agent(portno, regions, MR_COUNT, 2, add_peer_socket, remove_peer_socket, test_callback);

    printf("Starting benchmark ...\n");

    if (!isClient) {
        // server 측에서는 A배열 초기화?
        // 그리고 trigger?


        // 서버에서 할당한 메모리 해제
    } 

    // rc_ready

    // mode 마다 처리 다르게 하기

}