#ifndef RDMA_MR_H
#define RDMA_MR_H

#include "common.h"
#include "connection.h"

extern struct ibv_mr * mr_regions[MAX_MR];

//memory region state
int mr_all_recv(struct conn_context *ctx);      

//수신
int mr_all_sent(struct conn_context *ctx);      //전송
int mr_all_synced(struct conn_context *ctx);    //동기화

int mr_local_ready(struct conn_context *ctx, int mr_id);
int mr_remote_ready(struct conn_context *ctx, int mr_id);
void mr_register(struct conn_context *ctx, struct mr_context *mrs, int num_mrs, int msg_size);
void mr_remote_update(struct conn_context *ctx, addr_t *addr, addr_t *length, uint32_t *rkey, int mr_count);
void mr_prepare_msg(struct conn_context *ctx, int buffer, int msg_type);
int mr_next_to_sync(struct conn_context *ctx);

uint64_t mr_local_key(int sockfd, int mr_id);
uint64_t mr_local_rkey(int sockfd, int mr_id);
uint64_t mr_local_addr(int sockfd, int mr_id); 
uint64_t mr_remote_key(int sockfd, int mr_id);
uint64_t mr_remote_addr(int sockfd, int mr_id);

int mr_get_sq_idx(int sockfd);
struct ibv_mr * register_wq(int wq_sock, int pd_sock);

#endif
