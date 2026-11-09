#!/bin/bash
set -u

# Automated regression tests for myshell.
# Each test checks one behavior and prints a clear PASS/FAIL line.

if [ ! -x ./myshell ]; then
    gcc -Wall -o myshell main.c || exit 1
fi

BIN="$(pwd)/myshell"
WORK="shell_test_area"
PASS=0
FAIL=0

rm -rf "$WORK"
mkdir -p "$WORK"

pass() { echo "[PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL + 1)); }

run_shell() {
    local dir="$1" input="$2" out="$3" err="$4"
    mkdir -p "$dir"
    (
        cd "$dir" || exit 1
        printf '%b' "$input" | "$BIN" > "$out" 2> "$err"
    )
}

file_equals() {
    [ -f "$1" ] && [ "$(cat "$1")" = "$2" ]
}

# ------------------------------------------------------------
# 1. Virtual root and cd
# ------------------------------------------------------------
CASE="$WORK/root_cd"
mkdir -p "$CASE/demo" "$CASE/space dir"
touch "$CASE/plain_file"
run_shell "$CASE" 'pwd\ncd demo\npwd\ncd ..\ncd ..\npwd\nexit\n' root.out root.err

[ "$(sed -n '1p' "$CASE/root.out")" = '/' ] && pass "pwd shows startup directory as /" || fail "pwd shows startup directory as /"
[ "$(sed -n '2p' "$CASE/root.out")" = '/demo' ] && pass "cd enters a child directory" || fail "cd enters a child directory"
[ "$(sed -n '3p' "$CASE/root.out")" = '/' ] && pass "cd .. cannot go above virtual root" || fail "cd .. cannot go above virtual root"

run_shell "$CASE" 'cd demo\ncd\npwd\nexit\n' cdroot.out cdroot.err
grep -qx '/' "$CASE/cdroot.out" && pass "cd with no argument returns to /" || fail "cd with no argument returns to /"

run_shell "$CASE" 'cd "space dir"\npwd\nexit\n' space.out space.err
grep -qx '/space dir' "$CASE/space.out" && pass "cd supports quoted directory names" || fail "cd supports quoted directory names"

run_shell "$CASE" 'cd no_such_dir\npwd\nexit\n' badcd.out badcd.err
grep -q 'cd:' "$CASE/badcd.err" && pass "cd reports a missing directory" || fail "cd reports a missing directory"
grep -qx '/' "$CASE/badcd.out" && pass "shell continues after failed cd" || fail "shell continues after failed cd"

run_shell "$CASE" 'cd plain_file\nexit\n' cdfile.out cdfile.err
grep -q 'not a directory' "$CASE/cdfile.err" && pass "cd rejects a regular file" || fail "cd rejects a regular file"

# ------------------------------------------------------------
# 2. mkdir, touch, rm
# ------------------------------------------------------------
CASE="$WORK/files"
mkdir -p "$CASE"
run_shell "$CASE" 'mkdir newdir\ntouch newfile.txt\nexit\n' create.out create.err
[ -d "$CASE/newdir" ] && pass "mkdir creates a directory" || fail "mkdir creates a directory"
[ -f "$CASE/newfile.txt" ] && pass "touch creates a file" || fail "touch creates a file"

run_shell "$CASE" 'mkdir newdir\nexit\n' again.out again.err
grep -q 'mkdir:' "$CASE/again.err" && pass "mkdir reports existing directory" || fail "mkdir reports existing directory"

printf 'data\n' > "$CASE/time.txt"
OLD=$(stat -c %Y "$CASE/time.txt")
sleep 1
run_shell "$CASE" 'touch time.txt\nexit\n' touch.out touch.err
NEW=$(stat -c %Y "$CASE/time.txt")
[ "$NEW" -gt "$OLD" ] && pass "touch updates file timestamp" || fail "touch updates file timestamp"

printf 'remove\n' > "$CASE/remove.txt"
mkdir -p "$CASE/empty" "$CASE/nonempty"
printf 'x\n' > "$CASE/nonempty/x"
run_shell "$CASE" 'rm remove.txt\nrm empty\nrm nonempty\nexit\n' rm.out rm.err
[ ! -e "$CASE/remove.txt" ] && pass "rm removes a file" || fail "rm removes a file"
[ ! -d "$CASE/empty" ] && pass "rm removes an empty directory" || fail "rm removes an empty directory"
[ -d "$CASE/nonempty" ] && grep -q 'rm:' "$CASE/rm.err" && pass "rm refuses non-empty directory" || fail "rm refuses non-empty directory"

run_shell "$CASE" 'rm /\nexit\n' rootrm.out rootrm.err
grep -q 'cannot remove the shell root' "$CASE/rootrm.err" && pass "rm protects virtual root" || fail "rm protects virtual root"

# ------------------------------------------------------------
# 3. cp and mv
# ------------------------------------------------------------
CASE="$WORK/copy_move"
mkdir -p "$CASE"
printf 'copy-content\n' > "$CASE/source.txt"
run_shell "$CASE" 'cp source.txt copied.txt\nmv copied.txt moved.txt\nexit\n' copy.out copy.err
file_equals "$CASE/moved.txt" 'copy-content' && pass "cp copies file contents" || fail "cp copies file contents"
[ ! -e "$CASE/copied.txt" ] && pass "mv renames a file" || fail "mv renames a file"

printf 'keep-this-content\n' > "$CASE/same.txt"
run_shell "$CASE" 'cp same.txt same.txt\nexit\n' same.out same.err
file_equals "$CASE/same.txt" 'keep-this-content' && pass "cp same-file protection preserves data" || fail "cp same-file protection preserves data"
grep -q 'same file' "$CASE/same.err" && pass "cp same-file case reports an error" || fail "cp same-file case reports an error"

run_shell "$CASE" 'cp missing.txt x.txt\nexit\n' cpmiss.out cpmiss.err
[ ! -e "$CASE/x.txt" ] && grep -q 'cp:' "$CASE/cpmiss.err" && pass "cp reports missing source" || fail "cp reports missing source"

run_shell "$CASE" 'mv missing.txt y.txt\nexit\n' mvmiss.out mvmiss.err
[ ! -e "$CASE/y.txt" ] && grep -q 'mv:' "$CASE/mvmiss.err" && pass "mv reports missing source" || fail "mv reports missing source"

# ------------------------------------------------------------
# 4. cat, echo and quoting
# ------------------------------------------------------------
CASE="$WORK/text"
mkdir -p "$CASE"
printf 'cat-content\n' > "$CASE/cat.txt"
run_shell "$CASE" 'cat cat.txt\necho hello world\necho "two words"\necho '\''single quoted'\''\necho escaped\ space\nexit\n' text.out text.err

grep -q 'cat-content' "$CASE/text.out" && pass "cat displays file contents" || fail "cat displays file contents"
grep -q '^hello world$' "$CASE/text.out" && pass "echo prints normal arguments" || fail "echo prints normal arguments"
grep -q '^two words$' "$CASE/text.out" && pass "double quotes preserve spaces" || fail "double quotes preserve spaces"
grep -q '^single quoted$' "$CASE/text.out" && pass "single quotes preserve spaces" || fail "single quotes preserve spaces"
grep -q '^escaped space$' "$CASE/text.out" && pass "backslash escapes a space" || fail "backslash escapes a space"

run_shell "$CASE" 'echo $HOME\nexit\n' env.out env.err
grep -q "$HOME" "$CASE/env.out" && pass "echo expands environment variables" || fail "echo expands environment variables"

run_shell "$CASE" 'cat missing.txt\nexit\n' catmiss.out catmiss.err
grep -q 'cat:' "$CASE/catmiss.err" && pass "cat reports missing file" || fail "cat reports missing file"

# ------------------------------------------------------------
# 5. Inspection and system commands
# ------------------------------------------------------------
CASE="$WORK/inspection"
mkdir -p "$CASE/folder"
printf 'abc\n' > "$CASE/listed.txt"
run_shell "$CASE" 'ls\nls -l\nwhoami\nuname\nuptime\nmeminfo\nhelp\nexit\n' inspect.out inspect.err

grep -q 'listed.txt' "$CASE/inspect.out" && pass "ls lists directory contents" || fail "ls lists directory contents"
grep -Eq -- '-rw[-rwx]{7}.*listed.txt' "$CASE/inspect.out" && pass "ls -l prints type and permissions" || fail "ls -l prints type and permissions"
grep -q "$(whoami)" "$CASE/inspect.out" && pass "whoami prints username" || fail "whoami prints username"
grep -q 'System Name' "$CASE/inspect.out" && pass "uname prints system information" || fail "uname prints system information"
grep -q 'Uptime:' "$CASE/inspect.out" && pass "uptime prints uptime" || fail "uptime prints uptime"
grep -q 'Load average:' "$CASE/inspect.out" && pass "uptime prints load average" || fail "uptime prints load average"
grep -q 'MemTotal:' "$CASE/inspect.out" && pass "meminfo prints memory information" || fail "meminfo prints memory information"
grep -q 'Custom Shell Commands' "$CASE/inspect.out" && pass "help prints command summary" || fail "help prints command summary"

# ------------------------------------------------------------
# 6. Redirection
# ------------------------------------------------------------
CASE="$WORK/redirection"
mkdir -p "$CASE"
run_shell "$CASE" 'echo first > data.txt\necho second >> data.txt\ncat < data.txt\nprintf external > external.txt\nexit\n' redir.out redir.err

[ "$(sed -n '1p' "$CASE/data.txt")" = first ] && pass "> creates/truncates output file" || fail "> creates/truncates output file"
[ "$(sed -n '2p' "$CASE/data.txt")" = second ] && pass ">> appends output" || fail ">> appends output"
grep -q first "$CASE/redir.out" && grep -q second "$CASE/redir.out" && pass "< redirects standard input" || fail "< redirects standard input"
file_equals "$CASE/external.txt" external && pass "external command redirection works" || fail "external command redirection works"

run_shell "$CASE" 'echo built-in > builtin.txt\nexit\n' built.out built.err
file_equals "$CASE/builtin.txt" built-in && pass "built-in redirection works" || fail "built-in redirection works"

# ------------------------------------------------------------
# 7. Pipes and external commands
# ------------------------------------------------------------
CASE="$WORK/pipes"
mkdir -p "$CASE"
run_shell "$CASE" 'printf external-command > external_cmd.txt\necho alpha beta gamma | tr a-z A-Z | wc -w\necho hello | tr a-z A-Z > piped.txt\nexit\n' pipe.out pipe.err

file_equals "$CASE/external_cmd.txt" external-command && pass "execvp runs external command" || fail "execvp runs external command"
grep -Eq '(^|[[:space:]])3([[:space:]]|$)' "$CASE/pipe.out" && pass "three-stage pipeline works" || fail "three-stage pipeline works"
file_equals "$CASE/piped.txt" HELLO && pass "pipeline output redirection works" || fail "pipeline output redirection works"

# ------------------------------------------------------------
# 8. Parser errors
# ------------------------------------------------------------
CASE="$WORK/parser"
mkdir -p "$CASE"
run_shell "$CASE" 'echo hello |\n| echo hello\necho a || wc\necho x >\necho x > > y\necho x > a > b\necho "unterminated\nexit\n' parser.out parser.err

[ "$(grep -c 'invalid command syntax' "$CASE/parser.err" || true)" -eq 6 ] && pass "parser rejects bad pipe/redirection syntax" || fail "parser rejects bad pipe/redirection syntax"
grep -q 'unmatched quote' "$CASE/parser.err" && pass "tokenizer rejects unmatched quotes" || fail "tokenizer rejects unmatched quotes"

python - <<'PY' > "$CASE/many_args.txt"
print('echo ' + ' '.join('x' for _ in range(80)))
print('exit')
PY
(cd "$CASE" && "$BIN" < many_args.txt > many.out 2> many.err)
grep -q 'invalid command syntax' "$CASE/many.err" && pass "parser rejects too many arguments" || fail "parser rejects too many arguments"

python - <<'PY' > "$CASE/long.txt"
print('echo ' + 'x' * 1500)
print('exit')
PY
(cd "$CASE" && "$BIN" < long.txt > long.out 2> long.err)
grep -q 'input line is too long' "$CASE/long.err" && pass "overlong input is rejected" || fail "overlong input is rejected"

# ------------------------------------------------------------
# 9. Exit behavior
# ------------------------------------------------------------
CASE="$WORK/exit"
mkdir -p "$CASE"
set +e
(cd "$CASE" && printf 'exit 7\n' | "$BIN" >/dev/null 2>/dev/null)
CODE=$?
set -e 2>/dev/null || true
[ "$CODE" -eq 7 ] && pass "exit 7 returns status 7" || fail "exit 7 returns status 7"

set +e
(cd "$CASE" && printf 'exit nope\n' | "$BIN" >/dev/null 2> bad.err)
CODE=$?
set -e 2>/dev/null || true
[ "$CODE" -eq 2 ] && grep -q 'numeric argument required' "$CASE/bad.err" && pass "invalid exit status returns 2" || fail "invalid exit status returns 2"

run_shell "$CASE" 'exit 1 2\necho still-running\nexit\n' many.out many.err
grep -q 'too many arguments' "$CASE/many.err" && grep -q 'still-running' "$CASE/many.out" && pass "exit with too many arguments keeps shell running" || fail "exit with too many arguments keeps shell running"

# ------------------------------------------------------------
# 10. Background and history
# ------------------------------------------------------------
CASE="$WORK/background"
mkdir -p "$CASE"
START=$(date +%s%N)
run_shell "$CASE" 'sleep 1 &\necho returned\nexit\n' bg.out bg.err
END=$(date +%s%N)
ELAPSED=$(( (END - START) / 1000000 ))

grep -q 'background pid' "$CASE/bg.out" && grep -q returned "$CASE/bg.out" && pass "background command returns control" || fail "background command returns control"
[ "$ELAPSED" -lt 900 ] && pass "background child does not block shell" || fail "background child does not block shell"

run_shell "$CASE" 'echo one\necho two\nhistory\nexit\n' hist.out hist.err
grep -qE '[0-9]+  echo one' "$CASE/hist.out" && pass "history records commands" || fail "history records commands"
grep -qE '[0-9]+  history' "$CASE/hist.out" && pass "history records itself" || fail "history records itself"

(cd "$CASE" && { for i in $(seq 1 105); do echo "echo item$i"; done; echo history; echo exit; } | "$BIN" > limit.out 2>/dev/null)
grep -q 'echo item105' "$CASE/limit.out" && pass "history keeps newest commands" || fail "history keeps newest commands"
! grep -qE '[0-9]+  echo item1$' "$CASE/limit.out" && pass "history enforces 100-command limit" || fail "history enforces 100-command limit"

# ------------------------------------------------------------
# 11. Virtual-root boundary
# ------------------------------------------------------------
OUTSIDE="$WORK/outside_secret.txt"
printf 'outside-secret\n' > "$OUTSIDE"
CASE="$WORK/jail/root"
mkdir -p "$CASE"
ln -s ../.. "$CASE/escape_link"

run_shell "$CASE" 'cat ../../outside_secret.txt\ncp ../../outside_secret.txt copied.txt\nmv ../../outside_secret.txt moved.txt\ncat escape_link/outside_secret.txt\nmkdir /absolute_dir\nexit\n' jail.out jail.err

! grep -q outside-secret "$CASE/jail.out" && pass "cat cannot escape virtual root" || fail "cat cannot escape virtual root"
[ ! -e "$CASE/copied.txt" ] && pass "cp cannot read above virtual root" || fail "cp cannot read above virtual root"
[ -f "$OUTSIDE" ] && [ ! -e "$CASE/moved.txt" ] && pass "mv cannot move file above virtual root" || fail "mv cannot move file above virtual root"
grep -q 'outside the shell root' "$CASE/jail.err" && pass "symlink escape is blocked" || fail "symlink escape is blocked"
[ -d "$CASE/absolute_dir" ] && pass "virtual absolute path remains inside root" || fail "virtual absolute path remains inside root"

# ------------------------------------------------------------
# Final result
# ------------------------------------------------------------
echo
echo "Result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
echo "All automated tests passed."
