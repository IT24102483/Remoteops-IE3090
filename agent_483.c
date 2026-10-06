#include <stdio.h>
#include <dirent.h>
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


/* Read Linux system statistics and send a personalised response. */
static int handle_sysinfo(int fd)
{
    double cpu_load = 0.0;
    double uptime = 0.0;
    unsigned long total_kb = 0;
    unsigned long available_kb = 0;
    int found_total = 0;
    int found_available = 0;
    char line[256];
    char response[256];

    FILE *file = fopen("/proc/loadavg", "r");
    if (!file)
        return send_response(fd,
                             "ERR 006 SYSINFO_FAILED SID:3842\n");
    int valid = fscanf(file, "%lf", &cpu_load) == 1;
    fclose(file);
    if (!valid)
        return send_response(fd,
                             "ERR 006 SYSINFO_FAILED SID:3842\n");

    file = fopen("/proc/uptime", "r");
    if (!file)
        return send_response(fd,
                             "ERR 006 SYSINFO_FAILED SID:3842\n");
    valid = fscanf(file, "%lf", &uptime) == 1;
    fclose(file);
    if (!valid)
        return send_response(fd,
                             "ERR 006 SYSINFO_FAILED SID:3842\n");

    file = fopen("/proc/meminfo", "r");
    if (!file)
        return send_response(fd,
                             "ERR 006 SYSINFO_FAILED SID:3842\n");

    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "MemTotal: %lu kB", &total_kb) == 1)
            found_total = 1;
        if (sscanf(line, "MemAvailable: %lu kB",
                   &available_kb) == 1)
            found_available = 1;
    }
    fclose(file);

    if (!found_total || !found_available ||
        available_kb > total_kb)
        return send_response(fd,
                             "ERR 006 SYSINFO_FAILED SID:3842\n");

    double used_mb = (total_kb - available_kb) / 1024.0;

    snprintf(response, sizeof(response),
             "OK SYSINFO %.2f %.2f %.0f SID:3842\n",
             cpu_load, used_mb, uptime);

    return send_response(fd, response);
}


/* Return process names and PIDs as one comma-separated line. */
static int handle_listproc(int fd)
{
    DIR *directory = opendir("/proc");
    if (!directory)
        return send_response(fd,
                             "ERR 006 LISTPROC_FAILED SID:3842\n");

    char response[8000] = "OK PROCS ";
    size_t used = strlen(response);
    const char *suffix = " SID:3842\n";
    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(directory)) != NULL) {
        const char *pid = entry->d_name;
        if (!pid[0] || strspn(pid, "0123456789") != strlen(pid))
            continue;

        char filename[512];
        snprintf(filename, sizeof(filename), "/proc/%s/comm", pid);

        FILE *file = fopen(filename, "r");
        if (!file)
            continue;

        char name[256];
        if (!fgets(name, sizeof(name), file)) {
            fclose(file);
            continue;
        }
        fclose(file);

        name[strcspn(name, "\r\n")] = '\0';
        for (size_t i = 0; name[i]; i++) {
            unsigned char ch = (unsigned char)name[i];
            if (ch <= 32 || ch == ',' || ch == '(' || ch == ')')
                name[i] = '_';
        }

        char item[600];
        int length = snprintf(item, sizeof(item), "%s%s(%s)",
                              count ? "," : "", name, pid);
        if (length < 0 || (size_t)length >= sizeof(item))
            continue;

        if (used + (size_t)length + strlen(suffix) + 1 >
            sizeof(response))
            break;

        memcpy(response + used, item, (size_t)length);
        used += (size_t)length;
        response[used] = '\0';
        count++;
    }

    closedir(directory);

    if (!count) {
        strcpy(response + used, "NONE");
        used += strlen("NONE");
    }

    strcpy(response + used, suffix);
    return send_response(fd, response);
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

            if (strcmp(command, "SYSINFO") == 0) {
                if (handle_sysinfo(client_fd) < 0)
                    break;
                continue;
            }

            if (strcmp(command, "LISTPROC") == 0) {
                if (handle_listproc(client_fd) < 0)
                    break;
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
