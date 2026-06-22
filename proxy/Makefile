CC = gcc
MHD_PREFIX := $(shell brew --prefix libmicrohttpd 2>/dev/null)
MHD_CFLAGS := $(if $(MHD_PREFIX),-I$(MHD_PREFIX)/include)
MHD_LDFLAGS := $(if $(MHD_PREFIX),-L$(MHD_PREFIX)/lib)
CFLAGS = -Wall -Wextra -Werror -g -O2 -I../deps/hiredis -I./include $(MHD_CFLAGS)
LDFLAGS = ../deps/hiredis/libhiredis.a $(MHD_LDFLAGS) -lmicrohttpd -lpthread -lm

SRC_DIR = src
OBJ_DIR = obj

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
TARGET = dicedb-proxy

all: hiredis $(TARGET)

hiredis:
	$(MAKE) -C ../deps/hiredis

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

test: all
	python3 tests/test_proxy.py

.PHONY: all clean hiredis test
