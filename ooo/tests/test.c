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

#define BUCKET_COUNT 2
#define HASH_SIZE 10

#define OFFLOAD_COUNT 50000

#define IO_SIZE 65536   // 64KB

#define BUFFER_SIZE (2 * 1024 * 1024)  // 2MB HugePage
#define HUGEPAGE_PATH "/dev/hugepages" // HugePage 경로

#define REDN_SINGLE 1
//#define REDN_PARALLEL 1  
//#define REDN_SEQUENTIAL 1

#define REDN defined(REDN_SINGLE) || defined(REDN_PARALLEL) || defined(REDN_SEQUENTIAL)

#define ONE_SIDED 1
#define TWO_SIDED 1

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

struct __attribute__((__packed__)) hash_bucket {
	uint8_t key[3];
	uint64_t addr;
	uint64_t value[32768]; //XXX inline values for now 
};

#define SHM_PATH "/ifbw_shm"
#define SHM_F_SIZE 128

#define LAT 1

// shared memory  
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
struct time_stats *timer;
struct time_stats *timer_total;

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

struct wqe_ctrl_seg *sr0_ctrl = NULL;
struct mlx5_wqe_raddr_seg * sr0_raddr = NULL;
struct mlx5_wqe_data_seg * sr0_data[2] = { NULL };
struct wqe_ctrl_seg *sr1_ctrl = NULL;
struct mlx5_wqe_data_seg * sr1_data = NULL;
struct mlx5_wqe_raddr_seg * sr1_raddr = NULL;
struct mlx5_wqe_atomic_seg * sr1_atomic = NULL;
struct wqe_ctrl_seg *sr2_ctrl = NULL;
struct mlx5_wqe_data_seg * sr2_data = NULL;
struct mlx5_wqe_raddr_seg * sr2_raddr = NULL;

int sr0_wrid, sr1_wrid, sr2_wrid;

struct timespec start;

// test callback function
void print_seg_data()
{
	if(sr0_data && sr0_raddr) {
		printf("------ AFTER ------\n");
		printf("sr0_data[0]: addr %lu length %u\n", be64toh(sr0_data[0]->addr), ntohl(sr0_data[0]->byte_count));
		printf("sr0_data[1]: addr %lu length %u\n", be64toh(sr0_data[1]->addr), ntohl(sr0_data[1]->byte_count));
		printf("sr0_raddr: raddr %lu\n", ntohll(sr0_raddr->raddr));
		printf("sr1_atomic: compare %lx (original: %lx) swap_add %lx (original: %lx)\n",
				be64toh(sr1_atomic->compare), sr1_atomic->compare, be64toh(sr1_atomic->swap_add), sr1_atomic->swap_add);
		printf("sr1_raddr: raddr %lu\n", ntohll(sr1_raddr->raddr));

		uint32_t sr2_meta = ntohl(sr2_ctrl->opmod_idx_opcode);
		uint16_t idx2 =  ((sr2_meta >> 8) & (UINT_MAX));
		uint8_t opmod2 = ((sr2_meta >> 24) & (UINT_MAX));
		uint8_t opcode2 = (sr2_meta & USHRT_MAX);

		printf("&sr2_ctrl->opmod_idx_opcode %lu\n", (uintptr_t)&sr2_ctrl->opmod_idx_opcode);
		printf("sr2_ctrl: raw %lx idx %u opmod %u opcode %u qpn_ds %x fm_ce_se %x (imm %u)\n", *((uint64_t *)&sr2_ctrl->opmod_idx_opcode), idx2, opmod2, opcode2, ntohl(sr2_ctrl->qpn_ds), ntohl(sr2_ctrl->fm_ce_se), ntohl(sr2_ctrl->imm));
		printf("sr2_data: addr %lu length %u\n", be64toh(sr2_data->addr), ntohl(sr2_data->byte_count));
		printf("*sr2_data->addr = %lu\n", *((uint64_t *)be64toh(sr2_data->addr)));
	}
}

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

/* --- post hash response (client) --- */
uint32_t post_get_req_async(int sockfd, uint32_t key, addr_t addr, uint32_t imm, uint32_t offset)
{
	struct rdma_metadata *send_meta =  (struct rdma_metadata *)
		calloc(1, sizeof(struct rdma_metadata) + 2 * sizeof(struct ibv_sge));

	printf("--> Send GET [key %u addr %lu]\n", key, addr);

	addr_t base_addr = mr_local_addr(sockfd, MR_BUFFER) + offset;
	uint8_t *param1 = (uint8_t *) base_addr; //key
	uint64_t *param2 = (uint64_t *) (base_addr + 4); //addr

	param1[0] = 0;
	param1[1] = 0;
	param1[2] = key;
	*param2 = htonll(addr);

	send_meta->sge_entries[0].addr = (uintptr_t) param1;
	send_meta->sge_ent`ries[0].length = 3;
	send_meta->sge_entries[1].addr = (uintptr_t) param2;
	send_meta->sge_entries[1].length = 8;
	send_meta->length = 11;
	send_meta->sge_count = 2;
	send_meta->addr = 0;
	send_meta->imm = imm;
	return IBV_WRAPPER_SEND_WITH_IMM_ASYNC(sockfd, send_meta, MR_BUFFER, 0);
}

void post_get_req_sync(int *socks, uint32_t key, addr_t addr, int response_id)
{
	struct timespec start, end;

	addr_t base_addr = mr_local_addr(socks[0], MR_DATA);
	volatile uint64_t *res = (volatile uint64_t *) (base_addr);

#if REDN_PARALLEL

	// key 수정할 필요가 있을지도
	for(int h=0; h<BUCKET_COUNT; h++)	// ibv_post_send 
		post_get_req_async(socks[h], key+h, addr + h*sizeof(struct hash_bucket), response_id, h*16);

	time_stats_start(timer);

	for(int h=0; h<BUCKET_COUNT; h++)
		IBV_TRIGGER(master_sock, socks[h], 0); 	// ibv_post_send

	time_stats_stop(timer);

	time_stats_print(timer, "Run Complete");

#elif defined(REDN_SEQUENTIAL)

	for(int h=0; h<BUCKET_COUNT; h++) {
		post_get_req_async(socks[0], key, addr + h*sizeof(struct hash_bucket), response_id + h, h*16);
	}

	time_stats_start(timer);

	IBV_TRIGGER(master_sock, socks[0], 0);

	IBV_AWAIT_RESPONSE(socks[0], response_id + BUCKET_COUNT - 1); 	// busy wait

	time_stats_stop(timer);

	time_stats_print(timer, "Run Complete");

#elif defined(ONE_SIDED)

	volatile struct hash_bucket *bucket = NULL;
	uint32_t wr_id = 0;
	addr_t bucket_addr =  mr_remote_addr(socks[0], MR_DATA);

	printf("--> Send GET [key %u addr %lu]\n", key, addr);

	printf("read from remote addr %lu\n", bucket_addr);

	rdma_meta_t *meta = (rdma_meta_t *) calloc(1, sizeof(rdma_meta_t))
		+ 1 * sizeof(struct ibv_sge);

	meta->addr = bucket_addr;
	meta->length = 19 * 6;
	meta->sge_count = 1;
	meta->sge_entries[0].addr = base_addr;
	meta->sge_entries[0].length = 19 * 6;
	meta->next = NULL;

	printf("reading from dst %lu to src %lu\n", bucket_addr, base_addr);
	wr_id = IBV_WRAPPER_RDMA_READ_ASYNC(socks[0], meta, MR_DATA, MR_DATA);

	IBV_TRIGGER(master_sock, socks[0], 0);

	time_stats_start(timer);

	IBV_AWAIT_WORK_COMPLETION(socks[0], wr_id);	// busy wait

	bucket = (volatile struct hash_bucket *) base_addr;

	if(bucket->key[0] == (uint8_t)key) {
		printf("found key\n");
		IBV_TRIGGER(master_sock, socks[0], 0);

		printf("value size %u\n", res[IO_SIZE/8 - 1]);
	}
	else {
		printf("didn't find key required %d. found %d \n", key, bucket->key[0]);
	}

	time_stats_stop(timer);

#else	// two-sided 

	post_get_req_async(socks[0], key, addr, response_id, 0);

	time_stats_start(timer);

	IBV_TRIGGER(master_sock, socks[0], 0);

	time_stats_stop(timer);

#endif
}

/* --- call back function (server) --- */
void add_peer_socket(int sockfd)
{
	int sock_type = rc_connection_meta(sockfd);

	printf("ADDING PEER SOCKET %d (type: %d)\n", sockfd, sock_type);

	if(isClient || sock_type != SOCK_CLIENT) { //XXX do not post receives on master socket
	
#if defined(TWO_SIDED)		
		for(int i=0; i<100; i++) {
			IBV_RECEIVE_IMM(sockfd);
		}
#endif
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
	// server 
	if(!isClient) {	

		int sock_type = rc_connection_meta(msg->sockfd);
		printf("test callback sock_type %d\n", sock_type);

		//XXX do not post receives on master lock socket
		if(sock_type != SOCK_CLIENT)	// master, worker
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
	debug_print("REMOVING PEER SOCKET %d\n", sockfd);
}

/* --- offload function --- */
void * offload_hash(void *arg)
{

	struct timespec start, end;

	int id = *((int *)arg);
	int count = OFFLOAD_COUNT;

	int master = master_sock;
	int client = client_sock[id];
	int worker = worker_sock[id];

	//int sr0_wrid, sr1_wrid, sr2_wrid;
	uint64_t base_data_addr = mr_local_addr(worker, MR_DATA);
	uint64_t base_buffer_addr = mr_local_addr(worker, MR_BUFFER);

	struct ibv_mr *client_wq_mr = register_wq(client, client);
	struct ibv_mr *worker_wq_mr = register_wq(worker, worker);

	printf("offload hash with id %d\n", id);

	while(!(rc_ready(client)) || !(rc_ready(worker))) {
        	asm("");
	}

	printf("performing hash offload [client: %d worker: %d]\n", client, worker);

	int count_1 = 8;
	int count_2 = 1;

	for(int k=0; k<count; k++)
	{
		start = timer_start();

		IBV_WAIT_EXPLICIT(worker, client, 1);

		if(k == count - 1)
			IBV_TRIGGER_EXPLICIT(worker, worker, count_1 - 1);
		else
			IBV_TRIGGER_EXPLICIT(worker, worker, count_1);

		printf("remote start: %lu end: %lu\n", mr_remote_addr(worker, MR_DATA), mr_remote_addr(worker, MR_DATA) + mr_sizes[MR_DATA]);
		sr0_wrid = post_hash_read(worker, client_wq_mr->lkey, mr_remote_key(worker, MR_DATA));

		sr1_wrid = IBV_CAS_ASYNC(worker, base_buffer_addr, base_buffer_addr, 0, 1, mr_remote_key(master, MR_BUFFER), client_wq_mr->lkey, 1);
	

		IBV_WAIT_EXPLICIT(worker, worker, 1);

		IBV_TRIGGER_EXPLICIT(worker, client, count_2);
		
		sr2_wrid = post_dummy_write(client, IO_SIZE, k + 1);

		if(k == 0)
			IBV_TRIGGER(master, worker, 2); // trigger first two wrs

#if 1

		// find READ WR
		
		sr0_ctrl = IBV_FIND_WQE(worker, sr0_wrid);

		if(!sr0_ctrl) {
			printf("Failed to find sr0 seg\n");
			pause();
		}

		uint32_t sr0_meta = ntohl(sr0_ctrl->opmod_idx_opcode);
		uint16_t idx0 =  ((sr0_meta >> 8) & (UINT_MAX));
		uint8_t opmod0 = ((sr0_meta >> 24) & (UINT_MAX));
		uint8_t opcode0 = (sr0_meta & USHRT_MAX);

		//printf("sr0 (READ) segment will be posted to idx #%u\n", idx0);


		// find CAS WR
		sr1_ctrl = IBV_FIND_WQE(worker, sr1_wrid);

		if(!sr1_ctrl) {
			printf("Failed to find sr1 seg\n");
			pause();
		}

		uint32_t sr1_meta = ntohl(sr1_ctrl->opmod_idx_opcode);
		uint16_t idx1 =  ((sr1_meta >> 8) & (UINT_MAX));
		uint8_t opmod1 = ((sr1_meta >> 24) & (UINT_MAX));
		uint8_t opcode1 = (sr1_meta & USHRT_MAX);

		//printf("sr1 (CAS) segment will be posted to idx #%u\n", idx1);

		// find WRITE WR
		sr2_ctrl = IBV_FIND_WQE(client, sr2_wrid);

		if(!sr2_ctrl) {
			printf("Failed to find sr2 seg\n");
			pause();
		}

		uint32_t sr2_meta = ntohl(sr2_ctrl->opmod_idx_opcode);
		uint16_t idx2 =  ((sr2_meta >> 8) & (UINT_MAX));
		uint8_t opmod2 = ((sr2_meta >> 24) & (UINT_MAX));
		uint8_t opcode2 = (sr2_meta & USHRT_MAX);

		//printf("sr2 (WRITE) segment will be posted to idx #%u\n", idx2);


		void *seg0 = ((void*)sr0_ctrl) + sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg);

		// need to modify 2 sges
		for(int i=0; i<2; i++) {
			sr0_data[i] = (struct mlx5_wqe_data_seg *) (seg0 + i * sizeof(struct mlx5_wqe_data_seg));
		}

		seg0 = ((void*)sr0_ctrl) + sizeof(struct mlx5_wqe_ctrl_seg);

		sr0_raddr = (struct mlx5_wqe_raddr_seg *) seg0;


		void *seg1 = ((void*)sr1_ctrl) + sizeof(struct mlx5_wqe_ctrl_seg) +
			sizeof(struct mlx5_wqe_atomic_seg) + sizeof(struct mlx5_wqe_raddr_seg);

		sr1_data = (struct mlx5_wqe_data_seg *) seg1;

		seg1 = ((void*)sr1_ctrl) + sizeof(struct mlx5_wqe_ctrl_seg);

		sr1_raddr = (struct mlx5_wqe_raddr_seg *) seg1;

		seg1 = ((void*)sr1_ctrl) + sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg);

		sr1_atomic = (struct mlx5_wqe_atomic_seg *) seg1; 


		void *seg2 = ((void*)sr2_ctrl) + sizeof(struct mlx5_wqe_ctrl_seg);

		sr2_raddr = (struct mlx5_wqe_raddr_seg *) seg2;

		seg2 = ((void*)sr2_ctrl) + sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg);

		sr2_data = (struct mlx5_wqe_data_seg *) seg2; 

		//XXX uncomment temporarily
		sr0_data[0]->addr = htobe64(((uintptr_t) (&sr2_ctrl->qpn_ds)));
		sr0_data[1]->addr = htobe64(((uintptr_t) (&sr2_data->addr)));

		//XXX might increase latency
		sr0_ctrl->fm_ce_se = htonl(0);

		sr2_ctrl->fm_ce_se = htonl(0);

		sr2_ctrl->opmod_idx_opcode = sr2_ctrl->opmod_idx_opcode | 0x09000000; //SEND

		sr1_atomic->swap_add =  htobe64(*((uint64_t *)&sr2_ctrl->opmod_idx_opcode));

		sr2_ctrl->qpn_ds = htonl((0 << 8) | 3);

		sr2_ctrl->opmod_idx_opcode = sr2_ctrl->opmod_idx_opcode & 0x00FFFFFF; //NOOP

		sr2_ctrl->imm = htonl(k+1);

		sr1_raddr->raddr = htobe64((uintptr_t) &sr2_ctrl->opmod_idx_opcode);
		sr1_atomic->compare = htobe64(*((uint64_t *)&sr2_ctrl->opmod_idx_opcode));
		
		// set up RECV for client inputs
		struct rdma_metadata *recv_meta =  (struct rdma_metadata *)
			calloc(1, sizeof(struct rdma_metadata) + 2 * sizeof(struct ibv_sge));

		recv_meta->sge_entries[0].addr = ((uintptr_t)&sr1_atomic->compare)+1;
		recv_meta->sge_entries[0].length = 3;
		recv_meta->sge_entries[1].addr = (uintptr_t)&sr0_raddr->raddr;
		recv_meta->sge_entries[1].length = 8;
		recv_meta->length = 11;
		recv_meta->sge_count = 2;

		IBV_RECEIVE_SG(client, recv_meta, worker_wq_mr->lkey);


#endif
	
		count_1 += 6;
		count_2 += 1;

		temp1_wrid[k] = sr1_wrid;
		temp2_wrid[k] = sr2_wrid;

		printf("iter lat: %lu usec\n", timer_end(start)/1000);

#if defined(REDN_PARALLEL) || defined(REDN_SEQUENTIAL)
		// rate limit
		while(k - n_hash_req/BUCKET_COUNT > 200)
			ibw_cpu_relax();
#else

#ifdef LAT
		// rate limit
		while(k - n_hash_req > 1000)
			ibw_cpu_relax();
#else
		// rate limit
		while(k - n_hash_req > 1000)
			ibw_cpu_relax();
#endif
		
#endif
	}
}

int main(int argc, char **argv) {

    int shm_fd, shm_ret;
    char *server_ip = argv[1];
	uint32_t get_key;

    // 1. setting
    timer = (struct time_stats*) malloc(sizeof(struct time_stats));
    int *shm_proc = (int*)create_shm(&shm_fd, &shm_ret);

    pthread_spin_init(&sock_lock, PTHREAD_PROCESS_PRIVATE);

    // 2. argument 
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
    allocate_physical_memory(BUFFER_SIZE);

    // 4. rdma connection (server & client)
    init_rdma_agent(portno, regions, MR_COUNT, 2, isClient, add_peer_socket, remove_peer_socket, test_callback);

    // 5. client or server 
    if (isClient) { 
        printf("[Client] Connecting to %s\n", server_ip);
        int iters = atoi(argv[2]);

        if (iters > OFFLOAD_COUNT) {
            return 1;
        }
		time_stats_init(timer, iters);
        
        // return sockfd
        master_sock = add_connection(server_ip, portno, SOCK_MASTER, 1, 0);
    } 
    else {	// server
        printf("[Server] Listening on %s\n", portno);

        printf("---- Initializing hashmap ----\n");
		addr_t addr = regions[MR_DATA].addr;
        struct hash_bucket *bucket = (struct hash_bucket*)addr;

		for (int i = 0; i < HASH_SIZE; i++)
		{
			bucket[i].key[0] = i;
			bucket[i].addr = htobe64((uintptr_t)&bucket[i].value[0]);
			bucket[i].value[0] = i;

			printf("bucket[%d] key=%u addr=%lu\n", i, *((uint32_t *)bucket[i].key), be64toh(bucket[i].addr)); 
		}
		

		pthread_join(comm_thread, NULL);
    	free_physical_memory(addr, BUFFER_SIZE);
    	printf("Agent thread terminated. Exiting main.\n");

		return 0;
    }

	while(!(rc_ready(master_sock)) && !stop) {
				asm("");
	}

    // 6. Run in client mode
#if REDN_PARALLEL
	for(int h=0; h<BUCKET_COUNT; h++) {
		client_sock[h] = add_connection(argv[1], portno, SOCK_CLIENT, 1, IBV_EXP_QP_CREATE_MANAGED_SEND);
	}
#else
	client_sock[0] = add_connection(argv[1], portno, SOCK_CLIENT, 1, IBV_EXP_QP_CREATE_MANAGED_SEND);
#endif

	printf("Starting benchmark ...\n");

	int response_id = 1;
	for(int i=0; i<iters; i++) {

#if REDN_PARALLEL
		for(int h=0; h<BUCKET_COUNT; h++)
			IBV_RECEIVE_IMM(client_sock[h]);
#else
		IBV_RECEIVE_IMM(client_sock[0]);
#endif
	
	get_key = rand() % HASH_SIZE;
	post_get_req_sync(client_sock, get_key, mr_remote_addr(client_sock[0], MR_DATA), response_id);
	}

	time_stats_print(timer, "Run Complete");

	return 0;
}