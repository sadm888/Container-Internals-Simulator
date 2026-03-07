#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "container.h"
#include "logger.h"

static void print_banner(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║         Container Internals Simulator                ║\n");
    printf("║         Linux Primitives  —  No Docker               ║\n");
    printf("║         Syscalls: clone()  kill()  waitpid()         ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

int main(void) {
    print_banner();
    printf("Commands: create  list  stop <id>  exit\n\n");
    log_event("=== simulator started ===");
    char line[256];
    while(1){
        printf("container-sim> ");
        fflush(stdout);
        if(!fgets(line,sizeof(line),stdin)){
            printf("\n");
            break;
        }
        line[strcspn(line,"\n")]='\0';
        if (strcmp(line,"create")==0){
            container_create();
        }
        else if(strcmp(line, "list")==0){
            container_list();
        } 
        else if(strncmp(line,"stop ",5)==0){
            int id=atoi(line + 5);
            if(id<=0){
                printf("[error] usage: stop <id>\n\n");
            }
            else{
                container_stop(id);
            }      
        } 
        else if(strcmp(line, "exit")==0){
            cleanup_all_containers();
            log_event("=== simulator stopped ===");
            printf("bye.\n");
            break;
        } 
        else if (line[0] != '\0'){
            printf("[error] unknown command: '%s'\n\n", line);
        }
    }
    return 0;
}