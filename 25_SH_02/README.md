# Custom Shell — Roll 25, SH_02

## Overview

This project is a small UNIX-like shell written in C for the Operating Systems Lab assignment.

The folder where `myshell` is started becomes the shell's virtual root `/`. The shell's own file and directory commands cannot move above that folder. For example, if the program is started inside an assignment folder, `pwd` shows `/`, and `cd ..` at `/` stays at `/`.

I kept the source organized so each part can be explained separately in a viva:

1. Virtual-root path handling
2. Command history
3. Individual command functions
4. One command dispatcher
5. Tokenizer and parser
6. Redirection and process control
7. Main shell loop

There is only one command structure (`Cmd`). It stores the command arguments and optional redirection information.

## Files

- `main.c` — complete shell implementation
- `build.sh` — compiles the program
- `test.sh` — automated regression tests
- `run.sh` — builds and then runs the tests
- `README.md` — this file

## Compile

```bash
chmod +x build.sh test.sh run.sh
./build.sh
```

The build command is:

```bash
gcc -Wall -o myshell main.c
```

Run the shell with:

```bash
./myshell
```

Run the automated tests with:

```bash
./test.sh
```

Or build and test together:

```bash
./run.sh
```

## Commands and main system calls

| Command / feature | Main calls used |
|---|---|
| `pwd` | `getcwd()` |
| `ls`, `ls -l` | `opendir()`, `readdir()`, `closedir()`, `lstat()` |
| `cd` | `chdir()` |
| `whoami` | `getuid()`, `getpwuid()` |
| `uname` | `uname()` |
| `mkdir` | `mkdir()` |
| `touch` | `open()`, `close()`, `utimensat()` |
| `rm` | `unlink()`, `rmdir()` |
| `cp` | `open()`, `read()`, `write()`, `close()`, `stat()` |
| `mv` | `rename()` |
| `cat` | `open()`, `read()`, `write()`, `close()` |
| `echo` | built in, with `getenv()` for `$VAR` |
| `uptime` | reads `/proc/uptime` and `/proc/loadavg` |
| `meminfo` | reads `/proc/meminfo` |
| child process | `fork()` |
| external command | `execvp()` |
| waiting | `waitpid()` |
| pipes | `pipe()`, `dup2()` |
| redirection | `open()`, `dup2()` |
| background cleanup | `waitpid(..., WNOHANG)` |
| Ctrl+C | `sigaction()` / signal handling |

`system()` is not used.

## Supported shell features

The shell supports:

- required Assignment 2 commands
- command arguments
- quoted text with single or double quotes
- escaped characters such as `escaped\ space`
- multiple-stage pipelines, for example:

```bash
echo one two three | tr a-z A-Z | wc -w
```

- input redirection:

```bash
cat < input.txt
```

- output redirection:

```bash
echo hello > output.txt
```

- append redirection:

```bash
echo again >> output.txt
```

- background execution:

```bash
sleep 5 &
```

- command history:

```bash
history
```

- Ctrl+C handling so the shell itself is not normally killed while a foreground child is running
- external commands through `execvp()`

## Virtual root behavior

Suppose the real folder is:

```text
/home/student/OS_Assignment
```

and the shell is started there.

Inside the shell:

```text
pwd
/

cd test
pwd
/test

cd ..
pwd
/

cd ..
pwd
/
```

Paths beginning with `/` are also treated as paths from the shell root, not from the computer's real root.

The shell also checks existing path components with `realpath()` so a symbolic link cannot be used by the shell's own file commands to escape the virtual root.

For external programs, obvious path arguments are converted to the virtual-root path system before `execvp()`. This is an assignment-level path restriction, not a security sandbox for arbitrary third-party programs.

## Parsing approach

The input line is handled in two simple stages.

First, `tokenize()` separates words and operators. It also handles quoted strings and escaped characters.

Then `parse()` groups the tokens into `Cmd` structures. A `Cmd` contains:

```c
char *argv[MAX_ARGS];
int argc;
char *input_file;
char *output_file;
int append;
```

For example:

```bash
cat notes.txt | grep hello > result.txt
```

is parsed as two commands. The first command is `cat notes.txt`. The second command is `grep hello`, with `result.txt` as its output file.

## Parent and child processes

Commands such as `cd` must run in the parent shell because they change the shell's own state.

Other commands can run in a child process created by `fork()`. If a command is not one of the manually implemented commands, the child uses `execvp()`.

For a pipeline, each command gets its own child process. `pipe()` creates the connection and `dup2()` connects the correct pipe end to standard input or standard output.

## Error handling included

The code checks cases such as:

- missing command arguments
- nonexistent files/directories
- `cd` into a regular file
- copying a file onto itself
- removing the virtual root
- invalid pipe syntax
- repeated/leading/trailing pipes
- missing redirection filenames
- repeated output redirection
- unmatched quotes
- too many arguments/tokens
- overly long input lines
- failed `fork()`, `pipe()`, `dup2()`, and file operations

## Automated tests

`test.sh` currently performs 63 checks.

The tests cover:

- virtual-root `pwd` and `cd`
- quoted directory names
- failed `cd`
- `mkdir`, `touch`, and timestamp updates
- `rm` for files and directories
- virtual-root deletion protection
- `cp`, `mv`, and same-file copy protection
- `cat` and `echo`
- single quotes, double quotes, and escaped spaces
- environment variable output
- `ls` and `ls -l`
- `whoami`, `uname`, `uptime`, `meminfo`, and `help`
- `<`, `>`, and `>>`
- built-in and external-command redirection
- external commands using `execvp()`
- three-stage pipelines
- pipeline plus redirection
- invalid parser syntax
- unmatched quotes
- too many arguments
- overly long input
- `exit 7`, invalid exit values, and too many exit arguments
- background execution
- history and its 100-command limit
- virtual-root protection for `cat`, `cp`, `mv`, symbolic links, and virtual absolute paths

A successful run ends with:

```text
Result: 63 passed, 0 failed
All automated tests passed.
```

## summary

The main flow of the shell is:

```text
read command
    ↓
tokenize
    ↓
parse into Cmd structures
    ↓
parent built-in?
    ├── yes → run in shell process
    └── no  → fork child process(es)
                  ↓
             redirection / pipes
                  ↓
          own command or execvp()
                  ↓
               waitpid()
```

The most important concepts to understand are `fork()`, `waitpid()`, `execvp()`, `pipe()`, `dup2()`, file descriptors, and why `cd` must run in the parent process.
