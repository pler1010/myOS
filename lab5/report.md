### Ex2

#### 完成进程空间资源复制

没什么好说的，代码也很短，直接获得虚拟地址后memcpy就行（因为此时ucore使用虚拟地址寻址，memcpy传参也需要虚拟地址）。
```c
/*
    * LAB5:EXERCISE2 - 将源页内容复制到新页，并在进程 B 的页表中建立映射。
    *
    * 实现步骤：
    * 1. 使用 `page2kva` 获取源页的内核虚拟地址。
    * 2. 使用 `page2kva` 获取目标页的内核虚拟地址。
    * 3. 使用 `memcpy` 将数据从源页复制到目标页。
    * 4. 使用 `page_insert` 将新页插入进程 B 的页表中。
    */
    // (1) 获取源页的内核虚拟地址。
    void *src_kvaddr = page2kva(page);
    // (2) 获取目标页的内核虚拟地址。
    void *dst_kvaddr = page2kva(npage);
    // (3) 将源页的内容复制到目标页。
    memcpy(dst_kvaddr, src_kvaddr, PGSIZE);
    // (4) 使用相同权限将新页插入进程 B 的页表。
    ret = page_insert(to, npage, start, perm);
```

#### Copy on Write (COW) 机制设计与实现

本部分内容思路人为构建，部分代码借助GPT生成，仅作参考。

##### 思路构筑与代码实现
1. 共享内存

- COW的核心在于写时复制，而不对页进行写操作（仅读）时不分配新的内存空间。
- 子进程页表建立映射时直接指向父进程的pa可以达到这样的效果。

2. 物理页ref计数

- ref相关代码其实框架已经给出，即在ref == 0时才对物理页进行free（page_remove_pte中逻辑，在proc:do_exit中被使用）；这侧面佐证我们的代码逻辑。
- 因为涉及物理页共享的问题，此时在销毁某个进程时，只看某个进程的页表项，不再能够判断该页是否可以free；需要同时判断ref页是否为0，才能够决定是否销毁物理页，否则只能删除页表映射（可能还有别的进程在看）。

3. 具体逻辑
- 在父子进程内存空间复制时，修改 copy_range 函数，将物理页设置为只读并增加引用计数，同时页表项直接指向原物理页。
```c
uint32_t perm = (*ptep & PTE_USER) & ~PTE_W; // 只读权限
struct Page *page = pte2page(*ptep);
page->ref_count += 1;
ret = page_insert(to, page, start, perm); // 插入目标进程的页表；使用的是原物理页！
```
- 通过异常处理实现写时复制：当进程写入只读页时，触发页错误异常：操作系统可以捕获该异常并调用页错误处理程序。
    - 检查异常地址对应的页是否为共享页。
    - 如果是共享页
        - 分配新的物理页。
        - 复制旧页数据到新页。
        - 更新页表，将新页映射为可写。
        - 减少旧页引用计数，如果引用计数为 0，释放旧页。

- 简单认为触发该异常都是共享页，如果想要与非法写页区分开，可能需要在进程创建时为页表项新增一个“共享页”标志位。
```c
void handle_page_fault(uintptr_t addr) {
    pte_t *ptep = get_pte(current->pgdir, addr, 0);
    if (ptep && (*ptep & PTE_V) && !(*ptep & PTE_W)) {
        struct Page *old_page = pte2page(*ptep);
        if (old_page->ref_count > 1) {
            struct Page *new_page = alloc_page();
            memcpy(page2kva(new_page), page2kva(old_page), PGSIZE); // 复制数据
            page_insert(current->pgdir, new_page, addr, PTE_USER | PTE_W); // 映射新页
            old_page->ref_count -= 1; // 更新引用计数
        } else {
            // 如果引用计数为 1，直接设置页表为可写，即直接复用之前的物理页（接管）
            *ptep |= PTE_W;
        }
    } else {
        // 处理其他类型的页错误
        panic("Unexpected page fault!");
    }
}
```
- 当进程释放共享页时，减少引用计数，引用计数为 0 时释放页。这部分代码在pmm.c中其实已经有实现，这里展示一下：
```c
// page_remove_pte - free an Page sturct which is related linear address la
//                - and clean(invalidate) pte which is related linear address la
// note: PT is changed, so the TLB need to be invalidate
static inline void page_remove_pte(pde_t *pgdir, uintptr_t la, pte_t *ptep) {
    if (*ptep & PTE_V) {  //(1) check if this page table entry is
        struct Page *page =
            pte2page(*ptep);  //(2) find corresponding page to pte
        page_ref_dec(page);   //(3) decrease page reference
        if (page_ref(page) ==
            0) {  //(4) and free this page when page reference reachs 0
            free_page(page);
        }
        *ptep = 0;                  //(5) clear second page table entry
        tlb_invalidate(pgdir, la);  //(6) flush tlb
    }
}
```

### 执行轨迹分析

```c
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
```

该位置首次调用schedule后，调用后依次`proc_run->switch_to->kernel_thread_entry->init_main`。

```c
static int
init_main(void *arg)
{
    size_t nr_free_pages_store = nr_free_pages(); // 获取当前系统的空闲页面数量
    size_t kernel_allocated_store = kallocated(); // 获取当前系统的空闲页面数量

    int pid = kernel_thread(user_main, NULL, 0); // 创建内核进程执行用户进程
    if (pid <= 0)
    { // 线程创建失败
        panic("create user_main failed.\n");
    }

    while (do_wait(0, NULL) == 0)
    {               // 等待进程推出
        schedule(); // 切换执行其他可运行进程
    }

    cprintf("all user-mode processes have quit.\n");
    assert(initproc->cptr == NULL && initproc->yptr == NULL && initproc->optr == NULL);
    assert(nr_process == 2);
    assert(list_next(&proc_list) == &(initproc->list_link));
    assert(list_prev(&proc_list) == &(initproc->list_link));

    cprintf("init check memory pass.\n");
    return 0;
}
```

这个进程创建了一个新的内核进程`user_main`，然后就等所有进程执行结束后它才再次活跃，否则轮到它就继续`schedule`，它退出时会执行panic，操作系统关闭。

`user_main`只是开启了一个用户进程，使用`kernel_execve`，实际上就是把自己“变成”另一个进程。

接下来不断`wait`，直到所有进程执行完成，`init_main`停止，触发`panic`

