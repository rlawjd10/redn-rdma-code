#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <rdma/rdma_cma.h>
#include "globals.h"
#include "uthash.h"

//---------mlx5 metadata
struct mlx5_wqe_wait_en_seg { // wait enable segment - 특정 조건이 충족될 때까지 대기
	uint8_t		rsvd0[8]; // 예약된 공간
	uint32_t	pi; // producer index (현재 WQE의 순서)
	uint32_t	obj_num; // object number (wait 대상 obj id)
};

struct mlx5_wqe_inline_seg { // inline data segemtn - 작은 크기의 데이터를 메모리에 저장 안하고 WQE 내부에 직접 삽입 (<128 bytes)
	uint32_t	byte_count; // inline data size
};

#if 0 
//---------device metadata
struct context {
	struct ibv_context *ctx; // rdma device context
	struct ibv_pd *pd; // protection domain
	struct id_record *id_by_addr; //addr to id hashmap
	struct id_record *id_by_qp; //qp to id hashmap
};
#else
struct context { // scalability
	int n_dev; //number of RDMA devices
	struct ibv_context *ctx[MAX_DEVICES];
	struct ibv_pd *pd[MAX_DEVICES];
    // uthash
	struct id_record *id_by_addr; //addr to id hashmap
	struct id_record *id_by_qp; //qp to id hashmap
};
#endif

//---------connection metadata
struct conn_context
{
	int devid;

	//unique connection (socket) descriptor
    // RDMA 연결 설정 및 QP 정보 교환 -> 서버 메모리 주소 (Rkey)
	int sockfd;

	//connection state
	int state;

	//app identifier
	int app_type;

	//internal queue (mellanox direct verbs)
    // rdma_create_qp()보다 세밀한 제어가 가능 (cross-channel, advanced atomic, direct data placement mode)
    // rdma_create_qp()는 일반적인 QP 설정, mlx5dv_qp는 하드웨어 수준에서 QP 속성 조정정
	struct mlx5dv_qp *iqp;

	//internal completion queue
	struct mlx5dv_cq *icq;

	//completion queue
	struct ibv_cq *cq;

	//completion channel
	struct ibv_comp_channel *comp_channel;

	//background cq polling thread
	pthread_t cq_poller_thread;

	//polling mode: 1 means cq thread is always active, 0 means only during bootstrap
	int poll_always;

	//enables or disables background polling thread
	int poll_enable;

	//provides completion poll permission (in case of multiple threads)
	int poll_permission;

	//registered memory regions
	struct ibv_mr **local_mr;
	struct mr_context **remote_mr;

	//checks whether mr init message have been sent/recv'd
	int mr_init_sent;
	int mr_init_recv;

	//bootstrap flags (signifies whether access permissions are available for an mr)
	int *local_mr_ready;
	int *remote_mr_ready;
	int *local_mr_sent;

	//total number of remote MRs
	int remote_mr_total;

	//idx of local_mr to be sent next; (다음에 전송할 local mr index)
	int local_mr_to_sync;

	//send/rcv buffers
	struct ibv_mr **msg_send_mr;
	struct ibv_mr **msg_rcv_mr;
	struct message **msg_send;
	struct message **msg_rcv;

	//determines acquisitions of send buffers
	int *send_slots;
	uint8_t send_idx;

	//connection id
	struct rdma_cm_id *id;

	//locking and synchronization
	uint32_t last_send;
	uint32_t last_send_compl;
	uint32_t last_rcv;
	uint32_t last_rcv_compl;
	uint32_t last_msg; //used to ensure no conflicts when writing on msg send buffer

	uint32_t n_posted_ops; // number of posted (signaled) send/receive operations

	pthread_mutex_t wr_lock;
	pthread_cond_t wr_completed;
    // 같은 소켓에서 WR ID 순서를 유지하기 위한 lock
	pthread_spinlock_t post_lock; //ensures that rdma ops on the same socket have monotonically increasing wr id

	struct app_response *pendings; //hashmap of pending application responses (used exclusively by application)
	struct buffer_record *buffer_bindings; //hashmap of send buffer ownership per wr_id
	pthread_spinlock_t buffer_lock; //concurrent access to buffer hashtable

	pthread_spinlock_t init_lock; //used during initialization

	// QP buffers
	int sq_mr_idx;
	int rq_mr_idx;
	void *sq_start; // 송신 큐 시작 주소
	size_t sq_wqe_cnt; // 송신 큐 WQE 개수
	//addr_t sq_cur_post;
	uint64_t *sq_wrid; // 송신 큐 WR ID 저장
	void *sq_end; // 송신 큐 끝 주소
	uint32_t scur_post; // 현재 송신 포스트 인덱스
	uint32_t bf_offset; // BlueFlame 버퍼 오프셋
	pthread_spinlock_t bf_lock; // BlueFlame lock

	uint8_t		fm_ce_se_tbl[8];    // fast memory completion
	uint8_t		fm_ce_se_acc[32];   // fast memory completion acc

	uint8_t		sq_signal_bits; // 송신 queue signal flag

	// QP flags
	int flags; // local
	int rflags; //remote
	
	UT_hash_handle qp_hh;
};


// ------------ MLX5 defs ---------------

enum {
	SEND_WQE_BB	= 64,
	SEND_WQE_SHIFT	= 6,
};

struct wqe_ctrl_seg {
	uint32_t	opmod_idx_opcode;
	uint32_t	qpn_ds;
	uint8_t		signature;
	uint8_t		rsvd[2];
	uint8_t		fm_ce_se;
	uint32_t	imm;
};

struct wqe_data_seg {
	uint32_t	byte_count;
	uint32_t	lkey;
	uint64_t	addr;
};

struct wqe_raddr_seg {
	uint64_t	raddr;
	uint32_t	rkey;
	uint32_t	reserved;
};

struct wqe_atomic_seg {
	uint64_t	swap_add;
	uint64_t	compare;
};

struct wqe_inl_data_seg {
	uint32_t	byte_count;
};

struct wqe_wait_en_seg {
	uint8_t		rsvd0[8];
	uint32_t	pi;
	uint32_t	obj_num;
};

//--------memory region metadta
struct mr_context
{
    int type;
	addr_t addr;
	addr_t length;	
	//access keys
	uint32_t lkey;
	uint32_t rkey;
	// 1 if physical mr; otherwise virtual
	int physical;
};

//---------RC metadata
typedef struct rc_metadata {
	int flags;
	int type;
	int mr_count;
	// 원격 MR 주소, 크기, rkey
	uint64_t addr[MAX_MR];
	uint64_t length[MAX_MR];
	uint32_t rkey[MAX_MR];
} rc_meta_t;

//---------rdma operation metadata
typedef struct rdma_metadata {
	int op;
	uint32_t wr_id;
	addr_t addr;
	addr_t length;
	uint32_t imm;
	int sge_count;
	struct rdma_metadata *next;
	struct ibv_sge sge_entries[];
} rdma_meta_t;

//---------user-level metadata
//msg payload
struct app_context {
	int sockfd;  //socket id on which the msg came on 
	uint32_t id; //can be used as an app-specific request-response matcher
	char* data;  //convenience pointer to data blocks
};

//msg response tracker
struct app_response { //used to keep track of pending responses
	uint32_t id;
	int ready;
	UT_hash_handle hh;
};

#endif
