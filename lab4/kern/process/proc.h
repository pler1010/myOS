#ifndef __KERN_PROCESS_PROC_H__
#define __KERN_PROCESS_PROC_H__

#include <defs.h>
#include <list.h>
#include <trap.h>
#include <memlayout.h>

// process's state in his life cycle
enum proc_state
{
    PROC_UNINIT = 0, // uninitialized 创建态
    PROC_SLEEPING,   // sleeping 阻塞态
    PROC_RUNNABLE,   // runnable(maybe running) 就绪态+运行态
    PROC_ZOMBIE,     // almost dead, and wait parent proc to reclaim his resource 终止态
};

// 上下文
struct context
{
    uintptr_t ra; // 返回地址寄存器
    uintptr_t sp; // 栈指针寄存器
    uintptr_t s0;
    uintptr_t s1;
    uintptr_t s2;
    uintptr_t s3;
    uintptr_t s4;
    uintptr_t s5;
    uintptr_t s6;
    uintptr_t s7;
    uintptr_t s8;
    uintptr_t s9;
    uintptr_t s10;
    uintptr_t s11;
};

#define PROC_NAME_LEN 15          // 进程名称的最大长度
#define MAX_PROCESS 4096          // 系统支持的最大进程数量
#define MAX_PID (MAX_PROCESS * 2) // 最大的进程 ID 值

extern list_entry_t proc_list;

struct proc_struct
{
    enum proc_state state;        // 进程状态（就绪、运行、阻塞等）
    int pid;                      // Process ID
    int runs;                     // 进程已运行的次数
    uintptr_t kstack;             // 进程内核栈的虚拟地址
    volatile bool need_resched;   // bool value: need to be rescheduled to release CPU?
    struct proc_struct *parent;   // 父进程的指针（idle没有父进程，其他都有）
    struct mm_struct *mm;         // Process's memory management field 指向进程的页表
    struct context context;       // Switch here to run process
    struct trapframe *tf;         // Trap frame for current interrupt 中断陷入时的寄存器状态
    uintptr_t cr3;                // CR3 register: 页目录表的基地址 (PDT)
    uint32_t flags;               // Process flag
    char name[PROC_NAME_LEN + 1]; // Process name
    list_entry_t list_link;       // Process link list，链接到proc_list表中
    list_entry_t hash_link;       // Process hash list
};

// 链表节点le转换为对应的进程控制块proc_struct结构体指针。
#define le2proc(le, member) \
    to_struct((le), struct proc_struct, member)

// 指向闲逛进程、初始化进程、当前进程
extern struct proc_struct *idleproc, *initproc, *current;

void proc_init(void);
void proc_run(struct proc_struct *proc);
int kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags);

char *set_proc_name(struct proc_struct *proc, const char *name);
char *get_proc_name(struct proc_struct *proc);
void cpu_idle(void) __attribute__((noreturn));

struct proc_struct *find_proc(int pid);
int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf);
int do_exit(int error_code);

#endif /* !__KERN_PROCESS_PROC_H__ */
