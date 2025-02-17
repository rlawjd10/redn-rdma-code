#include "mlx5_intf.h"
#include "connection.h"

//event channel. used to setup rdma RCs and communicate mr keys
struct rdma_event_channel *ec = NULL;

//local memory regions (for rdma reads/writes)
int num_mrs;
struct mr_context *mrs = NULL;

int msg_size; //msg buffer size

int global_sq_sz = MAX_SEND_QUEUE_SIZE; //1024

pthread_mutexattr_t attr;
pthread_mutex_t cq_lock;

const int TIMEOUT_IN_MS = 500;
const char* DEFAULT_PORT = "12345";

static struct context *s_ctx = NULL; // scalability context
static int *s_conn_bitmap = NULL;
static struct rdma_cm_id **s_conn_ids = NULL;
static pre_conn_cb_fn s_on_pre_conn_cb = NULL;
static connect_cb_fn s_on_connect_cb = NULL;
static completion_cb_fn s_on_completion_cb = NULL;
static disconnect_cb_fn s_on_disconnect_cb = NULL;

int exit_rc_loop = 0;

/* ----- utils.h inline fuction ----- */
extern inline void set_seed(int seed);
extern inline int fastrand(int seed);
extern inline int cmp_counters(uint32_t a, uint32_t b);
extern inline int diff_counters(uint32_t a, uint32_t b);
extern inline int find_first_empty_bit_and_set(int bitmap[], int n);
extern inline int find_first_empty_bit(int bitmap[], int n);
extern inline int find_next_empty_bit(int idx, int bitmap[], int n);
extern inline int find_first_set_bit_and_empty(int bitmap[], int n);
extern inline int find_first_set_bit(int bitmap[], int n);
extern inline int find_next_set_bit(int idx, int bitmap[], int n);
extern inline int find_bitmap_weight(int bitmap[], int n);
extern inline struct sockaddr_in * copy_ipv4_sockaddr(struct sockaddr_storage *in);

//connection management (연결, QP 생성, 연결 완료)
__attribute__((visibility ("hidden"))) 
int init_connection(struct rdma_cm_id *id, int type, int always_poll, int flags)
{
	int sockfd = find_first_empty_bit_and_set(s_conn_bitmap, MAX_CONNECTIONS);

	if(sockfd < 0)
		rc_die("can't open new connection; number of open sockets == MAX_CONNECTIONS");

	//debug_print("adding connection on socket #%d\n", sockfd);

	struct conn_context *ctx = (struct conn_context *)calloc(1, sizeof(struct conn_context)); // 104 bytes	
    ctx->local_mr = (struct ibv_mr **)calloc(MAX_MR, sizeof(struct ibv_mr*));               // 8 bytes * 2 = 16 bytes
	ctx->remote_mr = (struct mr_context **)calloc(MAX_MR, sizeof(struct mr_context*));      // 8 bytes * 2 = 16 bytes
	ctx->remote_mr_ready = (int *)calloc(MAX_MR, sizeof(int));                              // 4 bytes * 2 = 8 bytes
	ctx->local_mr_ready = (int *)calloc(MAX_MR, sizeof(int));                               // 4 bytes * 2 = 8 bytes
	ctx->local_mr_sent = (int *)calloc(MAX_MR, sizeof(int));                                // 4 bytes * 2 = 8 bytes
	ctx->msg_send_mr = (struct ibv_mr **)calloc(MAX_BUFFER, sizeof(struct ibv_mr*));        // 8 bytes * 1 = 8 bytes
	ctx->msg_rcv_mr = (struct ibv_mr **)calloc(MAX_BUFFER, sizeof(struct ibv_mr*));         // 8 bytes * 1 = 8 bytes
	ctx->msg_send = (struct message **)calloc(MAX_BUFFER, sizeof(struct message*));         // 8 bytes * 1 = 8 bytes
	ctx->msg_rcv = (struct message **)calloc(MAX_BUFFER, sizeof(struct message*));          // 8 bytes * 1 = 8 bytes
	ctx->send_slots = (int *)calloc(MAX_BUFFER, sizeof(int));                               // 4 bytes * 1 = 4 bytes
	ctx->pendings = NULL; 
	ctx->buffer_bindings = NULL;
	ctx->poll_permission = 1;
	ctx->poll_always = always_poll;
	ctx->poll_enable = 1;
	ctx->flags = flags;

	id->context = ctx;
	ctx->id = id; // rdma_cm_id 구조체

	// spin lock 초기화
	pthread_mutex_init(&ctx->wr_lock, NULL);
	pthread_cond_init(&ctx->wr_completed, NULL);	// condition variable 초기화
	pthread_spin_init(&ctx->post_lock, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_init(&ctx->buffer_lock, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_init(&ctx->init_lock, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_init(&ctx->bf_lock, PTHREAD_PROCESS_PRIVATE);

	// 소켓 및 연결 정보 저장.
	s_conn_bitmap[sockfd] = 1;
	ctx->sockfd = sockfd;
	ctx->app_type = type;
	s_conn_ids[sockfd] = id;

	return sockfd;
}

__attribute__((visibility ("hidden"))) // resolve() 후에 private_mr, QP, mlx5dv, CQ 설정 
int setup_connection(struct rdma_cm_id * id, struct rdma_conn_param * cm_params)
{
	struct ibv_qp_init_attr qp_attr;
	struct conn_context *ctx = (struct conn_context *)id->context;
	struct rc_metadata *rc_meta = NULL;

	try_build_device(id);	// device 정보 조회 및 생성

#if 1	// rdma 연결 메타데이터 (private_data) 업데이트 및 원격 MR과 동기화
	if(cm_params)
		rc_meta = (struct rc_metadata *) cm_params->private_data;

	// modify connection parameters using metadata exchanged between client and server
	if(rc_meta) {
		// debug_print("private data %p (len: given %d expected %lu)\n",
		// 		rc_meta, cm_params->private_data_len,
		// 		sizeof(struct rc_metadata) + MAX_MR * sizeof(struct ibv_mr));

		// rc_meta 정보를 기반으로 MR 업데이트
		if(sizeof(struct rc_metadata) > cm_params->private_data_len)	
			rc_die("invalid connection param length");
		
		ctx->flags = rc_meta->flags;
	    ctx->app_type = rc_meta->type;

		mr_remote_update(ctx, rc_meta->addr, rc_meta->length, rc_meta->rkey, rc_meta->mr_count);
	}
#endif
	// build send, receive, and completion queues
	build_cq_channel(id);
	build_qp_attr(id, &qp_attr);

	if(rdma_create_qp(id, rc_get_pd(id), &qp_attr))
		rc_die("queue pair creation failed");


	/* MLX5 Direct Verbs를 사용하여 RDMA QP 초기화 */
	// 즉, QP를 low-level hardware objects로 변환하여 direct verbs API에서 사용할 수 있도록 초기화
	// rdma-core 보다 더 낮은 수준에서 QP를 직접 제어 -> 하드웨어 직접 접근
	struct mlx5dv_obj dv_obj = {};
	memset((void *)&dv_obj, 0, sizeof(struct mlx5dv_obj));
	ctx->iqp = (struct mlx5dv_qp *)malloc(sizeof(struct mlx5dv_qp));

	dv_obj.qp.in = id->qp;
	dv_obj.qp.out = ctx->iqp;

 	int ret = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_QP); // libmlx5/mlx5.c

	ctx->sq_wrid = calloc(ctx->iqp->sq.wqe_cnt, sizeof(uint64_t));

	// QP signal bits 설정, 모든 송신 큐 요청에 대한 CQ 업데이트
	if(qp_attr.sq_sig_all)
		ctx->sq_signal_bits = MLX5_WQE_CTRL_CQ_UPDATE; // 8
	else
		ctx->sq_signal_bits = 0;	// CQ에 신호를 보내지 않고 송신 큐 요청만 처리

	// update connection hash tables (RDMA QP 및 연결정보를 빠르게 검색하기 위한 해시 테이블)
	struct id_record *entry = (struct id_record *)calloc(1, sizeof(struct id_record));
	struct sockaddr_in *addr_p = copy_ipv4_sockaddr(&id->route.addr.src_storage);
	
	if(addr_p == NULL)
		rc_die("compatibility issue: can't use a non-IPv4 address");

	entry->addr = *addr_p;	
	entry->qp_num = id->qp->qp_num;
	entry->id = id;

	//add the structure to both hash tables
	// IP는 rdma_resolve_*를 통해 사용(서버 간 연결 수립립),  QP는 RDMA 연결 후에 RDMA 작업을 함 
	HASH_ADD(qp_hh, s_ctx->id_by_qp, qp_num, sizeof(uint32_t), entry);
	HASH_ADD(addr_hh, s_ctx->id_by_addr, addr, sizeof(struct sockaddr_in), entry);
}

__attribute__((visibility ("hidden"))) // RDMA_CM_EVENT_ESTABLISHED 후 호출 
int finalize_connection(struct rdma_cm_id * id, struct rdma_conn_param * cm_params)
{
	struct conn_context *ctx = (struct conn_context *)id->context;
	struct rc_metadata *rc_meta = NULL;

#if 1
	if(cm_params)
		rc_meta = (struct rc_metadata *) cm_params->private_data;

	// modify connection parameters using metadata exchanged between client and server
	if(rc_meta) {

		if(sizeof(struct rc_metadata) > cm_params->private_data_len)
			rc_die("invalid connection param length");

		mr_remote_update(ctx, rc_meta->addr, rc_meta->length, rc_meta->rkey, rc_meta->mr_count);
	}
#endif

	rc_set_state(id, RC_CONNECTION_READY);
}

// device management
__attribute__((visibility ("hidden"))) // device
void try_build_device(struct rdma_cm_id *id)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	// RDMA 장치 컨텍스트가 없으면, 생성하고 기본 값을 초기화 
	if (!s_ctx) {
		s_ctx = (struct context *)malloc(sizeof(struct context));
		s_ctx->id_by_addr = NULL;
		s_ctx->id_by_qp = NULL;
		s_ctx->n_dev = 0;

		for(int i=0; i<MAX_DEVICES; i++) {
			s_ctx->ctx[i] = NULL;
			s_ctx->pd[i] = NULL;
		}
	}

	// 기존 RDMA 장치가 있는지 확인 (s_ctx->ctx[])
	for(int i=0; i<s_ctx->n_dev; i++) {
		if(s_ctx->ctx[i] == id->verbs) {
			ctx->devid = i;
			return;
		}
	}

	if(s_ctx->n_dev == MAX_DEVICES)
		rc_die("failed to allocate new rdma device. try increasing MAX_DEVICES.");

	// allocate new device (새로운 RDMA 장치 등록)
	//debug_print("initializing rdma device-%d\n", s_ctx->n_dev);
	s_ctx->ctx[s_ctx->n_dev] = id->verbs; 
	s_ctx->pd[s_ctx->n_dev] = ibv_alloc_pd(id->verbs); // pd는 rdma qp와 mr을 보호 
	ctx->devid = s_ctx->n_dev;
	s_ctx->n_dev++;
}

// qp & context management (이벤트 수신할 채널 생성, QP 속성 및 매개변수 설정)
__attribute__((visibility ("hidden"))) 
void build_cq_channel(struct rdma_cm_id *id)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	int idx = ctx->devid; // try_build_device()에서 설정한 devid

	// create completion queue channel
	ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx[idx]);

	if (!ctx->comp_channel)
		rc_die("ibv_create_comp_channel() failed");

	// 50개의 CQE를 할당 
	ctx->cq = ibv_create_cq(s_ctx->ctx[idx], 50, NULL, ctx->comp_channel, 0); /* cqe=10 is arbitrary */

	if (!ctx->cq)
		rc_die("Failed to create CQ");

	// CQ 이벤트 발생 시 알림을 받을 수 있도록 
	ibv_req_notify_cq(ctx->cq, 0);

	// Mellanox Direct Verbs를 사용하여 CQ 초기화 
	struct mlx5dv_obj dv_obj = {};
	memset((void *)&dv_obj, 0, sizeof(struct mlx5dv_obj));
	ctx->icq = (struct mlx5dv_cq *)malloc(sizeof(struct mlx5dv_cq));

	dv_obj.cq.in = ctx->cq;
	dv_obj.cq.out = ctx->icq;

 	int ret = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_CQ);

	//printf("creating background thread to poll completions (blocking)\n");
	
	// CQ 이벤트 폴링하는 백그라운드 스레드 
	pthread_create(&ctx->cq_poller_thread, NULL, poll_cq_blocking_loop, ctx);
}

__attribute__((visibility ("hidden"))) 
void build_qp_attr(struct rdma_cm_id *id, struct ibv_qp_init_attr *qp_attr)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	memset(qp_attr, 0, sizeof(*qp_attr));

	qp_attr->send_cq = ctx->cq;
	qp_attr->recv_cq = ctx->cq;
	qp_attr->qp_type = IBV_QPT_RC;

	qp_attr->cap.max_send_wr = MAX_SEND_QUEUE_SIZE;
	qp_attr->cap.max_recv_wr = MAX_RECV_QUEUE_SIZE;
	qp_attr->cap.max_send_sge = 16;
	qp_attr->cap.max_recv_sge = 16;
}

__attribute__((visibility ("hidden"))) // RDMA 연결 파라미터 설정 (connect server)
void build_rc_params(struct rdma_cm_id *id, struct rdma_conn_param *params)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	memset(params, 0, sizeof(*params));

	params->initiator_depth = params->responder_resources = 1;
	params->rnr_retry_count = 7; /* infinite retry */

	struct rc_metadata *meta = (struct rc_metadata *)calloc(1, sizeof(struct rc_metadata));

	// 원격 MR 정보를 클라이언트 저장 
	meta->flags = ctx->flags;
	meta->type = ctx->app_type;
	meta->mr_count = num_mrs;
	for(int i=0; i<meta->mr_count; i++) {
		meta->addr[i] = (uintptr_t) ctx->local_mr[i]->addr;
		meta->length[i] = ctx->local_mr[i]->length;
		meta->rkey[i] = ctx->local_mr[i]->rkey;
	}

	params->private_data = meta;
	params->private_data_len = sizeof(*meta);

	if(sizeof(*meta) > 56)
		rc_die("metadata length greater than max allowed size\n");
}


/* ----- event handling ----- */
__attribute__((visibility ("hidden"))) 
void rdma_event_loop(struct rdma_event_channel *ec, int exit_on_connect, int exit_on_disconnect)
{
	struct rdma_cm_event *event = NULL;
	struct rdma_conn_param cm_params;

	while (rdma_get_cm_event(ec, &event) == 0) {
		struct rdma_cm_event event_copy;

		memcpy(&event_copy, event, sizeof(*event));
		//debug_print("received event[%d]: %s\n", event_copy.event, rdma_event_str(event_copy.event));

		rdma_ack_cm_event(event);

		if (event_copy.event == RDMA_CM_EVENT_ADDR_RESOLVED) {
			rdma_resolve_route(event_copy.id, TIMEOUT_IN_MS);
		}
		//client
		else if (event_copy.event == RDMA_CM_EVENT_ROUTE_RESOLVED) {	
			setup_connection(event_copy.id, NULL);

			if (s_on_pre_conn_cb) {
				//debug_print("trigger pre-connection callback\n");
				s_on_pre_conn_cb(event_copy.id);
			}
			// connecting to the server...
			build_rc_params(event_copy.id, &cm_params);
			if(rdma_connect(event_copy.id, &cm_params))
				rc_die("failed to connect\n");
		}
		// server
		else if (event_copy.event == RDMA_CM_EVENT_CONNECT_REQUEST) { // 연결 요청 수신 및 private_data 확인 
			int always_poll = 1;

			struct rc_metadata *rc_meta_temp;
			if(&event_copy.param.conn)
				rc_meta_temp = (struct rc_metadata *) (&event_copy.param.conn)->private_data;

			init_connection(event_copy.id, -1, always_poll, 0);
			setup_connection(event_copy.id, &event_copy.param.conn);

			if (s_on_pre_conn_cb) {
				//debug_print("trigger pre-connection callback\n");
				s_on_pre_conn_cb(event_copy.id);
			}

			build_rc_params(event_copy.id, &cm_params);
			rdma_accept(event_copy.id, &cm_params);
		}
		// client & Server
		else if (event_copy.event == RDMA_CM_EVENT_ESTABLISHED) {
			finalize_connection(event_copy.id, &event_copy.param.conn);
			
			if (s_on_connect_cb) {
				//debug_print("trigger post-connection callback\n");
				s_on_connect_cb(event_copy.id);
			}
			if (exit_on_connect)
				break;
		}
		else if (event_copy.event == RDMA_CM_EVENT_DISCONNECTED) {
			if (s_on_disconnect_cb) {
				debug_print("trigger disconnection callback\n");
				s_on_disconnect_cb(event_copy.id);
			}

			rc_disconnect(event_copy.id);

			if (exit_on_disconnect) {
				rc_clear(event_copy.id);
				break;
			}

		}
		else if (event_copy.event == RDMA_CM_EVENT_REJECTED) {
			debug_print("rejection reason: %d\n", event_copy.status);
			rc_die("Connection failure. Exiting..");
		}
		else if (event_copy.event == RDMA_CM_EVENT_TIMEWAIT_EXIT) {
			//this event indicates that the recently destroyed queue pair is ready to be reused
			//at this point, clean up any allocated memory for connection
			rc_clear(event_copy.id);
		}
		else {
			rc_die("Unhandled event. Exiting..");
		}
	}
}


/* ----- request completions ----- */
__attribute__((visibility ("hidden"))) 
void update_completions(struct ibv_wc *wc) // work completion 업데이트
{
	//FIXME: one hash search is performed for every send completion. Optimize!
	struct rdma_cm_id *id = find_connection_by_wc(wc);
	struct conn_context *ctx = (struct conn_context *)id->context;

	//signal any threads blocking on wr.id
	if (wc->opcode & IBV_WC_RECV) {
		debug_print("COMPLETION --> (RECV WR #%lu) [qp_num %u]\n", wc->wr_id, id->qp->qp_num);
		//ctx->last_rcv_compl = 1;
	}
	else {
		debug_print("COMPLETION --> (SEND WR #%lu) [qp_num %u]\n", wc->wr_id, id->qp->qp_num);
		//pthread_mutex_lock(&ctx->wr_lock);
		ctx->last_send_compl = wc->wr_id;
		//pthread_cond_broadcast(&ctx->wr_completed);	
		//pthread_mutex_unlock(&ctx->wr_lock);
	}
}

// synchronous rdma operations
__attribute__((visibility ("hidden")))
void spin_till_response(struct rdma_cm_id *id, uint32_t seqn)
{
	struct ibv_wc wc;

	struct conn_context *ctx = (struct conn_context *)id->context;

	//debug_print("spinning till response with seqn %u (last received seqn -> %u)\n",
	//		seqn, last_compl_wr_id(ctx, 0));

	//while(!ibw_cmpxchg(&ctx->poll_permission, 1, 0))
	//	ibw_cpu_relax();

	// seqn(app id)은 마지막 수신 완료된 WR ID보다 작거나 MAX_PENDING보다 크면 대기
	// 원하는 WR ID가 도착할 때까지 대기 (completion과의 차이점)
	while(ctx->last_rcv_compl < seqn || ((ctx->last_rcv_compl - seqn) > MAX_PENDING)) {
		poll_cq(ctx->cq, &wc);
		ibw_cpu_relax();
	}

	return;
	//if(ibw_cmpxchg(&ctx->poll_permission, 0, 1))
	//	rc_die("failed to give up permission; possible race condition");
}

__attribute__((visibility ("hidden")))
void block_till_response(struct rdma_cm_id *id, uint32_t seqn)
{
	struct ibv_cq *cq;
	struct ibv_wc wc;
	void *ev_ctx;

	struct conn_context *ctx = (struct conn_context *)id->context;

	//debug_print("spinning till response with seqn %u (last received seqn -> %u)\n",
	//		seqn, last_compl_wr_id(ctx, 0));

	//while(!ibw_cmpxchg(&ctx->poll_permission, 1, 0))
	//	ibw_cpu_relax();

	while(ctx->last_rcv_compl < seqn || ((ctx->last_rcv_compl - seqn) > MAX_PENDING)) {
		ibv_get_cq_event(ctx->comp_channel, &cq, &ev_ctx);	// CQ 이벤트가 완료될 때까지 대기 (경쟁 피하기 위해서 )
		ibv_ack_cq_events(cq, 1);	//ack 승인 but mutex 비용 
		ibv_req_notify_cq(cq, 0);	// 새로운 이벤트가 발생하면 알림을 받을 수 있도록 0: 모든 CQ 이벤트, 1: 특정 CQ 이벤트 (e.g., IBV_WR_SEND)
		poll_cq(ctx->cq, &wc);
		ibw_cpu_relax();
	}

	return;
	//if(ibw_cmpxchg(&ctx->poll_permission, 0, 1))
	//	rc_die("failed to give up permission; possible race condition");
}

//spin till we receive a completion with wr_id (overrides poll_cq loop)
void spin_till_completion(struct rdma_cm_id *id, uint32_t wr_id)
{
	struct ibv_wc wc;

	struct conn_context *ctx = (struct conn_context *)id->context;

	//debug_print("spinning till WR %u completes (last completed WR -> %u)\n",
	//		wr_id, last_compl_wr_id(ctx, 1));

	// wr_id가 last_compl_wr_id보다 크면 대기 (완료되지 않음)
	while(cmp_counters(wr_id, last_compl_wr_id(ctx, 1)) > 0) {
		ibw_cpu_relax();
	}

}

//spin till we receive a completion with wr_id (overrides poll_cq loop)
void block_till_completion(struct rdma_cm_id *id, uint32_t wr_id)
{
	struct ibv_cq *cq;
	struct ibv_wc wc;
	void *ev_ctx;

	struct conn_context *ctx = (struct conn_context *)id->context;

	//debug_print("blocking till WR %u completes (last completed WR -> %u)\n",
	//		wr_id, last_compl_wr_id(ctx, 1));
#if 1
	while(!ibw_cmpxchg(&ctx->poll_permission, 1, 0))
		ibw_cpu_relax();

	while(cmp_counters(wr_id, last_compl_wr_id(ctx, 1)) > 0) {
		ibv_get_cq_event(ctx->comp_channel, &cq, &ev_ctx);
		ibv_ack_cq_events(cq, 1);
		ibv_req_notify_cq(cq, 0);
		poll_cq(ctx->cq, &wc);
		ibw_cpu_relax();
	}

	if(ibw_cmpxchg(&ctx->poll_permission, 0, 1))
		rc_die("failed to reset permission; possible race condition");
#else
	while(cmp_counters(wr_id, last_compl_wr_id(ctx, 1)) > 0) {
		ibw_cpu_relax();
	}
#endif
}


/* ----- completion polling ----- */
__attribute__((visibility ("hidden"))) 
void * poll_cq_spinning_loop(void *ctx)
{
	struct ibv_wc wc;

	while(((struct conn_context*)ctx)->poll_enable) {
		if(((struct conn_context*)ctx)->poll_permission)
			poll_cq(((struct conn_context*)ctx)->cq, &wc);	
		ibw_cpu_relax();
	}

	printf("end poll_cq loop for sockfd %d\n", ((struct conn_context*)ctx)->sockfd);
	return NULL;
}

__attribute__((visibility ("hidden"))) 
void * poll_cq_blocking_loop(void *ctx)
{
	struct ibv_cq *cq;
	struct ibv_wc wc;
	void *ev_ctx;

	while(((struct conn_context*)ctx)->poll_enable) {
		// CQ 이벤트가 발생할 때까지 대기 
		ibv_get_cq_event(((struct conn_context*)ctx)->comp_channel, &cq, &ev_ctx);
		ibv_ack_cq_events(cq, 1);
		ibv_req_notify_cq(cq, 0);
		poll_cq(((struct conn_context*)ctx)->cq, &wc);
	}

	printf("end poll_cq loop for sockfd %d\n", ((struct conn_context*)ctx)->sockfd);
	return NULL;
}

__attribute__((visibility ("hidden"))) 
inline void poll_cq(struct ibv_cq *cq, struct ibv_wc *wc)
{
	while(ibv_poll_cq(cq, 1, wc)) {
		if (wc->status == IBV_WC_SUCCESS) {
			update_completions(wc);
			s_on_completion_cb(wc);
		}
		else {
#if 1
			const char *descr;
			descr = ibv_wc_status_str(wc->status);
			struct rdma_cm_id *id = find_connection_by_wc(wc);
			struct conn_context *ctx = (struct conn_context *)id->context;
			// printf("COMPLETION FAILURE on sockfd %d (%s WR #%lu) status[%d] = %s\n",
			// 		ctx->sockfd, (wc->opcode & IBV_WC_RECV)?"RECV":"SEND",
			// 		wc->wr_id, wc->status, descr);
#endif
		}
	}
}

__attribute__((visibility ("hidden"))) 
inline void poll_cq_debug(struct ibv_cq *cq, struct ibv_wc *wc)
{
	while(ibv_poll_cq(cq, 1, wc)) {
		if (wc->status == IBV_WC_SUCCESS) {
			update_completions(wc);

			//debug_print("trigger completion callback\n");
			s_on_completion_cb(wc);

			if(wc->opcode & IBV_WC_RECV)
				return;
		}
		else {
#ifdef DEBUG
			const char *descr;
			descr = ibv_wc_status_str(wc->status);
			debug_print("COMPLETION FAILURE (%s WR #%lu) status[%d] = %s\n",
					(wc->opcode & IBV_WC_RECV)?"RECV":"SEND",
					wc->wr_id, wc->status, descr);
#endif
			rc_die("poll_cq: status is not IBV_WC_SUCCESS");
		}
	}
}


/* ----- helper functions ----- */
__attribute__((visibility ("hidden"))) 
struct rdma_cm_id* find_next_connection(struct rdma_cm_id* id) // 사용 가능한 RDMA 연결 찾기 
{
	int i = 0;

	if(id == NULL)
		i = find_first_set_bit(s_conn_bitmap, MAX_CONNECTIONS); // 첫번째로 채워진 bit
	else {
		struct conn_context *ctx = (struct conn_context *) id->context;
		i = find_next_set_bit(ctx->sockfd, s_conn_bitmap, MAX_CONNECTIONS); // 다음으로 채워진 bit
	}

	if(i >= 0)
		return get_connection(i);
	else
		return NULL;	 
}

__attribute__((visibility ("hidden"))) 
struct rdma_cm_id* find_connection_by_addr(struct sockaddr_in *addr) // IP 주소를 통해 연결 찾기
{
	struct id_record *entry = NULL;
	//debug_print("[hash] looking up id with sockaddr: %s:%hu\n",
	//		inet_ntoa(addr->sin_addr), addr->sin_port);
	// ip 주소를 통해 연결 찾기 
	HASH_FIND(addr_hh, s_ctx->id_by_addr, addr, sizeof(*addr), entry);
	if(!entry)
		rc_die("hash lookup failed; id doesn't exist");
	return entry->id;
}

__attribute__((visibility ("hidden"))) 
struct rdma_cm_id* find_connection_by_wc(struct ibv_wc *wc) 	// work completion을 통해 연결 찾기
{
	struct id_record *entry = NULL;
	//debug_print("[hash] looking up id with qp_num: %u\n", wc->qp_num);
	// QP 번호를 통해 연결 찾기
	HASH_FIND(qp_hh, s_ctx->id_by_qp, &wc->qp_num, sizeof(wc->qp_num), entry);
	if(!entry)
		rc_die("hash lookup failed; id doesn't exist");
	return entry->id;
}

__attribute__((visibility ("hidden"))) 
struct rdma_cm_id* get_connection(int sockfd)	// sockfd를 통해 연결 찾기
{
	if(sockfd > MAX_CONNECTIONS)
		rc_die("invalid sockfd; must be less than MAX_CONNECTIONS");

	if(s_conn_bitmap[sockfd])
		return s_conn_ids[sockfd];
	else
		return NULL;
}


/* ----- rc connection handling ----- */
__attribute__((visibility ("hidden"))) // RDMA 연결 관리 모듈을 초기화 
void rc_init(pre_conn_cb_fn pc, connect_cb_fn conn, completion_cb_fn comp, disconnect_cb_fn disc)
{
	debug_print("initializing RC module\n");
	
	s_conn_bitmap = calloc(MAX_CONNECTIONS, sizeof(int));
	s_conn_ids = (struct rdma_cm_id **)calloc(MAX_CONNECTIONS, sizeof(struct rdma_cm_id*));

	// call back function
	s_on_pre_conn_cb = pc;	// 연결 전 콜백 함수
	s_on_connect_cb = conn;	// 연결 후 콜백 함수
	s_on_completion_cb = comp;	// 완료 콜백 함수
	s_on_disconnect_cb = disc;	// 연결 해제 콜백 함수
}

__attribute__((visibility ("hidden"))) 
void rc_set_state(struct rdma_cm_id *id, int new_state)	// 연결 상태 변경
{
	struct conn_context *ctx = (struct conn_context *) id->context;

	if(ctx->state == new_state)
		return;

	//printf("modify state for socket #%d from %d to %d\n", ctx->sockfd, ctx->state, new_state);

	ctx->state = new_state;
	
	if((ctx->state == RC_CONNECTION_READY && !ctx->poll_always) || ctx->state == RC_CONNECTION_TERMINATED)
		ctx->poll_enable = 0;	
  		//void *ret;
    		//if(pthread_join(ctx->cq_poller_thread, &ret) != 0)
		//	rc_die("pthread_join() error");
	if(ctx->state == RC_CONNECTION_TERMINATED)
		pthread_cancel(ctx->cq_poller_thread);
}

int rc_ready(int sockfd)	// RC_CONNECTION_READY
{
	if(get_connection(sockfd)) {
		struct conn_context *ctx = (struct conn_context *) s_conn_ids[sockfd]->context;
		if(ctx->state == RC_CONNECTION_READY)
			return 1;
		else
			return 0;
	}
	else
		return 0;
}

int rc_active(int sockfd)	// RC_CONNECTION_ACTIVE
{
	if(get_connection(sockfd)) {
		struct conn_context *ctx = (struct conn_context *) s_conn_ids[sockfd]->context;
		if(ctx->state >= RC_CONNECTION_ACTIVE)
			return 1;
		else
			return 0;
	}
	else
		return 0;
}

int rc_terminated(int sockfd)	// RC_CONNECTION_TERMINATED
{
	if(get_connection(sockfd)) {
		struct conn_context *ctx = (struct conn_context *) s_conn_ids[sockfd]->context;
		if(ctx->state == RC_CONNECTION_TERMINATED)
			return 1;
		else
			return 0;
	}
	else
		return 0;
}

/* ----- connection info (lookup metadata) ----- */
int rc_connection_count()
{
	//return HASH_CNT(qp_hh, s_ctx->id_by_qp);
	return find_bitmap_weight(s_conn_bitmap, MAX_CONNECTIONS); 
}

int rc_next_connection(int cur)	
{
	int i = 0;

	if(cur < 0)
		i = find_first_set_bit(s_conn_bitmap, MAX_CONNECTIONS);
	else {
		i = find_next_set_bit(cur, s_conn_bitmap, MAX_CONNECTIONS);
	}

	if(i >= 0)
		return i;
	else
		return -1;	 
}

int rc_connection_meta(int sockfd)
{
	if(get_connection(sockfd)) {
		struct conn_context *ctx = (struct conn_context *) s_conn_ids[sockfd]->context;
		return ctx->app_type;
	}
	else
		return -1;
}

char* rc_connection_ip(int sockfd) {
	if(get_connection(sockfd)) {
		struct sockaddr_in *addr_in = copy_ipv4_sockaddr(&s_conn_ids[sockfd]->route.addr.dst_storage);
		char *s = malloc(sizeof(char)*INET_ADDRSTRLEN);
		s = inet_ntoa(addr_in->sin_addr);
		return s;
	}
	else
		return NULL;
}

int rc_connection_qpnum(int sockfd)
{
	if(get_connection(sockfd)) {
		struct conn_context *ctx = (struct conn_context *) s_conn_ids[sockfd]->context;
		return ctx->id->qp->qp_num;
	}
	else
		return -1;
}

int rc_connection_cqnum(int sockfd)
{
	if(get_connection(sockfd)) {
		struct conn_context *ctx = (struct conn_context *) s_conn_ids[sockfd]->context;
		return ctx->icq->cqn;
	}
	else
		return -1;
}

__attribute__((visibility ("hidden"))) 
struct ibv_pd * rc_get_pd(struct rdma_cm_id *id)
{
#if 1
	struct conn_context *ctx = (struct conn_context *) id->context;

	if(ctx->devid > MAX_DEVICES-1)
		rc_die("invalid rdma device index for connection.");

	return s_ctx->pd[ctx->devid];
#else
	return s_ctx->pd;
#endif
}

struct ibv_context * rc_get_context(int id)
{
	return s_ctx->ctx[id];
}


/* ----- buffer ----- */
//FIXME: need a synchronization mechanism in case of simultaneous acquisitions
__attribute__((visibility ("hidden"))) 
int _rc_acquire_buffer(int sockfd, void ** ptr, int user)	// RDMA 송신 버퍼 할당 
{
#if 1
	struct conn_context *ctx = (struct conn_context *) get_connection(sockfd)->context;
	int i = ctx->send_idx++ % MAX_BUFFER;	// 사용 가능한 버퍼 인덱스 
	debug_print("acquire buffer ID = %d on sockfd %d\n", i, sockfd);

	if(user) {
		// buffer metadata 초기화 
		ctx->msg_send[i]->meta.app.id = 0; //set app.id to zero
		ctx->msg_send[i]->meta.app.data = ctx->msg_send[i]->data; //set a convenience pointer for data
		*ptr = (void *) &ctx->msg_send[i]->meta.app; //doesn't matter if we return app or mr
	}

	return i;
#endif
	//return 0;
}

int rc_acquire_buffer(int sockfd, struct app_context ** ptr)
{
	return _rc_acquire_buffer(sockfd, (void **) ptr, 1);
}

//bind acquired buffer to specific wr id
__attribute__((visibility ("hidden"))) 
int rc_bind_buffer(struct rdma_cm_id *id, int buffer, uint32_t wr_id)	// 버퍼와 WR ID 바인딩 
{
#if 1
	debug_print("binding buffer[%d] --> (SEND WR #%u)\n", buffer, wr_id);
	struct conn_context *ctx = (struct conn_context *) id->context;

	// send buffer ownership
	struct buffer_record *rec = calloc(1, sizeof(struct buffer_record));
	rec->wr_id = wr_id;
	rec->buff_id = buffer;

	pthread_spin_lock(&ctx->buffer_lock);
	HASH_ADD(hh, ctx->buffer_bindings, wr_id, sizeof(rec->wr_id), rec);
	pthread_spin_unlock(&ctx->buffer_lock);
	return 1;
#endif
	//return 1;
}

__attribute__((visibility ("hidden"))) 
int rc_release_buffer(int sockfd, uint32_t wr_id)
{
#if 1
	struct conn_context *ctx = (struct conn_context *) s_conn_ids[sockfd]->context;
	struct buffer_record *b;

	pthread_spin_lock(&ctx->buffer_lock);	// hash table 보호 
	HASH_FIND(hh, ctx->buffer_bindings, &wr_id, sizeof(wr_id), b);
	if(b) {
		debug_print("released buffer[%d] --> (SEND WR #%u)\n", b->buff_id, wr_id);
		HASH_DEL(ctx->buffer_bindings, b);
		if(ctx->send_slots[b->buff_id])
			ctx->send_slots[b->buff_id] = 0;
		int ret = b->buff_id;
		free(b);

		pthread_spin_unlock(&ctx->buffer_lock);
		return ret;
	}
	else {
		pthread_spin_unlock(&ctx->buffer_lock);
		rc_die("failed to release buffer. possible race condition.\n");
	}

	return -1;
#endif
	//return 0;
}

/* ----- remove ----- */
__attribute__((visibility ("hidden"))) 
void rc_disconnect(struct rdma_cm_id *id)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	debug_print("terminating connection on socket #%d\n", ctx->sockfd);

	rc_set_state(id, RC_CONNECTION_TERMINATED);

#if 1
	//delete from hashtables
	struct id_record *entry = NULL;
	HASH_FIND(qp_hh, s_ctx->id_by_qp, &id->qp->qp_num, sizeof(id->qp->qp_num), entry);
	if(!entry)
		rc_die("hash delete failed; id doesn't exist");
	HASH_DELETE(qp_hh, s_ctx->id_by_qp, entry);
	HASH_DELETE(addr_hh, s_ctx->id_by_addr, entry);
	free(entry);

	struct app_response *current_p, *tmp_p;
	struct buffer_record *current_b, *tmp_b;

	HASH_ITER(hh, ctx->pendings, current_p, tmp_p) {
		HASH_DEL(ctx->pendings,current_p);
		free(current_p);          
	}

	HASH_ITER(hh, ctx->buffer_bindings, current_b, tmp_b) {
		HASH_DEL(ctx->buffer_bindings,current_b);
		free(current_b);          
	}

	//destroy queue pair and disconnect
	rdma_destroy_qp(id);
	rdma_disconnect(id);
#endif
}

__attribute__((visibility ("hidden"))) 
void rc_clear(struct rdma_cm_id *id)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	if(!rc_terminated(ctx->sockfd))
		rc_die("can't clear metadata for non-terminated connection");

	debug_print("clearing connection metadata for socket #%d\n", ctx->sockfd);
	s_conn_bitmap[ctx->sockfd] = 0;
	s_conn_ids[ctx->sockfd] = NULL;

	for(int i=0; i<MAX_MR; i++) {
		if(ctx->local_mr_ready[i]) {
#if 0
			//XXX removing this for now; only need to deregister MR once
			debug_print("deregistering mr[addr:%lx, len:%lu]\n",
					(uintptr_t)ctx->local_mr[i]->addr, ctx->local_mr[i]->length);
			ibv_dereg_mr(ctx->local_mr[i]);
#endif
		}
		if(ctx->remote_mr_ready[i])
			free(ctx->remote_mr[i]);
	}

	free(ctx->local_mr);
	free(ctx->local_mr_ready);
	free(ctx->local_mr_sent);
	free(ctx->remote_mr);
	free(ctx->remote_mr_ready);

	for(int i=0; i<MAX_BUFFER; i++) {
		debug_print("deregistering msg_send_mr[addr:%lx, len:%lu]\n",
				(uintptr_t)ctx->msg_send_mr[i]->addr, ctx->msg_send_mr[i]->length);
		debug_print("deregistering msg_rcv_mr[addr:%lx, len:%lu]\n",
				(uintptr_t)ctx->msg_rcv_mr[i]->addr, ctx->msg_rcv_mr[i]->length);
		ibv_dereg_mr(ctx->msg_send_mr[i]);
		ibv_dereg_mr(ctx->msg_rcv_mr[i]);
		free(ctx->msg_send[i]);
		free(ctx->msg_rcv[i]);
	}

	free(ctx->msg_send_mr);
	free(ctx->msg_rcv_mr);
	free(ctx->msg_send);
	free(ctx->msg_rcv);
 
	free(ctx);
	free(id);
}

void rc_die(const char *reason)
{
	fprintf(stderr, "%s [error code: %d]\n", reason, errno);
	exit(EXIT_FAILURE);
}

