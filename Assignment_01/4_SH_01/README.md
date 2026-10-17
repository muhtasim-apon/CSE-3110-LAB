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
4. [Root Folder Protection (Sandbox)](#4-root-folder-protection-sandbox)
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

**Extra design feature:** the folder where `myshell` is started becomes the shell's **root `/`**. Inside the shell, `/` means that folder, and `pwd` and the prompt show it by its name (started in `4_SH_01`, `pwd` prints `4_SH_01`). No command can reach anything above that folder: not `cd ..`, `ls ..`, `cat ../file`, `> ../file`, `/etc`, or a symlink that points outside.

---

## 2. Submitted Files

| File | Description |
|---|---|
| `main.c` | Full source code of the shell (single file) |
| `build_and_test.sh` | Compiles with `gcc -Wall -o myshell main.c` and runs **250 automated test cases** |
| `README.md` | This document |

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
./build_and_test.sh          # compile, then run all 250 tests
./build_and_test.sh build    # compile only
./build_and_test.sh run      # compile, then start the shell
```

- The code compiles with **no warnings** under `-Wall`.
- Leave the shell with `exit [n]` or **Ctrl+D**.
- **Ctrl+C** stops only the running command. The shell itself keeps running.

---

## 4. Root Folder Protection (Sandbox)

At start-up the shell saves its current folder (`getcwd()`) as the root. Every path a user types is checked before use:

1. **`resolve()`: building the path.** `/x` and `~/x` start at the root, and any other path starts at the current folder. The shell handles `.` and `..` itself. A `..` that would go above the root is rejected immediately.
2. **`jail_check()`: checking symlinks.** `realpath()` is run on the path, or on its deepest existing parent. If the real location is outside the root, the path is rejected. This blocks symlinks such as `ln -s .. link` or `ln -s /etc link`. Dangling symlinks are blocked too.
3. **`virt()` and `show()`: showing the path.** `virt()` gives a real path relative to the root (the root is `/`, `root/a/b` is `/a/b`); `cd -` uses it. `show()` prints the root by its folder name: the root is `4_SH_01` and `root/a/b` is `4_SH_01/a/b`. `show()` is used by `pwd`, by the prompt and in `$PWD`.

| Where the check is used | Example blocked input |
|---|---|
| `cd`, `ls`, `cat`, `mkdir`, `touch`, `rm`, `cp`, `mv` | `cd ..` at `/`, `cd ../../..`, `ls ..`, `cat ../x`, `cp ../x .`, `mv a ../a` |
| Redirection `<`, `>`, `>>` | `echo hi > ../out.txt`, `cat < ../secret` |
| Path arguments of external programs | `/bin/ls ..`, `wc ../x`, `../script.sh`. `/x` is mapped to the real `root/x` |
| Extra safety in `rm` / `mv` | `rm /`, `rm .`, `rm ..`, `rm` of the folder you are in, `mv /` |

The blocked message is:
```
cd: ..: Permission denied (outside root directory)
```

---

## 5. Commands and System Calls

### 5.1 File & Directory Inspection Commands
| Command | Usage | System calls | Cases handled |
|---|---|---|---|
| `pwd` | `pwd` | `getcwd()` | prints the path with the root folder's name (`4_SH_01`, `4_SH_01/a/b`) |
| `ls` | `ls [-a] [-l] [path...]` | `opendir()`, `readdir()`, `closedir()`, `stat()`, `lstat()`, `readlink()`, `getpwuid()` | sorted output; folders end with `/`; hidden files only with `-a`; `-l` shows mode, links, owner, size and time; files or several folders as arguments; missing path; invalid option |
| `cd` | `cd [dir \| - \| ~ \| /]` | `chdir()`, `stat()`, `getcwd()` | **built-in**; no argument, `~` or `/` go to the root; `cd -` goes to the previous folder; `cd ..` is blocked at the root; not-a-directory; too many arguments |
| `whoami` | `whoami` | `getuid()`, `getpwuid()` | |
| `uname` | `uname [-a -s -n -r -v -m]` | `uname()` | labelled table without options; single-line output with options; invalid option |

### 5.2 File & Directory Manipulation Commands
| Command | Usage | System calls | Cases handled |
|---|---|---|---|
| `mkdir` | `mkdir [-p] dir...` | `mkdir()` with mode **0755** | several folders; `-p` creates parents; already exists; missing parent; no argument |
| `touch` | `touch file...` | `open()` with `O_CREAT`, `close()`, `utimensat()` | new empty file; existing file keeps its content and gets a new time; directory; missing parent |
| `rm` | `rm [-r] [-f] path...` | `lstat()`, `unlink()`, `rmdir()` (`-r` also uses `opendir()`/`readdir()`) | file; empty folder; non-empty folder gives an error (or `-r`); `-f` ignores missing files; a symlink is removed without its target being touched |
| `cp` | `cp src dst` or `cp src... dir` | `open()`, `read()`, `write()`, `close()`, `stat()` | overwrite; copy into a folder; same-file check; source is a folder; keeps file mode; 1 MB binary copied exactly |
| `mv` | `mv src dst` or `mv src... dir` | `rename()`, `lstat()` | rename; move into a folder; move a folder into itself is refused; missing source |

### 5.3 File Viewing & Process Inspection Commands
| Command | Usage | System calls / source | Cases handled |
|---|---|---|---|
| `cat` | `cat [file...]` | `open()`, `read()`, `write()`, `close()` | several files; missing file followed by a valid one; a directory; empty file; no argument (or `-`) reads standard input, which lets it work with `<` and pipes |
| `echo` | `echo [-n] text...` | `write()` (through stdout) | **built-in**; `$VAR`, `${VAR}`, `$?`; quotes; `-n` |
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
| `help` | shows every command, its options, the operators and the root rule |
| `history [n]` | lists the last commands (all, or the last `n`) |

When a built-in is used with a redirection (`history > file`), the shell saves stdin/stdout with `dup()`, redirects them with `dup2()`, runs the command, and then restores them.

---

## 7. Parsing and the Execution Loop

- **Loop:** prints the prompt `myshell:4_SH_01/current/folder> `, reads a line with `fgets()`, runs it, and repeats until `exit` or EOF. The prompt appears only on a terminal (`isatty()`), so piped test input stays clean.
- **Tokenizer:**
  - splits words on spaces and tabs;
  - recognises `|`, `<`, `>`, `>>` and `&` even without spaces (`echo hi>f.txt`);
  - `"double quotes"` keep spaces and expand variables;
  - `'single quotes'` are fully literal;
  - `\` escapes one character;
  - `#` starts a comment.
- **Variables:** `$NAME`, `${NAME}`, `$?` (last exit status) and `$$` (shell PID). An unset variable expands to nothing.
- **Parser:** groups words into commands joined by pipes and attaches their redirections. Syntax errors give a clear message, for example:
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
  - at most 16 commands in one pipeline.
- **Signals:** the shell catches `SIGINT` with `sigaction()`. Foreground children get the default action back, and background children ignore it.

---

## 8. Advanced Features

All four optional features are implemented.

| # | Feature | How it works | System calls |
|---|---|---|---|
| 1 | **Piping** `cmd1 \| cmd2 \| ...` | one `pipe()` between each pair of commands; each command runs in its own child | `pipe()`, `fork()`, `dup2()`, `close()`, `waitpid()` |
| 2 | **I/O redirection** `<`, `>`, `>>` | the file is opened inside the root and placed on stdin/stdout. `>` truncates, `>>` appends. Works together with pipes | `open()`, `dup2()`, `close()` |
| 3 | **Background** `cmd &` | the parent does not wait and prints `[job] pid`. Before each prompt, finished jobs are collected and reported as `[job] Done (status n)`. Background jobs read from `/dev/null` | `fork()`, `waitpid(-1, …, WNOHANG)` |
| 4 | **History** | in-memory list of the last 100 commands: `history`, `history n`, `!!` (repeat the last), `!n` (repeat number n) | |

---

## 9. Error Handling and Exit Status

Every error goes to **stderr**, in the form `command: object: reason`, using `perror()` or `strerror(errno)`. For example:
```
rm: cannot remove 'd': Directory not empty (use rm -r)
cp: cannot stat 'nope': No such file or directory
cat: d: Is a directory
notacommand: command not found
```

| Exit status | Meaning |
|---|---|
| `0` | success |
| `1` | command failed (missing file, blocked path, bad argument) |
| `2` | syntax error, or `exit` with a non-numeric argument |
| `126` | program found but could not be executed |
| `127` | command not found |
| `128+n` | child killed by signal `n` |

---

## 10. Test Cases

`build_and_test.sh` compiles the shell and then runs every test in a **fresh, empty folder**, so that folder is the shell's root `/`. A decoy file is placed one level above it. This proves that nothing outside the root can be read, changed or deleted.

Four kinds of check are used:
- `check`: the output must match exactly (stdout and stderr together);
- `contains`: the output must contain a given text;
- `status`: the shell's exit status;
- `fact`: a condition on disk afterwards (file exists, `cmp`, file mode, and so on).

**Result: 250 passed, 0 failed.**

| # | Group | Tests | What is covered |
|---|---|---|---|
| 1 | Root folder (sandbox) | 58 | `pwd` = root folder name; `cd ..`, `../../..`, `d1/../..` and `./../` blocked; `cd /`, `cd`, `cd ~`, `cd -`, `cd /a/b`; `/home` and the real parent path stay inside; virtual `$PWD`; `ls`/`cat`/`cp`/`mv`/`touch`/`mkdir`/`rm` with `..` blocked, with disk checks; `rm .`, `rm /`, `rm` of the current folder, `mv /`; symlink escapes; dangling symlink; a symlink inside the root works; `<`/`>` outside; external `/bin/ls ..`, `wc ../x`, `../evil.sh` |
| 2 | pwd / cd errors | 4 | nested `pwd`, missing folder, `cd` into a file, too many arguments |
| 3 | ls | 14 | empty folder, sorting, `/` marks, hidden files, `-a`, file argument, nested path, several folders, `-l` mode/size/dir, `-la`, missing path, invalid option |
| 4 | whoami / uname | 7 | same as `id -un`; table; `-a`, `-r`, `-sm`; invalid option |
| 5 | mkdir | 8 | mode 0755, several, already exists, `-p` nested, `-p` existing, missing parent, no argument |
| 6 | touch | 8 | new empty file, several, keeps content and updates time, directory, missing parent, no argument |
| 7 | rm | 9 | file, empty folder, non-empty error, `-r`, missing, `-f`, several, symlink only, no argument |
| 8 | cp | 14 | copy, overwrite, into folder, several into folder, target not a folder, same file, missing source, source is a folder, missing destination folder, one argument, 1 MB binary (`cmp`), mode kept |
| 9 | mv | 6 | rename, into folder, folder rename, into itself, missing source, one argument |
| 10 | cat | 6 | one file, several, missing + valid, directory, empty file, stdin |
| 11 | echo & variables | 18 | spaces, double/single quotes, `$HOME`, `${HOME}x`, `$PATH`, unset variable, `$?` after success/failure/not-found, `-n`, no argument, escapes, lone `$`, unterminated quote, bad substitution |
| 12 | uptime / meminfo | 5 | every field is present |
| 13 | help / history | 10 | help text, `history`, `history n`, `!!`, `!n`, event not found, bad argument, blank lines not stored |
| 14 | exit | 9 | stops the loop, status 0, `exit 5`, `exit 256`, last status, `exit abc`, too many arguments, EOF |
| 15 | Parsing | 8 | empty lines, tabs, comments, unknown command, external program with arguments, line too long (shell continues), too many arguments |
| 16 | Redirection, pipes, background | 22 | `>`, `>` without spaces, truncate, `>>`, `<`, `<` + `>`, missing input file, `ls \| cat`, 3-stage pipe, pipe into a file, built-in in a pipe, `cd` in a pipe, 6 syntax errors, background does not block, Done message, background + redirect |
| 17 | Extra corner cases | 19 | relative `..` symlink escape, `echo piped\|cat`, `history \| cat`, `cat < f \| cat > g`, `history > file`, no colour codes in files, `ls > x \| cat`, `ls \| cat < a`, missing `<` file in a pipe, background pipeline, `&` alone, `expr 40 + 2`, two pipes |
| 18 | Assignment rules | 25 | no `system(`/`popen(`; no `-Wall` warnings; each of the 23 required system calls is used |
| | **Total** | **250** | |

---

## 11. Sample Session

This is real output, with the shell started in an empty folder named `4_SH_01`:
```
myshell:4_SH_01> pwd
4_SH_01
myshell:4_SH_01> cd ..
cd: ..: Permission denied (outside root directory)
myshell:4_SH_01> mkdir -p projects/os
myshell:4_SH_01> cd projects/os
myshell:4_SH_01/projects/os> pwd
4_SH_01/projects/os
myshell:4_SH_01/projects/os> cd ../../..
cd: ../../..: Permission denied (outside root directory)
myshell:4_SH_01/projects/os> echo "Hello from $USER" > note.txt
myshell:4_SH_01/projects/os> cat note.txt
Hello from muhtasim
myshell:4_SH_01/projects/os> cp note.txt copy.txt
myshell:4_SH_01/projects/os> ls -l
-rw-r--r--  1 muhtasim       20 Oct 11 21:45 copy.txt
-rw-r--r--  1 muhtasim       20 Oct 11 21:45 note.txt
myshell:4_SH_01/projects/os> cat note.txt | tr a-z A-Z
HELLO FROM MUHTASIM
myshell:4_SH_01/projects/os> cd /
myshell:4_SH_01> cat ../secret.txt
cat: ../secret.txt: Permission denied (outside root directory)
myshell:4_SH_01> ls
projects/
myshell:4_SH_01> history 3
   13  cat ../secret.txt
   14  ls
   15  history 3
myshell:4_SH_01> exit
```

---

## 12. Requirement Checklist

Each item below is checked against the assignment document. The last column names the test group that proves it.

### I. Objectives
| Requirement | Status | Proof |
|---|---|---|
| a. Accept interactive commands with a prompt | ✅ | `myshell:4_SH_01/dir> ` prompt, §11 |
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
| No standard binaries used for the manual utilities | ✅ | all 14 are written in `main.c` |

### III. Built-in Command Handling (in the parent process)
| Requirement | Status | Proof |
|---|---|---|
| `cd` with `chdir()`, including `cd ..` and `cd` with no arguments | ✅ | group 1 |
| `echo` text and environment variables (`echo $PATH`, `echo Hello`) | ✅ | group 11 |
| `exit` with a status code | ✅ | group 14 (`exit 5` gives status 5) |
| `help` menu | ✅ | group 13 |

### IV. Command Parsing & Execution Loop
| Requirement | Status | Proof |
|---|---|---|
| a. Multiple arguments (`ls -l dir`, `cp src dst`) | ✅ | groups 3, 8 |
| b. Clear errors for invalid commands, missing arguments and file errors (`perror`/`strerror`) | ✅ | almost every group |
| c. Continuous loop with prompt until `exit` | ✅ | group 14, §11 |

### V. Advanced Features (extra credit)
| Feature | Status | Proof |
|---|---|---|
| 1. Piping with `pipe()` + `dup2()` | ✅ | groups 16, 17 |
| 2. Redirection `>`, `<`, `>>` | ✅ | groups 16, 17 |
| 3. Background execution with `&` | ✅ | groups 16, 17 |
| 4. Command history | ✅ | group 13 |

### Deliverables
| Deliverable | Status |
|---|---|
| Single C source file with the full shell (`main.c`) | ✅ |
| Shell script that compiles with `gcc -Wall -o myshell main.c` and runs automated tests (`build_and_test.sh`) | ✅ |
| README with compilation steps, system calls per command, features and test cases | ✅ |

---

## 13. Known Limitations

- Programs started with `execvp()` (for example `python3`) have their **path arguments** checked against the root, but a program can still open other files on its own. Fully confining external programs would need `chroot()` and root privileges.
- History is kept **in memory** only and is cleared when the shell exits, as the assignment describes.
- `cp` copies regular files only (no `cp -r`). `rm -r` is supported.
