#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 9410
#define FILE_LIMIT (10ULL * 1024 * 1024)

static int send_all(int fd, const void *data, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = send(fd, (const char *)data + done,
                         size - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int read_line(int fd, char *line, size_t capacity)
{
    size_t used = 0;
    while (used < capacity - 1) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        line[used++] = ch;
        if (ch == '\n') {
            line[used] = '\0';
            return 0;
        }
    }
    return -1;
}

static int valid_filename(const char *name)
{
    size_t length = strlen(name);
    if (!length || length > 200 || name[0] == '.')
        return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '.' || ch == '_' || ch == '-'))
            return 0;
    }
    return 1;
}

static int upload_file(int fd, const char *name)
{
    FILE *file = fopen(name, "rb");
    if (!file) {
        perror("Local file");
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        puts("Cannot measure file size.");
        return 0;
    }

    long size = ftell(file);
    if (size < 0 || (unsigned long long)size > FILE_LIMIT) {
        fclose(file);
        puts("File must be at most 10 MiB.");
        return 0;
    }
    rewind(file);

    char header[512];
    snprintf(header, sizeof(header), "PUT %s %ld\n", name, size);
    if (send_all(fd, header, strlen(header)) < 0) {
        fclose(file);
        return -1;
    }

    long remaining = size;
    char buffer[4096];
    while (remaining > 0) {
        size_t chunk = remaining > (long)sizeof(buffer) ?
                       sizeof(buffer) : (size_t)remaining;
        size_t n = fread(buffer, 1, chunk, file);
        if (n != chunk || send_all(fd, buffer, n) < 0) {
            fclose(file);
            return -1;
        }
        remaining -= (long)n;
    }
    fclose(file);

    char response[8192];
    if (read_line(fd, response, sizeof(response)) < 0)
        return -1;
    printf("Agent: %s", response);
    return 0;
}

static int download_file(int fd, const char *name)
{
    char request[512], response[8192];
    snprintf(request, sizeof(request), "GET %s\n", name);
    if (send_all(fd, request, strlen(request)) < 0 ||
        read_line(fd, response, sizeof(response)) < 0)
        return -1;

    printf("Agent: %s", response);
    if (strncmp(response, "ERR ", 4) == 0)
        return 0;

    char received_name[201], sid[32], extra;
    unsigned long long size;
    if (sscanf(response, "OK FILE_SEND %200s %llu %31s %c",
               received_name, &size, sid, &extra) != 3 ||
        strcmp(received_name, name) != 0 ||
        strcmp(sid, "SID:3842") != 0 || size > FILE_LIMIT) {
        puts("Invalid file header.");
        return -1;
    }

    char target[256], temporary[256];
    snprintf(target, sizeof(target), "downloaded_%s", name);
    snprintf(temporary, sizeof(temporary), ".download-XXXXXX");

    int output = mkstemp(temporary);
    if (output < 0) {
        perror("Download file");
        return -1;
    }

    unsigned long long remaining = size;
    char buffer[4096];
    int failed = 0;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ?
                       sizeof(buffer) : (size_t)remaining;
        ssize_t n = recv(fd, buffer, chunk, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            failed = 1;
            break;
        }

        size_t written = 0;
        while (written < (size_t)n) {
            ssize_t count = write(output, buffer + written,
                                  (size_t)n - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                failed = 1;
                break;
            }
            written += (size_t)count;
        }
        if (failed)
            break;
        remaining -= (unsigned long long)n;
    }

    if (close(output) < 0)
        failed = 1;
    if (failed || rename(temporary, target) < 0) {
        unlink(temporary);
        puts("Download failed.");
        return -1;
    }

    printf("Saved %llu bytes as %s\n", size, target);
    return 0;
}

int main(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &server.sin_addr) != 1 ||
        connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    printf("RemoteOps Controller - IT24102483\n");
    printf("Connected to Agent on port %d\n", PORT);
    puts("File commands: PUT filename / GET filename");

    char command[1024], response[8192];
    while (1) {
        printf("remoteops> ");
        fflush(stdout);
        if (!fgets(command, sizeof(command), stdin))
            break;

        size_t length = strlen(command);
        if (!length || command[length - 1] != '\n') {
            puts("Command too long. Closing connection.");
            break;
        }
        command[--length] = '\0';
        if (length && command[length - 1] == '\r')
            command[--length] = '\0';
        if (!length)
            continue;

        int is_put = strcmp(command, "PUT") == 0 ||
                     strncmp(command, "PUT ", 4) == 0;
        int is_get = strcmp(command, "GET") == 0 ||
                     strncmp(command, "GET ", 4) == 0;

        if (is_put || is_get) {
            char name[201], extra;
            if (sscanf(command + 3, "%200s %c", name, &extra) != 1 ||
                !valid_filename(name)) {
                puts("Use PUT filename or GET filename.");
                puts("Filename: letters, numbers, dot, underscore or dash.");
                continue;
            }
            int result = is_put ? upload_file(sock, name) :
                                  download_file(sock, name);
            if (result < 0) {
                puts("Transfer failed. Closing connection.");
                break;
            }
            continue;
        }

        command[length] = '\n';
        command[length + 1] = '\0';
        if (send_all(sock, command, length + 1) < 0 ||
            read_line(sock, response, sizeof(response)) < 0) {
            puts("Agent disconnected or communication failed.");
            break;
        }
        printf("Agent: %s", response);
        if (strcmp(command, "QUIT\n") == 0)
            break;
    }

    close(sock);
    return 0;
}
