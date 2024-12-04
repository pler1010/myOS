#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void wakeup_proc(struct proc_struct *proc)
{
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

void schedule(void)
{
    bool intr_flag;
    list_entry_t *le, *last;         // le是链表当前元素，last是循环的上一个元素
    struct proc_struct *next = NULL; // next保存下一个要调度的进程

    local_intr_save(intr_flag);
    {
        // 当前进程不需要再被调度
        current->need_resched = 0;

        // 判断当前进程是否是空闲进程（idleproc），如果是，设置last为proc_list
        last = (current == idleproc) ? &proc_list : &(current->list_link);
        le = last;

        // 遍历进程链表，寻找下一个就绪状态的进程
        do
        {
            // 获取下一个进程
            if ((le = list_next(le)) != &proc_list)
            {
                next = le2proc(le, list_link);
                // 如果进程的状态是就绪，跳出循环
                if (next->state == PROC_RUNNABLE)
                {
                    break;
                }
            }
        } while (le != last); // 如果没找到，继续循环

        // 如果没有找到就绪进程，则选择空闲进程作为下一个进程
        if (next == NULL || next->state != PROC_RUNNABLE)
        {
            next = idleproc;
        }

        // 增加该进程的运行次数
        next->runs++;

        // 如果下一个进程与当前进程不同，则执行进程切换
        if (next != current)
        {
            proc_run(next); // 切换到下一个进程
        }
    }

    // 恢复中断状态
    local_intr_restore(intr_flag);
}
