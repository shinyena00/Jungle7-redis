# 미니 Redis 서버 빌드 (Linux·WSL 전용: epoll을 쓴다)
#   make server : ./mini-redis 만들기
#   make clean
#
# 실행: ./mini-redis
#   MINI_REDIS_BIND (기본 127.0.0.1), MINI_REDIS_PORT (기본 6380)
# 테스트는 tests/ 안의 Makefile (cd tests && make e2e)

CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -g -O0 -D_GNU_SOURCE
INCLUDE := -Iinclude

SOURCES := src/server.c src/resp.c src/command.c src/store.c
HEADERS := include/store.h include/command.h include/resp.h
TARGET  := mini-redis

.PHONY: all server clean

all: server

server: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(SOURCES)

clean:
	rm -f $(TARGET)
