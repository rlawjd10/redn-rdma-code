#include "messaging.h"
#include "connection.h"
#include "mlx5_intf.h"
#include "verbs.h"

// IBV_WRAPPER_OP_ASYNC, IBV_WRAPPER_OP_SYNC, IBV_WRAPPER_OP_ASYNC_ALL, IBV_WRAPPER_OP_SYNC_ALL
IBV_WRAPPER_FUNC(SEND)                 
IBV_WRAPPER_FUNC(SEND_WITH_IMM)       
IBV_WRAPPER_FUNC(RDMA_READ)
IBV_WRAPPER_FUNC(RDMA_WRITE)
IBV_WRAPPER_FUNC(RDMA_WRITE_WITH_IMM)

/* --- wr id 기반 wqe 반환 -> send_rdma_operation() 호출  --- */
uint32_t IBV_NEXT_WR_ID(int sockfd)
{
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;
	return next_wr_id(ctx, 1);
}

struct ibv_cq * IBV_GET_CQ(int sockfd)
{
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;
	return ctx->cq;
}

struct wqe_ctrl_seg * IBV_FIND_WQE(int sockfd, uint32_t wr_id)  // 특정 wr id의 wqe 반환 
{
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;
	struct wqe_ctrl_seg *seg = NULL;
	debug_print("Find ctrl seg for wr_id: %u\n", wr_id);

	for(int i=0; i<ctx->sq_wqe_cnt; i++) {
		//debug_print("seg #%d has wr_id %lu (required %u)\n", i, ctx->sq_wrid[i], wr_id);
		if(ctx->sq_wrid[i] == wr_id) {
			seg = (struct wqe_ctrl_seg *) get_send_wqe(ctx, i); // i 번쨰 wqe의 시작 주소 반환 
			uint32_t meta = ntohl(seg->opmod_idx_opcode);
			uint8_t opcode = (meta & USHRT_MAX);
			uint16_t idx = ((meta >> 8) & (UINT_MAX));
			
			printf("found wr_id. [seg %d] idx #%d opcode %u [%s]\n", i, idx, opcode,
					stringify_verb(opcode));	
			return seg;
		}
	}

	if(!seg) {
		debug_print("wr_id %u not found\n", wr_id);
	}

	return seg;
}

struct wqe_ctrl_seg * IBV_GET_WQE(int sockfd, uint32_t idx) // 특정 인덱스의 wqe 반환 
{
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;
	struct wqe_ctrl_seg *seg = (struct wqe_ctrl_seg *) get_send_wqe(ctx, idx);

	return seg;
}

/* --- basic post operation 함수를 호출하여 동기/비동기, single node/broadcast 전송 ---*/
// wrapper operation 
uint32_t IBV_WRAPPER_OP_ASYNC(int sockfd, rdma_meta_t *meta,
		int local_id, int remote_id, int opcode)   
{
	uint32_t wr_id = send_rdma_operation(get_connection(sockfd), meta, local_id, remote_id, opcode);
	return wr_id;
}

void IBV_WRAPPER_OP_SYNC(int sockfd, rdma_meta_t *meta, int local_id, int remote_id, int opcode)
{
	uint32_t wr_id = send_rdma_operation(get_connection(sockfd), meta, local_id, remote_id, opcode);
	spin_till_completion(get_connection(sockfd), wr_id);
}

// broadcast operations
//TODO: implement a waiting function for broadcast operations
void IBV_WRAPPER_OP_ASYNC_ALL(rdma_meta_t *meta, int local_id, int remote_id, int opcode)
{
	struct rdma_cm_id *id = NULL;

    // 사용 가능한 모든 RDMA 연결에 대해 연산 수행 
	for(id=find_next_connection(NULL); id!=NULL; id=find_next_connection(id)) {
		send_rdma_operation(id, meta, local_id, remote_id, opcode);
	}
}

void IBV_WRAPPER_OP_SYNC_ALL(rdma_meta_t *meta, int local_id, int remote_id, int opcode)
{
	int i=0;
	struct rdma_cm_id *id = NULL;
	struct rdma_cm_id *conn_ids[rc_connection_count()];
	uint32_t wr_ids[rc_connection_count()];
	
	for(id=find_next_connection(NULL), i=0; id!=NULL; id=find_next_connection(id), i++) {
		wr_ids[i] = send_rdma_operation(id, meta, local_id, remote_id, opcode);
		conn_ids[i] = id;
	}

    // wr 완료까지 대기 
	for(int j=0; j<i; j++) {
		spin_till_completion(conn_ids[j], wr_ids[j]);
	}
}

// send, post operations -> ibv_post_send() 호출 
// 직접 호출하는 API 역할 
uint32_t IBV_SEND_ASYNC(int sockfd, addr_t src, addr_t size, uint32_t imm, int local_id)
{
	int ret = 0;
	struct ibv_send_wr sr;
	struct ibv_send_wr *bad_sr;
	struct ibv_sge sge;

	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;

	struct ibv_mr *local_mr = ctx->local_mr[local_id];

	memset(&sge, 0, sizeof(struct ibv_sge));
	sge.addr = src;
	sge.length = size;
	sge.lkey = local_mr->lkey;

	/* prepare the send work request */
	memset (&sr, 0, sizeof (sr));
	sr.next = NULL;
	//sr.wr_id = next_wr_id(ctx, 1);
	sr.wr_id = 99;
	sr.sg_list = &sge;
	sr.num_sge = 1;
	sr.send_flags = IBV_SEND_SIGNALED;

	if(imm) {
		sr.imm_data = htonl(imm);
		sr.opcode = IBV_WR_SEND_WITH_IMM;
	}
	else
		sr.opcode = IBV_WR_SEND;

	ctx->n_posted_ops++;

	uint32_t sr_id = sr.wr_id;
	debug_print("POST --> SEND (SEND WR %u) [local addr %lx size %lu]\n",
			sr_id, src, size);
	ret = ibv_post_send_wrapper(ctx, ctx->id->qp, &sr, &bad_sr);

	if(ret) {
		printf("ibv_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma operation");
	}

	return sr_id;
}

void IBV_SEND_SYNC(int sockfd, addr_t src, addr_t size, uint32_t imm, int local_id)
{
	uint32_t wr_id = IBV_SEND_ASYNC(sockfd, src, size, imm, local_id);
	spin_till_completion(get_connection(sockfd), wr_id);
}

uint32_t IBV_POST_ASYNC(int sockfd, struct ibv_send_wr *wr) // wr list 처리 
{
	struct ibv_send_wr *bad_wr = NULL;
	struct ibv_send_wr *cur_wr = NULL;
	uint32_t wr_id;

	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;

	cur_wr = wr;

	do {
		
		wr_id = cur_wr->wr_id;
		ctx->n_posted_ops++;
		debug_print("POST --> %s (opcode %u) (SEND WR %u) [local addr %lx remote addr %lx]\n",
			 stringify_verb(cur_wr->opcode), cur_wr->opcode, wr_id, cur_wr->sg_list[0].addr, cur_wr->wr.rdma.remote_addr);
		cur_wr = cur_wr->next;

	} while(cur_wr);

	int ret = ibv_post_send_wrapper(ctx, ctx->id->qp, wr, &bad_wr);


	if(ret) {
		printf("ibv_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma operation");
	}

	return wr_id;
}

void IBV_POST_SYNC(int sockfd, struct ibv_send_wr *wr)
{
	uint32_t wr_id = IBV_POST_ASYNC(sockfd, wr);
	spin_till_completion(get_connection(sockfd), wr_id);
}

// advanced operations
uint32_t IBV_CAS_ASYNC(int sockfd, addr_t src, addr_t dst, addr_t compare, addr_t swap, uint64_t lkey, uint64_t rkey, int fence)
{
	int ret = 0;
	struct ibv_send_wr sr;
	struct ibv_send_wr *bad_sr;
	struct ibv_sge sge;

	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;

	memset(&sge, 0, sizeof(struct ibv_sge));
	sge.addr = src;
	sge.length = sizeof(uint64_t);
	sge.lkey = lkey;

	/* prepare the send work request */
	memset (&sr, 0, sizeof (sr));
	sr.next = NULL;
	sr.wr_id = next_wr_id(ctx, 1);
	sr.sg_list = &sge;
	sr.num_sge = 1;
	sr.wr.atomic.compare_add = compare;
	sr.wr.atomic.swap = swap;
	sr.wr.atomic.remote_addr = dst; 
	sr.wr.atomic.rkey = rkey;
	sr.send_flags = IBV_SEND_SIGNALED;

	if(fence)
		sr.send_flags |= IBV_SEND_FENCE;    // FENCE -> 이전 WR 완료 후 실행 

	sr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;

	ctx->n_posted_ops++;

	uint32_t sr_id = sr.wr_id;
	debug_print("POST --> CAS (SEND WR %u) [local addr %lx remote addr %lx swap %lu]\n",
			sr_id, src, dst, swap);
	ret = ibv_post_send_wrapper(ctx, ctx->id->qp, &sr, &bad_sr);

	if(ret) {
		printf("ibv_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma operation");
	}

	return sr_id;
}

void IBV_CAS_SYNC(int sockfd, addr_t src, addr_t dst, addr_t compare, addr_t swap, uint64_t lkey, uint64_t rkey, int fence)
{
	uint32_t wr_id = IBV_CAS_ASYNC(sockfd, src, dst, compare, swap, lkey, rkey, fence);
	spin_till_completion(get_connection(sockfd), wr_id);
}

uint32_t IBV_FETCH_ADD_ASYNC(int sockfd, addr_t src, addr_t dst, addr_t size, addr_t cmp, uint64_t lkey, uint64_t rkey)
{
	int ret = 0;
	struct ibv_send_wr sr;
	struct ibv_send_wr *bad_sr;
	struct ibv_sge sge;

	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;
	
	memset(&sge, 0, sizeof(struct ibv_sge));
	sge.addr = src;
	sge.length = size;
	sge.lkey = lkey;

	/* prepare the send work request */
	memset (&sr, 0, sizeof (sr));
	sr.next = NULL;
	sr.wr_id = next_wr_id(ctx, 1);
	sr.sg_list = &sge;
	sr.num_sge = 1;

	sr.wr.atomic.compare_add = cmp;
	sr.wr.atomic.swap = 0;
	sr.wr.atomic.remote_addr = dst; 
	sr.wr.atomic.rkey = rkey;
	sr.send_flags = IBV_SEND_SIGNALED;
	sr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;

	//XXX testing only
	sr.send_flags |= IBV_SEND_FENCE;

	ctx->n_posted_ops++;

	uint32_t sr_id = sr.wr_id;
	debug_print("POST --> ADD (SEND WR %u) [local addr %lx remote addr %lx cmp %lu]\n",
			sr_id, src, dst, cmp);
	ret = ibv_post_send_wrapper(ctx, ctx->id->qp, &sr, &bad_sr);

	if(ret) {
		printf("ibv_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma operation");
	}

	return sr_id;
}

void IBV_FETCH_ADD_SYNC(int sockfd, addr_t src, addr_t dst, addr_t size, addr_t cmp, uint64_t lkey, uint64_t rkey)
{
	uint32_t wr_id = IBV_FETCH_ADD_ASYNC(sockfd, src, dst, size, cmp, lkey, rkey);
	spin_till_completion(get_connection(sockfd), wr_id);
}

uint32_t IBV_CONVERT_ENDIAN_ASYNC(int sockfd, addr_t src, addr_t dst, addr_t size, uint64_t lkey, uint64_t rkey)
{
	int ret = 0;
	struct ibv_send_wr sr;
	struct ibv_send_wr *bad_sr;

	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;

	if(size > 16)
		rc_die("endian conversion failed; max number of sge allowed is 16");

	struct ibv_sge sge[size];
	memset(sge, 0, sizeof(struct ibv_sge) * size);

    // 바이트 순서 변경을 위해 sge 설정 
	for(int i=0; i<size; i++) {
		sge[i].addr = src + size - 1 - i;
		sge[i].length = 1;
		sge[i].lkey = lkey;
	}

	/* prepare the send work request */
	memset (&sr, 0, sizeof (sr));
	sr.next = NULL;
	sr.opcode = IBV_WR_RDMA_READ;
	sr.wr_id = next_wr_id(ctx, 1);
	sr.sg_list = sge;
	sr.num_sge = size;
	sr.wr.rdma.rkey = rkey;
	sr.wr.rdma.remote_addr = dst;
	sr.send_flags = IBV_SEND_SIGNALED;

	ctx->n_posted_ops++;

	uint32_t sr_id = sr.wr_id;
	debug_print("POST --> READ (SEND WR %u)\n", sr_id);
	ret = ibv_post_send_wrapper(ctx, ctx->id->qp, &sr, &bad_sr);

	if(ret) {
		printf("ibv_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma operation");
	}

	return sr_id;
}

void IBV_CONVERT_ENDIAN_SYNC(int sockfd, addr_t src, addr_t dst, addr_t size, uint64_t lkey, uint64_t rkey)
{
	uint32_t wr_id = IBV_CONVERT_ENDIAN_ASYNC(sockfd, src, dst, size, lkey, rkey);
	spin_till_completion(get_connection(sockfd), wr_id);
}

uint32_t IBV_NOOP_ASYNC(int sockfd, int signaled)   // no-operation (QP 상태 유지를 위함)
{
	int ret = 0;
	struct ibv_send_wr sr;
	struct ibv_send_wr *bad_sr;
	struct ibv_sge sge;

	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;

	memset(&sge, 0, sizeof(struct ibv_sge));

	/* prepare the send work request */
	memset (&sr, 0, sizeof (sr));
	sr.next = NULL;
	sr.wr_id = next_wr_id(ctx, 1);
	sr.sg_list = &sge;
	sr.num_sge = 0;
	if(signaled)
		sr.send_flags = IBV_SEND_SIGNALED;
	sr.opcode = 0;  // no operation 

	ctx->n_posted_ops++;

	uint32_t sr_id = sr.wr_id;
	debug_print("POST --> NOOP (SEND WR %u)\n", sr_id);
	ret = ibv_post_send_wrapper(ctx, ctx->id->qp, &sr, &bad_sr);

	if(ret) {
		printf("ibv_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma operation");
	}

	return sr_id;
}

void IBV_NOOP_SYNC(int sockfd, int signaled)
{
	if(!signaled)
		rc_die("cannot synchronously execute unsignaled verb");
	uint32_t wr_id = IBV_NOOP_ASYNC(sockfd, signaled);
	spin_till_completion(get_connection(sockfd), wr_id);
}

// receive operations 
void IBV_RECEIVE_IMM(int sockfd)    // immediate data 수신 
{
	struct rdma_cm_id *id = get_connection(sockfd);	// scokfd를 통해 bitmap에서 id 찾기 
	receive_imm(id, 0);
}

void IBV_RECEIVE(int sockfd, addr_t addr, addr_t size, int local_id)    // single SGE
{
	struct rdma_cm_id *id = get_connection(sockfd);
	struct conn_context *ctx = (struct conn_context *)id->context;

	struct ibv_recv_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(wr));

	struct ibv_mr *local_mr = ctx->local_mr[local_id];

	wr.wr_id = 99;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	sge.addr = (uintptr_t) addr;
	sge.length = size;
	sge.lkey = mr_local_key(sockfd, local_id);


	debug_print("POST --> RECV (WR #%lu) [send_fd: %d, addr %lx, len %u, qp_num %u]\n",
			wr.wr_id, ctx->sockfd, sge.addr, sge.length, id->qp->qp_num);

	//ctx->n_posted_ops++;

	ibv_post_recv(id->qp, &wr, &bad_wr);
}

void IBV_RECEIVE_ANY(int sockfd, addr_t addr, addr_t size, uint64_t lkey)
{
	struct rdma_cm_id *id = get_connection(sockfd);
	struct conn_context *ctx = (struct conn_context *)id->context;

	struct ibv_recv_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(wr));

	wr.wr_id = 99;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	sge.addr = (uintptr_t) addr;
	sge.length = size;
	sge.lkey = lkey;

	debug_print("POST --> RECV (WR #%lu) [send_fd: %d, addr %lx, len %u, qp_num %u]\n",
			wr.wr_id, ctx->sockfd, sge.addr, sge.length, id->qp->qp_num);

	//ctx->n_posted_ops++;

	ibv_post_recv(id->qp, &wr, &bad_wr);
}

void IBV_RECEIVE_SG(int sockfd, rdma_meta_t *meta, uint64_t lkey)   // multi-SGE 
{
	struct rdma_cm_id *id = get_connection(sockfd);
	struct conn_context *ctx = (struct conn_context *)id->context;

	struct ibv_recv_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(wr));

	wr.wr_id = 99;
	wr.sg_list = meta->sge_entries;
	wr.num_sge = meta->sge_count;   // sge 개수 

	if(wr.num_sge == 0 || wr.num_sge > 16)
		rc_die("invalid number of sge entries for receive");

	for(int i=0; i<wr.num_sge; i++) {   // 각 SGE의 lkey 설정 
		wr.sg_list[i].lkey = lkey; 
	}

	debug_print("POST --> RECV (WR #%lu) [send_fd: %d, qp_num %u]\n",
			wr.wr_id, ctx->sockfd, id->qp->qp_num);

	for(int i=0; i<wr.num_sge; i++)
		debug_print("----------- sge%d [addr %lx, length %u]\n", i, wr.sg_list[i].addr, wr.sg_list[i].length);

	//ctx->n_posted_ops++;

	ibv_post_recv(id->qp, &wr, &bad_wr);
}

// two-sided rdma (rpc???? -> send messages & recv)
void IBV_WRAPPER_SEND_MSG_SYNC(int sockfd, int buffer_id, int solicit)
{
	struct rdma_cm_id *id = get_connection(sockfd);
	struct conn_context *ctx = (struct conn_context *)id->context;

	if(solicit && !ctx->msg_send[buffer_id]->meta.app.id)
		rc_die("app_id must be greater than 0 to match request to response");

	debug_print("sending synchronous message on buffer[%d] with (RPC #%u)\n",
			buffer_id, ctx->msg_send[buffer_id]->meta.app.id);

	if(solicit) {
		//register_pending(id, ctx->msg_send[buffer_id]->meta.app.id);
	}

	ctx->msg_send[buffer_id]->id = MSG_CUSTOM;

	spin_till_completion(id, send_message(id, buffer_id));
}

uint32_t IBV_WRAPPER_SEND_MSG_ASYNC(int sockfd, int buffer_id, int solicit)
{
	struct rdma_cm_id *id = get_connection(sockfd);
	struct conn_context *ctx = (struct conn_context *)id->context;

	debug_print("sending asynchronous message on buffer[%d] - [RPC #%u] [Data: %s]\n",
			buffer_id, ctx->msg_send[buffer_id]->meta.app.id,
			ctx->msg_send[buffer_id]->meta.app.data);

	ctx->msg_send[buffer_id]->id = MSG_CUSTOM;

	uint32_t wr_id = send_message(id, buffer_id);

	if(solicit) {
		//register_pending(id, ctx->msg_send[buffer_id]->meta.app.id);
	}

	return wr_id;
}

void IBV_RECEIVE_MSG(int sockfd, int buffer)
{
	struct rdma_cm_id *id = get_connection(sockfd);
	receive_message(id, buffer);
}

// pending messages - rpc 요청에 대한 응답 대기 (hash table 이용)
/* Add an entry for a pending msg response

   This entry is used by waiting threads to check if a response for an rpc has been received
   Note: applications using this must avoid duplicates of app_id per connection
*/
void register_pending(struct rdma_cm_id *id, uint32_t app_id)
{
	struct conn_context *ctx = (struct conn_context *)id->context;
	struct app_response *p = (struct app_response *) calloc(1, sizeof(struct app_response));

	debug_print("awaiting response for (RPC #%u) [qp_num %u\n", app_id, id->qp->qp_num);
	p->id = app_id;
	HASH_ADD(hh, ctx->pendings, id, sizeof(p->id), p);
}

/* Remove pending rpc response entry

   Called by thread waiting for rpc response
*/
void remove_pending(struct rdma_cm_id *id, struct app_response *p)
{
	struct conn_context *ctx = (struct conn_context *)id->context;
	/*
	struct app_response *p;

	debug_print("[APP] deleting response hook for id #%u\n", app_id);
	HASH_FIND_INT(ctx->pendings, app_id, p);
	*/

	debug_print("response received for (RPC #%u) [qp_num %u]\n", p->id, id->qp->qp_num);
	if(p)
		HASH_DEL(ctx->pendings, p);
	else
		rc_die("failed to remove pending app response; undefined behavior");
}

void update_pending(struct rdma_cm_id *id, uint32_t app_id) // 응답 수신 시 last_rcv_compl 업데이트 
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	ctx->last_rcv_compl = app_id;

	debug_print("Received response with seqn %u [qp_num %u]\n",
			app_id, id->qp->qp_num);
}

//------Request await functions------
/* Wait till response is received for an RPC
   
   Note: app_id is application-defined and both send & rcv messages
   must share the same app_id for this to work (which is set by user)
*/
// busy-waiting
void IBV_AWAIT_RESPONSE(int sockfd, uint32_t app_id)
{
	struct rdma_cm_id *id = get_connection(sockfd);

	debug_print("wait till response seqn = %u\n", app_id);
	spin_till_response(id, app_id);	// busy-waiting
	debug_print("wait ending. received response with seqn = %u\n", app_id);
}

// uses rdma notify mechanism
void IBV_AWAIT_RESPONSE_NOTIFY(int sockfd, uint32_t app_id)
{
	struct rdma_cm_id *id = get_connection(sockfd);

	debug_print("wait till response seqn = %u\n", app_id);
	block_till_response(id, app_id);
	debug_print("wait ending. received response with seqn = %u\n", app_id);
}

// busy-waiting
void IBV_AWAIT_WORK_COMPLETION(int sockfd, uint32_t wr_id)
{
	struct rdma_cm_id *id = get_connection(sockfd);

	spin_till_completion(id, wr_id);
}

// uses rdma notify mechanism
void IBV_AWAIT_WORK_COMPLETION_NOTIFY(int sockfd, uint32_t wr_id)
{
	struct rdma_cm_id *id = get_connection(sockfd);

	block_till_completion(id, wr_id);
}

void IBV_AWAIT_PENDING_WORK_COMPLETIONS(int sockfd)
{
	struct rdma_cm_id *id = get_connection(sockfd);
	struct conn_context *ctx = (struct conn_context *)id->context;

	spin_till_completion(id, ctx->last_send);
}

/* --- basic post operations --- */
// wrapper 함수에서 사용하는 low-level operations 
__attribute__((visibility ("hidden"))) 
uint32_t send_rdma_operation(struct rdma_cm_id *id, rdma_meta_t *meta, int local_id, int remote_id, int opcode)
{
	int timeout = 5; //set bootstrap timeout to 5 sec
	struct mr_context *remote_mr = NULL;
	struct ibv_mr *local_mr = NULL;
	int one_sided = op_one_sided(opcode);
	struct conn_context *ctx = (struct conn_context *)id->context;
	rdma_meta_t *next_meta = NULL;
	struct ibv_send_wr *wr_head = NULL;
	struct ibv_send_wr *wr = NULL;
	struct ibv_send_wr *bad_wr = NULL;
	uint32_t last_wr_id;
	int opcount = 0;
	int ret;

	if(local_id > MAX_MR || remote_id > MAX_MR)
		rc_die("invalid memory regions specified");

    // 메모리 준비 여부 확인 
	while(!mr_local_ready(ctx, local_id) || (one_sided && !mr_remote_ready(ctx, remote_id))) {
		if(timeout == 0)
			rc_die("failed to issue sync; no metadata available for remote mr\n");
		debug_print("keys haven't yet been received; sleeping for 1 sec...\n");
		timeout--;
		sleep(1);
	}

	local_mr = ctx->local_mr[local_id];

	if(one_sided)
		remote_mr = ctx->remote_mr[remote_id];

	pthread_mutex_lock(&ctx->wr_lock);
	//pthread_spin_lock(&ctx->post_lock);
	do {	
		opcount++;

		if(wr) {
			wr->next = (struct ibv_send_wr*) malloc(sizeof(struct ibv_send_wr));
			wr = wr->next;
		}
		else {
			wr = (struct ibv_send_wr*) malloc(sizeof(struct ibv_send_wr));
			wr_head = wr;
		}

		memset(wr, 0, sizeof(struct ibv_send_wr));

		for(int i=0; i<meta->sge_count; i++) {
			meta->sge_entries[i].lkey = local_mr->lkey;
		}

		if(one_sided) {
			wr->wr.rdma.remote_addr = meta->addr;
			wr->wr.rdma.rkey = remote_mr->rkey;
		}

        // WQE 생성 및 설정 
		wr->wr_id = next_wr_id(ctx, 1);
		wr->opcode = opcode;
		wr->send_flags = IBV_SEND_SIGNALED;
		wr->num_sge = meta->sge_count;

		if(wr->num_sge)
			wr->sg_list = meta->sge_entries;
		else
			wr->sg_list = NULL;

		if(opcode == IBV_WR_RDMA_WRITE_WITH_IMM || opcode == IBV_WR_SEND_WITH_IMM)
			wr->imm_data = htonl(meta->imm);

		/*
		debug_print("%s (SEND WR #%lu) [opcode %d, remote addr %lx, len %lu, qp_num %u]\n",
				stringify_verb(opcode), wr->wr_id, opcode, meta->addr, meta->length, id->qp->qp_num);

		for(int i=0; i<wr->num_sge; i++)
			debug_print("----------- sge%d [addr %lx, length %u]\n", i, wr->sg_list[i].addr, wr->sg_list[i].length);
		*/

		meta = meta->next;

		last_wr_id = wr->wr_id;

		ctx->n_posted_ops++;

	} while(meta); //loop to batch rdma operations

    // ibv_post_send 호출
	debug_print("POST --> %s (SEND WR %lu) [send_fd: %d batch_size: %d]\n", stringify_verb(opcode), wr_head->wr_id, ctx->sockfd, opcount);
	ret = ibv_post_send_wrapper(ctx, id->qp, wr_head, &bad_wr);

	pthread_mutex_unlock(&ctx->wr_lock);
	//pthread_spin_unlock(&ctx->post_lock);

	if(ret) {
		printf("ibv_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma operation");
	}
	
	while(wr_head) {
		wr = wr_head;
		wr_head = wr_head->next;
		free(wr);
	}

	return last_wr_id;
}

__attribute__((visibility ("hidden"))) 
uint32_t send_message(struct rdma_cm_id *id, int buffer)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	struct ibv_send_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;
	int ret;

	memset(&wr, 0, sizeof(wr));

	//wr.wr_id = (uintptr_t)id;
	wr.wr_id = next_wr_id(ctx, 2);
	wr.opcode = IBV_WR_SEND;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;

	sge.addr = (uintptr_t)ctx->msg_send[buffer];
	sge.length = sizeof(*ctx->msg_send[buffer]) + sizeof(char)*msg_size;
	sge.lkey = ctx->msg_send_mr[buffer]->lkey;

    // ibv_post_send 호출 
	if(rc_bind_buffer(id, buffer, wr.wr_id)) {
		debug_print("POST --> (SEND WR #%lu) [addr %lx, len %u, qp_num %u]\n",
			wr.wr_id, sge.addr, sge.length, id->qp->qp_num);

		ret = ibv_post_send_wrapper(ctx, id->qp, &wr, &bad_wr);
		if(ret) {
			printf("ibv_post_send_wrapper: errno = %d\n", ret);
			rc_die("failed to post rdma operation");
		}
	}
	else
		rc_die("failed to bind send buffer");

	ctx->n_posted_ops++;
	
	return wr.wr_id;
}

__attribute__((visibility ("hidden"))) 
void receive_message(struct rdma_cm_id *id, int buffer)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	struct ibv_recv_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(wr));

	wr.wr_id = buffer;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	sge.addr = (uintptr_t)ctx->msg_rcv[buffer];
	sge.length = sizeof(*ctx->msg_rcv[buffer]) + sizeof(char)*msg_size;
	sge.lkey = ctx->msg_rcv_mr[buffer]->lkey;

	debug_print("POST --> RECV (WR #%lu) [send_fd: %d, addr %lx, len %u, qp_num %u]\n",
			wr.wr_id, ctx->sockfd, sge.addr, sge.length, id->qp->qp_num);

	//ctx->n_posted_ops++;

	ibv_post_recv(id->qp, &wr, &bad_wr);
}

__attribute__((visibility ("hidden"))) 
void receive_imm(struct rdma_cm_id *id, int buffer)	// two-sided rdma 
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	struct ibv_recv_wr wr, *bad_wr = NULL;

	memset(&wr, 0, sizeof(wr));

	wr.wr_id = buffer;
	wr.sg_list = NULL;	// immediate 수신에서는 SGE가 필요없어서 NULL로 설정
	wr.num_sge = 0;

	//debug_print("POST --> RECV-IMM (WR #%lu) [send_fd %d]\n", wr.wr_id, ctx->sockfd);

	//ctx->n_posted_ops++;

	ibv_post_recv(id->qp, &wr, &bad_wr);
}

#ifdef EXP_VERBS
uint32_t IBV_CALC_OP_ASYNC(int sockfd, rdma_meta_t *meta, int local_id, int remote_id, int opcode)
{
	int ret = 0;
	struct ibv_exp_send_wr sr;
	struct ibv_exp_send_wr *bad_sr;
	struct ibv_sge *sge;

	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;

	struct ibv_mr *local_mr = ctx->local_mr[local_id];
	struct mr_context *remote_mr = ctx->remote_mr[remote_id];


	for(int i=0; i<meta->sge_count; i++) {
		// FIXME: no idea why the length has to be 16
		//meta->sge_entries[i].length = sizeof(uint64_t) + 8;
		meta->sge_entries[i].lkey = local_mr->lkey;
	}



	/* prepare the send work request */
	memset (&sr, 0, sizeof (sr));
	sr.next = NULL;
	sr.wr_id = next_wr_id(ctx, 1);
	sr.exp_opcode = IBV_EXP_WR_RDMA_WRITE;
	//sr.ex.imm_data = NULL;

	//sr.exp_opcode = IBV_EXP_WR_SEND;

	//sr.exp_opcode = IBV_EXP_WR_RDMA_WRITE_WITH_IMM;
	//sr.ex.imm_data = 10;

	sr.sg_list = meta->sge_entries;
	sr.num_sge = meta->sge_count;
	sr.wr.rdma.remote_addr = meta->addr;
	sr.wr.rdma.rkey = remote_mr->rkey;
	sr.exp_send_flags = IBV_EXP_SEND_SIGNALED;
#if 1
	sr.exp_send_flags |= IBV_EXP_SEND_WITH_CALC;

	sr.op.calc.calc_op   = opcode;
	sr.op.calc.data_type = IBV_EXP_CALC_DATA_TYPE_UINT;
	sr.op.calc.data_size = IBV_EXP_CALC_DATA_SIZE_64_BIT;
#endif
	ctx->n_posted_ops++;

	uint32_t sr_id = sr.wr_id;
	debug_print("POST --> CALC (SEND WR %u) [calc_op %d remote_addr %lx qp_num %u]\n",
			sr_id, sr.op.calc.calc_op, meta->addr, ctx->id->qp->qp_num);
	for(int i=0; i<sr.num_sge; i++)
		debug_print("----------- sge%d [addr %lx, length %u]\n", i, sr.sg_list[i].addr, sr.sg_list[i].length);

	ret = ibv_exp_post_send_wrapper(ctx, ctx->id->qp, &sr, &bad_sr);

	if(ret) {
		printf("ibv_exp_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma operation");
	}

	return sr_id;
}

void IBV_CALC_OP_SYNC(int sockfd, rdma_meta_t *meta, int local_id, int remote_id, int opcode)
{
	uint32_t wr_id = IBV_CALC_OP_ASYNC(sockfd, meta, local_id, remote_id, opcode);
	spin_till_completion(get_connection(sockfd), wr_id);
}

uint32_t IBV_WAIT(int msockfd, int sockfd)
{
	struct conn_context *mctx = (struct conn_context *)get_connection(msockfd)->context;
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;	

	struct ibv_exp_send_wr *bad_wr = NULL;
	struct ibv_exp_send_wr *wr = (struct ibv_exp_send_wr*) malloc(sizeof(struct ibv_exp_send_wr));
	memset(wr, 0, sizeof(struct ibv_exp_send_wr));

	/* SEND_EN (QP, beforecount) */
	wr->wr_id = next_wr_id(mctx, 1);
	wr->next = NULL;
	wr->sg_list = NULL;
	wr->num_sge = 0;
	wr->exp_opcode = IBV_EXP_WR_CQE_WAIT;
	//wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;// | IBV_EXP_SEND_SIGNALED;
	wr->ex.imm_data = 0;
	wr->task.cqe_wait.cq = ctx->cq;

#if 1
	wr->task.cqe_wait.cq_count = ctx->n_posted_ops;
#else
	
	wr->task.cqe_wait.cq_count = ctx->n_posted_ops;
	//wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;
	
	//wr->task.cqe_wait.cq_count = ctx->n_posted_ops;
	//wr->exp_send_flags = IBV_EXP_SEND_SIGNALED;
	ctx->n_posted_ops++;
	
#endif


	debug_print("POST --> WAIT (SEND WR #%lu) [send_fd:%d wait_fd:%d wait_idx:%d]\n",
			wr[0].wr_id, msockfd, sockfd, wr->task.cqe_wait.cq_count);

	uint32_t wr_id = wr->wr_id;
	int ret = ibv_exp_post_send_wrapper(mctx, mctx->id->qp, wr, &bad_wr);

	if(ret) {
		printf("ibv_exp_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma wr");
	}

	return wr_id;
}


uint32_t IBV_WAIT_TILL(int msockfd, int sockfd, uint32_t count)
{
	struct conn_context *mctx = (struct conn_context *)get_connection(msockfd)->context;
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;	

	struct ibv_exp_send_wr *bad_wr = NULL;
	struct ibv_exp_send_wr *wr = (struct ibv_exp_send_wr*) malloc(sizeof(struct ibv_exp_send_wr));
	memset(wr, 0, sizeof(struct ibv_exp_send_wr));

	/* SEND_EN (QP, beforecount) */
	wr->wr_id = next_wr_id(mctx, 1);
	wr->next = NULL;
	wr->sg_list = NULL;
	wr->num_sge = 0;
	wr->exp_opcode = IBV_EXP_WR_CQE_WAIT;
	//wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;// | IBV_EXP_SEND_SIGNALED;
	wr->ex.imm_data = 0;
	wr->task.cqe_wait.cq = ctx->cq;

#if 0
	wr->task.cqe_wait.cq_count = ctx->n_posted_ops;
#else
	wr->task.cqe_wait.cq_count = ctx->n_posted_ops + count;
	wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;
#endif


	debug_print("POST --> WAIT (SEND WR #%lu) [send_fd:%d wait_fd:%d wait_idx:%d]\n",
			wr[0].wr_id, msockfd, sockfd, wr->task.cqe_wait.cq_count);

	uint32_t wr_id = wr->wr_id;
	int ret = ibv_exp_post_send_wrapper(mctx, mctx->id->qp, wr, &bad_wr);

	if(ret) {
		printf("ibv_exp_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma wr");
	}

	return wr_id;
}

uint32_t IBV_WAIT_EXPLICIT(int msockfd, int sockfd, uint32_t count)
{
	struct conn_context *mctx = (struct conn_context *)get_connection(msockfd)->context;
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;	

	struct ibv_exp_send_wr *bad_wr = NULL;
	struct ibv_exp_send_wr *wr = (struct ibv_exp_send_wr*) malloc(sizeof(struct ibv_exp_send_wr));
	memset(wr, 0, sizeof(struct ibv_exp_send_wr));

	/* SEND_EN (QP, beforecount) */
	wr->wr_id = next_wr_id(mctx, 1);
	wr->next = NULL;
	wr->sg_list = NULL;
	wr->num_sge = 0;
	wr->exp_opcode = IBV_EXP_WR_CQE_WAIT;
#if 0
	wr->exp_send_flags = IBV_EXP_SEND_WAIT_EXPLICIT;
#endif
	//wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;// | IBV_EXP_SEND_SIGNALED;
	wr->ex.imm_data = 0;
	wr->task.cqe_wait.cq = ctx->cq;

#if 0
	wr->task.cqe_wait.cq_count = ctx->n_posted_ops;
#else
	wr->task.cqe_wait.cq_count = count;
	wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;
#endif


	debug_print("POST --> WAIT (SEND WR #%lu) [send_fd:%d wait_fd:%d wait_idx:%d]\n",
			wr[0].wr_id, msockfd, sockfd, wr->task.cqe_wait.cq_count);

	uint32_t wr_id = wr->wr_id;

	int ret = ibv_exp_post_send_wrapper(mctx, mctx->id->qp, wr, &bad_wr);

	if(ret) {
		printf("ibv_exp_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma wr");
	}

	return wr_id;
}

struct ibv_exp_send_wr * ibv_create_exp_wait_wr(int sendQ, int listenQ, int n_wait, int last)
{
	struct conn_context *sctx = (struct conn_context *)get_connection(sendQ)->context;
	struct conn_context *lctx = (struct conn_context *)get_connection(listenQ)->context;	

	struct ibv_exp_send_wr *bad_wr = NULL;
	struct ibv_exp_send_wr *wr = (struct ibv_exp_send_wr*) malloc(sizeof(struct ibv_exp_send_wr));
	memset(wr, 0, sizeof(struct ibv_exp_send_wr));

	wr->wr_id = next_wr_id(sctx, 1);
	wr->next = NULL;
	wr->sg_list = NULL;
	wr->num_sge = 0;
	wr->exp_opcode = IBV_EXP_WR_CQE_WAIT;
	//wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;// | IBV_EXP_SEND_SIGNALED;
	if(last)
		wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;

	wr->ex.imm_data = 0;
	wr->task.cqe_wait.cq = lctx->cq;
	if(n_wait)
		wr->task.cqe_wait.cq_count = n_wait;
	else
		wr->task.cqe_wait.cq_count = lctx->n_posted_ops;
	
	return wr;
}

struct ibv_exp_send_wr * ibv_create_exp_send_wr(int sendQ, int opcode, int local, int remote, addr_t size)
{
	struct conn_context *sctx = (struct conn_context *)get_connection(sendQ)->context;

	struct ibv_exp_send_wr *bad_wr = NULL;
	struct ibv_exp_send_wr *wr = (struct ibv_exp_send_wr*) malloc(sizeof(struct ibv_exp_send_wr));
	memset(wr, 0, sizeof(struct ibv_exp_send_wr));

	struct ibv_sge sge;
	memset(&sge, 0, sizeof(struct ibv_sge));
	sge.addr = (uintptr_t) mr_local_addr(sendQ, local);
	sge.length = size;
	sge.lkey = mr_local_key(sendQ, local);

	wr->wr_id = next_wr_id(sctx, 1);
	wr->next = NULL;
	wr->sg_list = &sge;
	wr->num_sge = 1;
	wr->exp_opcode = opcode;
	wr->wr.rdma.remote_addr = (uintptr_t) mr_remote_addr(sendQ, remote);
	wr->wr.rdma.rkey = mr_remote_key(sendQ, remote);
	wr->ex.imm_data = 0;
	wr->exp_send_flags = IBV_EXP_SEND_SIGNALED;

	return wr;
}

uint32_t IBV_TRIGGER(int msockfd, int sockfd, int count)
{
#if 0
	struct conn_context *mctx = (struct conn_context *)get_connection(msockfd)->context;
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;	

	struct ibv_exp_task task[1];
	struct ibv_exp_task *task_bad;
	struct ibv_exp_send_wr wr[1];

	memset(task, 0, sizeof(*task) * 1);
	memset(wr, 0, sizeof(*wr) * 1);

	task[0].task_type = IBV_EXP_TASK_SEND;
	task[0].item.qp = mctx->id->qp;
	task[0].item.send_wr = wr;

	task[0].next = NULL;

	/* SEND_EN (QP, beforecount) */
	wr[0].wr_id = next_wr_id(mctx, 1);
	wr[0].next = NULL;
	wr[0].sg_list = NULL;
	wr[0].num_sge = 0;
	wr[0].exp_opcode = IBV_EXP_WR_SEND_ENABLE;
	wr[0].exp_send_flags = IBV_EXP_SEND_SIGNALED | IBV_EXP_SEND_WAIT_EN_LAST;
	wr[0].ex.imm_data = 0;

	wr[0].task.wqe_enable.qp = ctx->id->qp;
	wr[0].task.wqe_enable.wqe_count = count;

	debug_print("POST --> TASK(EN WR#%lu) [master = %d] [worker = %d]\n", wr[0].wr_id, msockfd, sockfd);

	int ret = ibv_exp_post_task(rc_get_context(ctx->devid), task, &task_bad);

	if(ret) {
		printf("ibv_exp_post_task: errno = %d\n", ret);
		rc_die("failed to post rdma task");
	}
#else

	struct conn_context *mctx = (struct conn_context *)get_connection(msockfd)->context;
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;	

	struct ibv_exp_send_wr *bad_wr = NULL;
	struct ibv_exp_send_wr *wr = (struct ibv_exp_send_wr*) malloc(sizeof(struct ibv_exp_send_wr));
	memset(wr, 0, sizeof(struct ibv_exp_send_wr));

	/* SEND_EN (QP, beforecount) */
	wr->wr_id = next_wr_id(mctx, 1);
	wr->next = NULL;
	wr->sg_list = NULL;
	wr->num_sge = 0;
	wr->exp_opcode = IBV_EXP_WR_SEND_ENABLE;
	//wr->exp_send_flags = IBV_EXP_SEND_SIGNALED | IBV_EXP_SEND_WAIT_EN_LAST;
	wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;
	//wr->exp_send_flags = IBV_EXP_SEND_WAIT_EXPLICIT;
	wr->ex.imm_data = 0;

	wr->task.wqe_enable.qp = ctx->id->qp;
	wr->task.wqe_enable.wqe_count = count;

	debug_print("POST --> SEND_ENABLE(WR#%lu) [master = %d] [worker = %d]\n", wr[0].wr_id, msockfd, sockfd);

	uint32_t wr_id = wr->wr_id;

	int ret = ibv_exp_post_send_wrapper(mctx, mctx->id->qp, wr, &bad_wr);

	if(ret) {
		printf("ibv_exp_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma wr");
	}

	return wr_id;
#endif

}

uint32_t IBV_NEXT_WR_IDX(int sockfd, int pos)
{
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;
	return get_wr_idx(ctx, pos);
}

uint32_t IBV_TRIGGER_EXPLICIT(int msockfd, int sockfd, int count)
{
	struct conn_context *mctx = (struct conn_context *)get_connection(msockfd)->context;
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;	

	struct ibv_exp_send_wr *bad_wr = NULL;
	struct ibv_exp_send_wr *wr = (struct ibv_exp_send_wr*) malloc(sizeof(struct ibv_exp_send_wr));
	memset(wr, 0, sizeof(struct ibv_exp_send_wr));

	/* SEND_EN (QP, beforecount) */
	wr->wr_id = next_wr_id(mctx, 1);
	wr->next = NULL;
	wr->sg_list = NULL;
	wr->num_sge = 0;
	wr->exp_opcode = IBV_EXP_WR_SEND_ENABLE;
	//wr->exp_send_flags = IBV_EXP_SEND_SIGNALED | IBV_EXP_SEND_WAIT_EN_LAST;
	//wr->exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;
#ifdef MODDED_DRIVER
	wr->exp_send_flags = IBV_EXP_SEND_WAIT_EXPLICIT;
#endif
	wr->ex.imm_data = 0;

	wr->task.wqe_enable.qp = ctx->id->qp;

#ifdef MODDED_DRIVER
	wr->task.wqe_enable.wqe_count = count;
#else
	wr->task.wqe_enable.wqe_count = 0;
#endif

	debug_print("POST --> SEND_ENABLE(WR#%lu) [master = %d] [worker = %d] [idx = %d]\n", wr[0].wr_id, msockfd, sockfd, count);

	uint32_t wr_id = wr->wr_id;

	int ret = ibv_exp_post_send_wrapper(mctx, mctx->id->qp, wr, &bad_wr);

	if(ret) {
		printf("ibv_exp_post_send_wrapper: errno = %d\n", ret);
		rc_die("failed to post rdma wr");
	}

#ifndef MODDED_DRIVER
	struct wqe_ctrl_seg *sr_ctrl = IBV_FIND_WQE(msockfd, wr_id);
	void *seg = ((void*)sr_ctrl) + sizeof(struct mlx5_wqe_ctrl_seg);

	struct wqe_wait_en_seg *sr_en_wait = (struct wqe_wait_en_seg *) seg;
	sr_en_wait->pi = htonl(count);
#endif

	return wr_id;
}

void IBV_SET_TRIGGER(int msockfd, int sockfd, int nops, int nwait)
{

	struct conn_context *mctx = (struct conn_context *)get_connection(msockfd)->context;
	struct conn_context *ctx = (struct conn_context *)get_connection(sockfd)->context;	

	struct ibv_exp_task task[1];
	struct ibv_exp_task *task_bad;

	int n = 2 * nops - 1;
	struct ibv_exp_send_wr wr[n];

	memset(task, 0, sizeof(*task) * 1);
	memset(wr, 0, sizeof(*wr) * n);

	task[0].task_type = IBV_EXP_TASK_SEND;
	task[0].item.qp = mctx->id->qp;
	task[0].item.send_wr = wr;

	task[0].next = NULL;

	/* SEND_EN (QP, beforecount) */
	wr[0].wr_id = next_wr_id(mctx, 1);
	wr[0].next = n > 1 ? &wr[1] : NULL;
	//debug_print("Check next: %p\n", wr[0].next);
	wr[0].sg_list = NULL;
	wr[0].num_sge = 0;
	wr[0].exp_opcode = IBV_EXP_WR_SEND_ENABLE;
	//wr[0].exp_send_flags = IBV_EXP_SEND_SIGNALED;
	//wr[0].exp_send_flags = (n == 1 ? IBV_EXP_SEND_WAIT_EN_LAST : 0);
	wr[0].exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;
	wr[0].ex.imm_data = 0;

	wr[0].task.wqe_enable.qp = ctx->id->qp;
	wr[0].task.wqe_enable.wqe_count = nwait;

	for(int i=1; i<n; i+=2)
	{
		/* WAIT (QP, 1) */
		wr[i].wr_id	= next_wr_id(mctx, 1);
		wr[i].next = &wr[i+1];
		wr[i].sg_list    = NULL;
		wr[i].num_sge    = 0;
		wr[i].exp_opcode = IBV_EXP_WR_CQE_WAIT;
		//wr[i].exp_send_flags = IBV_EXP_SEND_SIGNALED;
		//wr[i].exp_send_flags = (i == n-2 ? IBV_EXP_SEND_WAIT_EN_LAST : 0);
		wr[i].exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;
		wr[i].ex.imm_data = 0;
		wr[i].task.cqe_wait.cq = ctx->cq;
		wr[i].task.cqe_wait.cq_count = nwait;

		/* SEND_EN (QP, aftercount) */
		wr[i+1].wr_id = next_wr_id(mctx, 1);
		wr[i+1].next = i < n-2 ? &wr[i+2] : NULL;
		wr[i+1].sg_list = NULL;
		wr[i+1].num_sge = 0;
		wr[i+1].exp_opcode = IBV_EXP_WR_SEND_ENABLE;
		//wr[i+1].exp_send_flags = IBV_EXP_SEND_SIGNALED;
		//wr[i+1].exp_send_flags = (i == n-2 ? IBV_EXP_SEND_WAIT_EN_LAST : 0);
		wr[i+1].exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;
		wr[i+1].ex.imm_data = 0;

		wr[i+1].task.wqe_enable.qp = ctx->id->qp;
		wr[i+1].task.wqe_enable.wqe_count = nwait;
	}

#ifdef DEBUG
	debug_print("POST --> TASK (COUNT = %d) [master = %d] [worker = %d]\n",
			n, msockfd, sockfd);
	for(int i=0; i<n; i++) {
		addr_t n_wr_id = wr[i].next != NULL ? wr[i].next->wr_id : 0;
		if(wr[i].exp_opcode == IBV_EXP_WR_CQE_WAIT)
			debug_print("------------ WAIT (WR# %lu) [flags: %lu next: %lu]\n",
					wr[i].wr_id, wr[i].exp_send_flags, n_wr_id);
		else
			debug_print("------------ SEND_ENABLE (WR# %lu) [flags: %lu next: %lu]\n",
					wr[i].wr_id, wr[i].exp_send_flags, n_wr_id);
	}
#endif

	int ret = ibv_exp_post_task(rc_get_context(ctx->devid), task, &task_bad);

	if(ret) {
		printf("ibv_exp_post_task: errno = %d\n", ret);
		rc_die("failed to post rdma task");
	}		
}

#endif
