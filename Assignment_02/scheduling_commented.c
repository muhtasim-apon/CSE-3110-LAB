/*
 * ==========================================================================
 *  CSE-3110 Operating Systems Lab - Assignment 02
 *  CPU Scheduling: FCFS, SJF, SRTN, Round Robin, Lottery, Priority and
 *  SJF with Aging
 * ==========================================================================
 *
 *  How the program works
 *  ---------------------
 *   1. It asks in the terminal how many processes you want (1-20).
 *   2. It writes ONE file, input.txt, that holds a separate, randomly
 *      generated set of parameters for every algorithm:
 *          [FCFS]     arrival burst            (one row per process)
 *          [SJF]      arrival burst
 *          [SRTN]     arrival burst
 *          [RR]       arrival burst            (quantum is NOT in the file)
 *          [LOTTERY]  arrival burst tickets, then time quantum and seed
 *          [COMPARE]  arrival burst tickets priority, then lottery quantum,
 *                     lottery seed and aging factor alpha
 *          [PRIORITY] arrival burst priority (smaller number = higher priority)
 *          [SJF_AGING] arrival burst, then the aging factor alpha
 *      The Round Robin time quantum is typed in the terminal each time
 *      Round Robin runs.
 *   3. A menu lets you run any single algorithm or compare all seven. Each
 *      menu choice opens input.txt again and reads its own section, so the
 *      results always come from the file (you can even edit it while the
 *      program is running).
 *   4. When the program ends - menu option 0, Ctrl+C, closing the terminal
 *      or end of input - input.txt is deleted. The next run asks for the
 *      number of processes again and generates a brand-new random file.
 *   5. An input.txt that already exists before the program starts is never
 *      overwritten: the program offers to keep it as a backup or to quit.
 *
 *  Rules taken from the lab sheet
 *  ------------------------------
 *   - Ties are broken by earlier arrival time, then by lower process ID.
 *   - The clock starts at time 0. Whenever no process has arrived yet the
 *     CPU is idle (shown as IDLE in the Gantt chart). Every generated
 *     section contains at least one idle gap, and idle time lowers the CPU
 *     utilisation.
 *   - CT  = completion time          TAT = CT - arrival
 *     WT  = TAT - burst              RT  = first time on the CPU - arrival
 *     CPU utilisation = busy time / elapsed time x 100 %
 *                       (elapsed time = 0 .. last completion, idle included)
 *     Throughput      = processes completed / elapsed time
 *
 *  Build:  gcc -Wall -Wextra -O2 scheduling.c -o scheduling
 *  Run:    ./scheduling
 * ==========================================================================
 */
#define _POSIX_C_SOURCE 200809L   /* for sigaction() on Linux */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define INPUT_FILE   "input.txt"
#define MAXP         20        /* maximum number of processes                */
#define MAXVALUE     100000000L /* largest number accepted from input.txt: even
                                * with 20 processes no time can overflow a long */
#define MAXSLICES    500000L   /* most time slices one RR/lottery run may need */
#define ALGS         7         /* algorithms in the comparison (menu option 6) */
#define GANTT_IDLE   (-1)      /* "process id" used for an idle CPU segment  */
#define LOTTERY_RUNS 100       /* seeds 1..100 averaged in the comparison    */

/* --------------------------------------------------------------------------
 *  Data types
 * -------------------------------------------------------------------------- */

/* One process. The first group of fields is read from input.txt; the rest
 * is filled in by the scheduling algorithm and by compute_stats(). */
typedef struct {
    int  pid;                  /* process number: P1 has pid 1, ...          */
    long at, bt, tickets;      /* arrival time, burst time, lottery tickets  */
    long prio;                 /* priority, smaller number = more important  */
    long rem;                  /* burst time still left during simulation    */
    long st, ct;               /* first time on the CPU, completion time     */
    long tat, wt, rt;          /* turnaround, waiting and response time      */
    int  started;              /* 1 once the process has run at least once   */
} Process;

/* A set of processes, always stored in PID order (p[0] = P1, p[1] = P2...). */
typedef struct { int n; Process p[MAXP]; } Workload;

/* One block of the Gantt chart: process `pid` ran from time s to time e. */
typedef struct { int pid; long s, e; } Segment;
/* The segment array grows on demand (see gantt_add), so a chart is never cut. */
typedef struct { int n, cap; Segment *s; } Gantt;

/* Summary numbers printed under every result table. */
typedef struct {
    double avg_tat, avg_wt, avg_rt, util, thr;
    long   busy, elapsed;
} Stats;

/* Row layouts used inside input.txt sections. The values are bit flags:
 * bit 0 = the row has tickets, bit 1 = the row has a priority. */
enum {
    FMT_AB   = 0,   /* "arrival burst"          - FCFS, SJF, SRTN, RR, SJF_AGING */
    FMT_ABT  = 1,   /* "arrival burst tickets"  - LOTTERY */
    FMT_ABP  = 2,   /* "arrival burst priority" - PRIORITY */
    FMT_ABTP = 3    /* "arrival burst tickets priority" - COMPARE */
};

/* --------------------------------------------------------------------------
 *  Cleanup: input.txt is flushed and deleted whenever the program ends
 * -------------------------------------------------------------------------- */

/* Set to 1 as soon as input.txt has been created, so the cleanup code only
 * deletes a file that this run made. sig_atomic_t because the signal handler
 * reads it. */
static volatile sig_atomic_t file_created = 0;

/*
 * cleanup_file() - delete input.txt on a normal exit.
 *
 * Registered with atexit() in install_handlers(), so it runs automatically
 * when main() returns (menu option 0) or when exit() is called (for example
 * when stdin reaches end of file). remove() deletes the file. The message
 * says what really happened: deleted, already gone (somebody else removed
 * it), or NOT deleted together with the reason.
 */
static void cleanup_file(void)
{
    if (!file_created) return;
    file_created = 0;
    if (remove(INPUT_FILE) == 0)
        printf("\n%s has been flushed and deleted.\n", INPUT_FILE);
    else if (errno == ENOENT)
        printf("\n%s was already gone; nothing to delete.\n", INPUT_FILE);
    else
        fprintf(stderr, "\nWarning: %s could not be deleted: %s\n", INPUT_FILE, strerror(errno));
}

/*
 * on_signal() - delete input.txt when the program is interrupted.
 *
 * Called for the signals listed in install_handlers(). atexit() handlers do
 * NOT run when a signal kills the program, so the file must be removed here.
 * Inside a signal handler only "async-signal-safe" functions may be used,
 * which is why unlink(), write() and _exit() are used instead of remove(),
 * printf() and exit(). The result of unlink() decides which message is
 * printed, so "deleted" is only claimed when it is true.
 * The exit status 128 + signal number is the usual shell convention.
 */
static void on_signal(int sig)
{
    static const char none[] = "\nInterrupted.\n";
    static const char good[] = "\nInterrupted - input.txt flushed and deleted.\n";
    static const char gone[] = "\nInterrupted - input.txt was already gone.\n";
    static const char fail[] = "\nInterrupted - WARNING: input.txt could not be deleted.\n";
    const char *msg = none;
    size_t len = sizeof none - 1;
    if (file_created) {
        if (unlink(INPUT_FILE) == 0) { msg = good; len = sizeof good - 1; }
        else if (errno == ENOENT)    { msg = gone; len = sizeof gone - 1; }
        else                         { msg = fail; len = sizeof fail - 1; }
    }
    if (write(STDOUT_FILENO, msg, len) < 0) { }
    _exit(128 + sig);
}

/*
 * install_handlers() - make sure input.txt is not left behind.
 *
 * Connects on_signal() to the signals that can end the program from outside
 * (Ctrl+C, kill, terminal closed, Ctrl+\, broken output pipe), and registers
 * cleanup_file() for every normal exit path. All signals are blocked while
 * the handler runs, so it cannot interrupt itself. Only SIGKILL (kill -9)
 * cannot be caught by any program.
 */
static void install_handlers(void)
{
    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE };
    struct sigaction sa = { .sa_handler = on_signal };
    sigfillset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
    atexit(cleanup_file);
}

/*
 * hold_signals() - hold = 1 postpones all signals, hold = 0 delivers them.
 *
 * Used around the creation of input.txt: without it a Ctrl+C between
 * creating the file and setting file_created would leave the file behind.
 */
static void hold_signals(int hold)
{
    static sigset_t old;
    sigset_t all;
    sigfillset(&all);
    if (hold) sigprocmask(SIG_BLOCK, &all, &old);
    else      sigprocmask(SIG_SETMASK, &old, NULL);
}

/* --------------------------------------------------------------------------
 *  Safe file helpers
 * -------------------------------------------------------------------------- */

/*
 * create_new() - create a file for writing, but only if it does not exist.
 *
 * O_CREAT | O_EXCL makes the operating system do the "does it exist?" check
 * and the creation in one step, so an existing file can never be emptied
 * (fopen(path, "w") would do exactly that). Returns NULL with errno set
 * (EEXIST if the name is taken).
 */
static FILE *create_new(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        int e = errno;
        close(fd);
        unlink(path);
        errno = e;
    }
    return f;
}

/*
 * finish_file() - close a written file and check that everything arrived.
 *
 * Output is buffered, so a full disk may only be noticed by fflush() or
 * fclose(); ferror() remembers an error from any earlier fprintf().
 * Returns 0 if the file is complete, -1 (after printing why) otherwise.
 */
static int finish_file(FILE *f, const char *path)
{
    errno = 0;
    int bad = fflush(f) != 0 || ferror(f), e = errno;
    if (fclose(f) != 0) { if (!bad) e = errno; bad = 1; }
    if (!bad) return 0;
    fprintf(stderr, "Error: %s could not be written completely: %s\n", path, e ? strerror(e) : "write error");
    return -1;
}

/* --------------------------------------------------------------------------
 *  Terminal input helpers
 * -------------------------------------------------------------------------- */

/*
 * read_line() - read one whole line from the terminal.
 *
 * The line is read character by character up to the newline, so even a line
 * longer than the buffer is consumed completely and its tail can never be
 * taken as the answer to the next question. Returns 1 if the line fitted, 0
 * if it was too long. If stdin ends (Ctrl+D or a finished pipe) the program
 * exits, and cleanup_file() deletes input.txt.
 */
static int read_line(char *buf, int size)
{
    int c, len = 0, fits = 1;
    fflush(stdout);
    while ((c = getchar()) != EOF && c != '\n') {
        if (c != '\0' && len < size - 1) buf[len++] = (char)c;
        else fits = 0;
    }
    buf[len] = '\0';
    if (c == EOF && len == 0 && fits) {
        printf("\n[end of input] Exiting.\n");
        exit(0);
    }
    return fits;
}

/*
 * parse_number() - convert the number at *p and move *p behind it.
 *
 * Used for the terminal and for input.txt, so both follow the same rules.
 * Returns 0 if there is no number, if it does not fit in a long (strtol then
 * sets errno to ERANGE) or if it is not followed by a space or the end of
 * the text ("12abc", "1.5" and "3-4" are rejected).
 */
static int parse_number(char **p, long *v)
{
    char *end;
    errno = 0;
    *v = strtol(*p, &end, 10);
    if (end == *p || errno == ERANGE || (*end != '\0' && !isspace((unsigned char)*end))) return 0;
    *p = end;
    return 1;
}

/*
 * read_long() - ask the user for a whole number in the range [lo, hi].
 *
 * The value is accepted only if the line contains one number and nothing
 * else except spaces, and the number is inside the range; otherwise an error
 * is printed and the question is asked again.
 */
static long read_long(const char *prompt, long lo, long hi)
{
    char buf[128];
    for (;;) {
        printf("%s", prompt);
        int ok = read_line(buf, sizeof buf);
        char *p = buf;
        long v = 0;
        ok = ok && parse_number(&p, &v);
        while (ok && isspace((unsigned char)*p)) p++;
        if (ok && *p == '\0' && v >= lo && v <= hi) return v;
        printf("  Please enter a whole number between %ld and %ld.\n", lo, hi);
    }
}

/*
 * pause_enter() - wait for Enter after a result has been printed.
 *
 * Gives the user time to read the output (or take a screenshot) before the
 * menu is printed again. End of input exits the program like read_long().
 */
static void pause_enter(void)
{
    char buf[64];
    printf("\nPress Enter to return to the menu...");
    read_line(buf, sizeof buf);
}

/* --------------------------------------------------------------------------
 *  input.txt generation (random values, different for every section)
 * -------------------------------------------------------------------------- */

/*
 * seed_random() - choose a different starting point for rand() every run.
 *
 * The time and the process id are mixed with random bytes from the operating
 * system (/dev/urandom); if that device is missing, time and pid still do.
 */
static void seed_random(void)
{
    unsigned seed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16), extra = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&extra, sizeof extra, 1, f) == 1) seed ^= extra;
        fclose(f);
    }
    srand(seed);
}

/*
 * rnd() - random whole number from lo to hi, both included.
 *
 * rand() % span alone would make small results slightly more likely, because
 * the RAND_MAX + 1 possible values cannot be divided evenly. Values from the
 * incomplete last group are therefore thrown away and drawn again, so every
 * result is equally likely.
 */
static long rnd(long lo, long hi)
{
    unsigned long span = (unsigned long)(hi - lo + 1), range = RAND_MAX + 1UL, r;
    do r = (unsigned long)rand(); while (r >= range - range % span);
    return lo + (long)(r % span);
}

/*
 * write_rows() - write one section's process count and process rows.
 *
 * First line: n. Then a comment naming the columns, then one row per
 * process P1..Pn ("arrival burst", plus tickets and/or priority if the
 * section has them). Every section is drawn fresh from rand(), so two sections never
 * share data:
 *     arrival : 0 .. 3n   (rows are therefore not in arrival order)
 *     burst   : 1 .. 12 ms
 *     tickets : 5, 10, ... 50
 *     priority: 1 .. 10   (1 = highest; equal priorities can occur)
 *
 * Guaranteed idle time: one randomly chosen process (`late`) arrives
 * 1..4 ms AFTER the CPU has finished all the other processes.
 * None of the algorithms leaves the CPU idle while a process is
 * waiting, so the moment the CPU runs out of work is the same for all of
 * them; it can be found by replaying the arrivals in time order:
 *     t = 0;  for each other process in arrival order:
 *                 t = max(t, arrival) + burst
 * at[late] = t + 1..4 therefore creates a real idle gap of 1..4 ms in
 * every algorithm's schedule, and CPU utilisation is below 100 % for all
 * of them. (Other idle gaps may appear by chance, and the CPU is also idle
 * from 0 if no process arrives at time 0.) With n = 1 the single process
 * arrives at 1..4 ms, so the CPU is idle from time 0.
 * Each row ends with "# Pk" so the file is easy to read by hand.
 */
static void write_rows(FILE *f, int n, int fmt)
{
    long at[MAXP], bt[MAXP];
    for (int i = 0; i < n; i++) {
        at[i] = rnd(0, 3L * n);
        bt[i] = rnd(1, 12);
    }
    int late = (int)rnd(0, n - 1);
    int ord[MAXP], m = 0;                        /* the other processes ... */
    for (int i = 0; i < n; i++) {
        if (i == late) continue;
        int j = m++;                             /* ... sorted by arrival */
        while (j > 0 && at[ord[j - 1]] > at[i]) { ord[j] = ord[j - 1]; j--; }
        ord[j] = i;
    }
    long t = 0;                                  /* when the CPU runs out of work */
    for (int k = 0; k < m; k++)
        t = (t > at[ord[k]] ? t : at[ord[k]]) + bt[ord[k]];
    at[late] = t + rnd(1, 4);

    fprintf(f, "%d            # number of processes\n", n);
    fprintf(f, "# arrival burst%s%s\n", fmt & FMT_ABT ? " tickets" : "", fmt & FMT_ABP ? " priority" : "");
    for (int i = 0; i < n; i++) {
        fprintf(f, "%-4ld %-4ld ", at[i], bt[i]);
        if (fmt & FMT_ABT) fprintf(f, "%-4ld ", 5 * rnd(1, 10));
        if (fmt & FMT_ABP) fprintf(f, "%-4ld ", rnd(1, 10));
        fprintf(f, "    # P%d\n", i + 1);
    }
}

/*
 * generate_input() - create input.txt with all eight sections.
 *
 * create_new() refuses to touch a file that already exists. file_created is
 * set straight away so that the file is deleted on exit even if writing
 * fails half way, and finish_file() reports a write error (for example a
 * full disk) instead of continuing with an incomplete file.
 *
 * Section contents:
 *   [FCFS], [SJF], [SRTN], [RR]  n + "arrival burst" rows
 *   [LOTTERY]                    n + "arrival burst tickets" rows + quantum
 *                                (2..4) + random seed (1..9999)
 *   [COMPARE]                    n + "arrival burst tickets priority" rows +
 *                                lottery quantum + lottery seed + alpha; used
 *                                by option 6 so that all seven algorithms run
 *                                on the SAME data
 *   [PRIORITY]                   n + "arrival burst priority" rows
 *   [SJF_AGING]                  n + "arrival burst" rows + aging factor alpha
 *                                (2..5)
 * Returns 0 on success, -1 if the file cannot be created or written.
 */
static int generate_input(int n)
{
    FILE *f = create_new(INPUT_FILE);
    if (!f) { perror(INPUT_FILE); return -1; }
    file_created = 1;

    fprintf(f, "# input.txt - generated with random values (deleted on exit)\n");
    fprintf(f, "# Every section: n, one row per process (P1..Pn), then its parameters.\n");
    fprintf(f, "# Keep one row or parameter per line; values are 0 to %ld.\n", MAXVALUE);
    fprintf(f, "# The Round Robin time quantum is NOT stored here: it is asked in the terminal.\n\n");

    fprintf(f, "[FCFS]\n");
    write_rows(f, n, FMT_AB);

    fprintf(f, "\n[SJF]\n");
    write_rows(f, n, FMT_AB);

    fprintf(f, "\n[SRTN]\n");
    write_rows(f, n, FMT_AB);

    fprintf(f, "\n[RR]\n");
    write_rows(f, n, FMT_AB);

    fprintf(f, "\n[LOTTERY]\n");
    write_rows(f, n, FMT_ABT);
    fprintf(f, "%-12ld # time quantum q (ms)\n", rnd(2, 4));
    fprintf(f, "%-12ld # random seed\n", rnd(1, 9999));

    fprintf(f, "\n[COMPARE]\n");
    fprintf(f, "# smaller priority number = higher priority\n");
    write_rows(f, n, FMT_ABTP);
    fprintf(f, "%-12ld # lottery time quantum q (ms)\n", rnd(2, 4));
    fprintf(f, "%-12ld # lottery random seed (single sample run)\n", rnd(1, 9999));
    fprintf(f, "%-12ld # aging factor alpha (ms)\n", rnd(2, 5));

    fprintf(f, "\n[PRIORITY]\n");
    fprintf(f, "# smaller priority number = higher priority\n");
    write_rows(f, n, FMT_ABP);

    fprintf(f, "\n[SJF_AGING]\n");
    write_rows(f, n, FMT_AB);
    fprintf(f, "%-12ld # aging factor alpha (ms)\n", rnd(2, 5));

    return finish_file(f, INPUT_FILE);
}

/*
 * back_up_input() - rename an existing input.txt to input.txt.bak1 (or the
 * next free number).
 *
 * rename() would silently replace an existing backup, so the backup name is
 * first reserved with create_new(); rename() then replaces only our own empty
 * placeholder. Returns 0 on success, -1 if the file could not be moved (it is
 * then still untouched).
 */
static int back_up_input(void)
{
    char name[64];
    for (int i = 1; i <= 999; i++) {
        snprintf(name, sizeof name, "%s.bak%d", INPUT_FILE, i);
        FILE *f = create_new(name);
        if (!f && errno == EEXIST) continue;
        if (!f) { perror(name); return -1; }
        fclose(f);
        if (rename(INPUT_FILE, name) != 0) {
            perror(INPUT_FILE);
            unlink(name);
            return -1;
        }
        printf("Your existing %s was kept as %s.\n", INPUT_FILE, name);
        return 0;
    }
    printf("Error: no free backup name for %s\n", INPUT_FILE);
    return -1;
}

/*
 * protect_existing_input() - never overwrite a user's input.txt.
 *
 * lstat() (not stat) is used so that even a broken symbolic link counts as
 * existing. Returns 0 if the name is free (possibly after making a backup),
 * -1 if the program should stop.
 */
static int protect_existing_input(void)
{
    struct stat sb;
    if (lstat(INPUT_FILE, &sb) != 0) return 0;
    printf("%s already exists in this folder and will not be overwritten.\n"
           "  1. Keep it as a backup (renamed) and generate a new one\n"
           "  0. Quit and leave it untouched\n", INPUT_FILE);
    if (read_long("Choose an option (0-1): ", 0, 1) == 0) return -1;
    return back_up_input();
}

/* --------------------------------------------------------------------------
 *  input.txt parsing
 * -------------------------------------------------------------------------- */

/* blank() - 1 if only spaces (or nothing) are left in the text. */
static int blank(const char *p)
{
    while (isspace((unsigned char)*p)) p++;
    return *p == '\0';
}

/*
 * load_section() - read one section of input.txt into a Workload.
 *
 *   name    section header to look for, e.g. "[RR]"
 *   fmt     row layout of that section (one of the FMT_ values)
 *   w       filled with the processes, in PID order
 *   par     receives the `npar` numbers that follow the rows (lottery:
 *           quantum and seed; SJF aging: alpha); may be NULL when npar is 0
 *
 * How it works:
 *   1. The file is read line by line. Everything after '#' is a comment and
 *      is cut off; empty lines are skipped. A line that does not fit in the
 *      buffer (or contains a NUL byte) is an error.
 *   2. Lines are ignored until the header `name`; any other header ends the
 *      section. The same header twice is an error.
 *   3. `got` counts the data lines of the section and tells what the next one
 *      must be: line 0 is n, lines 1..n are process rows, then npar lines
 *      with one parameter each. A line with too many or too few numbers, and
 *      any line after the last parameter, is an error.
 *   4. Every number must be a whole number from 0 to MAXVALUE, the process
 *      count 1..MAXP, every burst >= 1 and every ticket count >= 1. Together
 *      with MAXP this limit makes integer overflow impossible.
 *   5. The first problem is written to err[] with its line number; nothing
 *      is simulated unless the whole section is correct.
 * The file is opened again on every call, which is why every menu option
 * really uses what is currently stored in input.txt.
 * Returns 0 on success, -1 (after printing the reason) on any error.
 */
static int load_section(const char *name, int fmt, Workload *w, long *par, int npar)
{
    FILE *f = fopen(INPUT_FILE, "r");
    if (!f) { perror(INPUT_FILE); return -1; }
    int cols = 2 + !!(fmt & FMT_ABT) + !!(fmt & FMT_ABP), got = 0, in = 0, seen = 0, no = 0;
    char line[512], err[160] = "";
    memset(w, 0, sizeof *w);

    while (!err[0] && fgets(line, sizeof line, f)) {
        no++;
        if (!strchr(line, '\n') && !feof(f)) {
            snprintf(err, sizeof err, "line %d is too long or is not plain text", no);
            break;
        }
        line[strcspn(line, "#")] = '\0';
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '[') {
            in = strncmp(p, name, strlen(name)) == 0 && blank(p + strlen(name));
            if (in && seen++) snprintf(err, sizeof err, "line %d: section appears more than once", no);
            continue;
        }
        if (!in || *p == '\0') continue;

        long v[4] = { 0 }, x;
        int c = 0, want = got >= 1 && got <= w->n ? cols : 1;
        if (got > w->n + npar) {
            snprintf(err, sizeof err, "line %d: unexpected extra data", no);
            break;
        }
        while (*p && !err[0]) {
            if (isspace((unsigned char)*p)) { p++; continue; }
            if (!parse_number(&p, &x))
                snprintf(err, sizeof err, "line %d: '%.*s' is not a valid whole number",
                         no, (int)(strcspn(p, " \t\r\n") > 24 ? 24 : strcspn(p, " \t\r\n")), p);
            else if (x < 0 || x > MAXVALUE)
                snprintf(err, sizeof err, "line %d: %ld is outside 0 to %ld", no, x, MAXVALUE);
            else if (c++ < 4)
                v[c - 1] = x;
        }
        if (err[0]) break;
        if (c != want) {
            snprintf(err, sizeof err, "line %d has %d number(s), expected %d", no, c, want);
        } else if (got == 0) {
            if (v[0] < 1 || v[0] > MAXP)
                snprintf(err, sizeof err, "line %d: process count must be between 1 and %d", no, MAXP);
            else
                w->n = (int)v[0];
        } else if (got <= w->n) {
            w->p[got - 1] = (Process){ .pid = got, .at = v[0], .bt = v[1],
                                       .tickets = fmt & FMT_ABT ? v[2] : 0,
                                       .prio = fmt & FMT_ABP ? v[cols - 1] : 0 };
            if (v[1] < 1) snprintf(err, sizeof err, "line %d: burst of P%d must be at least 1", no, got);
            else if ((fmt & FMT_ABT) && v[2] < 1)
                snprintf(err, sizeof err, "line %d: tickets of P%d must be at least 1", no, got);
        } else {
            par[got - w->n - 1] = v[0];
        }
        got++;
    }
    if (!err[0] && ferror(f)) snprintf(err, sizeof err, "the file could not be read completely");
    fclose(f);
    if (!err[0] && !seen) snprintf(err, sizeof err, "section not found");
    if (!err[0] && got == 0) snprintf(err, sizeof err, "section is empty");
    if (!err[0] && got < 1 + w->n + npar)
        snprintf(err, sizeof err, "section has %d data line(s), expected %d", got, 1 + w->n + npar);
    if (!err[0]) return 0;
    printf("Error in %s, section %s: %s\n", INPUT_FILE, name, err);
    return -1;
}

/* --------------------------------------------------------------------------
 *  Gantt chart
 * -------------------------------------------------------------------------- */

/*
 * gantt_add() - record that `pid` (or GANTT_IDLE) used the CPU from s to e.
 *
 * Empty pieces (e <= s) are ignored. If the new piece belongs to the same
 * process as the previous one and starts exactly where that one ended, the
 * two are merged, so a process that keeps the CPU for several quanta in a
 * row (for example the last process left in Round Robin) appears as one
 * block like "P1 10-30" instead of "P1 10-14 | P1 14-18 | ...".
 * When the array is full it is doubled with realloc(), so the chart is always
 * complete, however many segments there are.
 */
static void gantt_add(Gantt *g, int pid, long s, long e)
{
    if (e <= s) return;
    if (g->n > 0 && g->s[g->n - 1].pid == pid && g->s[g->n - 1].e == s) {
        g->s[g->n - 1].e = e;
        return;
    }
    if (g->n == g->cap) {
        int cap = g->cap ? 2 * g->cap : 64;
        Segment *bigger = realloc(g->s, (size_t)cap * sizeof *bigger);
        if (!bigger) {
            fprintf(stderr, "Error: not enough memory for the Gantt chart\n");
            exit(1);
        }
        g->s = bigger;
        g->cap = cap;
    }
    g->s[g->n++] = (Segment){ pid, s, e };
}

/* gantt_free() - release the segments and make the chart empty again. */
static void gantt_free(Gantt *g)
{
    free(g->s);
    *g = (Gantt){ 0 };
}

/*
 * print_gantt() - print the chart in the lab-sheet text format:
 *     | P2 0-3 | IDLE 3-4 | P4 4-8 |
 * Each block is built in a small buffer first so its length is known; when
 * the next block would pass column 96 the chart continues on a new line,
 * which keeps long Round Robin charts readable in a normal terminal.
 */
static void print_gantt(const Gantt *g)
{
    printf("Gantt chart:\n  |");
    int col = 3;
    for (int i = 0; i < g->n; i++) {
        char cell[48];
        const Segment *s = &g->s[i];
        if (s->pid == GANTT_IDLE) snprintf(cell, sizeof cell, " IDLE %ld-%ld |", s->s, s->e);
        else                      snprintf(cell, sizeof cell, " P%d %ld-%ld |", s->pid, s->s, s->e);
        int len = (int)strlen(cell);
        if (col + len > 96) { printf("\n  |"); col = 3; }
        printf("%s", cell);
        col += len;
    }
    printf("\n");
}

/* --------------------------------------------------------------------------
 *  Tables, metrics and statistics
 * -------------------------------------------------------------------------- */

/*
 * rule() - print a horizontal table border such as  +---------+------+
 * wd[i] is the text width of column i; each column also has one space of
 * padding on both sides, hence wd[i] + 2 dashes.
 */
static void rule(const int *wd, int nc)
{
    putchar(' ');
    putchar('+');
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < wd[i] + 2; j++) putchar('-');
        putchar('+');
    }
    putchar('\n');
}

/*
 * row() - print one table row  | cell | cell |  with every cell
 * right-aligned in its column width, so numbers line up under each other.
 */
static void row(const int *wd, int nc, char cells[][24])
{
    printf(" |");
    for (int i = 0; i < nc; i++) printf(" %*s |", wd[i], cells[i]);
    putchar('\n');
}

/*
 * print_table() - print a whole table: heading, n rows and borders.
 * Every column is as wide as its heading or its longest cell, so the lines
 * stay straight for large numbers too.
 */
static void print_table(const char **hd, int nc, char v[][8][24], int n)
{
    int wd[8];
    char cells[8][24];
    for (int i = 0; i < nc; i++) {
        wd[i] = (int)strlen(hd[i]);
        for (int k = 0; k < n; k++)
            if ((int)strlen(v[k][i]) > wd[i]) wd[i] = (int)strlen(v[k][i]);
        snprintf(cells[i], sizeof cells[i], "%s", hd[i]);
    }
    rule(wd, nc); row(wd, nc, cells); rule(wd, nc);
    for (int k = 0; k < n; k++) row(wd, nc, v[k]);
    rule(wd, nc);
}

/*
 * input_cells() - fill the first cells of a table row (process, arrival,
 * burst and, depending on fmt, tickets and/or priority) and the matching
 * headings. Returns the number of columns used.
 */
static int input_cells(const Process *p, int fmt, char v[][24], const char **hd)
{
    int c = 3;
    snprintf(v[0], sizeof v[0], "P%d", p->pid);
    snprintf(v[1], sizeof v[1], "%ld", p->at);
    snprintf(v[2], sizeof v[2], "%ld", p->bt);
    if (fmt & FMT_ABT)  { hd[c] = "Tickets";  snprintf(v[c++], sizeof v[0], "%ld", p->tickets); }
    if (fmt & FMT_ABP) { hd[c] = "Priority"; snprintf(v[c++], sizeof v[0], "%ld", p->prio); }
    return c;
}

/*
 * print_input() - show exactly what was read from input.txt: the section
 * name and a table of Process / Arrival / Burst / Tickets / Priority.
 */
static void print_input(const char *section, const Workload *w, int fmt)
{
    const char *hd[8] = { "Process", "Arrival (ms)", "Burst (ms)" };
    char v[MAXP][8][24];
    int nc = 0;
    for (int i = 0; i < w->n; i++) nc = input_cells(&w->p[i], fmt, v[i], hd);
    printf("Input read from %s, section %s (n = %d):\n", INPUT_FILE, section, w->n);
    print_table(hd, nc, v, w->n);
}

/*
 * print_result_table() - the per-process result table required by the lab:
 *     Process | Arrival | Burst | (Tickets or Priority) | Completion |
 *     Turnaround | Waiting | Response
 *
 * `order` lists the process indexes in the order the rows should appear.
 * The non-preemptive algorithms pass their execution order (the lab sheet
 * prints SJF rows in execution order); NULL means plain PID order P1, P2, ...
 */
static void print_result_table(const Workload *w, const int *order, int fmt)
{
    const char *hd[8] = { "Process", "Arrival", "Burst" };
    char v[MAXP][8][24];
    int c = 0;
    for (int k = 0; k < w->n; k++) {
        const Process *p = &w->p[order ? order[k] : k];
        long r[4] = { p->ct, p->tat, p->wt, p->rt };
        c = input_cells(p, fmt, v[k], hd);
        for (int i = 0; i < 4; i++) snprintf(v[k][c + i], sizeof v[k][c + i], "%ld", r[i]);
    }
    hd[c] = "Completion"; hd[c + 1] = "Turnaround"; hd[c + 2] = "Waiting"; hd[c + 3] = "Response";
    printf("Result table (all times in ms):\n");
    print_table(hd, c + 4, v, w->n);
}

/*
 * compute_stats() - per-process metrics and their averages.
 *
 * Uses the completion time (ct) and first start time (st) that the
 * algorithm stored, and the formulas from the lab sheet:
 *     TAT = CT - AT,   WT = TAT - BT,   RT = ST - AT
 * WT = TAT - BT is correct for preemptive algorithms too, because a process
 * that runs in several pieces still needs exactly BT ms of CPU in total.
 * Elapsed time runs from time 0 to the last completion, so it includes
 * every idle period (before the first arrival and between processes). Busy
 * time is the sum of all bursts, the CPU time actually used by processes.
 * Idle time = elapsed - busy, and
 *     CPU utilisation = busy / elapsed x 100 %
 * which is below 100 % whenever the Gantt chart contains an IDLE block.
 */
static void compute_stats(Workload *w, Stats *s)
{
    double sum_tat = 0, sum_wt = 0, sum_rt = 0;
    long max_ct = 0;
    s->busy = 0;
    for (int i = 0; i < w->n; i++) {
        Process *p = &w->p[i];
        p->tat = p->ct - p->at;
        p->wt  = p->tat - p->bt;
        p->rt  = p->st - p->at;
        sum_tat += (double)p->tat; sum_wt += (double)p->wt; sum_rt += (double)p->rt;
        s->busy += p->bt;
        if (p->ct > max_ct) max_ct = p->ct;
    }
    s->elapsed = max_ct;                       /* from time 0, idle included */
    s->avg_tat = sum_tat / w->n;
    s->avg_wt  = sum_wt / w->n;
    s->avg_rt  = sum_rt / w->n;
    s->util    = s->elapsed > 0 ? 100.0 * (double)s->busy / (double)s->elapsed : 0.0;
    s->thr     = s->elapsed > 0 ? w->n / (double)s->elapsed : 0.0;
}

/*
 * print_stats() - print the averages, CPU utilisation and throughput that
 * the lab asks for under every result table.
 */
static void print_stats(const Stats *s)
{
    printf("Average Turnaround Time = %.2f ms\n", s->avg_tat);
    printf("Average Waiting Time    = %.2f ms\n", s->avg_wt);
    printf("Average Response Time   = %.2f ms\n", s->avg_rt);
    printf("CPU Utilisation         = %.2f %%  (busy %ld ms + idle %ld ms = %ld ms)\n",
           s->util, s->busy, s->elapsed - s->busy, s->elapsed);
    printf("Throughput              = %.4f processes/ms\n", s->thr);
}

/* banner() - print a boxed title so each algorithm's output stands out. */
static void banner(const char *title)
{
    printf("\n==========================================================================\n");
    printf("  %s\n", title);
    printf("==========================================================================\n");
}

/* --------------------------------------------------------------------------
 *  Helpers shared by the scheduling algorithms
 * -------------------------------------------------------------------------- */

/*
 * reset() - prepare a workload for a new simulation.
 * rem (remaining burst) starts at the full burst, all results are cleared.
 * Every algorithm calls this first, so the same Workload can be scheduled
 * several times (the comparison of all algorithms relies on this).
 */
static void reset(Workload *w)
{
    for (int i = 0; i < w->n; i++) {
        Process *p = &w->p[i];
        p->rem = p->bt; p->st = p->ct = 0; p->started = 0;
        p->tat = p->wt = p->rt = 0;
    }
}

/*
 * earlier() - the lab's tie-breaking rule.
 * Returns 1 if process a should go before process b: earlier arrival time
 * first, and for equal arrivals the lower process ID.
 */
static int earlier(const Process *a, const Process *b)
{
    if (a->at != b->at) return a->at < b->at;
    return a->pid < b->pid;
}

/*
 * sort_by_arrival() - fill ord[] with the process indexes sorted by
 * earlier(), i.e. by (arrival, PID). Insertion sort is used because n is at
 * most 20 and it is stable and easy to follow. The Workload itself is not
 * reordered; it stays in PID order.
 */
static void sort_by_arrival(const Workload *w, int *ord)
{
    for (int i = 0; i < w->n; i++) ord[i] = i;
    for (int i = 1; i < w->n; i++) {
        int x = ord[i], j = i - 1;
        while (j >= 0 && earlier(&w->p[x], &w->p[ord[j]])) { ord[j + 1] = ord[j]; j--; }
        ord[j + 1] = x;
    }
}

/*
 * next_arrival_after() - the earliest arrival time that is strictly later
 * than t, among processes that still have work. Used to jump the clock over
 * an idle gap and, in SRTN, to find the next event. Returns LONG_MAX if no
 * process arrives after t.
 */
static long next_arrival_after(const Workload *w, long t)
{
    long na = LONG_MAX;
    for (int i = 0; i < w->n; i++)
        if (w->p[i].rem > 0 && w->p[i].at > t && w->p[i].at < na) na = w->p[i].at;
    return na;
}

/*
 * run_piece() - let process p use the CPU from time t for `len` ms.
 * The first time a process runs its start time st is stored (this gives the
 * response time). The piece is added to the Gantt chart, the remaining burst
 * is reduced, and when it reaches 0 the completion time is stored.
 * All the algorithms use this one function to "run" a process.
 */
static void run_piece(Process *p, Gantt *g, long t, long len)
{
    if (!p->started) { p->st = t; p->started = 1; }
    gantt_add(g, p->pid, t, t + len);
    p->rem -= len;
    if (p->rem == 0) p->ct = t + len;
}

/* --------------------------------------------------------------------------
 *  The scheduling algorithms
 * -------------------------------------------------------------------------- */

/*
 * fcfs() - First-Come, First-Served (non-preemptive).
 *
 * 1. Sort the processes by arrival time (ties: lower PID) - this order is
 *    the order in which they run and is returned in exec[].
 * 2. Start the clock t at 0.
 * 3. For each process in that order: if it has not arrived yet (t < at)
 *    the CPU is idle until it arrives, so an IDLE block is added and t jumps
 *    to its arrival. The process then runs its whole burst without being
 *    interrupted, and t moves to its completion time.
 */
static void fcfs(Workload *w, Gantt *g, int *exec)
{
    reset(w);
    sort_by_arrival(w, exec);
    long t = 0;                                /* the clock starts at 0 */
    for (int k = 0; k < w->n; k++) {
        Process *p = &w->p[exec[k]];
        if (t < p->at) { gantt_add(g, GANTT_IDLE, t, p->at); t = p->at; }
        run_piece(p, g, t, p->bt);
        t += p->bt;
    }
}

/*
 * sjf() - Shortest Job First (non-preemptive).
 *
 * Every time the CPU becomes free (at time t):
 * 1. Look only at processes that have already arrived (at <= t) and are not
 *    finished, and pick the one with the smallest burst time. Equal bursts
 *    are decided by earlier(): earlier arrival, then lower PID.
 * 2. If nothing has arrived yet, the CPU is idle until the next arrival;
 *    an IDLE block is added and the choice is made again at that time.
 * 3. The chosen process runs its full burst (no preemption), t moves to its
 *    completion and its index is appended to exec[] (execution order).
 * When all processes arrive at 0 this is the same as sorting by burst.
 */
static void sjf(Workload *w, Gantt *g, int *exec)
{
    reset(w);
    long t = 0;                                /* the clock starts at 0 */
    int done = 0;
    while (done < w->n) {
        int best = -1;
        for (int i = 0; i < w->n; i++) {
            Process *p = &w->p[i];
            if (p->rem == 0 || p->at > t) continue;
            if (best < 0 || p->bt < w->p[best].bt ||
                (p->bt == w->p[best].bt && earlier(p, &w->p[best])))
                best = i;
        }
        if (best < 0) {
            long na = next_arrival_after(w, t);
            gantt_add(g, GANTT_IDLE, t, na);
            t = na;
            continue;
        }
        Process *p = &w->p[best];
        run_piece(p, g, t, p->bt);
        t += p->bt;
        exec[done++] = best;
    }
}

/*
 * srtn() - Shortest Remaining Time Next (preemptive SJF), event driven.
 *
 * Instead of stepping one millisecond at a time, the clock jumps from one
 * event to the next. The only moments a decision can change are when a new
 * process arrives or the running process finishes, so:
 * 1. Among arrived, unfinished processes pick the one with the smallest
 *    remaining time (ties: earlier(), i.e. earlier arrival, then lower PID).
 * 2. If the running process (cur) has a remaining time that is equal to or
 *    smaller than that choice, it keeps the CPU - the lab says a new arrival
 *    with the SAME remaining time must not preempt it.
 * 3. If a different process is chosen while cur still has work, that is a
 *    preemption and it is counted.
 * 4. The chosen process runs until the next event: its own completion
 *    (t + rem) or the next arrival, whichever is first.
 * 5. If nothing is ready, the CPU is idle until the next arrival.
 * Because the loop runs once per event (about 2n times), even very long
 * bursts are simulated instantly.
 */
static void srtn(Workload *w, Gantt *g, int *preemptions)
{
    reset(w);
    long t = 0;                                /* the clock starts at 0 */
    int done = 0, cur = -1;
    *preemptions = 0;
    while (done < w->n) {
        int best = -1;
        for (int i = 0; i < w->n; i++) {
            Process *p = &w->p[i];
            if (p->rem == 0 || p->at > t) continue;
            if (best < 0 || p->rem < w->p[best].rem ||
                (p->rem == w->p[best].rem && earlier(p, &w->p[best])))
                best = i;
        }
        if (best < 0) {
            long na = next_arrival_after(w, t);
            gantt_add(g, GANTT_IDLE, t, na);
            t = na;
            continue;
        }
        if (cur >= 0 && w->p[cur].rem > 0 && w->p[cur].rem <= w->p[best].rem) best = cur;
        if (cur >= 0 && w->p[cur].rem > 0 && best != cur) (*preemptions)++;
        cur = best;

        Process *p = &w->p[cur];
        long next = t + p->rem;                  /* its completion ...      */
        long na = next_arrival_after(w, t);
        if (na < next) next = na;                /* ... or the next arrival */
        run_piece(p, g, t, next - t);
        t = next;
        if (p->rem == 0) done++;
    }
}

/*
 * round_robin() - Round Robin with time quantum q (preemptive).
 *
 * Uses a FIFO ready queue stored as a circular array (at most n processes
 * can be waiting at once, so MAXP slots are enough).
 * 1. Processes are admitted to the tail of the queue in arrival order
 *    (ADMIT) as soon as the clock reaches their arrival time.
 * 2. If the queue is empty the CPU is idle until the next arrival.
 * 3. Otherwise the process at the head is taken out and runs for
 *    min(remaining burst, q) ms.
 * 4. After the slice, processes that arrived during it (or exactly when it
 *    ended) are admitted FIRST, and only then is the preempted process put
 *    back at the tail if it still has work - this is the order the lab
 *    sheet requires when an arrival and a quantum expiry happen together.
 * 5. A process whose remaining burst reaches 0 is finished.
 * `switches` counts how many times the CPU moved to a different process.
 * With q larger than every burst each process runs to completion in one
 * slice, so the schedule becomes exactly FCFS.
 */
static void round_robin(Workload *w, Gantt *g, long q, int *switches)
{
    int ord[MAXP], queue[MAXP];
    int head = 0, count = 0, k = 0, done = 0, last = -1;
    reset(w);
    sort_by_arrival(w, ord);
    long t = 0;                                /* the clock starts at 0 */
    *switches = 0;

#define ENQ(x)  do { queue[(head + count) % MAXP] = (x); count++; } while (0)
#define ADMIT() while (k < w->n && w->p[ord[k]].at <= t) { ENQ(ord[k]); k++; }

    ADMIT();
    while (done < w->n) {
        if (count == 0) {                         /* nothing ready: CPU idle */
            long na = w->p[ord[k]].at;
            gantt_add(g, GANTT_IDLE, t, na);
            t = na;
            ADMIT();
            continue;
        }
        int x = queue[head];                      /* dequeue the head */
        head = (head + 1) % MAXP;
        count--;
        if (last >= 0 && last != x) (*switches)++;

        Process *p = &w->p[x];
        long slice = p->rem < q ? p->rem : q;
        run_piece(p, g, t, slice);
        t += slice;
        ADMIT();                                  /* new arrivals go first */
        if (p->rem > 0) ENQ(x); else done++;      /* then the preempted one */
        last = x;
    }
#undef ENQ
#undef ADMIT
}

/*
 * slices() - how many time slices of q ms the workload needs in total:
 * every process needs burst / q slices, rounded up.
 */
static long slices(const Workload *w, long q)
{
    long total = 0;
    for (int i = 0; i < w->n; i++) total += (w->p[i].bt + q - 1) / q;
    return total;
}

/*
 * ask_quantum() - ask for the Round Robin quantum in the terminal.
 *
 * The upper limit is 100 ms, or the longest burst if that is larger, so a
 * quantum that makes RR equal to FCFS can always be typed. A quantum that
 * would need more than MAXSLICES slices (a burst of 100000000 ms with q = 1)
 * is refused with an explanation instead of running for hours.
 */
static long ask_quantum(const char *what, const Workload *w)
{
    char prompt[96];
    long hi = 100;
    for (int i = 0; i < w->n; i++) if (w->p[i].bt > hi) hi = w->p[i].bt;
    snprintf(prompt, sizeof prompt, "Enter the %s q (ms, 1-%ld): ", what, hi);
    for (;;) {
        long q = read_long(prompt, 1, hi);
        if (slices(w, q) <= MAXSLICES) return q;
        printf("  q = %ld needs %ld time slices (limit %ld). Please enter a larger quantum.\n",
               q, slices(w, q), MAXSLICES);
    }
}

/* lottery_quantum_ok() - the same check for the quantum from input.txt. */
static int lottery_quantum_ok(const Workload *w, long q)
{
    if (q < 1) printf("Error: lottery quantum must be >= 1\n");
    else if (slices(w, q) > MAXSLICES)
        printf("Error: lottery quantum %ld needs %ld time slices (limit %ld); use a larger quantum\n",
               q, slices(w, q), MAXSLICES);
    else return 1;
    return 0;
}

/* Lottery uses its own random number generator so that a given seed always
 * gives the same draws (reproducible runs), independent of the rand() calls
 * used to generate input.txt. */
static unsigned long long rng_state;

/*
 * lrng_seed() - start the lottery generator from `seed`.
 * The seed is spread over all 64 bits with an LCG step, because xorshift
 * produces poor first values from small seeds such as 1, 2, 3. A state of 0
 * would make xorshift return 0 forever, so it is replaced.
 */
static void lrng_seed(unsigned long long s)
{
    rng_state = s * 6364136223846793005ULL + 1442695040888963407ULL;
    if (rng_state == 0) rng_state = 88172645463325252ULL;
}

/*
 * lrng() - next 64-bit pseudo-random number (Marsaglia xorshift64).
 * Three shift-and-XOR steps scramble the state; it is fast and has a period
 * of 2^64 - 1, which is far more than any simulation here needs.
 */
static unsigned long long lrng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/*
 * lottery() - Lottery Scheduling with time quantum q and a random seed.
 *
 * At every scheduling point (time t):
 * 1. pool = total tickets of the processes that have arrived and still have
 *    work. Finished processes take their tickets out of the pool, so the
 *    shares of the others grow.
 * 2. If the pool is empty, nothing is ready and the CPU is idle until the
 *    next arrival.
 * 3. A ticket number draw = 1 .. pool is picked at random.
 * 4. Ticket numbers are given out in PID order over the runnable processes
 *    (e.g. P1 = 1-10, P2 = 11-30, P3 = 31-35). Walking through them and
 *    adding up tickets, the first process whose running total reaches draw
 *    holds the winning ticket.
 * 5. The winner runs for one quantum, or less if its remaining burst is
 *    shorter, then the next lottery is held.
 * With verbose = 1 every draw is printed in the lab-sheet log format:
 *     t=0  pool=35  draw=24 -> P2 runs 0-2  (P2 left 4)  [20/35 = 57.1%]
 * The part in [..] is the winner's real chance in that draw. It changes
 * during the run because only arrived, unfinished processes hold tickets.
 * verbose = 0 is used for the 100 silent runs of the comparison.
 */
static void lottery(Workload *w, Gantt *g, long q, long seed, int verbose)
{
    reset(w);
    lrng_seed((unsigned long long)seed);
    long t = 0;                                /* the clock starts at 0 */
    int done = 0;
    if (verbose)
        printf("Execution log (tickets numbered in PID order over runnable processes;\n"
               "[..] = the winner's tickets / pool = its real chance in that draw):\n");
    while (done < w->n) {
        long pool = 0;
        for (int i = 0; i < w->n; i++)
            if (w->p[i].rem > 0 && w->p[i].at <= t) pool += w->p[i].tickets;
        if (pool == 0) {
            long na = next_arrival_after(w, t);
            if (verbose) printf("  t=%-5ld CPU idle until %ld\n", t, na);
            gantt_add(g, GANTT_IDLE, t, na);
            t = na;
            continue;
        }
        unsigned long long r, range = (unsigned long long)pool;
        do r = lrng(); while (r >= ULLONG_MAX - ULLONG_MAX % range);   /* no modulo bias */
        long draw = (long)(r % range) + 1, acc = 0;
        int win = -1;
        for (int i = 0; i < w->n && win < 0; i++) {
            if (w->p[i].rem == 0 || w->p[i].at > t) continue;
            acc += w->p[i].tickets;
            if (draw <= acc) win = i;
        }
        Process *p = &w->p[win];
        long slice = p->rem < q ? p->rem : q;
        run_piece(p, g, t, slice);
        if (verbose) {
            char span[48], left[48];
            snprintf(span, sizeof span, "%ld-%ld", t, t + slice);
            if (p->rem > 0) snprintf(left, sizeof left, "(P%d left %ld)", p->pid, p->rem);
            else            snprintf(left, sizeof left, "(P%d done)", p->pid);
            printf("  t=%-5ld pool=%-5ld draw=%-5ld -> P%-2d runs %-10s %-15s [%ld/%ld = %.1f%%]\n",
                   t, pool, draw, p->pid, span, left, p->tickets, pool,
                   100.0 * (double)p->tickets / (double)pool);
        }
        t += slice;
        if (p->rem == 0) done++;
    }
}

/*
 * priority_np() - Priority Scheduling (non-preemptive).
 *
 * A SMALLER priority number means a MORE important process. Every time the
 * CPU becomes free (at time t):
 * 1. Look only at processes that have already arrived (at <= t) and are not
 *    finished, and pick the one with the smallest priority number. Equal
 *    priorities are decided by earlier(): earlier arrival, then lower PID.
 * 2. If nothing has arrived yet, the CPU is idle until the next arrival;
 *    an IDLE block is added and the choice is made again at that time.
 * 3. The chosen process runs its full burst - a more important process that
 *    arrives meanwhile has to wait - then t moves to its completion and its
 *    index is appended to exec[] (execution order).
 */
static void priority_np(Workload *w, Gantt *g, int *exec)
{
    reset(w);
    long t = 0;                                /* the clock starts at 0 */
    int done = 0;
    while (done < w->n) {
        int best = -1;
        for (int i = 0; i < w->n; i++) {
            Process *p = &w->p[i];
            if (p->rem == 0 || p->at > t) continue;
            if (best < 0 || p->prio < w->p[best].prio ||
                (p->prio == w->p[best].prio && earlier(p, &w->p[best])))
                best = i;
        }
        if (best < 0) {
            long na = next_arrival_after(w, t);
            gantt_add(g, GANTT_IDLE, t, na);
            t = na;
            continue;
        }
        Process *p = &w->p[best];
        run_piece(p, g, t, p->bt);
        t += p->bt;
        exec[done++] = best;
    }
}

/*
 * effective_burst() - the aged burst used by SJF with aging at time t:
 *     effective burst = burst - floor(time spent in the ready queue / alpha)
 * The algorithm is non-preemptive, so a process that has not run yet has
 * been in the ready queue ever since it arrived: t - at ms (never negative
 * here, so integer division is the floor). The result may drop to 0 or
 * below; it is only used to rank the processes, the real burst never changes.
 */
static long effective_burst(const Process *p, long t, long alpha)
{
    return p->bt - (t - p->at) / alpha;
}

/*
 * sjf_aging() - Shortest Job First with aging (non-preemptive).
 *
 * Same as sjf(), but the choice uses the effective burst of every arrived,
 * unfinished process, recalculated each time the CPU becomes free. The
 * longer a process waits the smaller its effective burst gets, so a long
 * process cannot be overtaken by new short ones for ever (no starvation).
 * Equal effective bursts are decided by earlier(): earlier arrival, then
 * lower PID. The chosen process runs its full, real burst.
 * A small alpha ages quickly (closer to FCFS); a very large alpha never
 * ages, which gives exactly the plain SJF schedule.
 * With verbose = 1 every decision is printed, for example:
 *     t=12    ready=3   P1: burst 9 - aged 2 (waited 12) = 7 -> runs 12-21
 */
static void sjf_aging(Workload *w, Gantt *g, long alpha, int *exec, int verbose)
{
    reset(w);
    long t = 0;                                /* the clock starts at 0 */
    int done = 0;
    if (verbose)
        printf("Execution log (effective burst = burst - floor(waited / %ld), smallest runs):\n", alpha);
    while (done < w->n) {
        int best = -1, ready = 0;
        long best_eff = 0;
        for (int i = 0; i < w->n; i++) {
            Process *p = &w->p[i];
            if (p->rem == 0 || p->at > t) continue;
            ready++;
            long eff = effective_burst(p, t, alpha);
            if (best < 0 || eff < best_eff ||
                (eff == best_eff && earlier(p, &w->p[best]))) {
                best = i;
                best_eff = eff;
            }
        }
        if (best < 0) {
            long na = next_arrival_after(w, t);
            if (verbose) printf("  t=%-5ld CPU idle until %ld\n", t, na);
            gantt_add(g, GANTT_IDLE, t, na);
            t = na;
            continue;
        }
        Process *p = &w->p[best];
        if (verbose)
            printf("  t=%-5ld ready=%-3d P%d: burst %ld - aged %ld (waited %ld) = %ld -> runs %ld-%ld\n",
                   t, ready, p->pid, p->bt, p->bt - best_eff, t - p->at, best_eff, t, t + p->bt);
        run_piece(p, g, t, p->bt);
        t += p->bt;
        exec[done++] = best;
    }
}

/* --------------------------------------------------------------------------
 *  Menu actions - each one reads its own section of input.txt
 * -------------------------------------------------------------------------- */

/*
 * menu_fcfs() - menu option 1.
 * Reads [FCFS], prints the input, runs fcfs() and prints the Gantt chart,
 * the result table (rows in the order the processes ran) and the averages.
 */
static void menu_fcfs(void)
{
    Workload w; Gantt g = { 0 }; Stats s; int exec[MAXP];
    banner("FIRST-COME, FIRST-SERVED (FCFS) - non-preemptive");
    if (load_section("[FCFS]", FMT_AB, &w, NULL, 0) < 0) return;
    print_input("[FCFS]", &w, FMT_AB);
    fcfs(&w, &g, exec);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, exec, FMT_AB);
    printf("\n"); print_stats(&s);
    gantt_free(&g);
}

/*
 * menu_sjf() - menu option 2.
 * Reads [SJF], runs non-preemptive sjf() and prints the Gantt chart, the
 * result table with rows in the order the processes ran (as in the lab
 * sheet's SJF output) and the averages.
 */
static void menu_sjf(void)
{
    Workload w; Gantt g = { 0 }; Stats s; int exec[MAXP];
    banner("SHORTEST JOB FIRST (SJF) - non-preemptive");
    if (load_section("[SJF]", FMT_AB, &w, NULL, 0) < 0) return;
    print_input("[SJF]", &w, FMT_AB);
    sjf(&w, &g, exec);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, exec, FMT_AB);
    printf("\n"); print_stats(&s);
    gantt_free(&g);
}

/*
 * menu_srtn() - menu option 3.
 * Reads [SRTN], runs srtn() and prints the Gantt chart, the result table,
 * the averages and the number of preemptions.
 */
static void menu_srtn(void)
{
    Workload w; Gantt g = { 0 }; Stats s; int pre;
    banner("SHORTEST REMAINING TIME NEXT (SRTN) - preemptive, event-driven");
    if (load_section("[SRTN]", FMT_AB, &w, NULL, 0) < 0) return;
    print_input("[SRTN]", &w, FMT_AB);
    srtn(&w, &g, &pre);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, NULL, FMT_AB);
    printf("\n"); print_stats(&s);
    printf("Preemptions             = %d\n", pre);
    gantt_free(&g);
}

/*
 * menu_rr() - menu option 4.
 * Reads [RR] (processes only), then asks for the time quantum q in the
 * terminal - the quantum is deliberately not stored in input.txt. Runs
 * round_robin() and prints the Gantt chart, result table, averages and the
 * number of times the CPU switched process.
 */
static void menu_rr(void)
{
    Workload w; Gantt g = { 0 }; Stats s; int sw;
    banner("ROUND ROBIN (RR) - preemptive, time quantum from the terminal");
    if (load_section("[RR]", FMT_AB, &w, NULL, 0) < 0) return;
    print_input("[RR]", &w, FMT_AB);
    long q = ask_quantum("time quantum", &w);
    printf("\nTime quantum q = %ld ms\n", q);
    round_robin(&w, &g, q, &sw);
    compute_stats(&w, &s);
    print_gantt(&g);
    printf("\n"); print_result_table(&w, NULL, FMT_AB);
    printf("\n"); print_stats(&s);
    printf("Process switches        = %d\n", sw);
    gantt_free(&g);
}

/*
 * menu_lottery() - menu option 5.
 * Reads [LOTTERY]: arrival, burst and tickets per process, then the quantum and the
 * seed (all random values from input.txt). Prints each process's ticket
 * share (the chance if all processes were runnable together), then runs lottery() with the execution log switched on, followed
 * by the Gantt chart, result table (with tickets) and averages.
 */
static void menu_lottery(void)
{
    Workload w; Gantt g = { 0 }; Stats s; long ex[2];
    banner("LOTTERY SCHEDULING - probabilistic, preemptive");
    if (load_section("[LOTTERY]", FMT_ABT, &w, ex, 2) < 0) return;
    long q = ex[0], seed = ex[1];
    if (!lottery_quantum_ok(&w, q)) return;
    print_input("[LOTTERY]", &w, FMT_ABT);
    long total = 0;
    for (int i = 0; i < w.n; i++) total += w.p[i].tickets;
    printf("Time quantum q = %ld ms, random seed = %ld, total tickets = %ld (read from %s)\n",
           q, seed, total, INPUT_FILE);
    printf("Ticket shares if every process were runnable at once:\n ");
    for (int i = 0; i < w.n; i++) printf(" P%d=%.1f%%", w.p[i].pid, 100.0 * (double)w.p[i].tickets / (double)total);
    printf("\n\n");
    lottery(&w, &g, q, seed, 1);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, NULL, FMT_ABT);
    printf("\n"); print_stats(&s);
    gantt_free(&g);
}

/*
 * menu_compare() - menu option 6: all seven algorithms on the same data.
 *
 * Reads [COMPARE] (arrival, burst, tickets and priority for every process,
 * then the lottery quantum, the lottery seed and the aging factor alpha) and
 * asks for the Round Robin quantum in the terminal. Each algorithm is run on
 * a fresh copy of the same workload and its Gantt chart is printed. Because
 * one lottery run is a matter of luck, the lottery row of the table is the
 * mean over seeds 1..LOTTERY_RUNS; the seed from the file is used for the
 * sample Gantt chart. Finally one table shows average TAT, WT, RT, CPU
 * utilisation and throughput per algorithm, and the best algorithm for each
 * time metric is named (ties are all listed).
 */
static void menu_compare(void)
{
    Workload base, w; long par[3]; int tmp[MAXP], n_int;
    banner("COMPARISON OF ALL SEVEN ALGORITHMS (shared [COMPARE] workload)");
    if (load_section("[COMPARE]", FMT_ABTP, &base, par, 3) < 0) return;
    long lq = par[0], lseed = par[1], alpha = par[2];
    if (!lottery_quantum_ok(&base, lq)) return;
    if (alpha < 1) { printf("Error: aging factor alpha must be >= 1\n"); return; }
    print_input("[COMPARE]", &base, FMT_ABTP);
    printf("Lottery: q = %ld ms, sample seed = %ld; SJF Aging: alpha = %ld ms (read from %s)\n",
           lq, lseed, alpha, INPUT_FILE);
    long q = ask_quantum("Round Robin time quantum", &base);

    const char *names[ALGS];
    Stats st[ALGS];
    char rrname[48], lotname[48], agename[48];
    snprintf(rrname, sizeof rrname, "RR (q=%ld)", q);
    snprintf(lotname, sizeof lotname, "Lottery (q=%ld, mean %d)", lq, LOTTERY_RUNS);
    snprintf(agename, sizeof agename, "SJF Aging (alpha=%ld)", alpha);
    names[0] = "FCFS"; names[1] = "SJF"; names[2] = "SRTN"; names[3] = rrname;
    names[4] = lotname; names[5] = "Priority"; names[6] = agename;

    Gantt g = { 0 };                              /* g.n = 0 empties it for reuse */
    w = base; g.n = 0; fcfs(&w, &g, tmp); compute_stats(&w, &st[0]);
    printf("\n[FCFS]\n"); print_gantt(&g);
    w = base; g.n = 0; sjf(&w, &g, tmp); compute_stats(&w, &st[1]);
    printf("\n[SJF]\n"); print_gantt(&g);
    w = base; g.n = 0; srtn(&w, &g, &n_int); compute_stats(&w, &st[2]);
    printf("\n[SRTN]  (preemptions: %d)\n", n_int); print_gantt(&g);
    w = base; g.n = 0; round_robin(&w, &g, q, &n_int); compute_stats(&w, &st[3]);
    printf("\n[%s]  (process switches: %d)\n", rrname, n_int); print_gantt(&g);
    w = base; g.n = 0; lottery(&w, &g, lq, lseed, 0);
    printf("\n[%s]  (sample run, seed %ld)\n", lotname, lseed); print_gantt(&g);
    w = base; g.n = 0; priority_np(&w, &g, tmp); compute_stats(&w, &st[5]);
    printf("\n[Priority]\n"); print_gantt(&g);
    w = base; g.n = 0; sjf_aging(&w, &g, alpha, tmp, 0); compute_stats(&w, &st[6]);
    printf("\n[%s]\n", agename); print_gantt(&g);

    memset(&st[4], 0, sizeof st[4]);              /* lottery: mean of many seeds */
    for (long sd = 1; sd <= LOTTERY_RUNS; sd++) {
        Stats one;
        w = base; g.n = 0;
        lottery(&w, &g, lq, sd, 0);
        compute_stats(&w, &one);
        st[4].avg_tat += one.avg_tat;
        st[4].avg_wt  += one.avg_wt;
        st[4].avg_rt  += one.avg_rt;
        st[4].util    += one.util;
        st[4].thr     += one.thr;
    }
    st[4].avg_tat /= LOTTERY_RUNS;
    st[4].avg_wt  /= LOTTERY_RUNS;
    st[4].avg_rt  /= LOTTERY_RUNS;
    st[4].util    /= LOTTERY_RUNS;
    st[4].thr     /= LOTTERY_RUNS;
    gantt_free(&g);

    /* The name is left-aligned, so this table is built here: all cells are
     * written first, then every column gets the width of its longest cell. */
    const char *hd[6] = { "Algorithm", "Avg TAT", "Avg WT", "Avg RT", "CPU %", "Throughput" };
    char v[ALGS][6][48];
    int wd[6];
    for (int i = 0; i < ALGS; i++) {
        const double m[5] = { st[i].avg_tat, st[i].avg_wt, st[i].avg_rt, st[i].util, st[i].thr };
        snprintf(v[i][0], sizeof v[i][0], "%s", names[i]);
        for (int c = 0; c < 5; c++) snprintf(v[i][c + 1], sizeof v[i][0], "%.*f", c == 4 ? 4 : 2, m[c]);
    }
    for (int c = 0; c < 6; c++) {
        wd[c] = (int)strlen(hd[c]);
        for (int i = 0; i < ALGS; i++) if ((int)strlen(v[i][c]) > wd[c]) wd[c] = (int)strlen(v[i][c]);
    }
    printf("\nINTEGRATED COMPARISON (times in ms)\n");
    rule(wd, 6);
    for (int i = -1; i < ALGS; i++) {             /* i = -1 is the heading */
        printf(" |");
        for (int c = 0; c < 6; c++) printf(" %*s |", c == 0 ? -wd[c] : wd[c], i < 0 ? hd[c] : v[i][c]);
        printf("\n");
        if (i < 0) rule(wd, 6);
    }
    rule(wd, 6);

    const char *metric[3] = { "turnaround time", "waiting time", "response time" };
    for (int m = 0; m < 3; m++) {
        double best = 0;
        for (int i = 0; i < ALGS; i++) {
            double x = m == 0 ? st[i].avg_tat : m == 1 ? st[i].avg_wt : st[i].avg_rt;
            if (i == 0 || x < best) best = x;
        }
        printf("Best average %-16s: ", metric[m]);
        int first = 1;
        for (int i = 0; i < ALGS; i++) {
            double x = m == 0 ? st[i].avg_tat : m == 1 ? st[i].avg_wt : st[i].avg_rt;
            if (x - best < 1e-9 * (1.0 + best)) { printf("%s%s", first ? "" : ", ", names[i]); first = 0; }
        }
        printf("  (%.2f ms)\n", best);
    }
}

/*
 * menu_show_file() - menu option 7: print input.txt exactly as it is on
 * disk, so you can see (or screenshot) the random values being used.
 */
static void menu_show_file(void)
{
    banner("CONTENTS OF input.txt");
    FILE *f = fopen(INPUT_FILE, "r");
    if (!f) { perror(INPUT_FILE); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    if (ferror(f)) perror(INPUT_FILE);
    fclose(f);
}

/*
 * menu_priority() - menu option 8.
 * Reads [PRIORITY] (arrival, burst and priority), runs non-preemptive
 * priority_np() and prints the Gantt chart, the result table with rows in
 * the order the processes ran and the averages.
 */
static void menu_priority(void)
{
    Workload w; Gantt g = { 0 }; Stats s; int exec[MAXP];
    banner("PRIORITY SCHEDULING - non-preemptive, smaller number = higher priority");
    if (load_section("[PRIORITY]", FMT_ABP, &w, NULL, 0) < 0) return;
    print_input("[PRIORITY]", &w, FMT_ABP);
    priority_np(&w, &g, exec);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, exec, FMT_ABP);
    printf("\n"); print_stats(&s);
    gantt_free(&g);
}

/* max_wait() - the longest waiting time of any process (starvation check). */
static long max_wait(const Workload *w)
{
    long m = 0;
    for (int i = 0; i < w->n; i++) if (w->p[i].wt > m) m = w->p[i].wt;
    return m;
}

/*
 * menu_sjf_aging() - menu option 9.
 * Reads [SJF_AGING] (arrival and burst, then the aging factor alpha), runs
 * sjf_aging() with its execution log and prints the Gantt chart, the result
 * table with rows in the order the processes ran and the averages. Plain
 * sjf() is also run on a copy of the same data, so the last lines show what
 * aging did to the average and to the longest waiting time.
 */
static void menu_sjf_aging(void)
{
    Workload w; Gantt g = { 0 }; Stats s; int exec[MAXP]; long alpha;
    banner("SHORTEST JOB FIRST (SJF) WITH AGING - non-preemptive");
    if (load_section("[SJF_AGING]", FMT_AB, &w, &alpha, 1) < 0) return;
    if (alpha < 1) { printf("Error: aging factor alpha must be >= 1\n"); return; }
    Workload plain = w; Gantt pg = { 0 }; Stats ps; int pexec[MAXP];
    print_input("[SJF_AGING]", &w, FMT_AB);
    printf("Aging factor alpha = %ld ms (read from %s)\n\n", alpha, INPUT_FILE);
    sjf_aging(&w, &g, alpha, exec, 1);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, exec, FMT_AB);
    printf("\n"); print_stats(&s);

    sjf(&plain, &pg, pexec);
    compute_stats(&plain, &ps);
    printf("\nEffect of aging on the same data:\n");
    printf("  Average waiting time  : SJF %.2f ms -> with aging %.2f ms\n", ps.avg_wt, s.avg_wt);
    printf("  Longest waiting time  : SJF %ld ms -> with aging %ld ms\n", max_wait(&plain), max_wait(&w));
    gantt_free(&g);
    gantt_free(&pg);
}

/* print_menu() - print the list of menu options. */
static void print_menu(void)
{
    printf("\n==================== CPU SCHEDULING MENU ====================\n");
    printf("  1. First-Come, First-Served (FCFS)\n");
    printf("  2. Shortest Job First (SJF)\n");
    printf("  3. Shortest Remaining Time Next (SRTN)\n");
    printf("  4. Round Robin (RR)  - time quantum asked here\n");
    printf("  5. Lottery Scheduling\n");
    printf("  6. Compare all seven algorithms\n");
    printf("  7. Show input.txt\n");
    printf("  8. Priority Scheduling\n");
    printf("  9. Shortest Job First with Aging (SJF Aging)\n");
    printf("  0. Exit (input.txt will be deleted)\n");
    printf("=============================================================\n");
}

/*
 * main() - program flow.
 * 1. Line-buffer stdout so output appears in order even when piped.
 * 2. Install the cleanup handlers BEFORE input.txt exists, so the file is
 *    removed however the program ends.
 * 3. Seed rand() with the time, the process id and system randomness: two
 *    runs in the same second still get different random values.
 * 4. Make sure no existing input.txt is lost, ask for the number of processes
 *    and generate input.txt (signals are postponed while it is written).
 * 5. Menu loop: run the chosen option, wait for Enter, show the menu again.
 *    Option 0 returns from main(), and atexit() then deletes input.txt.
 */
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_handlers();
    seed_random();

    printf("=============================================================\n");
    printf("   CPU Scheduling Simulator - CSE-3110 OS Lab, Assignment 02\n");
    printf("=============================================================\n");
    if (protect_existing_input() < 0) return 1;
    int n = (int)read_long("How many processes do you want? (1-20): ", 1, MAXP);

    hold_signals(1);
    int made = generate_input(n);
    hold_signals(0);
    if (made < 0) return 1;
    printf("%s created with random parameters for %d processes in sections\n"
           "[FCFS] [SJF] [SRTN] [RR] [LOTTERY] [COMPARE] [PRIORITY] [SJF_AGING].\n", INPUT_FILE, n);

    for (;;) {
        print_menu();
        long choice = read_long("Choose an option (0-9): ", 0, 9);
        switch (choice) {
        case 1: menu_fcfs();      break;
        case 2: menu_sjf();       break;
        case 3: menu_srtn();      break;
        case 4: menu_rr();        break;
        case 5: menu_lottery();   break;
        case 6: menu_compare();   break;
        case 7: menu_show_file(); break;
        case 8: menu_priority();  break;
        case 9: menu_sjf_aging(); break;
        case 0:
            printf("Exiting...\n");
            return 0;
        }
        pause_enter();
    }
}
