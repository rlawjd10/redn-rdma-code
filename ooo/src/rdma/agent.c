#include <sys/syscall.h>
#include <pthread.h>
#include <stdatomic.h>

#include "verbs.h"
#include "messaging.h"
#include "mr.h"
#include "agent.h"

pthread_t comm_thread;

int rdma_initialized = 0;
char port[10];

//initialize memory region information
void init_rdma_agent(char *listen_port, struct mr_context *regions,
		int region_count, uint16_t buffer_size,
		app_conn_cb_fn app_connect,
		app_disc_cb_fn app_disconnect,
		app_recv_cb_fn app_receive)
{
	//pthread_mutex_lock(&global_mutex);
	if(rdma_initialized)    //이미 초기화되었는지 
		return;

	if(region_count > MAX_MR)
		rc_die("region count is greater than MAX_MR");

	mrs = regions;
	num_mrs = region_count;
	msg_size = buffer_size;

    // callback 함수 설정 
	app_conn_event = app_connect;
	app_disc_event = app_disconnect;
	app_recv_event = app_receive;

	set_seed(5);

	if(listen_port)
		snprintf(port, sizeof(port), "%s", listen_port);

	// event기반 callback함수들 handling하기 위함 
	rc_init(on_pre_conn,
		on_connection,
		on_completion,
		on_disconnect);

	ec = rdma_create_event_channel();
	if (ec == NULL)
		rc_die("failed to create event channel");

	if(!listen_port)
		pthread_create(&comm_thread, NULL, client_loop, NULL);
	else
		pthread_create(&comm_thread, NULL, server_loop, port);

	rdma_initialized = 1;
}

//request connection to another RDMA agent (non-blocking)
//returns socket descriptor if successful, otherwise -1
int add_connection(char* ip, char *port, int app_type, int polling_loop, int flags) 
{
	debug_print("attempting to add connection to %s:%s\n", ip, port);

	if(!rdma_initialized)
		rc_die("can't add connection; client must be initialized first\n");

	struct addrinfo *addr;
	struct rdma_cm_id *cm_id = NULL;

	getaddrinfo(ip, port, NULL, &addr); 

	rdma_create_event_channel();

	rdma_create_id(ec, &cm_id, NULL, RDMA_PS_TCP);
	rdma_resolve_addr(cm_id, NULL, addr->ai_addr, TIMEOUT_IN_MS);

	freeaddrinfo(addr);

	int sockfd = init_connection(cm_id, app_type, polling_loop, flags);

	printf("[RDMA-Client] Creating connection (status:pending) to %s:%s on sockfd %d\n", ip, port, sockfd);

	return sockfd;
}

// event handling callbacks
static void on_pre_conn(struct rdma_cm_id *id)
{
	struct conn_context *ctx = (struct conn_context *)id->context;
    
    debug_print("mrs: %p, num_mrs: %d, msg_size: %d\n", mrs, num_mrs, msg_size);
	
	mr_register(ctx, mrs, num_mrs, msg_size);
}

static void on_connection(struct rdma_cm_id *id)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	printf("Connection established [sockfd:%d] [qpnum: %d]\n", ctx->sockfd, id->qp->qp_num);

	app_conn_event(ctx->sockfd);	// add_peer_socket 호출 
}

static void on_disconnect(struct rdma_cm_id *id)
{
	struct conn_context *ctx = (struct conn_context *)id->context;
	app_disc_event(ctx->sockfd);	// remove_peer_socket 호출 
	printf("Connection terminated [sockfd:%d]\n", ctx->sockfd);
}

static void on_completion(struct ibv_wc *wc)
{
	struct rdma_cm_id *id = find_connection_by_wc(wc);
	struct conn_context *ctx = (struct conn_context *)id->context;

	if (wc->opcode & IBV_WC_RECV) {
		uint32_t rcv_i = wc->wr_id;

		//if(wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM || wc->wr_id == 99) {
	       if(rc_ready(ctx->sockfd)) {	
			uint32_t app_id = ntohl(wc->imm_data);
			debug_print("application callback: seqn = %u\n", app_id);
			//if(app_id) {
				update_pending(id, app_id);
				struct app_context imm_msg;
				imm_msg.id = app_id;
				imm_msg.sockfd = ctx->sockfd;
				imm_msg.data = 0;
				app_recv_event(&imm_msg);	//test_callback 호출 
				return;
			//}
		}
	       else
		       rc_die("invalid message\n");
	}
	else { 
		debug_print("skipping message with opcode:%i, wr_id:%lu \n", wc->opcode, wc->wr_id);
		return;
	}
}

// client&server event loop
static void* client_loop()
{
	rdma_event_loop(ec, 0, 1); /* exit upon disconnect */
	rdma_destroy_event_channel(ec);
	debug_print("exiting rc_client_loop\n");
	return NULL;
}

static void* server_loop(void *port)
{
	struct sockaddr_in addr;
	struct rdma_cm_id *cm_id = NULL;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(atoi(port));
	addr.sin_addr.s_addr = INADDR_ANY;

	rdma_create_id(ec, &cm_id, NULL, RDMA_PS_TCP);
	rdma_bind_addr(cm_id, (struct sockaddr *)&addr);
	rdma_listen(cm_id, 100); /* backlog=10 is arbitrary */

	printf("[RDMA-Server] Listening on port %d for connections. interrupt (^C) to exit.\n", atoi(port));
	
	while (1) {
		rdma_event_loop(ec, 0, 0); /* do not exit upon disconnect */
		// 종료조건 -> main에서 SIGINT 발생시 종료 
	}

	rdma_destroy_id(cm_id);
	rdma_destroy_event_channel(ec);

	debug_print("exiting rc_server_loop\n");

	return 0;
}
