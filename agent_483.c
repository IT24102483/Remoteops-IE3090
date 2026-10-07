#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
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

/* Protect the shared log when several Controllers are connected. */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void log_event(int fd, const char *event, const char *detail)
{
    char address[INET_ADDRSTRLEN] = "local";
    unsigned int port = 0;
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);

    if (fd >= 0 &&
        getpeername(fd, (struct sockaddr *)&peer, &peer_size) == 0) {
        inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
        port = ntohs(peer.sin_port);
    }

    char clean[8192];
    size_t used = 0;
    while (detail[used] && used < sizeof(clean) - 1) {
        unsigned char ch = (unsigned char)detail[used];
        clean[used] = (ch < 32 || ch == 127) ? ' ' : (char)ch;
        used++;
    }
    clean[used] = '\0';

    time_t now = time(NULL);
    struct tm local;
    char timestamp[32] = "unknown-time";
    if (localtime_r(&now, &local))
        strftime(timestamp, sizeof(timestamp),
                 "%Y-%m-%d %H:%M:%S", &local);

    pthread_mutex_lock(&log_mutex);
    FILE *file = fopen("remoteops_IT24102483.log", "a");
    if (file) {
        fprintf(file,
                "[%s] IT24102483 peer=%s:%u fd=%d %s %s\n",
                timestamp, address, port, fd, event, clean);
        if (fclose(file) != 0)
            perror("Log close");
    } else {
        perror("Log open");
    }
    pthread_mutex_unlock(&log_mutex);
}

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
    log_event(fd, "RESPONSE", text);
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
static int build_sysinfo(char *response, size_t capacity)
{
    double cpu_load = 0.0;
    double uptime = 0.0;
    unsigned long total_kb = 0;
    unsigned long available_kb = 0;
    int found_total = 0;
    int found_available = 0;
    char line[256];

    FILE *file = fopen("/proc/loadavg", "r");
    if (!file)
        return -1;
    int valid = fscanf(file, "%lf", &cpu_load) == 1;
    fclose(file);
    if (!valid)
        return -1;

    file = fopen("/proc/uptime", "r");
    if (!file)
        return -1;
    valid = fscanf(file, "%lf", &uptime) == 1;
    fclose(file);
    if (!valid)
        return -1;

    file = fopen("/proc/meminfo", "r");
    if (!file)
        return -1;

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
        return -1;

    double used_mb = (total_kb - available_kb) / 1024.0;

    snprintf(response, capacity,
             "OK SYSINFO %.2f %.2f %.0f SID:3842\n",
             cpu_load, used_mb, uptime);

    return 0;
}



static int handle_sysinfo(int fd)
{
    char response[256];
    if (build_sysinfo(response, sizeof(response)) < 0)
        return send_response(fd, "ERR 006 SYSINFO_FAILED SID:3842\n");
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


/* Only fixed commands from this whitelist may be executed. */
static int handle_exec(int fd, const char *command)
{
    const char *program = NULL;

    if (strcmp(command, "EXEC DATE") == 0)
        program = "/usr/bin/date";
    else if (strcmp(command, "EXEC UPTIME") == 0)
        program = "/usr/bin/uptime";
    else if (strcmp(command, "EXEC DISKFREE") == 0)
        program = "/usr/bin/df -h /";
    else if (strcmp(command, "EXEC HOSTNAME") == 0)
        program = "/usr/bin/hostname";
    else if (strcmp(command, "EXEC WHOAMI") == 0)
        program = "/usr/bin/whoami";
    else
        return send_response(fd,
                             "ERR 002 COMMAND_NOT_ALLOWED SID:3842\n");

    FILE *pipe = popen(program, "r");
    if (!pipe)
        return send_response(fd,
                             "ERR 006 EXEC_FAILED SID:3842\n");

    char response[4096] = "OK EXEC_RESULT ";
    size_t used = strlen(response);
    const char *suffix = " SID:3842\n";
    int ch;
    int overflow = 0;

    while ((ch = fgetc(pipe)) != EOF) {
        if (used + strlen(suffix) + 1 >= sizeof(response)) {
            overflow = 1;
            continue;
        }

        if (ch < 32 || ch == 127)
            ch = ' ';
        response[used++] = (char)ch;
    }

    int read_failed = ferror(pipe);
    int status = pclose(pipe);

    if (read_failed || status != 0 || overflow)
        return send_response(fd,
                             "ERR 006 EXEC_FAILED SID:3842\n");

    while (used > strlen("OK EXEC_RESULT ") &&
           response[used - 1] == ' ')
        used--;

    strcpy(response + used, suffix);
    return send_response(fd, response);
}


/* Each Controller has its own socket and authentication state. */

#define FILE_LIMIT (10ULL * 1024 * 1024)
#define FILE_DIRECTORY "./agentfiles/IT24102483"

static int send_bytes(int fd, const void *data, size_t size)
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

/* Only a plain filename is allowed, without directory paths. */
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

static int handle_put(int fd, const char *command)
{
    char name[201], size_text[32], extra;
    if (sscanf(command, "PUT %200s %31s %c",
               name, size_text, &extra) != 2 ||
        !valid_filename(name) ||
        strspn(size_text, "0123456789") != strlen(size_text)) {
        send_response(fd, "ERR 003 INVALID_PUT SID:3842\n");
        return -1;
    }

    errno = 0;
    char *end;
    unsigned long long size = strtoull(size_text, &end, 10);
    if (errno || *end || size > FILE_LIMIT) {
        send_response(fd, "ERR 004 FILE_TOO_LARGE SID:3842\n");
        return -1;
    }

    char target[512];
    char temporary[512];
    snprintf(target, sizeof(target), "%s/%s", FILE_DIRECTORY, name);
    snprintf(temporary, sizeof(temporary),
             "%s/.upload-XXXXXX", FILE_DIRECTORY);

    int output = mkstemp(temporary);
    if (output < 0) {
        send_response(fd, "ERR 006 FILE_WRITE_FAILED SID:3842\n");
        return -1;
    }

    unsigned long long remaining = size;
    char buffer[4096];
    int failed = 0;

    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ?
                       sizeof(buffer) : (size_t)remaining;
        ssize_t received = recv(fd, buffer, chunk, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0) {
            failed = 1;
            break;
        }

        size_t written = 0;
        while (written < (size_t)received) {
            ssize_t n = write(output, buffer + written,
                              (size_t)received - written);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                failed = 1;
                break;
            }
            written += (size_t)n;
        }
        if (failed)
            break;
        remaining -= (unsigned long long)received;
    }

    if (close(output) < 0)
        failed = 1;

    if (failed) {
        unlink(temporary);
        send_response(fd, "ERR 006 FILE_TRANSFER_FAILED SID:3842\n");
        return -1;
    }

    if (rename(temporary, target) < 0) {
        unlink(temporary);
        return send_response(fd,
                             "ERR 006 FILE_WRITE_FAILED SID:3842\n");
    }

    char response[512];
    snprintf(response, sizeof(response),
             "OK FILE_RECEIVED %s SID:3842\n", name);
    return send_response(fd, response);
}

static int handle_get(int fd, const char *command)
{
    char name[201], extra;
    if (sscanf(command, "GET %200s %c", name, &extra) != 1 ||
        !valid_filename(name))
        return send_response(fd, "ERR 003 INVALID_GET SID:3842\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FILE_DIRECTORY, name);
    int input = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0)
        return send_response(fd, "ERR 005 FILE_NOT_FOUND SID:3842\n");

    struct stat info;
    if (fstat(input, &info) < 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0) {
        close(input);
        return send_response(fd, "ERR 005 FILE_NOT_FOUND SID:3842\n");
    }

    if ((unsigned long long)info.st_size > FILE_LIMIT) {
        close(input);
        return send_response(fd, "ERR 004 FILE_TOO_LARGE SID:3842\n");
    }

    char response[512];
    snprintf(response, sizeof(response),
             "OK FILE_SEND %s %llu SID:3842\n",
             name, (unsigned long long)info.st_size);

    if (send_response(fd, response) < 0) {
        close(input);
        return -1;
    }

    unsigned long long remaining = (unsigned long long)info.st_size;
    char buffer[4096];
    int result = 0;

    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buffer) ?
                       sizeof(buffer) : (size_t)remaining;
        ssize_t n = read(input, buffer, chunk);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || send_bytes(fd, buffer, (size_t)n) < 0) {
            result = -1;
            break;
        }
        remaining -= (unsigned long long)n;
    }

    close(input);
    return result;
}


/* Monitoring belongs to one authenticated TCP session. */
struct monitor_state {
    int udp_fd;
    int running;
    pthread_t thread;
    atomic_int stop;
    struct sockaddr_in destination;
};

static void *monitor_worker(void *argument)
{
    struct monitor_state *state = argument;

    while (!atomic_load(&state->stop)) {
        char response[256];

        if (build_sysinfo(response, sizeof(response)) == 0) {
            /* UDP format starts with SYSINFO, without TCP's OK. */
            const char *message = response + 3;
            sendto(state->udp_fd, message, strlen(message), 0,
                   (struct sockaddr *)&state->destination,
                   sizeof(state->destination));
        }

        /* Send every two seconds; check STOP every 100 ms. */
        for (int i = 0; i < 20; i++) {
            if (atomic_load(&state->stop))
                break;
            struct timespec delay = {0, 100000000L};
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
                if (atomic_load(&state->stop))
                    break;
            }
        }
    }
    return NULL;
}

static void stop_monitor(struct monitor_state *state)
{
    if (!state->running)
        return;

    atomic_store(&state->stop, 1);
    pthread_join(state->thread, NULL);
    close(state->udp_fd);
    state->udp_fd = -1;
    state->running = 0;
}

static int handle_monitor(int fd, const char *command,
                          struct monitor_state *state)
{
    if (strcmp(command, "MONITOR STOP") == 0) {
        stop_monitor(state);
        return send_response(fd, "OK MONITOR_STOPPED SID:3842\n");
    }

    char port_text[16], extra;
    if (sscanf(command, "MONITOR START %15s %c",
               port_text, &extra) != 1 ||
        strspn(port_text, "0123456789") != strlen(port_text))
        return send_response(fd, "ERR 003 INVALID_MONITOR SID:3842\n");

    errno = 0;
    char *end;
    unsigned long port = strtoul(port_text, &end, 10);
    if (errno || *end || port == 0 || port > 65535)
        return send_response(fd, "ERR 003 INVALID_MONITOR SID:3842\n");

    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &peer_size) < 0)
        return send_response(fd, "ERR 006 MONITOR_FAILED SID:3842\n");

    stop_monitor(state);

    state->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->udp_fd < 0)
        return send_response(fd, "ERR 006 MONITOR_FAILED SID:3842\n");

    state->destination = peer;
    state->destination.sin_port = htons((unsigned short)port);
    atomic_store(&state->stop, 0);

    int error = pthread_create(&state->thread, NULL,
                               monitor_worker, state);
    if (error != 0) {
        close(state->udp_fd);
        state->udp_fd = -1;
        return send_response(fd, "ERR 006 MONITOR_FAILED SID:3842\n");
    }

    state->running = 1;
    return send_response(fd, "OK MONITOR_STARTED SID:3842\n");
}

static void *handle_client(void *argument)
{
    int client_fd = *(int *)argument;
    free(argument);

    struct monitor_state monitor = {0};
    monitor.udp_fd = -1;
    atomic_init(&monitor.stop, 0);

        printf("Controller connected.\n");
        log_event(client_fd, "CONNECT", "TCP session opened");
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
            if (strncmp(command, "AUTH", 4) == 0)
                log_event(client_fd, "COMMAND", "AUTH [token omitted]");
            else
                log_event(client_fd, "COMMAND", command);


            if (!authenticated) {
                if (strcmp(command, AUTH_COMMAND) == 0) {
                    authenticated = 1;
                    log_event(client_fd, "AUTH", "SUCCESS");
                    if (send_response(client_fd,
                                      "OK AUTHENTICATED SID:3842\n") < 0)
                        break;
                } else {
                    send_response(client_fd,
                                  "ERR 001 AUTH_FAILED SID:3842\n");
                    log_event(client_fd, "AUTH", "FAILED");
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

            if (strcmp(command, "EXEC") == 0 ||
                strncmp(command, "EXEC ", 5) == 0) {
                if (handle_exec(client_fd, command) < 0)
                    break;
                continue;
            }

            if (strcmp(command, "PUT") == 0 ||
                strncmp(command, "PUT ", 4) == 0) {
                if (handle_put(client_fd, command) < 0)
                    break;
                continue;
            }

            if (strcmp(command, "GET") == 0 ||
                strncmp(command, "GET ", 4) == 0) {
                if (handle_get(client_fd, command) < 0)
                    break;
                continue;
            }


            if (strcmp(command, "MONITOR") == 0 ||
                strncmp(command, "MONITOR ", 8) == 0) {
                if (handle_monitor(client_fd, command, &monitor) < 0)
                    break;
                continue;
            }

            if (strcmp(command, "QUIT") == 0) {
                stop_monitor(&monitor);
                send_response(client_fd, "OK BYE SID:3842\n");
                break;
            }

            if (send_response(client_fd,
                              "ERR 003 UNKNOWN_COMMAND SID:3842\n") < 0)
                break;
        }

        stop_monitor(&monitor);
        log_event(client_fd, "DISCONNECT", "TCP session ended");
        close(client_fd);
        printf("Controller disconnected.\n");

    return NULL;
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
    log_event(-1, "START", "Agent listening on TCP port 9410");

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }

        int *client_socket = malloc(sizeof(*client_socket));
        if (!client_socket) {
            perror("malloc");
            close(client_fd);
            continue;
        }

        *client_socket = client_fd;
        pthread_t thread;
        int error = pthread_create(&thread, NULL,
                                   handle_client, client_socket);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            free(client_socket);
            close(client_fd);
            continue;
        }

        pthread_detach(thread);
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
