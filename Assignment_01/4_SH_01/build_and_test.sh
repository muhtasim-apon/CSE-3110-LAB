#!/bin/bash
#
# build_and_test.sh - compile myshell and run the automated test cases
#
# Usage:
#   ./build_and_test.sh          compile, then run all tests
#   ./build_and_test.sh build    compile only
#   ./build_and_test.sh run      compile, then start the shell (root = this folder)
#
# Every test starts myshell inside a fresh empty folder ($JAIL), so that folder is the
# shell's root "/". A decoy file is placed one level above it, in $BASE, to prove that
# no command can reach outside the root.

cd "$(dirname "$0")" || exit 1

echo "Compiling: gcc -Wall -o myshell main.c"
WARN=$(gcc -Wall -o myshell main.c 2>&1)
if [ $? -ne 0 ]; then
    echo "$WARN"
    echo "Compilation FAILED"
    exit 1
fi
echo "Compilation successful"
echo

[ "$1" = "build" ] && exit 0
if [ "$1" = "run" ]; then
    ./myshell
    exit $?
fi

SHELL_BIN="$(pwd)/myshell"
SRC="$(pwd)/main.c"
BASE=$(mktemp -d)
JAIL="$BASE/jail"
export LC_ALL=C
PASS=0
FAIL=0

# fresh "bash setup": empty jail, decoy outside it, then run the setup inside the jail
fresh() {
    rm -rf "$JAIL"
    mkdir -p "$JAIL"
    echo "secret" > "$BASE/decoy.txt"
    rm -rf "$BASE/out" "$BASE/x" "$BASE/x.txt" "$BASE/newfile"
    [ -n "$1" ] && (cd "$JAIL" && eval "$1")
}
# run "myshell input" (\n separates lines)
run() {
    (cd "$JAIL" && printf '%b\n' "$1" | "$SHELL_BIN" 2>&1)
}
ok()  { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; echo "   input   : $2"; echo "   expected: $3"; echo "   got     : $4"; FAIL=$((FAIL + 1)); }

# check "name" "input" "exact expected output" ["setup"]
check() {
    fresh "$4"; out=$(run "$2")
    if [ "$out" = "$3" ]; then ok "$1"; else bad "$1" "$2" "$3" "$out"; fi
}
# contains "name" "input" "text the output must contain" ["setup"]
contains() {
    fresh "$4"; out=$(run "$2")
    if printf '%s' "$out" | grep -qF -- "$3"; then ok "$1"; else bad "$1" "$2" "(contains) $3" "$out"; fi
}
# status "name" "input" "expected exit status of myshell" ["setup"]
status() {
    fresh "$4"; (cd "$JAIL" && printf '%b\n' "$2" | "$SHELL_BIN" >/dev/null 2>&1); st=$?
    if [ "$st" = "$3" ]; then ok "$1"; else bad "$1" "$2" "status $3" "status $st"; fi
}
# fact "name" "bash condition checked after the previous test"
fact() {
    if (cd "$JAIL" && eval "$2"); then ok "$1"; else bad "$1" "(after previous test)" "$2" "false"; fi
}

DENY="Permission denied (outside root directory)"
R=$(basename "$JAIL")     # pwd and the prompt show the root by its folder name
echo "Running tests in $JAIL"

echo; echo "== 1. Root folder (jail) =="
check    "pwd at start shows folder name" "pwd"                               "$R"
check    "cd .. at root is blocked"        "cd ..\npwd"                        "cd: ..: $DENY"$'\n'"$R"
check    "cd d1 then cd .. returns to root" "mkdir d1\ncd d1\npwd\ncd ..\npwd"  "$R/d1"$'\n'"$R"
check    "second cd .. from d1 blocked"    "mkdir d1\ncd d1\ncd ..\ncd ..\npwd" "cd: ..: $DENY"$'\n'"$R"
check    "cd ../../.. blocked"             "cd ../../..\npwd"                  "cd: ../../..: $DENY"$'\n'"$R"
check    "cd d1/../.. blocked"             "mkdir d1\ncd d1/../..\npwd"        "cd: d1/../..: $DENY"$'\n'"$R"
check    "cd ./../ blocked"                "cd ./../"                          "cd: ./../: $DENY"
check    "cd a/b/../../.. blocked"         "mkdir -p a/b\ncd a/b/../../..\npwd" "cd: a/b/../../..: $DENY"$'\n'"$R"
check    "cd a/b/../.. stays inside"       "mkdir -p a/b\ncd a/b/../..\npwd"   "$R"
check    "cd / goes to root"               "mkdir -p a/b\ncd a/b\ncd /\npwd"   "$R"
check    "cd (no args) goes to root"       "mkdir -p a/b\ncd a/b\ncd\npwd"     "$R"
check    "cd ~ goes to root"               "mkdir a\ncd a\ncd ~\npwd"          "$R"
check    "cd ~/a"                          "mkdir a\ncd ~/a\npwd"              "$R/a"
check    "cd - returns to previous"        "mkdir a\ncd a\ncd /\ncd -\npwd"    "$R/a"$'\n'"$R/a"
check    "cd /a/b absolute inside root"    "mkdir -p a/b\ncd /a/b\npwd"        "$R/a/b"
check    "cd /// is root"                  "mkdir a\ncd a\ncd ///\npwd"        "$R"
check    "cd /home is not the real /home"  "cd /home"                          "cd: /home: No such file or directory"
check    "cd to real parent path fails"    "cd $BASE"                          "cd: $BASE: No such file or directory"
check    "\$PWD is virtual"                'mkdir a\ncd a\necho $PWD'          "$R/a"
check    "ls .. blocked"                   "ls .."                             "ls: ..: $DENY"
check    "ls ../.. blocked"                "mkdir a\ncd a\nls ../.."           "ls: ../..: $DENY"
check    "ls /etc is inside root"          "ls /etc"                           "ls: cannot access '/etc': No such file or directory"
check    "cat ../decoy.txt blocked"        "cat ../decoy.txt"                  "cat: ../decoy.txt: $DENY"
check    "cp ../decoy.txt blocked"         "cp ../decoy.txt x.txt"             "cp: ../decoy.txt: $DENY"
fact     "  nothing copied"                "[ ! -e x.txt ]"
check    "cp to ../ blocked"               "cp a.txt ../x.txt"                 "cp: ../x.txt: $DENY"  "echo hi > a.txt"
fact     "  nothing written outside"       "[ ! -e $BASE/x.txt ]"
check    "mv to ../ blocked"               "mv a.txt ../x.txt"                 "mv: ../x.txt: $DENY"  "echo hi > a.txt"
fact     "  file still inside"             "[ -f a.txt ] && [ ! -e $BASE/x.txt ]"
check    "mv ../decoy.txt blocked"         "mv ../decoy.txt here.txt"          "mv: ../decoy.txt: $DENY"
check    "touch ../x blocked"              "touch ../x"                        "touch: ../x: $DENY"
fact     "  nothing created outside"       "[ ! -e $BASE/x ]"
check    "mkdir ../x blocked"              "mkdir ../x"                        "mkdir: ../x: $DENY"
check    "mkdir -p ../../x blocked"        "mkdir -p ../../x"                  "mkdir: ../../x: $DENY"
check    "rm ../decoy.txt blocked"         "rm ../decoy.txt"                   "rm: ../decoy.txt: $DENY"
fact     "  decoy still exists"            "[ -f $BASE/decoy.txt ]"
check    "rm -r .. refused"                "rm -r .."                          "rm: refusing to remove '.' or '..' directory: skipping '..'"
fact     "  decoy still exists"            "[ -f $BASE/decoy.txt ]"
check    "rm . refused"                    "rm ."                              "rm: refusing to remove '.' or '..' directory: skipping '.'"
check    "rm / refused"                    "rm -r /"                           "rm: refusing to remove the root directory '/'"
check    "rm folder that holds cwd refused" "mkdir a\ncd a\nrm /a"             "rm: cannot remove '/a': current directory is inside it"
check    "mv / refused"                    "mv / x"                            "mv: cannot move the root directory"
check    "symlink escape: cd blocked"      "cd esc"                            "cd: esc: $DENY"       "ln -s $BASE esc"
check    "symlink escape: ls blocked"      "ls esc"                            "ls: esc: $DENY"       "ln -s $BASE esc"
check    "symlink escape: cat blocked"     "cat esc/decoy.txt"                 "cat: esc/decoy.txt: $DENY" "ln -s $BASE esc"
check    "symlink escape: cp into blocked" "cp a.txt esc"                      "cp: esc: $DENY"       "ln -s $BASE esc; echo hi > a.txt"
check    "symlink escape: redirect blocked" "echo hi > esc/out"                "myshell: esc/out: $DENY" "ln -s $BASE esc"
fact     "  nothing written outside"       "[ ! -e $BASE/out ]"
check    "dangling symlink out blocked"    "touch dl"                          "touch: dl: $DENY"     "ln -s $BASE/newfile dl"
fact     "  nothing created outside"       "[ ! -e $BASE/newfile ]"
check    "symlink inside root works"       "cd lnk\npwd"                       "$R/real"                "mkdir real; ln -s real lnk"
check    "redirect > ../out blocked"       "echo hi > ../out"                  "myshell: ../out: $DENY"
fact     "  nothing written outside"       "[ ! -e $BASE/out ]"
check    "redirect < ../decoy blocked"     "cat < ../decoy.txt"                "myshell: ../decoy.txt: $DENY"
check    "external: /bin/ls .. blocked"    "/bin/ls .."                        "/bin/ls: ..: $DENY"
check    "external: wc ../decoy blocked"   "wc -l ../decoy.txt"                "wc: ../decoy.txt: $DENY"
check    "external: /x maps into root"     "/bin/cat /f.txt"                   "inside"               "echo inside > f.txt"
check    "external: ../script blocked"     "../evil.sh"                        "myshell: ../evil.sh: $DENY"

echo; echo "== 2. pwd / cd errors =="
check    "pwd in nested dir"               "mkdir -p x/y\ncd x\ncd y\npwd"     "$R/x/y"
check    "cd missing dir"                  "cd nodir"                          "cd: nodir: No such file or directory"
check    "cd into a file"                  "cd f.txt"                          "cd: f.txt: Not a directory"  "touch f.txt"
check    "cd too many args"                "mkdir a b\ncd a b\npwd"            "cd: too many arguments"$'\n'"$R"

echo; echo "== 3. ls =="
check    "ls empty dir"                    "ls"                                ""
check    "ls sorted, dirs marked /"        "ls"                                $'a.txt\nb.txt\nc/'  "touch b.txt a.txt; mkdir c"
check    "ls hides hidden files"           "ls"                                "v"                  "touch .hid v"
check    "ls -a at root (no ..)"           "ls -a"                             $'.\n.hid'           "touch .hid"
check    "ls -a in subdir shows .."        "mkdir d\ncd d\nls -a"              $'.\n..'
check    "ls a file"                       "ls f.txt"                          "f.txt"              "touch f.txt"
check    "ls subdir path"                  "ls a/b"                            "c.txt"              "mkdir -p a/b; touch a/b/c.txt"
check    "ls several dirs"                 "ls a b"                            $'a:\n1\n\nb:\n2'    "mkdir a b; touch a/1 b/2"
contains "ls -l file line"                 "ls -l"                             "-rw-r--r--  1"      "printf hello > f.txt; chmod 644 f.txt"
contains "ls -l shows size"                "ls -l"                             "       5 "          "printf hello > f.txt"
contains "ls -l dir line"                  "ls -l"                             "drwx"               "mkdir d"
contains "ls -la combined"                 "ls -la"                            ".hid"               "touch .hid"
check    "ls missing"                      "ls nope"                           "ls: cannot access 'nope': No such file or directory"
check    "ls invalid option"               "ls -z"                             "ls: invalid option -- 'z'"

echo; echo "== 4. whoami / uname =="
check    "whoami"                          "whoami"                            "$(id -un)"
contains "uname"                           "uname"                             "System Name : $(uname -s)"
contains "uname machine"                   "uname"                             "Machine     : $(uname -m)"
check    "uname -a"                        "uname -a"                          "$(uname -snrvm)"
check    "uname -r"                        "uname -r"                          "$(uname -r)"
check    "uname -sm"                       "uname -sm"                         "$(uname -sm)"
check    "uname invalid option"            "uname -x"                          "uname: invalid option -- 'x'"

echo; echo "== 5. mkdir =="
check    "mkdir"                           "mkdir d\nls"                       "d/"
fact     "  mode is 755"                   '[ "$(stat -c %a d)" = 755 ]'
check    "mkdir several"                   "mkdir a b c\nls"                   $'a/\nb/\nc/'
check    "mkdir existing"                  "mkdir d\nmkdir d"                  "mkdir: cannot create directory 'd': File exists"
check    "mkdir -p nested"                 "mkdir -p x/y/z\ncd x/y/z\npwd"     "$R/x/y/z"
check    "mkdir -p existing is ok"         "mkdir -p x\nmkdir -p x\necho ok"   "ok"
check    "mkdir missing parent"            "mkdir a/b"                         "mkdir: cannot create directory 'a/b': No such file or directory"
check    "mkdir no args"                   "mkdir"                             "usage: mkdir [-p] <directory> ..."

echo; echo "== 6. touch =="
check    "touch new file"                  "touch t.txt\nls"                   "t.txt"
fact     "  file is empty"                 "[ -f t.txt ] && [ ! -s t.txt ]"
check    "touch several"                   "touch a b\nls"                     $'a\nb'
check    "touch keeps content"             "touch k.txt\ncat k.txt"            "keep"               "echo keep > k.txt; touch -d 2000-01-01 k.txt"
fact     "  time updated"                  '[ "$(stat -c %Y k.txt)" -gt 1000000000 ]'
check    "touch a directory"               "touch d\necho ok"                  "ok"                 "mkdir d"
check    "touch missing parent"            "touch no/x"                        "touch: cannot touch 'no/x': No such file or directory"
check    "touch no args"                   "touch"                             "usage: touch <file> ..."

echo; echo "== 7. rm =="
check    "rm file"                         "rm f\nls"                          ""                   "touch f"
check    "rm empty dir"                    "rm d\nls"                          ""                   "mkdir d"
check    "rm non-empty dir"                "rm d"                              "rm: cannot remove 'd': Directory not empty (use rm -r)" "mkdir d; touch d/x"
check    "rm -r non-empty dir"             "rm -r d\nls"                       ""                   "mkdir -p d/e; touch d/x d/e/y"
check    "rm missing"                      "rm nope"                           "rm: cannot remove 'nope': No such file or directory"
check    "rm -f missing is quiet"          "rm -f nope\necho ok"               "ok"
check    "rm several"                      "rm a b\nls"                        "c"                  "touch a b c"
check    "rm symlink (not its target)"     "rm lnk\nls"                        "real/"              "mkdir real; ln -s real lnk"
check    "rm no args"                      "rm"                                "usage: rm [-r] [-f] <file | directory> ..."

echo; echo "== 8. cp =="
check    "cp and cat"                      "cp s.txt d.txt\ncat d.txt"         "hello"              "echo hello > s.txt"
check    "cp overwrites"                   "cp s.txt d.txt\ncat d.txt"         "new"                "echo new > s.txt; echo oldoldold > d.txt"
check    "cp into dir"                     "cp s.txt dir\ncat dir/s.txt"       "hello"              "echo hello > s.txt; mkdir dir"
check    "cp several into dir"             "cp a b dir\nls dir"                $'a\nb'              "touch a b; mkdir dir"
check    "cp several, target not dir"      "cp a b c"                          "cp: target 'c' is not a directory" "touch a b"
check    "cp same file"                    "cp s.txt s.txt"                    "cp: 's.txt' and 's.txt' are the same file" "touch s.txt"
check    "cp missing source"               "cp nope x"                         "cp: cannot stat 'nope': No such file or directory"
check    "cp a directory"                  "cp dir x"                          "cp: -r not specified; omitting directory 'dir'" "mkdir dir"
check    "cp missing dest folder"          "cp s.txt no/x"                     "cp: cannot create 'no/x': No such file or directory" "touch s.txt"
check    "cp one arg"                      "cp s.txt"                          "usage: cp <source> <destination>  |  cp <source>... <directory>"
check    "cp 1 MB binary file"             "cp big.bin big2.bin"               ""                   "head -c 1048576 /dev/urandom > big.bin"
fact     "  copy is identical"             "cmp -s big.bin big2.bin"
check    "cp keeps mode"                   "cp s.sh d.sh"                      ""                   "touch s.sh; chmod 755 s.sh"
fact     "  mode is 755"                   '[ "$(stat -c %a d.sh)" = 755 ]'

echo; echo "== 9. mv =="
check    "mv rename"                       "mv a.txt b.txt\nls\ncat b.txt"     $'b.txt\nhello'      "echo hello > a.txt"
check    "mv into dir"                     "mv a.txt dir\nls dir"              "a.txt"              "touch a.txt; mkdir dir"
check    "mv dir rename"                   "mv d e\nls"                        "e/"                 "mkdir d"
check    "mv dir into itself"              "mv d d/sub"                        "mv: cannot move 'd' to a subdirectory of itself" "mkdir d"
check    "mv missing source"               "mv nope x"                         "mv: cannot stat 'nope': No such file or directory"
check    "mv one arg"                      "mv a"                              "usage: mv <source> <destination>  |  mv <source>... <directory>"

echo; echo "== 10. cat =="
check    "cat file"                        "cat s.txt"                         "hello file"         "echo 'hello file' > s.txt"
check    "cat several"                     "cat a b"                           $'A\nB'              "echo A > a; echo B > b"
check    "cat missing + valid"             "cat nope a"                        $'cat: nope: No such file or directory\nA' "echo A > a"
check    "cat a directory"                 "cat d"                             "cat: d: Is a directory" "mkdir d"
check    "cat empty file"                  "cat e"                             ""                   "touch e"
check    "cat stdin from pipe"             "echo piped | cat"                  "piped"

echo; echo "== 11. echo and variables =="
check    "echo"                            "echo hello"                        "hello"
check    "echo collapses spaces"           "echo hello    world"               "hello world"
check    "echo double quotes"              'echo "a   b"'                      "a   b"
check    "echo single quotes literal"      "echo '\$HOME'"                     '$HOME'
check    "echo \$HOME"                     'echo $HOME'                        "$HOME"
check    "echo \${HOME}x"                  'echo ${HOME}x'                     "${HOME}x"
check    "echo \$PATH"                     'echo $PATH'                        "$PATH"
check    "echo var in double quotes"       'echo "[$HOME]"'                    "[$HOME]"
check    "echo undefined var"              'echo [$NOPE_XYZ]'                  "[]"
check    "echo \$? after success"          'echo hi\necho $?'                  $'hi\n0'
check    "echo \$? after failure"          'cat nope\necho $?'                 $'cat: nope: No such file or directory\n1'
check    "echo \$? after unknown cmd"      'nosuchcmd\necho $?'                $'nosuchcmd: command not found\n127'
check    "echo -n"                         "echo -n hi\necho there"            "hithere"
check    "echo no args"                    "echo"                              ""
check    "echo escaped space"              'echo a\\ \\ b'                     "a  b"
check    "echo lone \$"                    'echo $ 5'                          '$ 5'
check    "unterminated quote"              'echo "abc'                         "myshell: syntax error: unterminated quote"
check    "bad substitution"                'echo ${'                           "myshell: bad substitution"

echo; echo "== 12. uptime / meminfo =="
contains "uptime"                          "uptime"                            "Uptime       :"
contains "uptime load"                     "uptime"                            "Load average :"
contains "meminfo total"                   "meminfo"                           "MemTotal"
contains "meminfo available"               "meminfo"                           "MemAvailable"
contains "meminfo used"                    "meminfo"                           "MemUsed"

echo; echo "== 13. help / history =="
contains "help lists cd"                   "help"                              "cd [dir|-|~|/]"
contains "help lists operators"            "help"                              "Operators:"
check    "history"                         "echo a\necho b\nhistory"           $'a\nb\n    1  echo a\n    2  echo b\n    3  history'
check    "history N"                       "echo a\necho b\nhistory 1"         $'a\nb\n    3  history 1'
check    "!! repeats last"                 "echo hi\n!!"                       $'hi\necho hi\nhi'
check    "!N repeats number N"             "echo x\necho y\n!1"                $'x\ny\necho x\nx'
check    "!! with no history"              "!!"                                "myshell: !!: event not found"
check    "!99 not found"                   "echo a\n!99"                       $'a\nmyshell: !99: event not found'
check    "history bad arg"                 "history abc"                       "history: abc: numeric argument required"
check    "blank lines not in history"      "\n\nhistory"                       "    1  history"

echo; echo "== 14. exit =="
check    "exit stops the shell"            "exit\necho after"                  ""
status   "exit status 0"                   "exit"                              0
status   "exit 5"                          "exit 5"                            5
status   "exit 256 wraps to 0"             "exit 256"                          0
status   "exit uses last status"           "cat nope\nexit"                    1
status   "exit abc"                        "exit abc"                          2
check    "exit abc message"                "exit abc"                          "exit: abc: numeric argument required"
check    "exit too many args"              "exit 1 2\necho still"              $'exit: too many arguments\nstill'
status   "EOF (Ctrl+D) exits"              "echo hi"                           0

echo; echo "== 15. Parsing and errors =="
check    "empty lines"                     "\n\n\necho ok"                     "ok"
check    "spaces and tabs"                 "   \t  echo \t tab  "              "tab"
check    "comment line"                    "# comment\necho ok"                "ok"
check    "unknown command"                 "notacommand"                       "notacommand: command not found"
check    "external program with args"      "printf %s-%s a b"                  "a-b"
contains "line too long"                   "echo $(printf 'x%.0s' {1..1100})\necho next" "input line too long"
contains "  shell continues after"         "echo $(printf 'x%.0s' {1..1100})\necho next" "next"
contains "too many arguments"              "echo $(printf 'a %.0s' {1..70})"   "too many arguments"

echo; echo "== 16. Advanced: redirection, pipes, background =="
check    "> creates file"                  "echo hi > o.txt\ncat o.txt"        "hi"
check    "> without spaces"                "echo hi>o.txt\ncat o.txt"          "hi"
check    "> truncates"                     "echo longline > o\necho s > o\ncat o" "s"
check    ">> appends"                      "echo a > o\necho b >> o\ncat o"    $'a\nb'
check    "< input"                         "cat < in.txt"                      "from file"          "echo 'from file' > in.txt"
check    "< and > together"                "cat < in.txt > out.txt\ncat out.txt" "from file"        "echo 'from file' > in.txt"
check    "< missing file"                  "cat < nope"                        "myshell: nope: No such file or directory"
check    "ls | cat"                        "ls | cat"                          $'x\ny'              "touch x y"
check    "3-stage pipe"                    "echo hello | cat | tr a-z A-Z"     "HELLO"
check    "pipe into file"                  "ls a | cat > list.txt\ncat list.txt" "a"               "touch a"
contains "builtin in pipe"                 "help | cat"                        "Operators:"
check    "cd in pipe does not change dir"  "mkdir d\ncd d | cat\npwd"          "$R"
check    "syntax: ls >"                    "ls >"                              "myshell: syntax error near unexpected token \`newline'"
check    "syntax: | ls"                    "| ls"                              "myshell: syntax error near unexpected token \`|'"
check    "syntax: ls | | cat"              "ls | | cat"                        "myshell: syntax error near unexpected token \`|'"
check    "syntax: ls |"                    "ls |"                              "myshell: syntax error: missing command"
check    "syntax: ls & ls"                 "ls & ls"                           "myshell: syntax error near unexpected token \`&'"
check    "syntax: > >"                     "echo > > f"                        "myshell: syntax error near unexpected token \`>'"
fresh; start=$(date +%s%N); (cd "$JAIL" && printf 'sleep 2 &\necho done\n' | "$SHELL_BIN" > "$BASE/bg.out" 2>&1); end=$(date +%s%N)
out=$(cat "$BASE/bg.out")
if [ $(( (end - start) / 1000000 )) -lt 1500 ] && echo "$out" | grep -q "^\[1\] [0-9]*$" && echo "$out" | grep -q "^done$"; then
    ok "background does not block"
else
    bad "background does not block" "sleep 2 &" "[1] <pid> and done in < 1.5 s" "$out ($(( (end - start) / 1000000 )) ms)"
fi
contains "background Done message"        "sleep 0.2 &\n/bin/sleep 0.5\necho x" "[1] Done (status 0)    sleep"
contains "background with redirect"       "echo bg > b.txt &\n/bin/sleep 0.3"  "Done (status 0)    echo"
fact     "  background job wrote file"     '[ "$(cat b.txt)" = bg ]'

echo; echo "== 17. Corner cases from run_tests.sh =="
check    "relative symlink .. : cd blocked" "cd escape_link"                   "cd: escape_link: $DENY"   "ln -s .. escape_link"
check    "relative symlink .. : ls blocked" "ls escape_link"                   "ls: escape_link: $DENY"   "ln -s .. escape_link"
check    "relative symlink .. : cat blocked" "cat escape_link/decoy.txt"       "cat: escape_link/decoy.txt: $DENY" "ln -s .. escape_link"
check    "pipe without spaces"             "echo piped|cat"                    "piped"
check    "built-in on left of pipe"        "history | cat"                     "    1  history | cat"
check    "< and > with a pipe"             "cat < s.txt | cat > out.txt"       ""                   "printf 'line one\nline two\n' > s.txt"
fact     "  output identical (cmp)"        "cmp -s s.txt out.txt"
check    "built-in output redirected"      "echo a\nhistory > h.txt\ncat h.txt" $'a\n    1  echo a\n    2  history > h.txt'
check    "ls > file"                       "ls > out.txt"                      ""                   "mkdir d; touch f"
fact     "  no colour codes in file"       '! grep -q "$(printf "\033")" out.txt && grep -qx "d/" out.txt'
check    "> on left of pipe (bash-like)"   "ls > x.txt | cat"                  ""                   "touch a"
fact     "  listing went to the file"      'grep -qx a x.txt'
check    "< on right of pipe (bash-like)"  "ls | cat < a.txt"                  "from a"             "echo 'from a' > a.txt"
check    "< missing on right of pipe"      "ls | cat < nofile"                 "myshell: nofile: No such file or directory"
contains "background pipeline output"      "ls | cat &\n/bin/sleep 0.3\necho x" "file1.txt"        "touch file1.txt"
contains "background pipeline Done"        "ls | cat &\n/bin/sleep 0.3\necho x" "[1] Done (status 0)    ls" "touch file1.txt"
check    "& alone"                         "&"                                 "myshell: syntax error near unexpected token \`&'"
check    "external expr 40 + 2"            "expr 40 + 2"                       "42"
check    "two pipes work"                  "ls | cat | cat"                    $'a\nb'              "touch a b"

echo; echo "== 18. Assignment rules =="
if grep -qE 'system\(|popen\(' "$SRC"; then bad "no system()/popen() in main.c" "grep" "none" "found"; else ok "no system()/popen() in main.c"; fi
if [ -z "$WARN" ]; then ok "compiles with -Wall and no warnings"; else bad "compiles with -Wall and no warnings" "gcc" "no output" "$WARN"; fi
for f in fork execvp waitpid pipe dup2 getcwd chdir opendir readdir closedir stat mkdir rmdir unlink rename open read write close utimensat getuid getpwuid uname; do
    if grep -q "\b$f(" "$SRC"; then ok "uses $f()"; else bad "uses $f()" "grep" "$f(" "missing"; fi
done

rm -rf "$BASE"
echo
echo "Passed: $PASS   Failed: $FAIL"
[ $FAIL -eq 0 ]
