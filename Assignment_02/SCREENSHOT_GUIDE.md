# Screenshot Guide — Assignment 02 (CPU Scheduling)

The program now works with **one randomly generated `input.txt`** and an
interactive menu. There are no command-line options and no `inputs/` files any more.

Build first, from the `Assignment_02` folder:

```bash
gcc -Wall -Wextra -O2 scheduling.c -o scheduling
./scheduling
```

Paste each screenshot into the dashed box with the **same number** in
`OS_Lab_Scheduling_Report.docx` (click the box, delete it, then Insert → Pictures,
about 16 cm wide). After pasting all images, right-click the Table of Contents →
Update Field.

> **Important:** the values are random, so your numbers will differ from any
> example. Take screenshots 2–10 in **one run** of the program (do not exit in
> between), so all of them use the same `input.txt`. Screenshot 3 then shows the
> exact values that screenshots 4–10 used.
>
> Every section of `input.txt` contains at least one idle gap (one random
> process arrives 1–4 ms after the CPU has finished all the others), so every
> Gantt chart shows an `IDLE` block and CPU utilisation is always below 100 %.
> Only the Gantt chart is printed; there is no separate execution-order line.

## One run, in this order

| # | Report section | What to type | What must be visible |
|---|---|---|---|
| 1 | 4.1 | `gcc -Wall -Wextra -O2 scheduling.c -o scheduling` | The gcc command with no warnings/errors |
| 2 | 4.2 | `./scheduling` → e.g. `5` | “How many processes do you want?”, your answer, “input.txt created …” and the menu |
| 3 | 4.3 | `7` | The full `input.txt`: sections `[FCFS] [SJF] [SRTN] [RR] [LOTTERY] [COMPARE]` with different random values; no RR quantum in the file |
| 4 | 6.1 | Enter → `1` | FCFS: input table read from `[FCFS]`, Gantt chart with IDLE block(s), result table, averages, `CPU Utilisation = … % (busy … ms + idle … ms = … ms)` below 100 %, throughput |
| 5 | 6.2 | Enter → `2` | SJF: input from `[SJF]`, Gantt chart with IDLE block(s), result table, averages |
| 6 | 6.3 | Enter → `3` | SRTN: Gantt chart, result table, averages and `Preemptions = …` |
| 7 | 6.4 | Enter → `4` → e.g. `3` | RR: input from `[RR]`, the prompt “Enter the time quantum q” with your value, Gantt chart, table, averages, process switches |
| 8 | 6.5 | Enter → `5` | Lottery: arrival/burst/tickets, q and seed read from the file, ticket shares, the execution log `t=… pool=… draw=… -> P… runs …` (with “CPU idle until …” lines), table with tickets |
| 9 | 6.6 | Enter → `6` → e.g. `2` | Comparison: input from `[COMPARE]`, the five Gantt charts, the INTEGRATED COMPARISON table and the three “Best average …” lines |
| 10 | 7 (Q2) | Enter → `6` → `100` | Comparison with RR q = 100 (larger than every burst): the RR row and Gantt chart are identical to FCFS |
| 11 | 4.4 | Enter → `0`, then `ls input.txt` | “input.txt has been flushed and deleted.” and `ls: cannot access 'input.txt'` |
| 12 | 4.4 | `./scheduling` → a different number, e.g. `4` → `7` | A new run: new process count and a new `input.txt` with different random values |

Long outputs (options 6 and 7) can be scrolled with the terminal scroll bar, or
taken as two screenshots (top and bottom).

## Optional extras

| Screenshot | How |
|---|---|
| Ctrl+C cleanup | Start `./scheduling`, enter a number, press **Ctrl+C**, then `ls input.txt` → “Interrupted - input.txt flushed and deleted.” and the file is gone |
| Wrong input | At any prompt type `abc` or `99` → “Please enter a whole number between …” and the question is asked again |
| Worked examples (section 5) | While the menu is open, replace a section of `input.txt` in another terminal with a lab-sheet example (see Appendix A of the report), then choose that option — the program re-reads the file every time |
