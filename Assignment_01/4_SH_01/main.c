/* myshell - a custom UNIX-like shell (CSE-3110 OS Lab, Assignment 1)
 * The folder where myshell is started becomes its root "/" (pwd and the prompt show it by its name).
 * No command (cd, ls, cat, cp, mv, rm, redirection, ...) can reach a path above it. */
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<ctype.h>
#include<errno.h>
#include<limits.h>
#include<signal.h>
#include<time.h>
#include<unistd.h>
#include<fcntl.h>
#include<dirent.h>
#include<pwd.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<sys/wait.h>
#include<sys/utsname.h>
#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_CMDS 16
#define MAX_TOKENS 256
#define MAX_JOBS 64
#define BUF_SIZE 4096
#define HIST_SIZE 100
#define WORD_BUF 16384

char root[PATH_MAX];        //real path of the shell root ("" when started in "/")
size_t rootlen;
char prevdir[PATH_MAX];     //real path of the previous directory, for "cd -"
int last_status=0;          //exit status of the last command, for $? and exit
char *history[HIST_SIZE];
int hist_total=0;
struct {pid_t pid;int num;char name[64];} jobs[MAX_JOBS];
int job_count=0;

/* ============ Helpers ============ */

int write_all(int fd,const char *buf,size_t n)
{
    ssize_t w;
    while(n>0)
    {
        w=write(fd,buf,n);
        if(w<0&&errno==EINTR)continue;
        if(w<=0)return -1;
        buf+=w;
        n-=w;
    }
    return 0;
}
const char *base_name(const char *path)
{
    const char *p=strrchr(path,'/');
    return p?p+1:path;
}
//is this real path the root itself or something below it?
int inside(const char *real)
{
    if(rootlen==0)return 1;
    return strncmp(real,root,rootlen)==0&&(real[rootlen]=='\0'||real[rootlen]=='/');
}
//real path -> path inside the shell ("/" is the shell root), used when a path is read back
const char *virt(const char *real)
{
    if(!inside(real))return real;
    return real[rootlen]?real+rootlen:"/";
}
//real path -> path printed for the user (the root is shown by its folder name, e.g. "4_SH_01/a/b")
const char *show(const char *real)
{
    static char shown[PATH_MAX];
    if(rootlen==0||!inside(real))return virt(real);
    snprintf(shown,sizeof(shown),"%s%s",base_name(root),real+rootlen);
    return shown;
}
//follow symlinks of the deepest existing part of a real path and make sure it stays inside root
//returns 0 ok, -1 error (errno), -2 outside root
int jail_check(const char *path)
{
    char tmp[PATH_MAX],real[PATH_MAX],*t;
    struct stat st;
    snprintf(tmp,sizeof(tmp),"%s",path);
    while(1)
    {
        if(realpath(tmp,real)!=NULL)return inside(real)?0:-2;
        if(errno!=ENOENT&&errno!=ENOTDIR)return -1;
        if(lstat(tmp,&st)==0)return -2;     //dangling symlink: its target is unknown, block it
        t=strrchr(tmp,'/');
        if(t==NULL)return -1;
        if(t==tmp)t[1]='\0';
        else *t='\0';
    }
}
//turn a user path into a real path inside root
//"/x" and "~/x" start at root, other paths start at the current folder, ".." can never pass root
//returns 0 ok, -1 error (errno), -2 outside root
int resolve(const char *path,char *out)
{
    char vp[PATH_MAX*2],cwd[PATH_MAX],*comp[PATH_MAX/2],*t,*save;
    int n=0,i;
    size_t len;
    if(path==NULL||path[0]=='\0'||strcmp(path,"~")==0)path="/";
    else if(strncmp(path,"~/",2)==0)path++;
    if(path[0]=='/')snprintf(vp,sizeof(vp),"%s",path);
    else
    {
        if(getcwd(cwd,sizeof(cwd))==NULL)return -1;
        if(!inside(cwd))return -2;
        if(strlen(cwd)+strlen(path)+2>sizeof(vp)){errno=ENAMETOOLONG;return -1;}
        snprintf(vp,sizeof(vp),"%s/%s",cwd+rootlen,path);
    }
    for(t=strtok_r(vp,"/",&save);t!=NULL;t=strtok_r(NULL,"/",&save))
    {
        if(strcmp(t,".")==0)continue;
        if(strcmp(t,"..")==0)
        {
            if(n==0)return -2;              //trying to go above root
            n--;
            continue;
        }
        comp[n++]=t;
    }
    len=snprintf(out,PATH_MAX,"%s",root);
    for(i=0;i<n;i++)
    {
        if(len+strlen(comp[i])+2>PATH_MAX){errno=ENAMETOOLONG;return -1;}
        len+=sprintf(out+len,"/%s",comp[i]);
    }
    if(len==0)strcpy(out,"/");
    return jail_check(out);
}
//resolve() and print the error message for command cmd
int fix(const char *cmd,const char *path,char *out)
{
    int r=resolve(path,out);
    if(r==-2)fprintf(stderr,"%s: %s: Permission denied (outside root directory)\n",cmd,path);
    else if(r==-1)fprintf(stderr,"%s: %s: %s\n",cmd,path,strerror(errno));
    return r;
}

/* ============ 1. File & Directory Inspection Commands ============ */

//pwd: getcwd(), printed relative to the shell root
int pwd_call(int argc,char *argv[])
{
    char cwd[PATH_MAX];
    if(getcwd(cwd,sizeof(cwd))==NULL)
    {
        perror("pwd");
        return 1;
    }
    printf("%s\n",show(cwd));
    return 0;
}
int cmp_names(const void *a,const void *b)
{
    return strcmp(*(char *const *)a,*(char *const *)b);
}
//print one ls line; path is the real path, name is what the user sees
void ls_entry(const char *path,const char *name,int lflag)
{
    struct stat st;
    struct passwd *pw;
    char mode[11],tbuf[32],link[PATH_MAX];
    const char *rwx="rwxrwxrwx";
    int i,special=strcmp(name,".")==0||strcmp(name,"..")==0;
    ssize_t n;
    if(!lflag)
    {
        if(!special&&stat(path,&st)==0&&S_ISDIR(st.st_mode))printf("%s/\n",name);
        else printf("%s\n",name);
        return;
    }
    if(lstat(path,&st)!=0)
    {
        fprintf(stderr,"ls: cannot access '%s': %s\n",name,strerror(errno));
        return;
    }
    mode[0]=S_ISDIR(st.st_mode)?'d':S_ISLNK(st.st_mode)?'l':'-';
    for(i=0;i<9;i++)mode[i+1]=(st.st_mode&(0400>>i))?rwx[i]:'-';
    mode[10]='\0';
    pw=getpwuid(st.st_uid);
    strftime(tbuf,sizeof(tbuf),"%b %e %H:%M",localtime(&st.st_mtime));
    printf("%s %2ld %-8s %8lld %s %s",mode,(long)st.st_nlink,pw?pw->pw_name:"?",(long long)st.st_size,tbuf,name);
    if(S_ISLNK(st.st_mode)&&(n=readlink(path,link,sizeof(link)-1))>=0)
    {
        link[n]='\0';
        printf(" -> %s",link);
    }
    else if(S_ISDIR(st.st_mode)&&!special)printf("/");
    printf("\n");
}
//ls [-a] [-l] [path...]: opendir(), readdir(), closedir(), stat()
int ls_call(int argc,char *argv[])
{
    char *targets[MAX_ARGS],real[PATH_MAX],path[PATH_MAX+512],**names,*p;
    int i,j,n,cap,aflag=0,lflag=0,ntarget=0,ret=0;
    DIR *dir;
    struct dirent *e;
    struct stat st;
    for(i=1;i<argc;i++)
    {
        if(argv[i][0]=='-'&&argv[i][1]!='\0')
        {
            for(p=argv[i]+1;*p;p++)
            {
                if(*p=='a')aflag=1;
                else if(*p=='l')lflag=1;
                else
                {
                    fprintf(stderr,"ls: invalid option -- '%c'\n",*p);
                    return 2;
                }
            }
        }
        else targets[ntarget++]=argv[i];
    }
    if(ntarget==0)targets[ntarget++]=".";
    for(i=0;i<ntarget;i++)
    {
        if(fix("ls",targets[i],real)!=0){ret=1;continue;}
        if(stat(real,&st)!=0)
        {
            fprintf(stderr,"ls: cannot access '%s': %s\n",targets[i],strerror(errno));
            ret=1;
            continue;
        }
        if(!S_ISDIR(st.st_mode))
        {
            ls_entry(real,targets[i],lflag);
            continue;
        }
        dir=opendir(real);
        if(dir==NULL)
        {
            fprintf(stderr,"ls: cannot open directory '%s': %s\n",targets[i],strerror(errno));
            ret=1;
            continue;
        }
        if(ntarget>1)printf("%s%s:\n",i?"\n":"",targets[i]);
        n=0;
        cap=64;
        names=malloc(cap*sizeof(char *));
        while((e=readdir(dir))!=NULL)
        {
            if(e->d_name[0]=='.'&&!aflag)continue;      //hidden files only with -a
            if(strcmp(e->d_name,"..")==0&&strcmp(real,root)==0)continue;   //nothing above root
            if(n==cap)
            {
                cap*=2;
                names=realloc(names,cap*sizeof(char *));
            }
            names[n++]=strdup(e->d_name);
        }
        closedir(dir);
        qsort(names,n,sizeof(char *),cmp_names);
        for(j=0;j<n;j++)
        {
            snprintf(path,sizeof(path),"%s/%s",real,names[j]);
            ls_entry(path,names[j],lflag);
            free(names[j]);
        }
        free(names);
    }
    return ret;
}
//whoami: getuid(), getpwuid()
int whoami_call(int argc,char *argv[])
{
    struct passwd *pw=getpwuid(getuid());
    if(pw==NULL)
    {
        perror("whoami");
        return 1;
    }
    printf("%s\n",pw->pw_name);
    return 0;
}
//uname [-a -s -n -r -v -m]: uname()
int uname_call(int argc,char *argv[])
{
    struct utsname u;
    int i,show[5]={0},first=1;
    char *p,*opts="snrvm",*q;
    char *field[5];
    if(uname(&u)!=0)
    {
        perror("uname");
        return 1;
    }
    if(argc==1)
    {
        printf("System Name : %s\n",u.sysname);
        printf("Node Name   : %s\n",u.nodename);
        printf("Release     : %s\n",u.release);
        printf("Version     : %s\n",u.version);
        printf("Machine     : %s\n",u.machine);
        return 0;
    }
    for(i=1;i<argc;i++)
    {
        if(argv[i][0]!='-'||argv[i][1]=='\0')
        {
            fprintf(stderr,"uname: extra operand '%s'\n",argv[i]);
            return 1;
        }
        for(p=argv[i]+1;*p;p++)
        {
            if(*p=='a')show[0]=show[1]=show[2]=show[3]=show[4]=1;
            else if((q=strchr(opts,*p))!=NULL)show[q-opts]=1;
            else
            {
                fprintf(stderr,"uname: invalid option -- '%c'\n",*p);
                return 1;
            }
        }
    }
    field[0]=u.sysname;field[1]=u.nodename;field[2]=u.release;field[3]=u.version;field[4]=u.machine;
    for(i=0;i<5;i++)
    {
        if(!show[i])continue;
        printf("%s%s",first?"":" ",field[i]);
        first=0;
    }
    printf("\n");
    return 0;
}

/* ============ 2. File & Directory Manipulation Commands ============ */

//mkdir -p: create every missing folder on the way
int mkdir_parents(char *real)
{
    char *p;
    struct stat st;
    if(real[rootlen]=='\0')return 0;            //the root always exists
    for(p=real+rootlen+1;*p;p++)
    {
        if(*p!='/')continue;
        *p='\0';
        if(mkdir(real,0755)!=0&&errno!=EEXIST){*p='/';return -1;}
        *p='/';
    }
    if(mkdir(real,0755)!=0)
    {
        if(errno==EEXIST&&stat(real,&st)==0&&S_ISDIR(st.st_mode))return 0;
        return -1;
    }
    return 0;
}
//mkdir [-p] dir...: mkdir() with mode 0755
int mkdir_call(int argc,char *argv[])
{
    char real[PATH_MAX];
    int i,pflag=0,count=0,ret=0;
    for(i=1;i<argc;i++)
    {
        if(strcmp(argv[i],"-p")==0)pflag=1;
        else if(argv[i][0]=='-'&&argv[i][1]!='\0')
        {
            fprintf(stderr,"mkdir: invalid option '%s'\n",argv[i]);
            return 1;
        }
        else count++;
    }
    if(count==0)
    {
        fprintf(stderr,"usage: mkdir [-p] <directory> ...\n");
        return 1;
    }
    for(i=1;i<argc;i++)
    {
        if(strcmp(argv[i],"-p")==0)continue;
        if(fix("mkdir",argv[i],real)!=0){ret=1;continue;}
        if((pflag?mkdir_parents(real):mkdir(real,0755))!=0)
        {
            fprintf(stderr,"mkdir: cannot create directory '%s': %s\n",argv[i],strerror(errno));
            ret=1;
        }
    }
    return ret;
}
//touch file...: open() with O_CREAT, then utimensat() to set the time to now
int touch_call(int argc,char *argv[])
{
    char real[PATH_MAX];
    int i,fd,ret=0;
    if(argc<2)
    {
        fprintf(stderr,"usage: touch <file> ...\n");
        return 1;
    }
    for(i=1;i<argc;i++)
    {
        if(fix("touch",argv[i],real)!=0){ret=1;continue;}
        fd=open(real,O_WRONLY|O_CREAT|O_NONBLOCK,0644);
        if(fd<0&&errno!=EISDIR)
        {
            fprintf(stderr,"touch: cannot touch '%s': %s\n",argv[i],strerror(errno));
            ret=1;
            continue;
        }
        if(fd>=0)close(fd);
        if(utimensat(AT_FDCWD,real,NULL,0)!=0)
        {
            fprintf(stderr,"touch: cannot touch '%s': %s\n",argv[i],strerror(errno));
            ret=1;
        }
    }
    return ret;
}
//rm -r: remove a folder and everything inside it (symlinks are removed, never followed)
int remove_tree(const char *path)
{
    char sub[PATH_MAX+512];
    struct stat st;
    struct dirent *e;
    DIR *dir;
    int ret=0;
    if(lstat(path,&st)!=0)return -1;
    if(!S_ISDIR(st.st_mode))return unlink(path);
    dir=opendir(path);
    if(dir==NULL)return -1;
    while((e=readdir(dir))!=NULL)
    {
        if(strcmp(e->d_name,".")==0||strcmp(e->d_name,"..")==0)continue;
        snprintf(sub,sizeof(sub),"%s/%s",path,e->d_name);
        if(remove_tree(sub)!=0)ret=-1;
    }
    closedir(dir);
    if(rmdir(path)!=0)ret=-1;
    return ret;
}
//rm [-r] [-f] path...: unlink() for files, rmdir() for empty folders
int rm_call(int argc,char *argv[])
{
    char real[PATH_MAX],cwd[PATH_MAX],name[PATH_MAX],*p;
    const char *b;
    int i,rflag=0,fflag=0,count=0,ret=0;
    size_t len;
    struct stat st;
    for(i=1;i<argc;i++)
    {
        if(argv[i][0]=='-'&&argv[i][1]!='\0')
        {
            for(p=argv[i]+1;*p;p++)
            {
                if(*p=='r'||*p=='R')rflag=1;
                else if(*p=='f')fflag=1;
                else
                {
                    fprintf(stderr,"rm: invalid option -- '%c'\n",*p);
                    return 1;
                }
            }
        }
        else count++;
    }
    if(count==0)
    {
        fprintf(stderr,"usage: rm [-r] [-f] <file | directory> ...\n");
        return 1;
    }
    for(i=1;i<argc;i++)
    {
        if(argv[i][0]=='-'&&argv[i][1]!='\0')continue;
        snprintf(name,sizeof(name),"%s",argv[i]);
        len=strlen(name);
        while(len>1&&name[len-1]=='/')name[--len]='\0';
        b=base_name(name);
        if(strcmp(b,".")==0||strcmp(b,"..")==0)
        {
            fprintf(stderr,"rm: refusing to remove '.' or '..' directory: skipping '%s'\n",argv[i]);
            ret=1;
            continue;
        }
        if(fix("rm",argv[i],real)!=0){ret=1;continue;}
        if(strcmp(real,root[0]?root:"/")==0)
        {
            fprintf(stderr,"rm: refusing to remove the root directory '/'\n");
            ret=1;
            continue;
        }
        len=strlen(real);
        if(getcwd(cwd,sizeof(cwd))!=NULL&&strncmp(cwd,real,len)==0&&(cwd[len]=='\0'||cwd[len]=='/'))
        {
            fprintf(stderr,"rm: cannot remove '%s': current directory is inside it\n",argv[i]);
            ret=1;
            continue;
        }
        if(lstat(real,&st)!=0)
        {
            if(fflag&&errno==ENOENT)continue;
            fprintf(stderr,"rm: cannot remove '%s': %s\n",argv[i],strerror(errno));
            ret=1;
            continue;
        }
        if(S_ISDIR(st.st_mode))
        {
            if((rflag?remove_tree(real):rmdir(real))!=0)
            {
                if(errno==ENOTEMPTY||errno==EEXIST)fprintf(stderr,"rm: cannot remove '%s': Directory not empty (use rm -r)\n",argv[i]);
                else fprintf(stderr,"rm: cannot remove '%s': %s\n",argv[i],strerror(errno));
                ret=1;
            }
        }
        else if(unlink(real)!=0)
        {
            fprintf(stderr,"rm: cannot remove '%s': %s\n",argv[i],strerror(errno));
            ret=1;
        }
    }
    return ret;
}
//copy one regular file with read() and write()
int copy_file(const char *src,const char *dst,const char *dname,mode_t mode)
{
    char buf[BUF_SIZE];
    int in,out;
    ssize_t n;
    in=open(src,O_RDONLY);
    if(in<0)
    {
        perror("cp: source");
        return 1;
    }
    out=open(dst,O_WRONLY|O_CREAT|O_TRUNC,mode&0777);
    if(out<0)
    {
        fprintf(stderr,"cp: cannot create '%s': %s\n",dname,strerror(errno));
        close(in);
        return 1;
    }
    while((n=read(in,buf,sizeof(buf)))>0)
    {
        if(write_all(out,buf,n)!=0)
        {
            perror("cp: write");
            break;
        }
    }
    if(n<0)perror("cp: read");
    close(in);
    close(out);
    return n!=0;
}
//cp src dst | cp src... dir
int cp_call(int argc,char *argv[])
{
    char src[PATH_MAX],dst[PATH_MAX],target[PATH_MAX+512];
    struct stat ss,ds;
    int i,isdir,ret=0;
    if(argc<3)
    {
        fprintf(stderr,"usage: cp <source> <destination>  |  cp <source>... <directory>\n");
        return 1;
    }
    if(fix("cp",argv[argc-1],dst)!=0)return 1;
    isdir=stat(dst,&ds)==0&&S_ISDIR(ds.st_mode);
    if(argc>3&&!isdir)
    {
        fprintf(stderr,"cp: target '%s' is not a directory\n",argv[argc-1]);
        return 1;
    }
    for(i=1;i<argc-1;i++)
    {
        if(fix("cp",argv[i],src)!=0){ret=1;continue;}
        if(stat(src,&ss)!=0)
        {
            fprintf(stderr,"cp: cannot stat '%s': %s\n",argv[i],strerror(errno));
            ret=1;
            continue;
        }
        if(S_ISDIR(ss.st_mode))
        {
            fprintf(stderr,"cp: -r not specified; omitting directory '%s'\n",argv[i]);
            ret=1;
            continue;
        }
        if(isdir)snprintf(target,sizeof(target),"%s/%s",dst,base_name(src));
        else snprintf(target,sizeof(target),"%s",dst);
        if(jail_check(target)==-2)
        {
            fprintf(stderr,"cp: %s: Permission denied (outside root directory)\n",argv[argc-1]);
            ret=1;
            continue;
        }
        if(stat(target,&ds)==0&&ds.st_dev==ss.st_dev&&ds.st_ino==ss.st_ino)
        {
            fprintf(stderr,"cp: '%s' and '%s' are the same file\n",argv[i],argv[argc-1]);
            ret=1;
            continue;
        }
        if(copy_file(src,target,argv[argc-1],ss.st_mode)!=0)ret=1;
    }
    return ret;
}
//mv src dst | mv src... dir: rename()
int mv_call(int argc,char *argv[])
{
    char src[PATH_MAX],dst[PATH_MAX],target[PATH_MAX+512];
    struct stat ss,ds;
    int i,isdir,ret=0;
    size_t len;
    if(argc<3)
    {
        fprintf(stderr,"usage: mv <source> <destination>  |  mv <source>... <directory>\n");
        return 1;
    }
    if(fix("mv",argv[argc-1],dst)!=0)return 1;
    isdir=stat(dst,&ds)==0&&S_ISDIR(ds.st_mode);
    if(argc>3&&!isdir)
    {
        fprintf(stderr,"mv: target '%s' is not a directory\n",argv[argc-1]);
        return 1;
    }
    for(i=1;i<argc-1;i++)
    {
        if(fix("mv",argv[i],src)!=0){ret=1;continue;}
        if(lstat(src,&ss)!=0)
        {
            fprintf(stderr,"mv: cannot stat '%s': %s\n",argv[i],strerror(errno));
            ret=1;
            continue;
        }
        if(strcmp(src,root[0]?root:"/")==0)
        {
            fprintf(stderr,"mv: cannot move the root directory\n");
            ret=1;
            continue;
        }
        if(isdir)snprintf(target,sizeof(target),"%s/%s",dst,base_name(src));
        else snprintf(target,sizeof(target),"%s",dst);
        if(jail_check(target)==-2)
        {
            fprintf(stderr,"mv: %s: Permission denied (outside root directory)\n",argv[argc-1]);
            ret=1;
            continue;
        }
        len=strlen(src);
        if(S_ISDIR(ss.st_mode)&&strncmp(target,src,len)==0&&target[len]=='/')
        {
            fprintf(stderr,"mv: cannot move '%s' to a subdirectory of itself\n",argv[i]);
            ret=1;
            continue;
        }
        if(rename(src,target)!=0)
        {
            fprintf(stderr,"mv: cannot move '%s' to '%s': %s\n",argv[i],argv[argc-1],strerror(errno));
            ret=1;
        }
    }
    return ret;
}

/* ============ 3. File Viewing & Process Inspection Commands ============ */

//read a whole small file into buf with open(), read(), close()
int read_file(const char *path,char *buf,int size)
{
    int fd,total=0;
    ssize_t n;
    fd=open(path,O_RDONLY);
    if(fd<0)return -1;
    while(total<size-1&&(n=read(fd,buf+total,size-1-total))>0)total+=n;
    close(fd);
    buf[total]='\0';
    return total;
}
int cat_fd(int fd,const char *name)
{
    char buf[BUF_SIZE];
    ssize_t n;
    while((n=read(fd,buf,sizeof(buf)))>0)
        if(write_all(STDOUT_FILENO,buf,n)!=0)return 1;
    if(n<0)
    {
        fprintf(stderr,"cat: %s: %s\n",name,strerror(errno));
        return 1;
    }
    return 0;
}
//cat [file...]: open(), read(), write(); no file (or "-") reads standard input
int cat_call(int argc,char *argv[])
{
    char real[PATH_MAX];
    int i,fd,ret=0;
    if(argc<2)return cat_fd(STDIN_FILENO,"stdin");
    for(i=1;i<argc;i++)
    {
        if(strcmp(argv[i],"-")==0)
        {
            ret|=cat_fd(STDIN_FILENO,"stdin");
            continue;
        }
        if(fix("cat",argv[i],real)!=0){ret=1;continue;}
        fd=open(real,O_RDONLY);
        if(fd<0)
        {
            fprintf(stderr,"cat: %s: %s\n",argv[i],strerror(errno));
            ret=1;
            continue;
        }
        ret|=cat_fd(fd,argv[i]);
        close(fd);
    }
    return ret;
}
//echo [-n] text...: builtin ($VAR, ${VAR} and $? are expanded by the parser)
int echo_call(int argc,char *argv[])
{
    int i=1,newline=1;
    if(argc>1&&strcmp(argv[1],"-n")==0){newline=0;i=2;}
    for(;i<argc;i++)
    {
        fputs(argv[i],stdout);
        if(i<argc-1)putchar(' ');
    }
    if(newline)putchar('\n');
    fflush(stdout);
    return 0;
}
//uptime: /proc/uptime and /proc/loadavg
int uptime_call(int argc,char *argv[])
{
    char buf[256];
    double up,load1,load5,load15;
    long total;
    if(read_file("/proc/uptime",buf,sizeof(buf))<0||sscanf(buf,"%lf",&up)!=1)
    {
        perror("uptime: /proc/uptime");
        return 1;
    }
    if(read_file("/proc/loadavg",buf,sizeof(buf))<0||sscanf(buf,"%lf %lf %lf",&load1,&load5,&load15)!=3)
    {
        perror("uptime: /proc/loadavg");
        return 1;
    }
    total=(long)up;
    printf("Uptime       : %ld days, %ld hours, %ld minutes, %ld seconds\n",total/86400,(total%86400)/3600,(total%3600)/60,total%60);
    printf("Load average : %.2f (1 min), %.2f (5 min), %.2f (15 min)\n",load1,load5,load15);
    return 0;
}
//meminfo: parse /proc/meminfo
int meminfo_call(int argc,char *argv[])
{
    char buf[8192],name[64],*line,*save;
    char *wanted[]={"MemTotal","MemFree","MemAvailable","Buffers","Cached","SwapTotal","SwapFree"};
    long value,total=-1,avail=-1;
    int i;
    if(read_file("/proc/meminfo",buf,sizeof(buf))<0)
    {
        perror("meminfo: /proc/meminfo");
        return 1;
    }
    for(line=strtok_r(buf,"\n",&save);line!=NULL;line=strtok_r(NULL,"\n",&save))
    {
        if(sscanf(line,"%63[^:]: %ld",name,&value)!=2)continue;    //"MemTotal:   16314772 kB"
        if(strcmp(name,"MemTotal")==0)total=value;
        if(strcmp(name,"MemAvailable")==0)avail=value;
        for(i=0;i<7;i++)
        {
            if(strcmp(name,wanted[i])==0)
            {
                printf("%-13s: %10ld kB  (%ld MB)\n",name,value,value/1024);
                break;
            }
        }
    }
    if(total>=0&&avail>=0)printf("%-13s: %10ld kB  (%ld MB)\n","MemUsed",total-avail,(total-avail)/1024);
    return 0;
}

/* ============ Built-in commands (run inside the shell process) ============ */

//cd [dir | - | ~ | /]: chdir(), never above root
int cd_call(int argc,char *argv[])
{
    char real[PATH_MAX],old[PATH_MAX],back[PATH_MAX];
    const char *target;
    struct stat st;
    int dash=0;
    if(argc>2)
    {
        fprintf(stderr,"cd: too many arguments\n");
        return 1;
    }
    target=argc>1?argv[1]:"/";
    if(strcmp(target,"-")==0)
    {
        snprintf(back,sizeof(back),"%s",virt(prevdir));
        target=back;
        dash=1;
    }
    if(fix("cd",target,real)!=0)return 1;
    if(stat(real,&st)!=0)
    {
        fprintf(stderr,"cd: %s: %s\n",target,strerror(errno));
        return 1;
    }
    if(!S_ISDIR(st.st_mode))
    {
        fprintf(stderr,"cd: %s: Not a directory\n",target);
        return 1;
    }
    if(getcwd(old,sizeof(old))==NULL)old[0]='\0';
    if(chdir(real)!=0)
    {
        fprintf(stderr,"cd: %s: %s\n",target,strerror(errno));
        return 1;
    }
    if(old[0])strcpy(prevdir,old);
    if(getcwd(real,sizeof(real))!=NULL)
    {
        setenv("PWD",show(real),1);
        if(dash)printf("%s\n",show(real));
    }
    return 0;
}
//exit [n]: leave the shell with status n (default: status of the last command)
int exit_call(int argc,char *argv[])
{
    char *end;
    long code=last_status;
    if(argc>2)
    {
        fprintf(stderr,"exit: too many arguments\n");
        return 1;
    }
    if(argc==2)
    {
        code=strtol(argv[1],&end,10);
        if(argv[1][0]=='\0'||*end!='\0')
        {
            fprintf(stderr,"exit: %s: numeric argument required\n",argv[1]);
            code=2;
        }
    }
    fflush(stdout);
    exit((int)(code&255));
}
int help_call(int argc,char *argv[])
{
    printf("myshell - custom shell. \"/\" is the folder where myshell was started;\n");
    printf("no command can reach a path above it (\"cd ..\" at \"/\" is blocked).\n\n");
    printf("Built-in commands (run inside the shell process):\n");
    printf("  cd [dir|-|~|/]      change directory (no argument = root)\n");
    printf("  echo [-n] text      print text ($VAR, ${VAR}, $? are expanded)\n");
    printf("  exit [n]            leave the shell with status n\n");
    printf("  help                show this help\n");
    printf("  history [n]         show history; !! repeats last, !N repeats number N\n\n");
    printf("Commands run in a child process (fork + waitpid):\n");
    printf("  pwd                 print current directory\n");
    printf("  ls [-a] [-l] [path] list a directory\n");
    printf("  whoami              print user name\n");
    printf("  uname [-asnrvm]     print system information\n");
    printf("  mkdir [-p] dir...   create directories (mode 0755)\n");
    printf("  touch file...       create file / update its time\n");
    printf("  rm [-r] [-f] path.. remove files or empty directories (-r: everything)\n");
    printf("  cp src dst          copy a file (dst may be a directory)\n");
    printf("  mv src dst          move / rename (dst may be a directory)\n");
    printf("  cat [file...]       print files (no file: standard input)\n");
    printf("  uptime              system uptime and load average\n");
    printf("  meminfo             memory statistics\n");
    printf("Any other command is run with fork + execvp.\n\n");
    printf("Operators: cmd1 | cmd2   cmd > file   cmd >> file   cmd < file   cmd &\n");
    printf("Quotes: \"text with $VAR\"  'literal text'  \\x escapes one character\n");
    return 0;
}
void hist_add(const char *line)
{
    int k=hist_total%HIST_SIZE;
    free(history[k]);
    history[k]=strdup(line);
    hist_total++;
}
//history entry number num (1 based), NULL if it does not exist
const char *hist_get(long num)
{
    if(num<1||num>hist_total||num<=hist_total-HIST_SIZE)return NULL;
    return history[(num-1)%HIST_SIZE];
}
//history [n]: list the last commands
int history_call(int argc,char *argv[])
{
    int i,start=hist_total>HIST_SIZE?hist_total-HIST_SIZE:0;
    long n;
    char *end;
    if(argc>2)
    {
        fprintf(stderr,"history: too many arguments\n");
        return 1;
    }
    if(argc==2)
    {
        n=strtol(argv[1],&end,10);
        if(argv[1][0]=='\0'||*end!='\0'||n<0)
        {
            fprintf(stderr,"history: %s: numeric argument required\n",argv[1]);
            return 1;
        }
        if(hist_total-n>start)start=hist_total-n;
    }
    for(i=start;i<hist_total;i++)printf("%5d  %s\n",i+1,history[i%HIST_SIZE]);
    return 0;
}

typedef struct {const char *name;int (*fn)(int,char **);int builtin;} Command;
Command commands[]={
    {"cd",cd_call,1},{"echo",echo_call,1},{"exit",exit_call,1},{"help",help_call,1},{"history",history_call,1},
    {"pwd",pwd_call,0},{"ls",ls_call,0},{"whoami",whoami_call,0},{"uname",uname_call,0},
    {"mkdir",mkdir_call,0},{"touch",touch_call,0},{"rm",rm_call,0},{"cp",cp_call,0},{"mv",mv_call,0},
    {"cat",cat_call,0},{"uptime",uptime_call,0},{"meminfo",meminfo_call,0},
    {NULL,NULL,0}
};
Command *find_command(const char *name)
{
    int i;
    for(i=0;commands[i].name!=NULL;i++)
        if(strcmp(commands[i].name,name)==0)return &commands[i];
    return NULL;
}

/* ============ Command parsing ============ */

enum {T_WORD,T_PIPE,T_IN,T_OUT,T_APPEND,T_BG};
typedef struct {int type;char *word;} Token;
typedef struct {char *argv[MAX_ARGS];int argc;char *in,*out;int append;} Cmd;
char words[WORD_BUF];       //storage for the words of the current line

int add_char(char **wp,char c)
{
    if(*wp>=words+WORD_BUF-1)
    {
        fprintf(stderr,"myshell: command line too long\n");
        return -1;
    }
    *(*wp)++=c;
    return 0;
}
//$NAME, ${NAME}, $? or $$ at *pp: append its value to the word
int expand_var(const char **pp,char **wp)
{
    const char *p=*pp+1,*val;
    char name[256],num[32];
    int k=0;
    if(*p=='?'){snprintf(num,sizeof(num),"%d",last_status);val=num;p++;}
    else if(*p=='$'){snprintf(num,sizeof(num),"%d",(int)getpid());val=num;p++;}
    else if(*p=='{')
    {
        p++;
        while(*p&&*p!='}'&&k<255)name[k++]=*p++;
        if(*p!='}'||k==0)
        {
            fprintf(stderr,"myshell: bad substitution\n");
            return -1;
        }
        p++;
        name[k]='\0';
        if(strcmp(name,"?")==0){snprintf(num,sizeof(num),"%d",last_status);val=num;}
        else val=getenv(name);
    }
    else if(isalpha((unsigned char)*p)||*p=='_')
    {
        while((isalnum((unsigned char)*p)||*p=='_')&&k<255)name[k++]=*p++;
        name[k]='\0';
        val=getenv(name);
    }
    else val="$";               //a lone "$" stays as it is
    *pp=p;
    if(val==NULL)val="";
    for(;*val;val++)if(add_char(wp,*val))return -1;
    return 0;
}
//split a line into words and the operators | < > >> &
int tokenize(const char *p,Token *tok,int *ntok)
{
    char *wp=words,*start;
    int n=0,quoted;
    while(1)
    {
        while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
        if(*p=='\0'||*p=='#')break;                 //end of line or comment
        if(n>=MAX_TOKENS)
        {
            fprintf(stderr,"myshell: too many arguments\n");
            return -1;
        }
        if(*p=='|'){tok[n++].type=T_PIPE;p++;continue;}
        if(*p=='<'){tok[n++].type=T_IN;p++;continue;}
        if(*p=='&'){tok[n++].type=T_BG;p++;continue;}
        if(*p=='>')
        {
            if(p[1]=='>'){tok[n++].type=T_APPEND;p+=2;}
            else{tok[n++].type=T_OUT;p++;}
            continue;
        }
        start=wp;
        quoted=0;
        while(*p&&!strchr(" \t\r\n|<>&",*p))
        {
            if(*p=='\'')            //'single quotes': everything literal
            {
                quoted=1;
                p++;
                while(*p&&*p!='\'')if(add_char(&wp,*p++))return -1;
                if(*p!='\'')
                {
                    fprintf(stderr,"myshell: syntax error: unterminated quote\n");
                    return -1;
                }
                p++;
            }
            else if(*p=='"')        //"double quotes": only $ and \ are special
            {
                quoted=1;
                p++;
                while(*p&&*p!='"')
                {
                    if(*p=='\\'&&p[1]&&strchr("\"\\$",p[1])){p++;if(add_char(&wp,*p++))return -1;}
                    else if(*p=='$'){if(expand_var(&p,&wp))return -1;}
                    else if(add_char(&wp,*p++))return -1;
                }
                if(*p!='"')
                {
                    fprintf(stderr,"myshell: syntax error: unterminated quote\n");
                    return -1;
                }
                p++;
            }
            else if(*p=='\\')
            {
                p++;
                if(*p&&add_char(&wp,*p++))return -1;
            }
            else if(*p=='$'){if(expand_var(&p,&wp))return -1;}
            else if(add_char(&wp,*p++))return -1;
        }
        if(wp==start&&!quoted)continue;            //unset $VAR expands to nothing
        if(add_char(&wp,'\0'))return -1;
        tok[n].type=T_WORD;
        tok[n++].word=start;
    }
    *ntok=n;
    return 0;
}
const char *token_name(Token *tok,int i,int ntok)
{
    const char *names[]={"word","|","<",">",">>","&"};
    return i>=ntok?"newline":names[tok[i].type];
}
//group the tokens into commands connected by pipes
int parse(Token *tok,int ntok,Cmd *cmd,int *ncmd,int *bg)
{
    int i,n=0;
    Cmd *c=&cmd[0];
    *bg=0;
    *ncmd=0;
    memset(c,0,sizeof(Cmd));
    for(i=0;i<ntok;i++)
    {
        switch(tok[i].type)
        {
        case T_WORD:
            if(c->argc>=MAX_ARGS-1)
            {
                fprintf(stderr,"myshell: too many arguments (max %d)\n",MAX_ARGS-1);
                return -1;
            }
            c->argv[c->argc++]=tok[i].word;
            break;
        case T_PIPE:
            if(c->argc==0)
            {
                fprintf(stderr,"myshell: syntax error near unexpected token `|'\n");
                return -1;
            }
            if(n+1>=MAX_CMDS)
            {
                fprintf(stderr,"myshell: too many pipes (max %d commands)\n",MAX_CMDS);
                return -1;
            }
            c=&cmd[++n];
            memset(c,0,sizeof(Cmd));
            break;
        case T_BG:
            if(i!=ntok-1||c->argc==0)
            {
                fprintf(stderr,"myshell: syntax error near unexpected token `&'\n");
                return -1;
            }
            *bg=1;
            break;
        default:        //< > >>
            if(i+1>=ntok||tok[i+1].type!=T_WORD)
            {
                fprintf(stderr,"myshell: syntax error near unexpected token `%s'\n",token_name(tok,i+1,ntok));
                return -1;
            }
            if(tok[i].type==T_IN)c->in=tok[i+1].word;
            else
            {
                c->out=tok[i+1].word;
                c->append=tok[i].type==T_APPEND;
            }
            i++;
        }
    }
    if(c->argc==0)
    {
        if(n==0&&c->in==NULL&&c->out==NULL)return 0;       //empty line
        fprintf(stderr,"myshell: syntax error: missing command\n");
        return -1;
    }
    for(i=0;i<=n;i++)cmd[i].argv[cmd[i].argc]=NULL;
    *ncmd=n+1;
    return 0;
}

/* ============ Process control ============ */

//< > >>: open() the file inside root and dup2() it onto stdin / stdout
int redirect(Cmd *c)
{
    char real[PATH_MAX];
    int fd;
    if(c->in)
    {
        if(fix("myshell",c->in,real)!=0)return -1;
        fd=open(real,O_RDONLY);
        if(fd<0)
        {
            fprintf(stderr,"myshell: %s: %s\n",c->in,strerror(errno));
            return -1;
        }
        dup2(fd,STDIN_FILENO);
        close(fd);
    }
    if(c->out)
    {
        if(fix("myshell",c->out,real)!=0)return -1;
        fd=open(real,O_WRONLY|O_CREAT|(c->append?O_APPEND:O_TRUNC),0644);
        if(fd<0)
        {
            fprintf(stderr,"myshell: %s: %s\n",c->out,strerror(errno));
            return -1;
        }
        dup2(fd,STDOUT_FILENO);
        close(fd);
    }
    return 0;
}
//external programs: block path arguments outside root, map "/x" and "~/x" to the real path
int jail_args(char *argv[])
{
    static char conv[MAX_ARGS][PATH_MAX];
    char *a;
    int i;
    if(strchr(argv[0],'/')&&argv[0][0]!='/'&&fix("myshell",argv[0],conv[0])!=0)return -1;
    for(i=1;argv[i]!=NULL;i++)
    {
        a=argv[i];
        if(a[0]=='-')continue;
        if(a[0]=='/'||a[0]=='~'||strcmp(a,"..")==0||strncmp(a,"../",3)==0||strstr(a,"/..")!=NULL)
        {
            if(fix(argv[0],a,conv[i])!=0)return -1;
            if(a[0]=='/'||a[0]=='~')argv[i]=conv[i];
        }
    }
    return 0;
}
//inside a child: run our own command, or exec the program
void child_run(Cmd *c)
{
    Command *b=find_command(c->argv[0]);
    int r;
    if(redirect(c)!=0)_exit(1);
    if(b!=NULL)
    {
        r=b->fn(c->argc,c->argv);
        fflush(stdout);
        _exit(r);
    }
    if(jail_args(c->argv)!=0)_exit(1);
    execvp(c->argv[0],c->argv);
    if(errno==ENOENT)fprintf(stderr,"%s: command not found\n",c->argv[0]);
    else fprintf(stderr,"%s: %s\n",c->argv[0],strerror(errno));
    _exit(errno==ENOENT?127:126);
}
int exit_code(int status)
{
    if(WIFEXITED(status))return WEXITSTATUS(status);
    if(WIFSIGNALED(status))return 128+WTERMSIG(status);
    return 1;
}
//run cmd[0] | cmd[1] | ... with pipe(), fork(), dup2(), waitpid()
int execute(Cmd *cmd,int n,int bg)
{
    pid_t pids[MAX_CMDS],pid;
    int i,j,pfd[2],prev=-1,status=0,saved_in,saved_out,r=1,fd;
    Command *b=find_command(cmd[0].argv[0]);
    if(n==1&&!bg&&b!=NULL&&b->builtin)       //builtin: no child process
    {
        fflush(stdout);
        saved_in=dup(STDIN_FILENO);
        saved_out=dup(STDOUT_FILENO);
        if(redirect(&cmd[0])==0)r=b->fn(cmd[0].argc,cmd[0].argv);
        fflush(stdout);
        dup2(saved_in,STDIN_FILENO);
        dup2(saved_out,STDOUT_FILENO);
        close(saved_in);
        close(saved_out);
        return r;
    }
    for(i=0;i<n;i++)
    {
        if(i<n-1&&pipe(pfd)!=0)
        {
            perror("myshell: pipe");
            break;
        }
        fflush(stdout);
        pid=fork();
        if(pid<0)
        {
            perror("myshell: fork");
            if(i<n-1){close(pfd[0]);close(pfd[1]);}
            break;
        }
        if(pid==0)
        {
            signal(SIGINT,bg?SIG_IGN:SIG_DFL);
            if(prev!=-1)
            {
                dup2(prev,STDIN_FILENO);
                close(prev);
            }
            else if(bg&&(fd=open("/dev/null",O_RDONLY))>=0)   //background jobs never read the terminal
            {
                dup2(fd,STDIN_FILENO);
                close(fd);
            }
            if(i<n-1)
            {
                close(pfd[0]);
                dup2(pfd[1],STDOUT_FILENO);
                close(pfd[1]);
            }
            child_run(&cmd[i]);
        }
        pids[i]=pid;
        if(prev!=-1){close(prev);prev=-1;}
        if(i<n-1)
        {
            close(pfd[1]);
            prev=pfd[0];
        }
    }
    if(prev!=-1)close(prev);
    if(i==0)return 1;
    if(bg)
    {
        for(j=0;j<MAX_JOBS&&jobs[j].pid!=0;j++);
        if(j<MAX_JOBS)
        {
            jobs[j].pid=pids[i-1];
            jobs[j].num=++job_count;
            snprintf(jobs[j].name,sizeof(jobs[j].name),"%s",cmd[0].argv[0]);
        }
        printf("[%d] %d\n",job_count,(int)pids[i-1]);
        return 0;
    }
    for(j=0;j<i;j++)
    {
        while(waitpid(pids[j],&status,0)<0&&errno==EINTR);
        if(j==i-1)r=exit_code(status);
    }
    return i<n?1:r;
}
//collect finished background jobs: waitpid() with WNOHANG
void reap_jobs(void)
{
    pid_t pid;
    int status,j;
    while((pid=waitpid(-1,&status,WNOHANG))>0)
    {
        for(j=0;j<MAX_JOBS;j++)
        {
            if(jobs[j].pid!=pid)continue;
            printf("[%d] Done (status %d)    %s\n",jobs[j].num,exit_code(status),jobs[j].name);
            jobs[j].pid=0;
            break;
        }
    }
    fflush(stdout);
}
//a line starting with !! or !N is replaced by that history entry
int hist_expand(char *line)
{
    char tmp[MAX_LINE];
    const char *h;
    char *p=line,*rest;
    long num;
    while(*p==' '||*p=='\t')p++;
    if(p[0]!='!')return 0;
    if(p[1]=='!'){h=hist_get(hist_total);rest=p+2;}
    else if(isdigit((unsigned char)p[1])){num=strtol(p+1,&rest,10);h=hist_get(num);}
    else return 0;
    if(h==NULL)
    {
        fprintf(stderr,"myshell: %.*s: event not found\n",(int)(rest-p),p);
        return -1;
    }
    if(strlen(h)+strlen(rest)>=MAX_LINE)
    {
        fprintf(stderr,"myshell: command line too long\n");
        return -1;
    }
    snprintf(tmp,sizeof(tmp),"%s%s",h,rest);
    strcpy(line,tmp);
    printf("%s\n",line);        //show the command that will run
    return 1;
}
void on_sigint(int sig)
{
    ssize_t r=write(STDOUT_FILENO,"\n",1);
    (void)r;
    (void)sig;
}

/* ============ Main loop ============ */

int main(void)
{
    char line[MAX_LINE],cwd[PATH_MAX];
    Token tok[MAX_TOKENS];
    Cmd cmd[MAX_CMDS];
    int ntok,ncmd,bg,ch,interactive=isatty(STDIN_FILENO);
    struct sigaction sa;
    if(getcwd(root,sizeof(root))==NULL)
    {
        perror("myshell: getcwd");
        return 1;
    }
    strcpy(prevdir,root);
    if(strcmp(root,"/")==0)root[0]='\0';
    rootlen=strlen(root);
    setenv("PWD",show(rootlen?root:"/"),1);
    memset(&sa,0,sizeof(sa));
    sa.sa_handler=on_sigint;        //Ctrl+C does not kill the shell, only the running command
    sigaction(SIGINT,&sa,NULL);
    while(1)
    {
        reap_jobs();
        if(interactive)
        {
            printf("myshell:%s> ",getcwd(cwd,sizeof(cwd))?show(cwd):"?");
            fflush(stdout);
        }
        errno=0;
        if(fgets(line,sizeof(line),stdin)==NULL)
        {
            if(errno==EINTR){clearerr(stdin);continue;}
            if(interactive)printf("exit\n");
            break;
        }
        if(strchr(line,'\n')==NULL&&!feof(stdin))
        {
            while((ch=getchar())!='\n'&&ch!=EOF);
            fprintf(stderr,"myshell: input line too long (max %d characters)\n",MAX_LINE-2);
            last_status=1;
            continue;
        }
        line[strcspn(line,"\r\n")]='\0';
        if(hist_expand(line)<0){last_status=1;continue;}
        if(line[strspn(line," \t")]!='\0')hist_add(line);
        if(tokenize(line,tok,&ntok)!=0||parse(tok,ntok,cmd,&ncmd,&bg)!=0)
        {
            last_status=2;
            continue;
        }
        if(ncmd==0)continue;
        last_status=execute(cmd,ncmd,bg);
    }
    return last_status;
}
