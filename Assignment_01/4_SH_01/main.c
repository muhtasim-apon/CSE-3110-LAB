/* myshell - a custom UNIX-like shell (CSE-3110 OS Lab, Assignment 1)
 * Every command is written with the POSIX system calls named in the assignment. */
#define _GNU_SOURCE                 //turns on O_PATH and the other GNU extras; must come before every #include
#include<stdio.h>                   //printf, fprintf, fgets, perror, snprintf
#include<stdlib.h>                  //malloc, realloc, free, exit, getenv, strtol, qsort
#include<string.h>                  //strcmp, strlen, strchr, strerror, strdup, memset
#include<ctype.h>                   //isalpha, isalnum, isdigit for variable names and !N
#include<errno.h>                   //errno and error codes (ENOENT, EISDIR, EINTR ...)
#include<limits.h>                  //PATH_MAX, the longest path we store
#include<signal.h>                  //sigaction, signal, SIGINT for Ctrl+C handling
#include<time.h>                    //localtime, strftime for the ls -l date
#include<unistd.h>                  //fork, execvp, pipe, dup2, read, write, close, chdir, getcwd, unlink, rmdir
#include<fcntl.h>                   //open flags O_RDONLY, O_CREAT, O_TRUNC, O_APPEND, AT_FDCWD
#include<dirent.h>                  //opendir, readdir, closedir for ls and rm -r
#include<pwd.h>                     //getpwuid: user name from a user id
#include<sys/types.h>               //pid_t, mode_t, ssize_t types
#include<sys/stat.h>                //stat, mkdir, utimensat, S_ISDIR
#include<sys/wait.h>                //waitpid and WIFEXITED / WEXITSTATUS macros
#include<sys/utsname.h>             //uname and struct utsname
#include<sys/prctl.h>               //prctl for PR_SET_NO_NEW_PRIVS, needed before the sandbox starts
#include<sys/syscall.h>             //syscall(): the Landlock calls have no C library wrapper
#include<stdint.h>                  //uint64_t, int32_t: exact sizes the Landlock structures need
#define MAX_LINE 1024               //longest input line (including newline and '\0')
#define MAX_ARGS 64                 //most words in one command (+1 slot for the NULL end)
#define MAX_CMDS 16                 //most commands joined by pipes in one line
#define MAX_TOKENS 256              //most tokens (words + operators) in one line
#define MAX_JOBS 64                 //most background jobs remembered at once
#define BUF_SIZE 4096               //block size for read()/write() copying
#define HIST_SIZE 100               //how many history entries are kept in memory
#define MAX_REDIR 16                //most < > >> redirections on one command
#define WORD_BUF 16384              //space for all words of one parsed line
#define SAVED_IN 100        //spare fds that keep stdin/stdout while a builtin is redirected
#define SAVED_OUT 101               //spare fd for the saved stdout
#define C_RESET "\033[0m"   //prompt colours, used only when stdout is a terminal
#define C_BOLD  "\033[1m"           //bold text
#define C_RED   "\033[1;31m"        //bold red: "$" after a failed command
#define C_CYAN  "\033[1;36m"        //bold cyan: shell name and banner
#define LL_CREATE 444               //system call number of landlock_create_ruleset
#define LL_ADD 445                  //system call number of landlock_add_rule
#define LL_RESTRICT 446             //system call number of landlock_restrict_self
#define LL_EXEC 1ULL                //right: run files
#define LL_WRITE 2ULL               //right: write files
#define LL_READ 4ULL                //right: read files
#define LL_ALL_V1 ((1ULL<<13)-1)    //all 13 rights of Landlock version 1
#define LL_REFER (1ULL<<13)         //version 2 right: link or move a file to another folder
#define LL_TRUNCATE (1ULL<<14)      //version 3 right: cut a file to a new size

char prevdir[PATH_MAX];     //previous directory, for "cd -"
char start_dir[PATH_MAX];           //folder where myshell was started: the root of the jail (shown by its name, e.g. 4_SH_01)
size_t start_len=0;                 //length of start_dir (0 = started in "/", nothing to shorten)
int last_status=0;          //exit status of the last command, for $? and exit
char *history[HIST_SIZE];           //ring buffer of the last commands typed
int hist_total=0;                   //how many commands were ever added (gives history numbers)
struct {pid_t pid;int num;char name[64];} jobs[MAX_JOBS];   //running background jobs: pid, job number, command name
int job_count=0;                    //last job number handed out ([1], [2], ...)
volatile sig_atomic_t at_prompt=0;  //1 while the shell waits for input (Ctrl+C then moves to a new line)
struct ll_attr {uint64_t handled;}; //Landlock ruleset: the rights it controls
struct ll_rule {uint64_t allowed;int32_t fd;} __attribute__((packed));   //Landlock rule: rights allowed below the folder open on fd

/*
 * ========================== OVERALL EXECUTION ORDER ==========================
 *
 * 1. STARTUP (main)
 *    - getcwd() saves the start folder. It becomes the root of the shell's jail.
 *    - TMPDIR points at the start folder, because /tmp is blocked for programs.
 *    - sigaction() installs on_sigint, so Ctrl+C cannot kill the shell.
 *    - On a terminal, the banner is printed.
 *
 * 2. READ-EXECUTE LOOP (main repeats these steps until exit or Ctrl+D)
 *
 *    reap_jobs()        report background jobs that have finished
 *        |
 *    print the prompt   "myshell> ", only on a terminal
 *        |
 *    fgets()            read one line; Ctrl+D leaves the loop
 *        |
 *    hist_expand()      replace !! and !N with the saved command
 *        |
 *    hist_add()         save the line in the history ring
 *        |
 *    tokenize()         line -> tokens (words and | < > >> &)
 *        |              expands quotes, \x, $VAR, ${VAR} and $? (expand_var)
 *    parse()            tokens -> Cmd array, one Cmd per piped command
 *        |
 *    execute()          run the commands and wait for them
 *        |
 *    last_status        kept for $? and for the prompt colour
 *
 * 3. INSIDE execute()
 *
 *    A. Single builtin (cd, echo, exit, help, history) without '&'
 *       save stdin/stdout -> redirect() -> run the function in the shell itself
 *       -> restore stdin/stdout.
 *       It must run in the shell: a child's "cd" would not change the shell.
 *
 *    B. Everything else
 *       for each command:
 *           pipe()   a pipe to the next command (not after the last one)
 *           fork()   a child process
 *           child:   connect the pipes with dup2(), then child_run()
 *           parent:  close the pipe ends it does not need
 *       then:
 *           background ('&'): save the job, print [n] pid, return at once
 *           foreground:       waitpid() for every child;
 *                             the last command decides the exit status
 *
 * 4. INSIDE child_run() (the child process, it never returns)
 *
 *    find_command()       look the name up in the commands[] table
 *        |
 *    redirect()           apply < > >> with open() and dup2()
 *        |
 *        +-- found:       run our own xxx_call() function, then _exit(status)
 *        |
 *        +-- not found:   external program
 *                         expand_path() checks every argument
 *                         jail_program() starts the Landlock sandbox
 *                         execvp() replaces the child with the program
 *                         a failed execvp() gives status 127 or 126
 *
 * 5. THE JAIL (nothing outside the start folder can be reached)
 *
 *    Our own commands: every path goes through
 *        expand_path() or expand_link() -> expand_jail() -> expand_home()
 *        + jail_ok() -> realpath() -> below_root()
 *    External programs: the same argument check, then the kernel sandbox
 *        jail_program() -> ll_allow(), which also stops paths the program
 *        opens by itself (for example: sh -c "cat /etc/passwd").
 *
 * 6. EXAMPLE: cat a.txt | wc -l > b.txt
 *
 *    tokenize  WORD(cat) WORD(a.txt) PIPE WORD(wc) WORD(-l) OUT WORD(b.txt)
 *    parse     cmd[0] = cat a.txt
 *              cmd[1] = wc -l, with the redirection > b.txt
 *    execute   pipe(), then two children:
 *      child 0 stdout = pipe write end -> child_run -> cat_call -> _exit
 *      child 1 stdin  = pipe read end  -> child_run -> redirect (stdout = b.txt)
 *              -> argument check -> jail_program -> execvp("wc")
 *    parent    closes both pipe ends, waits for both children;
 *              the status of wc becomes last_status.
 *
 * 7. SHUTDOWN
 *    exit_call() ends the process directly. At end of input (Ctrl+D) main leaves
 *    the loop and returns last_status.
 */

/* ============ Helpers ============ */

int write_all(int fd,const char *buf,size_t n)  //write all n bytes, even if write() does it in parts
{                                   //start of write_all
    ssize_t w;                      //bytes written by one write() call
    while(n>0)                      //repeat until every byte is out
    {                               //start of loop body
        w=write(fd,buf,n);          //system call: write the remaining bytes
        if(w<0&&errno==EINTR)continue;  //interrupted by a signal: just try again
        if(w<=0)return -1;          //real error: report failure
        buf+=w;                     //skip the bytes already written
        n-=w;                       //fewer bytes left
    }                               //end of loop
    return 0;                       //everything written
}                                   //end of write_all
const char *base_name(const char *path)     //last part of a path ("a/b/c.txt" -> "c.txt")
{                                   //start of base_name
    const char *p=strrchr(path,'/');    //find the last '/'
    return p?p+1:path;              //text after it, or the whole path if there is no '/'
}                                   //end of base_name
//path shown to the user: inside the start folder it is written from the folder's name
//("/home/u/x/4_SH_01/demo" -> "4_SH_01/demo"); anywhere else the full path is shown
const char *show_path(const char *real)   //real path -> text shown to the user
{                                   //start of show_path
    static char shown[PATH_MAX];    //static so the text stays valid after return
    if(start_len==0||strncmp(real,start_dir,start_len)!=0)return real;     //not below the start folder: full path
    if(real[start_len]!='\0'&&real[start_len]!='/')return real;   //only a name prefix ("4_SH_01x"): full path
    snprintf(shown,sizeof(shown),"%s%s",base_name(start_dir),real+start_len);  //folder name + the rest
    return shown;                   //e.g. "4_SH_01" or "4_SH_01/demo"
}                                   //end of show_path
//"~" and "~/x" become the home folder ($HOME); any other path is passed on unchanged
//returns 0 ok, -1 error (message printed for command cmd)
int expand_home(const char *cmd,const char *path,char *out)   //expand "~", copy path into out (no jail check here)
{                                   //start of expand_path
    const char *home=getenv("HOME");    //home folder from the environment
    if(home==NULL||home[0]=='\0')home="/";  //no HOME set: use "/" instead
    if(strcmp(path,"~")==0)path=home;   //"~" alone means the home folder
    else if(strncmp(path,"~/",2)==0)    //"~/x" means x inside the home folder
    {                               //start of "~/x" case
        if(strlen(home)+strlen(path)>=PATH_MAX)     //result would not fit
        {                           //start of error case
            fprintf(stderr,"%s: %s: %s\n",cmd,path,strerror(ENAMETOOLONG));  //"cmd: path: File name too long"
            return -1;              //tell the caller it failed
        }                           //end of error case
        snprintf(out,PATH_MAX,"%s%s",home,path+1);  //home + "/x" (skip the '~')
        return 0;                   //done
    }                               //end of "~/x" case
    if(strlen(path)>=PATH_MAX)      //path too long to copy
    {                               //start of error case
        fprintf(stderr,"%s: %s: %s\n",cmd,path,strerror(ENAMETOOLONG));  //report it
        return -1;                  //failure
    }                               //end of error case
    snprintf(out,PATH_MAX,"%s",path);   //any other path is copied unchanged; the kernel resolves it
    return 0;                       //success
}                                   //end of expand_home
//the jail test: real must already be resolved by realpath(); returns 1 inside, 0 outside
int below_root(const char *real)    //is this resolved path the start folder or inside it?
{                                   //start of below_root
    if(start_len==0)return 1;       //started in "/": everything is inside
    if(strncmp(real,start_dir,start_len)!=0)return 0;   //does not start with the start folder: outside
    return real[start_len]=='\0'||real[start_len]=='/';   //full name match only ("4_SH_01x" is outside)
}                                   //end of below_root
//resolve the path (realpath follows "..", "." and links) and test it with below_root()
//a path that does not exist yet is judged by its deepest parent that does exist
//follow=0: the last name itself is not followed (rm and mv act on a link, not its target)
int jail_ok(const char *path,int follow)   //may this path be used? 1 yes, 0 no
{                                   //start of jail_ok
    char cur[PATH_MAX],res[PATH_MAX],*cut;   //path being tested, resolved path, position of the last '/'
    const char *name;               //last name of cur
    size_t len;                     //length of cur
    int fd;                         //fd for testing a broken link
    snprintf(cur,sizeof(cur),"%s",path);   //work on a copy, it is cut below
    while(1)                        //walk up the path until one part can be resolved
    {                               //start of walk loop
        if(follow)                  //resolve this part (skipped once for follow=0)
        {                           //start of resolve case
            if(realpath(cur,res)!=NULL)return below_root(res);   //it exists: the resolved path gives the answer
            fd=open(cur,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);   //why did it fail? open it without following a link
            if(fd<0&&errno==ELOOP)return 0;   //a broken link: its target could be anywhere, refuse
            if(fd>=0)close(fd);     //only a test, close it again
        }                           //end of resolve case
        follow=1;                   //every parent folder is always followed
        len=strlen(cur);            //length before cutting
        while(len>1&&cur[len-1]=='/')cur[--len]='\0';   //drop trailing '/' ("a/b/" is "a/b")
        if(strcmp(cur,".")==0)return 0;   //even the current folder cannot be resolved: refuse
        cut=strrchr(cur,'/');       //find the last '/'
        name=cut?cut+1:cur;         //the last name of the path
        if(strcmp(name,"..")==0)return 0;   //an unresolved "..": cannot be judged, refuse
        if(cut==NULL)strcpy(cur,".");   //plain name: its parent is the current folder
        else if(cut==cur)cur[1]='\0';   //"/x": its parent is the root
        else *cut='\0';             //"a/b": its parent is "a"
    }                               //end of walk loop
}                                   //end of jail_ok
//expand "~", then refuse anything outside the start folder; returns 0 ok, -1 refused
int expand_jail(const char *cmd,const char *path,char *out,int follow)   //expand "~", then check the jail
{                                   //start of expand_jail
    if(expand_home(cmd,path,out)!=0)return -1;   //handle "~"; a too long path already printed its error
    if(jail_ok(out,follow))return 0;   //inside the jail: path is ready
    fprintf(stderr,"%s: %s: access denied (outside %s)\n",cmd,path,base_name(start_dir));   //report the refused path
    return -1;                      //failure
}                                   //end of expand_jail
//the check used by nearly every command: the path and all its links must stay inside
int expand_path(const char *cmd,const char *path,char *out)   //normal path: links are followed
{                                   //start of expand_path
    return expand_jail(cmd,path,out,1);   //follow=1
}                                   //end of expand_path
//used by rm and mv: they work on the link itself, so a link to the outside may be removed
int expand_link(const char *cmd,const char *path,char *out)   //path of a link: the last name is not followed
{                                   //start of expand_link
    return expand_jail(cmd,path,out,0);   //follow=0
}                                   //end of expand_link
//ask the kernel which Landlock version it has (flag 1 = return the version)
int landlock_version(void)          //Landlock version of this kernel; below 1 = no Landlock
{                                   //start of landlock_version
    return (int)syscall(LL_CREATE,NULL,0,1);   //system call: landlock_create_ruleset
}                                   //end of landlock_version
//add one rule to the ruleset; a path missing on this system (e.g. /lib64) is skipped
void ll_allow(int rules,const char *path,uint64_t rights)   //allow rights below path
{                                   //start of ll_allow
    struct ll_rule rule;            //the rule given to the kernel
    int fd=open(path,O_PATH|O_CLOEXEC);   //system call: O_PATH only names the folder for the rule
    if(fd<0)return;                 //no such path: nothing to allow
    rule.allowed=rights;            //what may be done there
    rule.fd=fd;                     //the folder or file it applies to
    if(syscall(LL_ADD,rules,1,&rule,0)!=0)perror("myshell: landlock rule");   //system call: landlock_add_rule (1 = rule for a path)
    close(fd);                      //the kernel keeps its own reference
}                                   //end of ll_allow
//kernel sandbox for external programs: the argument check cannot see the paths a program
//opens by itself (sh -c "cat /etc/passwd"), so here the kernel refuses them
//the start folder gets every right; system folders can only be read and run
int jail_program(void)              //put this child in the sandbox; 0 ok, -1 error
{                                   //start of jail_program
    int ver=landlock_version(),rules;   //Landlock version, fd of the ruleset
    uint64_t all=LL_ALL_V1;         //every right this kernel knows
    uint64_t dev=LL_READ|LL_WRITE;  //rights for the /dev files
    struct ll_attr attr;            //describes the ruleset
    if(ver<1)return 0;              //no Landlock: only the argument check protects us
    if(ver>=2)all|=LL_REFER;        //version 2 knows this right too
    if(ver>=3){all|=LL_TRUNCATE;dev|=LL_TRUNCATE;}   //version 3 also controls truncating, /dev needs it for "> /dev/null"
    attr.handled=all;               //everything not allowed by a rule is refused
    rules=(int)syscall(LL_CREATE,&attr,sizeof(attr),0);   //system call: landlock_create_ruleset
    if(rules<0)return -1;           //could not create it
    ll_allow(rules,start_dir,all);  //inside the start folder everything is allowed
    ll_allow(rules,"/usr",LL_READ|LL_EXEC);   //programs and libraries: read and run only
    ll_allow(rules,"/bin",LL_READ|LL_EXEC);   //programs
    ll_allow(rules,"/lib",LL_READ|LL_EXEC);   //shared libraries
    ll_allow(rules,"/lib64",LL_READ|LL_EXEC);   //shared libraries and the program loader
    ll_allow(rules,"/etc/ld.so.cache",LL_READ);   //the loader finds libraries through this file
    ll_allow(rules,"/dev/null",dev);   //discards output
    ll_allow(rules,"/dev/zero",dev);   //endless zero bytes
    ll_allow(rules,"/dev/urandom",dev);   //random bytes
    ll_allow(rules,"/dev/tty",dev); //the terminal
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)!=0)return -1;   //required: without it an unprivileged process may not start the sandbox
    if(syscall(LL_RESTRICT,rules,0)!=0)return -1;   //system call: landlock_restrict_self, the sandbox is now on
    close(rules);                   //the sandbox stays on without this fd
    return 0;                       //program can now be started
}                                   //end of jail_program

/* ============ 1. File & Directory Inspection Commands ============ */

//pwd: getcwd()
int pwd_call(int argc,char *argv[])   //pwd command
{                                   //start of pwd
    char cwd[PATH_MAX];             //buffer for the current folder
    if(getcwd(cwd,sizeof(cwd))==NULL)   //system call: ask the kernel for the current folder
    {                               //start of error case
        perror("pwd");              //print why it failed
        return 1;                   //exit status 1
    }                               //end of error case
    printf("%s\n",show_path(cwd));  //print it (start folder shown by its name)
    return 0;                       //success
}                                   //end of pwd
int cmp_names(const void *a,const void *b)  //compare two names for qsort (alphabetical order)
{                                   //start of cmp_names
    return strcmp(*(char *const *)a,*(char *const *)b);    //each item is a char*, compare the strings
}                                   //end of cmp_names
//print one ls line; path is the real path, name is what the user sees
void ls_entry(const char *path,const char *name,int lflag)   //print one entry of ls
{                                   //start of ls_entry
    struct stat st;                 //file information from stat()
    struct passwd *pw;              //owner information from getpwuid()
    char mode[11],tbuf[32];         //"drwxr-xr-x" text and the date text
    const char *rwx="rwxrwxrwx";    //letters for the 9 permission bits
    int i,special=strcmp(name,".")==0||strcmp(name,"..")==0;   //"." and ".." get no trailing '/'
    if(!lflag)                      //short format: only the name
    {                               //start of short format
        if(!special&&stat(path,&st)==0&&S_ISDIR(st.st_mode))printf("%s/\n",name);    //folder: add '/'
        else printf("%s\n",name);   //file: name only
        return;                     //done with this entry
    }                               //end of short format
    if(stat(path,&st)!=0)           //system call: read size, mode, owner, time
    {                               //start of error case
        fprintf(stderr,"ls: cannot access '%s': %s\n",name,strerror(errno));  //report it
        return;                     //skip this entry
    }                               //end of error case
    mode[0]=S_ISDIR(st.st_mode)?'d':'-';    //first letter: d for a folder
    for(i=0;i<9;i++)mode[i+1]=(st.st_mode&(0400>>i))?rwx[i]:'-';    //test each permission bit from 0400 down
    mode[10]='\0';                  //end the mode string
    pw=getpwuid(st.st_uid);         //owner id -> owner name
    strftime(tbuf,sizeof(tbuf),"%b %e %H:%M",localtime(&st.st_mtime));    //modification time as "Oct 17 19:45"
    printf("%s %2ld %-8s %8lld %s %s",mode,(long)st.st_nlink,pw?pw->pw_name:"?",(long long)st.st_size,tbuf,name);   //mode, links, owner, size, date, name
    if(S_ISDIR(st.st_mode)&&!special)printf("/");  //folders end with '/'
    printf("\n");                   //end of the line
}                                   //end of ls_entry
//ls [-a] [-l] [path...]: opendir(), readdir(), closedir(), stat()
int ls_call(int argc,char *argv[])   //ls command
{                                   //start of ls
    char *targets[MAX_ARGS],real[PATH_MAX],path[PATH_MAX+512],**names,*p;  //paths to list, full entry path, sorted names
    int i,j,n,cap,aflag=0,lflag=0,ntarget=0,ret=0;     //counters, option flags, exit status
    DIR *dir;                       //open folder handle
    struct dirent *e;               //one folder entry from readdir()
    struct stat st;                 //information about each target
    for(i=1;i<argc;i++)             //look at every argument
    {                               //start of argument loop
        if(argv[i][0]=='-'&&argv[i][1]!='\0')  //it is an option like -l or -la
        {                           //start of option case
            for(p=argv[i]+1;*p;p++) //each letter after the '-'
            {                       //start of letter loop
                if(*p=='a')aflag=1; //-a: show hidden files
                else if(*p=='l')lflag=1;    //-l: long format
                else                //any other letter is not supported
                {                   //start of error case
                    fprintf(stderr,"ls: invalid option -- '%c'\n",*p);    //report it
                    return 2;       //status 2 like GNU ls
                }                   //end of error case
            }                       //end of letter loop
        }                           //end of option case
        else targets[ntarget++]=argv[i];    //not an option: a path to list
    }                               //end of argument loop
    if(ntarget==0)targets[ntarget++]=".";   //no path given: list the current folder
    for(i=0;i<ntarget;i++)          //list each target
    {                               //start of target loop
        if(expand_path("ls",targets[i],real)!=0){ret=1;continue;}  //handle "~", jail check; skip on error
        if(stat(real,&st)!=0)       //system call: does it exist, and what is it?
        {                           //start of error case
            fprintf(stderr,"ls: cannot access '%s': %s\n",targets[i],strerror(errno));    //e.g. No such file
            ret=1;                  //remember the failure
            continue;               //go to the next target
        }                           //end of error case
        if(!S_ISDIR(st.st_mode))    //a file: print just that one entry
        {                           //start of file case
            ls_entry(real,targets[i],lflag);    //print it
            continue;               //next target
        }                           //end of file case
        dir=opendir(real);          //system call: open the folder for reading
        if(dir==NULL)               //could not open it (e.g. no permission)
        {                           //start of error case
            fprintf(stderr,"ls: cannot open directory '%s': %s\n",targets[i],strerror(errno));   //report it
            ret=1;                  //remember the failure
            continue;               //next target
        }                           //end of error case
        if(ntarget>1)printf("%s%s:\n",i?"\n":"",targets[i]);  //several folders: print a "name:" header
        n=0;                        //no names collected yet
        cap=64;                     //start with room for 64 names
        names=malloc(cap*sizeof(char *));   //array of name pointers
        while((e=readdir(dir))!=NULL)   //system call: read entries one by one
        {                           //start of readdir loop
            if(e->d_name[0]=='.'&&!aflag)continue;      //hidden files only with -a
            if(n==cap)              //array is full
            {                       //start of grow case
                cap*=2;             //double the room
                names=realloc(names,cap*sizeof(char *));    //grow the array
            }                       //end of grow case
            names[n++]=strdup(e->d_name);   //keep a copy of the name
        }                           //end of readdir loop
        closedir(dir);              //system call: close the folder
        qsort(names,n,sizeof(char *),cmp_names);    //sort names alphabetically
        for(j=0;j<n;j++)            //print each name
        {                           //start of print loop
            snprintf(path,sizeof(path),"%s/%s",real,names[j]);    //full path, needed for stat()
            ls_entry(path,names[j],lflag);  //print one line
            free(names[j]);         //free the copied name
        }                           //end of print loop
        free(names);                //free the array
    }                               //end of target loop
    return ret;                     //0 if all went well, else 1
}                                   //end of ls
//whoami: getuid(), getpwuid()
int whoami_call(int argc,char *argv[])   //whoami command
{                                   //start of whoami
    struct passwd *pw=getpwuid(getuid());   //system calls: my user id -> password entry
    if(pw==NULL)                    //no entry found
    {                               //start of error case
        perror("whoami");           //report it
        return 1;                   //failure
    }                               //end of error case
    printf("%s\n",pw->pw_name);     //print the user name
    return 0;                       //success
}                                   //end of whoami
//uname [-a -s -n -r -v -m]: uname()
int uname_call(int argc,char *argv[])   //uname command
{                                   //start of uname
    struct utsname u;               //filled by the uname() system call
    int i,show[5]={0},first=1;      //which fields to print; first avoids a leading space
    char *p,*opts="snrvm",*q;       //option letters in field order
    char *field[5];                 //pointers to the 5 fields
    if(uname(&u)!=0)                //system call: get system information
    {                               //start of error case
        perror("uname");            //report it
        return 1;                   //failure
    }                               //end of error case
    if(argc==1)                     //no options: print a labelled table
    {                               //start of table case
        printf("System Name : %s\n",u.sysname);    //e.g. Linux
        printf("Node Name   : %s\n",u.nodename);   //host name
        printf("Release     : %s\n",u.release);    //kernel release
        printf("Version     : %s\n",u.version);    //kernel build version
        printf("Machine     : %s\n",u.machine);    //architecture, e.g. x86_64
        return 0;                   //done
    }                               //end of table case
    for(i=1;i<argc;i++)             //read the options
    {                               //start of option loop
        if(argv[i][0]!='-'||argv[i][1]=='\0')  //not an option: uname takes no operands
        {                           //start of error case
            fprintf(stderr,"uname: extra operand '%s'\n",argv[i]);   //report it
            return 1;               //failure
        }                           //end of error case
        for(p=argv[i]+1;*p;p++)     //each letter of the option
        {                           //start of letter loop
            if(*p=='a')show[0]=show[1]=show[2]=show[3]=show[4]=1;  //-a: all fields
            else if((q=strchr(opts,*p))!=NULL)show[q-opts]=1;      //-s -n -r -v -m: mark that field
            else                    //unknown letter
            {                       //start of error case
                fprintf(stderr,"uname: invalid option -- '%c'\n",*p);    //report it
                return 1;           //failure
            }                       //end of error case
        }                           //end of letter loop
    }                               //end of option loop
    field[0]=u.sysname;field[1]=u.nodename;field[2]=u.release;field[3]=u.version;field[4]=u.machine;  //fields in -s -n -r -v -m order
    for(i=0;i<5;i++)                //print the chosen fields
    {                               //start of print loop
        if(!show[i])continue;       //not asked for
        printf("%s%s",first?"":" ",field[i]);  //space between fields
        first=0;                    //later fields need a space
    }                               //end of print loop
    printf("\n");                   //end of the line
    return 0;                       //success
}                                   //end of uname

/* ============ 2. File & Directory Manipulation Commands ============ */

//mkdir -p: create every missing folder on the way
int mkdir_parents(char *real)   //helper for mkdir -p
{                                   //start of mkdir_parents
    char *p;                        //walks along the path
    struct stat st;                 //used to check an existing folder
    for(p=real+1;*p;p++)            //look at each character (skip a leading '/')
    {                               //start of path loop
        if(*p!='/')continue;        //only stop at each '/'
        *p='\0';                    //cut the path here: "a/b/c" -> "a"
        if(mkdir(real,0755)!=0&&errno!=EEXIST){*p='/';return -1;}    //create this part; existing is fine
        *p='/';                     //put the '/' back
    }                               //end of path loop
    if(mkdir(real,0755)!=0)         //create the last folder
    {                               //start of error case
        if(errno==EEXIST&&stat(real,&st)==0&&S_ISDIR(st.st_mode))return 0;    //already a folder: fine with -p
        return -1;                  //real failure (or a file with that name)
    }                               //end of error case
    return 0;                       //success
}                                   //end of mkdir_parents
//mkdir [-p] dir...: mkdir() with mode 0755
int mkdir_call(int argc,char *argv[])   //mkdir command
{                                   //start of mkdir
    char real[PATH_MAX];            //path after "~" expansion
    int i,pflag=0,count=0,ret=0;    //-p flag, number of folders, exit status
    for(i=1;i<argc;i++)             //first pass: read options and count folders
    {                               //start of option loop
        if(strcmp(argv[i],"-p")==0)pflag=1;    //-p: make parents too
        else if(argv[i][0]=='-'&&argv[i][1]!='\0')     //any other option is invalid
        {                           //start of error case
            fprintf(stderr,"mkdir: invalid option '%s'\n",argv[i]);    //report it
            return 1;               //failure
        }                           //end of error case
        else count++;               //a folder name
    }                               //end of option loop
    if(count==0)                    //no folder given
    {                               //start of error case
        fprintf(stderr,"usage: mkdir [-p] <directory> ...\n");     //show the usage
        return 1;                   //failure
    }                               //end of error case
    for(i=1;i<argc;i++)             //second pass: create each folder
    {                               //start of create loop
        if(strcmp(argv[i],"-p")==0)continue;   //skip the option itself
        if(expand_path("mkdir",argv[i],real)!=0){ret=1;continue;}  //handle "~", jail check
        if((pflag?mkdir_parents(real):mkdir(real,0755))!=0)  //system call mkdir() with mode 0755
        {                           //start of error case
            fprintf(stderr,"mkdir: cannot create directory '%s': %s\n",argv[i],strerror(errno));     //e.g. File exists
            ret=1;                  //remember the failure, keep going
        }                           //end of error case
    }                               //end of create loop
    return ret;                     //0 if all were created
}                                   //end of mkdir
//touch file...: open() with O_CREAT, then utimensat() to set the time to now
int touch_call(int argc,char *argv[])   //touch command
{                                   //start of touch
    char real[PATH_MAX];            //path after "~" expansion
    int i,fd,ret=0;                 //counter, file descriptor, exit status
    if(argc<2)                      //no file given
    {                               //start of error case
        fprintf(stderr,"usage: touch <file> ...\n");   //show the usage
        return 1;                   //failure
    }                               //end of error case
    for(i=1;i<argc;i++)             //each file
    {                               //start of file loop
        if(expand_path("touch",argv[i],real)!=0){ret=1;continue;}  //handle "~", jail check
        fd=open(real,O_WRONLY|O_CREAT|O_NONBLOCK,0644);    //system call: create if missing (mode 644), keep content
        if(fd<0&&errno!=EISDIR)     //failed, and not because it is a folder
        {                           //start of error case
            fprintf(stderr,"touch: cannot touch '%s': %s\n",argv[i],strerror(errno));     //report it
            ret=1;                  //remember the failure
            continue;               //next file
        }                           //end of error case
        if(fd>=0)close(fd);         //system call: close it again
        if(utimensat(AT_FDCWD,real,NULL,0)!=0)     //system call: set access/modify time to now (works on folders too)
        {                           //start of error case
            fprintf(stderr,"touch: cannot touch '%s': %s\n",argv[i],strerror(errno));     //report it
            ret=1;                  //remember the failure
        }                           //end of error case
    }                               //end of file loop
    return ret;                     //0 if all succeeded
}                                   //end of touch
//rm -r: remove a folder and everything inside it
//unlink() is tried first: it removes files and symlinks (never following them) and fails with EISDIR on a folder
int remove_tree(const char *path)   //helper for rm -r (recursive)
{                                   //start of remove_tree
    char sub[PATH_MAX+512];         //path of one entry inside the folder
    struct dirent *e;               //one folder entry
    DIR *dir;                       //open folder handle
    int ret=0;                      //0 = all removed
    if(unlink(path)==0)return 0;    //system call: a file or a symlink is removed right here
    if(errno!=EISDIR)return -1;     //failed for another reason than "is a folder"
    dir=opendir(path);              //system call: open the folder
    if(dir==NULL)return -1;         //cannot open it
    while((e=readdir(dir))!=NULL)   //system call: each entry inside
    {                               //start of entry loop
        if(strcmp(e->d_name,".")==0||strcmp(e->d_name,"..")==0)continue;  //never touch "." and ".."
        snprintf(sub,sizeof(sub),"%s/%s",path,e->d_name);  //full path of the entry
        if(remove_tree(sub)!=0)ret=-1;  //remove it (recursively); remember any failure
    }                               //end of entry loop
    closedir(dir);                  //system call: close the folder
    if(rmdir(path)!=0)ret=-1;       //system call: remove the now empty folder
    return ret;                     //0 if everything was removed
}                                   //end of remove_tree
//rm [-r] [-f] path...: unlink() for files, rmdir() for empty folders
int rm_call(int argc,char *argv[])   //rm command
{                                   //start of rm
    char real[PATH_MAX],cwd[PATH_MAX],name[PATH_MAX],*p;   //expanded path, current folder, cleaned name
    const char *b;                  //last part of the name
    int i,rflag=0,fflag=0,count=0,ret=0;   //-r, -f, number of paths, exit status
    size_t len;                     //string length helper
    for(i=1;i<argc;i++)             //first pass: options and count
    {                               //start of option loop
        if(argv[i][0]=='-'&&argv[i][1]!='\0')  //an option like -r, -f, -rf
        {                           //start of option case
            for(p=argv[i]+1;*p;p++) //each letter
            {                       //start of letter loop
                if(*p=='r'||*p=='R')rflag=1;   //-r / -R: recursive
                else if(*p=='f')fflag=1;    //-f: ignore missing files
                else                //unknown letter
                {                   //start of error case
                    fprintf(stderr,"rm: invalid option -- '%c'\n",*p);    //report it
                    return 1;       //failure
                }                   //end of error case
            }                       //end of letter loop
        }                           //end of option case
        else count++;               //a path to remove
    }                               //end of option loop
    if(count==0)                    //nothing to remove
    {                               //start of error case
        fprintf(stderr,"usage: rm [-r] [-f] <file | directory> ...\n");    //show the usage
        return 1;                   //failure
    }                               //end of error case
    for(i=1;i<argc;i++)             //second pass: remove each path
    {                               //start of remove loop
        if(argv[i][0]=='-'&&argv[i][1]!='\0')continue;     //skip options
        snprintf(name,sizeof(name),"%s",argv[i]);  //copy the name to clean it
        len=strlen(name);           //its length
        while(len>1&&name[len-1]=='/')name[--len]='\0';    //drop trailing '/' ("dir/" -> "dir")
        b=base_name(name);          //last part of the name
        if(strcmp(b,".")==0||strcmp(b,"..")==0)    //safety: never remove "." or ".."
        {                           //start of refuse case
            fprintf(stderr,"rm: refusing to remove '.' or '..' directory: skipping '%s'\n",argv[i]);  //explain
            ret=1;                  //failure for this one
            continue;               //next path
        }                           //end of refuse case
        if(expand_link("rm",argv[i],real)!=0){ret=1;continue;}   //handle "~", jail check (a link itself is not followed)
        len=strlen(real);           //length of the expanded path
        while(len>1&&real[len-1]=='/')real[--len]='\0';     //"//" is "/" too
        if(strcmp(real,"/")==0)     //safety: never remove the root folder
        {                           //start of refuse case
            fprintf(stderr,"rm: refusing to remove the root directory '/'\n");     //explain
            ret=1;                  //failure for this one
            continue;               //next path
        }                           //end of refuse case
        len=strlen(real);           //length again for the prefix test
        if(getcwd(cwd,sizeof(cwd))!=NULL&&strncmp(cwd,real,len)==0&&(cwd[len]=='\0'||cwd[len]=='/'))  //safety: we are inside it
        {                           //start of refuse case
            fprintf(stderr,"rm: cannot remove '%s': current directory is inside it\n",argv[i]);    //explain
            ret=1;                  //failure for this one
            continue;               //next path
        }                           //end of refuse case
        if(unlink(real)==0)continue;                //file or symlink
        if(errno==ENOENT&&fflag)continue;   //missing and -f given: stay quiet
        if(errno!=EISDIR)           //any error other than "is a folder"
        {                           //start of error case
            fprintf(stderr,"rm: cannot remove '%s': %s\n",argv[i],strerror(errno));   //e.g. No such file
            ret=1;                  //remember the failure
            continue;               //next path
        }                           //end of error case
        if((rflag?remove_tree(real):rmdir(real))!=0)  //folder
        {                           //start of error case
            if(errno==ENOTEMPTY||errno==EEXIST)fprintf(stderr,"rm: cannot remove '%s': Directory not empty (use rm -r)\n",argv[i]);   //hint to use -r
            else fprintf(stderr,"rm: cannot remove '%s': %s\n",argv[i],strerror(errno));  //other reason
            ret=1;                  //remember the failure
        }                           //end of error case
    }                               //end of remove loop
    return ret;                     //0 if everything was removed
}                                   //end of rm
//copy one regular file with read() and write()
int copy_file(const char *src,const char *dst,const char *sname,const char *dname,mode_t mode)   //copy src to dst; sname/dname are the names shown in errors
{                                   //start of copy_file
    char buf[BUF_SIZE];             //copy buffer (4 KB)
    int in,out;                     //source and destination file descriptors
    ssize_t n;                      //bytes read in one step
    in=open(src,O_RDONLY);          //system call: open the source for reading
    if(in<0)                        //cannot open it
    {                               //start of error case
        fprintf(stderr,"cp: cannot open '%s' for reading: %s\n",sname,strerror(errno));  //e.g. Permission denied
        return 1;                   //failure
    }                               //end of error case
    out=open(dst,O_WRONLY|O_CREAT|O_TRUNC,mode&0777);  //system call: create/empty the destination with the source's mode
    if(out<0)                       //cannot create it
    {                               //start of error case
        fprintf(stderr,"cp: cannot create '%s': %s\n",dname,strerror(errno));     //report it
        close(in);                  //do not leak the source fd
        return 1;                   //failure
    }                               //end of error case
    while((n=read(in,buf,sizeof(buf)))>0)   //system call: read the next block
    {                               //start of copy loop
        if(write_all(out,buf,n)!=0) //write the same block to the destination
        {                           //start of error case
            perror("cp: write");    //e.g. disk full
            break;                  //stop copying
        }                           //end of error case
    }                               //end of copy loop
    if(n<0)perror("cp: read");      //read() failed
    close(in);                      //system call: close the source
    close(out);                     //system call: close the destination
    return n!=0;                    //0 only when read() reached the end of the file
}                                   //end of copy_file
//cp src dst | cp src... dir
int cp_call(int argc,char *argv[])   //cp command
{                                   //start of cp
    char src[PATH_MAX],dst[PATH_MAX],target[PATH_MAX+512];     //source, destination, final target path
    struct stat ss,ds;              //information about source and destination
    int i,isdir,ret=0;              //counter, "destination is a folder", exit status
    if(argc<3)                      //need at least a source and a destination
    {                               //start of error case
        fprintf(stderr,"usage: cp <source> <destination>  |  cp <source>... <directory>\n");  //show the usage
        return 1;                   //failure
    }                               //end of error case
    if(expand_path("cp",argv[argc-1],dst)!=0)return 1;     //last argument is the destination
    isdir=stat(dst,&ds)==0&&S_ISDIR(ds.st_mode);   //system call: is the destination a folder?
    if(argc>3&&!isdir)              //several sources need a folder as destination
    {                               //start of error case
        fprintf(stderr,"cp: target '%s' is not a directory\n",argv[argc-1]);   //report it
        return 1;                   //failure
    }                               //end of error case
    for(i=1;i<argc-1;i++)           //copy each source
    {                               //start of source loop
        if(expand_path("cp",argv[i],src)!=0){ret=1;continue;}  //handle "~", jail check
        if(stat(src,&ss)!=0)        //system call: source must exist
        {                           //start of error case
            fprintf(stderr,"cp: cannot stat '%s': %s\n",argv[i],strerror(errno));     //e.g. No such file
            ret=1;                  //remember the failure
            continue;               //next source
        }                           //end of error case
        if(S_ISDIR(ss.st_mode))     //folders are not copied (no -r)
        {                           //start of error case
            fprintf(stderr,"cp: -r not specified; omitting directory '%s'\n",argv[i]);    //explain
            ret=1;                  //remember the failure
            continue;               //next source
        }                           //end of error case
        if(isdir)snprintf(target,sizeof(target),"%s/%s",dst,base_name(src));  //into a folder: keep the file name
        else snprintf(target,sizeof(target),"%s",dst);  //otherwise copy to the given name
        if(!jail_ok(target,1))      //a link inside the folder could still lead outside
        {                           //start of error case
            fprintf(stderr,"cp: %s: access denied (outside %s)\n",target,base_name(start_dir));   //report it
            ret=1;                  //remember the failure
            continue;               //next source
        }                           //end of error case
        if(stat(target,&ds)==0&&ds.st_dev==ss.st_dev&&ds.st_ino==ss.st_ino)    //same device + inode = same file
        {                           //start of error case
            fprintf(stderr,"cp: '%s' and '%s' are the same file\n",argv[i],argv[argc-1]);     //copying would empty it
            ret=1;                  //remember the failure
            continue;               //next source
        }                           //end of error case
        if(copy_file(src,target,argv[i],argv[argc-1],ss.st_mode)!=0)ret=1;    //do the copy with read()/write()
    }                               //end of source loop
    return ret;                     //0 if all were copied
}                                   //end of cp
//mv src dst | mv src... dir: rename()
int mv_call(int argc,char *argv[])   //mv command
{                                   //start of mv
    char src[PATH_MAX],dst[PATH_MAX],target[PATH_MAX+512];     //source, destination, final target path
    struct stat ss,ds;              //information about source and destination
    int i,isdir,ret=0;              //counter, "destination is a folder", exit status
    size_t len;                     //string length helper
    if(argc<3)                      //need a source and a destination
    {                               //start of error case
        fprintf(stderr,"usage: mv <source> <destination>  |  mv <source>... <directory>\n");  //show the usage
        return 1;                   //failure
    }                               //end of error case
    if(expand_path("mv",argv[argc-1],dst)!=0)return 1;     //last argument is the destination
    isdir=stat(dst,&ds)==0&&S_ISDIR(ds.st_mode);   //system call: is the destination a folder?
    if(argc>3&&!isdir)              //several sources need a folder
    {                               //start of error case
        fprintf(stderr,"mv: target '%s' is not a directory\n",argv[argc-1]);   //report it
        return 1;                   //failure
    }                               //end of error case
    for(i=1;i<argc-1;i++)           //move each source
    {                               //start of source loop
        if(expand_link("mv",argv[i],src)!=0){ret=1;continue;}   //handle "~", jail check (a link itself is not followed)
        if(stat(src,&ss)!=0)        //system call: source must exist
        {                           //start of error case
            fprintf(stderr,"mv: cannot stat '%s': %s\n",argv[i],strerror(errno));     //e.g. No such file
            ret=1;                  //remember the failure
            continue;               //next source
        }                           //end of error case
        len=strlen(src);            //length of the source path
        while(len>1&&src[len-1]=='/')src[--len]='\0';  //drop trailing '/' ("//" becomes "/")
        if(strcmp(src,"/")==0)      //safety: never move the root folder
        {                           //start of refuse case
            fprintf(stderr,"mv: cannot move the root directory\n");    //explain
            ret=1;                  //failure for this one
            continue;               //next source
        }                           //end of refuse case
        if(isdir)snprintf(target,sizeof(target),"%s/%s",dst,base_name(src));  //into a folder: keep the name
        else snprintf(target,sizeof(target),"%s",dst);  //otherwise rename to the given name
        len=strlen(src);            //length for the prefix test
        if(S_ISDIR(ss.st_mode)&&strncmp(target,src,len)==0&&target[len]=='/')     //target lies inside the folder itself
        {                           //start of refuse case
            fprintf(stderr,"mv: cannot move '%s' to a subdirectory of itself\n",argv[i]);     //explain
            ret=1;                  //failure for this one
            continue;               //next source
        }                           //end of refuse case
        if(rename(src,target)!=0)   //system call: move/rename in one step
        {                           //start of error case
            fprintf(stderr,"mv: cannot move '%s' to '%s': %s\n",argv[i],argv[argc-1],strerror(errno));   //report it
            ret=1;                  //remember the failure
        }                           //end of error case
    }                               //end of source loop
    return ret;                     //0 if all were moved
}                                   //end of mv

/* ============ 3. File Viewing & Process Inspection Commands ============ */

//read a whole small file into buf with open(), read(), close()
int read_file(const char *path,char *buf,int size)   //returns bytes read, -1 on error
{                                   //start of read_file
    int fd,total=0;                 //file descriptor, bytes read so far
    ssize_t n;                      //bytes read in one step
    fd=open(path,O_RDONLY);         //system call: open for reading
    if(fd<0)return -1;              //cannot open it
    while(total<size-1&&(n=read(fd,buf+total,size-1-total))>0)total+=n;    //system call: read until full or end of file
    close(fd);                      //system call: close it
    buf[total]='\0';                //make it a C string
    return total;                   //number of bytes read
}                                   //end of read_file
int cat_fd(int fd,const char *name) //copy everything from fd to stdout
{                                   //start of cat_fd
    char buf[BUF_SIZE];             //copy buffer
    ssize_t n;                      //bytes read in one step
    while((n=read(fd,buf,sizeof(buf)))>0)  //system call: read the next block
        if(write_all(STDOUT_FILENO,buf,n)!=0)return 1;     //system call: write it to stdout
    if(n<0)                         //read() failed (e.g. a folder: Is a directory)
    {                               //start of error case
        fprintf(stderr,"cat: %s: %s\n",name,strerror(errno));     //report it
        return 1;                   //failure
    }                               //end of error case
    return 0;                       //success
}                                   //end of cat_fd
//cat [file...]: open(), read(), write(); no file (or "-") reads standard input
int cat_call(int argc,char *argv[])   //cat command
{                                   //start of cat
    char real[PATH_MAX];            //path after "~" expansion
    int i,fd,ret=0;                 //counter, file descriptor, exit status
    if(argc<2)return cat_fd(STDIN_FILENO,"stdin");    //no file: copy standard input (works with < and pipes)
    for(i=1;i<argc;i++)             //each file in order
    {                               //start of file loop
        if(strcmp(argv[i],"-")==0)  //"-" means standard input
        {                           //start of stdin case
            ret|=cat_fd(STDIN_FILENO,"stdin");     //copy stdin
            continue;               //next argument
        }                           //end of stdin case
        if(expand_path("cat",argv[i],real)!=0){ret=1;continue;}    //handle "~", jail check
        fd=open(real,O_RDONLY);     //system call: open the file
        if(fd<0)                    //cannot open it
        {                           //start of error case
            fprintf(stderr,"cat: %s: %s\n",argv[i],strerror(errno));  //e.g. No such file
            ret=1;                  //remember the failure
            continue;               //still print the other files
        }                           //end of error case
        ret|=cat_fd(fd,argv[i]);    //print its content
        close(fd);                  //system call: close it
    }                               //end of file loop
    return ret;                     //0 if all files were printed
}                                   //end of cat
//echo [-n] text...: builtin ($VAR, ${VAR} and $? are expanded by the parser)
int echo_call(int argc,char *argv[])   //echo builtin
{                                   //start of echo
    int i=1,newline=1;              //first word to print, print a newline at the end?
    if(argc>1&&strcmp(argv[1],"-n")==0){newline=0;i=2;}   //-n: no newline, skip the option
    for(;i<argc;i++)                //each word
    {                               //start of word loop
        fputs(argv[i],stdout);      //print the word
        if(i<argc-1)putchar(' ');   //one space between words
    }                               //end of word loop
    if(newline)putchar('\n');       //end the line
    fflush(stdout);                 //write it out now (needed before a redirect is undone)
    return 0;                       //echo always succeeds
}                                   //end of echo
//uptime: /proc/uptime and /proc/loadavg
int uptime_call(int argc,char *argv[])   //uptime command
{                                   //start of uptime
    char buf[256];                  //file content
    double up,load1,load5,load15;   //seconds since boot, 1/5/15-minute load averages
    long total;                     //whole seconds since boot
    if(read_file("/proc/uptime",buf,sizeof(buf))<0||sscanf(buf,"%lf",&up)!=1)     //first number = seconds since boot
    {                               //start of error case
        perror("uptime: /proc/uptime");    //report it
        return 1;                   //failure
    }                               //end of error case
    if(read_file("/proc/loadavg",buf,sizeof(buf))<0||sscanf(buf,"%lf %lf %lf",&load1,&load5,&load15)!=3)   //first three numbers = load averages
    {                               //start of error case
        perror("uptime: /proc/loadavg");   //report it
        return 1;                   //failure
    }                               //end of error case
    total=(long)up;                 //drop the fraction of a second
    printf("Uptime       : %ld days, %ld hours, %ld minutes, %ld seconds\n",total/86400,(total%86400)/3600,(total%3600)/60,total%60);   //split seconds into d/h/m/s
    printf("Load average : %.2f (1 min), %.2f (5 min), %.2f (15 min)\n",load1,load5,load15);  //CPU load
    return 0;                       //success
}                                   //end of uptime
//meminfo: parse /proc/meminfo
int meminfo_call(int argc,char *argv[])   //meminfo command
{                                   //start of meminfo
    char buf[8192],name[64],*line,*save;   //file content, field name, current line, strtok_r state
    char *wanted[]={"MemTotal","MemFree","MemAvailable","Buffers","Cached","SwapTotal","SwapFree"};    //fields we print
    long value,total=-1,avail=-1;   //field value in kB, total and available memory
    int i;                          //counter
    if(read_file("/proc/meminfo",buf,sizeof(buf))<0)   //read the whole file
    {                               //start of error case
        perror("meminfo: /proc/meminfo");  //report it
        return 1;                   //failure
    }                               //end of error case
    for(line=strtok_r(buf,"\n",&save);line!=NULL;line=strtok_r(NULL,"\n",&save))  //go line by line
    {                               //start of line loop
        if(sscanf(line,"%63[^:]: %ld",name,&value)!=2)continue;    //"MemTotal:   16314772 kB"
        if(strcmp(name,"MemTotal")==0)total=value;     //remember total for MemUsed
        if(strcmp(name,"MemAvailable")==0)avail=value;     //remember available for MemUsed
        for(i=0;i<7;i++)            //is this one of the wanted fields?
        {                           //start of field loop
            if(strcmp(name,wanted[i])==0)  //yes
            {                       //start of print case
                printf("%-13s: %10ld kB  (%ld MB)\n",name,value,value/1024);    //print in kB and MB
                break;              //no need to check further
            }                       //end of print case
        }                           //end of field loop
    }                               //end of line loop
    if(total>=0&&avail>=0)printf("%-13s: %10ld kB  (%ld MB)\n","MemUsed",total-avail,(total-avail)/1024);   //used = total - available
    return 0;                       //success
}                                   //end of meminfo

/* ============ Built-in commands (run inside the shell process) ============ */

//cd [dir | -]: chdir(); getcwd() remembers the old folder for "cd -"
int cd_call(int argc,char *argv[])   //cd builtin
{                                   //start of cd
    char real[PATH_MAX],old[PATH_MAX],back[PATH_MAX];  //target path, folder before cd, copy of prevdir
    const char *target;             //where to go
    int dash=0;                     //1 for "cd -" (prints the new folder)
    if(argc>2)                      //cd takes at most one argument
    {                               //start of error case
        fprintf(stderr,"cd: too many arguments\n");    //report it
        return 1;                   //failure
    }                               //end of error case
    if(argc==2&&argv[1][0]=='\0')return 0;     //cd "" stays here, like bash
    target=argc>1?argv[1]:start_dir;   //no argument: the start folder
    if(strcmp(target,"-")==0)       //"cd -": go back to the previous folder
    {                               //start of "cd -" case
        snprintf(back,sizeof(back),"%s",prevdir);  //copy it, prevdir changes below
        target=back;                //go there
        dash=1;                     //print it afterwards
    }                               //end of "cd -" case
    if(expand_path("cd",target,real)!=0)return 1;  //handle "~" and "~/x", jail check
    if(getcwd(old,sizeof(old))==NULL)old[0]='\0';   //system call: remember where we are now
    if(chdir(real)!=0)          //ENOENT, ENOTDIR ("Not a directory"), EACCES ...
    {                               //start of error case
        fprintf(stderr,"cd: %s: %s\n",target,strerror(errno));    //report why
        return 1;                   //failure, folder unchanged
    }                               //end of error case
    if(old[0])strcpy(prevdir,old);  //the old folder becomes the "cd -" target
    if(dash&&getcwd(real,sizeof(real))!=NULL)printf("%s\n",show_path(real));  //"cd -" prints the new folder
    return 0;                       //success
}                                   //end of cd
//exit [n]: leave the shell with status n (default: status of the last command)
int exit_call(int argc,char *argv[])   //exit builtin
{                                   //start of exit
    char *end;                      //where number parsing stopped
    long code=last_status;          //default exit status
    if(argc>2)                      //at most one argument
    {                               //start of error case
        fprintf(stderr,"exit: too many arguments\n");  //report it and stay in the shell
        return 1;                   //failure
    }                               //end of error case
    if(argc==2)                     //an exit status was given
    {                               //start of number case
        code=strtol(argv[1],&end,10);   //read it as a decimal number
        if(argv[1][0]=='\0'||*end!='\0')   //not a whole number
        {                           //start of error case
            fprintf(stderr,"exit: %s: numeric argument required\n",argv[1]);  //report it
            code=2;                 //bash also exits with 2 here
        }                           //end of error case
    }                               //end of number case
    fflush(stdout);                 //write any pending output first
    exit((int)(code&255));          //end the shell process; status is 0-255
}                                   //end of exit
//one help page per command: usage, what it does, and the system calls behind it
struct {const char *name,*usage,*info,*calls;} help_pages[]={  //table used by "help <command>"
    {"cd","cd [dir | -]","Change the shell's directory. No argument = start folder, - = previous folder,\n  .. = parent folder. Nothing above the start folder can be reached.","chdir(), getcwd() (builtin, runs in the shell process)"},   //cd page
    {"echo","echo [-n] text...","Print text. $VAR, ${VAR}, $PWD and $? are expanded; -n = no newline.","write() through stdout, getenv() (builtin)"},  //echo page
    {"exit","exit [n]","Leave the shell with status n (default: status of the last command).","exit() (builtin)"},  //exit page
    {"help","help [command]","Show the command list, or the help page of one command.","builtin"},     //help page
    {"history","history [n]","Show the last n commands (all if n is missing).\n  !! runs the last command again, !N runs command number N.","builtin, in-memory list"},    //history page
    {"pwd","pwd","Print the current directory, written from the start folder's name\n  (e.g. 4_SH_01/demo). The start folder is the root of this shell.","getcwd()"},   //pwd page
    {"ls","ls [-a] [-l] [path...]","List a directory. -a shows hidden files, -l the long format.\n  Folders end with /.","opendir(), readdir(), closedir(), stat()"},    //ls page
    {"whoami","whoami","Print the current user name.","getuid(), getpwuid()"},     //whoami page
    {"uname","uname [-a -s -n -r -v -m]","Print system name, host, kernel release, version and machine.","uname()"},    //uname page
    {"mkdir","mkdir [-p] dir...","Create directories with mode 0755. -p creates missing parents too.","mkdir()"},    //mkdir page
    {"touch","touch file...","Create an empty file, or set the time of an existing one to now.","open(O_CREAT), close(), utimensat()"},   //touch page
    {"rm","rm [-r] [-f] path...","Remove files and empty directories. -r removes a folder and its contents,\n  -f ignores missing files.","unlink(), rmdir()"},   //rm page
    {"cp","cp src dst | cp src... dir","Copy files (dst may be a directory). The mode is kept.","open(), read(), write(), close()"},  //cp page
    {"mv","mv src dst | mv src... dir","Move or rename files and folders.","rename()"},  //mv page
    {"cat","cat [file...]","Print files. No file (or -) reads standard input.","open(), read(), write(), close()"},  //cat page
    {"uptime","uptime","Show how long the system has been running and the load average.","open(), read() on /proc/uptime and /proc/loadavg"},     //uptime page
    {"meminfo","meminfo","Show total, free, available and used memory and swap.","open(), read() on /proc/meminfo"},  //meminfo page
    {NULL,NULL,NULL,NULL}           //end marker of the table
};                                  //end of help_pages
//help [command]: the whole menu, or the page of one command
int help_call(int argc,char *argv[])   //help builtin
{                                   //start of help
    int i,j,ret=0;                  //table index, argument index, exit status
    if(argc>1)                      //"help ls cp": show those pages
    {                               //start of page case
        for(j=1;j<argc;j++)         //each asked command
        {                           //start of argument loop
            for(i=0;help_pages[i].name!=NULL&&strcmp(help_pages[i].name,argv[j])!=0;i++);  //search the table
            if(help_pages[i].name==NULL)   //reached the end marker: not found
            {                       //start of error case
                fprintf(stderr,"help: no help topic for '%s'\n",argv[j]);  //report it
                ret=1;              //remember the failure
                continue;           //next argument
            }                       //end of error case
            printf("%s%s\n  usage : %s\n  %s\n  system calls: %s\n",j>1?"\n":"",help_pages[i].name,help_pages[i].usage,help_pages[i].info,help_pages[i].calls);   //print the page
        }                           //end of argument loop
        return ret;                 //0 if all pages were found
    }                               //end of page case
    printf("myshell - custom UNIX shell.\n\n");    //menu title
    printf("Built-in commands (run inside the shell process):\n");     //group: built-ins
    printf("  cd [dir|-|..]       change directory (no argument = start folder)\n");   //cd line
    printf("  echo [-n] text      print text ($VAR, ${VAR}, $? are expanded)\n");     //echo line
    printf("  exit [n]            leave the shell with status n\n");  //exit line
    printf("  help [command]      show this help, or the help of one command\n");     //help line
    printf("  history [n]         show history; !! repeats last, !N repeats number N\n\n");   //history line
    printf("File and directory commands (child process: fork + waitpid):\n");  //group: file commands
    printf("  pwd                 print current directory (start folder shown by its name)\n");    //pwd line
    printf("  ls [-a] [-l] [path] list a directory\n");     //ls line
    printf("  mkdir [-p] dir...   create directories (mode 0755)\n");     //mkdir line
    printf("  touch file...       create file / update its time\n");  //touch line
    printf("  rm [-r] [-f] path.. remove files or empty directories (-r: everything)\n");  //rm line
    printf("  cp src dst          copy a file (dst may be a directory)\n");   //cp line
    printf("  mv src dst          move / rename (dst may be a directory)\n");     //mv line
    printf("  cat [file...]       print files (no file: standard input)\n\n");    //cat line
    printf("System information:\n");   //group: system info
    printf("  whoami              print user name\n");  //whoami line
    printf("  uname [-asnrvm]     print system information\n");   //uname line
    printf("  uptime              system uptime and load average\n");     //uptime line
    printf("  meminfo             memory statistics\n\n");    //meminfo line
    printf("Any other command is run with fork + execvp (e.g. wc, sort, grep).\n");   //external programs
    printf("The start folder is the root: nothing outside it can be reached.\n\n");   //the jail
    printf("Operators: cmd1 | cmd2   cmd > file   cmd >> file   cmd < file   cmd &\n");  //pipes, redirection, background
    printf("Quotes: \"text with $VAR\"  'literal text'  \\x escapes one character\n");  //quoting rules
    printf("Keys: Ctrl+C stops the running command, Ctrl+D leaves the shell.\n");  //keyboard keys
    printf("Type \"help <command>\" for details, e.g. help ls\n");     //how to get a page
    return 0;                       //success
}                                   //end of help
void hist_add(const char *line)     //store one command line in the history ring
{                                   //start of hist_add
    int k=hist_total%HIST_SIZE;     //slot to use (oldest entry is overwritten after 100)
    free(history[k]);               //free the old text in that slot (free(NULL) is fine)
    history[k]=strdup(line);        //keep a copy of the line
    hist_total++;                   //one more command in total
}                                   //end of hist_add
//history entry number num (1 based), NULL if it does not exist
const char *hist_get(long num)   //get history entry number num
{                                   //start of hist_get
    if(num<1||num>hist_total||num<=hist_total-HIST_SIZE)return NULL;   //out of range or already forgotten
    return history[(num-1)%HIST_SIZE];     //its slot in the ring
}                                   //end of hist_get
//history [n]: list the last commands
int history_call(int argc,char *argv[])   //history builtin
{                                   //start of history
    int i,start=hist_total>HIST_SIZE?hist_total-HIST_SIZE:0;   //oldest entry still kept
    long n;                         //how many to show
    char *end;                      //where number parsing stopped
    if(argc>2)                      //at most one argument
    {                               //start of error case
        fprintf(stderr,"history: too many arguments\n");   //report it
        return 1;                   //failure
    }                               //end of error case
    if(argc==2)                     //"history n"
    {                               //start of number case
        n=strtol(argv[1],&end,10);  //read n
        if(argv[1][0]=='\0'||*end!='\0'||n<0)  //not a whole number >= 0
        {                           //start of error case
            fprintf(stderr,"history: %s: numeric argument required\n",argv[1]);   //report it
            return 1;               //failure
        }                           //end of error case
        if(hist_total-n>start)start=hist_total-n;  //show only the last n
    }                               //end of number case
    for(i=start;i<hist_total;i++)printf("%5d  %s\n",i+1,history[i%HIST_SIZE]);    //number and command
    return 0;                       //success
}                                   //end of history

typedef struct {const char *name;int (*fn)(int,char **);int builtin;} Command;    //name, function, 1 = runs in the parent
Command commands[]={                //table of every command written in this file
    {"cd",cd_call,1},{"echo",echo_call,1},{"exit",exit_call,1},{"help",help_call,1},{"history",history_call,1},   //built-ins: run inside the shell
    {"pwd",pwd_call,0},{"ls",ls_call,0},{"whoami",whoami_call,0},{"uname",uname_call,0},     //run in a forked child
    {"mkdir",mkdir_call,0},{"touch",touch_call,0},{"rm",rm_call,0},{"cp",cp_call,0},{"mv",mv_call,0},   //run in a forked child
    {"cat",cat_call,0},{"uptime",uptime_call,0},{"meminfo",meminfo_call,0},    //run in a forked child
    {NULL,NULL,0}                   //end marker
};                                  //end of commands
Command *find_command(const char *name)    //look a name up in the table
{                                   //start of find_command
    int i;                          //table index
    for(i=0;commands[i].name!=NULL;i++)    //walk the table
        if(strcmp(commands[i].name,name)==0)return &commands[i];   //found it
    return NULL;                    //not ours: an external program
}                                   //end of find_command

/* ============ Command parsing ============ */

enum {T_WORD,T_PIPE,T_IN,T_OUT,T_APPEND,T_BG};     //token kinds: word | < > >> &
typedef struct {int type;char *word;} Token;   //one token; word is set only for T_WORD
typedef struct {int type;char *file;} Redir;      //one < > or >> with its file, kept in typed order
typedef struct {char *argv[MAX_ARGS];int argc;Redir redir[MAX_REDIR];int nredir;} Cmd;  //one command of a pipeline
char words[WORD_BUF];       //storage for the words of the current line

int add_char(char **wp,char c)      //append one character to the word storage
{                                   //start of add_char
    if(*wp>=words+WORD_BUF-1)       //storage is full
    {                               //start of error case
        fprintf(stderr,"myshell: command line too long\n");    //report it
        return -1;                  //failure
    }                               //end of error case
    *(*wp)++=c;                     //store the character and move on
    return 0;                       //success
}                                   //end of add_char
//value of variable name: $PWD comes from getcwd(), the rest from the environment
const char *var_value(const char *name)   //value of one shell variable
{                                   //start of var_value
    static char cwd[PATH_MAX];      //static so the pointer stays valid after return
    if(strcmp(name,"PWD")==0)return getcwd(cwd,sizeof(cwd))?show_path(cwd):NULL;   //$PWD is the current folder, shown like pwd
    return getenv(name);            //any other variable, e.g. $HOME, $PATH
}                                   //end of var_value
//$NAME, ${NAME} or $? at *pp: append its value to the word
int expand_var(const char **pp,char **wp)   //expand one $ variable
{                                   //start of expand_var
    const char *p=*pp+1,*val;       //p: character after '$'; val: the value to insert
    char name[256],num[32];         //variable name, number text for $?
    int k=0;                        //length of the name
    if(*p=='?'){snprintf(num,sizeof(num),"%d",last_status);val=num;p++;}  //$?: last exit status
    else if(*p=='{')                //${NAME}
    {                               //start of ${NAME} case
        p++;                        //skip '{'
        while(*p&&*p!='}'&&k<255)name[k++]=*p++;   //copy the name up to '}'
        if(*p!='}'||k==0)           //no closing '}' or empty name
        {                           //start of error case
            fprintf(stderr,"myshell: bad substitution\n");     //report it
            return -1;              //failure
        }                           //end of error case
        p++;                        //skip '}'
        name[k]='\0';               //end the name
        if(strcmp(name,"?")==0){snprintf(num,sizeof(num),"%d",last_status);val=num;}  //${?}
        else val=var_value(name);   //look the variable up
    }                               //end of ${NAME} case
    else if(isalpha((unsigned char)*p)||*p=='_')   //$NAME: starts with a letter or '_'
    {                               //start of $NAME case
        while((isalnum((unsigned char)*p)||*p=='_')&&k<255)name[k++]=*p++;    //copy letters, digits, '_'
        name[k]='\0';               //end the name
        val=var_value(name);        //look the variable up
    }                               //end of $NAME case
    else val="$";               //a lone "$" stays as it is
    *pp=p;                          //tell the caller where the variable ended
    if(val==NULL)val="";            //unset variable: empty text
    for(;*val;val++)if(add_char(wp,*val))return -1;    //copy the value into the word
    return 0;                       //success
}                                   //end of expand_var
//split a line into words and the operators | < > >> &
int tokenize(const char *p,Token *tok,int *ntok)   //line -> tokens
{                                   //start of tokenize
    char *wp=words,*start;          //wp: next free spot in words; start: beginning of this word
    int n=0,quoted;                 //token count; did this word contain quotes?
    while(1)                        //one token per round
    {                               //start of token loop
        while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;   //skip spaces between tokens
        if(*p=='\0'||*p=='#')break;                 //end of line or comment
        if(n>=MAX_TOKENS)           //too many tokens
        {                           //start of error case
            fprintf(stderr,"myshell: too many arguments\n");   //report it
            return -1;              //failure
        }                           //end of error case
        if(*p=='|'){tok[n++].type=T_PIPE;p++;continue;}    //pipe operator
        if(*p=='<'){tok[n++].type=T_IN;p++;continue;}  //input redirection
        if(*p=='&'){tok[n++].type=T_BG;p++;continue;}  //background operator
        if(*p=='>')                 //output redirection: > or >>
        {                           //start of '>' case
            if(p[1]=='>'){tok[n++].type=T_APPEND;p+=2;}    //">>" appends
            else{tok[n++].type=T_OUT;p++;}  //">" truncates
            continue;               //next token
        }                           //end of '>' case
        start=wp;                   //a word begins here
        quoted=0;                   //no quotes seen yet
        while(*p&&!strchr(" \t\r\n|<>&",*p))   //a word ends at a space or an operator
        {                           //start of word loop
            if(*p=='\'')            //'single quotes': everything literal
            {                       //start of single-quote case
                quoted=1;           //the word had quotes (so "" still counts as a word)
                p++;                //skip the opening quote
                while(*p&&*p!='\'')if(add_char(&wp,*p++))return -1;    //copy everything up to the closing quote
                if(*p!='\'')        //line ended without a closing quote
                {                   //start of error case
                    fprintf(stderr,"myshell: syntax error: unterminated quote\n");     //report it
                    return -1;      //failure
                }                   //end of error case
                p++;                //skip the closing quote
            }                       //end of single-quote case
            else if(*p=='"')        //"double quotes": only $ and \ are special
            {                       //start of double-quote case
                quoted=1;           //the word had quotes
                p++;                //skip the opening quote
                while(*p&&*p!='"')  //up to the closing quote
                {                   //start of inner loop
                    if(*p=='\\'&&p[1]&&strchr("\"\\$",p[1])){p++;if(add_char(&wp,*p++))return -1;}   //\" \\ \$ give the plain character
                    else if(*p=='$'){if(expand_var(&p,&wp))return -1;}     //variables are expanded inside "..."
                    else if(add_char(&wp,*p++))return -1;  //any other character is copied
                }                   //end of inner loop
                if(*p!='"')         //line ended without a closing quote
                {                   //start of error case
                    fprintf(stderr,"myshell: syntax error: unterminated quote\n");     //report it
                    return -1;      //failure
                }                   //end of error case
                p++;                //skip the closing quote
            }                       //end of double-quote case
            else if(*p=='\\')       //backslash outside quotes
            {                       //start of backslash case
                p++;                //skip the backslash
                if(*p&&add_char(&wp,*p++))return -1;   //next character is taken literally
            }                       //end of backslash case
            else if(*p=='$'){if(expand_var(&p,&wp))return -1;}     //$VAR outside quotes
            else if(add_char(&wp,*p++))return -1;  //an ordinary character
        }                           //end of word loop
        if(wp==start&&!quoted)continue;            //unset $VAR expands to nothing
        if(add_char(&wp,'\0'))return -1;   //end the word string
        tok[n].type=T_WORD;         //it is a word token
        tok[n++].word=start;        //pointing into the words storage
    }                               //end of token loop
    *ntok=n;                        //tell the caller how many tokens
    return 0;                       //success
}                                   //end of tokenize
const char *token_name(Token *tok,int i,int ntok)  //text of token i for error messages
{                                   //start of token_name
    const char *names[]={"word","|","<",">",">>","&"};     //one name per token kind
    return i>=ntok?"newline":names[tok[i].type];   //past the end means "newline", like bash
}                                   //end of token_name
//group the tokens into commands connected by pipes
int parse(Token *tok,int ntok,Cmd *cmd,int *ncmd,int *bg)   //tokens -> pipeline of commands
{                                   //start of parse
    int i,n=0;                      //token index, index of the current command
    Cmd *c=&cmd[0];                 //command being filled
    *bg=0;                          //not a background job yet
    *ncmd=0;                        //no commands yet
    memset(c,0,sizeof(Cmd));        //start with an empty command
    for(i=0;i<ntok;i++)             //each token
    {                               //start of token loop
        switch(tok[i].type)         //act on its kind
        {                           //start of switch
        case T_WORD:                //a word: add it to argv
            if(c->argc>=MAX_ARGS-1) //keep one slot for the NULL end
            {                       //start of error case
                fprintf(stderr,"myshell: too many arguments (max %d)\n",MAX_ARGS-1);   //report it
                return -1;          //failure
            }                       //end of error case
            c->argv[c->argc++]=tok[i].word;    //store the word
            break;                  //next token
        case T_PIPE:                //'|': start the next command
            if(c->argc==0)          //"| ls" or "ls | | cat": nothing before the pipe
            {                       //start of error case
                fprintf(stderr,"myshell: syntax error near unexpected token `|'\n");   //report it
                return -1;          //failure
            }                       //end of error case
            if(n+1>=MAX_CMDS)       //pipeline too long
            {                       //start of error case
                fprintf(stderr,"myshell: too many pipes (max %d commands)\n",MAX_CMDS);    //report it
                return -1;          //failure
            }                       //end of error case
            c=&cmd[++n];            //move to the next command
            memset(c,0,sizeof(Cmd));    //and empty it
            break;                  //next token
        case T_BG:                  //'&': run in the background
            if(i!=ntok-1||c->argc==0)  //must be last and follow a command
            {                       //start of error case
                fprintf(stderr,"myshell: syntax error near unexpected token `&'\n");   //report it
                return -1;          //failure
            }                       //end of error case
            *bg=1;                  //mark the line as a background job
            break;                  //next token
        default:        //< > >>
            if(i+1>=ntok||tok[i+1].type!=T_WORD)   //a file name must follow
            {                       //start of error case
                fprintf(stderr,"myshell: syntax error near unexpected token `%s'\n",token_name(tok,i+1,ntok));     //e.g. `newline'
                return -1;          //failure
            }                       //end of error case
            if(c->nredir>=MAX_REDIR)   //too many redirections on one command
            {                       //start of error case
                fprintf(stderr,"myshell: too many redirections (max %d)\n",MAX_REDIR);     //report it
                return -1;          //failure
            }                       //end of error case
            c->redir[c->nredir].type=tok[i].type;  //remember the kind (< > >>)
            c->redir[c->nredir++].file=tok[i+1].word;  //and the file name
            i++;                    //the file name is used up
        }                           //end of switch
    }                               //end of token loop
    if(c->argc==0)                  //last command has no words
    {                               //start of empty case
        if(n==0&&c->nredir==0)return 0;        //empty line
        fprintf(stderr,"myshell: syntax error: missing command\n");    //e.g. "ls |"
        return -1;                  //failure
    }                               //end of empty case
    for(i=0;i<=n;i++)cmd[i].argv[cmd[i].argc]=NULL;    //execvp() needs a NULL at the end of argv
    *ncmd=n+1;                      //number of commands in the pipeline
    return 0;                       //success
}                                   //end of parse

/* ============ Process control ============ */

//< > >>: open() the file and dup2() it onto stdin / stdout
int redirect(Cmd *c)   //apply all redirections of one command
{                                   //start of redirect
    char real[PATH_MAX];            //file path after "~" expansion
    int i,fd,flags;                 //counter, file descriptor, open() flags
    Redir *r;                       //current redirection
    for(i=0;i<c->nredir;i++)        //in typed order, like bash: "> a > b" creates a, output goes to b
    {                               //start of redirection loop
        r=&c->redir[i];             //this redirection
        if(expand_path("myshell",r->file,real)!=0)return -1;   //handle "~", jail check
        if(r->type==T_IN)flags=O_RDONLY;   //'<' reads the file
        else flags=O_WRONLY|O_CREAT|(r->type==T_APPEND?O_APPEND:O_TRUNC);  //'>' empties it, '>>' adds to the end
        fd=open(real,flags,0644);   //system call: open (new files get mode 644)
        if(fd<0)                    //cannot open it
        {                           //start of error case
            fprintf(stderr,"myshell: %s: %s\n",r->file,strerror(errno));  //e.g. No such file, Permission denied
            return -1;              //the command will not run
        }                           //end of error case
        dup2(fd,r->type==T_IN?STDIN_FILENO:STDOUT_FILENO);     //system call: put the file on fd 0 or fd 1
        close(fd);                  //system call: the original fd is no longer needed
    }                               //end of redirection loop
    return 0;                       //success
}                                   //end of redirect
//inside a child: run our own command, or exec the program
void child_run(Cmd *c)   //runs in the forked child, never returns
{                                   //start of child_run
    Command *b=find_command(c->argv[0]);   //is it one of our commands?
    int r,i;                        //its exit status, argument counter
    char real[PATH_MAX];            //result of the argument check, not used afterwards
    if(redirect(c)!=0)_exit(1);     //set up < > >>; on error the child ends with 1
    if(b!=NULL)                     //our own command (ls, cp, help in a pipe, ...)
    {                               //start of own-command case
        r=b->fn(c->argc,c->argv);   //run it in this child
        fflush(stdout);             //write its output before leaving
        _exit(r);                   //end the child with its status
    }                               //end of own-command case
    for(i=0;i<c->argc;i++)          //external program: check every argument first
        if((i>0||strchr(c->argv[0],'/')!=NULL)&&expand_path(c->argv[0],c->argv[i],real)!=0)_exit(1);   //argv[0] counts only if it is a path; refused: the child ends with 1
    if(jail_program()!=0)           //start the sandbox before the program runs
    {                               //start of error case
        perror("myshell: landlock");   //report it
        _exit(126);                 //never run a program without its sandbox
    }                               //end of error case
    execvp(c->argv[0],c->argv);     //system call: replace this child with the program (searches PATH)
    if(errno==ENOENT)fprintf(stderr,"%s: command not found\n",c->argv[0]);     //only reached if execvp failed
    else fprintf(stderr,"%s: %s\n",c->argv[0],strerror(errno));   //e.g. Permission denied
    _exit(errno==ENOENT?127:126);   //127 = not found, 126 = cannot run (like bash)
}                                   //end of child_run
int exit_code(int status)           //turn a waitpid() status into a shell exit status
{                                   //start of exit_code
    if(WIFEXITED(status))return WEXITSTATUS(status);   //normal exit: its status
    if(WIFSIGNALED(status))return 128+WTERMSIG(status);    //killed by signal n: 128+n
    return 1;                       //anything else counts as failure
}                                   //end of exit_code
//run cmd[0] | cmd[1] | ... with pipe(), fork(), dup2(), waitpid()
int execute(Cmd *cmd,int n,int bg)   //run a parsed line; returns its exit status
{                                   //start of execute
    pid_t pids[MAX_CMDS],pid;       //pid of each child, pid from fork()
    int i,j,pfd[2],prev=-1,status=0,r=1,fd;    //counters, pipe ends, read end of the previous pipe, wait status, result, fd
    Command *b=find_command(cmd[0].argv[0]);   //is the first command one of ours?
    if(n==1&&!bg&&b!=NULL&&b->builtin)       //builtin: no child process
    {                               //start of builtin case
        fflush(stdout);             //write pending output before fds change
        dup2(STDIN_FILENO,SAVED_IN);        //keep the terminal on spare fds while redirected
        dup2(STDOUT_FILENO,SAVED_OUT);     //system call: save stdout too
        if(redirect(&cmd[0])==0)r=b->fn(cmd[0].argc,cmd[0].argv);  //apply < > >> and run it in the shell itself
        fflush(stdout);             //write its output into the redirected file
        dup2(SAVED_IN,STDIN_FILENO);   //system call: give stdin back
        dup2(SAVED_OUT,STDOUT_FILENO); //system call: give stdout back
        close(SAVED_IN);            //system call: spare fd no longer needed
        close(SAVED_OUT);           //system call: spare fd no longer needed
        return r;                   //status of the builtin
    }                               //end of builtin case
    for(i=0;i<n;i++)                //start one child per command
    {                               //start of fork loop
        if(i<n-1&&pipe(pfd)!=0)     //system call: a pipe to the next command (not after the last)
        {                           //start of error case
            perror("myshell: pipe");   //report it
            break;                  //stop starting commands
        }                           //end of error case
        fflush(stdout);             //so the child does not inherit unwritten output
        pid=fork();                 //system call: create a child process
        if(pid<0)                   //fork failed
        {                           //start of error case
            perror("myshell: fork");   //report it
            if(i<n-1){close(pfd[0]);close(pfd[1]);}    //close the unused pipe
            break;                  //stop starting commands
        }                           //end of error case
        if(pid==0)                  //this is the child
        {                           //start of child code
            signal(SIGINT,bg?SIG_IGN:SIG_DFL);     //foreground: Ctrl+C kills it; background: ignores Ctrl+C
            if(prev!=-1)            //not the first command: read from the previous pipe
            {                       //start of pipe-input case
                dup2(prev,STDIN_FILENO);   //system call: pipe read end becomes stdin
                close(prev);        //original fd no longer needed
            }                       //end of pipe-input case
            else if(bg&&(fd=open("/dev/null",O_RDONLY))>=0)   //background jobs never read the terminal
            {                       //start of /dev/null case
                dup2(fd,STDIN_FILENO);     //system call: stdin = /dev/null
                close(fd);          //original fd no longer needed
            }                       //end of /dev/null case
            if(i<n-1)               //not the last command: write into the pipe
            {                       //start of pipe-output case
                close(pfd[0]);      //the child does not read its own pipe
                dup2(pfd[1],STDOUT_FILENO);    //system call: pipe write end becomes stdout
                close(pfd[1]);      //original fd no longer needed
            }                       //end of pipe-output case
            child_run(&cmd[i]);     //run the command (never returns)
        }                           //end of child code
        pids[i]=pid;                //parent: remember the child
        if(prev!=-1){close(prev);prev=-1;}     //parent closes the old read end (the child has it)
        if(i<n-1)                   //a new pipe was made
        {                           //start of pipe bookkeeping
            close(pfd[1]);          //parent never writes into it
            prev=pfd[0];            //its read end goes to the next command
        }                           //end of pipe bookkeeping
    }                               //end of fork loop
    if(prev!=-1)close(prev);        //close a read end left over after an error
    if(i==0)return 1;               //no child was started at all
    if(bg)                          //background: do not wait
    {                               //start of background case
        for(j=0;j<MAX_JOBS&&jobs[j].pid!=0;j++);   //find a free job slot
        if(j<MAX_JOBS)              //a slot is free
        {                           //start of slot case
            jobs[j].pid=pids[i-1];  //watch the last command of the pipeline
            jobs[j].num=++job_count;   //give it the next job number
            snprintf(jobs[j].name,sizeof(jobs[j].name),"%s",cmd[0].argv[0]);  //name shown in "Done"
        }                           //end of slot case
        printf("[%d] %d\n",job_count,(int)pids[i-1]);  //"[1] 12345", like bash
        return 0;                   //the shell continues at once
    }                               //end of background case
    for(j=0;j<i;j++)                //foreground: wait for every child
    {                               //start of wait loop
        while(waitpid(pids[j],&status,0)<0&&errno==EINTR);     //system call: wait; retry if Ctrl+C interrupted it
        if(j==i-1)r=exit_code(status);     //the last command decides the status
    }                               //end of wait loop
    if(WIFSIGNALED(status)&&WTERMSIG(status)==SIGINT)printf("\n");  //stopped by Ctrl+C: next prompt on a new line, like bash
    return i<n?1:r;                 //1 if not all commands could start
}                                   //end of execute
//collect finished background jobs: waitpid() with WNOHANG
void reap_jobs(void)   //report finished background jobs
{                                   //start of reap_jobs
    pid_t pid;                      //a finished child
    int status,j;                   //its status, job slot index
    while((pid=waitpid(-1,&status,WNOHANG))>0)     //system call: any finished child? (does not block)
    {                               //start of reap loop
        for(j=0;j<MAX_JOBS;j++)     //is it one of our jobs?
        {                           //start of job loop
            if(jobs[j].pid!=pid)continue;  //not this slot
            printf("[%d] Done (status %d)    %s\n",jobs[j].num,exit_code(status),jobs[j].name);   //report it
            jobs[j].pid=0;          //free the slot
            break;                  //found it
        }                           //end of job loop
    }                               //end of reap loop
    fflush(stdout);                 //show the messages now
}                                   //end of reap_jobs
//a line starting with !! or !N is replaced by that history entry
int hist_expand(char *line)   //replace !! / !N in the line
{                                   //start of hist_expand
    char tmp[MAX_LINE];             //the new line being built
    const char *h;                  //history entry to use
    char *p=line,*rest;             //p: first non-space; rest: text after !! or !N
    long num;                       //N of !N
    while(*p==' '||*p=='\t')p++;    //skip leading spaces
    if(p[0]!='!')return 0;          //not a history command: nothing to do
    if(p[1]=='!'){h=hist_get(hist_total);rest=p+2;}    //!!: the last command
    else if(isdigit((unsigned char)p[1])){num=strtol(p+1,&rest,10);h=hist_get(num);}  //!N: command number N
    else return 0;                  //a '!' followed by anything else is left alone
    if(h==NULL)                     //no such entry
    {                               //start of error case
        fprintf(stderr,"myshell: %.*s: event not found\n",(int)(rest-p),p);   //e.g. "!99: event not found"
        return -1;                  //failure
    }                               //end of error case
    if(strlen(h)+strlen(rest)>=MAX_LINE)   //result would not fit in the line buffer
    {                               //start of error case
        fprintf(stderr,"myshell: command line too long\n");    //report it
        return -1;                  //failure
    }                               //end of error case
    snprintf(tmp,sizeof(tmp),"%s%s",h,rest);   //old command + anything typed after !N
    strcpy(line,tmp);               //replace the line
    printf("%s\n",line);        //show the command that will run
    return 1;                       //the line was replaced
}                                   //end of hist_expand
void on_sigint(int sig)             //runs when Ctrl+C reaches the shell
{                                   //start of on_sigint
    ssize_t r=0;                    //result of write()
    if(at_prompt)r=write(STDOUT_FILENO,"\n",1);    //only at our own prompt: move to a new line (a running command prints its own)
    (void)r;                        //result not needed
    (void)sig;                      //signal number not needed
}                                   //end of on_sigint

/* ============ Main loop ============ */

int main(void)                      //program start
{                                   //start of main
    char line[MAX_LINE];            //one input line
    static char tmpdir[PATH_MAX+8]; //static: putenv() keeps this memory, it must stay valid
    Token tok[MAX_TOKENS];          //tokens of the line
    Cmd cmd[MAX_CMDS];              //commands of the pipeline
    int ntok,ncmd,bg,ch,color=0,interactive=isatty(STDIN_FILENO);     //counts, background flag, colour on?, is stdin a terminal?
    struct sigaction sa;            //how SIGINT is handled
    if(getcwd(prevdir,sizeof(prevdir))==NULL)  //system call: the start folder is the first "cd -" target
    {                               //start of error case
        perror("myshell: getcwd");  //report it
        return 1;                   //cannot run without a current folder
    }                               //end of error case
    strcpy(start_dir,prevdir);      //remember the start folder for pwd and the prompt
    start_len=strcmp(start_dir,"/")==0?0:strlen(start_dir);   //started in "/": show full paths
    snprintf(tmpdir,sizeof(tmpdir),"TMPDIR=%s",start_dir);   //programs cannot reach /tmp, so give them the start folder
    putenv(tmpdir);                 //put TMPDIR in the environment for every child
    memset(&sa,0,sizeof(sa));       //clear the sigaction settings
    sa.sa_handler=on_sigint;        //Ctrl+C does not kill the shell, only the running command
    sigaction(SIGINT,&sa,NULL);     //system call: install the handler
    if(interactive)                 //on a terminal: show the banner
    {                               //start of terminal setup
        color=isatty(STDOUT_FILENO);   //colours only when the output is a terminal too
        printf("%s+------------------------------------------------------------+%s\n",color?C_CYAN:"",color?C_RESET:"");  //banner top line
        printf("%s|  myshell - custom UNIX shell (CSE-3110 OS Lab, Assign. 1)  |%s\n",color?C_CYAN:"",color?C_RESET:"");  //banner title
        printf("%s+------------------------------------------------------------+%s\n",color?C_CYAN:"",color?C_RESET:"");  //banner bottom line
        printf("Type %shelp%s for commands, %shelp <cmd>%s for details, %sexit%s to quit.\n\n",   //how to get started
               color?C_BOLD:"",color?C_RESET:"",color?C_BOLD:"",color?C_RESET:"",color?C_BOLD:"",color?C_RESET:"");  //bold keywords
        if(landlock_version()<1)printf("Note: no Landlock in this kernel; external programs are only checked by their arguments.\n\n");   //warn if the kernel sandbox is missing
    }                               //end of terminal setup
    while(1)                        //the read-execute loop, until exit or Ctrl+D
    {                               //start of main loop
        reap_jobs();                //report background jobs that finished
        if(interactive)             //show a prompt only on a terminal
        {                           //start of prompt
            if(color)printf(C_CYAN "myshell" C_RESET "%s> " C_RESET,last_status?C_RED:"");    //prompt "myshell> " (red ">" after a failure)
            else printf("myshell> ");   //plain prompt, exactly as in the assignment
            fflush(stdout);         //show the prompt now
        }                           //end of prompt
        errno=0;                    //so EINTR can be told apart from end of input
        at_prompt=1;                //Ctrl+C now only gives a fresh prompt
        if(fgets(line,sizeof(line),stdin)==NULL)   //read one line
        {                           //start of no-input case
            at_prompt=0;            //no longer waiting for input
            if(errno==EINTR){clearerr(stdin);continue;}    //Ctrl+C while waiting: just show a new prompt
            if(interactive)printf("exit\n");   //Ctrl+D on a terminal: print "exit" like bash
            break;                  //end of input: leave the loop
        }                           //end of no-input case
        at_prompt=0;                //a command may run now: its Ctrl+C output is its own
        if(strchr(line,'\n')==NULL&&!feof(stdin))  //no newline: the line was longer than the buffer
        {                           //start of too-long case
            while((ch=getchar())!='\n'&&ch!=EOF);  //throw away the rest of that line
            fprintf(stderr,"myshell: input line too long (max %d characters)\n",MAX_LINE-2);   //report it
            last_status=1;          //failure
            continue;               //next prompt
        }                           //end of too-long case
        line[strcspn(line,"\r\n")]='\0';   //remove the newline
        if(hist_expand(line)<0){last_status=1;continue;}   //!! and !N; error if the entry is missing
        if(line[strspn(line," \t")]!='\0')hist_add(line);  //save non-blank lines in history
        if(tokenize(line,tok,&ntok)!=0||parse(tok,ntok,cmd,&ncmd,&bg)!=0)  //split into tokens, then commands
        {                           //start of syntax error case
            last_status=2;          //syntax errors give status 2, like bash
            continue;               //next prompt
        }                           //end of syntax error case
        if(ncmd==0)continue;        //empty line or only a comment
        last_status=execute(cmd,ncmd,bg);  //run it and keep the status for $?
    }                               //end of main loop
    return last_status;             //leave with the status of the last command
}                                   //end of main
