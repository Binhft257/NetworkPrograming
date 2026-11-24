#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#define COMMAND_CAPACITY 64
#define RESPONSE_CAPACITY 256

#define CODE_OK 200
#define CODE_BAD_REQUEST 400
#define CODE_INTERNAL_ERROR 500

typedef struct Response {
    int code;
    char command[COMMAND_CAPACITY];
    const char *message;
    char text[RESPONSE_CAPACITY];
    size_t length;
} Response;

int protocol_reply(Response *response, int code, const char *message);
int protocol_process(const char *request, size_t length, Response *response);

#endif
