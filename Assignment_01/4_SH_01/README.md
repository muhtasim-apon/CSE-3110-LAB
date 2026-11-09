# myshell: An Enhanced Custom UNIX Shell in C

| | |
|---|---|
| **Course** | CSE-3110: Operating Systems Lab |
| **Assignment** | Lab Assignment 1: Building an Enhanced Custom Shell |
| **Name** | Syed Muhtasim Alam Apon |
| **Roll** | SH-04 |
| **Registration No.** | 2023715947 |

---

## Table of Contents
1. [Overview](#1-overview)
2. [Submitted Files](#2-submitted-files)
3. [Compilation and Running](#3-compilation-and-running)
4. [Paths and Safety](#4-paths-and-safety)
5. [Commands and System Calls](#5-commands-and-system-calls)
6. [Built-in Commands](#6-built-in-commands)
7. [Parsing and the Execution Loop](#7-parsing-and-the-execution-loop)
8. [Advanced Features](#8-advanced-features)
9. [Error Handling and Exit Status](#9-error-handling-and-exit-status)
10. [Test Cases](#10-test-cases)
11. [Sample Session](#11-sample-session)
12. [Requirement Checklist](#12-requirement-checklist)
13. [Known Limitations](#13-known-limitations)

---

## 1. Overview

`myshell` is an interactive UNIX-like shell written in a single C file, `main.c`. It shows a prompt, reads a command line, splits it into words and operators, and executes it.

The assignment rules are followed throughout:
- `system()` and `popen()` are **never** used. No system binary is called for any of the 14 required commands. Each one is written directly with POSIX system calls.
- Commands run in child processes created with `fork()`. Other programs are started with `execvp()`, and the parent waits with `waitpid()`.
- Files are handled with `open()`, `read()`, `write()`, `close()`, `stat()`, `opendir()` and `readdir()`.

The shell behaves like a normal UNIX shell, with one deliberate rule: **the folder where it starts is its root, and nothing outside that folder can be reached** (§4). `cp src.txt dest.txt`, `cd ..` and `cd` with no argument work inside it. The assignment's example `ls -l /usr/bin` is refused, because `/usr/bin` lies outside.

---

## 2. Submitted Files

| File | Description |
|---|---|
| `main.c` | Full source code of the shell (single file) |
| `build_and_test.sh` | Compiles with `gcc -Wall -o myshell main.c` and runs **365 automated test cases** |
| `README.md` | This document |

**Submission:** the three files are packed in one archive, **`4_SH_01.zip`**, named as the guidelines require (`[Roll No.]_[Assignment Number].zip`, for example `123_SH_01.zip`).

---

## 3. Compilation and Running

**Manual:**
```bash
gcc -Wall -o myshell main.c
./myshell
```

**With the script:**
```bash
chmod +x build_and_test.sh
./build_and_test.sh          # compile, then run all 365 tests
./build_and_test.sh build    # compile only
./build_and_test.sh run      # compile, then start the shell
```

- The code compiles with **no warnings** under `-Wall`.
- Leave the shell with `exit [n]` or **Ctrl+D**.
- **Ctrl+C** stops only the running command. The shell itself keeps running.

---

## 4. Paths and Safety

**The start folder is the root of the shell.** Start `myshell` inside `4_SH_01` and that folder becomes its root: no command can go above it or reach anything outside it, including the home folder.

| Where you are | `pwd`, `cd -` and `$PWD` show |
|---|---|
| the start folder | `4_SH_01` |
| a folder inside it | `4_SH_01/demo/os` |

The full path is never shown, because the shell can never be outside the start folder.

| Input | Result |
|---|---|
| `cd ..` in the start folder | `cd: ..: access denied (outside 4_SH_01)`, the folder does not change |
| `cd ..` in a folder below it | works normally |
| `cd ~`, `cd /`, `cd $HOME`, `cd ../..` | `access denied` |
| `cd` with no argument | goes to the start folder (the real home folder is blocked) |
| `cd -` | goes to the previous folder, which is always inside |
| `ls ..`, `cat /etc/passwd`, `cp ../x y`, `echo x > ~/f`, `rm ../x` | `access denied` |

**Two layers enforce this:**

1. **Commands in `main.c`.** Every path goes through `expand_path()`, which calls `jail_ok()`. `realpath()` turns the path into its true location (it resolves `..`, `.` and symlinks), and `below_root()` compares that with the start folder. A link that leads outside is therefore blocked too, and `4_SH_01x` does not count as inside `4_SH_01`. A path that does not exist yet is judged by its nearest existing parent, a link to nowhere is refused, and `missing/..` is refused because `mkdir -p` could make it real.
2. **External programs.** `child_run()` runs the same check on every argument, so `wc ../x` and `/bin/ls` are refused. A program can still choose its own paths, as in `sh -c 'cd ..; cat x'`, so `jail_program()` also puts it in a **Landlock** sandbox just before `execvp()`. The kernel then refuses everything outside the start folder, for that program and all its children, and the limit cannot be removed. `sudo` and `su` stop working too (`PR_SET_NO_NEW_PRIVS`). The sandbox allows reading and running files under `/usr`, `/bin`, `/lib` and `/lib64` (programs need their libraries), `/etc/ld.so.cache`, and a few devices such as `/dev/null`.

A few further safety checks remain:

| Check | Example |
|---|---|
| `rm` never removes `.` or `..` | `rm -r ..` → `rm: refusing to remove '.' or '..' directory: skipping '..'` |
| `rm` does not remove the folder you are in | `rm -r /path/to/cwd` → `current directory is inside it` |
| `rm` and `rm -r` **never follow symlinks** | `unlink()` is tried first: it removes a file or a link itself and fails with `EISDIR` only on a real folder. So `rm -r` cannot walk through a link into another folder |
| `mv` moves a link, not its target | `rename()` never follows the link |

---

## 5. Commands and System Calls

### 5.1 File & Directory Inspection Commands
| Command | Usage | System calls | Cases handled |
|---|---|---|---|
| `pwd` | `pwd` | `getcwd()` | prints the current folder from the start folder's name (`4_SH_01`, `4_SH_01/demo`), never the full path |
| `ls` | `ls [-a] [-l] [path...]` | `opendir()`, `readdir()`, `closedir()`, `stat()` (`-l` owner name: `getpwuid()`) | sorted output; folders end with `/`; hidden files only with `-a`; `-l` shows mode, links, owner, size and time; files or several folders as arguments; missing path; invalid option |
| `cd` | `cd [dir \| -]` | `chdir()`, `getcwd()` | **built-in**; `chdir()` itself reports a missing folder or not-a-directory; no argument goes to the start folder; `cd ..` goes to the parent folder but never above the start folder; `cd ~`, `cd /` and any other path outside are refused (§4); `cd -` goes back to the previous folder and prints it; `cd ""` stays in place; not-a-directory; too many arguments |
| `whoami` | `whoami` | `getuid()`, `getpwuid()` | |
| `uname` | `uname [-a -s -n -r -v -m]` | `uname()` | labelled table without options; single-line output with options; invalid option |

### 5.2 File & Directory Manipulation Commands
| Command | Usage | System calls | Cases handled |
|---|---|---|---|
| `mkdir` | `mkdir [-p] dir...` | `mkdir()` with mode **0755** (`-p` uses `stat()` to accept an existing folder) | several folders; `-p` creates parents; already exists; missing parent; no argument |
| `touch` | `touch file...` | `open()` with `O_CREAT`, `close()`, `utimensat()` | new empty file; existing file keeps its content and gets a new time; directory; missing parent |
| `rm` | `rm [-r] [-f] path...` | `unlink()`, `rmdir()` (`-r` also uses `opendir()`/`readdir()`/`closedir()`) | `unlink()` is tried first and fails with `EISDIR` on a folder, which then goes to `rmdir()`; file; empty folder; non-empty folder gives an error (or `-r`); `-f` ignores missing files; a symlink is removed without its target being touched |
| `cp` | `cp src dst` or `cp src... dir` | `open()`, `read()`, `write()`, `close()` (`stat()` for checks) | unreadable source (`cannot open 'x' for reading`); overwrite; copy into a folder; same-file check; source is a folder; keeps file mode; 1 MB binary copied exactly |
| `mv` | `mv src dst` or `mv src... dir` | `rename()` (`stat()` for checks) | rename; overwrite; move one or several into a folder; several with a target that is not a folder; move a folder into itself is refused; missing source |

### 5.3 File Viewing & Process Inspection Commands
| Command | Usage | System calls / source | Cases handled |
|---|---|---|---|
| `cat` | `cat [file...]` | `open()`, `read()`, `write()`, `close()` | several files; missing file followed by a valid one; a directory; empty file; no argument (or `-`) reads standard input, which lets it work with `<` and pipes |
| `echo` | `echo [-n] text...` | `write()` (through stdout), `getenv()` | **built-in**; `$VAR`, `${VAR}`, `$?`, `$PWD`; quotes; `-n` |
| `uptime` | `uptime` | `open()`/`read()` of `/proc/uptime` and `/proc/loadavg` | days/hours/minutes/seconds plus 1/5/15-minute load average |
| `meminfo` | `meminfo` | `open()`/`read()` of `/proc/meminfo`, parsed with `sscanf()` | Total, Free, Available, Buffers, Cached, Swap, plus a calculated Used |

---

## 6. Built-in Commands

These run **inside the shell process, without `fork()`**. `cd` and `exit` have to change the shell itself: a child process could not change the parent's folder or end the parent.

| Built-in | Behaviour |
|---|---|
| `cd` | see 5.1 |
| `echo` | prints its arguments; supports `echo $PATH`, `echo Hello`, `echo "a   b"`, `echo '$HOME'` (literal) |
| `exit [n]` | ends the shell with status `n` (default: the status of the last command). `exit abc` gives an error and status 2; `exit 1 2` gives "too many arguments" and the shell keeps running |
| `help [command]` | `help` shows every command grouped by kind, the operators, quoting and keys. `help ls` (or `help cp mv`) shows each command's usage, description and **the system calls it uses**. An unknown name gives `help: no help topic for 'x'` and status 1 |
| `history [n]` | lists the last commands (all, or the last `n`) |

When a built-in is used with a redirection (`history > file`), the shell copies stdin/stdout to spare descriptors 100/101 with `dup2()`, redirects them with `dup2()`, runs the command, and then restores them with `dup2()`.

---

## 7. Parsing and the Execution Loop

- **Loop:** reads a line with `fgets()`, runs it, and repeats until `exit` or EOF.
- **Terminal UI:** on a terminal (`isatty()`), the shell starts with a banner and shows the prompt **`myshell> `**, exactly as in the assignment. Use `pwd` to see the current folder.
  - `myshell` is cyan, and the `>` turns red after a failed command.
  - **Ctrl+C** at the prompt gives a fresh prompt on a new line. While a command runs, Ctrl+C stops only that command (status 130). A shell started inside another myshell does not print an extra blank line.
  - Colours are used only when stdout is a terminal. With piped input there is no banner and no prompt, so test output stays clean.
- **Tokenizer:**
  - splits words on spaces and tabs;
  - recognises `|`, `<`, `>`, `>>` and `&` even without spaces (`echo hi>f.txt`);
  - `"double quotes"` keep spaces and expand variables;
  - `'single quotes'` are fully literal;
  - `\` escapes one character;
  - `#` starts a comment;
  - operators inside quotes are plain text (`echo "a|b > c"`).
- **Variables:** `$NAME`, `${NAME}` (from `getenv()`), `$PWD` (from `getcwd()`) and `$?` (last exit status). An unset variable expands to nothing.
- **Parser:** groups words into commands joined by pipes and attaches their redirections. Redirections are kept **in the order typed** and applied in that order, as in bash: `echo a > f1 > f2` creates both files and writes to `f2`, and `cat < missing > out` stops at the error, so `out` is not created. Syntax errors give a clear message, for example:
  ```
  myshell: syntax error near unexpected token `newline'     (ls >)
  myshell: syntax error near unexpected token `|'           (| ls,  ls | | cat)
  myshell: syntax error near unexpected token `&'           (&,  ls & ls)
  myshell: syntax error: missing command                    (ls |)
  myshell: syntax error: unterminated quote                 (echo "abc)
  ```
- **Limits:**
  - lines longer than 1022 characters are rejected with a message, and the shell keeps running;
  - at most 63 arguments per command;
  - at most 16 commands in one pipeline;
  - at most 16 redirections per command.
- **Signals:** the shell catches `SIGINT` with `sigaction()`. Foreground children get the default action back, and background children ignore it.

---

## 8. Advanced Features

All four optional features are implemented.

| # | Feature | How it works | System calls |
|---|---|---|---|
| 1 | **Piping** `cmd1 \| cmd2 \| ...` | one `pipe()` between each pair of commands; each command runs in its own child | `pipe()`, `fork()`, `dup2()`, `close()`, `waitpid()` |
| 2 | **I/O redirection** `<`, `>`, `>>` | each file is opened in the order typed and placed on stdin/stdout. `>` truncates, `>>` appends (and creates a missing file). Works with built-ins and together with pipes | `open()`, `dup2()`, `close()` |
| 3 | **Background** `cmd &` | the parent does not wait and prints `[job] pid`. Before each prompt, finished jobs are collected and reported as `[job] Done (status n)`. Background jobs read from `/dev/null` | `fork()`, `waitpid(-1, …, WNOHANG)` |
| 4 | **History** | in-memory list of the last 100 commands: `history`, `history n`, `!!` (repeat the last), `!n` (repeat number n) | |

---

## 9. Error Handling and Exit Status

Every error goes to **stderr**, in the form `command: object: reason`, using `perror()` or `strerror(errno)`. For example:
```
rm: cannot remove 'd': Directory not empty (use rm -r)
cp: cannot stat 'nope': No such file or directory
cat: d: Is a directory
cat: locked: Permission denied
myshell: ro/f: Permission denied
./s.sh: Permission denied
notacommand: command not found
```

| Exit status | Meaning |
|---|---|
| `0` | success |
| `1` | command failed (missing file, no permission, bad argument) |
| `2` | syntax error, or `exit` with a non-numeric argument |
| `126` | program found but could not be executed |
| `127` | command not found |
| `128+n` | child killed by signal `n` |

---

## 10. Test Cases

`build_and_test.sh` compiles the shell and then runs every test in a **fresh, empty folder**. A file `decoy.txt` is placed one level above it, and `HOME` is set to that parent folder, so `cd`, `~` and `..` give known paths. The test folder is named `work`, so inside it `pwd` prints `work/...`. The safety tests check on disk that `rm -r` never deletes anything through a symlink.

- The permission tests (group 19) are skipped when the script runs as root, because root ignores file permissions.
- The terminal tests (group 22) use the standard `script` command to give the shell a real terminal.

Five kinds of check are used:
- `check`: the output must match exactly (stdout and stderr together);
- `contains`: the output must contain a given text;
- `matches`: one output line must match a regular expression (used for changing numbers such as uptime);
- `status`: the shell's exit status;
- `fact`: a condition on disk afterwards (file exists, `cmp`, file mode, and so on).

**Result: 365 passed, 0 failed** (the same result on repeated runs).

| # | Group | Tests | What is covered |
|---|---|---|---|
| 1 | pwd, cd and paths | 34 | `pwd` shows the start folder by name (`work`, `work/d1`); `cd d1` / `cd ..`; `cd a/b/../..`; `cd` returns to the start folder; `cd -`; `$PWD`; symlinked folder; every way out is refused: `cd ..`, `cd ../..`, `cd /`, `cd ///`, `cd ~`, `cd $HOME`, `ls ..`, `ls -l /usr/bin`, `cat ../file`, absolute path, `~/file`, file symlink, `cp`/`mv`/`touch`/`>` with `../` (checked on disk), external program with `..` |
| 1b | Safety | 12 | `rm .`, `rm -r ..`, `rm -r /` and `rm -r //` and `mv /` (refused as outside), `rm` of the current folder, `rm` of a symlink removes only the link, `rm -r` never follows a symlink (checked on disk), `mv` of a symlink |
| 2 | pwd / cd errors | 5 | nested `pwd`, missing folder, `cd` into a file, too many arguments, `cd ""` stays |
| 3 | ls | 14 | empty folder, sorting, `/` marks, hidden files, `-a` shows `.` and `..`, file argument, nested path, several folders, `-l` mode/size/dir, `-la`, missing path, invalid option |
| 4 | whoami / uname | 7 | same as `id -un`; table; `-a`, `-r`, `-sm`; invalid option |
| 5 | mkdir | 8 | mode 0755, several, already exists, `-p` nested, `-p` existing, missing parent, no argument |
| 6 | touch | 8 | new empty file, several, keeps content and updates time, directory, missing parent, no argument |
| 7 | rm | 9 | file, empty folder, non-empty error, `-r`, missing, `-f`, several, symlink only, no argument |
| 8 | cp | 14 | copy, overwrite, into folder, several into folder, target not a folder, same file, missing source, source is a folder, missing destination folder, one argument, 1 MB binary (`cmp`), mode kept |
| 9 | mv | 6 | rename, into folder, folder rename, into itself, missing source, one argument |
| 10 | cat | 6 | one file, several, missing + valid, directory, empty file, stdin |
| 11 | echo & variables | 18 | spaces, double/single quotes, `$HOME`, `${HOME}x`, `$PATH`, unset variable, `$?` after success/failure/not-found, `-n`, no argument, escapes, lone `$`, unterminated quote, bad substitution |
| 12 | uptime / meminfo | 5 | every field is present |
| 13 | help / history | 16 | help text, `help cd`, `help ls`, `help cp mv`, unknown topic, `help ls \| cat`, no banner or prompt when piped, `history`, `history n`, `!!`, `!n`, event not found, bad argument, blank lines not stored |
| 14 | exit | 9 | stops the loop, status 0, `exit 5`, `exit 256`, last status, `exit abc`, too many arguments, EOF |
| 15 | Parsing | 8 | empty lines, tabs, comments, unknown command, external program with arguments, line too long (shell continues), too many arguments |
| 16 | Redirection, pipes, background | 22 | `>`, `>` without spaces, truncate, `>>`, `<`, `<` + `>`, missing input file, `ls \| cat`, 3-stage pipe, pipe into a file, built-in in a pipe, `cd` in a pipe, 6 syntax errors, background does not block, Done message, background + redirect |
| 17 | Extra corner cases | 19 | relative `..` symlink (`cd`, `ls`, `cat` through it), `echo piped\|cat`, `history \| cat`, `cat < f \| cat > g`, `history > file`, no colour codes in files, `ls > x \| cat`, `ls \| cat < a`, missing `<` file in a pipe, background pipeline, `&` alone, `expr 40 + 2`, two pipes |
| 18 | Assignment rules | 31 | no `system(`/`popen(`; none of `lstat`, `readlink`, `getpid`, `setenv` or `dup`; no `-Wall` warnings; each of the 23 required system calls is used, plus `realpath()` for the jail |
| 19 | Error handling | 17 | unreadable file for `cat`, `cp` and `<`; `mkdir`, `touch`, `rm` and `>` in a read-only folder; `cd` without `x`, `ls` without `r`; `>` to a folder; `>` and `mv` into a missing folder; non-executable file (126); executable script; killed by a signal (137); `$?` of external programs; `$?` = 2 after a syntax error |
| 20 | More edge cases | 33 | `exit -1` (255); `exit` in a pipe; built-in redirect restores stdout; `cd` with a redirect; `history 0` / `-1`; `!1 b`; history past 100 entries (numbering, forgotten entries); operators in quotes; trailing comment; empty `""` argument; `echo -n` alone; `uname -s -m`; uname table fields; uptime and load-average number format; MemTotal equals `/proc/meminfo`; `mv` several / overwrite; `cat a - b`; `touch` mode 644; `>>` creates; `> f1 > f2`; `< missing > f3`; 16-command pipeline works and 17 is refused; two background jobs, non-zero Done status |
| 21 | No system binaries | 3 | with an **empty `PATH`**, all 14 commands still work (`mkdir`, `touch`, `cp`, `mv`, `cat`, `ls`, `rm`, `pwd`, `whoami`, `uname`, `uptime`, `meminfo`, `echo`), while a real program (`wc`) is not found. This proves no system binary is used |
| 22 | Interactive | 6 | on a real terminal: banner, prompt is exactly `myshell> ` (no folder, no user@host), loop continues after an error; Ctrl+C at the prompt does not kill the shell; Ctrl+C stops a running `sleep` with status 130 |
| 23 | Jail | 55 | escapes through `..`, `~`, `$HOME`, absolute paths, symlinks, link chains, links to nowhere and `missing/..` for every command, each checked on disk; external programs, scripts and `sh -c` stopped by the kernel; symlinks inside the start folder and `gcc` still work |
| | **Total** | **365** | |

---

## 11. Sample Session

This is real output from `./myshell` started in the project folder `4_SH_01`. The prompt is `myshell> `. `pwd` shows the folder by its name, and leaving it is refused. On a terminal `myshell` is cyan and the `>` turns red after a failed command.
```
+------------------------------------------------------------+
|  myshell - custom UNIX shell (CSE-3110 OS Lab, Assign. 1)  |
+------------------------------------------------------------+
Type help for commands, help <cmd> for details, exit to quit.

myshell> pwd
4_SH_01
myshell> mkdir -p demo/os
myshell> cd demo/os
myshell> pwd
4_SH_01/demo/os
myshell> echo "Hello from $USER" > note.txt
myshell> cat note.txt
Hello from muhtasim
myshell> cp note.txt copy.txt
myshell> ls -l
-rw-r--r--  1 muhtasim       20 Oct 17 20:21 copy.txt
-rw-r--r--  1 muhtasim       20 Oct 17 20:21 note.txt
myshell> cat note.txt | tr a-z A-Z
HELLO FROM MUHTASIM
myshell> cd ../..
myshell> rm -r demo
myshell> cd ..
cd: ..: access denied (outside 4_SH_01)
myshell> cd ~
cd: ~: access denied (outside 4_SH_01)
myshell> pwd
4_SH_01
myshell> ls -l /usr/bin
ls: /usr/bin: access denied (outside 4_SH_01)
myshell> help pwd
pwd
  usage : pwd
  Print the current directory, written from the start folder's name
  (e.g. 4_SH_01/demo). The start folder is the root of this shell.
  system calls: getcwd()
myshell> exit
```

---

## 12. Requirement Checklist

Each item below is checked against the assignment document. The last column names the test group that proves it.

### I. Objectives
| Requirement | Status | Proof |
|---|---|---|
| a. Accept interactive commands with a prompt | ✅ | `myshell> ` prompt, exactly as in the assignment, with a banner; group 22, §11 |
| b. Parse and tokenize input into commands and arguments | ✅ | groups 11, 15 |
| c. Fork child processes and run programs with the exec family | ✅ | `fork()` + `execvp()`, groups 15, 16 |
| d. Built-in commands and direct POSIX system calls | ✅ | groups 1–14 |

### II. Shell Commands & System Calls
| # | Command | Required system call(s) | Status | Proof |
|---|---|---|---|---|
| 1 | `pwd` | `getcwd()` | ✅ | groups 1, 2 |
| 2 | `ls` | `opendir()`, `readdir()`, `closedir()`, `stat()` | ✅ | group 3 |
| 3 | `cd` | `chdir()` | ✅ | groups 1, 2 |
| 4 | `whoami` | `getuid()`, `getpwuid()` | ✅ | group 4 |
| 5 | `uname` | `uname()` | ✅ | group 4 |
| 6 | `mkdir` | `mkdir()` with mode | ✅ | group 5 |
| 7 | `touch` | `open(O_CREAT)`, `utimensat()` | ✅ | group 6 |
| 8 | `rm` | `unlink()` / `rmdir()` | ✅ | group 7 |
| 9 | `cp` | `read()`, `write()` | ✅ | group 8 |
| 10 | `mv` | `rename()` | ✅ | group 9 |
| 11 | `cat` | `open()`, `read()`, `write()` | ✅ | group 10 |
| 12 | `echo` | built-in | ✅ | group 11 |
| 13 | `uptime` | `/proc/uptime`, `/proc/loadavg` | ✅ | group 12 |
| 14 | `meminfo` | `/proc/meminfo` | ✅ | group 12 |

### Important Implementation Rules
| Rule | Status | Proof |
|---|---|---|
| No `system()` shell execution | ✅ | group 18 (grep for `system(`/`popen(`) |
| Process control with `fork()`, `execvp()`, `waitpid()` | ✅ | group 18 |
| File APIs `open/read/write/close/stat/opendir/readdir` | ✅ | group 18 |
| No standard binaries used for the manual utilities | ✅ | all 14 are written in `main.c`; group 21 runs them all with an **empty `PATH`** |

### III. Built-in Command Handling (in the parent process)
| Requirement | Status | Proof |
|---|---|---|
| `cd` with `chdir()`, including `cd ..` and `cd` with no arguments | ✅ | group 1 (`cd ..` goes to the parent up to the start folder, `cd` goes to the start folder) |
| `echo` text and environment variables (`echo $PATH`, `echo Hello`) | ✅ | group 11 |
| `exit` with a status code | ✅ | group 14 (`exit 5` gives status 5) |
| `help` menu, plus `help <command>` pages | ✅ | group 13 |

### IV. Command Parsing & Execution Loop
| Requirement | Status | Proof |
|---|---|---|
| a. Multiple arguments (`ls -l demo`, `cp src.txt dest.txt`) | ✅ | groups 3, 8. `ls -l /usr/bin` parses correctly but is refused because it is outside the start folder (§4) |
| b. Clear errors for invalid commands, missing arguments and file errors (`perror`/`strerror`) | ✅ | group 19 (permission, exec and signal errors), plus missing-argument and missing-file tests in groups 2–15 |
| c. Continuous loop with prompt until `exit` | ✅ | groups 14, 22, §11 |

### V. Advanced Features (extra credit)
| Feature | Status | Proof |
|---|---|---|
| 1. Piping with `pipe()` + `dup2()` | ✅ | groups 16, 17 |
| 2. Redirection `>`, `<`, `>>` | ✅ | groups 16, 17, 20 |
| 3. Background execution with `&` | ✅ | groups 16, 17, 20 |
| 4. Command history | ✅ | groups 13, 20 |

### Deliverables
| Deliverable | Status |
|---|---|
| Single C source file with the full shell (`main.c`) | ✅ |
| Shell script that compiles with `gcc -Wall -o myshell main.c` and runs automated tests (`build_and_test.sh`) | ✅ |
| README with compilation steps, system calls per command, features and test cases | ✅ (§3, §5, §6–8, §10) |
| One `.zip` named `[Roll No.]_[Assignment Number].zip` with the three files | ✅ `4_SH_01.zip` |

---

## 13. Known Limitations

- History is kept **in memory** only and is cleared when the shell exits, as the assignment describes.
- `cp` copies regular files only (no `cp -r`). `rm -r` is supported.
- `ls -l` shows a symlink with its target's details, because only `stat()` is used (as the assignment names). It does not use `lstat()`/`readlink()`, so `ls -l` does not print `link -> target`.
- There is no arrow-key line editing, because input is read with `fgets()`.
- The root is the folder `myshell` is **started** in, so it must be started from `4_SH_01`.
- The Landlock sandbox needs Linux 5.13 or newer with Landlock enabled. Without it the shell prints a note at start, and external programs are only checked by their arguments, so `sh -c '...'` could still leave the folder. Commands in `main.c` are not affected.
- Python does not start inside the sandbox, because it must list folders under `/usr/lib` and the sandbox allows no listing outside the start folder.
- External programs can read system files under `/usr`, for example through `sh -c`, because they need that folder to run at all. They cannot list it.
- `echo $HOME` still prints the path of the home folder: it is only text, and `echo $PATH` is required by the assignment. The folder itself cannot be reached.
- An external argument that looks like a path outside is refused even if it is not meant as a path, for example `expr 8 / 2` or `grep /home notes.txt`.
- Besides the calls each command uses, the shell itself needs a few support calls:
  - `sigaction()`/`signal()`, so Ctrl+C stops only the running command;
  - `isatty()`, to show the prompt only on a terminal;
  - `getenv()`, for `echo $PATH` (which the assignment requires) and for `~`;
  - `realpath()`, `prctl()` and the Landlock system calls (through `syscall()`), for the root-folder rule in §4;
  - `putenv()`, so temporary files of programs such as `gcc` stay in the start folder.
