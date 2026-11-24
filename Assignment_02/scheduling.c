#define _POSIX_C_SOURCE 200809L
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
#define MAXP         20
#define MAXVALUE     100000000L
#define MAXSLICES    500000L
#define LOTTERY_RUNS 100
#define ALGS         7

enum { PLAIN = 0, TICKETS = 1, PRIORITY = 2, FULL = TICKETS | PRIORITY };
enum { BY_ARRIVAL, BY_BURST, BY_PRIORITY, BY_AGED_BURST };
enum { FCFS, SJF, SRTN, RR, LOTTERY, PRIO, AGING };

typedef struct { int pid, started; long at, bt, tickets, prio, rem, st, ct, tat, wt, rt; } Process;
typedef struct { int n; Process p[MAXP]; } Workload;
typedef struct { int pid; long s, e; } Segment;
typedef struct { int n, cap; Segment *s; } Gantt;
typedef struct { double m[5]; long busy, elapsed, max_wt; } Stats;
typedef char Cell[48];
typedef struct {
    Workload w[ALGS];
    Gantt g[ALGS];
    Stats st[ALGS];
    Cell name[ALGS], note[ALGS];
    long par[3], q;
} Comparison;

static const char *METRIC[] = { "turnaround time", "waiting time", "response time" };
static volatile sig_atomic_t file_created = 0;

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

static void install_handlers(void)
{
    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE };
    struct sigaction sa = { .sa_handler = on_signal };
    sigfillset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
    atexit(cleanup_file);
}

static void hold_signals(int hold)
{
    static sigset_t old;
    sigset_t all;
    sigfillset(&all);
    if (hold) sigprocmask(SIG_BLOCK, &all, &old);
    else      sigprocmask(SIG_SETMASK, &old, NULL);
}

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

static int finish_file(FILE *f, const char *path)
{
    errno = 0;
    int bad = fflush(f) != 0 || ferror(f), e = errno;
    if (fclose(f) != 0) { if (!bad) e = errno; bad = 1; }
    if (!bad) return 0;
    fprintf(stderr, "Error: %s could not be written completely: %s\n", path, e ? strerror(e) : "write error");
    return -1;
}

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

static int parse_number(char **p, long *v)
{
    char *end;
    errno = 0;
    *v = strtol(*p, &end, 10);
    if (end == *p || errno == ERANGE || (*end != '\0' && !isspace((unsigned char)*end))) return 0;
    *p = end;
    return 1;
}

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

static long rnd(long lo, long hi)
{
    unsigned long span = (unsigned long)(hi - lo + 1), range = RAND_MAX + 1UL, r;
    do r = (unsigned long)rand(); while (r >= range - range % span);
    return lo + (long)(r % span);
}

static void write_section(FILE *f, const char *name, int n, int fmt)
{
    static const char *head[] = { "", " tickets", " priority", " tickets priority" };
    long at[MAXP], bt[MAXP], end = 0;
    for (int i = 0; i < n; i++) {
        at[i] = rnd(0, 3L * n);
        bt[i] = rnd(1, 12);
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
    if (fmt & PRIORITY) fprintf(f, "# smaller priority number = higher priority\n");
    fprintf(f, "%d            # number of processes\n# arrival burst%s\n", n, head[fmt]);
    for (int i = 0; i < n; i++) {
        fprintf(f, "%-4ld %-4ld ", at[i], bt[i]);
        if (fmt & TICKETS)  fprintf(f, "%-4ld ", 5 * rnd(1, 10));
        if (fmt & PRIORITY) fprintf(f, "%-4ld ", rnd(1, 10));
        fprintf(f, "    # P%d\n", i + 1);
    }
}

static int generate_input(int n)
{
    FILE *f = create_new(INPUT_FILE);
    if (!f) { perror(INPUT_FILE); return -1; }
    file_created = 1;
    fprintf(f, "# input.txt - generated with random values (deleted on exit)\n"
               "# Every section: n, one row per process (P1..Pn), then its parameters.\n"
               "# Keep one row or parameter per line; values are 0 to %ld.\n"
               "# The Round Robin time quantum is NOT stored here: it is asked in the terminal.\n", MAXVALUE);
    write_section(f, "FCFS", n, PLAIN);
    write_section(f, "SJF", n, PLAIN);
    write_section(f, "SRTN", n, PLAIN);
    write_section(f, "RR", n, PLAIN);
    write_section(f, "LOTTERY", n, TICKETS);
    fprintf(f, "%-12ld # time quantum q (ms)\n", rnd(2, 4));
    fprintf(f, "%-12ld # random seed\n", rnd(1, 9999));
    write_section(f, "COMPARE", n, FULL);
    fprintf(f, "%-12ld # lottery time quantum q (ms)\n", rnd(2, 4));
    fprintf(f, "%-12ld # lottery random seed (single sample run)\n", rnd(1, 9999));
    fprintf(f, "%-12ld # aging factor alpha (ms)\n", rnd(2, 5));
    write_section(f, "PRIORITY", n, PRIORITY);
    write_section(f, "SJF_AGING", n, PLAIN);
    fprintf(f, "%-12ld # aging factor alpha (ms)\n", rnd(2, 5));
    return finish_file(f, INPUT_FILE);
}

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

static int blank(const char *p)
{
    while (isspace((unsigned char)*p)) p++;
    return *p == '\0';
}

static int load_section(const char *name, int fmt, Workload *w, long *par, int npar)
{
    FILE *f = fopen(INPUT_FILE, "r");
    if (!f) { perror(INPUT_FILE); return -1; }
    int cols = 2 + !!(fmt & TICKETS) + !!(fmt & PRIORITY), got = 0, in = 0, seen = 0, no = 0;
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
                                       .tickets = fmt & TICKETS ? v[2] : 0,
                                       .prio = fmt & PRIORITY ? v[cols - 1] : 0 };
            if (v[1] < 1) snprintf(err, sizeof err, "line %d: burst of P%d must be at least 1", no, got);
            else if ((fmt & TICKETS) && v[2] < 1)
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

static void rule(const int *wd, int nc)
{
    printf(" +");
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < wd[i] + 2; j++) putchar('-');
        putchar('+');
    }
    putchar('\n');
}

static void print_table(const char **hd, int nc, Cell v[][8], int n, int left)
{
    int wd[8];
    for (int i = 0; i < nc; i++) {
        wd[i] = (int)strlen(hd[i]);
        for (int k = 0; k < n; k++)
            if ((int)strlen(v[k][i]) > wd[i]) wd[i] = (int)strlen(v[k][i]);
    }
    rule(wd, nc);
    for (int k = -1; k < n; k++) {
        printf(" |");
        for (int i = 0; i < nc; i++) printf(" %*s |", i < left ? -wd[i] : wd[i], k < 0 ? hd[i] : v[k][i]);
        printf("\n");
        if (k < 0) rule(wd, nc);
    }
    rule(wd, nc);
}

static int input_cells(const Process *p, int fmt, Cell *v, const char **hd)
{
    int c = 3;
    snprintf(v[0], sizeof v[0], "P%d", p->pid);
    snprintf(v[1], sizeof v[1], "%ld", p->at);
    snprintf(v[2], sizeof v[2], "%ld", p->bt);
    if (fmt & TICKETS)  { hd[c] = "Tickets";  snprintf(v[c++], sizeof v[0], "%ld", p->tickets); }
    if (fmt & PRIORITY) { hd[c] = "Priority"; snprintf(v[c++], sizeof v[0], "%ld", p->prio); }
    return c;
}

static void print_input(const char *section, const Workload *w, int fmt)
{
    const char *hd[8] = { "Process", "Arrival (ms)", "Burst (ms)" };
    Cell v[MAXP][8];
    int nc = 0;
    for (int i = 0; i < w->n; i++) nc = input_cells(&w->p[i], fmt, v[i], hd);
    printf("Input read from %s, section %s (n = %d):\n", INPUT_FILE, section, w->n);
    print_table(hd, nc, v, w->n, 0);
}

static void print_result(const Workload *w, const int *order, int fmt)
{
    const char *hd[8] = { "Process", "Arrival", "Burst" };
    Cell v[MAXP][8];
    int c = 0;
    for (int k = 0; k < w->n; k++) {
        const Process *p = &w->p[order ? order[k] : k];
        long r[4] = { p->ct, p->tat, p->wt, p->rt };
        c = input_cells(p, fmt, v[k], hd);
        for (int i = 0; i < 4; i++) snprintf(v[k][c + i], sizeof v[k][c + i], "%ld", r[i]);
    }
    hd[c] = "Completion"; hd[c + 1] = "Turnaround"; hd[c + 2] = "Waiting"; hd[c + 3] = "Response";
    printf("Result table (all times in ms):\n");
    print_table(hd, c + 4, v, w->n, 0);
}

static Stats compute_stats(Workload *w)
{
    Stats s = { 0 };
    for (int i = 0; i < w->n; i++) {
        Process *p = &w->p[i];
        p->tat = p->ct - p->at;
        p->wt  = p->tat - p->bt;
        p->rt  = p->st - p->at;
        s.m[0] += (double)p->tat; s.m[1] += (double)p->wt; s.m[2] += (double)p->rt;
        s.busy += p->bt;
        if (p->ct > s.elapsed) s.elapsed = p->ct;
        if (p->wt > s.max_wt) s.max_wt = p->wt;
    }
    for (int i = 0; i < 3; i++) s.m[i] /= w->n;
    s.m[3] = 100.0 * (double)s.busy / (double)s.elapsed;
    s.m[4] = w->n / (double)s.elapsed;
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
    else {
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
}

static void gantt_free(Gantt *g)
{
    free(g->s);
    *g = (Gantt){ 0 };
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
    if (mode == BY_PRIORITY)   return p->prio;
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

static void non_preemptive(Workload *w, Gantt *g, int *exec, int mode, long alpha, int verbose)
{
    long t = 0;
    reset(w, g);
    if (verbose)
        printf("Execution log (effective burst = burst - floor(waited / %ld), smallest runs):\n", alpha);
    for (int done = 0; done < w->n; ) {
        int x = pick(w, t, mode, alpha), ready = 0;
        if (x < 0) { t = idle(w, g, t, verbose); continue; }
        Process *p = &w->p[x];
        if (verbose) {
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
    non_preemptive(w, g, ord, BY_ARRIVAL, 0, 0);
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

static long slices(const Workload *w, long q)
{
    long total = 0;
    for (int i = 0; i < w->n; i++) total += (w->p[i].bt + q - 1) / q;
    return total;
}

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

static int lottery_quantum_ok(const Workload *w, long q)
{
    if (q < 1) printf("Error: lottery quantum must be >= 1\n");
    else if (slices(w, q) > MAXSLICES)
        printf("Error: lottery quantum %ld needs %ld time slices (limit %ld); use a larger quantum\n",
               q, slices(w, q), MAXSLICES);
    else return 1;
    return 0;
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
        printf("Execution log (tickets numbered in PID order over runnable processes;\n"
               "[..] = the winner's tickets / pool = its real chance in that draw):\n");
    for (int done = 0; done < w->n; ) {
        long pool = 0, acc = 0;
        for (int i = 0; i < w->n; i++)
            if (w->p[i].rem > 0 && w->p[i].at <= t) pool += w->p[i].tickets;
        if (pool == 0) { t = idle(w, g, t, verbose); continue; }
        unsigned long long r, range = (unsigned long long)pool;
        do r = lrng(); while (r >= ULLONG_MAX - ULLONG_MAX % range);
        long draw = (long)(r % range) + 1;
        int win = -1;
        while (acc < draw) {
            win++;
            if (w->p[win].rem > 0 && w->p[win].at <= t) acc += w->p[win].tickets;
        }
        Process *p = &w->p[win];
        long end = run_piece(p, g, t, p->rem < q ? p->rem : q);
        if (verbose) {
            char span[48], left[48];
            snprintf(span, sizeof span, "%ld-%ld", t, end);
            if (p->rem > 0) snprintf(left, sizeof left, "(P%d left %ld)", p->pid, p->rem);
            else            snprintf(left, sizeof left, "(P%d done)", p->pid);
            printf("  t=%-5ld pool=%-5ld draw=%-5ld -> P%-2d runs %-10s %-15s [%ld/%ld = %.1f%%]\n",
                   t, pool, draw, p->pid, span, left, p->tickets, pool,
                   100.0 * (double)p->tickets / (double)pool);
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
    long par[2] = { 1, 0 }, total = 0;
    int exec[MAXP], count = 0, fmt = A[opt].fmt;

    banner(A[opt].title);
    if (load_section(A[opt].sec, fmt, &w, par, A[opt].npar) < 0) return;
    if (opt == 5 && !lottery_quantum_ok(&w, par[0])) return;
    if (opt == 9 && par[0] < 1) { printf("Error: aging factor alpha must be >= 1\n"); return; }
    plain = w;
    print_input(A[opt].sec, &w, fmt);
    switch (opt) {
    case 1: non_preemptive(&w, &g, exec, BY_ARRIVAL, 0, 0);  break;
    case 2: non_preemptive(&w, &g, exec, BY_BURST, 0, 0);    break;
    case 3: count = srtn(&w, &g);                            break;
    case 4:
        par[0] = ask_quantum("time quantum", &w);
        printf("\nTime quantum q = %ld ms", par[0]);
        count = round_robin(&w, &g, par[0]);
        break;
    case 5:
        for (int i = 0; i < w.n; i++) total += w.p[i].tickets;
        printf("Time quantum q = %ld ms, random seed = %ld, total tickets = %ld (read from %s)\n"
               "Ticket shares if every process were runnable at once:\n ",
               par[0], par[1], total, INPUT_FILE);
        for (int i = 0; i < w.n; i++) printf(" P%d=%.1f%%", i + 1, 100.0 * (double)w.p[i].tickets / (double)total);
        printf("\n\n");
        lottery(&w, &g, par[0], par[1], 1);
        break;
    case 8: non_preemptive(&w, &g, exec, BY_PRIORITY, 0, 0); break;
    case 9:
        printf("Aging factor alpha = %ld ms (read from %s)\n\n", par[0], INPUT_FILE);
        non_preemptive(&w, &g, exec, BY_AGED_BURST, par[0], 1);
        break;
    }
    Stats s = compute_stats(&w);
    printf("\n"); print_gantt(&g);
    printf("\n"); print_result(&w, opt >= 3 && opt <= 5 ? NULL : exec, fmt);
    printf("\n"); print_stats(&s);
    if (opt == 3) printf("Preemptions             = %d\n", count);
    if (opt == 4) printf("Process switches        = %d\n", count);
    if (opt == 9) {
        non_preemptive(&plain, &g, exec, BY_BURST, 0, 0);
        Stats ps = compute_stats(&plain);
        printf("\nEffect of aging on the same data:\n");
        printf("  Average waiting time  : SJF %.2f ms -> with aging %.2f ms\n", ps.m[1], s.m[1]);
        printf("  Longest waiting time  : SJF %ld ms -> with aging %ld ms\n", ps.max_wt, s.max_wt);
    }
    gantt_free(&g);
}

static int build_comparison(Comparison *c)
{
    int exec[MAXP];
    Workload *w = c->w, mean;
    Gantt *g = c->g, scratch = { 0 };

    memset(c, 0, sizeof *c);
    if (load_section("[COMPARE]", FULL, &w[0], c->par, 3) < 0) return -1;
    if (!lottery_quantum_ok(&w[0], c->par[0])) return -1;
    if (c->par[2] < 1) { printf("Error: aging factor alpha must be >= 1\n"); return -1; }
    print_input("[COMPARE]", &w[0], FULL);
    printf("Lottery: q = %ld ms, sample seed = %ld; SJF Aging: alpha = %ld ms (read from %s)\n",
           c->par[0], c->par[1], c->par[2], INPUT_FILE);
    c->q = ask_quantum("Round Robin time quantum", &w[0]);
    mean = w[0];
    for (int i = 1; i < ALGS; i++) w[i] = w[0];

    strcpy(c->name[FCFS], "FCFS");
    strcpy(c->name[SJF], "SJF");
    strcpy(c->name[SRTN], "SRTN");
    strcpy(c->name[PRIO], "Priority");
    snprintf(c->name[RR], sizeof(Cell), "RR (q=%ld)", c->q);
    snprintf(c->name[LOTTERY], sizeof(Cell), "Lottery (q=%ld, mean %d)", c->par[0], LOTTERY_RUNS);
    snprintf(c->name[AGING], sizeof(Cell), "SJF Aging (alpha=%ld)", c->par[2]);

    non_preemptive(&w[FCFS], &g[FCFS], exec, BY_ARRIVAL, 0, 0);
    non_preemptive(&w[SJF], &g[SJF], exec, BY_BURST, 0, 0);
    snprintf(c->note[SRTN], sizeof(Cell), "preemptions: %d", srtn(&w[SRTN], &g[SRTN]));
    snprintf(c->note[RR], sizeof(Cell), "process switches: %d", round_robin(&w[RR], &g[RR], c->q));
    lottery(&w[LOTTERY], &g[LOTTERY], c->par[0], c->par[1], 0);
    snprintf(c->note[LOTTERY], sizeof(Cell), "sample run, seed %ld", c->par[1]);
    non_preemptive(&w[PRIO], &g[PRIO], exec, BY_PRIORITY, 0, 0);
    non_preemptive(&w[AGING], &g[AGING], exec, BY_AGED_BURST, c->par[2], 0);
    for (int i = 0; i < ALGS; i++) c->st[i] = compute_stats(&w[i]);

    double sum[5] = { 0 };
    for (long seed = 1; seed <= LOTTERY_RUNS; seed++) {
        lottery(&mean, &scratch, c->par[0], seed, 0);
        Stats one = compute_stats(&mean);
        for (int m = 0; m < 5; m++) sum[m] += one.m[m];
    }
    for (int m = 0; m < 5; m++) c->st[LOTTERY].m[m] = sum[m] / LOTTERY_RUNS;
    gantt_free(&scratch);
    return 0;
}

static double best_of(const Comparison *c, int m)
{
    double best = c->st[0].m[m];
    for (int i = 1; i < ALGS; i++) if (c->st[i].m[m] < best) best = c->st[i].m[m];
    return best;
}

static int is_best(const Comparison *c, int i, int m)
{
    double best = best_of(c, m);
    return c->st[i].m[m] - best < 1e-9 * (1.0 + best);
}

static void print_comparison(const Comparison *c)
{
    static const char *hd[8] = { "Algorithm", "Avg TAT", "Avg WT", "Avg RT", "CPU %", "Throughput" };
    Cell v[ALGS][8];
    for (int i = 0; i < ALGS; i++) {
        printf(c->note[i][0] ? "\n[%s]  (%s)\n" : "\n[%s]\n", c->name[i], c->note[i]);
        print_gantt(&c->g[i]);
        snprintf(v[i][0], sizeof(Cell), "%s", c->name[i]);
        for (int m = 0; m < 5; m++) snprintf(v[i][m + 1], sizeof(Cell), "%.*f", m == 4 ? 4 : 2, c->st[i].m[m]);
    }
    printf("\nINTEGRATED COMPARISON (times in ms)\n");
    print_table(hd, 6, v, ALGS, 1);
    for (int m = 0; m < 3; m++) {
        const char *sep = "";
        printf("Best average %-16s: ", METRIC[m]);
        for (int i = 0; i < ALGS; i++)
            if (is_best(c, i, m)) { printf("%s%s", sep, c->name[i]); sep = ", "; }
        printf("  (%.2f ms)\n", best_of(c, m));
    }
}

static void compare(void)
{
    static Comparison c;
    banner("COMPARISON OF ALL SEVEN ALGORITHMS (shared [COMPARE] workload)");
    if (build_comparison(&c) < 0) return;
    print_comparison(&c);
    for (int i = 0; i < ALGS; i++) gantt_free(&c.g[i]);
}

static void show_file(void)
{
    char line[512];
    banner("CONTENTS OF input.txt");
    FILE *f = fopen(INPUT_FILE, "r");
    if (!f) { perror(INPUT_FILE); return; }
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    if (ferror(f)) perror(INPUT_FILE);
    fclose(f);
}

int main(void)
{
    char buf[64];
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_handlers();
    seed_random();

    printf("=============================================================\n"
           "   CPU Scheduling Simulator - CSE-3110 OS Lab, Assignment 02\n"
           "=============================================================\n");
    if (protect_existing_input() < 0) return 1;
    int n = (int)read_long("How many processes do you want? (1-20): ", 1, MAXP);
    hold_signals(1);
    int made = generate_input(n);
    hold_signals(0);
    if (made < 0) return 1;
    printf("%s created with random parameters for %d processes in sections\n"
           "[FCFS] [SJF] [SRTN] [RR] [LOTTERY] [COMPARE] [PRIORITY] [SJF_AGING].\n", INPUT_FILE, n);

    for (;;) {
        printf("\n==================== CPU SCHEDULING MENU ====================\n"
               "  1. First-Come, First-Served (FCFS)\n"
               "  2. Shortest Job First (SJF)\n"
               "  3. Shortest Remaining Time Next (SRTN)\n"
               "  4. Round Robin (RR)  - time quantum asked here\n"
               "  5. Lottery Scheduling\n"
               "  6. Compare all seven algorithms\n"
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
