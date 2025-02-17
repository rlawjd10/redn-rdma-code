LIBIBVERBS_DIR := $(abspath $(CURDIR)/lib/libibverbs)
LIBMLX5_DIR := $(abspath $(CURDIR)/lib/libmlx5)

MODULES   := rdma

SRC_DIR   := $(addprefix src/,$(MODULES))
BUILD_DIR := $(addprefix build/,$(MODULES))

SRC       := $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.c))
SRCCPP    := $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.cpp))
OBJ       := $(patsubst src/%.c,build/%.o,$(SRC))
OBJCPP    := $(patsubst src/%.cpp,build/%.o,$(SRCCPP))

CC = gcc -g #-D_GNU_SOURCE # C compiler
CFLAGS = -fPIC -O2 -g -oO
CFLAGS += -I$(LIBMLX5_DIR)/src/
LDFLAGS = -shared  # linking flags

RM = rm -f  # rm command
TARGET_LIB = librdma.so # target lib

INCLUDES  := $(addprefix -I,src/ $(LIBMLX5_DIR)/src)

RDMA_FLAGS += -DDEBUG
RDMA_FLAGS += -DEXP_VERBS
RDMA_CFLAGS += -D_GNU_SOURCE
RDMA_FLAGS += -fvisibility=default

LDFLAGS += -Wl,-rpath=/usr/local/lib -L/usr/local/lib -Wl,-rpath=/usr/lib -L/usr/lib
LDLIBS = -lrdmacm -libverbs -lpthread -lmlx5

vpath %.c $(SRC_DIR)
vpath %.cpp $(SRC_DIR)

define make-goal
$1/%.o: %.c
	$(CC) $(INCLUDES) $(RDMA_FLAGS) -fPIC -c $$< -o $$@
$1/%.o: %.cpp
	$(CXX) $(INCLUDES) $(RDMA_FLAGS) -fPIC -c $$< -o $$@
endef

.PHONY: all checkdirs clean

all: checkdirs librdma
checkdirs: $(BUILD_DIR)
	@mkdir -p bin

clean:
	@rm -rf $(BUILD_DIR)

$(BUILD_DIR):
	@mkdir -p $@

#build/libmlfs.a: $(OBJ)
librdma: $(OBJ) $(OBJCPP)
	ar cr build/librdma.a $(OBJ) $(OBJCPP)
	$(CC) -shared $(DEBUG) -o build/librdma.so $(OBJ) $(OBJCPP) $(LD_FLAGS) $(LDLIBS)

$(foreach bdir,$(BUILD_DIR),$(eval $(call make-goal,$(bdir))))

