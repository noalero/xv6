#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;
struct spinlock noplock;
struct spinlock sleepinlock;
struct spinlock runnablelock;
struct spinlock runninglock;
struct spinlock ptimelock;
struct spinlock cpu_utlock;
int rate = 5;
uint program_time = 0;
uint sleeping_processes_mean = 0;
uint runnable_processes_mean = 0;
uint running_processes_mean = 0;
int num_of_proc = 0; // The number of processes in system (including processes that allready exited)
int cpu_utilization = 0;
uint start_time = 0;

// acquire(&ptimelock);
// uint program_time = 0;
// release(&ptimelock);
// acquire(&sleepinlock);
// uint sleeping_processes_mean = 0;
// release(&sleepinlock);
// acquire(&runnablelock);
// uint runnable_processes_mean = 0;
// release(&runnablelock);
// acquire(&runninglock);
// uint running_processes_mean = 0;
// release(&runninglock);
// acquire(&noplock);
// int num_of_proc = 0; // The number of processes in system (including processes that allready exited)
// release(&noplock);
// acquire(&cpu_utlock);
// int cpu_utilization = 0;
// release(&cpu_utlock);
// acquire(&stimelock);
// uint start_time = 0;
// release(&stimelock);


// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  acquire(&tickslock);
  start_time = ticks;
  release(&tickslock);
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  initlock(&noplock, "number_of_proc_lock");
  initlock(&sleepinlock, "sleepin_processes_mean_lock");
  initlock(&runnablelock, "runnable_processes_mean_lock");
  initlock(&runninglock, "running_processes_mean_lock");
  initlock(&ptimelock, "program_time_lock");
  initlock(&cpu_utlock, "cpu_utilization_lock");

  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  // // Check if needed here
  // p->last_runnable_time = 0;
  // p->mean_ticks = 0;
  // p->last_ticks = 0;
  // p->runnable_time = 0;
  // p->running_time = 0;
  // p->sleeping_time = 0;
  // //////////////////////

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  p->sleeping_time = 0;
  p->runnable_time = 0;
  p->running_time = 0;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");
  // Check if needed here
  // p->mean_ticks = 0;
  // p->last_ticks = 0;
  // p->sleeping_time = 0;
  // p->running_time = 0;
  // p->runnable_time = 0;

  p->state = RUNNABLE;
  acquire(&tickslock);
  p->last_runnable_time = ticks;
  release(&tickslock);
  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);//acquire?

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  // np->mean_ticks = 0;
  // np->last_ticks = 0;
  // np->sleeping_time = 0;
  // np->runnable_time = 0;
  // np->running_time = 0;
  acquire(&tickslock);
  uint ticks0 = ticks;
  release(&tickslock);
  np->last_time_state_changed = ticks0;
  np->last_runnable_time = ticks0;
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Updating global variables: num_of_proc  |
  // sleeping_processes_mean  |  runnable_processes_mean  |  running_processes_mean
  acquire(&noplock);
  num_of_proc ++;

  acquire(&sleepinlock);
  sleeping_processes_mean = ((sleeping_processes_mean * num_of_proc) + p->sleeping_time) / (num_of_proc + 1);
  release(&sleepinlock);
  acquire(&runnablelock);
  runnable_processes_mean = ((runnable_processes_mean * num_of_proc) + p->runnable_time) / (num_of_proc + 1);
  release(&runnablelock);
  acquire(&runninglock);
  running_processes_mean = ((running_processes_mean * num_of_proc) + p->running_time) / (num_of_proc + 1);
  release(&runninglock);

  release(&noplock);
  //-----------------------------------//

  // Updating global variables: program_time | cpu_utilization
  acquire(&ptimelock);

  acquire(&runninglock);
  program_time += p->running_time;
  release(&runninglock);

  acquire(&cpu_utlock);
  acquire(&tickslock);
  cpu_utilization = program_time / (ticks - start_time);
  release(&tickslock);
  release(&cpu_utlock);

  release(&ptimelock);
  //-------------------------------------//

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

//Maybe we need to change the functionality to cause the pause be more accurate - right now it is not exactly for x seconds
void checkAndPause(int processesCount)
{
    struct proc *p;
    uint curr_time = 0;
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->should_pause == 1) {
            release(&p->lock);
            acquire(&tickslock);
            uint ticks0 = ticks;
            release(&tickslock);
            curr_time = ticks0;
            while (curr_time - ticks0 < p->pause_left_time/(processesCount/6)) {
                acquire(&tickslock);
                curr_time = ticks;
                release(&tickslock);
            }
            acquire(&p->lock);
            p->pause_left_time = 0;
            p->should_pause = 0;
        }
        release(&p->lock);
    }
}

int initPause(void)
{
    struct proc *p;
    int processesCount=1;
    // Omri added - initialize all shouldPause flags to zero - is it necessary?
    for(p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        p->should_pause = 0;
        p->pause_left_time=0;
        release(&p->lock);
        processesCount++;
    }
    return processesCount;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  printf("proc.c RR\n");
  struct proc *p;
  struct cpu *c = mycpu();
  int processesCount;
  processesCount = initPause();
  uint ticks0, latest_runnable_time_burst;
  c->proc = 0;

  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
      intr_on();
      checkAndPause(processesCount);
      for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        acquire(&tickslock);
        ticks0 = ticks;
        release(&tickslock);
        latest_runnable_time_burst = ticks0 - p->last_time_state_changed;
        p->runnable_time += latest_runnable_time_burst;
        p->last_time_state_changed = ticks0;
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}
//The shell should get commands like in the RR scheduler?
//How should we check this?
void schedulerSJF(void){
  printf("**********Entering SJF scheduler in proc.c file******\n");
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  int min = __INT_MAX__;
  int processesCount =  initPause();
  uint ticks0, latest_runnable_time_burst;

  for(;;){
     intr_on();
     checkAndPause(processesCount);
     min = __INT_MAX__; // I think this line should be here (as in schedualerFCFS)

    // Find the process with minimal <mean_ticks>
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->mean_ticks < min && p->state == RUNNABLE) {
        min = p->mean_ticks;
      }
      release(&p->lock);
    }

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->mean_ticks == min && p->state == RUNNABLE) {
        acquire(&tickslock);
        ticks0 = ticks;
        release(&tickslock);
        latest_runnable_time_burst = ticks0 - p->last_time_state_changed;
        p->runnable_time += latest_runnable_time_burst;
        p->state = RUNNING;
        c->proc = p;

        swtch(&c->context, &p->context);
        
        acquire(&tickslock);
        p->last_ticks = ticks - ticks0; // CPU burst
        release(&tickslock);

        p->mean_ticks = ((10 - rate) * p->mean_ticks + p->last_ticks * rate) / 10;
        c->proc = 0;
        release(&p->lock);
        break;
      }
      release(&p->lock);
    }
  }
}


void schedulerFCFS(void){
  printf("********** Entering to FCFS scheduler *************\n");
  struct proc *p;
  struct cpu *c = mycpu();
  int min = __INT_MAX__;
  int processesCount =  initPause(); // Number of processes
  uint ticks0, latest_runnable_time_burst;
  c->proc = 0;

    for(;;){
     intr_on();
     checkAndPause(processesCount); // Check if <pause_system> was called, and pause acordinglly
     min = __INT_MAX__;
     // Find the process with minimal <last_runnable_time>
     for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);         
      if(p->state == RUNNABLE && p->last_runnable_time < min) {
        min = p->last_runnable_time;
      }
      release(&p->lock);
     }

     for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);         
      if(p->state == RUNNABLE && p->last_runnable_time == min) {
        acquire(&tickslock);
        ticks0 = ticks;
        release(&tickslock);
        latest_runnable_time_burst = ticks0 - p->last_time_state_changed;
        p->runnable_time += latest_runnable_time_burst;
        p->last_time_state_changed = ticks0;
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        c->proc = 0;    
        release(&p->lock);
        break;
      }
      release(&p->lock);
     }
  }
}


// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  uint ticks0;
  acquire(&tickslock);
  ticks0 = ticks;
  release(&tickslock);
  uint latest_running_time_burst = ticks0 - p->last_time_state_changed; // Only a process in <p->state == RUNNING> can yield
  p->running_time += latest_running_time_burst;
  p->last_time_state_changed = ticks0; // The <ticks> value of the time <p> entered <RUNNABLE> state
  p->last_runnable_time = ticks0; 
  p->state = RUNNABLE;  
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  uint ticks0;
  acquire(&tickslock);
  ticks0 = ticks;
  release(&tickslock);
  uint time_since_state_changed = ticks0 - p->last_time_state_changed; 
  if (p->state == RUNNING){
    p->running_time += time_since_state_changed;
  }
  else if (p->state == RUNNABLE){ // Check if a process can sleep from <RUNNABLE> state
    p->runnable_time += time_since_state_changed;
  }
  p->last_time_state_changed = ticks0;


  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        uint ticks0;
        acquire (&tickslock);
        ticks0 = ticks;
        release(&tickslock);
        uint latest_sleeping_time_burst = ticks0 - p->last_time_state_changed; // if(p->state == SLEEPING
        p->sleeping_time += latest_sleeping_time_burst;
        p->last_time_state_changed = ticks0;
        p->last_runnable_time = ticks0;
        p->state = RUNNABLE;

      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;
  uint ticks0;
  uint last_time_since_state_changed;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        acquire(&tickslock);
        ticks0 = ticks;
        release(&tickslock);
        last_time_since_state_changed = ticks0 - p->last_time_state_changed;
        p->sleeping_time += last_time_since_state_changed; // if(p->state == SLEEPING)
        p->last_time_state_changed = ticks0;
        p->last_runnable_time = ticks0;
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int
pause_system(int seconds)
{
    struct  proc *  curr_proc = myproc();
    acquire(&curr_proc->lock);
    curr_proc->pause_left_time = seconds * 100;
    curr_proc->should_pause = 1;
    release(&curr_proc->lock);
    yield();
    return 0;
}

int
kill_system(void)
{
  //Omri- I checked and the function is always gets into pid=0 -> we need to understand why
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      if((p->pid > 3) || (p->pid < 1)){// init process pid is 1, shell process pids are 2,3 - from a print OMRI made in the "exit" function
          p->killed = 1; //kill(p->pid);
      }
      release(&p->lock);
  }
  return 0; 
}

void
print_status(void)
{
  acquire(&tickslock);
  uint ticks0 = ticks;
  release(&tickslock);
  printf("Sleeping Processes Mean is %d\nRunnable Processes Maen is %d\nRunning Processes Mean is %d\nProgram Time is %d\nStart Time is %d\nCPU Utolization is %d\nTicks %d\n",
   sleeping_processes_mean, runnable_processes_mean, running_processes_mean, program_time, start_time, cpu_utilization, ticks0);
}
