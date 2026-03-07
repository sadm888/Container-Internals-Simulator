#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sched.h>
#include "container.h"
#include "logger.h"

#define STACK_SIZE (1024 * 1024)

static Container *head=NULL;  
static Container *tail=NULL;  
static int next_id=1;

static int container_child(void *arg){
    (void)arg;
    while(1) {
        sleep(1);
    }
    return 0;
}

//checks if any running containers have exited on their own
static void poll_states(void) {
    for (Container *c=head;c!=NULL; c=c->next){
        if (c->state==STATE_RUNNING){
            if(waitpid(c->pid,NULL,WNOHANG)>0) {
                c->state=STATE_STOPPED;
                strcpy(c->status,"Stopped");
                log_event("container-%d exited on its own (pid %d)",c->id,c->pid);
            }
        }
    }
}

int container_create(void){
    Container *c=malloc(sizeof(Container));
    if(!c){
        printf("[error] out of memory\n\n");
        return -1;
    }
    c->stack=malloc(STACK_SIZE);
    if(!c->stack){
        printf("[error] out of memory\n\n");
        free(c);
        return -1;
    }
    char *stack_top=c->stack+STACK_SIZE;
    pid_t pid=clone(container_child,stack_top,SIGCHLD,NULL);
    if(pid<0){
        printf("[error] clone() failed — run with sudo\n\n");
        free(c->stack);
        free(c);
        return -1;
    }
    c->id=next_id++;
    c->pid=pid;
    c->state=STATE_RUNNING;
    c->next=NULL;
    strcpy(c->status,"Running");
    // append to end of list which is top of stack(+memory space)
    if(tail==NULL){
        head=tail=c;
    } 
    else{
        tail->next=c;
        tail=c;
    }

    printf("\n");
    printf("  ┌─────────────────────────────────┐\n");
    printf("  │  container-%d — started          │\n",c->id);
    printf("  ├─────────────────────────────────┤\n");
    printf("  │  id     : %-4d                  │\n",c->id);
    printf("  │  pid    : %-6d                │\n",pid);
    printf("  │  status : Running               │\n");
    printf("  └─────────────────────────────────┘\n\n");
    log_event("container-%d STARTED (pid %d)",c->id,pid);
    return c->id;
}

int container_list(void){
    poll_states();
    printf("\n┌────┬──────────┬──────────┐\n");
    printf("│ ID │ PID      │ Status   │\n");
    printf("├────┼──────────┼──────────┤\n");
    if (head == NULL){
        printf("│      no containers       │\n");
    } 
    else{
        for(Container *c =head;c!=NULL;c=c->next) {
            printf("│ %-2d │ %-8d │ %-8s │\n",c->id,c->pid,c->status);
        }
    }
    printf("└────┴──────────┴──────────┘\n\n");
    return 0;
}

int container_stop(int id){
    poll_states();
    for(Container *c=head;c!=NULL;c=c->next){
        if(c->id!=id){
            continue;
        }
        if(c->state!=STATE_RUNNING){
            printf("[manager] container-%d is not running\n\n",id);
            return -1;
        }
        kill(c->pid, SIGKILL);
        waitpid(c->pid,NULL,0);
        c->state=STATE_STOPPED;
        strcpy(c->status,"Stopped");
        printf("[manager] container-%d stopped\n\n",id);
        log_event("container-%d STOPPED (pid %d killed)",id,c->pid);
        return 0;
    }
    printf("[error] container %d not found\n\n",id);
    return -1;
}

void cleanup_all_containers(void){
    Container *c=head;
    while(c!=NULL){
        Container *next=c->next;
        if(c->state==STATE_RUNNING){
            kill(c->pid,SIGKILL);
            waitpid(c->pid,NULL,0);
        }
        free(c->stack);
        free(c);
        c=next;
    }
    head=tail=NULL;
}