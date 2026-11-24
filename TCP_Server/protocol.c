#include "protocol.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/**
 * @function protocol_reply: Build a response followed by CRLF.
 * @param response: Response to fill; its command field is kept.
 * @param code: Numeric result code.
 * @param message: Fixed protocol message, without quotes or control characters.
 * @return: 0 on success, or -1 if the response does not fit.
 */
int protocol_reply(Response *response, int code, const char *message)
{
    response->code = code;
    response->message = message;
    int count = snprintf(response->text, sizeof(response->text),
                         "{\"type\":\"RESPONSE\",\"code\":%d,"
                         "\"message\":\"%s\",\"data\":null}\r\n", code, message);
    if (count < 0 || (size_t)count >= sizeof(response->text)) {
        response->length = 0;
        return -1;
    }
    response->length = (size_t)count;
    return 0;
}

/**
 * @function protocol_process: Validate a JSON request and dispatch its command.
 * @param request: Complete JSON message without CRLF.
 * @param length: Number of bytes in the message.
 * @param response: Result, including a safe command name for the log.
 * @return: 0 when a success/error response is ready, or -1 on an internal error.
 */
int protocol_process(const char *request, size_t length, Response *response)
{
    const char *end = NULL;
    strcpy(response->command, "-");

    /* Reject NUL characters that could truncate command names or JSON strings. */
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)request[i];
        if (c < 32 && c != '\t' && c != '\r' && c != '\n') {
            return protocol_reply(response, CODE_BAD_REQUEST, "INVALID_JSON");
        }
        if (c == '\\' && i + 1 < length) {
            if (length - i >= 6 && memcmp(request + i, "\\u0000", 6) == 0) {
                return protocol_reply(response, CODE_BAD_REQUEST, "INVALID_JSON");
            }
            ++i;
        }
    }

    cJSON *root = cJSON_ParseWithLengthOpts(request, length, &end, 0);
    if (root == NULL) {
        return protocol_reply(response, CODE_BAD_REQUEST, "INVALID_JSON");
    }
    while (end < request + length &&
           (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        ++end;
    }
    if (end != request + length) {
        cJSON_Delete(root);
        return protocol_reply(response, CODE_BAD_REQUEST, "INVALID_JSON");
    }

    cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(root) == 0 || cJSON_GetArraySize(root) != 2 ||
        cJSON_IsString(command) == 0 || cJSON_IsObject(data) == 0) {
        cJSON_Delete(root);
        return protocol_reply(response, CODE_BAD_REQUEST, "INVALID_REQUEST");
    }

    size_t command_length = strlen(command->valuestring);
    int valid = command_length > 0 && command_length < COMMAND_CAPACITY;
    for (size_t i = 0; valid != 0 && i < command_length; ++i) {
        char c = command->valuestring[i];
        int allowed = (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_';
        if (allowed == 0) {
            valid = 0;
        }
    }
    if (valid == 0) {
        cJSON_Delete(root);
        return protocol_reply(response, CODE_BAD_REQUEST, "INVALID_REQUEST");
    }
    strcpy(response->command, command->valuestring);

    int result;
    if (strcmp(response->command, "PING") == 0) {
        if (cJSON_GetArraySize(data) != 0) {
            result = protocol_reply(response, CODE_BAD_REQUEST, "INVALID_REQUEST");
        } else {
            result = protocol_reply(response, CODE_OK, "PONG");
        }
    } else {
        result = protocol_reply(response, CODE_BAD_REQUEST, "UNKNOWN_COMMAND");
    }
    cJSON_Delete(root);
    return result;
}
