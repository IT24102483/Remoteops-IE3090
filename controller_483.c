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

    close(sock);
    return 0;
}
