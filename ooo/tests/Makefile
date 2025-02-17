
# c 컴파일러 지정, GNU 확장 활성화, 최적화 레벨 
CC = gcc -g -D_GNU_SOURCE -O0

# 디렉토리 경로 설정 
LIBMLX5_DIR := $(abspath $(CURDIR)/../lib/libmlx5-41mlnx1)
RDMA_DIR := $(abspath $(CURDIR)/../build/)
INCLUDES  := -I$(RDMA_DIR)/../src/rdma -I$(LIBMLX5_DIR)/src/
$(info RDMA_DIR="$(RDMA_DIR)")
$(info INCLUDES="$(INCLUDES)")


LDLIBS = -lrdma -lm -pthread -lrt
LDFLAGS += -Wl,-rpath=$(abspath $(RDMA_DIR)) -L$(RDMA_DIR) 

RM = rm -f 
#DEPS = time_stat.c
#SRCS = rpc_client.c rpc_server.c cc_lat.c cc_bw.c if_lat.c if_bw.c calc_lat.c noop_lat.c atm_lat.c atm_bw.c write_lat.c write_bw.c read_lat.c loop_lat.c loop_bw.c hash_get.c fork_test.c lock_test.c chain_lat.c chain_bw.c if_cas_bw.c if_copy_bw.c hash_cas_bw.c hash_copy_bw.c test.c test2.c test3.c cqe_err.c example_test.c example_cas_test.c
SRCS = test.c
OBJS = $(SRCS:.c=.o)
#OBJS += $(DEPS:.c=.o)
BIN = $(SRCS:.c=)

# 항상 실행되는 타겟 
.PHONY: clean

# 기본 타겟: BIN에 포함된 실행 파일들을 빌드
all: $(BIN)


# BIN 타겟 (빌드 규칙)
$(BIN):$(OBJS)
		$(CC) $(OBJS) ${LDFLAGS} ${LDLIBS} -o $@

%.o: %.c
		$(CC) $(INCLUDES) -c $(CFLAGS) -o $@ $<

clean:
		$(RM) $(OBJS) $(BIN)