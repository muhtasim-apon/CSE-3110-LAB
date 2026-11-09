#!/bin/bash
#
# build_and_test.sh - compile myshell and run the automated test cases
#
# Usage:
#   ./build_and_test.sh          compile, then run all tests
#   ./build_and_test.sh build    compile only
#   ./build_and_test.sh run      compile, then start the shell
#
# Every test starts myshell inside a fresh empty folder ($WORK). A file "decoy.txt" lies one
# level above it, in $BASE, and HOME is set to $BASE so that "cd" and "~" give known paths.

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
BASE=$(cd "$(mktemp -d)" && pwd -P)     # physical path, as getcwd() reports it
WORK="$BASE/work"
export HOME="$BASE"                     # a known home folder for cd and ~
export LC_ALL=C
umask 022                               # new files 644, new folders 755
PASS=0
FAIL=0
D="access denied (outside work)"          # the message of every blocked path

# fresh "bash setup": empty work folder, decoy above it, then run the setup inside it
fresh() {
    [ -d "$WORK" ] && chmod -R u+rwx "$WORK" 2>/dev/null     # permission tests leave locked folders
    rm -rf "$WORK"
    mkdir -p "$WORK"
    echo "secret" > "$BASE/decoy.txt"
    rm -rf "$BASE/out" "$BASE/x" "$BASE/x.txt" "$BASE/newfile" "$BASE/work2"
    [ -n "$1" ] && (cd "$WORK" && eval "$1")
}
# run "myshell input" (\n separates lines)
run() {
    (cd "$WORK" && printf '%b\n' "$1" | "$SHELL_BIN" 2>&1)
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
    fresh "$4"; (cd "$WORK" && printf '%b\n' "$2" | "$SHELL_BIN" >/dev/null 2>&1); st=$?
    if [ "$st" = "$3" ]; then ok "$1"; else bad "$1" "$2" "status $3" "status $st"; fi
}
# fact "name" "bash condition checked after the previous test"
# matches "name" "input" "extended regex one output line must match" ["setup"]
matches() {
    fresh "$4"; out=$(run "$2")
    if printf '%s\n' "$out" | grep -qE -- "$3"; then ok "$1"; else bad "$1" "$2" "(regex) $3" "$out"; fi
}
fact() {
    if (cd "$WORK" && eval "$2"); then ok "$1"; else bad "$1" "(after previous test)" "$2" "false"; fi
}

W="$WORK"
R=$(basename "$WORK")     # pwd and the prompt show the start folder by its name ("work/...")
echo "Running tests in $WORK"

echo; echo "== 1. pwd, cd and paths =="
check    "pwd shows the start folder name" "pwd"                               "$R"
check    "cd d1 and cd .. back"            "mkdir d1\ncd d1\npwd\ncd ..\npwd"  "$R/d1"$'\n'"$R"
check    "cd .. blocked at the start"      "cd ..\npwd"                        "cd: ..: $D"$'\n'"$R"
check    "cd ../.. blocked"                "cd ../..\npwd"                     "cd: ../..: $D"$'\n'"$R"
check    "sibling work2 is not reachable"  "mkdir ../work2"                    "mkdir: ../work2: $D"
check    "cd .. works below the start"     "mkdir a\ncd a\ncd ..\npwd"         "$R"
check    "cd a/b/../.."                    "mkdir -p a/b\ncd a/b/../..\npwd"   "$R"
check    "cd / blocked"                    "cd /\npwd"                         "cd: /: $D"$'\n'"$R"
check    "cd .. twice stays at the start"  "cd ..\ncd ..\npwd"                 "cd: ..: $D"$'\n'"cd: ..: $D"$'\n'"$R"
check    "cd /// blocked"                  "cd ///\npwd"                       "cd: ///: $D"$'\n'"$R"
check    "cd (no args) goes to the start"  "mkdir a\ncd a\ncd\npwd"            "$R"
check    "cd ~ blocked"                    "cd ~\npwd"                         "cd: ~: $D"$'\n'"$R"
check    "cd \$HOME blocked"               "cd \$HOME\npwd"                    "cd: $BASE: $D"$'\n'"$R"
check    "cd - returns and prints"         "mkdir a\ncd a\ncd ..\ncd -\npwd"   "$R/a"$'\n'"$R/a"
check    "cd absolute path"                "mkdir -p a/b\ncd $W/a/b\npwd"      "$R/a/b"
check    "cd missing absolute path"        "cd /no/such/dir"                   "cd: /no/such/dir: $D"
check    "\$PWD follows cd"                'mkdir a\ncd a\necho $PWD'          "$R/a"
check    "cd symlinked folder"             "cd lnk\npwd"                       "$R/real"           "mkdir real; ln -s real lnk"
check    "ls .. blocked"                   "ls .."                             "ls: ..: $D"
check    "ls -l /usr/bin blocked"          "ls -l /usr/bin"                    "ls: /usr/bin: $D"
contains "ls ~ blocked"                    "ls ~"                              "ls: ~: $D"
check    "cat ../file blocked"             "cat ../decoy.txt"                  "cat: ../decoy.txt: $D"
check    "cat absolute path blocked"       "cat $BASE/decoy.txt"               "cat: $BASE/decoy.txt: $D"
check    "cat ~/file blocked"              "cat ~/decoy.txt"                   "cat: ~/decoy.txt: $D"
check    "cat file symlink blocked"        "cat fl"                            "cat: fl: $D"       "ln -s $BASE/decoy.txt fl"
check    "cp ../file blocked"              "cp ../decoy.txt x.txt"             "cp: ../decoy.txt: $D"
check    "cp into the parent blocked"      "cp a.txt ../x.txt"                 "cp: ../x.txt: $D"  "echo hi > a.txt"
fact     "  no copy outside"               "[ ! -e $BASE/x.txt ]"
check    "mv into the parent blocked"      "mv a.txt ../x.txt"                 "mv: ../x.txt: $D"  "echo hi > a.txt"
fact     "  not moved"                     "[ -f a.txt ] && [ ! -e $BASE/x.txt ]"
check    "touch ../x blocked"              "touch ../x"                        "touch: ../x: $D"
fact     "  not created"                   "[ ! -e $BASE/x ]"
check    "> with ../ blocked"              "echo hi > ../out"                  "myshell: ../out: $D"
check    "external program with .. blocked" "wc -l ../decoy.txt"               "wc: ../decoy.txt: $D"

echo; echo "== 1b. Safety =="
check    "rm . refused"                    "rm ."                              "rm: refusing to remove '.' or '..' directory: skipping '.'"
check    "rm -r .. refused"                "rm -r .."                          "rm: refusing to remove '.' or '..' directory: skipping '..'"
fact     "  parent untouched"              "[ -f $BASE/decoy.txt ]"
check    "rm -r / blocked"                 "rm -r /"                           "rm: /: $D"
check    "rm -r // blocked"                "rm -r //"                          "rm: //: $D"
check    "mv / blocked"                    "mv / x"                            "mv: /: $D"
check    "rm folder that holds cwd refused" "mkdir a\ncd a\nrm -r $W/a"        "rm: cannot remove '$W/a': current directory is inside it"
check    "rm a symlink removes the link"   "rm esc\nls"                        ""                  "ln -s $BASE esc"
fact     "  its target is untouched"       "[ -f $BASE/decoy.txt ]"
check    "rm -r never follows a symlink"   "rm -r d\nls"                       ""                  "mkdir d; ln -s $BASE d/esc"
fact     "  linked folder untouched"       "[ -f $BASE/decoy.txt ]"
check    "mv a symlink moves the link"     "mv fl g\nls\ncat g"                $'g\n'"cat: g: $D"  "ln -s $BASE/decoy.txt fl"

echo; echo "== 2. pwd / cd errors =="
check    "pwd in nested dir"               "mkdir -p x/y\ncd x\ncd y\npwd"     "$R/x/y"
check    "cd missing dir"                  "cd nodir"                          "cd: nodir: No such file or directory"
check    "cd into a file"                  "cd f.txt"                          "cd: f.txt: Not a directory"  "touch f.txt"
check    "cd too many args"                "mkdir a b\ncd a b\npwd"            "cd: too many arguments"$'\n'"$R"
check    "cd \"\" stays in place"           'mkdir a\ncd a\ncd ""\npwd'          "$R/a"

echo; echo "== 3. ls =="
check    "ls empty dir"                    "ls"                                ""
check    "ls sorted, dirs marked /"        "ls"                                $'a.txt\nb.txt\nc/'  "touch b.txt a.txt; mkdir c"
check    "ls hides hidden files"           "ls"                                "v"                  "touch .hid v"
check    "ls -a shows . and .."            "ls -a"                             $'.\n..\n.hid'       "touch .hid"
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
contains "help lists cd"                   "help"                              "cd [dir|-|..]"
contains "help lists operators"            "help"                              "Operators:"
contains "help cd page"                    "help cd"                           "system calls: chdir()"
contains "help ls page"                    "help ls"                           "opendir(), readdir(), closedir(), stat()"
contains "help several pages"              "help cp mv"                        "system calls: rename()"
check    "help unknown topic"              "help nosuch\necho \$?"              $'help: no help topic for \'nosuch\'\n1'
contains "help in a pipe"                  "help ls | cat"                     "usage : ls"
check    "no banner or prompt when piped"  "echo ok"                           "ok"
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
fresh; start=$(date +%s%N); (cd "$WORK" && printf 'sleep 2 &\necho done\n' | "$SHELL_BIN" > "$BASE/bg.out" 2>&1); end=$(date +%s%N)
out=$(cat "$BASE/bg.out")
if [ $(( (end - start) / 1000000 )) -lt 1500 ] && echo "$out" | grep -q "^\[1\] [0-9]*$" && echo "$out" | grep -q "^done$"; then
    ok "background does not block"
else
    bad "background does not block" "sleep 2 &" "[1] <pid> and done in < 1.5 s" "$out ($(( (end - start) / 1000000 )) ms)"
fi
contains "background Done message"        "sleep 0.2 &\nsleep 0.5\necho x" "[1] Done (status 0)    sleep"
contains "background with redirect"       "echo bg > b.txt &\nsleep 0.3"  "Done (status 0)    echo"
fact     "  background job wrote file"     '[ "$(cat b.txt)" = bg ]'

echo; echo "== 17. Extra corner cases =="
check    "relative symlink .. : cd blocked" "cd up\npwd"                       "cd: up: $D"$'\n'"$R" "ln -s .. up"
check    "relative symlink .. : ls blocked" "ls up"                            "ls: up: $D"        "ln -s .. up"
check    "relative symlink .. : cat blocked" "cat up/decoy.txt"                "cat: up/decoy.txt: $D" "ln -s .. up"
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
contains "background pipeline output"      "ls | cat &\nsleep 0.3\necho x" "file1.txt"        "touch file1.txt"
contains "background pipeline Done"        "ls | cat &\nsleep 0.3\necho x" "[1] Done (status 0)    ls" "touch file1.txt"
check    "& alone"                         "&"                                 "myshell: syntax error near unexpected token \`&'"
check    "external expr 40 + 2"            "expr 40 + 2"                       "42"
check    "two pipes work"                  "ls | cat | cat"                    $'a\nb'              "touch a b"

echo; echo "== 18. Assignment rules =="
if grep -qE 'system\(|popen\(' "$SRC"; then bad "no system()/popen() in main.c" "grep" "none" "found"; else ok "no system()/popen() in main.c"; fi
for f in lstat readlink getpid setenv dup; do
    if grep -qE "(^|[^a-z_0-9])$f\(" "$SRC"; then bad "does not use $f()" "grep" "none" "found"; else ok "does not use $f()"; fi
done
if [ -z "$WARN" ]; then ok "compiles with -Wall and no warnings"; else bad "compiles with -Wall and no warnings" "gcc" "no output" "$WARN"; fi
for f in fork execvp waitpid pipe dup2 getcwd chdir opendir readdir closedir stat mkdir rmdir unlink rename open read write close utimensat getuid getpwuid uname realpath; do
    if grep -q "\b$f(" "$SRC"; then ok "uses $f()"; else bad "uses $f()" "grep" "$f(" "missing"; fi
done

echo; echo "== 19. Error handling: file access, exec, signals =="
if [ "$(id -u)" != 0 ]; then        # root ignores file permissions, so these only run as a normal user
check    "cat unreadable file"             "cat locked"                        "cat: locked: Permission denied"   "echo s > locked; chmod 000 locked"
check    "cp unreadable source"            "cp locked x"                       "cp: cannot open 'locked' for reading: Permission denied" "echo s > locked; chmod 000 locked"
check    "< unreadable file"               "cat < locked"                      "myshell: locked: Permission denied" "echo s > locked; chmod 000 locked"
check    "mkdir in read-only folder"       "mkdir ro/x"                        "mkdir: cannot create directory 'ro/x': Permission denied" "mkdir ro; chmod 555 ro"
check    "touch in read-only folder"       "touch ro/x"                        "touch: cannot touch 'ro/x': Permission denied" "mkdir ro; chmod 555 ro"
check    "rm in read-only folder"          "rm ro/f"                           "rm: cannot remove 'ro/f': Permission denied" "mkdir ro; touch ro/f; chmod 555 ro"
check    "> in read-only folder"           "echo hi > ro/f"                    "myshell: ro/f: Permission denied" "mkdir ro; chmod 555 ro"
check    "cd into folder without x"        "cd nx"                             "cd: nx: Permission denied"        "mkdir nx; chmod 000 nx"
check    "ls folder without r"             "ls nr"                             "ls: cannot open directory 'nr': Permission denied" "mkdir nr; chmod 000 nr"
else
    echo "(running as root: permission tests skipped)"
fi
check    "> to a directory"                "echo hi > d"                       "myshell: d: Is a directory"       "mkdir d"
check    "> into missing folder"           "echo hi > no/f"                    "myshell: no/f: No such file or directory"
check    "mv into missing folder"          "mv a no/b"                         "mv: cannot move 'a' to 'no/b': No such file or directory" "touch a"
check    "non-executable file gives 126"   "./s.sh\necho \$?"                  $'./s.sh: Permission denied\n126'  "echo 'echo hi' > s.sh; chmod 644 s.sh"
check    "executable script runs"          "./s.sh"                            "hi"                 "printf '#!/bin/sh\necho hi\n' > s.sh; chmod 755 s.sh"
check    "killed by signal gives 128+9"    'sh -c "kill -9 \$\$"\necho $?'     "137"
check    "exit status of external"         "false\necho \$?\ntrue\necho \$?"   $'1\n0'
status   "\$? is 2 after a syntax error"    "ls >\nexit"                        2

echo; echo "== 20. More edge cases =="
status   "exit -1 gives 255"               "exit -1"                           255
check    "exit in a pipe keeps the shell"  "exit | cat\necho alive"            "alive"
check    "builtin redirect restores stdout" "echo a > f\necho b\ncat f"        $'b\na'
check    "cd with a redirect"              "cd d > out.txt\npwd"               "$R/d"               "mkdir d"
check    "history 0 prints nothing"        "history 0"                         ""
check    "history -1 is an error"          "history -1"                        "history: -1: numeric argument required"
check    "!N with extra arguments"         "echo a\n!1 b"                      $'a\necho a b\na b'
H=$(for i in $(seq 105); do printf 'echo %s\\n' "$i"; done)
contains "history keeps numbering past 100" "${H}history 2"                   "  106  history 2"
contains "history forgets entries past 100" "${H}!1"                          "myshell: !1: event not found"
check    "operators inside quotes"         "echo \"a|b > c\" 'd & e'"          "a|b > c d & e"
check    "comment after a command"         "echo hi # not printed"             "hi"
check    "empty quoted argument"           'echo "" x'                         " x"
check    "echo -n alone"                   "echo -n\necho x"                   "x"
check    "uname -s -m (separate flags)"    "uname -s -m"                       "$(uname -s) $(uname -m)"
contains "uname table: node name"          "uname"                             "Node Name   : $(uname -n)"
contains "uname table: release"            "uname"                             "Release     : $(uname -r)"
matches  "uptime numbers"                  "uptime"                            '^Uptime       : [0-9]+ days, [0-9]+ hours, [0-9]+ minutes, [0-9]+ seconds$'
matches  "load average numbers"            "uptime"                            '^Load average : [0-9]+\.[0-9]{2} \(1 min\), [0-9]+\.[0-9]{2} \(5 min\), [0-9]+\.[0-9]{2} \(15 min\)$'
matches  "meminfo MemTotal = /proc/meminfo" "meminfo"                          "^MemTotal *: +$(awk '/^MemTotal:/{print $2}' /proc/meminfo) kB"
check    "mv several into a folder"        "mv a b dir\nls dir"                $'a\nb'              "touch a b; mkdir dir"
check    "mv several, target not a folder" "mv a b c"                          "mv: target 'c' is not a directory" "touch a b"
check    "mv overwrites"                   "mv n o\ncat o\nls"                 $'new\no'            "echo new > n; echo old > o"
check    "cat - between files"             "echo mid | cat a - b"              $'A\nmid\nB'         "echo A > a; echo B > b"
check    "touch makes mode 644"            "touch t"                           ""
fact     "  mode is 644"                   '[ "$(stat -c %a t)" = 644 ]'
check    ">> creates a missing file"       "echo a >> n.txt\ncat n.txt"        "a"
check    "> a > b: both made, output in b" "echo a > f1 > f2\ncat f2"          "a"
fact     "  f1 exists and is empty"        "[ -f f1 ] && [ ! -s f1 ]"
check    "< missing stops > from running"  "cat < nope > f3"                   "myshell: nope: No such file or directory"
fact     "  f3 not created"                "[ ! -e f3 ]"
P16="echo x$(printf ' | cat%.0s' {1..15})"
check    "16-command pipeline works"       "$P16"                              "x"
check    "17 commands: too many pipes"     "$P16 | cat"                        "myshell: too many pipes (max 16 commands)"
contains "two background jobs numbered"    "sleep 0.1 &\nfalse &\nsleep 0.4\necho x" "[2] Done (status 1)    false"

echo; echo "== 21. No system binaries: all 14 commands work with an empty PATH =="
fresh; out=$(cd "$WORK" && printf 'mkdir d\ntouch d/f\necho hi > d/f\ncp d/f d/g\nmv d/g d/h\ncat d/h\nls d\nrm d/f d/h\nrm -r d\npwd\nwhoami\nuname -s\nuptime\nmeminfo\necho ok\nwc\n' | PATH= "$SHELL_BIN" 2>&1)
exp=$(printf 'hi\nf\nh\n%s\n%s\n%s\n' "$R" "$(id -un)" "$(uname -s)")
if [ "$(printf '%s\n' "$out" | head -6)" = "$exp" ]; then ok "file commands, pwd, whoami, uname with PATH empty"; else bad "file commands, pwd, whoami, uname with PATH empty" "PATH= myshell" "$exp" "$out"; fi
if printf '%s' "$out" | grep -q "^Uptime" && printf '%s' "$out" | grep -q "^MemTotal"; then ok "uptime and meminfo with PATH empty"; else bad "uptime and meminfo with PATH empty" "PATH= myshell" "Uptime/MemTotal lines" "$out"; fi
if printf '%s\n' "$out" | tail -2 | tr '\n' '|' | grep -qx "ok|wc: command not found|"; then ok "a real program (wc) is not found: nothing else is run"; else bad "a real program (wc) is not found" "PATH= myshell" "ok, wc: command not found" "$out"; fi

echo; echo "== 22. Interactive: prompt, banner, Ctrl+C =="
if command -v script >/dev/null 2>&1; then
    fresh; out=$(cd "$WORK" && { printf 'cat nope\nexit\n'; sleep 0.5; } | script -qc "$SHELL_BIN" /dev/null 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    if printf '%s' "$out" | grep -q "Type help for commands"; then ok "banner on a terminal"; else bad "banner on a terminal" "(terminal)" "banner" "$out"; fi
    if printf '%s' "$out" | grep -qF "myshell> "; then ok "prompt is exactly myshell> "; else bad "prompt is exactly myshell> " "(terminal)" "myshell> " "$out"; fi
    if ! printf '%s' "$out" | grep -qE "myshell[^>]+> |$(id -un)@"; then ok "prompt has nothing else (no folder, no user@host)"; else bad "prompt has nothing else" "(terminal)" "only myshell> " "$out"; fi
    if printf '%s' "$out" | grep -q "cat: nope: No such file or directory"; then ok "loop keeps running after an error"; else bad "loop keeps running after an error" "(terminal)" "error then prompt" "$out"; fi
else
    echo "(script command not found: terminal tests skipped)"
fi
fresh; rm -f "$BASE/fifo"; mkfifo "$BASE/fifo"
(cd "$WORK" && exec "$SHELL_BIN" < "$BASE/fifo" > "$BASE/sig.out" 2>&1) & spid=$!
exec 3>"$BASE/fifo"
sleep 0.2; kill -INT "$spid"                             # Ctrl+C at the prompt
printf 'sleep 5\n' >&3; sleep 0.4; pkill -INT -P "$spid" # Ctrl+C while a command runs
printf 'echo st=$?\necho alive\nexit\n' >&3; exec 3>&-
wait "$spid"; out=$(cat "$BASE/sig.out")
if printf '%s' "$out" | grep -q "^alive$"; then ok "Ctrl+C does not kill the shell"; else bad "Ctrl+C does not kill the shell" "SIGINT" "alive" "$out"; fi
if printf '%s' "$out" | grep -q "^st=130$"; then ok "Ctrl+C stops the running command (status 130)"; else bad "Ctrl+C stops the running command" "SIGINT to sleep" "st=130" "$out"; fi

echo; echo "== 23. Jail: nothing outside the start folder can be reached =="
safe='[ "$(cat $BASE/decoy.txt)" = secret ]'
check    "pwd shows only the folder name"  "pwd"                               "$R"
check    "\$PWD shows only the folder name" "echo \$PWD"                       "$R"
check    "cd - never leaves the start"     "cd -"                              "$R"
check    "cd ./.. blocked"                 "cd ./.."                           "cd: ./..: $D"
check    "cd a/../.. blocked"              "cd a/../.."                        "cd: a/../..: $D"   "mkdir a"
check    "cd quoted .. blocked"            "cd '..'"                           "cd: ..: $D"
check    "cd symlink to / blocked"         "cd l"                              "cd: l: $D"         "ln -s / l"
check    "cd sibling with same prefix blocked" "cd ../work2"                   "cd: ../work2: $D"  "mkdir ../work2"
check    "cd symlink chain blocked"        "cd l2"                             "cd: l2: $D"        "ln -s .. l1; ln -s l1 l2"
check    "!! repeats the block"            "cd ..\n!!\npwd"                    "cd: ..: $D"$'\n'"cd .."$'\n'"cd: ..: $D"$'\n'"$R"
check    "symlink inside the start works"  "cat l"                             "in"                "echo in > a; ln -s a l"
check    "folder symlink inside works"     "cd l\npwd"                         "$R/a"              "mkdir a; ln -s a l"
check    "cat < ../file blocked"           "cat < ../decoy.txt"                "myshell: ../decoy.txt: $D"
check    "cat < symlink blocked"           "cat < fl"                          "myshell: fl: $D"   "ln -s ../decoy.txt fl"
check    "cat /etc/passwd blocked"         "cat /etc/passwd"                   "cat: /etc/passwd: $D"
check    "> through a symlink blocked"     "echo x > fl"                       "myshell: fl: $D"   "ln -s ../decoy.txt fl"
fact     "  outside file unchanged"        "$safe"
check    ">> through a symlink blocked"    "echo x >> fl"                      "myshell: fl: $D"   "ln -s ../decoy.txt fl"
fact     "  outside file unchanged"        "$safe"
check    "> to a dangling symlink blocked" "echo x > dg"                       "myshell: dg: $D"   "ln -s ../newfile dg"
fact     "  nothing created outside"       "[ ! -e $BASE/newfile ]"
check    "touch dangling symlink blocked"  "touch dg"                          "touch: dg: $D"     "ln -s ../newfile dg"
fact     "  nothing created outside"       "[ ! -e $BASE/newfile ]"
check    "cp onto a symlink blocked"       "cp a fl"                           "cp: fl: $D"        "echo x > a; ln -s ../decoy.txt fl"
fact     "  outside file unchanged"        "$safe"
contains "cp into a folder through a link blocked" "cp decoy.txt d"            "$D"                "echo x > decoy.txt; mkdir d; ln -s ../../decoy.txt d/decoy.txt"
fact     "  outside file unchanged"        "$safe"
check    "mkdir -p missing/../.. blocked"  "mkdir -p m/../../x"                "mkdir: m/../../x: $D"
fact     "  nothing created outside"       "[ ! -e $BASE/x ] && [ ! -e m ]"
check    "rm ../file blocked"              "rm ../decoy.txt"                   "rm: ../decoy.txt: $D"
fact     "  outside file kept"             "$safe"
check    "rm -r the start folder blocked"  "rm -r ../work"                     "rm: ../work: $D"   "touch keep"
fact     "  start folder kept"             "[ -f keep ]"
check    "rm through a symlink blocked"    "rm up/decoy.txt"                   "rm: up/decoy.txt: $D" "ln -s .. up"
fact     "  outside file kept"             "$safe"
check    "rm of a link keeps its target"   "rm fl\nls"                         ""                  "ln -s ../decoy.txt fl"
fact     "  outside file kept"             "$safe"
check    "mv ../file blocked"              "mv ../decoy.txt x"                 "mv: ../decoy.txt: $D"
fact     "  outside file kept"             "$safe"
check    "mv the start folder blocked"     "mv ../work x"                       "mv: ../work: $D"
check    "/bin/ls blocked"                 "/bin/ls"                           "/bin/ls: /bin/ls: $D"
check    "/bin/pwd blocked"                "/bin/pwd"                          "/bin/pwd: /bin/pwd: $D"
check    "external with symlink blocked"   "wc fl"                             "wc: fl: $D"        "ln -s ../decoy.txt fl"
check    "external with ~ blocked"         "head ~/decoy.txt"                  "head: ~/decoy.txt: $D"
if [ "$(cat /proc/sys/kernel/osrelease >/dev/null; printf '#include<unistd.h>\nint main(){return syscall(444,0,0,1)<1;}' | gcc -xc - -o "$BASE/ll" 2>/dev/null && "$BASE/ll" && echo y)" = y ]; then
contains "sh -c cat ../file: kernel blocks" "sh -c 'cat ../decoy.txt'"         "Permission denied"
contains "sh -c cd ..: kernel blocks"      "sh -c 'cd ..; cat decoy.txt'"      "Permission denied"
contains "sh -c ls /: kernel blocks"       "sh -c 'ls /'"                      "Permission denied"
contains "sh -c /etc/passwd: kernel blocks" "sh -c 'cat /etc/passwd'"          "Permission denied"
contains "sh -c write outside: kernel blocks" "sh -c 'cd ..; echo x > newfile'" "Permission denied"
fact     "  nothing created outside"       "[ ! -e $BASE/newfile ]"
contains "sh -c rm outside: kernel blocks" "sh -c 'cd ..; rm decoy.txt'"       "ermi"
fact     "  outside file kept"             "$safe"
contains "script file: kernel blocks"      "./s.sh"                            "Permission denied" "printf '#!/bin/sh\ncat ../decoy.txt\n' > s.sh; chmod +x s.sh"
check    "programs work inside the sandbox" "sh -c 'cat a | tr a-z A-Z'"       "IN"                "echo in > a"
check    "gcc works inside the sandbox"    "gcc -o t t.c\n./t\necho \$?"       "7"                 "echo 'int main(){return 7;}' > t.c"
else
    echo "(no Landlock in this kernel: kernel sandbox tests skipped)"
fi

[ -d "$WORK" ] && chmod -R u+rwx "$WORK" 2>/dev/null
rm -rf "$BASE"
echo
echo "Passed: $PASS   Failed: $FAIL"
[ $FAIL -eq 0 ]
