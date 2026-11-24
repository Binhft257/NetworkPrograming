#include "session.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

/**
 * @function write_log: Append one result without recording request data.
 * @param log_file: Server log opened in append mode.
 * @param session: Client address and current login state.
 * @param response: Validated command, result code, and fixed message.
 * @return: No return value; a log error does not stop other clients.
 */
static void write_log(FILE *log_file, const Session *session,
                      const Response *response)
{
    char timestamp[20];
    char ip[INET_ADDRSTRLEN];
    struct tm local_time;
    time_t now = time(NULL);
    static int error_reported = 0;

    if (localtime_r(&now, &local_time) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%d/%m/%Y %H:%M:%S", &local_time) == 0) {
        strcpy(timestamp, "00/00/0000 00:00:00");
    }
    if (inet_ntop(AF_INET, &session->address.sin_addr, ip, sizeof(ip)) == NULL) {
        strcpy(ip, "unknown");
    }
    int result = fprintf(log_file, "[%s]$%s:%u$%d$%s$%d$%s\n",
                         timestamp, ip, (unsigned int)ntohs(session->address.sin_port),
                         session->user_id, response->command, response->code,
                         response->message);
    int flushed = fflush(log_file);
    if (result < 0 || flushed != 0) {
        if (error_reported == 0) {
            fputs("Cannot write server.log\n", stderr);
            error_reported = 1;
        }
        clearerr(log_file);
    }
}

/**
 * @function session_init: Initialize the state of a newly accepted client.
 * @param session: Session to initialize.
 * @param socket_fd: Nonblocking client socket.
 * @param address: Address returned by accept.
 * @return: No return value.
 */
void session_init(Session *session, int socket_fd,
                  const struct sockaddr_in *address)
{
    memset(session, 0, sizeof(*session));
    session->socket_fd = socket_fd;
    session->address = *address;
    session->user_id = -1;
}

/**
 * @function session_receive: Read available bytes once without waiting.
 * @param session: Client with input space available.
 * @return: 0 on data, EOF, or a temporary interruption; -1 on a socket error.
 */
int session_receive(Session *session)
{
    size_t available = sizeof(session->input) - session->input_length;
    if (available == 0 || session->read_closed != 0) {
        return 0;
    }
    ssize_t count = recv(session->socket_fd, session->input + session->input_length,
                         available, 0);
    if (count < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        } else {
            return -1;
        }
    }
    if (count == 0) {
        session->read_closed = 1;
    } else {
        session->input_length += (size_t)count;
    }
    return 0;
}

/**
 * @function session_queue: Add one complete message to the pending output.
 * @param session: Destination client.
 * @param message: Message bytes, including CRLF.
 * @param length: Number of bytes to append.
 * @return: 0 on success, or -1 if there is not enough space.
 */
int session_queue(Session *session, const char *message, size_t length)
{
    if (length > sizeof(session->output) - session->output_length) {
        return -1;
    }
    memcpy(session->output + session->output_length, message, length);
    session->output_length += length;
    return 0;
}

/**
 * @function session_process: Process complete frames and keep unfinished input.
 * @param session: Client whose input buffer may contain JSON messages.
 * @param log_file: Append log for the processing results.
 * @return: 0 on success, or -1 if a response cannot be prepared.
 */
int session_process(Session *session, FILE *log_file)
{
    size_t start = 0;
    int processed = 0;

    for (size_t i = 0; i < session->input_length; ++i) {
        if (session->input[i] != '\n') {
            continue;
        }
        if (processed == REQUESTS_PER_TURN ||
            sizeof(session->output) - session->output_length < RESPONSE_CAPACITY) {
            break;
        }
        Response response;
        strcpy(response.command, "-");
        size_t length = i - start;
        int result;

        if (session->discarding != 0) {
            result = protocol_reply(&response, CODE_BAD_REQUEST, "MESSAGE_TOO_LARGE");
        } else if (length == 0 || session->input[i - 1] != '\r') {
            result = protocol_reply(&response, CODE_BAD_REQUEST, "INVALID_FRAME");
        } else {
            result = protocol_process(session->input + start, length - 1, &response);
        }
        if (result < 0) {
            result = protocol_reply(&response, CODE_INTERNAL_ERROR, "INTERNAL_ERROR");
        }
        if (result < 0 || session_queue(session, response.text, response.length) < 0) {
            return -1;
        }
        write_log(log_file, session, &response);
        session->discarding = 0;
        start = i + 1;
        ++processed;
    }
    session->input_length -= start;
    memmove(session->input, session->input + start, session->input_length);

    if (session->input_length == sizeof(session->input) &&
        memchr(session->input, '\n', session->input_length) == NULL) {
        session->input_length = 0;
        session->discarding = 1;
    }

    /* A half-closed client may still read an error for its unfinished frame. */
    if (session->read_closed != 0 &&
        (session->input_length > 0 || session->discarding != 0) &&
        memchr(session->input, '\n', session->input_length) == NULL &&
        sizeof(session->output) - session->output_length >= RESPONSE_CAPACITY) {
        Response response;
        strcpy(response.command, "-");
        const char *message;
        if (session->discarding != 0) {
            message = "MESSAGE_TOO_LARGE";
        } else {
            message = "INCOMPLETE_MESSAGE";
        }
        if (protocol_reply(&response, CODE_BAD_REQUEST, message) < 0 ||
            session_queue(session, response.text, response.length) < 0) {
            return -1;
        }
        write_log(log_file, session, &response);
        session->input_length = 0;
        session->discarding = 0;
    }
    return 0;
}

/**
 * @function session_send: Send queued bytes and remember a partial send.
 * @param session: Client with a response waiting to be sent.
 * @return: 0 when sent or waiting for POLLOUT, or -1 on a socket error.
 */
int session_send(Session *session)
{
    while (session->output_sent < session->output_length) {
        ssize_t count = send(session->socket_fd, session->output + session->output_sent,
                             session->output_length - session->output_sent, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (count <= 0) {
            return -1;
        }
        session->output_sent += (size_t)count;
    }
    session->output_length = 0;
    session->output_sent = 0;
    return 0;
}

/**
 * @function session_finished: Check whether EOF and all pending data are handled.
 * @param session: Client state to inspect.
 * @return: 1 when the socket can be closed, otherwise 0.
 */
int session_finished(const Session *session)
{
    if (session->read_closed == 0) {
        return 0;
    }
    if (session->input_length > 0) {
        return 0;
    }
    if (session->output_length > 0) {
        return 0;
    }
    if (session->discarding != 0) {
        return 0;
    }
    return 1;
}
