/*
 * ============================================================================
 *  CSE-3110  Operating Systems Lab  -  Assignment 03
 *  CPU Scheduling Algorithms : Turnaround Time and Waiting Time
 * ============================================================================
 *
 *  Implemented in ONE C file (no scheduling libraries):
 *     1. FCFS   - First-Come First-Served        (non-preemptive, idle aware)
 *     2. SJF    - Shortest Job First             (non-preemptive, with aging)
 *     3. SRTN   - Shortest Remaining Time Next   (preemptive, event driven)
 *     4. RR     - Round Robin                    (variable quantum q, context
 *                                                 switch cost c)
 *     5. LOTTERY- Lottery scheduling             (probabilistic, random
 *                                                 tickets, seeded RNG)
 *     6. Integrated comparison of all algorithms on one workload
 *
 *  Workload sources (menu):
 *     - Randomly generated 10 / 20 processes, seeded from the SYSTEM TIME
 *     - A text file  (first line n, then n lines "arrival burst [tickets]")
 *     - The built-in "challenge test set" from the lab sheet
 *
 *  Formulae used for EVERY algorithm:
 *     CT  = Completion time  (time the process finishes)
 *     TAT = CT - AT                       (Turnaround time)
 *     WT  = TAT - BT                      (Waiting time)
 *     RT  = ST - AT                       (Response time, ST = first start)
 *     CPU Utilisation = busy time / elapsed time * 100 %
 *     Throughput      = processes completed / elapsed time
 *
 *  Tie-breaking rule (everywhere): earlier arrival first, then lower PID.
 *
 *  Build :  gcc -Wall -O2 scheduling.c -o scheduling -lm
 *  Run   :  ./scheduling                      (interactive menu)
 *           ./scheduling <file> <algo> [q] [c|seed]
 *              algo = fcfs | sjf | srtn | rr | lottery | all
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

#define MAXP        64      /* maximum number of processes                 */
#define GANTT_IDLE  (-1)    /* Gantt segment label: CPU idle               */
#define GANTT_CS    (-2)    /* Gantt segment label: context switch         */

/* ---------------------------------------------------------------------------
 *  Data structures
 * ------------------------------------------------------------------------- */

/* One process (PCB-like record) together with all the computed times. */
typedef struct {
    int  id;          /* process id, P1 .. Pn                              */
    long at;          /* arrival time                                      */
    long bt;          /* burst time (total CPU time needed)                */
    int  tickets;     /* lottery tickets                                   */
    long rem;         /* remaining burst during simulation                 */
    long st;          /* start time (first time it got the CPU)            */
    long ct;          /* completion time                                   */
    long tat;         /* turnaround time                                   */
    long wt;          /* waiting time                                      */
    long rt;          /* response time                                     */
    int  started;     /* 1 once the process has been dispatched            */
    int  done;        /* 1 once the process has finished                   */
} Process;

/* A complete workload: n processes stored in PID order (p[i].id == i+1). */
typedef struct {
    Process p[MAXP];
    int     n;
} Workload;

/* One block of the Gantt chart. pid > 0 = process, -1 = IDLE, -2 = CS. */
typedef struct { int pid; long s, e; } Segment;

/* Dynamically growing Gantt chart. */
typedef struct { Segment *a; int n, cap; } Gantt;

/* Summary statistics for one run of one algorithm. */
typedef struct {
    double avgTAT, avgWT, avgRT;
    double util, throughput;
    long   busy, elapsed, start, end;
    int    ctx;        /* number of context switches              */
    int    preempt;    /* number of preemptions (SRTN)            */
    double ms;         /* wall-clock time of the simulation (ms)  */
} Stats;

/* Extra counters collected by lottery runs. */
typedef struct {
    long wins[MAXP];   /* quanta won while ALL processes were runnable */
    long rounds;       /* number of such quanta                         */
} LotStat;

/* The current workload chosen from the menu. */
static Workload G;
static int      G_loaded = 0;
static char     G_source[160] = "none";


/* ============================================================================
 *  SECTION 1 : Small utilities (time, random numbers, input reading)
 * ========================================================================== */

/*
 * wall_ms()
 * ---------------------------------------------------------------------------
 * Purpose : Return the current value of the system's monotonic clock in
 *           milliseconds.
 * Flow    : 1. Call clock_gettime(CLOCK_MONOTONIC) to read the system clock.
 *           2. Convert seconds + nanoseconds into one double (ms).
 * Used by : Every algorithm runner, which reads the clock before and after
 *           a simulation so the program reports how long the scheduling
 *           calculation really took on this machine (execution measured
 *           according to system time).
 */
static double wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/*
 * print_system_time()
 * ---------------------------------------------------------------------------
 * Purpose : Print the current date and time of the system.
 * Flow    : 1. time(NULL) reads the calendar time in seconds since the epoch.
 *           2. localtime() converts it to the local time zone.
 *           3. strftime() formats it as "YYYY-MM-DD HH:MM:SS".
 * Used by : The banner and every report, so each output is stamped
 *           with the moment it was produced.
 */
static void print_system_time(void)
{
    char buf[64];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", lt);
    printf("System time : %s\n", buf);
}

/*
 * Private random number generator for Lottery scheduling
 * ---------------------------------------------------------------------------
 * rng_seed(seed)
 *   Purpose : Initialise the generator so that the same seed always gives
 *             the same sequence (reproducible runs, as the lab requires).
 *   Flow    : 1. Mix the seed with two large odd constants (splitmix style)
 *                so that small seeds (1, 2, 3 ...) still give very different
 *                internal states.
 *             2. Guard against an all-zero state (xorshift would get stuck).
 *
 * rng_next()
 *   Purpose : Produce the next pseudo-random 32-bit number.
 *   Flow    : 1. Apply the xorshift64* steps (shift-xor three times).
 *             2. Multiply by a constant and return the high 32 bits, which
 *                are the best-distributed bits of xorshift64*.
 *
 * A private generator is used instead of rand() so that lottery results do
 * not depend on the C library and do not disturb the rand() stream used for
 * generating random workloads.
 */
static unsigned long long rng_state = 1;

static void rng_seed(unsigned long long seed)
{
    unsigned long long z = seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    rng_state = z ? z : 1;
}

static unsigned long rng_next(void)
{
    unsigned long long x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return (unsigned long)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

/*
 * read_long(prompt, lo, hi, def)
 * ---------------------------------------------------------------------------
 * Purpose : Safely read one integer from the keyboard.
 * Flow    : 1. Print the prompt and read a whole line with fgets() (so a bad
 *              entry can never leave garbage in the input buffer).
 *           2. An empty line returns the default value 'def'.
 *           3. Convert the text with strtol() and check that the whole line
 *              was a number and lies inside [lo, hi].
 *           4. On a bad entry print an error and ask again.
 *           5. On end-of-file (Ctrl+D or piped input ended) return 'def'.
 * Returns : The validated number.
 */
static long read_long(const char *prompt, long lo, long hi, long def)
{
    char buf[128], *end;
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) { printf("\n"); return def; }
        char *s = buf;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '\0') return def;
        long v = strtol(s, &end, 10);
        while (isspace((unsigned char)*end)) end++;
        if (end != s && *end == '\0' && v >= lo && v <= hi) return v;
        printf("  Invalid input. Enter a number between %ld and %ld.\n", lo, hi);
    }
}

/*
 * read_yes(prompt, def)
 * ---------------------------------------------------------------------------
 * Purpose : Ask a yes/no question.
 * Flow    : 1. Read a line. If it is empty or EOF, return the default.
 *           2. The answer is "yes" if it starts with 'y' or 'Y'; anything
 *              else means "no".
 */
static int read_yes(const char *prompt, int def)
{
    char buf[64];
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) { printf("\n"); return def; }
    char *s = buf;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return def;
    return (*s == 'y' || *s == 'Y');
}

/*
 * read_line(prompt, out, size)
 * ---------------------------------------------------------------------------
 * Purpose : Read a line of text (e.g. a file name).
 * Flow    : 1. fgets() the line, 2. strip the trailing newline / spaces.
 * Returns : 1 if something was read, 0 on EOF or an empty line.
 */
static int read_line(const char *prompt, char *out, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, (int)size, stdin)) return 0;
    size_t L = strlen(out);
    while (L > 0 && isspace((unsigned char)out[L - 1])) out[--L] = '\0';
    return L > 0;
}


/* ============================================================================
 *  SECTION 2 : Gantt chart helpers
 * ========================================================================== */

/*
 * gantt_init(g) / gantt_free(g)
 * ---------------------------------------------------------------------------
 * Purpose : Create an empty Gantt chart / release its memory.
 * Flow    : init sets the array pointer to NULL and both counters to 0; the
 *           array grows on demand in gantt_add(). free releases the array
 *           and resets the struct so it can be reused.
 */
static void gantt_init(Gantt *g) { g->a = NULL; g->n = g->cap = 0; }
static void gantt_free(Gantt *g) { free(g->a); gantt_init(g); }

/*
 * gantt_add(g, pid, s, e, merge)
 * ---------------------------------------------------------------------------
 * Purpose : Append the block [s, e) run by 'pid' (or IDLE / CS) to the chart.
 * Flow    : 1. Ignore empty blocks (e <= s).
 *           2. If 'merge' is set and the previous block belongs to the same
 *              pid and ends exactly at s, just extend it (this keeps the SRTN
 *              chart readable because the event-driven loop runs a process in
 *              several pieces with no gap).
 *           3. Otherwise grow the array if needed (doubling the capacity) and
 *              store the new block.
 *           RR and Lottery call it with merge = 0 so that every quantum is
 *           visible as its own block.
 */
static void gantt_add(Gantt *g, int pid, long s, long e, int merge)
{
    if (e <= s) return;
    if (merge && g->n > 0 && g->a[g->n - 1].pid == pid && g->a[g->n - 1].e == s) {
        g->a[g->n - 1].e = e;
        return;
    }
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 64;
        Segment *na = realloc(g->a, (size_t)g->cap * sizeof *na);
        if (!na) { fprintf(stderr, "Out of memory\n"); exit(1); }
        g->a = na;
    }
    g->a[g->n].pid = pid;
    g->a[g->n].s = s;
    g->a[g->n].e = e;
    g->n++;
}

/*
 * print_gantt(g)
 * ---------------------------------------------------------------------------
 * Purpose : Print the text Gantt chart in the lab's format,
 *              | P2 0-3 | IDLE 3-4 | P4 4-8 |
 * Flow    : 1. Walk over the blocks in time order.
 *           2. Build the label for each block ("P<id>", "IDLE" or "CS") plus
 *              its start-end times.
 *           3. Keep a running line width and start a new line after about
 *              96 characters, so long charts (20 processes, small quantum)
 *              stay readable in the console.
 */
static void print_gantt(const Gantt *g)
{
    printf("\nGantt chart:\n");
    int width = 0;
    char cell[64];
    for (int i = 0; i < g->n; i++) {
        const Segment *sg = &g->a[i];
        if (sg->pid == GANTT_IDLE)
            snprintf(cell, sizeof cell, "| IDLE %ld-%ld ", sg->s, sg->e);
        else if (sg->pid == GANTT_CS)
            snprintf(cell, sizeof cell, "| CS %ld-%ld ", sg->s, sg->e);
        else
            snprintf(cell, sizeof cell, "| P%d %ld-%ld ", sg->pid, sg->s, sg->e);
        int L = (int)strlen(cell);
        if (width + L > 96) { printf("|\n"); width = 0; }
        printf("%s", cell);
        width += L;
    }
    printf("|\n");
}

/*
 * print_exec_order(g)
 * ---------------------------------------------------------------------------
 * Purpose : Print the order in which processes were dispatched, for example
 *           "P4 -> P1 -> P3 -> P2" (this is the SJF/RR "execution order"
 *           output the lab asks for).
 * Flow    : 1. Walk the Gantt chart, skipping IDLE and CS blocks.
 *           2. Print a process id only when it differs from the previous one
 *              (consecutive pieces of the same process count once).
 */
static void print_exec_order(const Gantt *g)
{
    printf("Execution order:");
    int last = 0, first = 1, cnt = 0;
    for (int i = 0; i < g->n; i++) {
        int pid = g->a[i].pid;
        if (pid <= 0 || pid == last) continue;
        if (!first) printf(" ->");
        if (!first && ++cnt % 14 == 0) printf("\n                ");
        printf(" ");
        printf("P%d", pid);
        last = pid;
        first = 0;
    }
    printf("\n");
}

/*
 * same_schedule(a, b)
 * ---------------------------------------------------------------------------
 * Purpose : Decide whether two Gantt charts describe the same schedule.
 * Flow    : 1. Skip IDLE/CS blocks and merge adjacent blocks of the same
 *              process on the fly (RR stores each quantum separately).
 *           2. Compare the resulting (pid, start, end) sequences one by one.
 * Used by : The RR quantum sweep, to show that RR with q > longest burst
 *           produces exactly the FCFS schedule (lab report question 2).
 */
static int same_schedule(const Gantt *a, const Gantt *b)
{
    int i = 0, j = 0;
    for (;;) {
        while (i < a->n && a->a[i].pid <= 0) i++;
        while (j < b->n && b->a[j].pid <= 0) j++;
        if (i >= a->n || j >= b->n) return (i >= a->n && j >= b->n);
        int  pa = a->a[i].pid, pb = b->a[j].pid;
        long sa = a->a[i].s, sb = b->a[j].s;
        long ea = a->a[i].e, eb = b->a[j].e;
        while (i + 1 < a->n && a->a[i + 1].pid == pa && a->a[i + 1].s == ea) ea = a->a[++i].e;
        while (j + 1 < b->n && b->a[j + 1].pid == pb && b->a[j + 1].s == eb) eb = b->a[++j].e;
        if (pa != pb || sa != sb || ea != eb) return 0;
        i++; j++;
    }
}


/* ============================================================================
 *  SECTION 3 : Workload creation (random / file / built-in sets)
 * ========================================================================== */

/*
 * reset_workload(w)
 * ---------------------------------------------------------------------------
 * Purpose : Prepare a workload for a fresh simulation.
 * Flow    : For every process: remaining = burst, start time = -1 (not yet
 *           started), all computed times = 0, started/done flags cleared.
 *           Every algorithm calls this first, so the same workload can be
 *           scheduled again and again by different algorithms.
 */
static void reset_workload(Workload *w)
{
    for (int i = 0; i < w->n; i++) {
        Process *p = &w->p[i];
        p->rem = p->bt;
        p->st = -1;
        p->ct = p->tat = p->wt = p->rt = 0;
        p->started = p->done = 0;
    }
}

/*
 * assign_random_tickets(w)
 * ---------------------------------------------------------------------------
 * Purpose : Give every process a random number of lottery tickets (1..50).
 * Flow    : For each process tickets = rand() % 50 + 1. The share of the
 *           CPU a process can expect is tickets / total tickets, so this
 *           assigns a random win probability to each process.
 */
static void assign_random_tickets(Workload *w)
{
    for (int i = 0; i < w->n; i++) w->p[i].tickets = rand() % 50 + 1;
}

/*
 * generate_random(w, n, seed)
 * ---------------------------------------------------------------------------
 * Purpose : Build a random workload of n processes (normally 10 or 20).
 * Flow    : 1. srand(seed). The caller passes time(NULL), the current system
 *              time, so every run gets a new workload. The seed is printed
 *              so a run can be reproduced.
 *           2. For each process i:
 *                id      = i + 1
 *                arrival = random in 0 .. 1.5*n (arrivals are unsorted and
 *                          sometimes leave the CPU idle, as the lab wants)
 *                burst   = random in 1 .. 20
 *                tickets = random in 1 .. 50
 */
static void generate_random(Workload *w, int n, unsigned seed)
{
    srand(seed);
    w->n = n;
    int span = n + n / 2;
    for (int i = 0; i < n; i++) {
        w->p[i].id = i + 1;
        w->p[i].at = rand() % (span + 1);
        w->p[i].bt = rand() % 20 + 1;
        w->p[i].tickets = rand() % 50 + 1;
    }
    reset_workload(w);
}

/*
 * load_file(w, path)
 * ---------------------------------------------------------------------------
 * Purpose : Read a workload from a text file.
 * Format  :   n
 *             arrival burst [tickets]      <- one line per process
 *           Blank lines and lines starting with '#' are ignored. If the
 *           tickets column is missing, a random ticket count is used.
 * Flow    : 1. Open the file. 2. Find the first meaningful line and read n.
 *           3. Read n process lines with sscanf (2 or 3 fields).
 *           4. Validate: 1 <= n <= MAXP, arrival >= 0, burst >= 1,
 *              tickets >= 1.
 * Returns : 1 on success, 0 on any error (an error message is printed).
 */
static int load_file(Workload *w, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("  Cannot open '%s'.\n", path); return 0; }
    char line[256];
    int n = -1, k = 0;
    Workload t;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '\0' || *s == '#') continue;
        if (n < 0) {
            if (sscanf(s, "%d", &n) != 1 || n < 1 || n > MAXP) {
                printf("  Bad process count (must be 1..%d).\n", MAXP);
                fclose(f);
                return 0;
            }
            continue;
        }
        if (k >= n) break;
        long at, bt;
        int tk;
        int got = sscanf(s, "%ld %ld %d", &at, &bt, &tk);
        if (got < 2 || at < 0 || bt < 1 || (got == 3 && tk < 1)) {
            printf("  Bad process line %d: %s", k + 1, line);
            fclose(f);
            return 0;
        }
        t.p[k].id = k + 1;
        t.p[k].at = at;
        t.p[k].bt = bt;
        t.p[k].tickets = (got == 3) ? tk : rand() % 50 + 1;
        k++;
    }
    fclose(f);
    if (n < 0 || k < n) { printf("  File has fewer process lines than n.\n"); return 0; }
    t.n = n;
    reset_workload(&t);
    *w = t;
    return 1;
}

/*
 * load_table(w, n, at[], bt[], tk[])
 * ---------------------------------------------------------------------------
 * Purpose : Fill a workload from three constant arrays (used by the built-in
 *           sets below).
 * Flow    : Copy arrival, burst and tickets of each process and set id = i+1.
 */
static void load_table(Workload *w, int n, const long *at, const long *bt, const int *tk)
{
    w->n = n;
    for (int i = 0; i < n; i++) {
        w->p[i].id = i + 1;
        w->p[i].at = at[i];
        w->p[i].bt = bt[i];
        w->p[i].tickets = tk[i];
    }
    reset_workload(w);
}

/*
 * load_challenge(w)
 * ---------------------------------------------------------------------------
 * Purpose : Load the "challenge test set" from the lab sheet (listed out of
 *           order on purpose):
 *              P1 4 7 10 | P2 0 3 5 | P3 6 2 20
 *              P4 4 4 10 | P5 9 9 5 | P6 12 4 10
 */
static void load_challenge(Workload *w)
{
    static const long at[] = {4, 0, 6, 4, 9, 12};
    static const long bt[] = {7, 3, 2, 4, 9, 4};
    static const int  tk[] = {10, 5, 20, 10, 5, 10};
    load_table(w, 6, at, bt, tk);
}

/*
 * load_lottery_example(w)
 * ---------------------------------------------------------------------------
 * Purpose : Load the lottery example from the lab sheet: P1 burst 10 with
 *           10 tickets, P2 burst 6 with 20 tickets, P3 burst 4 with 5
 *           tickets, all arriving at 0. Task 5 runs its 1000 simulations on
 *           this set.
 */
static void load_lottery_example(Workload *w)
{
    static const long at[] = {0, 0, 0};
    static const long bt[] = {10, 6, 4};
    static const int  tk[] = {10, 20, 5};
    load_table(w, 3, at, bt, tk);
}

/*
 * load_starvation(w)
 * ---------------------------------------------------------------------------
 * Purpose : Build the SJF starvation input that Task 2 asks for: at least
 *           8 processes, where one long process (P2, burst 10) waits more
 *           than 3 x its burst because short jobs (burst 4) keep arriving
 *           every 4 ms.
 *              P1 0 4 | P2 1 10 | P3..P12 arrive at 2, 6, 10, ..., 38 (burst 4)
 *           Plain SJF: P2 waits 43 ms (> 30). With aging alpha = 5 its
 *           effective burst shrinks and it is picked much earlier.
 */
static void load_starvation(Workload *w)
{
    long at[12], bt[12];
    int  tk[12];
    at[0] = 0; bt[0] = 4;
    at[1] = 1; bt[1] = 10;
    for (int i = 2; i < 12; i++) { at[i] = 2 + 4 * (i - 2); bt[i] = 4; }
    for (int i = 0; i < 12; i++) tk[i] = 10;
    load_table(w, 12, at, bt, tk);
}

/*
 * print_workload(w)
 * ---------------------------------------------------------------------------
 * Purpose : Show the input processes (PID, arrival, burst, tickets) and the
 *           lottery win probability of each process, tickets / total tickets.
 */
static void print_workload(const Workload *w)
{
    long total = 0;
    for (int i = 0; i < w->n; i++) total += w->p[i].tickets;
    printf("\nCurrent workload (%s), %d processes\n", G_source, w->n);
    printf("+------+---------+-------+---------+-------------+\n");
    printf("| PID  | Arrival | Burst | Tickets | Lottery P() |\n");
    printf("+------+---------+-------+---------+-------------+\n");
    for (int i = 0; i < w->n; i++) {
        const Process *p = &w->p[i];
        printf("| P%-3d | %7ld | %5ld | %7d | %10.2f%% |\n",
               p->id, p->at, p->bt, p->tickets,
               total ? 100.0 * p->tickets / total : 0.0);
    }
    printf("+------+---------+-------+---------+-------------+\n");
}


/* ============================================================================
 *  SECTION 4 : Metrics and report printing (shared by all algorithms)
 * ========================================================================== */

/*
 * before(a, b)
 * ---------------------------------------------------------------------------
 * Purpose : The lab's tie-breaking rule as one comparison.
 * Returns : 1 if process a must be preferred to process b: earlier arrival
 *           first, and for equal arrival the lower process id.
 */
static int before(const Process *a, const Process *b)
{
    if (a->at != b->at) return a->at < b->at;
    return a->id < b->id;
}

/*
 * sort_by_arrival(w, order)
 * ---------------------------------------------------------------------------
 * Purpose : Produce the indices of the processes sorted by (arrival, id).
 * Flow    : Insertion sort over an index array (n <= 64, so this is cheap)
 *           using before() as the comparison. The workload itself is not
 *           reordered, so tables are still printed in PID order.
 */
static void sort_by_arrival(const Workload *w, int *order)
{
    for (int i = 0; i < w->n; i++) order[i] = i;
    for (int i = 1; i < w->n; i++) {
        int key = order[i], j = i - 1;
        while (j >= 0 && before(&w->p[key], &w->p[order[j]])) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

/*
 * compute_metrics(w)
 * ---------------------------------------------------------------------------
 * Purpose : Derive TAT, WT and RT of every process after a simulation has
 *           filled in the start time (st) and completion time (ct).
 * Flow    : For every process:
 *              TAT = CT - AT
 *              WT  = TAT - BT  (correct for preemptive AND non-preemptive)
 *              RT  = ST - AT   (time until the process first got the CPU)
 */
static void compute_metrics(Workload *w)
{
    for (int i = 0; i < w->n; i++) {
        Process *p = &w->p[i];
        p->tat = p->ct - p->at;
        p->wt = p->tat - p->bt;
        p->rt = p->st - p->at;
    }
}

/*
 * compute_stats(w, g, s)
 * ---------------------------------------------------------------------------
 * Purpose : Fill the summary statistics of one run.
 * Flow    : 1. Average TAT, WT and RT over all processes.
 *           2. start = earliest arrival, end = latest completion,
 *              elapsed = end - start.
 *           3. busy = total length of the process blocks in the Gantt chart
 *              (IDLE and context-switch blocks are NOT useful work).
 *           4. CPU utilisation = busy / elapsed * 100,
 *              throughput = n / elapsed (processes per ms).
 *           5. Context switches = number of times the Gantt chart moves from
 *              one process to a different process (IDLE in between does not
 *              hide the switch; the first dispatch is not a switch).
 *           s->preempt and s->ms are set by the caller and kept as they are.
 */
static void compute_stats(const Workload *w, const Gantt *g, Stats *s)
{
    double sT = 0, sW = 0, sR = 0;
    long start = w->p[0].at, end = 0;
    for (int i = 0; i < w->n; i++) {
        sT += w->p[i].tat;
        sW += w->p[i].wt;
        sR += w->p[i].rt;
        if (w->p[i].at < start) start = w->p[i].at;
        if (w->p[i].ct > end) end = w->p[i].ct;
    }
    s->avgTAT = sT / w->n;
    s->avgWT = sW / w->n;
    s->avgRT = sR / w->n;
    s->start = start;
    s->end = end;
    s->elapsed = end - start;
    s->busy = 0;
    s->ctx = 0;
    int last = 0;
    for (int i = 0; i < g->n; i++) {
        if (g->a[i].pid <= 0) continue;
        s->busy += g->a[i].e - g->a[i].s;
        if (last && g->a[i].pid != last) s->ctx++;
        last = g->a[i].pid;
    }
    s->util = s->elapsed > 0 ? 100.0 * s->busy / s->elapsed : 0.0;
    s->throughput = s->elapsed > 0 ? (double)w->n / s->elapsed : 0.0;
}

/*
 * print_table(w, show_tickets)
 * ---------------------------------------------------------------------------
 * Purpose : The main console table, one row per process with
 *              PID | AT | BT | [Tickets] | ST | CT | TAT | WT | RT
 *           followed by a TOTAL/AVERAGE row.
 * Flow    : 1. Print the header (with a Tickets column only for Lottery).
 *           2. Print each process in PID order and add up TAT, WT and RT.
 *           3. Print the averages under their columns.
 */
static void print_table(const Workload *w, int show_tickets)
{
    const char *sep = show_tickets
        ? "+------+--------+--------+---------+--------+--------+--------+--------+--------+\n"
        : "+------+--------+--------+--------+--------+--------+--------+--------+\n";
    printf("\n%s", sep);
    if (show_tickets)
        printf("| PID  |   AT   |   BT   | Tickets |   ST   |   CT   |  TAT   |   WT   |   RT   |\n");
    else
        printf("| PID  |   AT   |   BT   |   ST   |   CT   |  TAT   |   WT   |   RT   |\n");
    printf("%s", sep);
    double sT = 0, sW = 0, sR = 0;
    for (int i = 0; i < w->n; i++) {
        const Process *p = &w->p[i];
        if (show_tickets)
            printf("| P%-3d | %6ld | %6ld | %7d | %6ld | %6ld | %6ld | %6ld | %6ld |\n",
                   p->id, p->at, p->bt, p->tickets, p->st, p->ct, p->tat, p->wt, p->rt);
        else
            printf("| P%-3d | %6ld | %6ld | %6ld | %6ld | %6ld | %6ld | %6ld |\n",
                   p->id, p->at, p->bt, p->st, p->ct, p->tat, p->wt, p->rt);
        sT += p->tat; sW += p->wt; sR += p->rt;
    }
    printf("%s", sep);
    if (show_tickets)
        printf("| AVERAGE                                          | %6.2f | %6.2f | %6.2f |\n",
               sT / w->n, sW / w->n, sR / w->n);
    else
        printf("| AVERAGE                                | %6.2f | %6.2f | %6.2f |\n",
               sT / w->n, sW / w->n, sR / w->n);
    printf("%s", sep);
    printf("AT=Arrival BT=Burst ST=Start CT=Completion TAT=Turnaround WT=Waiting RT=Response\n");
}

/*
 * print_stats(s)
 * ---------------------------------------------------------------------------
 * Purpose : Print the summary block below the table: average TAT/WT/RT,
 *           CPU utilisation, throughput, context switches (and preemptions
 *           when they were counted) and the simulation's real (system clock)
 *           execution time.
 */
static void print_stats(const Stats *s)
{
    printf("Average Turnaround Time : %.2f ms\n", s->avgTAT);
    printf("Average Waiting Time    : %.2f ms\n", s->avgWT);
    printf("Average Response Time   : %.2f ms\n", s->avgRT);
    printf("Elapsed time            : %ld ms (from t=%ld to t=%ld), CPU busy %ld ms\n",
           s->elapsed, s->start, s->end, s->busy);
    printf("CPU Utilisation         : %.2f %%\n", s->util);
    printf("Throughput              : %.4f processes/ms\n", s->throughput);
    printf("Context switches        : %d\n", s->ctx);
    if (s->preempt >= 0)
        printf("Preemptions             : %d\n", s->preempt);
    printf("Simulation run time     : %.4f ms (measured with the system clock)\n", s->ms);
}

/*
 * report(title, w, g, s, show_tickets)
 * ---------------------------------------------------------------------------
 * Purpose : Print the full result of one algorithm in a fixed order:
 *           title + system time, Gantt chart, execution order, the
 *           AT/BT/ST/CT/TAT/WT/RT table and the statistics block.
 */
static void report(const char *title, const Workload *w, const Gantt *g,
                   const Stats *s, int show_tickets)
{
    printf("\n==================================================================\n");
    printf(" %s\n", title);
    printf("==================================================================\n");
    print_system_time();
    print_gantt(g);
    print_exec_order(g);
    print_table(w, show_tickets);
    print_stats(s);
}


/* ============================================================================
 *  SECTION 5 : The scheduling algorithms
 * ========================================================================== */

/*
 * fcfs(w, g)  -  First-Come, First-Served (non-preemptive)
 * ---------------------------------------------------------------------------
 * Purpose : Run the processes strictly in arrival order.
 * Flow    : 1. reset_workload(), then sort the indices by (arrival, PID).
 *           2. t = 0 (the clock). For each process in that order:
 *                a. If the CPU is free before the process arrives (t < AT),
 *                   the CPU is IDLE from t to AT: add an IDLE block and
 *                   jump t to AT.
 *                b. ST = t (the process starts now).
 *                c. It runs to completion: t += BT, CT = t.
 *                d. Add the block [ST, CT) to the Gantt chart.
 *           3. compute_metrics() gives TAT, WT and RT
 *              (for FCFS, WT = RT = ST - AT).
 * Complexity : O(n^2) for the insertion sort, O(n) for the simulation.
 */
static void fcfs(Workload *w, Gantt *g)
{
    int order[MAXP];
    reset_workload(w);
    sort_by_arrival(w, order);
    long t = 0;
    for (int k = 0; k < w->n; k++) {
        Process *p = &w->p[order[k]];
        if (t < p->at) {
            gantt_add(g, GANTT_IDLE, t, p->at, 1);
            t = p->at;
        }
        p->st = t;
        p->started = 1;
        t += p->bt;
        p->ct = t;
        p->done = 1;
        gantt_add(g, p->id, p->st, p->ct, 0);
    }
    compute_metrics(w);
}

/*
 * sjf(w, g, alpha)  -  Shortest Job First (non-preemptive) with optional aging
 * ---------------------------------------------------------------------------
 * Purpose : Each time the CPU is free, pick the shortest job among the
 *           processes that have ALREADY arrived.
 * Aging   : When alpha > 0 the selection key is the EFFECTIVE burst
 *              eff = BT - floor(time waited in ready queue / alpha)
 *           where time waited = t - AT (a non-preempted process has never
 *           run before it is chosen). A long process that waits gets a
 *           smaller and smaller key, so it can no longer starve.
 *           alpha = 0 means plain SJF (eff = BT).
 * Flow    : 1. reset_workload(), t = 0, finished = 0.
 *           2. Repeat until all processes are finished:
 *                a. Scan all unfinished processes with AT <= t and keep the
 *                   one with the smallest key. On equal keys use the
 *                   tie rule (earlier arrival, then lower PID).
 *                b. If nobody has arrived yet, the CPU is IDLE until the
 *                   earliest future arrival: add an IDLE block, move t there
 *                   and repeat.
 *                c. Otherwise run the chosen process to completion:
 *                   ST = t, t += BT, CT = t, add its Gantt block.
 *           3. compute_metrics().
 * Complexity : O(n^2).
 */
static void sjf(Workload *w, Gantt *g, long alpha)
{
    reset_workload(w);
    long t = 0;
    int finished = 0;
    while (finished < w->n) {
        int best = -1;
        long bestKey = 0;
        for (int i = 0; i < w->n; i++) {
            Process *p = &w->p[i];
            if (p->done || p->at > t) continue;
            long key = p->bt;
            if (alpha > 0) key -= (t - p->at) / alpha;
            if (best < 0 || key < bestKey ||
                (key == bestKey && before(p, &w->p[best]))) {
                best = i;
                bestKey = key;
            }
        }
        if (best < 0) {                     /* CPU idle until next arrival */
            long next = -1;
            for (int i = 0; i < w->n; i++)
                if (!w->p[i].done && (next < 0 || w->p[i].at < next)) next = w->p[i].at;
            gantt_add(g, GANTT_IDLE, t, next, 1);
            t = next;
            continue;
        }
        Process *p = &w->p[best];
        p->st = t;
        p->started = 1;
        t += p->bt;
        p->ct = t;
        p->rem = 0;
        p->done = 1;
        finished++;
        gantt_add(g, p->id, p->st, p->ct, 0);
    }
    compute_metrics(w);
}

/*
 * srtn(w, g, &preemptions)  -  Shortest Remaining Time Next (preemptive SJF)
 * ---------------------------------------------------------------------------
 * Purpose : Always run the ready process with the smallest REMAINING time;
 *           a new arrival with a strictly shorter remaining time preempts
 *           the running process.
 * Event-driven design (Task 3):
 *           Instead of advancing the clock 1 ms at a time, the clock jumps
 *           straight to the next EVENT, which is either the next arrival or
 *           the completion of the running process, whichever comes first.
 *           The scheduling decision can only change at those instants, so
 *           this gives exactly the same result as the 1 ms loop, but bursts
 *           of 1,000,000 ms need only about 2n iterations.
 * Flow    : 1. reset_workload(), t = 0, cur = -1 (nothing running).
 *           2. Repeat until all processes are finished:
 *                a. Among the ready processes (AT <= t, rem > 0) choose the one
 *                   with the minimum remaining time. On a tie the CURRENT
 *                   process keeps the CPU (no preemption on equal time);
 *                   otherwise use the tie rule (arrival, then PID).
 *                b. If no process is ready, add IDLE up to the next arrival
 *                   and jump there.
 *                c. If the choice differs from the process that is still
 *                   running, that is a PREEMPTION: count it.
 *                d. next_arrival = earliest AT strictly greater than t.
 *                   run_until = min(t + rem[chosen], next_arrival).
 *                e. If the process runs for the first time, ST = t.
 *                   Add [t, run_until) to the Gantt chart (merged with its
 *                   previous piece), lower rem, set t = run_until.
 *                f. If rem == 0: CT = t, finished++, cur = -1.
 *                   Else cur = chosen (it is still running at the event).
 *           3. compute_metrics(), where WT = TAT - BT because the process
 *              may have run in several pieces.
 * Complexity : O(n) events x O(n) scan = O(n^2), independent of burst size.
 */
static void srtn(Workload *w, Gantt *g, int *preemptions)
{
    reset_workload(w);
    long t = 0;
    int finished = 0, cur = -1;
    *preemptions = 0;
    while (finished < w->n) {
        int best = -1;
        for (int i = 0; i < w->n; i++) {
            Process *p = &w->p[i];
            if (p->rem == 0 || p->at > t) continue;
            if (best < 0 || p->rem < w->p[best].rem) { best = i; continue; }
            if (p->rem == w->p[best].rem) {
                if (best == cur) continue;          /* running one keeps CPU */
                if (i == cur || before(p, &w->p[best])) best = i;
            }
        }
        long nextArr = -1;
        for (int i = 0; i < w->n; i++)
            if (w->p[i].at > t && (nextArr < 0 || w->p[i].at < nextArr)) nextArr = w->p[i].at;

        if (best < 0) {                     /* nobody ready : idle */
            gantt_add(g, GANTT_IDLE, t, nextArr, 1);
            t = nextArr;
            cur = -1;
            continue;
        }
        if (cur >= 0 && cur != best && w->p[cur].rem > 0) (*preemptions)++;

        Process *p = &w->p[best];
        long until = t + p->rem;
        if (nextArr >= 0 && nextArr < until) until = nextArr;
        if (!p->started) { p->started = 1; p->st = t; }
        gantt_add(g, p->id, t, until, 1);
        p->rem -= until - t;
        t = until;
        if (p->rem == 0) {
            p->ct = t;
            p->done = 1;
            finished++;
            cur = -1;
        } else {
            cur = best;
        }
    }
    compute_metrics(w);
}

/*
 * round_robin(w, g, q, c)  -  Round Robin with quantum q and switch cost c
 * ---------------------------------------------------------------------------
 * Purpose : Time-sharing: every ready process gets at most q ms of CPU in
 *           turn from a FIFO (circular) ready queue.
 * Parameters : q = time quantum entered by the user (variable),
 *              c = context-switch cost in ms. It is charged every time the
 *                  CPU is dispatched to a DIFFERENT process from the one that
 *                  ran last (also after an idle period), but not for the very
 *                  first dispatch. Switch time is shown as a CS block and is
 *                  not counted as useful CPU work.
 * Flow    : 1. reset_workload(); sort processes by (arrival, PID); enqueue
 *              everything that has arrived by t = 0.
 *           2. Repeat until all processes are finished:
 *                a. Queue empty: the CPU is IDLE until the next arrival. Jump
 *                   there and enqueue the new arrivals.
 *                b. Dequeue the head process P.
 *                c. If P differs from the last process that ran, add a
 *                   context switch: [t, t+c) as CS, t += c, then enqueue
 *                   whatever arrived during the switch.
 *                d. First dispatch of P: ST = t.
 *                e. Run P for slice = min(rem, q): Gantt block, t += slice,
 *                   rem -= slice.
 *                f. Enqueue every process that arrived up to t FIRST (the
 *                   lab rule: a new arrival at the instant a quantum expires
 *                   goes before the preempted process).
 *                g. If P still has work, put it at the TAIL of the queue;
 *                   otherwise CT = t and P is finished.
 *           3. compute_metrics().
 * Notes   : Each process is in the queue at most once, so a circular array
 *           of MAXP entries is enough. If q >= the longest burst every
 *           process finishes in one slice and RR becomes FCFS exactly.
 */
static void round_robin(Workload *w, Gantt *g, long q, long c)
{
    int order[MAXP], queue[MAXP];
    int head = 0, tail = 0, count = 0, k = 0, finished = 0, last = -1;
    reset_workload(w);
    sort_by_arrival(w, order);
    long t = 0;

#define ENQ(x)   do { queue[tail] = (x); tail = (tail + 1) % MAXP; count++; } while (0)
#define ADMIT()  while (k < w->n && w->p[order[k]].at <= t) { ENQ(order[k]); k++; }

    ADMIT();
    while (finished < w->n) {
        if (count == 0) {                   /* idle until next arrival */
            long nt = w->p[order[k]].at;
            gantt_add(g, GANTT_IDLE, t, nt, 1);
            t = nt;
            ADMIT();
            continue;
        }
        int i = queue[head];
        head = (head + 1) % MAXP;
        count--;
        Process *p = &w->p[i];

        if (last >= 0 && last != i && c > 0) {   /* context switch cost */
            gantt_add(g, GANTT_CS, t, t + c, 0);
            t += c;
            ADMIT();
        }
        if (!p->started) { p->started = 1; p->st = t; }
        long slice = p->rem < q ? p->rem : q;
        gantt_add(g, p->id, t, t + slice, 0);
        t += slice;
        p->rem -= slice;
        ADMIT();                            /* new arrivals before P */
        if (p->rem > 0) {
            ENQ(i);
        } else {
            p->ct = t;
            p->done = 1;
            finished++;
        }
        last = i;
    }
#undef ADMIT
#undef ENQ
    compute_metrics(w);
}

/*
 * lottery(w, g, q, seed, verbose, ls)  -  Lottery scheduling
 * ---------------------------------------------------------------------------
 * Purpose : Probabilistic proportional-share scheduling. Each process holds
 *           'tickets'. At every scheduling point a random ticket is drawn,
 *           and its owner runs for one quantum. A process with T tickets out
 *           of a pool of N wins with probability T / N.
 * Flow    : 1. reset_workload(); rng_seed(seed) so a seed always reproduces
 *              the same run.
 *           2. Repeat until all processes are finished:
 *                a. pool = sum of tickets of the READY processes (arrived
 *                   and with work left). Finished processes take their
 *                   tickets out of the pool, so everyone else's share grows.
 *                b. pool == 0 (nobody arrived yet): IDLE until the next
 *                   arrival.
 *                c. draw = random number in 1..pool.
 *                d. Ticket numbers are laid out in PID order over the ready
 *                   processes (e.g. P1 = 1-10, P2 = 11-30, P3 = 31-35). Walk
 *                   that order adding tickets until the running sum >= draw;
 *                   that process is the winner.
 *                e. If ALL processes were runnable at this draw, record
 *                   the winner in 'ls' (used for the win-share statistics).
 *                f. Run the winner for min(q, rem): set ST on its first
 *                   run, add a Gantt block, update rem and t.
 *                g. If verbose, print one log line like the lab sheet:
 *                   t=0   pool=35  draw=24 -> P2 runs 0-2    (P2 left 4)
 *                h. If rem == 0: CT = t, the process is finished.
 *           3. compute_metrics().
 */
static void lottery(Workload *w, Gantt *g, long q, unsigned long seed,
                    int verbose, LotStat *ls)
{
    reset_workload(w);
    rng_seed(seed);
    long t = 0;
    int finished = 0;
    if (verbose) printf("\nLottery execution log (q = %ld, seed = %lu):\n", q, seed);
    while (finished < w->n) {
        long pool = 0;
        int ready = 0;
        for (int i = 0; i < w->n; i++)
            if (!w->p[i].done && w->p[i].at <= t) { pool += w->p[i].tickets; ready++; }

        if (pool == 0) {                    /* nobody ready : idle */
            long next = -1;
            for (int i = 0; i < w->n; i++)
                if (!w->p[i].done && (next < 0 || w->p[i].at < next)) next = w->p[i].at;
            gantt_add(g, GANTT_IDLE, t, next, 1);
            if (verbose) printf("t=%-4ld pool=0    CPU idle until %ld\n", t, next);
            t = next;
            continue;
        }

        long draw = (long)(rng_next() % (unsigned long)pool) + 1;
        long acc = 0;
        int win = -1;
        for (int i = 0; i < w->n; i++) {
            if (w->p[i].done || w->p[i].at > t) continue;
            acc += w->p[i].tickets;
            if (acc >= draw) { win = i; break; }
        }
        if (ls && ready == w->n) { ls->wins[win]++; ls->rounds++; }

        Process *p = &w->p[win];
        if (!p->started) { p->started = 1; p->st = t; }
        long slice = p->rem < q ? p->rem : q;
        gantt_add(g, p->id, t, t + slice, 0);
        p->rem -= slice;
        if (verbose) {
            char run[48], left[32];
            snprintf(run, sizeof run, "P%d runs %ld-%ld", p->id, t, t + slice);
            if (p->rem > 0) snprintf(left, sizeof left, "(P%d left %ld)", p->id, p->rem);
            else snprintf(left, sizeof left, "(P%d done)", p->id);
            printf("t=%-4ld pool=%-4ld draw=%-4ld -> %-18s %s\n", t, pool, draw, run, left);
        }
        t += slice;
        if (p->rem == 0) {
            p->ct = t;
            p->done = 1;
            finished++;
        }
    }
    compute_metrics(w);
}


/* ============================================================================
 *  SECTION 6 : Runners (run one algorithm on a copy of the workload,
 *              measure it with the system clock, print the report)
 * ========================================================================== */

/*
 * run_and_report(algo, src, q, c_or_seed, verbose, out)
 * ---------------------------------------------------------------------------
 * Purpose : One entry point that runs any algorithm on a COPY of the source
 *           workload, so the original workload is never changed.
 * Parameters : algo 0 = FCFS, 1 = SJF (q holds alpha), 2 = SRTN,
 *              3 = RR (q, c), 4 = Lottery (q, seed).
 *              verbose = 1 prints the full report; 0 only computes.
 *              out (optional) receives the statistics.
 * Flow    : 1. Copy the workload, create an empty Gantt chart.
 *           2. Read the system clock, run the algorithm, read the clock
 *              again; the difference is the real execution time.
 *           3. compute_stats(), print the report if requested, free the chart.
 */
static void run_and_report(int algo, const Workload *src, long q, long c,
                           int verbose, Stats *out)
{
    Workload w = *src;
    Gantt g;
    Stats s;
    char title[128];
    memset(&s, 0, sizeof s);
    s.preempt = -1;
    gantt_init(&g);

    double t0 = wall_ms();
    switch (algo) {
    case 0: fcfs(&w, &g);
            snprintf(title, sizeof title, "FCFS - First Come First Served");
            break;
    case 1: sjf(&w, &g, q);
            if (q > 0) snprintf(title, sizeof title, "SJF - Shortest Job First (non-preemptive, aging alpha = %ld)", q);
            else snprintf(title, sizeof title, "SJF - Shortest Job First (non-preemptive)");
            break;
    case 2: srtn(&w, &g, &s.preempt);
            snprintf(title, sizeof title, "SRTN - Shortest Remaining Time Next (event driven)");
            break;
    case 3: round_robin(&w, &g, q, c);
            snprintf(title, sizeof title, "Round Robin (q = %ld ms, context switch cost c = %ld ms)", q, c);
            break;
    default: lottery(&w, &g, q, (unsigned long)c, verbose, NULL);
            snprintf(title, sizeof title, "Lottery Scheduling (q = %ld ms, seed = %ld)", q, c);
            break;
    }
    s.ms = wall_ms() - t0;
    compute_stats(&w, &g, &s);
    if (verbose) report(title, &w, &g, &s, algo == 4);
    if (out) *out = s;
    gantt_free(&g);
}

/*
 * fcfs_convoy_demo(src)
 * ---------------------------------------------------------------------------
 * Purpose : Task 1. Show the CONVOY EFFECT using the same burst times in two
 *           different arrival orders.
 * Flow    : 1. Take the arrival times of the current workload and sort them.
 *           2. Order A: give the LONGEST burst to the earliest arrival
 *              (long job at the head of the queue).
 *              Order B: give the SHORTEST burst to the earliest arrival.
 *           3. Run FCFS on both, print both reports and compare the average
 *              waiting times. Order A is worse because every short job
 *              waits behind the long one (the convoy).
 */
static void fcfs_convoy_demo(const Workload *src)
{
    int n = src->n;
    long at[MAXP], bt[MAXP];
    for (int i = 0; i < n; i++) { at[i] = src->p[i].at; bt[i] = src->p[i].bt; }
    for (int i = 1; i < n; i++)                         /* sort arrivals asc */
        for (int j = i; j > 0 && at[j] < at[j - 1]; j--) { long x = at[j]; at[j] = at[j - 1]; at[j - 1] = x; }
    for (int i = 1; i < n; i++)                         /* sort bursts desc  */
        for (int j = i; j > 0 && bt[j] > bt[j - 1]; j--) { long x = bt[j]; bt[j] = bt[j - 1]; bt[j - 1] = x; }

    Workload A = *src, B = *src;
    for (int i = 0; i < n; i++) {
        A.p[i].at = at[i]; A.p[i].bt = bt[i];           /* long first  */
        B.p[i].at = at[i]; B.p[i].bt = bt[n - 1 - i];   /* short first */
    }
    Stats sa, sb;
    printf("\n>>> Order A: longest burst arrives first");
    run_and_report(0, &A, 0, 0, 1, &sa);
    printf("\n>>> Order B: shortest burst arrives first");
    run_and_report(0, &B, 0, 0, 1, &sb);
    printf("\nCONVOY EFFECT SUMMARY\n");
    printf("  Order A (long first)  : avg WT = %.2f ms, avg TAT = %.2f ms\n", sa.avgWT, sa.avgTAT);
    printf("  Order B (short first) : avg WT = %.2f ms, avg TAT = %.2f ms\n", sb.avgWT, sb.avgTAT);
    printf("  The same bursts in a different order change avg WT by %.2f ms.\n", sa.avgWT - sb.avgWT);
}

/*
 * sjf_aging_demo(src, alpha)
 * ---------------------------------------------------------------------------
 * Purpose : Task 2. Show starvation under plain SJF and how aging fixes it.
 * Flow    : 1. Find the longest process (largest burst, lower PID on a tie).
 *           2. Run SJF without aging and SJF with the given alpha.
 *           3. Report the long process's waiting time in both runs, whether
 *              it starved (WT > 3 x BT), how much its WT dropped, and how the
 *              average WT changed (aging usually raises the average a little,
 *              which is the price of fairness).
 */
static void sjf_aging_demo(const Workload *src, long alpha)
{
    int L = 0;
    for (int i = 1; i < src->n; i++) if (src->p[i].bt > src->p[L].bt) L = i;

    Workload a = *src, b = *src;
    Gantt ga, gb;
    Stats sa, sb;
    memset(&sa, 0, sizeof sa); memset(&sb, 0, sizeof sb);
    sa.preempt = sb.preempt = -1;
    gantt_init(&ga); gantt_init(&gb);
    double t0 = wall_ms(); sjf(&a, &ga, 0);     sa.ms = wall_ms() - t0;
    t0 = wall_ms();        sjf(&b, &gb, alpha); sb.ms = wall_ms() - t0;
    compute_stats(&a, &ga, &sa);
    compute_stats(&b, &gb, &sb);

    report("SJF WITHOUT aging", &a, &ga, &sa, 0);
    char title[96];
    snprintf(title, sizeof title, "SJF WITH aging (alpha = %ld)", alpha);
    report(title, &b, &gb, &sb, 0);

    printf("\nSTARVATION / AGING SUMMARY (long process = P%d, burst %ld)\n",
           a.p[L].id, a.p[L].bt);
    printf("  Without aging : WT(P%d) = %ld ms  -> %s (3 x BT = %ld)\n", a.p[L].id, a.p[L].wt,
           a.p[L].wt > 3 * a.p[L].bt ? "STARVED" : "not starved", 3 * a.p[L].bt);
    printf("  With aging    : WT(P%d) = %ld ms\n", b.p[L].id, b.p[L].wt);
    printf("  Long process waiting time dropped by %ld ms\n", a.p[L].wt - b.p[L].wt);
    printf("  Average WT    : %.2f ms -> %.2f ms (change %+.2f ms)\n",
           sa.avgWT, sb.avgWT, sb.avgWT - sa.avgWT);
    gantt_free(&ga); gantt_free(&gb);
}

/*
 * srtn_vs_sjf(src)
 * ---------------------------------------------------------------------------
 * Purpose : Task 3. Run event-driven SRTN (full report), then compare its
 *           averages and preemption count with non-preemptive SJF on the
 *           same input.
 */
static void srtn_vs_sjf(const Workload *src)
{
    Stats a, b;
    run_and_report(2, src, 0, 0, 1, &a);
    run_and_report(1, src, 0, 0, 0, &b);
    printf("\nSRTN vs SJF on the same input\n");
    printf("  +-----------+---------+---------+---------+-------------+\n");
    printf("  | Algorithm | avg TAT | avg WT  | avg RT  | Preemptions |\n");
    printf("  +-----------+---------+---------+---------+-------------+\n");
    printf("  | SRTN      | %7.2f | %7.2f | %7.2f | %11d |\n", a.avgTAT, a.avgWT, a.avgRT, a.preempt);
    printf("  | SJF       | %7.2f | %7.2f | %7.2f | %11d |\n", b.avgTAT, b.avgWT, b.avgRT, 0);
    printf("  +-----------+---------+---------+---------+-------------+\n");
}

/*
 * rr_sweep(src)
 * ---------------------------------------------------------------------------
 * Purpose : Task 4. Tabulate Round Robin for q = 1, 2, 4, 8, 16 with
 *           context-switch cost c = 0 and c = 1.
 * Flow    : 1. For each c and q, run RR silently and print one table row:
 *              avg TAT, avg WT, avg RT, number of context switches and CPU
 *              utilisation (switch time is not useful work).
 *           2. For each c, report the best quantum by avg TAT and by avg RT.
 *              A very small q pays many switches (overhead, low utilisation);
 *              a very large q turns RR into FCFS (convoy effect, poor
 *              response time).
 *           3. Check: RR with q = longest burst + 1 and c = 0 gives exactly
 *              the FCFS schedule (lab report question 2).
 */
static void rr_sweep(const Workload *src)
{
    static const long qs[] = {1, 2, 4, 8, 16};
    printf("\nROUND ROBIN QUANTUM SWEEP\n");
    print_system_time();
    for (long c = 0; c <= 1; c++) {
        printf("\nContext-switch cost c = %ld ms\n", c);
        printf("+------+---------+---------+---------+----------+----------+\n");
        printf("|  q   | avg TAT | avg WT  | avg RT  | Switches | CPU Util |\n");
        printf("+------+---------+---------+---------+----------+----------+\n");
        long bestT = 0, bestR = 0;
        double bT = 1e18, bR = 1e18;
        for (int i = 0; i < 5; i++) {
            Stats s;
            run_and_report(3, src, qs[i], c, 0, &s);
            printf("| %4ld | %7.2f | %7.2f | %7.2f | %8d | %7.2f%% |\n",
                   qs[i], s.avgTAT, s.avgWT, s.avgRT, s.ctx, s.util);
            if (s.avgTAT < bT) { bT = s.avgTAT; bestT = qs[i]; }
            if (s.avgRT < bR) { bR = s.avgRT; bestR = qs[i]; }
        }
        printf("+------+---------+---------+---------+----------+----------+\n");
        printf("Best q for avg TAT/WT: %ld   Best q for avg RT: %ld\n", bestT, bestR);
    }
    printf("\nWhy: a very small q causes many context switches (overhead, lower\n"
           "utilisation when c > 0); a very large q degenerates into FCFS, so short\n"
           "jobs wait behind long ones (convoy effect, poor response time).\n");

    long maxbt = 0;
    for (int i = 0; i < src->n; i++) if (src->p[i].bt > maxbt) maxbt = src->p[i].bt;
    Workload a = *src, b = *src;
    Gantt ga, gb;
    gantt_init(&ga); gantt_init(&gb);
    fcfs(&a, &ga);
    round_robin(&b, &gb, maxbt + 1, 0);
    printf("\nCheck: RR with q = %ld (> longest burst %ld) equals FCFS schedule? %s\n",
           maxbt + 1, maxbt, same_schedule(&ga, &gb) ? "YES" : "NO");
    gantt_free(&ga); gantt_free(&gb);
}

/*
 * lottery_stats(src, q, runs)
 * ---------------------------------------------------------------------------
 * Purpose : Task 5. Run many seeded lottery simulations (seeds 1..runs) and
 *           measure how the random schedule behaves on average.
 * Flow    : 1. For every seed: run lottery() silently and accumulate each
 *              process's TAT (sum and sum of squares) plus the quanta won
 *              while all processes were runnable (LotStat).
 *           2. After every batch of 100 runs, compare the cumulative mean TAT
 *              with the previous batch and print the largest relative change.
 *              Report the first batch where all means changed by < 1 %.
 *           3. Print per process: mean TAT, standard deviation
 *              sd = sqrt(E[x^2] - E[x]^2), ticket share (tickets / total) and
 *              the measured win share. The win share converges to the ticket
 *              share, which is the proportional-share property.
 */
static void lottery_stats(const Workload *src, long q, long runs)
{
    int n = src->n;
    double sum[MAXP] = {0}, sq[MAXP] = {0}, prev[MAXP] = {0};
    LotStat ls;
    memset(&ls, 0, sizeof ls);
    long converged = -1;
    double t0 = wall_ms();

    printf("\nLOTTERY STATISTICS: %ld runs, seeds 1..%ld, q = %ld\n", runs, runs, q);
    print_system_time();
    printf("Convergence of mean TAT (checked every 100 runs):\n");
    for (long r = 1; r <= runs; r++) {
        Workload w = *src;
        Gantt g;
        gantt_init(&g);
        lottery(&w, &g, q, (unsigned long)r, 0, &ls);
        gantt_free(&g);
        for (int i = 0; i < n; i++) { sum[i] += w.p[i].tat; sq[i] += (double)w.p[i].tat * w.p[i].tat; }
        if (r % 100 == 0) {
            double maxrel = 0;
            for (int i = 0; i < n; i++) {
                double m = sum[i] / r;
                if (r > 100 && prev[i] > 0) {
                    double rel = fabs(m - prev[i]) / prev[i];
                    if (rel > maxrel) maxrel = rel;
                }
                prev[i] = m;
            }
            if (r > 100) {
                printf("  after %5ld runs : max change of mean TAT = %.3f %%\n", r, maxrel * 100);
                if (converged < 0 && maxrel < 0.01) converged = r;
            }
        }
    }
    double ms = wall_ms() - t0;

    long total = 0;
    for (int i = 0; i < n; i++) total += src->p[i].tickets;
    printf("\n+------+-------+---------+----------+---------+--------------+-----------+\n");
    printf("| PID  | Burst | Tickets | Mean TAT | SD TAT  | Ticket share | Win share |\n");
    printf("+------+-------+---------+----------+---------+--------------+-----------+\n");
    for (int i = 0; i < n; i++) {
        double m = sum[i] / runs;
        double var = sq[i] / runs - m * m;
        double sd = var > 0 ? sqrt(var) : 0;
        printf("| P%-3d | %5ld | %7d | %8.2f | %7.2f | %11.2f%% | %8.2f%% |\n",
               src->p[i].id, src->p[i].bt, src->p[i].tickets, m, sd,
               100.0 * src->p[i].tickets / total,
               ls.rounds ? 100.0 * ls.wins[i] / ls.rounds : 0.0);
    }
    printf("+------+-------+---------+----------+---------+--------------+-----------+\n");
    printf("Win share = fraction of quanta won while ALL processes were runnable (%ld quanta).\n", ls.rounds);
    if (converged > 0)
        printf("Mean TATs changed by less than 1%% between batches after %ld runs.\n", converged);
    else
        printf("Mean TATs did not settle below 1%% change in %ld runs.\n", runs);
    printf("Total time for %ld simulations: %.3f ms (system clock)\n", runs, ms);
}

/*
 * compare_all(src)
 * ---------------------------------------------------------------------------
 * Purpose : Task 6. Integrated comparison of every algorithm on one workload:
 *           FCFS, SJF, SRTN, RR (q = 2, c = 0), RR (q = 4, c = 0) and
 *           Lottery (q = 2, mean over seeds 1..100).
 * Flow    : 1. Run each deterministic algorithm once silently.
 *           2. Run Lottery 100 times (seeds 1..100) and average its avg TAT,
 *              WT, RT, utilisation and throughput.
 *           3. Print one comparison table, then name the best algorithm
 *              for each metric (lowest TAT, WT and RT).
 */
static void compare_all(const Workload *src)
{
    const char *names[6] = {"FCFS", "SJF", "SRTN", "RR q=2", "RR q=4", "Lottery q=2*"};
    Stats s[6];
    run_and_report(0, src, 0, 0, 0, &s[0]);
    run_and_report(1, src, 0, 0, 0, &s[1]);
    run_and_report(2, src, 0, 0, 0, &s[2]);
    run_and_report(3, src, 2, 0, 0, &s[3]);
    run_and_report(3, src, 4, 0, 0, &s[4]);
    memset(&s[5], 0, sizeof s[5]);
    for (long seed = 1; seed <= 100; seed++) {
        Stats t;
        run_and_report(4, src, 2, seed, 0, &t);
        s[5].avgTAT += t.avgTAT / 100;  s[5].avgWT += t.avgWT / 100;
        s[5].avgRT  += t.avgRT  / 100;  s[5].util  += t.util  / 100;
        s[5].throughput += t.throughput / 100;
        s[5].ctx += t.ctx;  s[5].ms += t.ms;
    }
    s[5].ctx /= 100;

    printf("\nINTEGRATED COMPARISON (%s, %d processes)\n", G_source, src->n);
    print_system_time();
    printf("+--------------+---------+---------+---------+----------+------------+----------+\n");
    printf("| Algorithm    | avg TAT | avg WT  | avg RT  | CPU Util | Throughput | Switches |\n");
    printf("+--------------+---------+---------+---------+----------+------------+----------+\n");
    int bT = 0, bW = 0, bR = 0;
    for (int i = 0; i < 6; i++) {
        printf("| %-12s | %7.2f | %7.2f | %7.2f | %7.2f%% | %10.4f | %8d |\n",
               names[i], s[i].avgTAT, s[i].avgWT, s[i].avgRT, s[i].util, s[i].throughput, s[i].ctx);
        if (s[i].avgTAT < s[bT].avgTAT) bT = i;
        if (s[i].avgWT < s[bW].avgWT) bW = i;
        if (s[i].avgRT < s[bR].avgRT) bR = i;
    }
    printf("+--------------+---------+---------+---------+----------+------------+----------+\n");
    printf("* Lottery = mean over seeds 1..100 (switches = mean per run).\n");
    printf("Best avg Turnaround Time : %s (%.2f ms)\n", names[bT], s[bT].avgTAT);
    printf("Best avg Waiting Time    : %s (%.2f ms)\n", names[bW], s[bW].avgWT);
    printf("Best avg Response Time   : %s (%.2f ms)\n", names[bR], s[bR].avgRT);
}


/* ============================================================================
 *  SECTION 7 : Menu and main()
 * ========================================================================== */

/*
 * ensure_workload()
 * ---------------------------------------------------------------------------
 * Purpose : Make sure a workload exists before an algorithm runs.
 * Flow    : If nothing is loaded yet, generate 10 random processes seeded
 *           with the current system time and show them.
 */
static void ensure_workload(void)
{
    if (G_loaded) return;
    unsigned seed = (unsigned)time(NULL);
    generate_random(&G, 10, seed);
    snprintf(G_source, sizeof G_source, "random, seed=%u", seed);
    G_loaded = 1;
    printf("No workload loaded - generated 10 random processes (seed %u from system time).\n", seed);
    print_workload(&G);
}

/*
 * print_menu()
 * ---------------------------------------------------------------------------
 * Purpose : Show the user menu with the current workload's source.
 */
static void print_menu(void)
{
    printf("\n================ CPU SCHEDULING SIMULATOR ================\n");
    printf(" Workload: %s (%d processes)\n", G_loaded ? G_source : "none", G_loaded ? G.n : 0);
    printf("----------------------------------------------------------\n");
    printf("  1. Generate random processes (10 / 20, seeded by system time)\n");
    printf("  2. Load processes from file\n");
    printf("  3. Load challenge test set (lab sheet)\n");
    printf("  4. Show current processes\n");
    printf("  5. FCFS\n");
    printf("  6. FCFS convoy-effect demo\n");
    printf("  7. SJF (non-preemptive, optional aging alpha)\n");
    printf("  8. SJF starvation + aging demo\n");
    printf("  9. SRTN (event driven) + comparison with SJF\n");
    printf(" 10. Round Robin (enter quantum q and switch cost c)\n");
    printf(" 11. Round Robin quantum sweep (q = 1,2,4,8,16 ; c = 0,1)\n");
    printf(" 12. Lottery scheduling (enter q and seed, random tickets)\n");
    printf(" 13. Lottery statistics (many seeded runs)\n");
    printf(" 14. Compare all algorithms\n");
    printf("  0. Exit\n");
    printf("==========================================================\n");
}

/*
 * menu_loop()
 * ---------------------------------------------------------------------------
 * Purpose : The interactive console loop.
 * Flow    : 1. Show the menu and read a choice (EOF or 0 exits).
 *           2. Options 1-3 build or load the workload. Option 1 asks for the
 *              number of processes (10 or 20) and seeds rand() with
 *              time(NULL).
 *           3. Options 5-14 make sure a workload exists, ask for the
 *              parameters of the algorithm (quantum, switch cost, alpha,
 *              seed ...) and call the matching runner, which prints the
 *              Gantt chart, the AT/BT/ST/CT/TAT/WT/RT table and the statistics.
 */
static void menu_loop(void)
{
    for (;;) {
        print_menu();
        long ch = read_long("Enter choice: ", 0, 14, 0);
        if (ch == 0) { printf("Goodbye.\n"); return; }

        if (ch >= 5) ensure_workload();
        switch (ch) {
        case 1: {
            long n = read_long("Number of processes (10 or 20) [10]: ", 1, MAXP, 10);
            unsigned seed = (unsigned)time(NULL);
            generate_random(&G, (int)n, seed);
            snprintf(G_source, sizeof G_source, "random, seed=%u", seed);
            G_loaded = 1;
            printf("Generated %ld random processes using system-time seed %u.\n", n, seed);
            print_workload(&G);
            break;
        }
        case 2: {
            char path[256];
            if (read_line("File path: ", path, sizeof path) && load_file(&G, path)) {
                snprintf(G_source, sizeof G_source, "file %.140s", path);
                G_loaded = 1;
                print_workload(&G);
            }
            break;
        }
        case 3:
            load_challenge(&G);
            snprintf(G_source, sizeof G_source, "challenge test set");
            G_loaded = 1;
            print_workload(&G);
            break;
        case 4:
            if (G_loaded) print_workload(&G); else printf("No workload loaded.\n");
            break;
        case 5:
            run_and_report(0, &G, 0, 0, 1, NULL);
            break;
        case 6:
            fcfs_convoy_demo(&G);
            break;
        case 7: {
            long a = read_long("Aging alpha (0 = no aging) [0]: ", 0, 1000000, 0);
            run_and_report(1, &G, a, 0, 1, NULL);
            break;
        }
        case 8: {
            int builtin = read_yes("Use built-in starvation input (12 processes)? (y/n) [y]: ", 1);
            long a = read_long("Aging alpha [5]: ", 1, 1000000, 5);
            if (builtin) {
                Workload s;
                load_starvation(&s);
                sjf_aging_demo(&s, a);
            } else {
                sjf_aging_demo(&G, a);
            }
            break;
        }
        case 9:
            srtn_vs_sjf(&G);
            break;
        case 10: {
            long q = read_long("Time quantum q (ms) [4]: ", 1, 1000000, 4);
            long c = read_long("Context switch cost c (ms) [0]: ", 0, 1000, 0);
            run_and_report(3, &G, q, c, 1, NULL);
            break;
        }
        case 11:
            rr_sweep(&G);
            break;
        case 12: {
            long q = read_long("Time quantum q (ms) [2]: ", 1, 1000000, 2);
            long seed = read_long("Random seed (0 = use system time) [0]: ", 0, 2000000000L, 0);
            if (seed == 0) seed = (long)(time(NULL) % 2000000000L);
            if (read_yes("Re-assign tickets randomly? (y/n) [n]: ", 0)) {
                srand((unsigned)time(NULL));
                assign_random_tickets(&G);
            }
            print_workload(&G);
            run_and_report(4, &G, q, seed, 1, NULL);
            break;
        }
        case 13: {
            int ex = read_yes("Use the lab's lottery example (P1 10/10, P2 6/20, P3 4/5)? (y/n) [y]: ", 1);
            long q = read_long("Time quantum q (ms) [2]: ", 1, 1000000, 2);
            long r = read_long("Number of runs [1000]: ", 1, 1000000, 1000);
            if (ex) {
                Workload e;
                load_lottery_example(&e);
                lottery_stats(&e, q, r);
            } else {
                lottery_stats(&G, q, r);
            }
            break;
        }
        case 14:
            compare_all(&G);
            break;
        }
    }
}

/*
 * main(argc, argv)
 * ---------------------------------------------------------------------------
 * Purpose : Program entry point.
 * Flow    : 1. Print the banner with the current system time and seed
 *              rand() with time(NULL).
 *           2. Command-line mode (Task 6: "input file + algorithm option"):
 *                ./scheduling <file> <fcfs|sjf|srtn|rr|lottery|all> [q] [c|seed]
 *              Load the file, run the requested algorithm and exit.
 *           3. Without arguments, start the interactive menu.
 */
int main(int argc, char **argv)
{
    printf("CSE-3110 OS Lab - CPU Scheduling Algorithms\n");
    print_system_time();
    srand((unsigned)time(NULL));

    if (argc >= 3) {
        if (!load_file(&G, argv[1])) return 1;
        snprintf(G_source, sizeof G_source, "file %.140s", argv[1]);
        G_loaded = 1;
        print_workload(&G);
        const char *a = argv[2];
        long q = argc >= 4 ? atol(argv[3]) : 2;
        long c = argc >= 5 ? atol(argv[4]) : 0;
        if (q < 1) q = 1;
        if (c < 0) c = 0;
        if      (!strcmp(a, "fcfs"))    run_and_report(0, &G, 0, 0, 1, NULL);
        else if (!strcmp(a, "sjf"))     run_and_report(1, &G, argc >= 4 ? atol(argv[3]) : 0, 0, 1, NULL);
        else if (!strcmp(a, "srtn"))    srtn_vs_sjf(&G);
        else if (!strcmp(a, "rr"))      run_and_report(3, &G, q, c, 1, NULL);
        else if (!strcmp(a, "lottery")) run_and_report(4, &G, q, c ? c : 1, 1, NULL);
        else if (!strcmp(a, "all"))     compare_all(&G);
        else {
            printf("Unknown algorithm '%s' (use fcfs|sjf|srtn|rr|lottery|all)\n", a);
            return 1;
        }
        return 0;
    }
    if (argc == 2) {
        printf("Usage: %s [<file> <fcfs|sjf|srtn|rr|lottery|all> [q] [c|seed]]\n", argv[0]);
        return 1;
    }
    menu_loop();
    return 0;
}
