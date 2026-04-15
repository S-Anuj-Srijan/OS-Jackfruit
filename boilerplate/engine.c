/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int stop_requested;
    int client_fd; // For CMD_RUN waiting socket
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    
    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

void *producer_thread(void *arg) {
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;
    while ((n = read(ctx->read_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        strncpy(item.container_id, ctx->container_id, sizeof(item.container_id)-1);
        item.container_id[CONTAINER_ID_LEN-1] = '\0';
        item.length = n;
        memcpy(item.data, buf, n);
        bounded_buffer_push(ctx->buffer, &item);
    }
    close(ctx->read_fd);
    free(ctx);
    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    
    if (cfg->log_write_fd > 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    if (cfg->nice_value != 0) {
        nice(cfg->nice_value);
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        // ignore for environments lacking fine mount namespace isolation but try
    }
    
    if (mount(cfg->rootfs, cfg->rootfs, "bind", MS_BIND | MS_REC, NULL) < 0) {
        perror("mount bind rootfs");
        return 1;
    }
    
    if (chdir(cfg->rootfs) < 0) {
        perror("chdir");
        return 1;
    }
    
#ifdef SYS_pivot_root
    // Modern pivot_root requires a new empty dir for old_root
    if (mkdir("old_root", 0755) < 0 && errno != EEXIST) {
        perror("mkdir old_root");
    }
    if (syscall(SYS_pivot_root, ".", "old_root") < 0) {
        // pivot_root can fail on some setups, fallback to chroot
        if (chroot(".") < 0) {
            perror("chroot (fallback from pivot_root)");
            return 1;
        }
    } else {
        chdir("/");
        umount2("/old_root", MNT_DETACH);
        rmdir("/old_root");
    }
#else
    if (chroot(".") < 0) {
        perror("chroot");
        return 1;
    }
#endif

    if (mkdir("/proc", 0755) < 0 && errno != EEXIST) {}
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount proc");
    }

    char *exec_args[] = {"/bin/sh", "-c", cfg->command, NULL};
    execvp(exec_args[0], exec_args);
    perror("execvp");
    return 1;
}

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid,
                          unsigned long soft_limit_bytes, unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) return -1;
    return 0;
}

static supervisor_ctx_t *global_ctx = NULL;
static void sigterm_handler(int sig) {
    if (global_ctx) global_ctx->should_stop = 1;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    global_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc; perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        return 1;
    }

    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGCHLD, SIG_IGN); // We will use waitpid loop, wait, we don't ignore it to allow waitpid

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 10) < 0) {
        perror("listen"); return 1;
    }

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    while (!ctx.should_stop) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *rec = ctx.containers;
            while (rec) {
                if (rec->host_pid == pid && (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING)) {
                    if (WIFEXITED(status)) {
                        rec->exit_code = WEXITSTATUS(status);
                        rec->state = CONTAINER_EXITED;
                    } else if (WIFSIGNALED(status)) {
                        rec->exit_signal = WTERMSIG(status);
                        if (rec->exit_signal == SIGKILL && !rec->stop_requested) {
                            rec->state = CONTAINER_KILLED;
                        } else {
                            rec->state = CONTAINER_STOPPED;
                        }
                    }
                    if (ctx.monitor_fd >= 0) {
                        unregister_from_monitor(ctx.monitor_fd, rec->id, rec->host_pid);
                    }
                }
                rec = rec->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        struct pollfd pfd;
        pfd.fd = ctx.server_fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 100) > 0) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                control_request_t req;
                if (read(client_fd, &req, sizeof(req)) == sizeof(req)) {
                    control_response_t res;
                    memset(&res, 0, sizeof(res));
                    
                    if (req.kind == CMD_START || req.kind == CMD_RUN) {
                        int pipefd[2];
                        pipe(pipefd);
                        
                        child_config_t *cfg = malloc(sizeof(child_config_t));
                        strncpy(cfg->id, req.container_id, sizeof(cfg->id)-1);
                        strncpy(cfg->rootfs, req.rootfs, sizeof(cfg->rootfs)-1);
                        strncpy(cfg->command, req.command, sizeof(cfg->command)-1);
                        cfg->nice_value = req.nice_value;
                        cfg->log_write_fd = pipefd[1];
                        
                        char *stack = malloc(STACK_SIZE);
                        pid_t child_pid = clone(child_fn, stack + STACK_SIZE, SIGCHLD | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS, cfg);
                        close(pipefd[1]);
                        
                        if (child_pid > 0) {
                            producer_ctx_t *pctx = malloc(sizeof(producer_ctx_t));
                            pctx->read_fd = pipefd[0];
                            strncpy(pctx->container_id, req.container_id, sizeof(pctx->container_id)-1);
                            pctx->buffer = &ctx.log_buffer;
                            
                            pthread_t pt;
                            pthread_create(&pt, NULL, producer_thread, pctx);
                            pthread_detach(pt);

                            pthread_mutex_lock(&ctx.metadata_lock);
                            container_record_t *rec = malloc(sizeof(*rec));
                            memset(rec, 0, sizeof(*rec));
                            strncpy(rec->id, req.container_id, sizeof(rec->id)-1);
                            rec->host_pid = child_pid;
                            rec->started_at = time(NULL);
                            rec->state = CONTAINER_RUNNING;
                            rec->soft_limit_bytes = req.soft_limit_bytes;
                            rec->hard_limit_bytes = req.hard_limit_bytes;
                            rec->client_fd = -1;
                            snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req.container_id);
                            rec->next = ctx.containers;
                            ctx.containers = rec;
                            
                            if (ctx.monitor_fd >= 0) {
                                register_with_monitor(ctx.monitor_fd, rec->id, child_pid, rec->soft_limit_bytes, rec->hard_limit_bytes);
                            }

                            if (req.kind == CMD_START) {
                                res.status = 0;
                                snprintf(res.message, sizeof(res.message), "Container %s started\n", req.container_id);
                                write(client_fd, &res, sizeof(res));
                                close(client_fd);
                            } else {
                                rec->client_fd = client_fd;
                            }
                            pthread_mutex_unlock(&ctx.metadata_lock);
                        } else {
                            res.status = 1;
                            snprintf(res.message, sizeof(res.message), "Failed to start container\n");
                            write(client_fd, &res, sizeof(res));
                            close(pipefd[0]);
                            close(client_fd);
                            free(stack);
                            free(cfg);
                        }
                    } else if (req.kind == CMD_PS) {
                        pthread_mutex_lock(&ctx.metadata_lock);
                        container_record_t *rec = ctx.containers;
                        int written = 0;
                        while (rec && written < (int)sizeof(res.message) - 64) {
                            written += snprintf(res.message + written, sizeof(res.message) - written,
                                     "ID: %s\tPID: %d\tState: %s\n", rec->id, rec->host_pid, state_to_string(rec->state));
                            rec = rec->next;
                        }
                        pthread_mutex_unlock(&ctx.metadata_lock);
                        res.status = 0;
                        write(client_fd, &res, sizeof(res));
                        close(client_fd);
                    } else if (req.kind == CMD_STOP) {
                        pthread_mutex_lock(&ctx.metadata_lock);
                        container_record_t *rec = ctx.containers;
                        int found = 0;
                        while (rec) {
                            if (strcmp(rec->id, req.container_id) == 0 && (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING)) {
                                rec->stop_requested = 1;
                                kill(rec->host_pid, SIGTERM);
                                found = 1;
                                break;
                            }
                            rec = rec->next;
                        }
                        pthread_mutex_unlock(&ctx.metadata_lock);
                        res.status = found ? 0 : 1;
                        snprintf(res.message, sizeof(res.message), found ? "Stopped\n" : "Not Found\n");
                        write(client_fd, &res, sizeof(res));
                        close(client_fd);
                    } else if (req.kind == CMD_LOGS) {
                        res.status = 0;
                        snprintf(res.message, sizeof(res.message), "Log path: %s/%s.log (not implemented via socket)\n", LOG_DIR, req.container_id);
                        write(client_fd, &res, sizeof(res));
                        close(client_fd);
                    } else {
                        close(client_fd);
                    }
                } else {
                    close(client_fd);
                }
            }
        }

        pthread_mutex_lock(&ctx.metadata_lock);
        container_record_t *rec = ctx.containers;
        while(rec) {
            if (rec->client_fd > 0 && rec->state != CONTAINER_RUNNING && rec->state != CONTAINER_STARTING) {
                control_response_t res;
                memset(&res, 0, sizeof(res));
                res.status = rec->exit_code;
                snprintf(res.message, sizeof(res.message), "Container %s exited (status %d, signal %d)\n",
                         rec->id, rec->exit_code, rec->exit_signal);
                write(rec->client_fd, &res, sizeof(res));
                close(rec->client_fd);
                rec->client_fd = -1;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *rec = ctx.containers;
    while(rec) {
        if (rec->state == CONTAINER_RUNNING) kill(rec->host_pid, SIGKILL);
        container_record_t *next = rec->next;
        free(rec);
        rec = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);
    bounded_buffer_destroy(&ctx.log_buffer);
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket"); return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (supervisor not running?)");
        close(sock);
        return 1;
    }
    
    if (write(sock, req, sizeof(*req)) != sizeof(*req)) {
        perror("write request");
        close(sock);
        return 1;
    }
    
    control_response_t res;
    if (read(sock, &res, sizeof(res)) <= 0) {
        close(sock);
        return 1;
    }
    close(sock);
    
    if (req->kind == CMD_LOGS) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char buf[1024];
            ssize_t n;
            while((n = read(fd, buf, sizeof(buf))) > 0) {
                write(STDOUT_FILENO, buf, n);
            }
            close(fd);
        } else {
            printf("No logs found for container %s\n", req->container_id);
        }
        return res.status;
    }
    
    printf("%s", res.message);
    return res.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    
    // forward signals implicitly by waiting socket
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]); return 1;
    }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]); return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0) return cmd_ps();
    if (strcmp(argv[1], "logs") == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0) return cmd_stop(argc, argv);
    usage(argv[0]); return 1;
}
