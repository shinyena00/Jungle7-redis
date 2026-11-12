# 조각 1 테스트 빌드
#   make unit   : 단위 테스트 (store.c 구현이 있어야 링크됨)
#   make e2e    : 외부 테스트 (서버를 먼저 띄워 둘 것, 포트는 MINI_REDIS_PORT, 기본 6380)
#   make check  : 둘 다
#
# 구현 파일 위치가 다르면: make unit STORE_SRC=../src/store.c

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -g -O0 -D_GNU_SOURCE
INCLUDE := -I../include
STORE_SRC ?= ../src/store.c
BUILD   := build

.PHONY: all unit e2e check clean

all: $(BUILD)/test_store $(BUILD)/test_resp

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_store: unit/test_store.c check.h ../include/store.h $(STORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ unit/test_store.c $(STORE_SRC)

$(BUILD)/test_resp: e2e/test_resp.c e2e/resp_client.c e2e/resp_client.h check.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ e2e/test_resp.c e2e/resp_client.c

unit: $(BUILD)/test_store
	./$(BUILD)/test_store

e2e: $(BUILD)/test_resp
	./$(BUILD)/test_resp

check: unit e2e

clean:
	rm -rf $(BUILD)
