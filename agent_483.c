#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 9410
#define BUFFER_SIZE 1024
#define AUTH_COMMAND "AUTH OPS-2483"

/* Send the complete response, even if send() sends only part. */
static int send_response(int fd, const char *text)
{
    size_t sent = 0;
    size_t length = strlen(text);

    while (sent < length) {
        ssize_t n = send(fd, text + sent, length - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Read one newline-terminated command from TCP. */
static int read_command(int fd, char *buffer, size_t capacity)
{
    size_t used = 0;

    while (used < capacity - 1) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;

        if (ch == '\n') {
            if (used > 0 && buffer[used - 1] == '\r')
                used--;
            buffer[used] = '\0';
            return 1;
        }

        buffer[used++] = ch;
    }

    return -1;
}

int main(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("RemoteOps Agent - IT24102483\n");
    printf("Listening on TCP port %d...\n", PORT);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }

        printf("Controller connected.\n");
        int authenticated = 0;
        char command[BUFFER_SIZE];

        while (1) {
            int result = read_command(client_fd, command, sizeof(command));
            if (result == 0)
                break;

            if (result < 0) {
                send_response(client_fd,
                              "ERR 003 COMMAND_TOO_LONG SID:3842\n");
                break;
            }

            printf("Received: %s\n", command);

            if (!authenticated) {
                if (strcmp(command, AUTH_COMMAND) == 0) {
                    authenticated = 1;
                    if (send_response(client_fd,
                                      "OK AUTH SID:3842\n") < 0)
                        break;
                } else {
                    send_response(client_fd,
                                  "ERR 001 UNAUTHORIZED SID:3842\n");
                    break;
                }
                continue;
            }

            if (strcmp(command, "QUIT") == 0) {
                send_response(client_fd, "OK BYE SID:3842\n");
                break;
            }

            if (send_response(client_fd,
                              "ERR 003 UNKNOWN_COMMAND SID:3842\n") < 0)
                break;
        }

        close(client_fd);
        printf("Controller disconnected.\n");
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
