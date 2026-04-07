#include<unistd.h>
#include<stdio.h>
int main(){
    int pid = fork();
    if(pid == 0){
              printf("this is the child process:\npid:%d\nchild pid:%d\n",getpid(),pid);
              exit(0);
    }
    else{
        printf("this is the parent process:\npid:%d\nchild pid:%d\n",getpid(),pid);
        sleep(100);
    }

}
