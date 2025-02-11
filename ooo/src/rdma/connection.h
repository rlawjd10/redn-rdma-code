#ifndef RDMA_CONNECTION_H
#define RDMA_CONNECTION_H

#include <pthread.h>
#include <netdb.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mlx5dv.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#ifdef EXP_VERBS
#include <infiniband/verbs_exp.h>
#endif

#include "uthash.h"
#include "messaging.h"  // rc_ide() 때문에
#include "utils.h"

extern const int TIMEOUT_IN_MS;
extern const char *DEFAULT_PORT;

extern struct rdma_event_channel *ec;
extern int num_mrs;
extern struct mr_context *mrs;
extern int msg_size;
extern int cq_loop;
// pthread_mutex_init 구조체 - dynammic 초기화 (원하는 값으로 초기화)
extern pthread_mutexattr_t attr;
extern pthread_mutex_t cq_lock;

extern int exit_rc_loop;

// agent callbacks
typedef void(*pre_conn_cb_fn)(struct rdma_cm_id *id);
typedef void(*connect_cb_fn)(struct rdma_cm_id *id);
typedef void(*completion_cb_fn)(struct ibv_wc *wc);
typedef void(*disconnect_cb_fn)(struct rdma_cm_id *id);

// user callbacks
typedef void(*app_conn_cb_fn)(int sockfd);
typedef void(*app_disc_cb_fn)(int sockfd);
typedef void(*app_recv_cb_fn)(struct app_context *msg);

enum rc_connection_state
{
	RC_CONNECTION_TERMINATED = -1,
	RC_CONNECTION_PENDING,
	RC_CONNECTION_ACTIVE,
	RC_CONNECTION_READY,

};

//rdma_cm_id hashmap value (defined below in 'context')
// rdma 연결 정보를 해시맵으로 저장해서 빠르게 조회 가능
struct id_record {
	struct sockaddr_in addr;
	uint32_t qp_num;
	struct rdma_cm_id *id;
	UT_hash_handle addr_hh; // ip
	UT_hash_handle qp_hh;   // qp number
};

struct buffer_record {	// 버퍼 정보 구조체
	uint32_t wr_id;
	int buff_id;
	UT_hash_handle hh;
};

static inline uint32_t last_compl_wr_id(struct conn_context *ctx, int send)
{
	//we maintain seperate wr_ids for send/rcv queues since
	//there is no ordering between their work requests
	if(send)
		return ctx->last_send_compl;
	else
		return ctx->last_rcv_compl;
}

//connection management (연결, QP 생성, 연결 완료)
int init_connection(struct rdma_cm_id *id, int type, int always_poll, int flags);   // connection 초기화
int setup_connection(struct rdma_cm_id *id, struct rdma_conn_param *cm_params);     // QP 생성 및 설정
int finalize_connection(struct rdma_cm_id *id, struct rdma_conn_param *cm_params);  // 연결 완료

// device management (device 정보 조회 및 생성)
void try_build_device(struct rdma_cm_id* id);

// qp & context management (이벤트 수신할 채널 생성, QP 속성 및 매개변수 설정)
void build_cq_channel(struct rdma_cm_id *id);
void build_qp_attr(struct rdma_cm_id *id, struct ibv_qp_init_attr *qp_attr);
void build_rc_params(struct rdma_cm_id *id, struct rdma_conn_param *params);


/* ----- event handling ----- */
void rdma_event_loop(struct rdma_event_channel *ec, int exit_on_connect, int exit_on_disconnect);

//request completions
void update_completions(struct ibv_wc *wc);
void spin_till_response(struct rdma_cm_id *id, uint32_t seqn);
void block_till_response(struct rdma_cm_id *id, uint32_t seqn);
void spin_till_completion(struct rdma_cm_id *id, uint32_t wr_id);
void block_till_completion(struct rdma_cm_id *id, uint32_t wr_id);

// completion polling
void* poll_cq_spinning_loop();
void* poll_cq_blocking_loop();
void poll_cq(struct ibv_cq *cq, struct ibv_wc *wc);
void poll_cq_debug(struct ibv_cq *cq, struct ibv_wc *wc);


/* ----- helper functions ----- */
struct rdma_cm_id* find_next_connection(struct rdma_cm_id* id);
struct rdma_cm_id* find_connection_by_addr(struct sockaddr_in *addr); // find connection by ip
struct rdma_cm_id* find_connection_by_wc(struct ibv_wc *wc);          // find connection by wc (work completion)
struct rdma_cm_id* get_connection(int sockfd);


/* rc connection handling */
//setup (연결 상태 관리)
void rc_init(pre_conn_cb_fn, connect_cb_fn, completion_cb_fn, disconnect_cb_fn);
//state
void rc_set_state(struct rdma_cm_id *id, int new_state);
int rc_ready(int sockfd);
int rc_active(int sockfd);
int rc_terminated(int sockfd);
int rc_connection_count();
int rc_next_connection(int cur);
int rc_connection_meta(int sockfd);
char* rc_connection_ip(int sockfd);
int rc_connection_qpnum(int sockfd);
int rc_connection_cqnum(int sockfd);
struct ibv_pd * rc_get_pd(struct rdma_cm_id* id);
struct ibv_context * rc_get_context(int id);

void rc_set_sq_sz(int size);

//buffers
int _rc_acquire_buffer(int sockfd, void ** ptr, int user);          // 메시지를 저장할 버퍼 확보
int rc_acquire_buffer(int sockfd, struct app_context ** ptr);      
int rc_bind_buffer(struct rdma_cm_id *id, int buffer, uint32_t wr_id);  // 버퍼에 wr id 바인딩
int rc_release_buffer(int sockfd, uint32_t wr_id);                      // 버퍼 해제

//remove
void rc_disconnect(struct rdma_cm_id *id);
void rc_clear(struct rdma_cm_id *id);
void rc_die(const char *message);

#endif
