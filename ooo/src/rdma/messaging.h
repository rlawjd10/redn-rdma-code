#ifndef RDMA_MESSAGING_H
#define RDMA_MESSAGING_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "utils.h"
#include "mr.h"

// 메시지 타입 정의
enum message_id
{
	MSG_INVALID = 0,
	MSG_INIT,
	MSG_MR,
	MSG_READY,
	MSG_DONE,
	MSG_CUSTOM
};

struct message
{
	int id;

	union
	{
		struct mr_context mr;
		struct app_context app;
	} meta;

	char data[];    // 가변 큭 데이터를 포함할 수 있음.
};

// 메시지 생성 함수. 
int create_message(struct rdma_cm_id *id, int msg_type, int value);

#endif
