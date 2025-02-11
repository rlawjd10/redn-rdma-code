#include "messaging.h"

//internal messaging protocol
__attribute__((visibility ("hidden"))) 
int create_message(struct rdma_cm_id *id, int msg_type, int value)
{
	struct conn_context *ctx = (struct conn_context *)id->context;

	//for the messaging protocol, we first check that previous message was transmitted
	//before overwriting local send buffer (to avoid corrupting data)

    // send buffer에 대한 lock을 획득하여 메시지 생성. (송신 버퍼 확보)
	void *ptr = NULL;
	int i = _rc_acquire_buffer(ctx->sockfd, &ptr, 0); // 확보된 버퍼의 인덱스.

    // mr 정보 설정.
	if(msg_type == MSG_MR) {
		debug_print("[BOOTSTRAP] creating MSG_MR on buffer[%d]\n", i);
		mr_prepare_msg(ctx, i, msg_type);
	}
    // 초기화 메시지 설정. -> addr과 length 설정을 통해 상대 노드에 필요한 정보 전달.
	else if(msg_type == MSG_INIT) {
		debug_print("[BOOTSTRAP] creating MSG_INIT on buffer[%d]\n", i);
		ctx->msg_send[i]->id = MSG_INIT;
		if(ctx->app_type >= 0)
			ctx->msg_send[i]->meta.mr.addr = ctx->app_type;

		ctx->msg_send[i]->meta.mr.length = value;
		if(value > MAX_MR) //2
			rc_die("error; local mr count shouldn't exceed MAX_MR");
	}
	else    // 메시지 타입이 잘못된 경우 에러 처리.
		rc_die("create message failed; invalid type");
	return i;
}
