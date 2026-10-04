#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 9410
#define BUFFER_SIZE 1024

int main()
{
    int server_fd;
    int client_fd;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;

    socklen_t client_len = sizeof(client_addr);

    char buffer[BUFFER_SIZE];

    /* Create TCP socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    printf("TCP socket created successfully.\n");

    /* Clear server address structure */
    memset(&server_addr, 0, sizeof(server_addr));

    /* IPv4 */
    server_addr.sin_family = AF_INET;

    /* Accept connections from any network interface */
    server_addr.sin_addr.s_addr = INADDR_ANY;

    /* Port 9410 */
    server_addr.sin_port = htons(PORT);

    /* Bind socket to port */
    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Socket bound to port %d.\n", PORT);

    /* Listen for connections */
    if (listen(server_fd, 5) < 0)
    {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("RemoteOps Agent started.\n");
    printf("Listening on TCP port %d...\n", PORT);

    while (1)
    {
        /* Accept Controller connection */
        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_addr,
                           &client_len);

        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        printf("Controller connected.\n");

        /* Receive data */
        memset(buffer, 0, sizeof(buffer));

        int bytes_received = recv(client_fd,
                                  buffer,
                                  sizeof(buffer) - 1,
                                  0);

        if (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';

            printf("Received: %s", buffer);

            /* Temporary response */
            const char *response =
                "Agent received your message.\n";

            send(client_fd,
                 response,
                 strlen(response),
                 0);
        }

        close(client_fd);

        printf("Controller disconnected.\n");
    }

    close(server_fd);

    return 0;
}
