#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 9410

int main(void)
{
    int sock;
    struct sockaddr_in server = {0};

    /* Create a TCP socket */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return 1;
    }

    /* Agent address on this same machine */
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1",
                  &server.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address\n");
        close(sock);
        return 1;
    }

    /* Connect to the Agent */
    if (connect(sock, (struct sockaddr *)&server,
                sizeof(server)) == -1) {
        perror("connect");
        close(sock);
        return 1;
    }

    
printf("Connected to Agent on port %d\n", PORT);

char command[1024];
char response[8192];

while (1) {
    printf("remoteops> ");
    fflush(stdout);

    if (fgets(command, sizeof(command), stdin) == NULL) {
        break;
    }

    size_t length = strlen(command);

    if (length == 0 || command[length - 1] != '\n') {
        puts("Command too long. Connection will close.");
        break;
    }

    if (length == 1) {
        continue;
    }

    size_t sent = 0;
    int failed = 0;

    while (sent < length) {
        ssize_t count = send(
            sock, command + sent, length - sent, MSG_NOSIGNAL
        );

        if (count <= 0) {
            perror("send");
            failed = 1;
            break;
        }

        sent += (size_t)count;
    }

    if (failed) {
        break;
    }

    size_t received = 0;
    int complete = 0;

    while (received < sizeof(response) - 1) {
        char byte;
        ssize_t count = recv(sock, &byte, 1, 0);

        if (count <= 0) {
            puts("Agent closed the connection or receive failed.");
            failed = 1;
            break;
        }

        response[received++] = byte;

        if (byte == '\n') {
            complete = 1;
            break;
        }
    }

    if (failed || !complete) {
        if (!failed) {
            puts("Response too long. Connection will close.");
        }
        break;
    }

    response[received] = '\0';
    printf("Agent: %s", response);

    if (strcmp(command, "QUIT\n") == 0) {
        break;
    }
}


    close(sock);
    return 0;
}
