#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#define LINE_SIZE 1024
#define MAX_ARGS 64
#define MAX_CMDS 16
#define HISTORY_SIZE 100
#define BUFFER_SIZE 4096
// One structure is enough to describe one command.
typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *input_file;
    char *output_file;
    int append;
} Cmd;
static char shell_root[PATH_MAX];
static size_t root_len;
static char history[HISTORY_SIZE][LINE_SIZE];
static int history_total = 0;

// 1. VIRTUAL ROOT
static int inside_root(const char *path) {
    if (strcmp(shell_root, "/") == 0) return 1;
    return strncmp(path, shell_root, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/');
}

static const char *shown_path(const char *real_path) {
    if (!inside_root(real_path)) return real_path;
    if (strcmp(real_path, shell_root) == 0) return "/";
    return real_path + root_len;
}
// Convert a path typed in our shell to a real path on the computer.
static int resolve_path(const char *name, char *out) {
    char cwd[PATH_MAX], virtual_name[PATH_MAX * 2], work[PATH_MAX * 2];
    char *parts[MAX_ARGS * 4], *part, *save;
    int count = 0;
    if (!name || !*name || strcmp(name, "~") == 0) name = "/";
    if (strncmp(name, "~/", 2) == 0) name++;
    if (name[0] == '/') {
        if (snprintf(virtual_name, sizeof(virtual_name), "%s", name) >= (int)sizeof(virtual_name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        if (!getcwd(cwd, sizeof(cwd)) || !inside_root(cwd)) {
            errno = EPERM;
            return -1;
        }
        const char *current = shown_path(cwd);
        if (snprintf(virtual_name, sizeof(virtual_name), "%s/%s",
                     strcmp(current, "/") == 0 ? "" : current, name) >= (int)sizeof(virtual_name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }
    snprintf(work, sizeof(work), "%s", virtual_name);
    for (part = strtok_r(work, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            if (count > 0) count--;       // cd .. at / stays at /
            continue;
        }
        if (count >= (int)(sizeof(parts) / sizeof(parts[0]))) {
            errno = ENAMETOOLONG;
            return -1;
        }
        parts[count++] = part;
    }
    snprintf(out, PATH_MAX, "%s", shell_root);
    for (int i = 0; i < count; i++) {
        size_t used = strlen(out);
        size_t need = strlen(parts[i]) + 2;
        if (used + need >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (strcmp(out, "/") != 0) strcat(out, "/");
        strcat(out, parts[i]);
    }
    // Check the deepest existing parent so symlinks cannot escape root.
    char check[PATH_MAX], real[PATH_MAX];
    snprintf(check, sizeof(check), "%s", out);
    while (1) {
        if (realpath(check, real)) {
            if (!inside_root(real)) {
                errno = EPERM;
                return -1;
            }
            return 0;
        }
        if (errno != ENOENT && errno != ENOTDIR) return -1;
        char *slash = strrchr(check, '/');
        if (!slash) return -1;
        if (slash == check) check[1] = '\0';
        else *slash = '\0';
    }
}

static int safe_path(const char *command, const char *name, char *out) {
    if (resolve_path(name, out) == 0) return 0;
    if (errno == EPERM) fprintf(stderr, "%s: path is outside the shell root\n", command);
    else fprintf(stderr, "%s: %s\n", command, strerror(errno));
    return -1;
}

// 2. HISTORY
static void add_history(const char *line) {
    snprintf(history[history_total % HISTORY_SIZE], LINE_SIZE, "%s", line);
    history_total++;
}

static int command_history(void) {
    int start = history_total > HISTORY_SIZE ? history_total - HISTORY_SIZE : 0;
    for (int i = start; i < history_total; i++)
        printf("%d  %s\n", i + 1, history[i % HISTORY_SIZE]);
    return 0;
}

// 3. COMMAND IMPLEMENTATIONS
static int command_pwd(void) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("pwd");
        return 1;
    }
    printf("%s\n", shown_path(cwd));
    return 0;
}

static void print_permissions(mode_t mode) {
    const mode_t bits[] = {
        S_IRUSR, S_IWUSR, S_IXUSR,
        S_IRGRP, S_IWGRP, S_IXGRP,
        S_IROTH, S_IWOTH, S_IXOTH
    };
    const char letters[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) putchar((mode & bits[i]) ? letters[i] : '-');
}

static int command_ls(char **args, int argc) {
    int long_list = 0;
    const char *name = ".";
    if (argc > 3) {
        fprintf(stderr, "ls: too many arguments\n");
        return 1;
    }
    if (argc >= 2) {
        if (strcmp(args[1], "-l") == 0) {
            long_list = 1;
            if (argc == 3) name = args[2];
        } else name = args[1];
    }
    char path[PATH_MAX];
    if (safe_path("ls", name, path)) return 1;
    DIR *dir = opendir(path);
    if (!dir) {
        perror("ls");
        return 1;
    }
    struct dirent *entry;
    int status = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!long_list) {
            printf("%s  ", entry->d_name);
            continue;
        }
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", path, entry->d_name) >= (int)sizeof(full)) {
            fprintf(stderr, "ls: path too long\n");
            status = 1;
            continue;
        }
        struct stat info;
        if (lstat(full, &info)) {
            perror("ls");
            status = 1;
            continue;
        }
        putchar(S_ISDIR(info.st_mode) ? 'd' : S_ISLNK(info.st_mode) ? 'l' : '-');
        print_permissions(info.st_mode);
        printf(" %8lld %s\n", (long long)info.st_size, entry->d_name);
    }
    if (!long_list) putchar('\n');
    if (closedir(dir)) {
        perror("ls");
        status = 1;
    }
    return status;
}

static int command_cd(char **args, int argc) {
    if (argc > 2) {
        fprintf(stderr, "cd: too many arguments\n");
        return 1;
    }
    const char *name = argc == 1 ? "/" : args[1];
    char path[PATH_MAX];
    if (safe_path("cd", name, path)) return 1;
    struct stat info;
    if (stat(path, &info)) {
        perror("cd");
        return 1;
    }
    if (!S_ISDIR(info.st_mode)) {
        fprintf(stderr, "cd: not a directory\n");
        return 1;
    }
    if (chdir(path)) {
        perror("cd");
        return 1;
    }
    return 0;
}

static int command_whoami(void) {
    struct passwd *user = getpwuid(getuid());
    if (!user) {
        perror("whoami");
        return 1;
    }
    puts(user->pw_name);
    return 0;
}

static int command_uname(void) {
    struct utsname info;
    if (uname(&info)) {
        perror("uname");
        return 1;
    }
    printf("System Name : %s\n", info.sysname);
    printf("Node Name   : %s\n", info.nodename);
    printf("Release     : %s\n", info.release);
    printf("Version     : %s\n", info.version);
    printf("Machine     : %s\n", info.machine);
    return 0;
}

static int command_mkdir(char **args, int argc) {
    if (argc != 2) {
        fprintf(stderr, "mkdir: expected one directory name\n");
        return 1;
    }
    char path[PATH_MAX];
    if (safe_path("mkdir", args[1], path)) return 1;
    if (mkdir(path, 0755)) {
        perror("mkdir");
        return 1;
    }
    return 0;
}

static int command_touch(char **args, int argc) {
    if (argc != 2) {
        fprintf(stderr, "touch: expected one file name\n");
        return 1;
    }
    char path[PATH_MAX];
    if (safe_path("touch", args[1], path)) return 1;
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        perror("touch");
        return 1;
    }
    close(fd);
    if (utimensat(AT_FDCWD, path, NULL, 0)) {
        perror("touch");
        return 1;
    }
    return 0;
}

static int command_rm(char **args, int argc) {
    if (argc != 2) {
        fprintf(stderr, "rm: expected one file or empty directory\n");
        return 1;
    }
    char path[PATH_MAX];
    if (safe_path("rm", args[1], path)) return 1;
    if (!strcmp(path, shell_root)) {
        fprintf(stderr, "rm: cannot remove the shell root\n");
        return 1;
    }
    struct stat info;
    if (lstat(path, &info)) {
        perror("rm");
        return 1;
    }
    int result = S_ISDIR(info.st_mode) ? rmdir(path) : unlink(path);
    if (result) {
        perror("rm");
        return 1;
    }
    return 0;
}

static int copy_data(int input, int output) {
    char buffer[BUFFER_SIZE];
    ssize_t n;
    while ((n = read(input, buffer, sizeof(buffer))) > 0) {
        ssize_t written_total = 0;
        while (written_total < n) {
            ssize_t written = write(output, buffer + written_total, n - written_total);
            if (written <= 0) return -1;
            written_total += written;
        }
    }
    return n < 0 ? -1 : 0;
}

static int command_cp(char **args, int argc) {
    if (argc != 3) {
        fprintf(stderr, "cp: usage cp source destination\n");
        return 1;
    }
    char source[PATH_MAX], destination[PATH_MAX];
    if (safe_path("cp", args[1], source) || safe_path("cp", args[2], destination)) return 1;
    struct stat src_info, dst_info;
    if (stat(source, &src_info)) {
        perror("cp");
        return 1;
    }
    if (!S_ISREG(src_info.st_mode)) {
        fprintf(stderr, "cp: source must be a regular file\n");
        return 1;
    }
    if (!stat(destination, &dst_info) &&
        src_info.st_dev == dst_info.st_dev && src_info.st_ino == dst_info.st_ino) {
        fprintf(stderr, "cp: source and destination are the same file\n");
        return 1;
    }
    int input = open(source, O_RDONLY);
    if (input < 0) {
        perror("cp");
        return 1;
    }
    int output = open(destination, O_CREAT | O_WRONLY | O_TRUNC, src_info.st_mode & 0777);
    if (output < 0) {
        perror("cp");
        close(input);
        return 1;
    }
    int status = 0;
    if (copy_data(input, output)) {
        perror("cp");
        status = 1;
    }
    close(input);
    close(output);
    return status;
}

static int command_mv(char **args, int argc) {
    if (argc != 3) {
        fprintf(stderr, "mv: usage mv source destination\n");
        return 1;
    }
    char source[PATH_MAX], destination[PATH_MAX];
    if (safe_path("mv", args[1], source) || safe_path("mv", args[2], destination)) return 1;
    if (!strcmp(source, shell_root)) {
        fprintf(stderr, "mv: cannot move the shell root\n");
        return 1;
    }
    if (rename(source, destination)) {
        perror("mv");
        return 1;
    }
    return 0;
}

static int command_cat(char **args, int argc) {
    if (argc > 2) {
        fprintf(stderr, "cat: expected zero or one file\n");
        return 1;
    }
    int input = STDIN_FILENO;
    char path[PATH_MAX];
    if (argc == 2) {
        if (safe_path("cat", args[1], path)) return 1;
        input = open(path, O_RDONLY);
        if (input < 0) {
            perror("cat");
            return 1;
        }
    }
    int status = copy_data(input, STDOUT_FILENO) ? 1 : 0;
    if (status) perror("cat");
    if (argc == 2) close(input);
    return status;
}

static int command_echo(char **args, int argc) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putchar(' ');
        if (args[i][0] == '$') {
            const char *value = getenv(args[i] + 1);
            if (value) fputs(value, stdout);
        } else fputs(args[i], stdout);
    }
    putchar('\n');
    return 0;
}

static int read_small_file(const char *name, char *buffer, size_t size) {
    int fd = open(name, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t total = 0, n = 0;
    while (total < (ssize_t)size - 1 &&
           (n = read(fd, buffer + total, size - 1 - total)) > 0)
        total += n;
    close(fd);
    if (n < 0) return -1;
    buffer[total] = '\0';
    return 0;
}

static int command_uptime(void) {
    char buffer[256];
    double up, load1, load5, load15;
    if (read_small_file("/proc/uptime", buffer, sizeof(buffer)) || sscanf(buffer, "%lf", &up) != 1) {
        perror("uptime");
        return 1;
    }
    long total = (long)up;
    printf("Uptime: %ld days, %ld hours, %ld minutes, %ld seconds\n",
           total / 86400, (total % 86400) / 3600, (total % 3600) / 60, total % 60);
    if (read_small_file("/proc/loadavg", buffer, sizeof(buffer)) ||
        sscanf(buffer, "%lf %lf %lf", &load1, &load5, &load15) != 3) {
        perror("uptime");
        return 1;
    }
    printf("Load average: %.2f %.2f %.2f\n", load1, load5, load15);
    return 0;
}

static int command_meminfo(void) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) {
        perror("meminfo");
        return 1;
    }
    const char *wanted[] = {
        "MemTotal:", "MemFree:", "MemAvailable:",
        "Buffers:", "Cached:", "SwapTotal:", "SwapFree:"
    };
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        for (int i = 0; i < 7; i++) {
            if (!strncmp(line, wanted[i], strlen(wanted[i]))) {
                fputs(line, stdout);
                break;
            }
        }
    }
    fclose(file);
    return 0;
}

static int command_help(void) {
    puts("Custom Shell Commands");
    puts("  pwd, ls [-l] [dir], cd [dir], whoami, uname");
    puts("  mkdir dir, touch file, rm path, cp src dst, mv src dst");
    puts("  cat [file], echo text/$VAR, uptime, meminfo");
    puts("  help, history, exit [status]");
    puts("  cmd1 | cmd2 | cmd3       multiple pipes");
    puts("  command < file           input redirection");
    puts("  command > file           output redirection");
    puts("  command >> file          append redirection");
    puts("  command &                background execution");
    puts("  / is the folder where myshell was started");
    return 0;
}

// 4. ONE COMMAND DISPATCHER
static int parent_builtin(const char *name) {
    return !strcmp(name, "cd") || !strcmp(name, "echo") ||
           !strcmp(name, "help") || !strcmp(name, "history") ||
           !strcmp(name, "exit");
}
// Return -1 only when the command should be handled by execvp().
static int run_command(char **args, int argc) {
    if (!strcmp(args[0], "pwd")) return command_pwd();
    if (!strcmp(args[0], "ls")) return command_ls(args, argc);
    if (!strcmp(args[0], "cd")) return command_cd(args, argc);
    if (!strcmp(args[0], "whoami")) return command_whoami();
    if (!strcmp(args[0], "uname")) return command_uname();
    if (!strcmp(args[0], "mkdir")) return command_mkdir(args, argc);
    if (!strcmp(args[0], "touch")) return command_touch(args, argc);
    if (!strcmp(args[0], "rm")) return command_rm(args, argc);
    if (!strcmp(args[0], "cp")) return command_cp(args, argc);
    if (!strcmp(args[0], "mv")) return command_mv(args, argc);
    if (!strcmp(args[0], "cat")) return command_cat(args, argc);
    if (!strcmp(args[0], "echo")) return command_echo(args, argc);
    if (!strcmp(args[0], "uptime")) return command_uptime();
    if (!strcmp(args[0], "meminfo")) return command_meminfo();
    if (!strcmp(args[0], "help")) return command_help();
    if (!strcmp(args[0], "history")) return command_history();
    return -1;
}

// 5. TOKENIZER AND PARSER
// Split the line into words and the operators | < > >> &.
static int tokenize(const char *line, char *tokens[], char storage[]) {
    const char *p = line;
    char *out = storage, *end = storage + LINE_SIZE;
    int count = 0;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (count >= MAX_ARGS * 4 - 1) return -2;
        tokens[count++] = out;
        if (strchr("|<>&", *p)) {
            if (out + 3 >= end) return -3;
            if (*p == '>' && p[1] == '>') {
                *out++ = '>'; *out++ = '>'; p += 2;
            } else *out++ = *p++;
            *out++ = '\0';
            continue;
        }
        char quote = 0;
        while (*p) {
            if (!quote && (isspace((unsigned char)*p) || strchr("|<>&", *p))) break;
            if (*p == '\'' || *p == '"') {
                if (!quote) { quote = *p++; continue; }
                if (quote == *p) { quote = 0; p++; continue; }
            }
            if (out + 2 >= end) return -3;
            if (*p == '\\' && p[1]) { p++; *out++ = *p++; }
            else *out++ = *p++;
        }
        if (quote) return -1;
        *out++ = '\0';
    }
    tokens[count] = NULL;
    return count;
}

static int operator_token(const char *s) {
    return !strcmp(s, "|") || !strcmp(s, "<") || !strcmp(s, ">") ||
           !strcmp(s, ">>") || !strcmp(s, "&");
}

static int parse(char *tokens[], int count, Cmd commands[], int *ncmd, int *background) {
    memset(commands, 0, sizeof(Cmd) * MAX_CMDS);
    *ncmd = 0;
    *background = 0;
    int current = 0;
    for (int i = 0; i < count; i++) {
        Cmd *cmd = &commands[current];
        if (!strcmp(tokens[i], "|")) {
            if (cmd->argc == 0 || current + 1 >= MAX_CMDS) return -1;
            cmd->argv[cmd->argc] = NULL;
            current++;
            continue;
        }
        if (!strcmp(tokens[i], "<") || !strcmp(tokens[i], ">") || !strcmp(tokens[i], ">>")) {
            char *op = tokens[i];
            if (i + 1 >= count || operator_token(tokens[i + 1])) return -1;
            if (!strcmp(op, "<")) {
                if (cmd->input_file) return -1;
                cmd->input_file = tokens[++i];
            } else {
                if (cmd->output_file) return -1;
                cmd->append = !strcmp(op, ">>");
                cmd->output_file = tokens[++i];
            }
            continue;
        }
        if (!strcmp(tokens[i], "&")) {
            if (i != count - 1 || cmd->argc == 0) return -1;
            *background = 1;
            continue;
        }
        if (cmd->argc >= MAX_ARGS - 1) return -1;
        cmd->argv[cmd->argc++] = tokens[i];
    }
    if (count == 0) return 0;
    if (commands[current].argc == 0) return -1;
    for (int i = 0; i <= current; i++)
        commands[i].argv[commands[i].argc] = NULL;
    *ncmd = current + 1;
    return 0;
}

// 6. REDIRECTION AND PROCESS CONTROL
static int redirect(Cmd *cmd) {
    char path[PATH_MAX];
    if (cmd->input_file) {
        if (safe_path("redirection", cmd->input_file, path)) return -1;
        int fd = open(path, O_RDONLY);
        if (fd < 0) { perror(cmd->input_file); return -1; }
        if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    }
    if (cmd->output_file) {
        if (safe_path("redirection", cmd->output_file, path)) return -1;
        int flags = O_CREAT | O_WRONLY | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(path, flags, 0644);
        if (fd < 0) { perror(cmd->output_file); return -1; }
        if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    }
    return 0;
}
// This supports the assignment root idea for obvious external path arguments.
static int protect_external_paths(char **args) {
    static char converted[MAX_ARGS][PATH_MAX];
    for (int i = 1; args[i]; i++) {
        if (args[i][0] == '-') continue;
        if (args[i][0] == '/' || args[i][0] == '~' || !strcmp(args[i], "..") ||
            !strncmp(args[i], "../", 3) || strstr(args[i], "/..")) {
            if (safe_path(args[0], args[i], converted[i])) return -1;
            args[i] = converted[i];
        }
    }
    return 0;
}

static void child_run(Cmd *cmd) {
    if (redirect(cmd)) _exit(1);
    int status = run_command(cmd->argv, cmd->argc);
    if (status != -1) {
        fflush(NULL);
        _exit(status);
    }
    if (protect_external_paths(cmd->argv)) _exit(1);
    execvp(cmd->argv[0], cmd->argv);
    if (errno == ENOENT) fprintf(stderr, "%s: command not found\n", cmd->argv[0]);
    else fprintf(stderr, "%s: %s\n", cmd->argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static int execute(Cmd commands[], int ncmd, int background) {
    // Parent built-ins must stay in this process when not piped/backgrounded.
    if (ncmd == 1 && !background && parent_builtin(commands[0].argv[0])) {
        if (!strcmp(commands[0].argv[0], "exit")) return 0;
        int saved_in = dup(STDIN_FILENO);
        int saved_out = dup(STDOUT_FILENO);
        if (saved_in < 0 || saved_out < 0) {
            perror("dup");
            return 1;
        }
        int status = 1;
        if (!redirect(&commands[0])) status = run_command(commands[0].argv, commands[0].argc);
        fflush(stdout);
        if (dup2(saved_in, STDIN_FILENO) < 0 || dup2(saved_out, STDOUT_FILENO) < 0) {
            perror("dup2");
            status = 1;
        }
        close(saved_in);
        close(saved_out);
        return status;
    }
    pid_t pids[MAX_CMDS];
    int previous_read = -1;
    int created = 0;
    for (int i = 0; i < ncmd; i++) {
        int pfd[2] = {-1, -1};
        if (i < ncmd - 1 && pipe(pfd)) {
            perror("pipe");
            break;
        }
        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            if (pfd[0] != -1) close(pfd[0]);
            if (pfd[1] != -1) close(pfd[1]);
            break;
        }
        if (child == 0) {
            signal(SIGINT, SIG_DFL);
            if (previous_read != -1 && dup2(previous_read, STDIN_FILENO) < 0) {
                perror("dup2");
                _exit(1);
            }
            if (i < ncmd - 1 && dup2(pfd[1], STDOUT_FILENO) < 0) {
                perror("dup2");
                _exit(1);
            }
            if (previous_read != -1) close(previous_read);
            if (pfd[0] != -1) close(pfd[0]);
            if (pfd[1] != -1) close(pfd[1]);
            child_run(&commands[i]);
        }
        pids[created++] = child;
        if (previous_read != -1) close(previous_read);
        if (i < ncmd - 1) {
            close(pfd[1]);
            previous_read = pfd[0];
        } else previous_read = -1;
    }
    if (previous_read != -1) close(previous_read);
    if (created != ncmd) {
        for (int i = 0; i < created; i++) waitpid(pids[i], NULL, 0);
        return 1;
    }
    if (background) {
        printf("[background pid %d]\n", (int)pids[created - 1]);
        return 0;
    }
    int final_status = 0;
    for (int i = 0; i < created; i++) {
        int status;
        while (waitpid(pids[i], &status, 0) < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            return 1;
        }
        if (i == created - 1) {
            if (WIFEXITED(status)) final_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) final_status = 128 + WTERMSIG(status);
            else final_status = 1;
        }
    }
    return final_status;
}

// 7. MAIN LOOP
static void shell_sigint(int signal_number) {
    (void)signal_number;
    write(STDOUT_FILENO, "\n", 1);
}

static int exit_status(Cmd *cmd, int *quit) {
    if (cmd->argc == 1) {
        *quit = 1;
        return 0;
    }
    if (cmd->argc > 2) {
        fprintf(stderr, "exit: too many arguments\n");
        return 1;
    }
    char *end;
    long code = strtol(cmd->argv[1], &end, 10);
    if (!cmd->argv[1][0] || *end) {
        fprintf(stderr, "exit: numeric argument required\n");
        *quit = 1;
        return 2;
    }
    *quit = 1;
    return (unsigned char)code;
}

int main(void) {
    if (!getcwd(shell_root, sizeof(shell_root))) {
        perror("getcwd");
        return 1;
    }
    root_len = strlen(shell_root);
    struct sigaction action = {0};
    action.sa_handler = shell_sigint;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    char line[LINE_SIZE], storage[LINE_SIZE];
    char *tokens[MAX_ARGS * 4];
    Cmd commands[MAX_CMDS];
    int interactive = isatty(STDIN_FILENO);
    int status = 0, quit = 0;
    if (interactive) puts("MyShell - type help for commands");
    while (!quit) {
        while (waitpid(-1, NULL, WNOHANG) > 0) { }
        if (interactive) {
            char cwd[PATH_MAX];
            printf("myshell:%s> ", getcwd(cwd, sizeof(cwd)) ? shown_path(cwd) : "?");
            fflush(stdout);
        }
        errno = 0;
        if (!fgets(line, sizeof(line), stdin)) {
            if (ferror(stdin) && errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            if (interactive) putchar('\n');
            break;
        }
        if (!strchr(line, '\n') && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) { }
            fprintf(stderr, "myshell: input line is too long\n");
            status = 2;
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[strspn(line, " \t")] == '\0') continue;
        add_history(line);
        int token_count = tokenize(line, tokens, storage);
        if (token_count < 0) {
            if (token_count == -1) fprintf(stderr, "myshell: unmatched quote\n");
            else if (token_count == -2) fprintf(stderr, "myshell: too many tokens\n");
            else fprintf(stderr, "myshell: command line is too long\n");
            status = 2;
            continue;
        }
        int ncmd = 0, background = 0;
        if (parse(tokens, token_count, commands, &ncmd, &background)) {
            fprintf(stderr, "myshell: invalid command syntax\n");
            status = 2;
            continue;
        }
        if (ncmd == 0) continue;
        if (ncmd == 1 && !strcmp(commands[0].argv[0], "exit")) {
            status = exit_status(&commands[0], &quit);
            continue;
        }
        status = execute(commands, ncmd, background);
    }
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
    return status;
}