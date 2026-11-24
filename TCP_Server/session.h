#ifndef SESSION_H
#define SESSION_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>

#define BUFFER_CAPACITY 4096
#define REQUESTS_PER_TURN 32

typedef struct Session {
    int socket_fd;
    struct sockaddr_in address;
    int user_id;                       /* -1 means not logged in */
    char input[BUFFER_CAPACITY];
    size_t input_length;
    char output[BUFFER_CAPACITY];
    size_t output_length;
    size_t output_sent;
    int discarding;                    /* discard an oversized frame until LF */
    int read_closed;                   /* recv returned 0 */
} Session;

void session_init(Session *session, int socket_fd,
                  const struct sockaddr_in *address);
int session_receive(Session *session);
int session_process(Session *session, FILE *log_file);
int session_queue(Session *session, const char *message, size_t length);
int session_send(Session *session);
int session_finished(const Session *session);

#endif
