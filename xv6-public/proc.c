#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#define USERTOP  KERNBASE - 1 - PGSIZE

typedef int thread_t;

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

//data for scheduling
struct {
    struct spinlock lock;
    int non_syscall_num;
    int total_cpu_share;
    int cosolvent;
    int min_pass;
    int mlfq_num;
    int mlfq_pass;
    struct proc *mlfq_head[3];
    struct proc *mlfq_tail[3];
    int boosting;
} SchedInfo;

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&SchedInfo.lock, "SchedInfo");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  // Make each pointer field to refer to appropirate field
  p->p_stack_num = &(p->stack_num);
  p->p_sz = &(p->sz);

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  // sz (top of heap) is *(p_sz) instead of sz
  sz = *(curproc->p_sz);
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  *(curproc->p_sz) = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid, stack_num;
 // uint sz;
  struct proc *np;
  struct proc *curproc = myproc();


  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy main thread's memory area including heap.
  if((stack_num = *(curproc->p_stack_num)) == 0){
    if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
      kfree(np->kstack);
      np->kstack = 0;
      np->state = UNUSED;
      return -1;
    }
  }
  //Thread exist
  else{
    // If threads' stacks exist, we should copy them too.
    if((np->pgdir = copyuvm2(curproc->pgdir, *(curproc->p_sz), 
        USERTOP -2*stack_num*PGSIZE + 1, USERTOP + PGSIZE +1)) == 0){
       kfree(np->kstack);
      np->kstack = 0;
      np->state = UNUSED;
      return -1;
    }
  }

  // Thread also can be a parnent of new process.
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));
  pid = np->pid;
  acquire(&ptable.lock);
  np->state = RUNNABLE;

  //I only have to update mlfq, and total number of tickets
  acquire(&SchedInfo.lock);
  SchedInfo.non_syscall_num++;
  
  //and update process's information
  np->mlfq = -1;
  np->cpu_portion = 0;
  np->pass = SchedInfo.min_pass;
  
  for(i = 0; i<64; i++){
    np->free_stack[i] = 0;
    np->p_free_stack[i] = &(np->free_stack[i]);
  }
  np->stack_num = 0;
  np->p_stack_num = &(np->stack_num);
  np->sz = curproc->sz;
  np->p_sz = &(np->sz);
  np->tid = 0;
  
  release(&SchedInfo.lock);
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p, *t, *before;
  int fd, pid;

  pid = curproc->pid;

  // We should make family threads to zombie too.
  acquire(&ptable.lock);
  for(t = ptable.proc; t < &ptable.proc[NPROC]; t++){
    if(t->pid == pid && t->state != ZOMBIE){
      curproc = t;
     
      // Release ptable lock because when updating inode,
      // ptable lock should not be held.
      release(&ptable.lock);
      
      if(curproc == initproc)
        panic("init exiting");

      // Close all open files.
      for(fd = 0; fd < NOFILE; fd++){
        if(curproc->ofile[fd]){
          fileclose(curproc->ofile[fd]);
          curproc->ofile[fd] = 0;
        }
      }
      
      //update total tickets
      acquire(&SchedInfo.lock);
      if(curproc->mlfq >=0){
        //check whether there is mlfq process
        if(--SchedInfo.mlfq_num == 0){
          SchedInfo.mlfq_pass = -1;
        }
        if(SchedInfo.mlfq_head[curproc->mlfq] == curproc){ 
          SchedInfo.mlfq_head[curproc->mlfq] = curproc->mlfq_next;
          if(SchedInfo.mlfq_tail[curproc->mlfq] == curproc){
            SchedInfo.mlfq_tail[curproc->mlfq] = 0;
          }
        }
        else{   
          // If exiting thread, thread doesn't have to head of each queue,
          // because another thread might called exit.
          // So we should find previous process/thread in queue.
          acquire(&ptable.lock);
          for(before = ptable.proc; before < &ptable.proc[NPROC]; before++){
            if(before->mlfq_next == curproc){
              break;
            }
          }
          release(&ptable.lock);
          // Found previous one. Make previous one and next one connected.
          before->mlfq_next = curproc->mlfq_next;
          if(SchedInfo.mlfq_tail[curproc->mlfq] == curproc){
            SchedInfo.mlfq_tail[curproc->mlfq] = before;
          }
          curproc->mlfq_next = 0;
        }
      }
      else if(curproc->cpu_portion > 0){
        // Only if main thread is exiting, stride info should be updated.
        if(curproc->tid == 0){
          SchedInfo.cosolvent /= curproc->cpu_portion;
          SchedInfo.total_cpu_share -= curproc->cpu_portion;
        }
      }
      else{                             
        //non_syscall
        if(curproc->tid == 0){
          // Only if main thread is exiting, stride info should be updated.
          SchedInfo.non_syscall_num--;
        }
      }
      release(&SchedInfo.lock);

      
      begin_op();
      iput(curproc->cwd);
      end_op();
      curproc->cwd = 0;

      acquire(&ptable.lock);

      // Parent might be sleeping in wait().
      if(curproc->tid == 0){
        wakeup1(curproc->parent);
      }

      // Pass abandoned children to init.
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        // If child is thread, it should be exited by other way
        if(p->parent == curproc && p->tid != 0){    
          p->parent = initproc;
          if(p->state == ZOMBIE)
            wakeup1(initproc);
        }
      }
      // Jump into the scheduler, never to return.
      curproc->state = ZOMBIE;
      
    }
  }

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      //wait only clear main thread's resources
      if(p->parent != curproc || p->tid != 0)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Clean up remaning thread's resources
        pid = p->pid;
        // If threads are existing, clear their resouces
        if(p->stack_num != 0){
          release(&ptable.lock);
          thread_clear(p->pid, p->tid);
          acquire(&ptable.lock);
        }
        // Found one.
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  struct proc *min_pass_p = 0;
  struct proc *temp = 0;
  int i =0;
  int sleeping_count = 0;
  int thread_num = 0;
  int thread_sleep = 0;

  //set data before start first scheduling
  acquire(&SchedInfo.lock);
  SchedInfo.mlfq_pass = -1;     
  SchedInfo.cosolvent = 1;     
  SchedInfo.min_pass = 0;
  SchedInfo.non_syscall_num = 1;

  for(i=0 ; i<3; i++){       
    SchedInfo.mlfq_head[i] = 0;
    SchedInfo.mlfq_tail[i] = 0;
  }
  release(&SchedInfo.lock);
  
  //set first process and shell data for not entering mlfq
  acquire(&ptable.lock);        
    p = ptable.proc;
    p->mlfq = -1;
    p++;
    p -> mlfq = -1;
  release(&ptable.lock);
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);    
    
    acquire(&SchedInfo.lock);
    min_pass_p=0;
    sleeping_count = 0;
    thread_sleep = 1;

    //Select a runnable and not in mlfq process to compare pass
    //And update sleeping process number
    for(p = ptable.proc; p< &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE || p->mlfq >= 0){
        if(p->state == SLEEPING && p->mlfq <0 && p->cpu_portion <=0 && p->tid == 0){
          
          thread_sleep = 1;
          if(*(p->p_stack_num) != 0){
            // Thread exist
            for(temp = ptable.proc; temp < &ptable.proc[NPROC]; temp++){
              if(temp->pid != p->pid){
                continue;
              }
              if(temp->state != SLEEPING){
                // If one thread is not sleeping, sleeping_count will be not updated
                thread_sleep = 0;
                break;
              }
            }
          }
          if(thread_sleep == 1){
            // All threads are sleeping
            sleeping_count += 1;
          }
        }
            continue;
      }
        //first runnable process is selected
        min_pass_p = p;         
        break;
    }
    
    //pick minimum pass process and update sleeping process number
    //if  min_pass_p is 0, it means there is no process handled by stride scheduler
    if(min_pass_p !=0){
      for(p = min_pass_p+1; p< &ptable.proc[NPROC]; p++){        
        if(p->state == SLEEPING && p->mlfq<0 && p->cpu_portion <=0 && p->tid == 0){
          
          thread_sleep = 1;
          if(*(p->p_stack_num) != 0){
            // Thread exist
            for(temp = ptable.proc; temp < &ptable.proc[NPROC]; temp++){
              if(temp->pid != p->pid){
                continue;
              }
              if(temp->state != SLEEPING){
                // If one thread is not sleeping, sleeping_count will be not updated
                thread_sleep = 0;
                break;
              }
            }
          }
          // If all threads are sleeping, increase sleeping count
          if(thread_sleep == 1){
            sleeping_count++;
          }
        }
        if(min_pass_p->pass > p->pass && p->state == RUNNABLE && p->mlfq<0)
          min_pass_p = p;
        }
    }

    temp = min_pass_p;
    thread_num = 1;
    if(min_pass_p !=0 && min_pass_p->mlfq < 0 &&  *(min_pass_p->p_stack_num) != 0){ //It is scheduled by stride
      thread_num = 0;
      // Get the number of thread for updating pass.
      for(p = ptable.proc; p< &ptable.proc[NPROC]; p++){
        if(p->pid == min_pass_p->pid && p->state == RUNNABLE){
          thread_num +=1;
        }
      }
    }
    
    //Compare selected minimum pass and mlfq pass
    //this part also handles the case when there is no selected process,
    if(min_pass_p == 0 || 
        (min_pass_p->pass >= SchedInfo.mlfq_pass && SchedInfo.mlfq_num > 0)){
        //mlfq_pass is minimum
        if(SchedInfo.mlfq_num>0){
        SchedInfo.min_pass = SchedInfo.mlfq_pass;
        }
        for(i = 0; i<3; i++){
            //Search each queue's runnable process
            if((min_pass_p = SchedInfo.mlfq_head[i]) !=0){
              p = min_pass_p;
              //If process is sleeping, send it to tail of queue
              do{
                if(p-> state != RUNNABLE){
                  SchedInfo.mlfq_tail[i] -> mlfq_next = p;
                  SchedInfo.mlfq_tail[i] = p;
                  SchedInfo.mlfq_head[i] = p-> mlfq_next;
                  p = p->mlfq_next;
                  SchedInfo.mlfq_tail[i] -> mlfq_next = 0;
                }
                else{
                  //p is runnable
                  break;  
                }
              }while(p != min_pass_p);
            
            if(p->state == RUNNABLE){
              min_pass_p = p;
              SchedInfo.boosting ++;
              //Update mlfq pass
              if(i ==0){
                SchedInfo.mlfq_pass += SchedInfo.cosolvent 
                  * (80 - SchedInfo.total_cpu_share);
              }
              else{
                SchedInfo.mlfq_pass += SchedInfo.cosolvent 
                  * (80 - SchedInfo.total_cpu_share) 
                    * (2 * i);
                  }
              break;
            }
          }
      }
        // If MLFQ is empty, backup previously selected process
        if(min_pass_p ==0){
            min_pass_p = temp;
        }
    }
    // When cpu_share called process is selected, update it's pass
    else if(min_pass_p->cpu_portion !=0){

      SchedInfo.min_pass = min_pass_p -> pass;
      
      if(SchedInfo.mlfq_pass < 0){ 
        //mlfq doesn't exist
        min_pass_p->pass += (SchedInfo.cosolvent / min_pass_p->cpu_portion)
          * (100 - SchedInfo.total_cpu_share) * thread_num;
      }
      else{
        //mlfq exist
        min_pass_p->pass += (SchedInfo.cosolvent/min_pass_p->cpu_portion) 
          * (80 - SchedInfo.total_cpu_share) * 20 * thread_num;
      }
    }
    // When default(non system called) process is selected, update it's pass
    // Notice: first proc and shell also can be updated here
    else{
      SchedInfo.min_pass = min_pass_p -> pass;
      if(SchedInfo.mlfq_pass < 0){              
        // mlfq doesn't exist
        min_pass_p->pass += SchedInfo.cosolvent 
          * (SchedInfo.non_syscall_num - sleeping_count) * thread_num;
      }
      else{       
        //mlfq exist
        min_pass_p->pass += SchedInfo.cosolvent 
          * (SchedInfo.non_syscall_num - sleeping_count) * 20 * thread_num;
      }
    }  
    release(&SchedInfo.lock);

    // When all process is sleeping, repeat procedure again
    if(min_pass_p ==0){
        release(&ptable.lock);
        continue;
    }

    // Process to run is selected. switch to the process
    c->proc = min_pass_p;
    switchuvm(min_pass_p);
    min_pass_p->state = RUNNING;
    swtch(&(c->scheduler), min_pass_p->context);
    switchkvm();

    c->proc = 0;
    release(&ptable.lock);
  }
}


// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();


  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;

  swtch(&p->context, mycpu()->scheduler);

  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
// Update mlfq by checking time quantum and allotment
void
yield(void)
{
  int i = 0;
  int mlfq_priority = 0;
  int time_quantum = 0;
  int time_allotment = 0;
  struct proc *p = myproc();
  struct proc *head = 0;
  struct proc *tail = 0;
  
  acquire(&ptable.lock);  //DOC: yieldlock
  acquire(&SchedInfo.lock);
  
  // Boosting
  i = 100;

  if(SchedInfo.boosting >= i){
    if(SchedInfo.mlfq_head[2] != 0){    
      head = SchedInfo.mlfq_head[2];
      tail = SchedInfo.mlfq_tail[2];
    }
    for(i = 1; i >= 0; i--){
      if(SchedInfo.mlfq_head[i] != 0){    
        if(head !=0){
          tail->mlfq_next = SchedInfo.mlfq_head[i];
        }
        else{
          head = SchedInfo.mlfq_head[i];
        }
        tail = SchedInfo.mlfq_tail[i];
      }
    }
    if(head!=0){
      SchedInfo.mlfq_head[0] = head;
      SchedInfo.mlfq_tail[0] = tail;
      for(i = 1; i < 3; i++){
        SchedInfo.mlfq_head[i] = 0;
        SchedInfo.mlfq_tail[i] = 0;
      }
      SchedInfo.boosting = 0;
      while(head != 0){
        head->mlfq = 0;
        head->time_quantum = 0;
        head->time_allotment = 0;
        head = head->mlfq_next;
      }
    }
  }

  // Check process mlfq data
  if((mlfq_priority = p -> mlfq) >= 0 && SchedInfo.mlfq_num > 0){

    mlfq_priority = p -> mlfq;
    time_quantum = ++(p -> time_quantum);
    time_allotment = ++(p -> time_allotment);
    // Time allotment is over, move to next queue
    if(mlfq_priority != 2 && time_allotment >= (mlfq_priority+1)*5){
      SchedInfo.mlfq_head[mlfq_priority] = p->mlfq_next;
        //no process in next queue
        if(SchedInfo.mlfq_head[mlfq_priority+1] == 0){      
          SchedInfo.mlfq_head[mlfq_priority+1] = p;
          SchedInfo.mlfq_tail[mlfq_priority+1] = p;
          p->mlfq_next = 0;
        }
        //move to next queue's head
        else{
          p->mlfq_next = SchedInfo.mlfq_head[mlfq_priority+1];  
          SchedInfo.mlfq_head[mlfq_priority+1] = p;
        }
        
        if(SchedInfo.mlfq_tail[mlfq_priority] == p){
          SchedInfo.mlfq_tail[mlfq_priority] = 0;
        }
        //update process's data
        p-> mlfq++;
        p-> time_quantum =0;
        p-> time_allotment =0;
    }
    // Time quantum is over, allotment is left
    // Move to tail of queue
    else if (time_quantum >= mlfq_priority * 2){
      SchedInfo.mlfq_tail[mlfq_priority] -> mlfq_next = p;
      SchedInfo.mlfq_tail[mlfq_priority] = p;
      SchedInfo.mlfq_head[mlfq_priority] = p->mlfq_next;
      p-> time_quantum = 0;
      p-> mlfq_next = 0;
    }
    // When time quantum is left, the processor must go on
    // If yield is called by system call, it should yield cpu
    else{
      if(p->test != 1){
        SchedInfo.boosting ++;
        release(&SchedInfo.lock);
        release(&ptable.lock);
        return;
      }
      // If yield is called by system call, it shoud yield cpu
      // quantum was not done so pass should be updated
      else{
        if(mlfq_priority != 0){
          SchedInfo.mlfq_pass -= SchedInfo.cosolvent * 
            (80 - SchedInfo.total_cpu_share) * 
              (2 * mlfq_priority - time_quantum);

        }
        // and move to tail of queue
        SchedInfo.mlfq_tail[mlfq_priority] -> mlfq_next = p;
        SchedInfo.mlfq_tail[mlfq_priority] = p;
        SchedInfo.mlfq_head[mlfq_priority] = p->mlfq_next;
        p-> time_quantum = 0;
        p-> mlfq_next = 0;
      }
    }
  }

  myproc()->state = RUNNABLE;
  release(&SchedInfo.lock);
  sched();
  release(&ptable.lock);
}

// allocate cpu portion to process (actually portion of time)
int
cpu_share(int portion)
{
    struct proc *p = myproc();
    int check=0;
    acquire(&SchedInfo.lock);
    
    if(portion <=0){
      check = -1;
    }
    else if(SchedInfo.total_cpu_share + portion <=20){
      p->cpu_portion = portion;
      SchedInfo.total_cpu_share+=portion;

      if(p->mlfq < 0){     //wasn't in mlfq
        SchedInfo.non_syscall_num--;
      }
      else{           //was in mlfq
        SchedInfo.mlfq_head[p->mlfq] = p->mlfq_next;
        if(SchedInfo.mlfq_tail[p->mlfq] == p){
          SchedInfo.mlfq_tail[p->mlfq] = 0;
        }
        p->mlfq = -1;
        p->time_quantum = 0;
        p->time_allotment = 0;

        if(--SchedInfo.mlfq_num ==0){
          SchedInfo.mlfq_pass = -1;
        }
      }
      p->pass = SchedInfo.min_pass;
      SchedInfo.cosolvent *= portion;
      check = 0;
    }
    else{       
      check = -1;
    }
    release(&SchedInfo.lock);
    return check;
}

void
run_MLFQ(void)
{
    struct proc *p = myproc();

    acquire(&SchedInfo.lock);
    // Check process called cpu_share syscall
    if(p->cpu_portion !=0){     
      SchedInfo.cosolvent /= p->cpu_portion;
      SchedInfo.total_cpu_share -= p->cpu_portion;
    }
    else{
      SchedInfo.non_syscall_num--;
    }

    SchedInfo.mlfq_num ++;
    // When there is no process in mlfq
    if(SchedInfo.mlfq_head[0] == 0){ 

      SchedInfo.mlfq_pass = SchedInfo.min_pass;
      SchedInfo.mlfq_head[0] = p;
      SchedInfo.mlfq_tail[0] = p;
        
    }
    else{
      p->mlfq_next = SchedInfo.mlfq_head[0];
      SchedInfo.mlfq_head[0] = p;
    }
    release(&SchedInfo.lock);
    
    p->mlfq = 0;
    p->cpu_portion = 0;
    p-> time_quantum = 0;
    p-> time_allotment = 0;
}


// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  //struct proc *next = 0;
  int mlfq_priority = p->mlfq;

  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
 
  acquire(&SchedInfo.lock);
  // Check if time allotment is over to prevent gaming the rule
  if(mlfq_priority >= 0 && SchedInfo.mlfq_num > 0){ 
    p -> time_allotment ++;
    if(mlfq_priority != 2 && p->time_allotment >= (mlfq_priority+1)*5){
      // Should move to next queue
      SchedInfo.mlfq_head[mlfq_priority] = p->mlfq_next;
      // When next queue is empty
      if(SchedInfo.mlfq_head[mlfq_priority+1] == 0){      
        SchedInfo.mlfq_head[mlfq_priority+1] = p;
        SchedInfo.mlfq_tail[mlfq_priority+1] = p;
        p->mlfq_next = 0;
      }
      else{
        p->mlfq_next = SchedInfo.mlfq_head[mlfq_priority+1];                  
        SchedInfo.mlfq_head[mlfq_priority+1] = p;
      }
      
      if(SchedInfo.mlfq_tail[mlfq_priority] == p){
        SchedInfo.mlfq_tail[mlfq_priority] = 0;
      }
      p-> mlfq++;
      p-> time_quantum =0;
      p-> time_allotment =0;
    }
    //just move to tail of queue
    else {
      if(mlfq_priority != 0){
        SchedInfo.mlfq_pass -= SchedInfo.cosolvent * 
          (80 - SchedInfo.total_cpu_share) * 
            (2 * mlfq_priority - p -> time_quantum +1);
        }
        // and move to tail of queue
        SchedInfo.mlfq_tail[mlfq_priority] -> mlfq_next = p;
        SchedInfo.mlfq_tail[mlfq_priority] = p;
        SchedInfo.mlfq_head[mlfq_priority] = p->mlfq_next;
        p-> time_quantum = 0;
        p-> mlfq_next = 0;
    }
  }
  release(&SchedInfo.lock);

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }

}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
  acquire(&SchedInfo.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
      // Set pass with minimum pass when waked up
      p->pass = SchedInfo.min_pass;
        
    }
  release(&SchedInfo.lock);
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  int check = 0;

  acquire(&ptable.lock);
  // Doesn't return until all thread is marked as killed.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      check = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      //release(&ptable.lock);
      //return 0;
    }
  }
  release(&ptable.lock);
  // If one or more process / thread is marked as killed, check will be 1.
  if(check == 1){
    return 0;
  }
  // If no process was found, check will be 0.
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}


// Create thread (LWP : Light weight processor).
// LWP shares memory space, file descriptor, and so on.
// @param[out]  thread deliver : thread id to caller.
// @param[in]   srart_routine : indicate pointer to function thread will run in.
// @parm[in]    arg : argument of start_routine.
// @return      return 0 if successfully completed, -1 if not. 
int 
thread_create(thread_t *thread, void * (*start_routine)(void *), void *arg)
{
  int i=0, stack_num;
  uint sz, sp, ustack[2];
  pde_t *pgdir;
  struct proc *nt;
  struct proc *curproc = myproc();
  
  pgdir = curproc->pgdir;

  if((nt = allocproc()) == 0){
    return -1;
  }

  nt->pid = curproc->pid;
  nt->tid = nexttid++;
  *(thread) = nt->tid;

  nt->pgdir = curproc->pgdir;
 
  nt->sz = curproc->sz;     //sz shoud be made with pointer!
  nt->p_sz = curproc->p_sz;

  nt->parent = curproc;
  
  *(nt->tf) = *(curproc->tf);
  nt->tf->eax = 0;
  nt->p_stack_num = curproc->p_stack_num;
  for(i=0; i<64; i++){
    nt->p_free_stack[i] = curproc->p_free_stack[i];
  }

  
  stack_num = *(curproc->p_stack_num);
  // make guard for kernel
  if(stack_num == 0){
    if((sz = allocuvm(pgdir, USERTOP, USERTOP +PGSIZE)) == 0)
      return -1;
    clearpteu(pgdir, (char*)(sz - PGSIZE));
  }

  for(i=0; i<stack_num; i++){
      if(*(curproc->p_free_stack[i]) !=1){
          break;
      }
  }
  *(curproc->p_free_stack[i]) = 1;

  //Found free stack page
  if(i != stack_num){
    sz = USERTOP - i * 2*PGSIZE;
    sp = sz;
    nt->stack_num = i;
  }
  else{
    stack_num++;
    sz = USERTOP -2 *stack_num *PGSIZE;
    if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0){
        *(curproc->p_free_stack[i]) = 0;
        return -1;
    }
    clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
    sp = sz;
    *(curproc->p_stack_num) = stack_num;
    nt->stack_num = stack_num-1;
  }


  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = (uint)arg;


  sp -= 2 * sizeof(uint);

  if(copyout(pgdir, sp, ustack, 8) < 0){
    return -1;
  }

  nt->pgdir = pgdir;
  nt->tf->esp = sp;
  nt->tf->ebp = sz;
  nt->tf->eip = (uint)start_routine;
  nt->sz = sz;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i]){
      nt->ofile[i] = filedup(curproc->ofile[i]);
    }

  nt->cwd = idup(curproc->cwd);

  switchuvm(curproc);

  safestrcpy(nt->name, curproc->name, sizeof(curproc->name));

  // Update scheduler info

  nt->pass = curproc->pass;
  nt->cpu_portion = curproc->cpu_portion;
  nt->test = 0;

  // If caller is scheduled in mlfq scheduler, new thread shoud be put to mlfq.
  if(curproc->mlfq >= 0){
    put_MLFQ(nt->tid);
  }
  else{
    nt->mlfq = -1;
    nt->time_allotment = 0;
    nt->time_quantum = 0;
    nt->mlfq_next = 0;
  }
  nt->state = RUNNABLE;
  return 0;
}

// Exit thread (LWP).
// Close file descriptor, update inode reference and scheduler information.
// State will became zombie after thread_exit()
// @param[in]   retval : Save exiting value that will be send to parent thread.
void 
thread_exit(void *retval){
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  // save return value at pcb
  curproc->ret_val = retval;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      curproc->ofile[fd] = 0;
    }
  }

  //update total tickets
  acquire(&SchedInfo.lock);
  if(curproc->mlfq >=0){
    //check whether there is mlfq process
    if(--SchedInfo.mlfq_num == 0){
      SchedInfo.mlfq_pass = -1;
    }
    SchedInfo.mlfq_head[curproc->mlfq] = curproc->mlfq_next;
    if(SchedInfo.mlfq_tail[curproc->mlfq] == curproc){
      SchedInfo.mlfq_tail[curproc->mlfq] = 0;
    }
  }
  // We don't have to update stride scheduler info
  // Because threads share time slice
  release(&SchedInfo.lock);

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);
  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");

}

// Wait and join thread (LWP).
// Wait until thread exit, and clear resources used by thread 
// @param[in]   thread : Tid of target thread to wait for.
// @param[out]  ret : return value of ended thread.
// return       return 0 if sucessfully completed, -1 if not.
int
thread_join(thread_t thread, void **retval){
  struct proc *p;
  int havekids, i=0;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->tid != thread)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        kfree(p->kstack);
        p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        p->p_sz = 0;
        p->tid = 0;
       *(curproc->p_free_stack[p->stack_num]) = 0;
        p->sz = 0;

        // Garbage collector
        // If exited  thread has top of threads' stack,
        // dealloc downward until meeting stack which is used by another thrad
        if(p->stack_num == *(p->p_stack_num)){
          for(i = p->stack_num; i >= 0; i--){
            if(*(curproc->p_free_stack[i]) !=0){
              break;    //found stack now used
            }
          }
          // Dealloc guard for kernel if no thread is left.
          if(i==0 && *(curproc->p_free_stack[0]) ==0){
            i = -1;
            if(deallocuvm(p->pgdir, USERTOP + PGSIZE, USERTOP) ==0){
              cprintf("dealloc error!\n");
              return -1;
            }
          }
          // Dealloc non used thread stacks at bottom side of thread stacks.
          if(deallocuvm(p->pgdir, USERTOP - 2*(i+1)*PGSIZE, USERTOP -2*(p->stack_num + 1)*PGSIZE) == 0){
             cprintf("dealloc error!\n");
             return -1;
          }
          *(p->p_stack_num) = i+1;
        }
        p->pgdir = 0;
        p->stack_num = 0;
        p->p_stack_num = 0;
        // Get return value from PCB ret_val field.
        *retval =(void*) p->ret_val;
        p->ret_val = 0;

        release(&ptable.lock);
        return 0;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }  
}

// This function is clear all family threads' resources except one.
// Memory space will be not freed because one left thread will use it.
// @param[in]   pid : pid of family threads to clear.
// @param[in]   tid : tid of the thread that will not cleared.
// return       Process / thread of main thread's parent.
struct proc
*thread_clear(int pid, int tid){
  
  int fd, i;
  struct proc *thread;
  struct proc *curproc = myproc();
  struct proc *parent = 0;
  struct proc *before;

  acquire(&ptable.lock);
  for(thread = ptable.proc; thread < &ptable.proc[NPROC]; thread++){
    if(thread->pid != pid || thread->tid == tid){
      continue;
    }
    // Found a thread to clear.
    release(&ptable.lock);
    if(thread->tid == 0){
      parent = thread->parent;
    }

    if(thread->state != ZOMBIE){
      if(thread == initproc)
        panic("init exiting");
      
      // Close all open files.
      for(fd = 0; fd < NOFILE; fd++){
        if(thread->ofile[fd]){
          fileclose(curproc->ofile[fd]);
          thread->ofile[fd] = 0;
        }
      }

      begin_op();
      iput(thread->cwd);
      end_op();
      thread->cwd = 0;
      
      // Update MLFQ info if the thread was in MLFQ
      acquire(&SchedInfo.lock);
      if(thread->mlfq >=0){
        //check whether there is mlfq process
        if(--SchedInfo.mlfq_num == 0){
          SchedInfo.mlfq_pass = -1;
        }
        if(SchedInfo.mlfq_head[curproc->mlfq] == curproc){ 
          SchedInfo.mlfq_head[curproc->mlfq] = curproc->mlfq_next;
          if(SchedInfo.mlfq_tail[curproc->mlfq] == curproc){
            SchedInfo.mlfq_tail[curproc->mlfq] = 0;
          }
        }
        else{ 
          // If exiting thread, thread doesn't have to head of each queue,
          // because another thread might called exit.
          // So we should find previous process/thread in queue.
          acquire(&ptable.lock);
          for(before = ptable.proc; before < &ptable.proc[NPROC]; before++){
            if(before->mlfq_next == curproc){
              break;
            }
          }
          release(&ptable.lock);
      
          // Found before.
          before->mlfq_next = curproc->mlfq_next;
          if(SchedInfo.mlfq_tail[curproc->mlfq] == curproc){
            SchedInfo.mlfq_tail[curproc->mlfq] = before;
          }
          curproc->mlfq_next = 0;
        }
      }
    else if(curproc->cpu_portion > 0){
      // Only if main thread is exiting, stride info should be updated.
      if(curproc->tid == 0){
        SchedInfo.cosolvent /= curproc->cpu_portion;
        SchedInfo.total_cpu_share -= curproc->cpu_portion;
      }
    }
    else{                             
      //non_syscall
      if(curproc->tid == 0){
        // Only if main thread is exiting, stride info should be updated.
        SchedInfo.non_syscall_num--;
      }
    }

      // We don't have to update stride scheduler info
      // Because threads share time slice
      release(&SchedInfo.lock);
    }

    // Clear PCB fields' data.
    kfree(thread->kstack);
    thread->kstack = 0;
    thread->pid = 0;
    thread->parent = 0;
    thread->name[0] = 0;
    thread->killed = 0;
    thread->state = UNUSED;
    thread->p_sz = 0;
    thread->tid = 0;
    thread->sz = 0;

    thread->pgdir = 0;
    thread->stack_num = 0;
    thread->p_stack_num = 0;
    acquire(&ptable.lock);
  }
  release(&ptable.lock);
  
  // Clear thread data.
  for(i = 0; i<64; i++){
    curproc->free_stack[i] = 0;
    curproc->p_free_stack[i] = &(curproc->free_stack[i]);
  }
  curproc->stack_num = 0;
  curproc->p_stack_num = &(curproc->stack_num);
  curproc->p_sz = &(curproc->sz);
  curproc->tid = 0;

  return parent;
}

// This function put created thread to mlfq
// @param[in]   tid : tid of the thread that is being created.
void
put_MLFQ(int tid)
{
  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->tid == tid){
      break;
    }
  }

  acquire(&SchedInfo.lock);
  SchedInfo.mlfq_num ++;
  
  // There is no chance of MLFQ is empty
  // Because at least master thread exist in MLFQ
  // Process/thread that making this thread can be head of first priority.
  // So it should be checked first.
  if(SchedInfo.mlfq_head[0] != 0){
    
    // If caller process/thread is in head of priority 0,
    // put new thread to next of caller.
    p->mlfq_next = SchedInfo.mlfq_head[0]->mlfq_next;
    SchedInfo.mlfq_head[0]->mlfq_next = p;
    if(SchedInfo.mlfq_tail[0] == SchedInfo.mlfq_head[0]){
      SchedInfo.mlfq_tail[0] = p;
    }
  }
  else{
    p->mlfq_next = 0;
    SchedInfo.mlfq_head[0] = p;
    SchedInfo.mlfq_tail[0] = p;
  }
  release(&SchedInfo.lock);
    
  p->mlfq = 0;
  p->cpu_portion = 0;
  p-> time_quantum = 0;
  p-> time_allotment = 0;
}
