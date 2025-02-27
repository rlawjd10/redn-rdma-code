
#include "mlx5_intf.h"


__attribute__((visibility ("hidden"))) 
void mlx5_build_ctrl_metadata(struct conn_context *ctx)
{
	uint8_t *tbl = ctx->fm_ce_se_tbl;   //wqe control metadata table
	uint8_t *acc = ctx->fm_ce_se_acc;   //additional metadata table
	int i;
    
    // cq update, solicited, fence 설정.
    // IBV_SEND_SOLICITED = 1, IBV_SEND_SIGNALED = 2, IBV_SEND_FENCE = 4
	tbl[0		       | 0		   | 0]		     = (0			| 0			  | 0);
	tbl[0		       | 0		   | IBV_SEND_FENCE] = (0			| 0			  | MLX5_WQE_CTRL_FENCE);
	tbl[0		       | IBV_SEND_SIGNALED | 0]		     = (0			| MLX5_WQE_CTRL_CQ_UPDATE | 0);
	tbl[0		       | IBV_SEND_SIGNALED | IBV_SEND_FENCE] = (0			| MLX5_WQE_CTRL_CQ_UPDATE | MLX5_WQE_CTRL_FENCE);
	tbl[IBV_SEND_SOLICITED | 0		   | 0]		     = (MLX5_WQE_CTRL_SOLICITED | 0			  | 0);
	tbl[IBV_SEND_SOLICITED | 0		   | IBV_SEND_FENCE] = (MLX5_WQE_CTRL_SOLICITED | 0			  | MLX5_WQE_CTRL_FENCE);
	tbl[IBV_SEND_SOLICITED | IBV_SEND_SIGNALED | 0]		     = (MLX5_WQE_CTRL_SOLICITED | MLX5_WQE_CTRL_CQ_UPDATE | 0);
	tbl[IBV_SEND_SOLICITED | IBV_SEND_SIGNALED | IBV_SEND_FENCE] = (MLX5_WQE_CTRL_SOLICITED | MLX5_WQE_CTRL_CQ_UPDATE | MLX5_WQE_CTRL_FENCE);
    // set the signal bits
	for (i = 0; i < 8; i++)
		tbl[i] = ctx->sq_signal_bits | tbl[i];

    // set the additional metadata
	memset(acc, 0, sizeof(ctx->fm_ce_se_acc));
	acc[0			       | 0			   | 0]			     = (0			| 0			  | 0);
	acc[0			       | 0			   | IBV_EXP_QP_BURST_FENCE] = (0			| 0			  | MLX5_WQE_CTRL_FENCE);
	acc[0			       | IBV_EXP_QP_BURST_SIGNALED | 0]			     = (0			| MLX5_WQE_CTRL_CQ_UPDATE | 0);
	acc[0			       | IBV_EXP_QP_BURST_SIGNALED | IBV_EXP_QP_BURST_FENCE] = (0			| MLX5_WQE_CTRL_CQ_UPDATE | MLX5_WQE_CTRL_FENCE);
	acc[IBV_EXP_QP_BURST_SOLICITED | 0			   | 0]			     = (MLX5_WQE_CTRL_SOLICITED | 0			  | 0);
	acc[IBV_EXP_QP_BURST_SOLICITED | 0			   | IBV_EXP_QP_BURST_FENCE] = (MLX5_WQE_CTRL_SOLICITED | 0			  | MLX5_WQE_CTRL_FENCE);
	acc[IBV_EXP_QP_BURST_SOLICITED | IBV_EXP_QP_BURST_SIGNALED | 0]			     = (MLX5_WQE_CTRL_SOLICITED | MLX5_WQE_CTRL_CQ_UPDATE | 0);
	acc[IBV_EXP_QP_BURST_SOLICITED | IBV_EXP_QP_BURST_SIGNALED | IBV_EXP_QP_BURST_FENCE] = (MLX5_WQE_CTRL_SOLICITED | MLX5_WQE_CTRL_CQ_UPDATE | MLX5_WQE_CTRL_FENCE);
	for (i = 0; i < 32; i++)
		acc[i] = ctx->sq_signal_bits | acc[i];
}

// 현재 wqe 위치 계산 -> 확장 wr 확인 & SGE 개수 검사 -> wqe 생성 -> wqe에 wr id 설정 -> 큐 크기 업데이트 (다음 wqe 위치 결정) -> doorbell 메커니즘을 사용하여 NIC에 전송
__attribute__((visibility ("hidden"))) 
int mlx5_post_send(struct ibv_exp_send_wr *wr, struct conn_context *ctx,
				   struct ibv_exp_send_wr **bad_wr, int is_exp_wr)
{
    // wqe를 생성할 위치 계산.
	void *seg;  // wqe 시작 주소.
	void *wqe2ring;
	int nreq;
	int err = 0;
	int size;
	unsigned idx;
	uint64_t exp_send_flags;

    //XXX check if the queue is full
	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		idx = ctx->scur_post & (ctx->iqp->sq.wqe_cnt - 1);
		seg = get_send_wqe(ctx, idx);   // 계산된 idx에 해당하는 wqe 주소 가져오기.

        // wr이 exp_wr인지 확인.
		exp_send_flags = is_exp_wr ? wr->exp_send_flags : ((struct ibv_send_wr *)wr)->send_flags;

		//XXX statically set SGE size
		if (wr->num_sge > 16) { // 16개 이상의 SGE를 가지는 경우 에러 처리.
			debug_print("max gs exceeded %d (max = %d)\n",
				 wr->num_sge, 16);
			errno = ENOMEM;
			err = errno;
			*bad_wr = wr;
			goto out;
		}

        // __mlx5_post_send()를 호출하여 WQE 생성.
		err = __mlx5_post_send(wr, ctx, exp_send_flags, seg, &size);
		if (err) {
			errno = err;
			*bad_wr = wr;
			goto out;
		}

		ctx->sq_wrid[idx] = wr->wr_id;  // wqe에 wr id 설정. -> completion 시 wr id를 통해 해당 wr을 찾을 수 있음.
        // 현재 WQE에 대한 data segment 크기 계산. -> 큐 크기를 업데이트.
		ctx->scur_post += DIV_ROUND_UP(size * 16, MLX5_SEND_WQE_BB);
		wqe2ring = seg;
	}
out:
	if (nreq) {

#if 1
		if (ctx->flags & IBV_EXP_QP_CREATE_MANAGED_SEND) {
			wmb();  // memory barrier (메모리 명령어 순서 보장)
			goto post_send_no_db;
		}
#else
		qp->sq.head += nreq;

		if (qp->gen_data.create_flags
					& CREATE_FLAG_NO_DOORBELL) {
			/* Controlled or peer-direct qp */
			wmb();
			if (qp->peer_enabled) {
				qp->peer_ctrl_seg = wqe2ring;
				qp->peer_seg_size += (size + 3) / 4;
			}
			goto post_send_no_db;
		}
#endif

		//XXX check which doorbell method to use. for now, putting MLX5_DB_METHOD_DB
        // doorbell 메커니즘을 사용하여 NIC에 전송. -> nic에게 wqe 전송하여 새로운 wqe 추가 알림.
		__ring_db(ctx, MLX5_DB_METHOD_DB, ctx->scur_post & 0xffff, wqe2ring, (size + 3) / 4);
	}

post_send_no_db:

	return err;
}

// low-level function to create WQE
__attribute__((visibility ("hidden"))) 
int __mlx5_post_send(struct ibv_exp_send_wr *wr,
				      struct conn_context *ctx, uint64_t exp_send_flags, void *seg, int *total_size)
{   
    // wqe 기본 변수 설정.
	struct mlx5_klm_buf *klm;
	void *ctrl = seg;   // control segment 시작 주소.
	struct ibv_qp *ibqp = ctx->id->qp;
	int err = 0;
	int size = 0;       // wqe 크기.
	uint8_t opmod = 0;
	void *qend = ctx->sq_end;
	uint32_t mlx5_opcode;   // mlx5 opcode
	struct mlx5_wqe_xrc_seg *xrc;
	int tmp = 0;
	int num_sge = wr->num_sge;
	uint8_t next_fence = 0;
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	int xlat_size;
	struct mlx5_mkey_seg *mk;
	int wqe_sz;
	uint64_t reglen;
	int atom_arg = 0;
	uint8_t fm_ce_se;
	uint32_t imm;

    // infiniband opcode를 mlx5 opcode로 변환.
	mlx5_opcode = MLX5_WRAPPER_IB_OPCODE_GET_OP(mlx5_ib_opcode[wr->exp_opcode]);
	imm = send_ieth(wr);    // immediate data 설정.

    // control segment 설정.
	seg += sizeof(struct mlx5_wqe_ctrl_seg);
	size = sizeof(struct mlx5_wqe_ctrl_seg) / 16;

	switch (wr->exp_opcode) {
    // RDMA READ/WRITE
	case IBV_EXP_WR_RDMA_READ:
	case IBV_EXP_WR_RDMA_WRITE:
	case IBV_EXP_WR_RDMA_WRITE_WITH_IMM:
        // set remote address segment
		set_raddr_seg(seg, wr->wr.rdma.remote_addr, wr->wr.rdma.rkey);
		seg  += sizeof(struct mlx5_wqe_raddr_seg);
		size += sizeof(struct mlx5_wqe_raddr_seg) / 16;
		break;

    // ATOMIC
	case IBV_EXP_WR_ATOMIC_CMP_AND_SWP:
	case IBV_EXP_WR_ATOMIC_FETCH_AND_ADD:

		set_raddr_seg(seg, wr->wr.atomic.remote_addr,
			      wr->wr.atomic.rkey);
		seg  += sizeof(struct mlx5_wqe_raddr_seg);

		set_atomic_seg(seg, wr->exp_opcode, wr->wr.atomic.swap,
			       wr->wr.atomic.compare_add);
		seg  += sizeof(struct mlx5_wqe_atomic_seg);

		size += (sizeof(struct mlx5_wqe_raddr_seg) +
		sizeof(struct mlx5_wqe_atomic_seg)) / 16;
		atom_arg = 8;
		break;
    
    // SEND
	case IBV_EXP_WR_SEND:
		break;

    // CQE WAIT
	case IBV_EXP_WR_CQE_WAIT:
		{
#if 1
			// set wait index to value provided by usr
			uint32_t wait_index = wr->task.cqe_wait.cq_count;
#else
			uint32_t wait_index = 0;
			struct mlx5_cq *wait_cq = to_mcq(wr->task.cqe_wait.cq);

			wait_index = wait_cq->wait_index +
					wr->task.cqe_wait.cq_count;
			wait_cq->wait_count = max(wait_cq->wait_count,
					wr->task.cqe_wait.cq_count);

			if (exp_send_flags & IBV_EXP_SEND_WAIT_EN_LAST) {
				wait_cq->wait_index += wait_cq->wait_count;
				wait_cq->wait_count = 0;
			}
#endif

			set_wait_en_seg(seg, ctx->icq->cqn, wait_index);
			seg   += sizeof(struct mlx5_wqe_wait_en_seg);
			size += sizeof(struct mlx5_wqe_wait_en_seg) / 16;
		}
		break;

	case IBV_EXP_WR_SEND_ENABLE:
	case IBV_EXP_WR_RECV_ENABLE:
		{
			unsigned head_en_index;
#if 1
			/*
			 * Posting work request for QP that does not support
			 * SEND/RECV ENABLE makes performance worse.
			 */
			//XXX should check target queue instead
			if (((wr->exp_opcode == IBV_EXP_WR_SEND_ENABLE) &&
				!(ctx->flags & IBV_EXP_QP_CREATE_MANAGED_SEND)) ||
				((wr->exp_opcode == IBV_EXP_WR_RECV_ENABLE) &&
				!(ctx->flags & IBV_EXP_QP_CREATE_MANAGED_RECV))) {
				return EINVAL;
			}

#else
			struct mlx5_wq *wq;
			struct mlx5_wq_recv_send_enable *wq_en;


			/*
			 * Posting work request for QP that does not support
			 * SEND/RECV ENABLE makes performance worse.
			 */
			if (((wr->exp_opcode == IBV_EXP_WR_SEND_ENABLE) &&
				!(to_mqp(wr->task.wqe_enable.qp)->gen_data.create_flags &
					IBV_EXP_QP_CREATE_MANAGED_SEND)) ||
				((wr->exp_opcode == IBV_EXP_WR_RECV_ENABLE) &&
				!(to_mqp(wr->task.wqe_enable.qp)->gen_data.create_flags &
					IBV_EXP_QP_CREATE_MANAGED_RECV))) {
				return EINVAL;
			}
#endif

#if 1
			// set enable index to value provided by usr
			head_en_index = wr->task.wqe_enable.wqe_count;
#else
			wq = (wr->exp_opcode == IBV_EXP_WR_SEND_ENABLE) ?
				&to_mqp(wr->task.wqe_enable.qp)->sq :
				&to_mqp(wr->task.wqe_enable.qp)->rq;

			wq_en = (wr->exp_opcode == IBV_EXP_WR_SEND_ENABLE) ?
				 &to_mqp(wr->task.wqe_enable.qp)->sq_enable :
				 &to_mqp(wr->task.wqe_enable.qp)->rq_enable;

			/* If wqe_count is 0 release all WRs from queue */
			if (wr->task.wqe_enable.wqe_count) {
				head_en_index = wq_en->head_en_index +
							wr->task.wqe_enable.wqe_count;
				wq_en->head_en_count = max(wq_en->head_en_count,
							   wr->task.wqe_enable.wqe_count);

				if ((int)(wq->head - head_en_index) < 0)
					return EINVAL;
			} else {
				head_en_index = wq->head;
				wq_en->head_en_count = wq->head - wq_en->head_en_index;
			}

			if (exp_send_flags & IBV_EXP_SEND_WAIT_EN_LAST) {
				wq_en->head_en_index += wq_en->head_en_count;
				wq_en->head_en_count = 0;
			}
#endif
			set_wait_en_seg(seg,
					wr->task.wqe_enable.qp->qp_num,
					head_en_index);

			seg += sizeof(struct mlx5_wqe_wait_en_seg);
			size += sizeof(struct mlx5_wqe_wait_en_seg) / 16;
		}
		break;
	
	case IBV_EXP_WR_NOP:
		break;
	default:
		break;
	}

	err = set_data_seg(ctx, seg, &size, !!(exp_send_flags & IBV_EXP_SEND_INLINE),
			   num_sge, wr->sg_list, atom_arg, 0, 0);
	if (err)
		return err;

    // control segment에 opcode 설정.
	fm_ce_se = ctx->fm_ce_se_tbl[exp_send_flags & (IBV_SEND_SOLICITED | IBV_SEND_SIGNALED | IBV_SEND_FENCE)];

	//XXX find value of scur_post
	uint16_t scur_post = 0;
	set_ctrl_seg_sig(ctrl, ctx->id->qp->qp_num,
			 mlx5_opcode, scur_post, opmod, size,
			 fm_ce_se, imm);
	
	//qp->gen_data.fm_cache = next_fence;
	*total_size = size;

	return 0;
}

// SEND WQE를 생성하여 NIC에 전송. (즉, ibv_post_send() 호출하는 래퍼 함수)
int ibv_post_send_wrapper(struct conn_context *ctx, struct ibv_qp *qp, struct ibv_send_wr *wr,
		                         struct ibv_send_wr **bad_wr)
{
#ifndef MODDED_DRIVER
	update_scur_post((struct ibv_exp_send_wr *)wr, ctx);    // WQE 크기 및 현재 queue 상태 최신으로 업데이트.
#endif
	return ibv_post_send(qp, wr, bad_wr);
}

int ibv_exp_post_send_wrapper(struct conn_context *ctx, struct ibv_qp *qp, struct ibv_exp_send_wr *wr,
		struct ibv_exp_send_wr **bad_wr)
{
#ifndef MODDED_DRIVER
	update_scur_post(wr, ctx);
#endif
	return ibv_exp_post_send(qp, wr, bad_wr);
}


/* WQE 크기 및 현재 queue 상태 업데이트 */
__attribute__((visibility ("hidden"))) 
int update_scur_post(struct ibv_exp_send_wr *wr, struct conn_context *ctx)
{

#ifdef IBV_WRAPPER_INLINE
	printf("inlining not supported\n");
	exit(EXIT_FAILURE);
#endif

	uint32_t idx = ctx->scur_post & (ctx->iqp->sq.wqe_cnt - 1);
	
	ctx->sq_wrid[idx] = wr->wr_id;

	int size = sizeof(struct mlx5_wqe_ctrl_seg) / 16;

	switch (wr->exp_opcode) {
	case IBV_EXP_WR_RDMA_READ:
	case IBV_EXP_WR_RDMA_WRITE:
	case IBV_EXP_WR_RDMA_WRITE_WITH_IMM:
		size += sizeof(struct mlx5_wqe_raddr_seg) / 16;
		break;

	case IBV_EXP_WR_ATOMIC_CMP_AND_SWP:
	case IBV_EXP_WR_ATOMIC_FETCH_AND_ADD:
		size += (sizeof(struct mlx5_wqe_raddr_seg) + sizeof(struct mlx5_wqe_atomic_seg)) / 16;
		break;

	case IBV_EXP_WR_SEND:
		break;

	case IBV_EXP_WR_CQE_WAIT:
		{
			size += sizeof(struct mlx5_wqe_wait_en_seg) / 16;
		}
		break;

	case IBV_EXP_WR_SEND_ENABLE:
	case IBV_EXP_WR_RECV_ENABLE:
		{
			size += sizeof(struct mlx5_wqe_wait_en_seg) / 16;
		}
		break;
	
	case IBV_EXP_WR_NOP:
		break;
	default:
		break;
	}

    // 현재 WQE에 대한 data segment 크기 계산. -> 큐 크기를 업데이트.
	for(int i=0; i<wr->num_sge; i++) {
		if (wr->sg_list[i].length > 0) {
			size += sizeof(struct mlx5_wqe_data_seg) / 16;
		}
	}

    // 현재 큐 상태(scru_post) 업데이트하여 다음 WQE를 준비.
	printf("updating scur_post %u by %d (original size %d)\n", ctx->scur_post, DIV_ROUND_UP(size * 16, MLX5_SEND_WQE_BB), size);
	ctx->scur_post += DIV_ROUND_UP(size * 16, MLX5_SEND_WQE_BB);

	return 0;
}


