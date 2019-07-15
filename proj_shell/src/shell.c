/**
 *  shell.c is...
 * 
 *  @ auhtor HyungSoon Kim
 *  @ since 2019-03-21
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

const int ARRAY_SIZE = 20;

int GetInput(char **result,FILE *stream);
void SetInput(char *result);
void ParseCommand(char **input, char **command);
int ForkExecWait(char **result);
int BatchMode(char *filename);

int main(int argc, char *argv[]){

    char *result;
    int i = 0,len = 0, check = 0;
    
    // batch mode
    if(argc == 2) {
        BatchMode(argv[1]);
        return 0;
    } 

    // Interactive mode
    while (1) {
        printf("prompt > ");
        
        // get input from stdin
        check = GetInput(&result, stdin);

        // if getting input was not successful, cancel input command
        if (check < 0) {
            printf("memory allocation error, type command again\n");
            continue;
        }

        // if ctrl + d is entered, GetInput will return 2
        if (check == 2) {
            printf("Ctrl+D exit\n");
            free(result);
            return 0;
        }

        // SetInput will remove \n from the input
        SetInput(result);
        
        // ForkExecWait will fork children processes, 
        // exec programs, and wait for all children
        // if first command is quit, the function will return 2
        if (ForkExecWait(&result) == 2) {
            printf("quit exit\n");
            free(result);
            return 0;
        }
        
        // reset input data for case if the memory being allocated again
        len =strlen(result);
        for (i = 0; i < len; i++) {
            result[i] = '\0';
        }
    }            
}

/**  function for getting input from stream
 *   @param[out] **result : *result is pointer to string of a line of input data
 *   @param[in]  *stream : input stream name
 *   @return     0 for normal action, 2 for meets EOF, -1 for allocation error
 */
int GetInput(char **result, FILE *stream){
    int total_size = 0, length = 0;
    char input[ARRAY_SIZE];

    // allocated a memory for input
    if ((*result = (char *)malloc(sizeof(char))) == NULL) {
        printf("malloc error");
        return -1;
    }    
    do {
        // get input by one line and store to temporary buffer
        fgets(input,ARRAY_SIZE,stream);

        // if stream Meets EOF, stop getting inputs
        if (feof(stream) != 0) {
            return 2;
        }
        length = strlen(input);
        total_size += length;
        
        // reallocate memory of string
        if ((*result = (char *)realloc(*result,total_size*sizeof(char))) == NULL) {
            printf("realloc error");
            return -1;
        }
        // copy data in temporary buffer to string
        strcat(*result, input);
        
        // get input until there is no left in stdin buffer
    } while (input[ARRAY_SIZE-2] != '\n' && length == ARRAY_SIZE-1);
    return 0;
}

/**  function for deleting \n from input
 *   @param[in, out]  *result : string to remove \n
 */

void SetInput(char *result){
    int i = 0;

    // delete \n at the end of the line
    while (result[i] != '\n') {
            i++;
    }
    result[i] = '\0';

}

/**  function for parse input with space so it derive command and options
 *   @param[in]  **input : *input is pointer to string of command and options
 *   @param[out] **command : array of string with command and options
 */
void ParseCommand(char **input, char **command){
    int i = 0;
    
    command[0] = strtok(*input, " ");
    while ((command[++i] = strtok(NULL, " ")) !=NULL) continue;
    command[++i] = NULL;
}

/**
 *   function for fork, execute, wait for children processes
 *
 *   @param[in]  result : input string with commands distinguished by ';'
 *   @return     0 for normal actions, 2 for quit command, -1 for excvp error
 */
int ForkExecWait(char **result){
    int pid = 0, status = 0, i = 0;
    char *temp;
    char *(command[20]);
    
    temp = strtok(*result, ";");
    //make process while there is no elements after ';'
    do {
        i++;
        // fork child process
        // if pid <0 fork() was not succesful and doesn't execute child code
        if ((pid = fork()) < 0){
            printf("fork error\n");
        } else if (pid == 0) { 
            //child will exec program with input command

            // parse the command with spaces
            ParseCommand(&temp,command);

            // child process will return 2  if first command is quit
            if ((strcmp(command[0],"quit") == 0 || strcmp(command[0],"Quit") == 0) && i == 1) {
                exit(2);
            } else if (execvp(command[0],command) < 0) {
                //the case if execvp was not successful
                printf("execvp for command %s error\n",command[0]);
                exit(-2);
            }
        }

    } while ((temp = strtok(NULL, ";")) != NULL && pid != 0);
    
    // wait until all children processes end
    // wait will return -1 if there is no child process left
    while(wait(&status) >= 0);
    // if command was quit, child process return 2
    if (WEXITSTATUS(status) == 2) {
        return 2;
    }
    
    // if execvp was error
    if (WEXITSTATUS(status) == -2) {
        return -2;
    }

    return 0;
}

/**  function for BatchMode
 *   @param[in]  *filename : file name for file to read commads
 *   @return     0 for normal actions, 2 for quit command, -1 for fopen error
 */

int BatchMode(char *filename){
    int i=0, len=0;
    char *result;
    FILE *fp = fopen(filename, "r");
    

    // the case can't open file
    if (fp == NULL) {
        printf("opening file error\n");
        return -1;
    }
    
    // get input from the file line by line until meets EOF or error
    while ((GetInput(&result,fp) == 0)) {
        
        
        //delete \n from input
        SetInput(result);

        // if command was quit
        
        if (ForkExecWait(&result) == 2) {
            free(result);
            fclose(fp);
            return 2;
        }

        // reset input data for case if the memory being allocated again
        len =strlen(result);
        for (i = 0; i < len; i++) {
            result[i] = '\0';
        }

    }
    free(result);
    fclose(fp);
    return 0;
}

