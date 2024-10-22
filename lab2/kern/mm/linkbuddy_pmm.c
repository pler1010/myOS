#include <pmm.h>
#include <list.h>
#include <string.h>
// #include <cmath.h>
#include <linkbuddy_pmm.h>
#include <stdio.h>

int free_pages_count = 0;
struct Page *base_buddy;   // 第一个页框地址,用来检查合并
free_area_t free_area[16]; // 定义长度为16的链表
// 2^15 = 32768 > total_frame = 31898
// free_list(i): 指向大小为2^i的空闲块的链表的头指针
// 维护16个链表，每个链表存放相同大小空闲块的块首page指针;维护链表内部地址单调递增
#define free_list(i) (free_area[i].free_list)

int log2_ceil(int x)
{
    int cnt = 0;
    x--;
    while (x > 0)
    {
        x >>= 1;
        cnt++;
    }
    return cnt;
}

// Insert函数：把目标块插入链表的合适位置，保证base的property已正确设置、只差插入
void Insert(struct Page *base)
{
    //  先检查有没有可以合并的块，再把区块头添加进链表中，再维护列表的顺序
    //  检查能否合并：先算自己page的id，再去一个个检查本级链表中的page_id
    //  若可以合并，删除链表中的buddy，合并成新节点(wait_page)，再去高级链表中检查能否合并
    //  最后添加进合适的链表中：维护顺序单调性
    //  若有哪一级为空，循环会直接退出，不需要特判

    int k = (base->property) - 1;
    int temp_k = k;
    struct Page *wait_page = base;
    while (1)
    {
        bool flag = 0;
        list_entry_t *le = &free_list(temp_k);
        int wait_id = wait_page - base_buddy;
        while ((le = list_next(le)) != &free_list(temp_k))
        {
            struct Page *check_page = le2page(le, page_link);
            int check_id = check_page - base_buddy;
            // 检查条件：1.大小相同（天然满足） 2.id1 xor id2 = 2^temp_k
            if ((wait_id ^ check_id) == (1 << temp_k))
            {
                list_del(&(check_page->page_link));
                if (wait_id > check_id)
                {
                    struct Page *temp_page = wait_page;
                    wait_page = check_page;
                    check_page = temp_page;
                    // swap(wait_page, check_page);
                }
                check_page->property = 0;
                ClearPageProperty(check_page);
                wait_page->property += 1;
                flag = 1; // 继续向上合并
                temp_k++;
                break;
            }
        }
        if (!flag)
            break; // 不用合并了，开始插入
    }
    // 此时新节点是wait_page,该加入temp_k层
    list_entry_t *le = &free_list(temp_k);
    int wait_id = wait_page - base_buddy;
    // 0 1 2 3 4 0 le:1 2 3 4
    bool flag = 0; // 是否插入在哪一个的前面：维护地址单调性
    while ((le = list_next(le)) != &free_list(temp_k))
    {
        struct Page *page = le2page(le, page_link);
        int le_id = page - base_buddy;
        if (wait_id < le_id)
        {
            list_add_before(le, &(wait_page->page_link)); // le == &(page->page_link)
            flag = 1;
            break;
        }
    }
    if (!flag)
        list_add_before(le, &(wait_page->page_link)); // 不插入在哪个前面，最后插入在起点前面，相当于链表末尾
}

static void buddy_init(void)
{
    for (int i = 0; i < 16; i++)
    {
        list_init(&free_list(i));
        // nr_free用不到，不再初始化
    }
    free_pages_count = 0;
}

/*完成内存初始化，最开始可能不对齐，给出若干个大小为1的块自行合并*/
static void buddy_init_memmap(struct Page *base, size_t n)
{
    //cprintf("n:%d\n",n);
    assert(n > 0);
    struct Page *p = base;
    base_buddy = base;
    for (; p != base + n; p++)
    {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        // property复用为size:0表示不是free块首
        set_page_ref(p, 0);
    }

    // 开始创建块进链表
    p = base;
    for (; p != base + n; p++)
    {
        p->property = 1; // 大小为2^(1-1)=1
        SetPageProperty(p);
        Insert(p); // 插入到0级链表中，自行合并
    }
    free_pages_count += n;
}

// 要分配m页:找到 >= pow(2,ceil(log2(m)) 的一个块分配
static struct Page *
buddy_alloc_pages(size_t n)
{
    assert(n > 0);
    int k = log2_ceil(n);
    
    if (k > 15)
    {
        return NULL;
    }
    int k_temp = k;
    // 找到最小的满足要求的块：k_max = 15; 一直到15都没找到，代表没有可以够你用的块来细分了
    while (list_empty(&free_list(k_temp)))
    {
        k_temp++;
        if (k_temp > 15)
            return NULL;
    }
    // 若本级链表中没有，从上到下分裂过来：中间一直为空(k_temp到k) k_t  k
    list_entry_t *le = NULL;
    while (k_temp > k)
    {
        // 对free_list(k_temp)的实际链表头分裂:
        // p从k_temp列表中删除;分裂出的两个节点添加到下级列表;
        le = list_next(&free_list(k_temp));
        struct Page *p = le2page(le, page_link);
        list_del(&(p->page_link));
        // property一定 >1: k < k_temp, k_temp的property为1：表示k_temp已经到了最底层的链表，矛盾
        int size = 1 << ((p->property) - 1);
        // 修改区块头信息
        // eg: p p+1 p+2 p+3  property=3 size=4
        (p + (size >> 1))->property = --(p->property);
        SetPageProperty(p + (size >> 1));
        // 添加到下级列表
        list_add_after(&free_list(k_temp - 1), &(p->page_link));
        list_add_after(&(p->page_link), &((p + (size >> 1))->page_link));
        k_temp--;
    }
    // 摘出head_k,返回
    struct Page *page = le2page(list_next(&free_list(k_temp)), page_link);
    list_del(&(page->page_link));
    page->property = 0;
    ClearPageProperty(page);
    free_pages_count -= 1 << k;
    //cprintf("offset:%d\n", page - base_buddy);
    return page;
}

static void
buddy_free_pages(struct Page *base, size_t n)
{
    assert(n > 0);
    int k = log2_ceil(n);
    n = 1 << k; // 真正需要释放的块数
    struct Page *p = base;
    for (; p != base + n; p++)
    {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    base->property = k + 1;
    SetPageProperty(base);
    free_pages_count += n;
    Insert(base); // 插入到合适位置
}

static size_t
buddy_nr_free_pages(void)
{
    return free_pages_count;
}

static void basic_check(void)
{
    cprintf("1、基础检查\n");
    struct Page *p0, *p1, *p2, *p3;
    p0 = p1 = p2 = NULL;
    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);
    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(page_ref(p0) == 0 && page_ref(p1) == 0 && page_ref(p2) == 0);
    assert(page2pa(p0) < npage * PGSIZE);
    assert(page2pa(p1) < npage * PGSIZE);
    assert(page2pa(p2) < npage * PGSIZE);
    assert(p1 == p0 + 1 || p2 == p1 + 1);
    free_page(p0);
    free_page(p1);
    free_page(p2);

    cprintf("2、分配检查\n");
    // 只能分配1个2^14内存块
    struct Page *large_alloc = alloc_pages(1 << 14);
    assert(large_alloc != NULL);
    assert(alloc_pages(1 << 14) == NULL);
    free_pages(large_alloc, 1 << 14);

    // 只能分配三个2^13内存块
    struct Page *small_alloc1 = alloc_pages(1 << 13);
    struct Page *small_alloc2 = alloc_pages(1 << 13);
    struct Page *small_alloc3 = alloc_pages(1 << 13);
    assert(small_alloc1 != NULL && small_alloc2 != NULL && small_alloc3 != NULL);
    assert(alloc_pages(1 << 13) == NULL);
    free_pages(small_alloc1, 1 << 13);
    free_pages(small_alloc2, 1 << 13);
    free_pages(small_alloc3, 1 << 13);

    size_t page_count = free_pages_count;
    
    // 分配n - 2^14个大小为1的内存块后，还能分配2^14大小的内存块
    for (size_t i = 0; i < (page_count - (1 << 14)); i++)
    {
        
        struct Page *single_alloc = alloc_page();
        assert(single_alloc != NULL);
    }
    struct Page *large_alloc2 = alloc_pages(1 << 14);
    assert(large_alloc2 != NULL);
    free_pages(large_alloc2, 1 << 14);

    cprintf("3、合并检查\n");
    p0 = alloc_page();
    p1 = alloc_page();
    p2 = alloc_page();
    p3 = alloc_page();
    assert(p0 != NULL && p1 != NULL && p2 != NULL && p3 != NULL);
    free_page(p1);
    free_page(p2);


    struct Page *p4 = alloc_pages(2);
    assert(p4 == p3 + 1);
    free_page(p3);

    struct Page *p5 = alloc_pages(2);
    assert(p5 == p2);
    free_page(p0);
    free_pages(p4,2);
    free_pages(p5,2);

    for (size_t i = 0; i < (page_count - (1 << 14)); i++)
        free_page(base_buddy + (1 << 14) + i);
}

static void buddy_check(void)
{
    cprintf("---------check开始---------\n");
    basic_check();
    cprintf("---------check成功---------\n");
}

const struct pmm_manager linkbuddy_pmm_manager = {
    .name = "linkbuddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
