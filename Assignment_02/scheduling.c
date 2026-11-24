#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INPUT_FILE   "input.txt"
#define MAXP         20
#define MAXSEG       4096
#define LOTTERY_RUNS 100

enum { PLAIN, TICKETS, PRIORITY };
enum { BY_ARRIVAL, BY_BURST, BY_PRIORITY, BY_AGED_BURST };

typedef struct { int pid, started; long at, bt, extra, rem, st, ct, tat, wt, rt; } Process;
typedef struct { int n; Process p[MAXP]; } Workload;
typedef struct { int pid; long s, e; } Segment;
typedef struct { int n; Segment s[MAXSEG]; } Gantt;
typedef struct { double m[5]; long busy, elapsed, max_wt; } Stats;

static const char *COLUMN[] = { "", "Tickets", "Priority" };
static volatile sig_atomic_t file_created = 0;

static void cleanup_file(void)
{
    if (file_created && remove(INPUT_FILE) == 0)
        printf("\n%s has been flushed and deleted.\n", INPUT_FILE);
}

static void on_signal(int sig)
{
    static const char msg[] = "\nInterrupted - input.txt flushed and deleted.\n";
    if (file_created) unlink(INPUT_FILE);
    if (write(STDOUT_FILENO, msg, sizeof msg - 1) < 0) { }
    _exit(128 + sig);
}

static void read_line(char *buf, int size)
{
    fflush(stdout);
    if (!fgets(buf, size, stdin)) {
        printf("\n[end of input] Exiting.\n");
        exit(0);
    }
}

static long read_long(const char *prompt, long lo, long hi)
{
    char buf[128], *end;
    for (;;) {
        printf("%s", prompt);
        read_line(buf, sizeof buf);
        long v = strtol(buf, &end, 10);
        while (isspace((unsigned char)*end)) end++;
        if (end != buf && *end == '\0' && v >= lo && v <= hi) return v;
        printf("  Please enter a whole number between %ld and %ld.\n", lo, hi);
    }
}

static long rnd(long lo, long hi) { return lo + rand() % (hi - lo + 1); }

static void write_section(FILE *f, const char *name, int n, int fmt)
{
    static const char *head[] = { "", " tickets", " priority" };
    long at[MAXP], bt[MAXP], extra[MAXP], end = 0;
    for (int i = 0; i < n; i++) {
        at[i] = rnd(0, 3L * n);
        bt[i] = rnd(1, 12);
        extra[i] = fmt == PRIORITY ? rnd(1, 10) : 5 * rnd(1, 10);
    }
    int late = (int)rnd(0, n - 1);
    for (int i = 0; i < n; i++) {
        long t = at[i];
        for (int j = 0; j < n; j++)
            if (j != late && at[j] >= at[i]) t += bt[j];
        if (i != late && t > end) end = t;
    }
    at[late] = end + rnd(1, 4);

    fprintf(f, "\n[%s]\n", name);
    if (fmt == PRIORITY) fprintf(f, "# smaller number = higher priority\n");
    fprintf(f, "%d            # number of processes\n# arrival burst%s\n", n, head[fmt]);
    for (int i = 0; i < n; i++) {
        fprintf(f, "%-4ld %-4ld ", at[i], bt[i]);
        if (fmt == PLAIN) fprintf(f, "         # P%d\n", i + 1);
        else              fprintf(f, "%-4ld     # P%d\n", extra[i], i + 1);
    }
}

static int generate_input(int n)
{
    FILE *f = fopen(INPUT_FILE, "w");
    if (!f) { perror(INPUT_FILE); return -1; }
    file_created = 1;
    fprintf(f, "# input.txt - generated with random values (deleted on exit)\n"
               "# Every section: n, one row per process (P1..Pn), then its parameters.\n"
               "# The Round Robin time quantum is NOT stored here: it is asked in the terminal.\n");
    write_section(f, "FCFS", n, PLAIN);
    write_section(f, "SJF", n, PLAIN);
    write_section(f, "SRTN", n, PLAIN);
    write_section(f, "RR", n, PLAIN);
    write_section(f, "LOTTERY", n, TICKETS);
    fprintf(f, "%-12ld # time quantum q (ms)\n", rnd(2, 4));
    fprintf(f, "%-12ld # random seed\n", rnd(1, 9999));
    write_section(f, "COMPARE", n, TICKETS);
    fprintf(f, "%-12ld # lottery time quantum q (ms)\n", rnd(2, 4));
    fprintf(f, "%-12ld # lottery random seed (single sample run)\n", rnd(1, 9999));
    write_section(f, "PRIORITY", n, PRIORITY);
    write_section(f, "SJF_AGING", n, PLAIN);
    fprintf(f, "%-12ld # aging factor alpha (ms)\n", rnd(2, 5));
    fclose(f);
    return 0;
}

static int load_section(const char *name, int fmt, Workload *w, long *par, int npar)
{
    FILE *f = fopen(INPUT_FILE, "r");
    if (!f) { perror(INPUT_FILE); return -1; }
    long nums[1 + MAXP * 3 + 8];
    int cnt = 0, in = 0;
    char line[512], *end;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "#")] = '\0';
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '[') {
            if (in) break;
            in = strncmp(p, name, strlen(name)) == 0;
            continue;
        }
        while (in && *p) {
            if (isspace((unsigned char)*p)) { p++; continue; }
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

    if (!in) { printf("Error: section %s not found in %s\n", name, INPUT_FILE); return -1; }
    if (cnt < 1 || nums[0] < 1 || nums[0] > MAXP) {
        printf("Error: section %s needs a process count between 1 and %d\n", name, MAXP);
        return -1;
    }
    int n = (int)nums[0], cols = fmt == PLAIN ? 2 : 3, need = 1 + n * cols + npar;
    if (cnt < need) {
        printf("Error: section %s has %d numbers, expected %d\n", name, cnt, need);
        return -1;
    }
    memset(w, 0, sizeof *w);
    w->n = n;
    for (int i = 0; i < n; i++) {
        const long *r = &nums[1 + i * cols];
        w->p[i] = (Process){ .pid = i + 1, .at = r[0], .bt = r[1], .extra = fmt == PLAIN ? 0 : r[2] };
        if (r[0] < 0 || r[1] < 1 || w->p[i].extra < (fmt == TICKETS)) {
            printf("Error: invalid values for P%d in section %s\n", i + 1, name);
            return -1;
        }
    }
    for (int i = 0; i < npar; i++) par[i] = nums[1 + n * cols + i];
    return 0;
}

static void print_gantt(const Gantt *g)
{
    int col = 3;
    printf("Gantt chart:\n  |");
    for (int i = 0; i < g->n; i++) {
        char name[16] = "IDLE", cell[64];
        if (g->s[i].pid) snprintf(name, sizeof name, "P%d", g->s[i].pid);
        int len = snprintf(cell, sizeof cell, " %s %ld-%ld |", name, g->s[i].s, g->s[i].e);
        if (col + len > 96) { printf("\n  |"); col = 3; }
        printf("%s", cell);
        col += len;
    }
    printf("\n");
}

static void rule(const char **hd, int nc)
{
    printf(" +");
    for (int i = 0; i < nc; i++) {
        for (size_t j = 0; j < strlen(hd[i]) + 2; j++) putchar('-');
        putchar('+');
    }
    putchar('\n');
}

static void print_table(const char **hd, int nc, long v[][8], int n)
{
    rule(hd, nc);
    printf(" |");
    for (int i = 0; i < nc; i++) printf(" %s |", hd[i]);
    printf("\n");
    rule(hd, nc);
    for (int k = 0; k < n; k++) {
        char pid[24];
        snprintf(pid, sizeof pid, "P%ld", v[k][0]);
        printf(" | %7s |", pid);
        for (int i = 1; i < nc; i++) printf(" %*ld |", (int)strlen(hd[i]), v[k][i]);
        printf("\n");
    }
    rule(hd, nc);
}

static void print_input(const char *section, const Workload *w, int fmt)
{
    const char *hd[] = { "Process", "Arrival (ms)", "Burst (ms)", COLUMN[fmt] };
    long v[MAXP][8];
    for (int i = 0; i < w->n; i++) {
        const Process *p = &w->p[i];
        v[i][0] = p->pid; v[i][1] = p->at; v[i][2] = p->bt; v[i][3] = p->extra;
    }
    printf("Input read from %s, section %s (n = %d):\n", INPUT_FILE, section, w->n);
    print_table(hd, fmt == PLAIN ? 3 : 4, v, w->n);
}

static void print_result(const Workload *w, const int *order, int fmt)
{
    const char *hd[8] = { "Process", "Arrival", "Burst" };
    int c = 3;
    long v[MAXP][8];
    if (fmt != PLAIN) hd[c++] = COLUMN[fmt];
    hd[c] = "Completion"; hd[c + 1] = "Turnaround"; hd[c + 2] = "Waiting"; hd[c + 3] = "Response";
    for (int k = 0; k < w->n; k++) {
        const Process *p = &w->p[order ? order[k] : k];
        v[k][0] = p->pid; v[k][1] = p->at; v[k][2] = p->bt; v[k][3] = p->extra;
        v[k][c] = p->ct; v[k][c + 1] = p->tat; v[k][c + 2] = p->wt; v[k][c + 3] = p->rt;
    }
    printf("Result table (all times in ms):\n");
    print_table(hd, c + 4, v, w->n);
}

static Stats compute_stats(Workload *w)
{
    Stats s = { 0 };
    for (int i = 0; i < w->n; i++) {
        Process *p = &w->p[i];
        p->tat = p->ct - p->at;
        p->wt  = p->tat - p->bt;
        p->rt  = p->st - p->at;
        s.m[0] += p->tat; s.m[1] += p->wt; s.m[2] += p->rt;
        s.busy += p->bt;
        if (p->ct > s.elapsed) s.elapsed = p->ct;
        if (p->wt > s.max_wt) s.max_wt = p->wt;
    }
    for (int i = 0; i < 3; i++) s.m[i] /= w->n;
    s.m[3] = 100.0 * s.busy / s.elapsed;
    s.m[4] = (double)w->n / s.elapsed;
    return s;
}

static void print_stats(const Stats *s)
{
    printf("Average Turnaround Time = %.2f ms\n", s->m[0]);
    printf("Average Waiting Time    = %.2f ms\n", s->m[1]);
    printf("Average Response Time   = %.2f ms\n", s->m[2]);
    printf("CPU Utilisation         = %.2f %%  (busy %ld ms + idle %ld ms = %ld ms)\n",
           s->m[3], s->busy, s->elapsed - s->busy, s->elapsed);
    printf("Throughput              = %.4f processes/ms\n", s->m[4]);
}

static void banner(const char *title)
{
    const char *line = "==========================================================================";
    printf("\n%s\n  %s\n%s\n", line, title, line);
}

static void reset(Workload *w, Gantt *g)
{
    g->n = 0;
    for (int i = 0; i < w->n; i++) { w->p[i].rem = w->p[i].bt; w->p[i].started = 0; }
}

static void gantt_add(Gantt *g, int pid, long s, long e)
{
    if (g->n > 0 && g->s[g->n - 1].pid == pid) g->s[g->n - 1].e = e;
    else if (g->n < MAXSEG) g->s[g->n++] = (Segment){ pid, s, e };
}

static long run_piece(Process *p, Gantt *g, long t, long len)
{
    if (!p->started) { p->st = t; p->started = 1; }
    gantt_add(g, p->pid, t, t + len);
    p->rem -= len;
    if (p->rem == 0) p->ct = t + len;
    return t + len;
}

static long next_arrival(const Workload *w, long t)
{
    long na = LONG_MAX;
    for (int i = 0; i < w->n; i++)
        if (w->p[i].rem > 0 && w->p[i].at > t && w->p[i].at < na) na = w->p[i].at;
    return na;
}

static long idle(const Workload *w, Gantt *g, long t, int verbose)
{
    long na = next_arrival(w, t);
    if (verbose) printf("  t=%-5ld CPU idle until %ld\n", t, na);
    gantt_add(g, 0, t, na);
    return na;
}

static long rank(const Process *p, long t, int mode, long alpha)
{
    if (mode == BY_BURST)      return p->rem;
    if (mode == BY_PRIORITY)   return p->extra;
    if (mode == BY_AGED_BURST) return p->rem - (t - p->at) / alpha;
    return p->at;
}

static int pick(const Workload *w, long t, int mode, long alpha)
{
    int best = -1;
    for (int i = 0; i < w->n; i++) {
        if (w->p[i].rem == 0 || w->p[i].at > t) continue;
        if (best >= 0) {
            long a = rank(&w->p[i], t, mode, alpha), b = rank(&w->p[best], t, mode, alpha);
            if (a > b || (a == b && w->p[i].at >= w->p[best].at)) continue;
        }
        best = i;
    }
    return best;
}

static void non_preemptive(Workload *w, Gantt *g, int *exec, int mode, long alpha)
{
    int aging = mode == BY_AGED_BURST;
    long t = 0;
    reset(w, g);
    if (aging)
        printf("Execution log (effective burst = burst - floor(waited / %ld), smallest runs):\n", alpha);
    for (int done = 0; done < w->n; ) {
        int x = pick(w, t, mode, alpha), ready = 0;
        if (x < 0) { t = idle(w, g, t, aging); continue; }
        Process *p = &w->p[x];
        if (aging) {
            long eff = rank(p, t, mode, alpha);
            for (int i = 0; i < w->n; i++) ready += w->p[i].rem > 0 && w->p[i].at <= t;
            printf("  t=%-5ld ready=%-3d P%d: burst %ld - aged %ld (waited %ld) = %ld -> runs %ld-%ld\n",
                   t, ready, p->pid, p->bt, p->bt - eff, t - p->at, eff, t, t + p->bt);
        }
        t = run_piece(p, g, t, p->bt);
        exec[done++] = x;
    }
}

static int srtn(Workload *w, Gantt *g)
{
    int cur = -1, preemptions = 0;
    long t = 0;
    reset(w, g);
    for (int done = 0; done < w->n; ) {
        int best = pick(w, t, BY_BURST, 0);
        if (best < 0) { t = idle(w, g, t, 0); continue; }
        if (cur >= 0 && w->p[cur].rem > 0) {
            if (w->p[cur].rem <= w->p[best].rem) best = cur;
            else preemptions++;
        }
        cur = best;
        Process *p = &w->p[cur];
        long na = next_arrival(w, t);
        t = run_piece(p, g, t, na - t < p->rem ? na - t : p->rem);
        if (p->rem == 0) done++;
    }
    return preemptions;
}

static int round_robin(Workload *w, Gantt *g, long q)
{
    int ord[MAXP], queue[MAXP], head = 0, count = 0, k = 0, last = -1, switches = 0;
    long t = 0;
    non_preemptive(w, g, ord, BY_ARRIVAL, 0);
    reset(w, g);
    for (int done = 0; done < w->n; ) {
        int x = -1;
        if (count > 0) {
            x = queue[head];
            head = (head + 1) % MAXP;
            count--;
            if (last >= 0 && last != x) switches++;
            t = run_piece(&w->p[x], g, t, w->p[x].rem < q ? w->p[x].rem : q);
            last = x;
        } else if (w->p[ord[k]].at > t) {
            t = idle(w, g, t, 0);
        }
        while (k < w->n && w->p[ord[k]].at <= t) queue[(head + count++) % MAXP] = ord[k++];
        if (x >= 0) {
            if (w->p[x].rem > 0) queue[(head + count++) % MAXP] = x;
            else done++;
        }
    }
    return switches;
}

static unsigned long long rng_state;

static unsigned long long lrng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void lottery(Workload *w, Gantt *g, long q, long seed, int verbose)
{
    long t = 0;
    reset(w, g);
    rng_state = (unsigned long long)seed * 6364136223846793005ULL + 1442695040888963407ULL;
    if (rng_state == 0) rng_state = 88172645463325252ULL;
    if (verbose)
        printf("Execution log (tickets numbered in PID order over runnable processes):\n");
    for (int done = 0; done < w->n; ) {
        long pool = 0, acc = 0;
        for (int i = 0; i < w->n; i++)
            if (w->p[i].rem > 0 && w->p[i].at <= t) pool += w->p[i].extra;
        if (pool == 0) { t = idle(w, g, t, verbose); continue; }
        long draw = (long)(lrng() % (unsigned long long)pool) + 1;
        int win = -1;
        while (acc < draw) {
            win++;
            if (w->p[win].rem > 0 && w->p[win].at <= t) acc += w->p[win].extra;
        }
        Process *p = &w->p[win];
        long end = run_piece(p, g, t, p->rem < q ? p->rem : q);
        if (verbose) {
            char span[48], left[48];
            snprintf(span, sizeof span, "%ld-%ld", t, end);
            if (p->rem > 0) snprintf(left, sizeof left, "(P%d left %ld)", p->pid, p->rem);
            else            snprintf(left, sizeof left, "(P%d done)", p->pid);
            printf("  t=%-5ld pool=%-5ld draw=%-5ld -> P%-2d runs %-10s %s\n",
                   t, pool, draw, p->pid, span, left);
        }
        t = end;
        if (p->rem == 0) done++;
    }
}

static void run(int opt)
{
    static const struct { const char *title, *sec; int fmt, npar; } A[10] = {
        [1] = { "FIRST-COME, FIRST-SERVED (FCFS) - non-preemptive", "[FCFS]", PLAIN, 0 },
        [2] = { "SHORTEST JOB FIRST (SJF) - non-preemptive", "[SJF]", PLAIN, 0 },
        [3] = { "SHORTEST REMAINING TIME NEXT (SRTN) - preemptive, event-driven", "[SRTN]", PLAIN, 0 },
        [4] = { "ROUND ROBIN (RR) - preemptive, time quantum from the terminal", "[RR]", PLAIN, 0 },
        [5] = { "LOTTERY SCHEDULING - probabilistic, preemptive", "[LOTTERY]", TICKETS, 2 },
        [8] = { "PRIORITY SCHEDULING - non-preemptive, smaller number = higher priority", "[PRIORITY]", PRIORITY, 0 },
        [9] = { "SHORTEST JOB FIRST (SJF) WITH AGING - non-preemptive", "[SJF_AGING]", PLAIN, 1 },
    };
    Workload w, plain;
    Gantt g = { 0 };
    long par[2] = { 1, 0 }, q, total = 0;
    int exec[MAXP], count = 0, fmt = A[opt].fmt;

    banner(A[opt].title);
    if (load_section(A[opt].sec, fmt, &w, par, A[opt].npar) < 0) return;
    if (par[0] < 1) {
        printf("Error: %s must be >= 1\n", opt == 5 ? "lottery quantum" : "aging factor alpha");
        return;
    }
    plain = w;
    print_input(A[opt].sec, &w, fmt);
    switch (opt) {
    case 1: non_preemptive(&w, &g, exec, BY_ARRIVAL, 0);  break;
    case 2: non_preemptive(&w, &g, exec, BY_BURST, 0);    break;
    case 3: count = srtn(&w, &g);                         break;
    case 4:
        q = read_long("Enter the time quantum q (ms, 1-100): ", 1, 100);
        printf("\nTime quantum q = %ld ms", q);
        count = round_robin(&w, &g, q);
        break;
    case 5:
        for (int i = 0; i < w.n; i++) total += w.p[i].extra;
        printf("Time quantum q = %ld ms, random seed = %ld, total tickets = %ld (read from %s)\nTicket shares:",
               par[0], par[1], total, INPUT_FILE);
        for (int i = 0; i < w.n; i++) printf(" P%d=%.1f%%", i + 1, 100.0 * w.p[i].extra / total);
        printf("\n\n");
        lottery(&w, &g, par[0], par[1], 1);
        break;
    case 8: non_preemptive(&w, &g, exec, BY_PRIORITY, 0); break;
    case 9:
        printf("Aging factor alpha = %ld ms (read from %s)\n\n", par[0], INPUT_FILE);
        non_preemptive(&w, &g, exec, BY_AGED_BURST, par[0]);
        break;
    }
    Stats s = compute_stats(&w);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result(&w, opt >= 3 && opt <= 5 ? NULL : exec, fmt);
    printf("\n"); print_stats(&s);
    if (opt == 3) printf("Preemptions             = %d\n", count);
    if (opt == 4) printf("Process switches        = %d\n", count);
    if (opt == 9) {
        non_preemptive(&plain, &g, exec, BY_BURST, 0);
        Stats ps = compute_stats(&plain);
        printf("\nEffect of aging on the same data:\n");
        printf("  Average waiting time  : SJF %.2f ms -> with aging %.2f ms\n", ps.m[1], s.m[1]);
        printf("  Longest waiting time  : SJF %ld ms -> with aging %ld ms\n", ps.max_wt, s.max_wt);
    }
}

static void compare(void)
{
    static const char *metric[] = { "turnaround time", "waiting time", "response time" };
    const char *line = " +----------------------------+---------+---------+---------+----------+------------+\n";
    char names[5][64] = { "FCFS", "SJF", "SRTN" };
    Workload w;
    Gantt g;
    Stats st[5];
    long par[2];
    int exec[MAXP], count;

    banner("COMPARISON OF ALL ALGORITHMS (shared [COMPARE] workload)");
    if (load_section("[COMPARE]", TICKETS, &w, par, 2) < 0) return;
    if (par[0] < 1) { printf("Error: lottery quantum must be >= 1\n"); return; }
    print_input("[COMPARE]", &w, TICKETS);
    printf("Lottery: q = %ld ms, sample seed = %ld (read from %s)\n", par[0], par[1], INPUT_FILE);
    long q = read_long("Enter the Round Robin time quantum q (ms, 1-100): ", 1, 100);
    snprintf(names[3], sizeof names[3], "RR (q=%ld)", q);
    snprintf(names[4], sizeof names[4], "Lottery (q=%ld, mean %d)", par[0], LOTTERY_RUNS);

    non_preemptive(&w, &g, exec, BY_ARRIVAL, 0); st[0] = compute_stats(&w);
    printf("\n[FCFS]\n"); print_gantt(&g);
    non_preemptive(&w, &g, exec, BY_BURST, 0); st[1] = compute_stats(&w);
    printf("\n[SJF]\n"); print_gantt(&g);
    count = srtn(&w, &g); st[2] = compute_stats(&w);
    printf("\n[SRTN]  (preemptions: %d)\n", count); print_gantt(&g);
    count = round_robin(&w, &g, q); st[3] = compute_stats(&w);
    printf("\n[%s]  (process switches: %d)\n", names[3], count); print_gantt(&g);
    lottery(&w, &g, par[0], par[1], 0);
    printf("\n[Lottery sample run, seed %ld]\n", par[1]); print_gantt(&g);

    memset(&st[4], 0, sizeof st[4]);
    for (long seed = 1; seed <= LOTTERY_RUNS; seed++) {
        lottery(&w, &g, par[0], seed, 0);
        Stats one = compute_stats(&w);
        for (int m = 0; m < 5; m++) st[4].m[m] += one.m[m] / LOTTERY_RUNS;
    }

    printf("\nINTEGRATED COMPARISON (times in ms)\n%s", line);
    printf(" | %-26s | %7s | %7s | %7s | %8s | %10s |\n%s",
           "Algorithm", "Avg TAT", "Avg WT", "Avg RT", "CPU %", "Throughput", line);
    for (int i = 0; i < 5; i++)
        printf(" | %-26s | %7.2f | %7.2f | %7.2f | %8.2f | %10.4f |\n", names[i],
               st[i].m[0], st[i].m[1], st[i].m[2], st[i].m[3], st[i].m[4]);
    printf("%s", line);
    for (int m = 0; m < 3; m++) {
        double best = st[0].m[m];
        const char *sep = "";
        for (int i = 1; i < 5; i++) if (st[i].m[m] < best) best = st[i].m[m];
        printf("Best average %-16s: ", metric[m]);
        for (int i = 0; i < 5; i++)
            if (st[i].m[m] - best < 1e-9) { printf("%s%s", sep, names[i]); sep = ", "; }
        printf("  (%.2f ms)\n", best);
    }
}

static void show_file(void)
{
    char line[512];
    banner("CONTENTS OF input.txt");
    FILE *f = fopen(INPUT_FILE, "r");
    if (!f) { perror(INPUT_FILE); return; }
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    fclose(f);
}

int main(void)
{
    char buf[64];
    struct sigaction sa = { .sa_handler = on_signal };
    setvbuf(stdout, NULL, _IOLBF, 0);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    atexit(cleanup_file);
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 16));

    printf("=============================================================\n"
           "   CPU Scheduling Simulator - CSE-3110 OS Lab, Assignment 02\n"
           "=============================================================\n");
    int n = (int)read_long("How many processes do you want? (1-20): ", 1, MAXP);
    if (generate_input(n) < 0) return 1;
    printf("%s created with random parameters for %d processes in sections\n"
           "[FCFS] [SJF] [SRTN] [RR] [LOTTERY] [COMPARE] [PRIORITY] [SJF_AGING].\n", INPUT_FILE, n);

    for (;;) {
        printf("\n==================== CPU SCHEDULING MENU ====================\n"
               "  1. First-Come, First-Served (FCFS)\n"
               "  2. Shortest Job First (SJF)\n"
               "  3. Shortest Remaining Time Next (SRTN)\n"
               "  4. Round Robin (RR)  - time quantum asked here\n"
               "  5. Lottery Scheduling\n"
               "  6. Compare all algorithms\n"
               "  7. Show input.txt\n"
               "  8. Priority Scheduling\n"
               "  9. Shortest Job First with Aging (SJF Aging)\n"
               "  0. Exit (input.txt will be deleted)\n"
               "=============================================================\n");
        int choice = (int)read_long("Choose an option (0-9): ", 0, 9);
        if (choice == 0) { printf("Exiting...\n"); return 0; }
        if (choice == 6)      compare();
        else if (choice == 7) show_file();
        else                  run(choice);
        printf("\nPress Enter to return to the menu...");
        read_line(buf, sizeof buf);
    }
}
