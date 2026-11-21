/*
 * ==========================================================================
 *  CSE-3110 Operating Systems Lab - Assignment 02
 *  CPU Scheduling: FCFS, SJF, SRTN, Round Robin and Lottery Scheduling
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
 *          [COMPARE]  arrival burst tickets, then lottery quantum and seed
 *      The Round Robin time quantum is typed in the terminal each time
 *      Round Robin runs.
 *   3. A menu lets you run any single algorithm or compare all five. Each
 *      menu choice opens input.txt again and reads its own section, so the
 *      results always come from the file (you can even edit it while the
 *      program is running).
 *   4. When the program ends - menu option 0, Ctrl+C, closing the terminal
 *      or end of input - input.txt is deleted. The next run asks for the
 *      number of processes again and generates a brand-new random file.
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
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INPUT_FILE   "input.txt"
#define MAXP         20        /* maximum number of processes                */
#define MAXSEG       4096      /* maximum number of Gantt chart segments     */
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
    long rem;                  /* burst time still left during simulation    */
    long st, ct;               /* first time on the CPU, completion time     */
    long tat, wt, rt;          /* turnaround, waiting and response time      */
    int  started;              /* 1 once the process has run at least once   */
} Process;

/* A set of processes, always stored in PID order (p[0] = P1, p[1] = P2...). */
typedef struct { int n; Process p[MAXP]; } Workload;

/* One block of the Gantt chart: process `pid` ran from time s to time e. */
typedef struct { int pid; long s, e; } Segment;
typedef struct { int n; Segment s[MAXSEG]; } Gantt;

/* Summary numbers printed under every result table. */
typedef struct {
    double avg_tat, avg_wt, avg_rt, util, thr;
    long   busy, elapsed;
} Stats;

/* Row layouts used inside input.txt sections. */
enum {
    FMT_AB,     /* "arrival burst"          - FCFS, SJF, SRTN, RR */
    FMT_ABT     /* "arrival burst tickets"  - LOTTERY, COMPARE */
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
 * when stdin reaches end of file). remove() deletes the file; the message
 * confirms to the user that the random data has been discarded.
 */
static void cleanup_file(void)
{
    if (file_created && remove(INPUT_FILE) == 0) {
        file_created = 0;
        printf("\n%s has been flushed and deleted.\n", INPUT_FILE);
    }
}

/*
 * on_signal() - delete input.txt when the program is interrupted.
 *
 * Called for SIGINT (Ctrl+C), SIGTERM (kill) and SIGHUP (terminal window
 * closed). atexit() handlers do NOT run when a signal kills the program, so
 * the file must be removed here. Inside a signal handler only
 * "async-signal-safe" functions may be used, which is why unlink(), write()
 * and _exit() are used instead of remove(), printf() and exit().
 * The exit status 128 + signal number is the usual shell convention.
 */
static void on_signal(int sig)
{
    static const char msg[] = "\nInterrupted - input.txt flushed and deleted.\n";
    if (file_created) unlink(INPUT_FILE);
    if (write(STDOUT_FILENO, msg, sizeof msg - 1) < 0) { /* nothing to do */ }
    _exit(128 + sig);
}

/*
 * install_handlers() - make sure input.txt can never be left behind.
 *
 * Connects on_signal() to the three signals that can end the program from
 * outside, and registers cleanup_file() for every normal exit path.
 */
static void install_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    atexit(cleanup_file);
}

/* --------------------------------------------------------------------------
 *  Terminal input helpers
 * -------------------------------------------------------------------------- */

/*
 * read_long() - ask the user for a whole number in the range [lo, hi].
 *
 * The whole line is read with fgets() and converted with strtol(). The value
 * is accepted only if the line contains a number and nothing else except
 * spaces, and the number is inside the range; otherwise an error is printed
 * and the question is asked again. Reading whole lines (instead of scanf)
 * means a wrong key press like "abc" can never leave junk in the input
 * buffer. If stdin ends (Ctrl+D or a finished pipe) the program exits, and
 * cleanup_file() deletes input.txt.
 */
static long read_long(const char *prompt, long lo, long hi)
{
    char buf[128];
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) {
            printf("\n[end of input] Exiting.\n");
            exit(0);
        }
        char *end;
        long v = strtol(buf, &end, 10);
        while (isspace((unsigned char)*end)) end++;
        if (end != buf && *end == '\0' && v >= lo && v <= hi)
            return v;
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
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) {
        printf("\n[end of input] Exiting.\n");
        exit(0);
    }
}

/* --------------------------------------------------------------------------
 *  input.txt generation (random values, different for every section)
 * -------------------------------------------------------------------------- */

/*
 * rnd() - random whole number from lo to hi, both included.
 *
 * rand() % (hi - lo + 1) gives 0 .. hi-lo; adding lo shifts it into range.
 * rand() is seeded once in main() with the current time and the process id,
 * so every run of the program produces different values.
 */
static long rnd(long lo, long hi) { return lo + rand() % (hi - lo + 1); }

/*
 * write_rows() - write one section's process count and process rows.
 *
 * First line: n. Then a comment naming the columns, then one row per
 * process P1..Pn ("arrival burst" or "arrival burst tickets"). Every
 * section is drawn fresh from rand(), so two sections never share data:
 *     arrival : 0 .. 3n   (rows are therefore not in arrival order)
 *     burst   : 1 .. 12 ms
 *     tickets : 5, 10, ... 50
 *
 * Guaranteed idle time: one randomly chosen process (`late`) arrives
 * 1..4 ms AFTER the CPU has finished all the other processes.
 * None of the five algorithms leaves the CPU idle while a process is
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
    long at[MAXP], bt[MAXP], tk[MAXP];
    for (int i = 0; i < n; i++) {
        at[i] = rnd(0, 3L * n);
        bt[i] = rnd(1, 12);
        tk[i] = 5 * rnd(1, 10);
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
    fprintf(f, fmt == FMT_AB ? "# arrival burst\n" : "# arrival burst tickets\n");
    for (int i = 0; i < n; i++) {
        if (fmt == FMT_AB) fprintf(f, "%-4ld %-4ld          # P%d\n", at[i], bt[i], i + 1);
        else               fprintf(f, "%-4ld %-4ld %-4ld     # P%d\n", at[i], bt[i], tk[i], i + 1);
    }
}

/*
 * generate_input() - create input.txt with all six sections.
 *
 * Opening with "w" creates the file, or empties it if an old one exists, so
 * every run starts from a clean file. file_created is set straight away so
 * that the file is deleted on exit even if writing fails half way.
 *
 * Section contents:
 *   [FCFS], [SJF], [SRTN], [RR]  n + "arrival burst" rows
 *   [LOTTERY]                    n + "arrival burst tickets" rows + quantum
 *                                (2..4) + random seed (1..9999)
 *   [COMPARE]                    n + "arrival burst tickets" rows + lottery
 *                                quantum + lottery seed; used by option 6 so
 *                                that all algorithms run on the SAME data
 * Returns 0 on success, -1 if the file cannot be created.
 */
static int generate_input(int n)
{
    FILE *f = fopen(INPUT_FILE, "w");
    if (!f) { perror(INPUT_FILE); return -1; }
    file_created = 1;

    fprintf(f, "# input.txt - generated with random values (deleted on exit)\n");
    fprintf(f, "# Every section: n, one row per process (P1..Pn), then its parameters.\n");
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
    write_rows(f, n, FMT_ABT);
    fprintf(f, "%-12ld # lottery time quantum q (ms)\n", rnd(2, 4));
    fprintf(f, "%-12ld # lottery random seed (single sample run)\n", rnd(1, 9999));

    fclose(f);
    return 0;
}

/* --------------------------------------------------------------------------
 *  input.txt parsing
 * -------------------------------------------------------------------------- */

/*
 * load_section() - read one section of input.txt into a Workload.
 *
 *   name    section header to look for, e.g. "[RR]"
 *   fmt     row layout of that section (FMT_AB or FMT_ABT)
 *   w       filled with the processes, in PID order
 *   extra   receives the `nextra` numbers that follow the rows
 *           (lottery: quantum and seed); may be NULL when nextra is 0
 *
 * How it works:
 *   1. The file is read line by line. Everything after '#' is a comment and
 *      is cut off, and leading spaces are skipped.
 *   2. Lines are ignored until the line that starts with `name`. The next
 *      line that starts with '[' is another section, so reading stops there.
 *   3. Inside the section every number is collected with strtol(), so the
 *      numbers may be spread over the lines in any way.
 *   4. The first number is n; the next n*cols numbers are the rows (cols =
 *      2 or 3); after them come the extra parameters.
 *   5. The values are checked (1 <= n <= MAXP, arrival >= 0, burst >= 1,
 *      tickets >= 1) so a hand-edited file cannot break the simulation.
 * The file is opened again on every call, which is why every menu option
 * really uses what is currently stored in input.txt.
 * Returns 0 on success, -1 (after printing the reason) on any error.
 */
static int load_section(const char *name, int fmt, Workload *w, long *extra, int nextra)
{
    FILE *f = fopen(INPUT_FILE, "r");
    if (!f) { perror(INPUT_FILE); return -1; }

    long nums[1 + MAXP * 3 + 8];
    int cnt = 0, in = 0, found = 0;
    char line[512];
    size_t nlen = strlen(name);

    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';                  /* drop the comment */
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '[') {
            if (in) break;                       /* next section reached */
            if (strncmp(p, name, nlen) == 0) in = found = 1;
            continue;
        }
        if (!in) continue;
        while (*p) {                             /* collect every number */
            if (isspace((unsigned char)*p)) { p++; continue; }
            char *end;
            long v = strtol(p, &end, 10);
            if (end == p) {
                printf("Error: bad value '%c' in section %s of %s\n", *p, name, INPUT_FILE);
                fclose(f);
                return -1;
            }
            if (cnt < (int)(sizeof nums / sizeof nums[0])) nums[cnt++] = v;
            p = end;
        }
    }
    fclose(f);

    if (!found) { printf("Error: section %s not found in %s\n", name, INPUT_FILE); return -1; }
    if (cnt < 1 || nums[0] < 1 || nums[0] > MAXP) {
        printf("Error: section %s needs a process count between 1 and %d\n", name, MAXP);
        return -1;
    }
    int n = (int)nums[0];
    int cols = (fmt == FMT_ABT) ? 3 : 2;
    int need = 1 + n * cols + nextra;
    if (cnt < need) {
        printf("Error: section %s has %d numbers, expected %d\n", name, cnt, need);
        return -1;
    }

    memset(w, 0, sizeof *w);
    w->n = n;
    for (int i = 0; i < n; i++) {
        const long *r = &nums[1 + i * cols];
        Process *pr = &w->p[i];
        pr->pid = i + 1;
        pr->at = r[0];
        pr->bt = r[1];
        pr->tickets = (fmt == FMT_ABT) ? r[2] : 0;
        if (pr->at < 0 || pr->bt < 1 || (fmt == FMT_ABT && pr->tickets < 1)) {
            printf("Error: invalid values for P%d in section %s\n", i + 1, name);
            return -1;
        }
    }
    for (int i = 0; i < nextra; i++) extra[i] = nums[1 + n * cols + i];
    return 0;
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
 */
static void gantt_add(Gantt *g, int pid, long s, long e)
{
    if (e <= s) return;
    if (g->n > 0 && g->s[g->n - 1].pid == pid && g->s[g->n - 1].e == s) {
        g->s[g->n - 1].e = e;
        return;
    }
    if (g->n < MAXSEG) g->s[g->n++] = (Segment){ pid, s, e };
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
 * print_input() - show exactly what was read from input.txt.
 *
 * Prints the section name and a table of Process / Arrival / Burst /
 * Tickets. show_arrival and show_tickets switch the optional columns on or
 * off (only the lottery and compare sections have tickets; every section
 * has arrival times). Column widths are the
 * header widths, which are wide enough for the generated values.
 */
static void print_input(const char *section, const Workload *w, int show_arrival, int show_tickets)
{
    printf("Input read from %s, section %s (n = %d):\n", INPUT_FILE, section, w->n);
    const char *hd[4]; int nc = 0;
    hd[nc++] = "Process";
    if (show_arrival) hd[nc++] = "Arrival (ms)";
    hd[nc++] = "Burst (ms)";
    if (show_tickets) hd[nc++] = "Tickets";
    int wd[4];
    char cells[4][24];
    for (int i = 0; i < nc; i++) { wd[i] = (int)strlen(hd[i]); snprintf(cells[i], 24, "%s", hd[i]); }
    rule(wd, nc); row(wd, nc, cells); rule(wd, nc);
    for (int i = 0; i < w->n; i++) {
        const Process *p = &w->p[i];
        int c = 0;
        snprintf(cells[c++], 24, "P%d", p->pid);
        if (show_arrival) snprintf(cells[c++], 24, "%ld", p->at);
        snprintf(cells[c++], 24, "%ld", p->bt);
        if (show_tickets) snprintf(cells[c++], 24, "%ld", p->tickets);
        row(wd, nc, cells);
    }
    rule(wd, nc);
}

/*
 * print_result_table() - the per-process result table required by the lab:
 *     Process | Arrival | Burst | (Tickets) | Completion | Turnaround |
 *     Waiting | Response
 *
 * `order` lists the process indexes in the order the rows should appear.
 * FCFS and SJF pass their execution order (the lab sheet prints SJF rows
 * in execution order); NULL means plain PID order P1, P2, ...
 */
static void print_result_table(const Workload *w, const int *order, int show_tickets)
{
    const char *hd[8]; int nc = 0;
    hd[nc++] = "Process"; hd[nc++] = "Arrival"; hd[nc++] = "Burst";
    if (show_tickets) hd[nc++] = "Tickets";
    hd[nc++] = "Completion"; hd[nc++] = "Turnaround"; hd[nc++] = "Waiting"; hd[nc++] = "Response";
    int wd[8];
    char cells[8][24];
    for (int i = 0; i < nc; i++) { wd[i] = (int)strlen(hd[i]); snprintf(cells[i], 24, "%s", hd[i]); }
    printf("Result table (all times in ms):\n");
    rule(wd, nc); row(wd, nc, cells); rule(wd, nc);
    for (int k = 0; k < w->n; k++) {
        const Process *p = &w->p[order ? order[k] : k];
        int c = 0;
        snprintf(cells[c++], 24, "P%d", p->pid);
        snprintf(cells[c++], 24, "%ld", p->at);
        snprintf(cells[c++], 24, "%ld", p->bt);
        if (show_tickets) snprintf(cells[c++], 24, "%ld", p->tickets);
        snprintf(cells[c++], 24, "%ld", p->ct);
        snprintf(cells[c++], 24, "%ld", p->tat);
        snprintf(cells[c++], 24, "%ld", p->wt);
        snprintf(cells[c++], 24, "%ld", p->rt);
        row(wd, nc, cells);
    }
    rule(wd, nc);
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
    long sum_tat = 0, sum_wt = 0, sum_rt = 0, max_ct = 0;
    s->busy = 0;
    for (int i = 0; i < w->n; i++) {
        Process *p = &w->p[i];
        p->tat = p->ct - p->at;
        p->wt  = p->tat - p->bt;
        p->rt  = p->st - p->at;
        sum_tat += p->tat; sum_wt += p->wt; sum_rt += p->rt;
        s->busy += p->bt;
        if (p->ct > max_ct) max_ct = p->ct;
    }
    s->elapsed = max_ct;                       /* from time 0, idle included */
    s->avg_tat = (double)sum_tat / w->n;
    s->avg_wt  = (double)sum_wt  / w->n;
    s->avg_rt  = (double)sum_rt  / w->n;
    s->util    = s->elapsed > 0 ? 100.0 * s->busy / s->elapsed : 0.0;
    s->thr     = s->elapsed > 0 ? (double)w->n / s->elapsed : 0.0;
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
 * All five algorithms use this one function to "run" a process.
 */
static void run_piece(Process *p, Gantt *g, long t, long len)
{
    if (!p->started) { p->st = t; p->started = 1; }
    gantt_add(g, p->pid, t, t + len);
    p->rem -= len;
    if (p->rem == 0) p->ct = t + len;
}

/* --------------------------------------------------------------------------
 *  The five scheduling algorithms
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
 *     t=0  pool=35  draw=24 -> P2 runs 0-2  (P2 left 4)
 * verbose = 0 is used for the 100 silent runs of the comparison.
 */
static void lottery(Workload *w, Gantt *g, long q, long seed, int verbose)
{
    reset(w);
    lrng_seed((unsigned long long)seed);
    long t = 0;                                /* the clock starts at 0 */
    int done = 0;
    if (verbose)
        printf("Execution log (tickets numbered in PID order over runnable processes):\n");
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
        long draw = (long)(lrng() % (unsigned long long)pool) + 1, acc = 0;
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
            char span[32], left[32];
            snprintf(span, sizeof span, "%ld-%ld", t, t + slice);
            if (p->rem > 0) snprintf(left, sizeof left, "(P%d left %ld)", p->pid, p->rem);
            else            snprintf(left, sizeof left, "(P%d done)", p->pid);
            printf("  t=%-5ld pool=%-5ld draw=%-5ld -> P%-2d runs %-10s %s\n",
                   t, pool, draw, p->pid, span, left);
        }
        t += slice;
        if (p->rem == 0) done++;
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
    print_input("[FCFS]", &w, 1, 0);
    fcfs(&w, &g, exec);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, exec, 0);
    printf("\n"); print_stats(&s);
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
    print_input("[SJF]", &w, 1, 0);
    sjf(&w, &g, exec);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, exec, 0);
    printf("\n"); print_stats(&s);
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
    print_input("[SRTN]", &w, 1, 0);
    srtn(&w, &g, &pre);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, NULL, 0);
    printf("\n"); print_stats(&s);
    printf("Preemptions             = %d\n", pre);
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
    print_input("[RR]", &w, 1, 0);
    long q = read_long("Enter the time quantum q (ms, 1-100): ", 1, 100);
    printf("\nTime quantum q = %ld ms\n", q);
    round_robin(&w, &g, q, &sw);
    compute_stats(&w, &s);
    print_gantt(&g);
    printf("\n"); print_result_table(&w, NULL, 0);
    printf("\n"); print_stats(&s);
    printf("Process switches        = %d\n", sw);
}

/*
 * menu_lottery() - menu option 5.
 * Reads [LOTTERY]: arrival, burst and tickets per process, then the quantum and the
 * seed (all random values from input.txt). Prints each process's ticket
 * share, then runs lottery() with the execution log switched on, followed
 * by the Gantt chart, result table (with tickets) and averages.
 */
static void menu_lottery(void)
{
    Workload w; Gantt g = { 0 }; Stats s; long ex[2];
    banner("LOTTERY SCHEDULING - probabilistic, preemptive");
    if (load_section("[LOTTERY]", FMT_ABT, &w, ex, 2) < 0) return;
    long q = ex[0], seed = ex[1];
    if (q < 1) { printf("Error: lottery quantum must be >= 1\n"); return; }
    print_input("[LOTTERY]", &w, 1, 1);
    long total = 0;
    for (int i = 0; i < w.n; i++) total += w.p[i].tickets;
    printf("Time quantum q = %ld ms, random seed = %ld, total tickets = %ld (read from %s)\n",
           q, seed, total, INPUT_FILE);
    printf("Ticket shares:");
    for (int i = 0; i < w.n; i++) printf(" P%d=%.1f%%", w.p[i].pid, 100.0 * w.p[i].tickets / total);
    printf("\n\n");
    lottery(&w, &g, q, seed, 1);
    compute_stats(&w, &s);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result_table(&w, NULL, 1);
    printf("\n"); print_stats(&s);
}

/*
 * menu_compare() - menu option 6: all five algorithms on the same data.
 *
 * Reads [COMPARE] (arrival, burst and tickets for every process, then the
 * lottery quantum and seed) and asks for the Round Robin quantum in the
 * terminal. Each algorithm is run on a fresh copy of the same workload and
 * its Gantt chart is printed. Because one lottery run is a matter of luck,
 * the lottery row of the table is the mean over seeds 1..LOTTERY_RUNS; the
 * seed from the file is used for the sample Gantt chart. Finally one table
 * shows average TAT, WT, RT, CPU utilisation and throughput per algorithm,
 * and the best algorithm for each time metric is named (ties are all
 * listed).
 */
static void menu_compare(void)
{
    Workload base, w; long ex[2]; int tmp[MAXP], n_int;
    banner("COMPARISON OF ALL ALGORITHMS (shared [COMPARE] workload)");
    if (load_section("[COMPARE]", FMT_ABT, &base, ex, 2) < 0) return;
    long lq = ex[0], lseed = ex[1];
    if (lq < 1) { printf("Error: lottery quantum must be >= 1\n"); return; }
    print_input("[COMPARE]", &base, 1, 1);
    printf("Lottery: q = %ld ms, sample seed = %ld (read from %s)\n", lq, lseed, INPUT_FILE);
    long q = read_long("Enter the Round Robin time quantum q (ms, 1-100): ", 1, 100);

    const char *names[5];
    Stats st[5];
    char rrname[32], lotname[48];
    snprintf(rrname, sizeof rrname, "RR (q=%ld)", q);
    snprintf(lotname, sizeof lotname, "Lottery (q=%ld, mean %d)", lq, LOTTERY_RUNS);
    names[0] = "FCFS"; names[1] = "SJF"; names[2] = "SRTN"; names[3] = rrname; names[4] = lotname;

    Gantt g;
    w = base; memset(&g, 0, sizeof g); fcfs(&w, &g, tmp);  compute_stats(&w, &st[0]);
    printf("\n[FCFS]\n");    print_gantt(&g);
    w = base; memset(&g, 0, sizeof g); sjf(&w, &g, tmp);   compute_stats(&w, &st[1]);
    printf("\n[SJF]\n");     print_gantt(&g);
    w = base; memset(&g, 0, sizeof g); srtn(&w, &g, &n_int); compute_stats(&w, &st[2]);
    printf("\n[SRTN]  (preemptions: %d)\n", n_int); print_gantt(&g);
    w = base; memset(&g, 0, sizeof g); round_robin(&w, &g, q, &n_int); compute_stats(&w, &st[3]);
    printf("\n[%s]  (process switches: %d)\n", rrname, n_int); print_gantt(&g);
    w = base; memset(&g, 0, sizeof g); lottery(&w, &g, lq, lseed, 0);
    printf("\n[Lottery sample run, seed %ld]\n", lseed); print_gantt(&g);

    memset(&st[4], 0, sizeof st[4]);              /* lottery: mean of many seeds */
    for (long sd = 1; sd <= LOTTERY_RUNS; sd++) {
        Stats one;
        w = base; memset(&g, 0, sizeof g);
        lottery(&w, &g, lq, sd, 0);
        compute_stats(&w, &one);
        st[4].avg_tat += one.avg_tat / LOTTERY_RUNS;
        st[4].avg_wt  += one.avg_wt  / LOTTERY_RUNS;
        st[4].avg_rt  += one.avg_rt  / LOTTERY_RUNS;
        st[4].util    += one.util    / LOTTERY_RUNS;
        st[4].thr     += one.thr     / LOTTERY_RUNS;
    }

    printf("\nINTEGRATED COMPARISON (times in ms)\n");
    printf(" +----------------------------+---------+---------+---------+----------+------------+\n");
    printf(" | %-26s | %7s | %7s | %7s | %8s | %10s |\n",
           "Algorithm", "Avg TAT", "Avg WT", "Avg RT", "CPU %", "Throughput");
    printf(" +----------------------------+---------+---------+---------+----------+------------+\n");
    for (int i = 0; i < 5; i++)
        printf(" | %-26s | %7.2f | %7.2f | %7.2f | %8.2f | %10.4f |\n",
               names[i], st[i].avg_tat, st[i].avg_wt, st[i].avg_rt, st[i].util, st[i].thr);
    printf(" +----------------------------+---------+---------+---------+----------+------------+\n");

    const char *metric[3] = { "turnaround time", "waiting time", "response time" };
    for (int m = 0; m < 3; m++) {
        double best = 1e18;
        for (int i = 0; i < 5; i++) {
            double v = m == 0 ? st[i].avg_tat : m == 1 ? st[i].avg_wt : st[i].avg_rt;
            if (v < best) best = v;
        }
        printf("Best average %-16s: ", metric[m]);
        int first = 1;
        for (int i = 0; i < 5; i++) {
            double v = m == 0 ? st[i].avg_tat : m == 1 ? st[i].avg_wt : st[i].avg_rt;
            if (v - best < 1e-9) { printf("%s%s", first ? "" : ", ", names[i]); first = 0; }
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
    fclose(f);
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
    printf("  6. Compare all algorithms\n");
    printf("  7. Show input.txt\n");
    printf("  0. Exit (input.txt will be deleted)\n");
    printf("=============================================================\n");
}

/*
 * main() - program flow.
 * 1. Line-buffer stdout so output appears in order even when piped.
 * 2. Install the cleanup handlers BEFORE input.txt exists, so the file is
 *    removed however the program ends.
 * 3. Seed rand() with the time and the process id: two runs in the same
 *    second still get different random values.
 * 4. Ask for the number of processes and generate input.txt.
 * 5. Menu loop: run the chosen option, wait for Enter, show the menu again.
 *    Option 0 returns from main(), and atexit() then deletes input.txt.
 */
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_handlers();
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 16));

    printf("=============================================================\n");
    printf("   CPU Scheduling Simulator - CSE-3110 OS Lab, Assignment 02\n");
    printf("=============================================================\n");
    int n = (int)read_long("How many processes do you want? (1-20): ", 1, MAXP);

    if (generate_input(n) < 0) return 1;
    printf("%s created with random parameters for %d processes in sections\n"
           "[FCFS] [SJF] [SRTN] [RR] [LOTTERY] [COMPARE].\n", INPUT_FILE, n);

    for (;;) {
        print_menu();
        long choice = read_long("Choose an option (0-7): ", 0, 7);
        switch (choice) {
        case 1: menu_fcfs();      break;
        case 2: menu_sjf();       break;
        case 3: menu_srtn();      break;
        case 4: menu_rr();        break;
        case 5: menu_lottery();   break;
        case 6: menu_compare();   break;
        case 7: menu_show_file(); break;
        case 0:
            printf("Exiting...\n");
            return 0;
        }
        pause_enter();
    }
}
