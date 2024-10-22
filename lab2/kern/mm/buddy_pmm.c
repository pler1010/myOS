#include <pmm.h>
#include <list.h>
#include <string.h>
#include <buddy_pmm.h>
#include <stdio.h>

typedef struct
{
    struct Page *base; // 第一个页框的地址
    size_t size;       // 树所覆盖的页框总数n的对数 size = log(n) ，也是树的层数-1
    size_t node_count; // 实际使用的结点数量 2n-1
    size_t page_count; // 初始总页框数量
    int nodes[65535];  // 节点数组。含义：该节点所覆盖的（空闲）页框数量，对数形式，-1表示被占用。
    unsigned int nr_free;
} buddy_tree_t;

buddy_tree_t buddy_tree;

#define nr_free (buddy_tree.nr_free)

// 判断x是否是2的幂
static bool isPowerOfTwo(size_t x)
{
    return (x > 0) && ((x & (x - 1)) == 0);
}

// log2(n)，向下取整
static uint32_t getPowerOfTwo(size_t n)
{
    assert(n > 0);
    uint32_t i = 0;
    while (n > 1)
    {
        n >>= 1;
        i++;
    }
    return i;
}

static void buddy_init(void)
{
    nr_free = 0;
    return;
}

static void buddy_init_memmap(struct Page *base, size_t n)
{
    assert(n > 0);
    struct Page *p = base;

    buddy_tree.base = base;
    buddy_tree.page_count = n;

    // size = log(n)，向上取整
    buddy_tree.size = isPowerOfTwo(n) ? getPowerOfTwo(n) : getPowerOfTwo(n) + 1;
    buddy_tree.node_count = 2 * (1 << buddy_tree.size) - 1;

    size_t unusable_pages_count = (1 << buddy_tree.size) - n;

    // 对每一个页面都进行初始化。伙伴系统中，property已经不起作用了，故不再对property属性进行相关操作。
    for (; p != base + n; p += 1)
    {
        assert(PageReserved(p));
        p->flags = 0;
        // p->property = 0;
        set_page_ref(p, 0);
    }
    nr_free += n;

    // 初始化nodes数组，从底层叶子结点开始，前n个为0, 后unusable_pages_count个为-1，然后依次更新祖先结点(i--)，
    for (size_t i = buddy_tree.node_count - 1; i >= buddy_tree.node_count - unusable_pages_count; i--)
        buddy_tree.nodes[i] = -1;

    for (size_t i = buddy_tree.node_count - unusable_pages_count - 1; i >= buddy_tree.node_count - (1 << buddy_tree.size); i--)
        buddy_tree.nodes[i] = 0;


    // 若祖先结点的两个子结点相同，则是它们的值加1或-1，若不同，则取较大者
    for (int i = buddy_tree.node_count - (1 << buddy_tree.size) - 1; i >= 0; i--)
    {
        int left_child = buddy_tree.nodes[2 * i + 1];
        int right_child = buddy_tree.nodes[2 * i + 2];
        if (left_child == right_child)
            if (left_child != -1)
                buddy_tree.nodes[i] = left_child + 1;
            else
                buddy_tree.nodes[i] = -1;
        else
            buddy_tree.nodes[i] = (left_child > right_child) ? left_child : right_child;
    }
    return;
}

// 分配一个内存块
static struct Page *buddy_alloc_pages(size_t n)
{
    assert(n > 0);
    // cprintf("需要分配的页框数%d\n", n);
    //    size_needed：需要分配的最小空间的幂
    int size_needed = isPowerOfTwo(n) ? getPowerOfTwo(n) : getPowerOfTwo(n) + 1;

    // cprintf("size_needed:%d\n", size_needed);

    size_t index = 0; // 最终找到的节点下标

    // 从根节点开始查找合适的节点
    while (index < buddy_tree.node_count)
    {
        // cprintf("index:%d\n", index);
        size_t left_child = 2 * index + 1;
        size_t right_child = 2 * index + 2;
        if (buddy_tree.nodes[index] == size_needed)
        {
            // cprintf("index:%d\n", index);
            if (left_child < buddy_tree.node_count && right_child < buddy_tree.node_count)
            {
                if (buddy_tree.nodes[left_child] == size_needed)
                    index = left_child;
                else if (buddy_tree.nodes[right_child] == size_needed)
                    index = right_child;
                else
                    break;
            }
            else
                break;
        }
        else if (buddy_tree.nodes[index] < (int)size_needed) // 其实只要根节点大于了size_needed，后面就不需要再判断了。
            return NULL;
        else
        {
            if (buddy_tree.nodes[right_child] >= (int)size_needed &&
                (buddy_tree.nodes[right_child] < buddy_tree.nodes[left_child] || buddy_tree.nodes[left_child] < (int)size_needed))
                index = right_child;
            else
                index = left_child;
        }
    }
    nr_free -= 1 << size_needed;

    // cprintf("最终index:%d\n", index);

    // offset = index所在层在index前的节点数量 乘以 2^full_size(这一层每个节点“满”覆盖的页框数量)
    size_t full_size = buddy_tree.size - getPowerOfTwo(index + 1);
    size_t offset = (index + 1 - (1 << getPowerOfTwo(index + 1))) * (1 << full_size);

    // cprintf("offset:%d\n", offset);
    struct Page *page = buddy_tree.base + offset;

    // 更新节点状态：自己更新为-1，祖先节点更新为两个子结点中的较大者。
    buddy_tree.nodes[index] = -1;
    while (index > 0)
    {
        index = (index - 1) / 2;
        buddy_tree.nodes[index] = buddy_tree.nodes[2 * index + 1] > buddy_tree.nodes[2 * index + 2] ? buddy_tree.nodes[2 * index + 1] : buddy_tree.nodes[2 * index + 2];
    }
    return page;
}

// 释放内存页块
static void buddy_free_pages(struct Page *base, size_t n)
{
    assert(n > 0);

    size_t n_size = isPowerOfTwo(n) ? getPowerOfTwo(n) : getPowerOfTwo(n) + 1;
    n = 1 << n_size; // 真实需要被释放的n

    // index：参数base对应的叶子结点的下标
    size_t offset = base - buddy_tree.base;
    size_t index = offset + (1 << buddy_tree.size) - 1;

    size_t i = 0;
    while (index > 0 && buddy_tree.nodes[index] != -1)
    {
        index = (index - 1) / 2;
        i++;
    }

    // 确保传入的size和n是合法的
    size_t correct_offset = (index + 1 - (1 << getPowerOfTwo(index + 1))) * (1 << i);
    assert(correct_offset == offset);
    assert((1 << i) == n);

    // 更新结点数值
    buddy_tree.nodes[index] = i;

    // 向上回溯，若父节点小于子节点（如：-1与子节点恢复后的值），则赋值给父节点，若和父节点相等，检查自己与兄弟节点是否属于“满”的状态（继续用变量i），若是，则合并（父节点值+1）。
    while (index > 0)
    {
        size_t parent = (index - 1) / 2;
        size_t sibling = (index % 2 == 0) ? index - 1 : index + 1; // 兄弟结点（伙伴）
        // 如果父节点小于任一子节点，则更新父节点为较大子节点的值
        if (buddy_tree.nodes[parent] < buddy_tree.nodes[index])
            buddy_tree.nodes[parent] = buddy_tree.nodes[index];
        // 合并
        else if (buddy_tree.nodes[index] == i && buddy_tree.nodes[sibling] == i)
            buddy_tree.nodes[parent] = buddy_tree.nodes[index] + 1;
        index = parent; // 向上移动到父节点继续检查
        i++;
    }

    struct Page *p = base;
    for (; p != base + n; p++)
    {
        assert(!PageReserved(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    nr_free += n;
}

static size_t
buddy_nr_free_pages(void)
{
    return nr_free;
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
    
    // 分配n - 2^14个大小为1的内存块后，还能分配2^14大小的内存块
    for (size_t i = 0; i < (buddy_tree.page_count - (1 << 14)); i++)
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

    for (size_t i = 0; i < (buddy_tree.page_count - (1 << 14)); i++)
        free_page(buddy_tree.base + (1 << 14) + i);
}

static void buddy_check(void)
{
    cprintf("---------check开始---------\n");
    basic_check();
    cprintf("---------check成功---------\n");
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};