#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/** @function read_response: Print one response line from the server. */
static int read_response(int socket_fd)
{
    char ch;
    while (recv(socket_fd, &ch, 1, 0) == 1) {
        if (ch != '\r') {
            putchar(ch);
        }
        if (ch == '\n') {
            return 0;
        }
    }
    return -1;
}

/** @function main: Send JSON lines and show server responses. */
int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s Server_IP Port\n", argv[0]);
        return 1;
    }

    char *end;
    long port = strtol(argv[2], &end, 10);
    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons((unsigned short)port);
    if (*end != '\0' || port < 1 || port > 65535 ||
        inet_pton(AF_INET, argv[1], &server.sin_addr) != 1) {
        fputs("Invalid IP or port\n", stderr);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return 1;
    }
    if (connect(socket_fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect");
        close(socket_fd);
        return 1;
    }

    char request[4096];
    for (;;) {
        fputs("JSON> ", stdout);
        fflush(stdout);
        if (fgets(request, sizeof(request), stdin) == NULL) {
            break;
        }
        size_t length = strlen(request);
        if (length == 0 || request[length - 1] != '\n') {
            fputs("Input is too long\n", stderr);
            break;
        }
        if (length == 1) {
            break;
        }
        request[length - 1] = '\r';
        request[length] = '\n';
        ++length;

        size_t sent = 0;
        while (sent < length) {
            ssize_t count = send(socket_fd, request + sent, length - sent, 0);
            if (count <= 0) {
                break;
            }
            sent += (size_t)count;
        }
        if (sent != length || read_response(socket_fd) < 0) {
            fputs("Connection closed\n", stderr);
            break;
        }
    }
    close(socket_fd);
    return 0;
}
