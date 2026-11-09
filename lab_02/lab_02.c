#include<stdio.h>
#include<stdlib.h> //exit
#include<sys/types.h> //p_type
#include<sys/stat.h> //chmod
#include<sys/wait.h> //wait()
#include <unistd.h> //system call
#include<string.h>
#define MAX_INPUT 100
#define MAX_ARGS 50

void printShell()
{
    printf("LAB 02 SHELL\n");
}

void printdir()
{
    char cwd[100];
    if(getcwd(cwd,sizeof(cwd))!=NULL)
    {
        printf("Current Working Directory:%s\n",cwd);
    }
    else
    {
    perror("getcwd Failed");
    return;

}
}

void shellinput(char *input)
{
    printf("\n>>> ");
    if(fgets(input,MAX_INPUT,stdin)==NULL)
    {
        perror("Input Failed. Existing the shell");
        exit(1);
    }
    input[strcspn(input, "\n")]= 0;//removing training newline
}
 
void parseinput(char *input, char *args[])
{
    int i=0;
    args[i]=strtok(input, " ");//space besed tokenizing
    while(args[i]!=NULL && i<MAX_ARGS-1) //splitting until null or max args reached
    {
        i++;
        args[i]=strtok(NULL, " ");
    }
    args[i]=NULL;
}
int builtin(char *args[])
{
    if(args[0]==NULL)
    {
        perror("No Command Found");
        return 1;
    }
    if(strcmp(args[0],"exit")==0)
    {
        printf("Exiting the shell\n"); //exit command
        exit(0);
    }
    if(strcmp(args[0], "cd")==0)
    {
        if(args[1]==NULL)
        {
            printf("No directory specified\n");
        }
        else
        {
            if(chdir(args[1])!=0)
            {
                perror("chdir failed");
            }
        }
        return 1;
    }
    //now the chmod command handling 
    if(strcmp(args[0], "chmod")==0)
    {
        if(args[1]==NULL || args[2]==NULL)
        {
            printf("for the usage . use help command\n");
            return 1;
        }
            char *end;
            long mode =strtol(args[1], &end, 8); //converting string to long in base 8
            if(*end != '\0' || mode <0 || mode >0777) //front 0 because of octal mode
            {
                printf("Invalid NUmbers . use 3 digits from 0-777");
                return 1;
            }
            if(chmod(args[2],(mode_t)mode)!=0)
            {
                perror("chmod failed");
            }
            else
            {
                printf("Permission of %s changed to %03lo\n",args[2],mode);
            }
            return 1;
        }
        if(strcmp(args[0],"help")==0){
            printf("This is help menu\n");
            printf("Commands are : 1.cd 2.exit 3.help 4.chmod permission_mode filename " );
            printf("Support Pipes: ls | grep txt \n");
            return 1;
        }
        return 0;
    }
    //executing a built in comand
    void executecommand(char **args)
    {
        pid_t pid= fork();
        if(pid ==0 )
        {
            if(execvp(args[0],args)<0)
            {
                printf("command failed\n");
            }
            exit(1);
        }
        else
        {
            wait(NULL); //parent waits for child to execute completely
        }
    }

    void executePiped(char **args1, char **args2) {
    int pipefd[2];
    pipe(pipefd);  // Create pipe

    pid_t p1 = fork();

    if (p1 == 0) {
        // First child process (left command)
        dup2(pipefd[1], STDOUT_FILENO); // Redirect stdout to pipe write end
        close(pipefd[0]); // Close unused read end
        close(pipefd[1]);

        execvp(args1[0], args1); // Execute first command
        perror("Pipe cmd1 failed");
        exit(1);
    }

    pid_t p2 = fork();

    if (p2 == 0) {
        // Second child process (right command)
        dup2(pipefd[0], STDIN_FILENO); // Redirect stdin to pipe read end
        close(pipefd[1]); // Close unused write end
        close(pipefd[0]);

        execvp(args2[0], args2); // Execute second command
        perror("Pipe cmd2 failed");
        exit(1);
    }

    // Parent process closes both ends of pipe
    close(pipefd[0]);
    close(pipefd[1]);

    // Wait for both child processes to complete
    wait(NULL);	
    wait(NULL);
}

int parsepipe(char *input, char **left, char **right)
{
    char *pipepos =strchr(input, '|');
    if(!pipepos)return 0;
    *pipepos ='\0';
    pipepos++;
    while(*pipepos==' ')pipepos++;//skipping the leading spaces 
    *left=input;
    *right=pipepos;
    return 1; //pipe exists
}

int main() {
    char input[MAX_INPUT];
    char *args1[MAX_ARGS], *args2[MAX_ARGS];

        printShell();

    while (1) {
        printdir();        // Show current directory
        shellinput(input);  // Get user input

        if (strlen(input) == 0) continue; // Ignore empty input

        char *left, *right;

        // Check if input contains a pipe
        if (parsepipe(input, &left, &right)) {
            parseinput(left, args1);   // Parse left command
            parseinput(right, args2);  // Parse right command

            executePiped(args1, args2); // Execute piped commands
        } else {
            parseinput(input, args1); // Parse normal command

            // Handle built-in commands first
            if (builtin(args1)) continue;

            executecommand(args1); // Execute external command
        }
    }

    return 0;
}
