CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -DCJSON_NESTING_LIMIT=64 -Ithird_party/cjson
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS = -lm

SOURCES = TCP_Server/server.c TCP_Server/session.c TCP_Server/protocol.c third_party/cjson/cJSON.c
HEADERS = TCP_Server/session.h TCP_Server/protocol.h third_party/cjson/cJSON.h

all: server client

server: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

client: TCP_Client/client.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ TCP_Client/client.c

clean:
	rm -f server client

.PHONY: all clean
