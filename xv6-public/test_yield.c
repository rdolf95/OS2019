#include "types.h"
#include "stat.h"
#include "user.h"


static int num =0;

void* thread_fork(void *data){

    int pid = 0, i = 0;

    printf(1,"tid: %d data:%d\n",getpid(), (int)data);

    if((pid = fork()) ==-1){
      printf(1,"fork error data: %d\n",(int)data);
      thread_exit(0);
    }
    else if (pid == 0){  //child
      for(i=0; i<10; i++){
          printf(1, "I'm child %d, data: %d\n",i, (int)data);
          sleep(10);
      }
      exit();
    }
    else{  //parent
      for(i = 0; i<10; i++){
        printf(1, "I'm parent %d, data: %d\n",i, (int)data);
        sleep(10);
      }
      wait();
    }


    thread_exit(data);
    exit();

}

void* thread_malloc(void *data){
 // int arr[10];
  //int *p_arr;
 // int i = 0;

 // for(i = 0; i<10; i++){
 //     arr[i] = i;
 // }

  printf(1, "before brk\n");

  sbrk(1000);
 /* 
  printf(1,"pid : %d, data: %d\n", getpid(), (int)data);
  printf(1, "before malloc\n");

  p_arr = (int*)malloc(sizeof(int)*15);

  for(i=0; i<10; i++){
    p_arr[i] = arr[i]*((int)data);

    printf(1,"p_arr[%d]: %d\n", i, p_arr[i]);
  }
*/
  thread_exit(data);
  exit();
}

void* thread_kill(void *data){

  printf(1,"kill!!\n");
  kill(getpid());

  thread_exit(data);
  exit();

}

void* thread_sleep(void *data){
  printf(1,"sleep! %d\n",(int)data);

  sleep(100);
  thread_exit(data);
  exit();
}

void* thread_normal(void *data){
    printf(1,"nomal! %d\n", (int)data);

    thread_exit(data);
    exit();
}



int
main(int argc, char *argv[]) {

    int thread1, thread2, thread3;
    int data1 =1, data2 = 2, data3 =3;
    int ret1=0, ret2=0, ret3=0;
  //  int i = 0, arr[10], *p_arr;
/*
    printf(1, "malloc in main\n");

    for(i = 0; i<10; i++){
      arr[i] = 1;
    }

    p_arr = (int*)malloc(sizeof(int)*15);

    for(i=0; i<10; i++){
        p_arr[i] = arr[i]*5;

        printf(1,"p_arr[%d]: %d\n", i, p_arr[i]);
    }
*/

    printf(1,"thread_f : %x\n",(uint)thread_malloc);
    printf(1,"num: %d\n", num);
    printf(1,"before create, proc->sz: %x\n",test());

    thread_create(&thread1, thread_malloc, (void*)data1);
    thread_create(&thread2, thread_malloc, (void*)data2);
    thread_create(&thread3, thread_malloc, (void*)data3);

    printf(1, "thread1 : %d, thread2 : %d, thread3 : %d\n",thread1, thread2, thread3);

    thread_join(thread1, (void*)&ret1);
    thread_join(thread2, (void*)&ret2);
    thread_join(thread3, (void*)&ret3);

    sleep(100);

    printf(1,"num: %d\nret1: %d\nret2: %d\nret3: %d\n",num, ret1, ret2, ret3);

    exit();
}

