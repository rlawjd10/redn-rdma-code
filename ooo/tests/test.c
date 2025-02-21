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
#include "agent.h"

#define BUCKET_COUNT 2

#define OFFLOAD_COUNT 50000

#define IO_SIZE 65536   // 64KB

#define BUFFER_SIZE (2 * 1024 * 1024)  // 2MB HugePage
#define HUGEPAGE_PATH "/dev/hugepages" // HugePage 경로

#define REDN_SINGLE 1
#define REDN_PARALLEL 1  
#define REDN_SEQUENTIAL 1

#define REDN defined(REDN_SINGLE) || defined(REDN_PARALLEL) || defined(REDN_SEQUENTIAL)

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
	SOCK_MASTER = 2,    
	SOCK_CLIENT,        
	SOCK_WORKER         
};

#define SHM_PATH "/ifbw_shm"
#define SHM_F_SIZE 128

#define LAT 1

// 공유 메모리 설정 
void* create_shm(int *fd, int *res) {
	void * addr;
	*fd = shm_open(SHM_PATH, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (*fd < 0) {
		exit(-1);
	}

	*res = ftruncate(*fd, SHM_F_SIZE);  // 공유 메모리 객체 크기 설정
	if (*res < 0)
	{
		exit(-1);
	}

    // 공유 메모리 객체를 메모리에 매핑, NULL: 커널이 알아서 주소 지정 
	addr = mmap(NULL, SHM_F_SIZE, PROT_WRITE, MAP_SHARED, *fd, 0);
	if (addr == MAP_FAILED){
		exit(-1);
	}

	return addr;
}

void destroy_shm(void *addr) {
	int ret, fd;
	ret = munmap(addr, SHM_F_SIZE);
	if (ret < 0)
	{
		exit(-1);
	}

	fd = shm_unlink(SHM_PATH);  // 공유 메모리 객체 제거 
	if (fd < 0) {
		exit(-1);
	}
}

volatile sig_atomic_t stop = 0;

int batch_size = 1;	//default - batching disabled
int sge_count = 1;	//default - 1 scatter/gather element
int use_cas = 0;	//default - compare_and_swap disabled

int psync = 0;		// used for process synchronization

char *portno = "12345";
char *intf = "enp3s0f0";

int isClient = 0;

struct mr_context regions[MR_COUNT];

static pthread_t offload_thread[BUCKET_COUNT];

int master_sock = 0;
int client_sock[BUCKET_COUNT] = {2, 3};
int worker_sock[BUCKET_COUNT] = {4, 6};

int thread_arg[BUCKET_COUNT] = {0, 0};

int n_client = 0;

pthread_spinlock_t sock_lock;

int temp1_wrid[OFFLOAD_COUNT] = {0};
int temp2_wrid[OFFLOAD_COUNT] = {0};

// count the # of requests received from client
volatile int n_hash_req = 0;

typedef uintptr_t addr_t;

uint64_t a[3] = {0, 1, 2};  

/* --- allocate memory ---*/
void *allocate_physical_memory(size_t size) {
    void *addr;

    for (int i = 0; i < MR_COUNT; i++) {
        addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (addr == MAP_FAILED) {
            perror("mmap failed");
            return NULL;
        }
        
        if (mlock(addr, size) != 0) {
            perror("mlock failed");
            munmap(addr, size);
            return NULL;
        }
        
        memset(addr, 0, size); 

        regions[i].type = i;
        regions[i].length = size;
        regions[i].addr = (uintptr_t) addr;
        
        // 2097152 bytes 
        //printf("Allocated MR[%d]: addr=%p, size=%lu bytes\n", i, addr, size);
    }

    return addr;
}

void free_physical_memory(void *addr, size_t size) {
    if (addr) {
        munlock(addr, size);
        munmap(addr, size);
    }
}

/* --- Returns new argc --- */
static int adjust_args(int i, char *argv[], int argc, unsigned del)
{
   if (i >= 0) {
      for (int j = i + del; j < argc; j++, i++)
         argv[i] = argv[j];
      argv[i] = NULL;
      return argc - del;
   }
   return argc;
}

int process_opt_args(int argc, char *argv[])
{
   int dash_d = -1;

restart:
   for (int i = 0; i < argc; i++) {
      //printf("argv[%d] = %s\n", i, argv[i]);
      if (strncmp("-b", argv[i], 2) == 0) {
         batch_size = atoi(argv[i+1]);
         dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 2);
	 goto restart;
      }
      else if (strncmp("-e", argv[i], 2) == 0) {
	 sge_count = atoi(argv[i+1]);
         dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 2);
	 goto restart;
      }
      else if (strncmp("-p", argv[i], 2) == 0) {
	 portno = argv[i+1];
         dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 2);
	 goto restart;
      }
      else if (strncmp("-i", argv[i], 2) == 0) {
	 intf = argv[i+1];
         dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 2);
	 goto restart;
      } 
      else if (strncmp("-s", argv[i], 2) == 0) {
	 psync = 1;
	 dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 1);
	 goto restart;
      } 
      else if (strncmp("-cas", argv[i], 4) == 0) {
	 use_cas = 1;
	 dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 1);
	 goto restart;
      } 
   }

   return argc;
}

/* --- call back function --- */
void add_peer_socket(int sockfd)
{
	int sock_type = rc_connection_meta(sockfd);

	printf("ADDING PEER SOCKET %d (type: %d)\n", sockfd, sock_type);

	if(isClient || sock_type != SOCK_CLIENT) { //XXX do not post receives on master socket
		for(int i=0; i<100; i++) {
			IBV_RECEIVE_IMM(sockfd);
		}

		return;
	}

#if defined(REDN)
	pthread_spin_lock(&sock_lock);

	int worker = add_connection(host, portno, SOCK_WORKER, 1, IBV_EXP_QP_CREATE_MANAGED_SEND);

	int id = n_client++;

	thread_arg[id] = id;

	client_sock[id] = sockfd;
	worker_sock[id] = worker;

	printf("input id %d to offload_hash\n", id);

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

#if defined(REDN)
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

void remove_peer_socket(int sockfd)
{
	;
}


int main(int argc, char **argv) {

    int shm_fd, shm_ret;
    char *server_ip = argv[1];
    void *addr;

    // 1. timer 설정 
    ////////////////
    int *shm_proc = (int*)create_shm(&shm_fd, &shm_ret);

    pthread_spin_init(&sock_lock, PTHREAD_PROCESS_PRIVATE);

    // 2. argument 처리 
    argc = process_opt_args(argc, argv);

    if(psync) {
		printf("Setting shm_proc to zero\n");
		*shm_proc = 0;
		return 0;
	}

    if (argc != 1 && argc != 3) {
        fprintf(stderr, "usage: %s <peer-address> <iters> [-p <portno>] [-e <sge count>] [-b <batch size>]  (note: run without args to use as server)\n", argv[0]);
        return 1;
    }

    if (argc > 1) { isClient = 1; }

    // 3. allocate dram region
    addr = allocate_physical_memory(BUFFER_SIZE);
    if (addr == NULL) {
        fprintf(stderr, "Failed to allocate hugepage memory\n");
        return EXIT_FAILURE;
    }

     // 4. rdma connection (server & client)
    init_rdma_agent(portno, regions, MR_COUNT, 2, isClient, add_peer_socket, remove_peer_socket, test_callback);

    // 5. client or server 
    if (isClient) { // client
        printf("[Client] Connecting to %s\n", server_ip);
        int iters = atoi(argv[2]);

        if (iters > OFFLOAD_COUNT) {
            return 1;
        }
        
        // sockfd를 반환
        master_sock = add_connection(server_ip, portno, SOCK_MASTER, 1, 0);
    } 
    else {          // server
        printf("[Server] Listening on %s\n", portno);
        sleep(10);
       
        printf("---- Initializing array A ----\n");
        for (int i = 0; i < 3; i++) {
            a[i] = i + 1000;
        }

    }

    // 6. rc_ready() 호출 

    // 7. mode-per connection (RedN)

    pthread_join(comm_thread, NULL);
    free_physical_memory(addr, BUFFER_SIZE);
    printf("Agent thread terminated. Exiting main.\n");

}