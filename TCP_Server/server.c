#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#define INITIAL_CAPACITY 16
#define ACCEPTS_PER_TURN 32
#define BACKLOG 4096
#define LOG_FILE "server.log"

/* Entry zero is the listener; matching entries describe the same client. */
static struct pollfd *poll_fds = NULL;
static Session *sessions = NULL;
static size_t connection_count = 0;
static size_t connection_capacity = 0;

/**
 * @function set_nonblocking: Keep a socket call from waiting for a client.
 * @param socket_fd: Listening or connected socket.
 * @return: 0 on success, or -1 on a file descriptor error.
 */
static int set_nonblocking(int socket_fd)
{
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

/**
 * @function add_connection: Grow the arrays and store a client's Session.
 * @param client_fd: Nonblocking socket returned by accept.
 * @param address: Client IPv4 address and port.
 * @return: 0 on success, or -1 when memory is unavailable.
 */
static int add_connection(int client_fd, const struct sockaddr_in *address)
{
    if (connection_count == connection_capacity) {
        size_t capacity;
        if (connection_capacity == 0) {
            capacity = INITIAL_CAPACITY;
        } else {
            capacity = connection_capacity * 2;
        }
        if (capacity < connection_capacity || capacity > SIZE_MAX / sizeof(*sessions) ||
            capacity > SIZE_MAX / sizeof(*poll_fds)) {
            return -1;
        }
        struct pollfd *new_fds = realloc(poll_fds, capacity * sizeof(*poll_fds));
        if (new_fds == NULL) {
            return -1;
        }
        poll_fds = new_fds;
        Session *new_sessions = realloc(sessions, capacity * sizeof(*sessions));
        if (new_sessions == NULL) {
            return -1;
        }
        sessions = new_sessions;
        connection_capacity = capacity;
    }
    session_init(&sessions[connection_count], client_fd, address);
    poll_fds[connection_count].fd = client_fd;
    poll_fds[connection_count].events = POLLIN;
    poll_fds[connection_count].revents = 0;
    ++connection_count;
    return 0;
}

/**
 * @function remove_connection: Close one socket and remove its Session.
 * @param index: Client position in both arrays.
 * @return: No return value; the last client fills the removed slot.
 */
static void remove_connection(size_t index)
{
    close(sessions[index].socket_fd);
    --connection_count;
    if (index != connection_count) {
        poll_fds[index] = poll_fds[connection_count];
        sessions[index] = sessions[connection_count];
    }
}

/**
 * @function accept_connections: Accept a batch without starving existing clients.
 * @param listen_fd: Listening socket reported readable by poll.
 * @param reserve_file: Spare descriptor used to refuse a connection at the limit.
 * @return: No return value; rejected sockets are closed immediately.
 */
static void accept_connections(int listen_fd, FILE **reserve_file)
{
    for (int accepted = 0; accepted < ACCEPTS_PER_TURN; ++accepted) {
        struct sockaddr_in address;
        socklen_t address_length = sizeof(address);
        int client_fd = accept(listen_fd, (struct sockaddr *)&address, &address_length);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if ((errno == EMFILE || errno == ENFILE) && *reserve_file != NULL) {
                fclose(*reserve_file);
                client_fd = accept(listen_fd, NULL, NULL);
                if (client_fd >= 0) {
                    close(client_fd);
                }
                *reserve_file = fopen("/dev/null", "r");
            }
            break;
        }
        if (set_nonblocking(client_fd) < 0 || add_connection(client_fd, &address) < 0) {
            close(client_fd);
        }
    }
}

/**
 * @function open_listener: Bind an IPv4 TCP socket and start listening.
 * @param port: Local port in host byte order.
 * @return: Nonblocking listening socket, or -1 on error.
 */
static int open_listener(unsigned short port)
{
    struct sockaddr_in address = {0};
    int reuse = 1;
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        close(socket_fd);
        return -1;
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(socket_fd, BACKLOG) < 0 || set_nonblocking(socket_fd) < 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

/**
 * @function main: Serve multiple TCP clients in one poll loop.
 * @param argc: Number of command-line arguments.
 * @param argv: Program name and port number.
 * @return: EXIT_FAILURE on startup or poll failure; normally keeps serving.
 */
int main(int argc, char *argv[])
{
    if (argc != 2 || argv[1][0] == '\0') {
        fprintf(stderr, "Usage: %s Port_Number\n", argv[0]);
        return EXIT_FAILURE;
    }
    for (const char *p = argv[1]; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            fputs("Invalid port\n", stderr);
            return EXIT_FAILURE;
        }
    }
    char *end;
    errno = 0;
    unsigned long port = strtoul(argv[1], &end, 10);
    if (errno != 0 || *end != '\0' || port == 0 || port > 65535) {
        fputs("Invalid port\n", stderr);
        return EXIT_FAILURE;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    /* Use the allowed descriptor limit instead of imposing a fixed client cap. */
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < limit.rlim_max) {
        limit.rlim_cur = limit.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &limit) < 0) {
            perror("setrlimit");
        }
    }
    FILE *log_file = fopen(LOG_FILE, "a");
    if (log_file == NULL) {
        perror(LOG_FILE);
        return EXIT_FAILURE;
    }
    FILE *reserve_file = fopen("/dev/null", "r");
    int listen_fd = open_listener((unsigned short)port);
    struct sockaddr_in unused_address = {0};
    if (listen_fd < 0 || add_connection(listen_fd, &unused_address) < 0) {
        perror("server startup");
        if (listen_fd >= 0) {
            close(listen_fd);
        }
        if (reserve_file != NULL) {
            fclose(reserve_file);
        }
        fclose(log_file);
        free(poll_fds);
        free(sessions);
        return EXIT_FAILURE;
    }

    for (;;) {
        int events = poll(poll_fds, (nfds_t)connection_count, -1);
        if (events < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        /* | groups event flags; & checks whether any grouped flag occurred. */
        /* POLLERR: socket error; POLLHUP: hangup; POLLNVAL: invalid socket fd. */
        if ((poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fputs("Listening socket failed\n", stderr);
            break;
        }
        /* POLLIN on the listener means a client is waiting for accept(). */
        if ((poll_fds[0].revents & POLLIN) != 0) {
            accept_connections(listen_fd, &reserve_file);
        }

        size_t i = 1;
        while (i < connection_count) {
            short ready = poll_fds[i].revents;
            Session *session = &sessions[i];
            if (ready == 0) {
                ++i;
                continue;
            }
            /* Close this client if its socket has an error or is invalid. */
            if ((ready & (POLLERR | POLLNVAL)) != 0) {
                remove_connection(i);
                continue;
            }
            /* POLLIN: data or EOF; POLLHUP: the peer has closed its side. */
            if (session->read_closed == 0 && session->output_length == 0 &&
                (ready & (POLLIN | POLLHUP)) != 0) {
                if (session_receive(session) < 0) {
                    remove_connection(i);
                    continue;
                }
            }
            if (session_send(session) < 0 || session_process(session, log_file) < 0 ||
                session_finished(session) != 0) {
                remove_connection(i);
                continue;
            }
            /* POLLOUT waits to send replies; POLLIN waits for more requests. */
            if (session->output_length > 0) {
                poll_fds[i].events = POLLOUT;
            } else {
                poll_fds[i].events = POLLIN;
            }
            ++i;
        }
    }

    for (size_t i = 1; i < connection_count; ++i) {
        close(sessions[i].socket_fd);
    }
    close(listen_fd);
    if (reserve_file != NULL) {
        fclose(reserve_file);
    }
    fclose(log_file);
    free(poll_fds);
    free(sessions);
    return EXIT_FAILURE;
}
