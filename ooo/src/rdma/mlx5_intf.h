#ifndef MLX5_WRAPPER_INTF_H
#define MLX5_WRAPPER_INTF_H

#include <string.h>
#include <stdint.h>
#include <mlx5dv.h>
#include "common.h"
#include "globals.h"
#include "utils.h"
#include "doorbell.h"

// qp.c
// 주소 정렬 확인.
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

#ifdef __cplusplus
extern "C" {
#endif


enum {
	MLX5_WRAPPER_INLINE_SEG	= 0x80000000,	// MSB (Most Significant Bit), inline data segment
};

// opcode - Mellanox WR
enum {
	MLX5_WRAPPER_OPCODE_NOP			= 0x00,     //no operation (0)
	MLX5_WRAPPER_OPCODE_SEND_INVAL		= 0x01, // send invalidation (1)
	MLX5_WRAPPER_OPCODE_RDMA_WRITE		= 0x08, // write (8)
	MLX5_WRAPPER_OPCODE_RDMA_WRITE_IMM	= 0x09, // write with immediate (9)
	MLX5_WRAPPER_OPCODE_SEND		= 0x0a,     // send (10)
	MLX5_WRAPPER_OPCODE_SEND_IMM		= 0x0b, // send with immediate (11)
	MLX5_WRAPPER_OPCODE_TSO			= 0x0e,     // TCP segmentation offload (14)
	MLX5_WRAPPER_OPCODE_RDMA_READ		= 0x10, // read (16)
	MLX5_WRAPPER_OPCODE_ATOMIC_CS		= 0x11, // compare and swap (17)
	MLX5_WRAPPER_OPCODE_ATOMIC_FA		= 0x12, // fetch and add (18)
	MLX5_WRAPPER_OPCODE_ATOMIC_MASKED_CS	= 0x14, // masked compare and swap (20)
	MLX5_WRAPPER_OPCODE_ATOMIC_MASKED_FA	= 0x15, // masked fetch and add (21)
	MLX5_WRAPPER_OPCODE_FMR			= 0x19,     // Fast Memory Registration (25)
	MLX5_WRAPPER_OPCODE_LOCAL_INVAL		= 0x1b, //local invalidataion (27)
	MLX5_WRAPPER_OPCODE_CONFIG_CMD		= 0x1f, // configuration command (31)
	MLX5_WRAPPER_OPCODE_UMR			= 0x25,     // update memory region (37)

	/* mlx5dv.h 에서 다루는 opcode에서 추가 */
	MLX5_WRAPPER_OPCODE_SEND_ENABLE		= 0x17, // 23
	MLX5_WRAPPER_OPCODE_RECV_ENABLE		= 0x16, // 22
	MLX5_WRAPPER_OPCODE_CQE_WAIT		= 0x0f, // 15
	MLX5_WRAPPER_OPCODE_TAG_MATCHING	= 0x28, // 40
};

// opcode 확장 flag
enum {
	MLX5_WRAPPER_OPCODE_BASIC	= 0x00010000,	//기본 WRAP 명령어.	
	MLX5_WRAPPER_OPCODE_MANAGED	= 0x00020000,	// 관리형 명령어.
	MLX5_WRAPPER_OPCODE_WITH_IMM	= 0x01000000,	// immediate data 포함.
	MLX5_WRAPPER_OPCODE_EXT_ATOMICS = 0x08,	// 확장된 원자 명령어.
};

// infiniband WR ipcode를 mellono WR opcode로 변환
#define MLX5_WRAPPER_IB_OPCODE(op, class, attr)     (((class) & 0x00FF0000) | ((attr) & 0xFF000000) | ((op) & 0x0000FFFF))
#define MLX5_WRAPPER_IB_OPCODE_GET_CLASS(opcode)    ((opcode) & 0x00FF0000)
#define MLX5_WRAPPER_IB_OPCODE_GET_OP(opcode)       ((opcode) & 0x0000FFFF)	// opcode값에서 하위 16bits만 추출출
#define MLX5_WRAPPER_IB_OPCODE_GET_ATTR(opcode)     ((opcode) & 0xFF000000)


static const uint32_t mlx5_ib_opcode[] = {
	[IBV_EXP_WR_SEND]                       = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_SEND,                MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_SEND_WITH_IMM]              = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_SEND_IMM,            MLX5_WRAPPER_OPCODE_BASIC, MLX5_WRAPPER_OPCODE_WITH_IMM),
	[IBV_EXP_WR_SEND_WITH_INV]		= MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_SEND_INVAL,          MLX5_WRAPPER_OPCODE_BASIC, MLX5_WRAPPER_OPCODE_WITH_IMM),
	[IBV_EXP_WR_RDMA_WRITE]                 = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_RDMA_WRITE,          MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_RDMA_WRITE_WITH_IMM]        = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_RDMA_WRITE_IMM,      MLX5_WRAPPER_OPCODE_BASIC, MLX5_WRAPPER_OPCODE_WITH_IMM),
	[IBV_EXP_WR_RDMA_READ]                  = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_RDMA_READ,           MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_ATOMIC_CMP_AND_SWP]         = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_ATOMIC_CS,           MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_ATOMIC_FETCH_AND_ADD]       = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_ATOMIC_FA,           MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_EXT_MASKED_ATOMIC_CMP_AND_SWP]   = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_ATOMIC_MASKED_CS,  MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_EXT_MASKED_ATOMIC_FETCH_AND_ADD] = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_ATOMIC_MASKED_FA,  MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_SEND_ENABLE]                = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_SEND_ENABLE,         MLX5_WRAPPER_OPCODE_MANAGED, 0),
	[IBV_EXP_WR_RECV_ENABLE]                = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_RECV_ENABLE,         MLX5_WRAPPER_OPCODE_MANAGED, 0),
	[IBV_EXP_WR_CQE_WAIT]                   = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_CQE_WAIT,            MLX5_WRAPPER_OPCODE_MANAGED, 0),
	[IBV_EXP_WR_NOP]			= MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_NOP,		  MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_UMR_FILL]			= MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_UMR,		  MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_UMR_INVALIDATE]             = MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_UMR,                 MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_TSO]			= MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_TSO,                 MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_BIND_MW]			= MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_UMR,                 MLX5_WRAPPER_OPCODE_BASIC, 0),
	[IBV_EXP_WR_LOCAL_INV]			= MLX5_WRAPPER_IB_OPCODE(MLX5_WRAPPER_OPCODE_UMR,                 MLX5_WRAPPER_OPCODE_BASIC, 0),
};

/* mlx5_intf.c */
// qp의 control 정보 테이블 초기화
void mlx5_build_ctrl_metadata(struct conn_context *ctx);

// send wqe를 생성한다.
int mlx5_post_send(struct ibv_exp_send_wr *wr, struct conn_context *ctx,
				   struct ibv_exp_send_wr **bad_wr, int is_exp_wr);

// send wqe에 wr을 삽입한다.
int __mlx5_post_send(struct ibv_exp_send_wr *wr,
				      struct conn_context *ctx, uint64_t exp_send_flags, void *seg, int *total_size);

int ibv_post_send_wrapper(struct conn_context *ctx, struct ibv_qp *qp, struct ibv_send_wr *wr,
		                         struct ibv_send_wr **bad_wr);

int ibv_exp_post_send_wrapper(struct conn_context *ctx, struct ibv_qp *qp, struct ibv_exp_send_wr *wr,
		struct ibv_exp_send_wr **bad_wr);

int update_scur_post(struct ibv_exp_send_wr *wr, struct conn_context *ctx);

/* inline fuction */
// send queue에서 특정 WQE 주소 가져오기.
static inline void *get_send_wqe(struct conn_context *ctx, int n)
{
	// 특정 인덱스 n의 WQE 메모리 주소 반환 (SQ 시작 주소 ~ WQE 크기만큼 주소 이동) -> n번째 WQE 시작 주소 
	return ctx->sq_start + (n << SEND_WQE_SHIFT); // wqe의 크기에 따라 shift
}

// queue 크기에 맞게 wr id를 변환하여 wqe 인덱스 구하기.
// static inline uint32_t get_wr_idx(struct conn_context *ctx, int pos)
// {
// 	uint32_t idx = pos & (ctx->iqp->sq.wqe_cnt - 1);
// 	return idx;
// }

// immediate 데이터가 있는 경우 해당 값 반환 (write, send -> metadata 함께 전달 가능능)
static inline uint32_t send_ieth(struct ibv_exp_send_wr *wr)
{
	return MLX5_WRAPPER_IB_OPCODE_GET_ATTR(mlx5_ib_opcode[wr->exp_opcode]) &
			MLX5_WRAPPER_OPCODE_WITH_IMM ?
				wr->ex.imm_data : 0;
}
// data point segment 설정.
static inline int set_data_ptr_seg(struct mlx5_wqe_data_seg *dseg, struct ibv_sge *sg,
			    int offset)
{

	dseg->byte_count = htonl(sg->length - offset);	//전송할 데이터 크기 설정.
	dseg->lkey       = htonl(sg->lkey);				// RNIC이 접근할 메모리 키 설정.
	dseg->addr       = htonll(sg->addr + offset);	// 전송을 위한 시작 주소 설정.

	return 0;
}

// inline data segment에 데이터 삽입
static inline int set_data_inl_seg(struct conn_context *ctx, int num_sge, struct ibv_sge *sg_list,
		     void *wqe, int *sz, int idx, int offset)
{
	struct mlx5_wqe_inline_seg *seg;
	void *addr;
	int len;
	int i;
	int inl = 0;
	void *qend = ctx->sq_end;
	int copy;

	seg = wqe;
	wqe += sizeof *seg;

	for (i = idx; i < num_sge; ++i) {
		// SGE 리스트에서 데이터 정보 가져오기.
		addr = (void *) (unsigned long)(sg_list[i].addr + offset);
		len  = sg_list[i].length - offset;
		inl += len;
		offset = 0;

		if (inl > IBV_INLINE_THRESHOLD) {	//128byte 이상인 경우 inline data 사용 불가.
			debug_print("inline failed. %u < threshold = %u\n", inl, IBV_INLINE_THRESHOLD);
			return ENOMEM;
		}

		// wqe에 데이터 복사 후 wqe 끝에 도달하면 다음 wqe 할당.
		if (wqe + len > qend) {
			copy = qend - wqe;
			memcpy(wqe, addr, copy);
			addr += copy;
			len -= copy;
			wqe = get_send_wqe(ctx, 0);
		}
		memcpy(wqe, addr, len);
		wqe += len;
	}

	// inline data segment에 데이터 크기 설정.
	if (inl) {
		seg->byte_count = htonl(inl | MLX5_WRAPPER_INLINE_SEG);
		*sz += ALIGN(inl + sizeof(seg->byte_count), 16) / 16;
	}

	return 0;
}

// non-inline은 inline과 다르게 데이터를 복사하는 게 아니라 실제 메모리 주소를 참조.
static inline int set_data_non_inl_seg(struct conn_context *ctx, int num_sge, struct ibv_sge *sg_list,
			 void *wqe, int *sz, int idx, int offset)
{
	struct mlx5_wqe_data_seg *dpseg = wqe;
	struct ibv_sge *psge;
	int i;
	
	for (i = idx; i < num_sge; ++i) {	// 여러 개의 SGE를 하나의 WQE에 추가가
		if (dpseg == ctx->sq_end)	
			dpseg = get_send_wqe(ctx, 0);

		if (sg_list[i].length) {	
			psge = sg_list + i;
			// wqe에 데이터 정보 설정.
			if (set_data_ptr_seg(dpseg, psge, offset)) {	// 메모리 주소만 포함되고 데이터는 복사하지 않음.
				debug_print("failed allocating memory for implicit lkey structure%s", "\n");
				return ENOMEM;
			}
			++dpseg;
			offset = 0;
			*sz += sizeof(struct mlx5_wqe_data_seg) / 16;
		}
	}

	return 0;
}

// atomic segment 설정.
static inline int set_data_atom_seg(struct conn_context *ctx, int num_sge, struct ibv_sge *sg_list,
			     void *wqe, int *sz, int atom_arg)
{
	struct mlx5_wqe_data_seg *dpseg = wqe;
	struct ibv_sge *psge;
	struct ibv_sge sge;
	int i;

	for (i = 0; i < num_sge; ++i) {
		if (dpseg == ctx->sq_end)
			dpseg = get_send_wqe(ctx, 0);

		// wqe에 데이터 정보 설정.
		if (sg_list[i].length) {
			sge = sg_list[i];
			sge.length = atom_arg;
			psge = &sge;
			if (set_data_ptr_seg(dpseg, psge, 0)) {
				debug_print("failed allocating memory for implicit lkey structure%s", "\n");
				return ENOMEM;
			}
			++dpseg;
			*sz += sizeof(struct mlx5_wqe_data_seg) / 16;
		}
	}

	return 0;
}

// atomic operation - control segment 설정. 기본 rdma와 달리 원격 메모리 값을 변경하는 것도 추가됨
static inline void set_atomic_seg(struct mlx5_wqe_atomic_seg *aseg,
			   enum ibv_wr_opcode   opcode,
			   uint64_t swap,
			   uint64_t compare_add)
{
	if (opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
		aseg->swap_add = htonll(swap); 		// 호스트 바이트 순서를 네트워크 바이트 순서로 변환.
		aseg->compare  = htonll(compare_add);
	} else {
		aseg->swap_add = htonll(compare_add);
		aseg->compare  = 0;
	}
}

static inline void set_ctrl_seg(uint32_t *start, uint32_t qp_num,
				uint8_t opcode, uint16_t idx, uint8_t opmod,
				uint8_t size, uint8_t fm_ce_se, uint32_t imm_invk_umrk)
{
	*start++ = htonl(opmod << 24 | idx << 8 | opcode);
	*start++ = htonl(qp_num << 8 | (size & 0x3F));
	*start++ = htonl(fm_ce_se);
	*start = imm_invk_umrk;
}

static inline void set_ctrl_seg_sig(uint32_t *start, uint32_t qp_num,
				    uint8_t opcode, uint16_t idx, uint8_t opmod,
				    uint8_t size, uint8_t fm_ce_se, uint32_t imm_invk_umrk)
{
	set_ctrl_seg(start, qp_num, opcode, idx, opmod, size, fm_ce_se, imm_invk_umrk);

	//XXX disable wq signatures
	//if (unlikely(ctrl_seg->wq_sig))
	//	*(start + 2) = htonl(~calc_xor(start, size << 4) << 24 | fm_ce_se);
}

static inline void set_raddr_seg(struct mlx5_wqe_raddr_seg *rseg,
				 uint64_t remote_addr, uint32_t rkey)
{
	rseg->raddr    = htonll(remote_addr);
	rseg->rkey     = htonl(rkey);
	rseg->reserved = 0;
}

static inline void set_wait_en_seg(void *wqe_seg, uint32_t obj_num, uint32_t count)
{
	struct mlx5_wqe_wait_en_seg *seg = (struct mlx5_wqe_wait_en_seg *)wqe_seg;

	seg->pi      = htonl(count); // PI (Poll Index) - Polling Index -> 현재 WQE의 인덱스
	seg->obj_num = htonl(obj_num);

	return;
}

static inline int set_data_seg(struct conn_context *ctx, void *seg, int *sz, int is_inl,
		 int num_sge, struct ibv_sge *sg_list, int atom_arg,
		 int idx, int offset)
{
	if (is_inl)
		return set_data_inl_seg(ctx, num_sge, sg_list, seg, sz, idx,
					offset);
	if (atom_arg)
		return set_data_atom_seg(ctx, num_sge, sg_list, seg, sz, atom_arg);

	return set_data_non_inl_seg(ctx, num_sge, sg_list, seg, sz, idx, offset);
}

#ifdef __cplusplus
}
#endif

#endif
