#include <proc.h>
#include <kmalloc.h>
#include <string.h>
#include <sync.h>
#include <pmm.h>
#include <error.h>
#include <sched.h>
#include <elf.h>
#include <vmm.h>
#include <trap.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* ------------- process/thread mechanism design&implementation -------------
(an simplified Linux process/thread mechanism )
introduction:
  ucore implements a simple process/thread mechanism. process contains the independent memory sapce, at least one threads
for execution, the kernel data(for management), processor state (for context switch), files(in lab6), etc. ucore needs to
manage all these details efficiently. In ucore, a thread is just a special kind of process(share process's memory).
------------------------------
process state       :     meaning               -- reason
    PROC_UNINIT     :   uninitialized           -- alloc_proc
    PROC_SLEEPING   :   sleeping                -- try_free_pages, do_wait, do_sleep
    PROC_RUNNABLE   :   runnable(maybe running) -- proc_init, wakeup_proc,
    PROC_ZOMBIE     :   almost dead             -- do_exit

-----------------------------
process state changing:

  alloc_proc                                 RUNNING
      +                                   +--<----<--+
      +                                   + proc_run +
      V                                   +-->---->--+
PROC_UNINIT -- proc_init/wakeup_proc --> PROC_RUNNABLE -- try_free_pages/do_wait/do_sleep --> PROC_SLEEPING --
                                           A      +                                                           +
                                           |      +--- do_exit --> PROC_ZOMBIE                                +
                                           +                                                                  +
                                           -----------------------wakeup_proc----------------------------------
-----------------------------
process relations
parent:           proc->parent  (proc is children)
children:         proc->cptr    (proc is parent)
older sibling:    proc->optr    (proc is younger sibling)
younger sibling:  proc->yptr    (proc is older sibling)
-----------------------------
related syscall for process:
SYS_exit        : process exit,                           -->do_exit
SYS_fork        : create child process, dup mm            -->do_fork-->wakeup_proc
SYS_wait        : wait process                            -->do_wait
SYS_exec        : after fork, process execute a program   -->load a program and refresh the mm
SYS_clone       : create child thread                     -->do_fork-->wakeup_proc
SYS_yield       : process flag itself need resecheduling, -- proc->need_sched=1, then scheduler will rescheule this process
SYS_sleep       : process sleep                           -->do_sleep
SYS_kill        : kill process                            -->do_kill-->proc->flags |= PF_EXITING
                                                                 -->wakeup_proc-->do_wait-->do_exit
SYS_getpid      : get the process's pid

*/

// the process set's list
list_entry_t proc_list;

#define HASH_SHIFT 10
#define HASH_LIST_SIZE (1 << HASH_SHIFT) // 1024
#define pid_hashfn(x) (hash32(x, HASH_SHIFT))

// has list for process set based on pid
static list_entry_t hash_list[HASH_LIST_SIZE];

// idle proc
struct proc_struct *idleproc = NULL;
// init proc
struct proc_struct *initproc = NULL;
// current proc
struct proc_struct *current = NULL;

static int nr_process = 0;

void kernel_thread_entry(void);
void forkrets(struct trapframe *tf);
void switch_to(struct context *from, struct context *to);

// alloc_proc - alloc a proc_struct and init all fields of proc_struct
static struct proc_struct *
alloc_proc(void)
{
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc != NULL)
    {
        // LAB4:EXERCISE1 YOUR CODE
        /*
         * below fields in proc_struct need to be initialized
         *       enum proc_state state;                      // Process state
         *       int pid;                                    // Process ID
         *       int runs;                                   // the running times of Proces
         *       uintptr_t kstack;                           // Process kernel stack
         *       volatile bool need_resched;                 // bool value: need to be rescheduled to release CPU?
         *       struct proc_struct *parent;                 // the parent process
         *       struct mm_struct *mm;                       // Process's memory management field
         *       struct context context;                     // Switch here to run process
         *       struct trapframe *tf;                       // Trap frame for current interrupt
         *       uintptr_t cr3;                              // CR3 register: the base addr of Page Directroy Table(PDT)
         *       uint32_t flags;                             // Process flag
         *       char name[PROC_NAME_LEN + 1];               // Process name
         */
        memset(proc, 0, sizeof(struct proc_struct));
        proc->state = PROC_UNINIT;
        proc->pid = -1;
        proc->cr3 = boot_cr3;
        // 该内核线程在内核中运行，故采用为uCore内核已经建立的页表，即设置为在uCore内核页表的起始地址boot_cr3
        // 出所有内核线程的内核虚地址空间（也包括物理地址空间）是相同的
    }
    return proc;
}

// set_proc_name - set the name of proc
char *
set_proc_name(struct proc_struct *proc, const char *name)
{
    memset(proc->name, 0, sizeof(proc->name));
    return memcpy(proc->name, name, PROC_NAME_LEN);
}

// get_proc_name - get the name of proc
char *
get_proc_name(struct proc_struct *proc)
{
    static char name[PROC_NAME_LEN + 1];
    memset(name, 0, sizeof(name));
    return memcpy(name, proc->name, PROC_NAME_LEN);
}

// get_pid - alloc a unique pid for process
// 该函数用于生成一个不与现有进程冲突的PID，并确保PID在最大值 (MAX_PID) 范围内循环。
// PID 从 1 开始递增，确保所有进程都能获得一个唯一的PID。
// 通过检查当前系统中的所有进程，跳过已使用的PID，找到一个可用的PID返回。
// 该函数使用了“最大PID”和“下一个安全的PID”来避免PID冲突。
static int
get_pid(void)
{
    // 静态断言，确保最大PID大于最大进程数（确保足够的PID可用）
    static_assert(MAX_PID > MAX_PROCESS);

    struct proc_struct *proc;                           // 临时存储进程的指针
    list_entry_t *list = &proc_list, *le;               // 遍历进程列表的指针
    static int next_safe = MAX_PID, last_pid = MAX_PID; // next_safe为下一个安全的PID，last_pid为当前PID

    // 如果last_pid已经达到了MAX_PID，则重置为1（跳过0，因为PID为1是第一个有效进程）
    if (++last_pid >= MAX_PID)
    {
        last_pid = 1; // 重置last_pid为1，PID从1开始分配
        goto inside;
    }

    // 如果last_pid超出了next_safe，进入到寻找下一个可用PID的流程
    if (last_pid >= next_safe)
    {
    inside:
        next_safe = MAX_PID; // 假设最大PID为下一个安全的PID
    repeat:
        le = list; // 从进程列表的开头开始查找
        // 遍历整个进程列表
        while ((le = list_next(le)) != list)
        {
            // 获取当前进程结构体
            proc = le2proc(le, list_link);

            // 如果当前进程的PID与last_pid相同，则说明该PID已被占用，需跳过该PID
            if (proc->pid == last_pid)
            {
                // 如果last_pid已达到next_safe，重新开始寻找
                if (++last_pid >= next_safe)
                {
                    // 如果PID达到了最大值MAX_PID，则重置为1
                    if (last_pid >= MAX_PID)
                    {
                        last_pid = 1;
                    }
                    // 设置下一个安全PID为MAX_PID
                    next_safe = MAX_PID;
                    // 重新开始检查
                    goto repeat;
                }
            }
            // 如果当前进程的PID大于last_pid，并且next_safe大于当前PID，更新next_safe
            else if (proc->pid > last_pid && next_safe > proc->pid)
            {
                next_safe = proc->pid; // 更新next_safe为当前进程PID
            }
        }
    }
    // 返回分配的PID（唯一且未被占用）
    return last_pid;
}

// proc_run - make process "proc" running on cpu
// NOTE: before call switch_to, should load  base addr of "proc"'s new PDT
void proc_run(struct proc_struct *proc)
{
    if (proc != current)
    {
        // LAB4:EXERCISE3 YOUR CODE
        /*
         * Some Useful MACROs, Functions and DEFINEs, you can use them in below implementation.
         * MACROs or Functions:
         *   local_intr_save():        Disable interrupts
         *   local_intr_restore():     Enable Interrupts
         *   lcr3():                   Modify the value of CR3 register
         *   switch_to():              Context switching between two processes
         */
        bool intr_flag;
        local_intr_save(intr_flag);
        {
            struct proc_struct *temp = current;
            current = proc;
            // 修改 CR3 寄存器的值，将新的页目录表基地址加载到 CR3 寄存器中
            lcr3(current->cr3);
            switch_to(&(temp->context), &(proc->context));
        }
        local_intr_restore(intr_flag);
    }
}

// forkret -- the first kernel entry point of a new thread/process
// NOTE: the addr of forkret is setted in copy_thread function
//       after switch_to, the current proc will execute here.
static void
forkret(void)
{
    forkrets(current->tf);
}

// hash_proc - add proc into proc hash_list
static void
hash_proc(struct proc_struct *proc)
{
    list_add(hash_list + pid_hashfn(proc->pid), &(proc->hash_link));
}

// find_proc - find proc frome proc hash_list according to pid
struct proc_struct *
find_proc(int pid)
{
    if (0 < pid && pid < MAX_PID)
    {
        list_entry_t *list = hash_list + pid_hashfn(pid), *le = list;
        while ((le = list_next(le)) != list)
        {
            struct proc_struct *proc = le2proc(le, hash_link);
            if (proc->pid == pid)
            {
                return proc;
            }
        }
    }
    return NULL;
}

// kernel_thread - create a kernel thread using "fn" function
// NOTE: the contents of temp trapframe tf will be copied to
//       proc->tf in do_fork-->copy_thread function
int kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags)
{
    struct trapframe tf;
    memset(&tf, 0, sizeof(struct trapframe));

    // 设置内核线程的参数和函数指针
    tf.gpr.s0 = (uintptr_t)fn;  // s0 寄存器保存函数指针
    tf.gpr.s1 = (uintptr_t)arg; // s1 寄存器保存函数参数

    // 设置 trapframe 中的 status 寄存器（SSTATUS）
    // SSTATUS_SPP：Supervisor Previous Privilege（设置为 supervisor 模式，因为这是一个内核线程）
    // SSTATUS_SPIE：Supervisor Previous Interrupt Enable（设置为启用中断，因为这是一个内核线程）
    // SSTATUS_SIE：Supervisor Interrupt Enable（设置为禁用中断，因为我们不希望该线程被中断）
    tf.status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;

    // 将入口点（epc）设置为 kernel_thread_entry 函数，作用实际上是将pc指针指向它(*trapentry.S会用到)
    tf.epc = (uintptr_t)kernel_thread_entry;

    // 使用 do_fork 创建一个新进程（内核线程），这样才真正用设置的tf创建新进程。
    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}

// setup_kstack - alloc pages with size KSTACKPAGE as process kernel stack
static int
setup_kstack(struct proc_struct *proc)
{
    // 调用 alloc_pages 分配2个页框
    struct Page *page = alloc_pages(KSTACKPAGE);
    if (page != NULL)
    {
        /// page2kva 将物理页转换为内核虚拟地址
        proc->kstack = (uintptr_t)page2kva(page);
        return 0;
    }
    return -E_NO_MEM;
}

// put_kstack - free the memory space of process kernel stack
static void
put_kstack(struct proc_struct *proc)
{
    free_pages(kva2page((void *)(proc->kstack)), KSTACKPAGE);
}

// copy_mm - process "proc" duplicate OR share process "current"'s mm according clone_flags
//         - if clone_flags & CLONE_VM, then "share" ; else "duplicate"
static int
copy_mm(uint32_t clone_flags, struct proc_struct *proc)
{
    assert(current->mm == NULL);
    /* do nothing in this project */
    return 0;
}

// copy_thread - setup the trapframe on the  process's kernel stack top and
//             - setup the kernel entry point and stack of process
static void
copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf)
{
    // +----------------------------+ <--- 高地址             ----
    // |                            |                           |
    // |       proc->tf             |                           |
    // |                            |                           |
    // +----------------------------+ <--- proc->tf             |-- KSTACKSIZE
    // |                            |                           |
    // |                            |                           |
    // |                            |                           |
    // +----------------------------+ <--- proc->kstack      ----
    // |                            |
    // |                            | <--- 低地址

    proc->tf = (struct trapframe *)(proc->kstack + KSTACKSIZE - sizeof(struct trapframe));
    *(proc->tf) = *tf;

    // Set a0 to 0 so a child process knows it's just forked 通知子进程它是通过 fork 创建的
    proc->tf->gpr.a0 = 0;
    // 如果 esp 为 0，表示创建的是一个内核线程，则使用 tf 的地址作为栈指针；
    // 否则，使用传入的 esp 作为栈指针
    proc->tf->gpr.sp = (esp == 0) ? (uintptr_t)proc->tf : esp;

    proc->context.ra = (uintptr_t)forkret;
    proc->context.sp = (uintptr_t)(proc->tf);
}

/* do_fork -     parent process for a new child process
 * @clone_flags: used to guide how to clone the child process
 * @stack:       the parent's user stack pointer. if stack==0, It means to fork a kernel thread.
 * @tf:          the trapframe info, which will be copied to child process's proc->tf
 */
int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf)
{
    int ret = -E_NO_FREE_PROC;
    struct proc_struct *proc;
    if (nr_process >= MAX_PROCESS)
    {
        goto fork_out;
    }
    ret = -E_NO_MEM;
    // LAB4:EXERCISE2 YOUR CODE
    /*
     * Some Useful MACROs, Functions and DEFINEs, you can use them in below implementation.
     * MACROs or Functions:
     *   alloc_proc:   create a proc struct and init fields (lab4:exercise1)
     *   setup_kstack: alloc pages with size KSTACKPAGE as process kernel stack
     *   copy_mm:      process "proc" duplicate OR share process "current"'s mm according clone_flags
     *                 if clone_flags & CLONE_VM, then "share" ; else "duplicate"
     *   copy_thread:  setup the trapframe on the  process's kernel stack top and
     *                 setup the kernel entry point and stack of process
     *   hash_proc:    add proc into proc hash_list
     *   get_pid:      alloc a unique pid for process
     *   wakeup_proc:  set proc->state = PROC_RUNNABLE
     * VARIABLES:
     *   proc_list:    the process set's list
     *   nr_process:   the number of process set
     */

    //    1. call alloc_proc to allocate a proc_struct
    //    2. call setup_kstack to allocate a kernel stack for child process
    //    3. call copy_mm to dup OR share mm according clone_flag
    //    4. call copy_thread to setup tf & context in proc_struct
    //    5. insert proc_struct into hash_list && proc_list
    //    6. call wakeup_proc to make the new child process RUNNABLE
    //    7. set ret vaule using child proc's pid

    // 若前3步中失败，需要回退已经进行的过程
    if ((proc = alloc_proc()) == NULL)
    { // 1.分配并初始化进程控制块
        goto fork_out;
    }

    proc->parent = current;

    if (setup_kstack(proc) != 0)
    { // 2.分配并初始化内核栈
        goto bad_fork_cleanup_proc;
    }

    if (copy_mm(clone_flags, proc) != 0)
    { // 3.根据clone_flags决定是复制还是共享内存管理系统
        goto bad_fork_cleanup_kstack;
    }

    copy_thread(proc, stack, tf); // 4.设置进程的中断帧和上下文

    // **禁用中断，使得线程id分配、添加进链表是原子性的（不会被时钟中断等打断）**
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        proc->pid = get_pid();
        hash_proc(proc);                          // 5.把设置好的进程加入hash链表
        list_add(&proc_list, &(proc->list_link)); // 5.把设置好的进程加入链表
        nr_process++;
    }
    local_intr_restore(intr_flag);

    wakeup_proc(proc); // 6.将新建的进程设为就绪态

    ret = proc->pid; // 7.将返回值设为线程id

fork_out:
    return ret;

bad_fork_cleanup_kstack:
    put_kstack(proc);
bad_fork_cleanup_proc:
    kfree(proc);
    goto fork_out;
}

// do_exit - called by sys_exit
//   1. call exit_mmap & put_pgdir & mm_destroy to free the almost all memory space of process
//   2. set process' state as PROC_ZOMBIE, then call wakeup_proc(parent) to ask parent reclaim itself.
//   3. call scheduler to switch to other process
int do_exit(int error_code)
{
    panic("process exit!!.\n");
}

// init_main - the second kernel thread used to create user_main kernel threads
static int
init_main(void *arg)
{
    cprintf("this initproc, pid = %d, name = \"%s\"\n", current->pid, get_proc_name(current));
    cprintf("To U: \"%s\".\n", (const char *)arg);
    cprintf("To U: \"en.., Bye, Bye. :)\"\n");
    return 0;
}

// proc_init - set up the first kernel thread idleproc "idle" by itself and
//           - create the second kernel thread init_main
// proc_init - 设置第一个内核线程 idleproc ("idle")，并创建第二个内核线程 init_main
void proc_init(void)
{
    int i;

    // 1、初始化进程控制块链表，用于存储所有进程
    list_init(&proc_list);

    // 2、初始化哈希表，哈希表用于根据进程PID快速查找进程
    for (i = 0; i < HASH_LIST_SIZE; i++)
    {
        list_init(hash_list + i);
    }

    // 3、分配并初始化第一个内核线程（进程）idleproc
    if ((idleproc = alloc_proc()) == NULL)
    {
        panic("cannot alloc idleproc.\n");
    }

    // 检查 idleproc 结构体是否正确
    // 检查context、进程名称是否全为0
    int *context_mem = (int *)kmalloc(sizeof(struct context));
    memset(context_mem, 0, sizeof(struct context));
    int context_init_flag = memcmp(&(idleproc->context), context_mem, sizeof(struct context));

    int *proc_name_mem = (int *)kmalloc(PROC_NAME_LEN);
    memset(proc_name_mem, 0, PROC_NAME_LEN);
    int proc_name_flag = memcmp(&(idleproc->name), proc_name_mem, PROC_NAME_LEN);

    if (idleproc->cr3 == boot_cr3 && idleproc->tf == NULL && !context_init_flag && idleproc->state == PROC_UNINIT && idleproc->pid == -1 && idleproc->runs == 0 && idleproc->kstack == 0 && idleproc->need_resched == 0 && idleproc->parent == NULL && idleproc->mm == NULL && idleproc->flags == 0 && !proc_name_flag)
    {
        cprintf("alloc_proc() correct!\n");
    }

    // 设置 idleproc 的进程 ID 为 0
    idleproc->pid = 0;
    // 设置 idleproc 的状态为可运行状态
    idleproc->state = PROC_RUNNABLE;
    // 设置 idleproc 的内核栈（bootstack 是一个全局栈，已经在系统启动时定义）
    idleproc->kstack = (uintptr_t)bootstack;
    // 设置 idleproc 需要调度标志为 1，表示需要立即进行调度
    idleproc->need_resched = 1;
    // 设置进程名称为 "idle"
    set_proc_name(idleproc, "idle");
    // 增加系统进程数量
    nr_process++;
    // 将当前运行的进程设置为 idleproc
    current = idleproc;

    // 4、创建第二个内核线程 init_main，进程名称为 "Hello world!!"
    int pid = kernel_thread(init_main, "Hello world!!", 0);
    if (pid <= 0)
    {
        panic("create init_main failed.\n");
    }

    // 通过进程 PID 查找创建的进程并将其赋值给 initproc
    initproc = find_proc(pid);

    // 设置 initproc 的 name 为 "init"
    set_proc_name(initproc, "init");

    // 进行一些断言检查，确保 idleproc 和 initproc 的初始化正确
    assert(idleproc != NULL && idleproc->pid == 0);
    assert(initproc != NULL && initproc->pid == 1);
}

// cpu_idle - at the end of kern_init, the first kernel thread idleproc will do below works
void cpu_idle(void)
{
    while (1)
    {
        if (current->need_resched)
        {
            schedule();
        }
    }
}
