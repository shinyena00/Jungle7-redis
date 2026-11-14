CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -g

SERVER := build/mini-redis
SOURCES := src/server.c src/resp.c src/command.c src/store.c
HEADERS := include/resp.h include/command.h include/store.h

.PHONY: all
all: $(SERVER)

$(SERVER): $(SOURCES) $(HEADERS)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)
