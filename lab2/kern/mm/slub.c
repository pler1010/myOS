#include <pmm.h>
#include <list.h>
#include <string.h>
free_area_t free_area_slub;

#define MAXSPLITPAGE 10
#define PAGESIZE 4096

#define free_list (free_area_slub.free_list)
#define nr_free (free_area_slub.nr_free)

static void
slub_init(void) {
    list_init(&free_list);
    nr_free = 0;
}

static void
slub_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        list_entry_t* le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page* page = le2page(le, page_link);
            if (base < page) {
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
            }
        }
    }
}

static struct Page *
slub_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {
        return NULL;
    }
    struct Page *page = NULL;
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        if (p->property >= n) {
            page = p;
            break;
        }
    }
    if (page != NULL) {
        list_entry_t* prev = list_prev(&(page->page_link));
        list_del(&(page->page_link));
        if (page->property > n) {
            struct Page *p = page + n;
            p->property = page->property - n;
            SetPageProperty(p);
            list_add(prev, &(p->page_link));
        }
        nr_free -= n;
        ClearPageProperty(page);
    }
    return page;
}

static void
slub_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p ++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;

    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        list_entry_t* le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page* page = le2page(le, page_link);
            if (base < page) {
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
            }
        }
    }

    list_entry_t* le = list_prev(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        if (p + p->property == base) {
            p->property += base->property;
            ClearPageProperty(base);
            list_del(&(base->page_link));
            base = p;
        }
    }

    le = list_next(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        if (base + base->property == p) {
            base->property += p->property;
            ClearPageProperty(p);
            list_del(&(p->page_link));
        }
    }
}
/****
 */

struct memForUser{
    struct Page *page;
    size_t offset;
    size_t size;
    list_entry_t memLink;
};

struct Page *page_here[MAXSPLITPAGE];
struct memForUser memList[MAXSPLITPAGE];
struct memForUser mem[MAXSPLITPAGE*PAGESIZE];

static void
slub_free_mem_(struct memForUser *list,struct memForUser *base,size_t n){
    base->size=n;
    if(list_empty(&list->memLink)){
        list_add(&list->memLink,&base->memLink);
    } else {
        list_entry_t* le=&list->memLink;
        while((le=list_next(le))!=&list->memLink){
            le-&(((struct memForUser *)(NULL))->memLink);
            struct memForUser *mem=to_struct(le,struct memForUser,memLink);
            if(base < mem){
                list_add_before(le, &(base->memLink));
                break;
            }
            else if(list_next(le)==&list->memLink) list_add(le,&(base->memLink));
        }
    }
    list_entry_t* le =list_prev(&(base->memLink));
    if(le != &(list->memLink)){
        struct memForUser *p=to_struct(le,struct memForUser,memLink);
        if(p + p->size==base){
            p->size+=base->size;
            list_del(&(base->memLink));
            base=p;
        }
    }
    le=list_next(&(base->memLink));
    if(le != &(list->memLink)){
        struct memForUser *p=to_struct(le,struct memForUser,memLink);
        if(base+base->size==p){
            base->size+=p->size;
            list_del(&(p->memLink));
        }
    }
}

static void
slub_free_mem(struct memForUser *base,size_t n){
    for(int i=0;i<MAXSPLITPAGE;i++){
        if(base->page==page_here[i]){
            slub_free_mem_(&memList[i],base,n);
            break;
        }
    }
}

static struct memForUser *
slub_alloc_mem_(int post,size_t n){
    if(page_here[post]==NULL){
        page_here[post]=slub_alloc_pages(1);
        list_init(&memList[post].memLink);
        list_add(&memList[post].memLink,&(mem+post*PAGESIZE)->memLink);
    }
    struct memForUser *mem=NULL;
    list_entry_t *le=&(memList[post].memLink);
    while ((le = list_next(le)) != &(memList[post].memLink)) {
        struct memForUser * p=to_struct(le,struct memForUser,memLink);
        if(p->size>=n){
            mem=p;
            break;
        }
    }
    if(mem!=NULL){
        list_entry_t* prev = list_prev(&(mem->memLink));
        list_del(&(mem->memLink));
        if(mem->size>n){
            struct memForUser *p=mem+n;
            p->offset=mem->offset+n;
            p->size=mem->size-n;
            list_add(prev,&(p->memLink));
        }
    }
    return mem;
}

static struct memForUser *
slub_alloc_mem(size_t n) {
    struct memForUser *p=NULL;
    for(int i=0;i<MAXSPLITPAGE;i++){
        p=slub_alloc_mem_(i,n);
        if(p!=NULL) break;
        
    }
    return p;
}

/****
 */

static size_t
slub_nr_free_pages(void) {
    return nr_free;
}

struct pmm_byte_manager{
    const char *name;
    void (*init)(void);
    void (*init_memmap)(struct Page *base,size_t n);
    struct Page *(*alloc_pages)(size_t n);
    void (*free_pages)(struct Page *base, size_t n);
    size_t (*nr_free_pages)(void);

    struct memForUser *(*alloc_mem)(size_t n);
    void (*free_mem)(struct memForUser *base,size_t n);
};
//这个结构体在
const struct pmm_byte_manager slub_pmm_manager = {
    .name = "slub_pmm_manager",
    .init = slub_init,
    .init_memmap = slub_init_memmap,
    .alloc_pages = slub_alloc_pages,
    .free_pages = slub_free_pages,
    .nr_free_pages = slub_nr_free_pages,

    .alloc_mem = slub_alloc_mem,
    .free_mem = slub_free_mem
};

