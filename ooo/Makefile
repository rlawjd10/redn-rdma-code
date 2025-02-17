#src/rdma
LIBIBVERBS_DIR := $(abspath $(CURDIR)/lib/libibverbs)
LIBMLX5_DIR := $(abspath $(CURDIR)/lib/libmlx5-41mlnx1)

SRC_DIR   := $(addprefix src/, rdma)
BUILD_DIR := $(addprefix build/, rdma)

# wildcard를 이용해서 src/rdma의 .c, .cpp 파일 찾기 
SRC       := $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.c))
SRCCPP    := $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.cpp))
OBJ       := $(patsubst src/%.c,build/%.o,$(SRC))
OBJCPP    := $(patsubst src/%.cpp,build/%.o,$(SRCCPP))

# 컴파일러 및 컴파일 옵션 설정 
CC = gcc -g #-D_GNU_SOURCE # C compiler
CFLAGS = -fPIC -O2 -g -oO
CFLAGS += -I$(LIBMLX5_DIR)/src/
LDFLAGS = -shared  # linking flags

RM = rm -f  # rm command
TARGET_LIB = librdma.so # target lib

INCLUDES  := $(addprefix -I,src/ $(LIBMLX5_DIR)/src)

# RDMA 관련 플래스 및 링커 옵션 
RDMA_FLAGS += -DDEBUG
RDMA_FLAGS += -DEXP_VERBS
RDMA_CFLAGS += -D_GNU_SOURCE
RDMA_FLAGS += -fvisibility=default

LDFLAGS += -Wl,-rpath=/usr/local/lib -L/usr/local/lib -Wl,-rpath=/usr/lib -L/usr/lib
LDLIBS = -lrdmacm -libverbs -lpthread -lmlx5

# vpath 설정 - 소스파일을 SRC_DIR에서 찾도록 함 (src/rdma)
vpath %.c $(SRC_DIR)
vpath %.cpp $(SRC_DIR)

# 컴파일 규칙 정의 
define make-goal
$1/%.o: %.c
	$(CC) $(INCLUDES) $(RDMA_FLAGS) -fPIC -c $$< -o $$@
$1/%.o: %.cpp
	$(CXX) $(INCLUDES) $(RDMA_FLAGS) -fPIC -c $$< -o $$@
endef

# phony 타겟 및 기본 타겟 
.PHONY: all checkdirs clean

all: checkdirs librdma
checkdirs: $(BUILD_DIR)
	@mkdir -p bin

clean:
	@rm -rf $(BUILD_DIR)

$(BUILD_DIR):
	@mkdir -p $@

# 라이브러리(librdma) 타겟 
librdma: $(OBJ) $(OBJCPP)
	ar cr build/librdma.a $(OBJ) $(OBJCPP)
	$(CC) -shared $(DEBUG) -o build/librdma.so $(OBJ) $(OBJCPP) $(LDFLAGS) $(LDLIBS)

$(foreach bdir,$(BUILD_DIR),$(eval $(call make-goal,$(bdir))))

