#include <mini-os/console.h>
#include <xen/memory.h>
#include <arch_mm.h>
#include <mini-os/hypervisor.h>
#include <libfdt.h>
#include <lib.h>

void arm_map_extra_stack_section(int);

uint32_t physical_address_offset;

void arch_init_mm(unsigned long *start_pfn_p, unsigned long *max_pfn_p)
{
    int memory;
    int prop_len = 0;
    const uint64_t *regs;

#ifdef CONFIG_VERBOSE_BOOT
    printk("    _text: %p(VA)\n", &_text);
    printk("    _etext: %p(VA)\n", &_etext);
    printk("    _erodata: %p(VA)\n", &_erodata);
    printk("    _edata: %p(VA)\n", &_edata);
    printk("    stack start: %p(VA)\n", _boot_stack);
    printk("    _end: %p(VA)\n", &_end);
#endif

    if (fdt_num_mem_rsv(device_tree) != 0)
        printk("WARNING: reserved memory not supported!\n");

    memory = fdt_node_offset_by_prop_value(device_tree, -1, "device_type", "memory", sizeof("memory"));
    if (memory < 0) {
        printk("No memory found in FDT!\n");
        BUG();
    }

    /* Xen will always provide us at least one bank of memory.
     * Mini-OS will use the first bank for the time-being. */
    regs = fdt_getprop(device_tree, memory, "reg", &prop_len);

    /* The property must contain at least the start address
     * and size, each of which is 8-bytes. */
    if (regs == NULL || prop_len < 16) {
        printk("Bad 'reg' property: %p %d\n", regs, prop_len);
        BUG();
    }

    unsigned int end = (unsigned int) &_end;
    paddr_t mem_base = fdt64_to_cpu(regs[0]);
    uint64_t mem_size = fdt64_to_cpu(regs[1]);
#ifdef CONFIG_VERBOSE_BOOT
    printk("Found memory at 0x%llx (len 0x%llx)\n",
            (unsigned long long) mem_base, (unsigned long long) mem_size);
#endif

    BUG_ON(to_virt(mem_base) > (void *) &_text);          /* Our image isn't in our RAM! */
    *start_pfn_p = PFN_UP(to_phys(end));
    uint64_t heap_len = mem_size - (PFN_PHYS(*start_pfn_p) - mem_base);

    /* Hack: Use the last 1 MB as extra stack space */
    int ram_top_pfn = *start_pfn_p + PFN_DOWN(heap_len);
    int heap_top_pfn = (ram_top_pfn & ~0xff) - 0x100;
    paddr_t heap_top = ((uint64_t) heap_top_pfn) << L1_PAGETABLE_SHIFT;

#ifdef CONFIG_VERBOSE_BOOT
    printk("Mapping 1 MB section at %llx as extra stack space.\n", (unsigned long long) heap_top);
#endif
    arm_map_extra_stack_section(heap_top);

    *max_pfn_p = heap_top_pfn;

#ifdef CONFIG_VERBOSE_BOOT
    printk("Using pages %lu to %lu as free space for heap.\n", *start_pfn_p, *max_pfn_p);
#endif

    /* The device tree is probably in memory that we're about to hand over to the page
     * allocator, so move it to the end and reserve that space.
     */
    uint32_t fdt_size = fdt_totalsize(device_tree);
    void *new_device_tree = to_virt(((*max_pfn_p << PAGE_SHIFT) - fdt_size) & PAGE_MASK);
    if (new_device_tree != device_tree) {
        memmove(new_device_tree, device_tree, fdt_size);
    }
    device_tree = new_device_tree;
    *max_pfn_p = to_phys(new_device_tree) >> PAGE_SHIFT;
}

void arch_init_p2m(unsigned long max_pfn)
{
}

static unsigned long demand_map_area_start;
#define DEMAND_MAP_PAGES 160 /* 640 KiB is enough for anyone */

// FIXME: the x86 backend doesn't track free/in-use explicitly
// because it can read the pagetable. Can we do that too?

// FIXME: because of the above there isn't a way to mark a page
// as free, so we leak.

/* One whole byte to signal free (true) or in-use (false) */
static char demand_map_area_free[DEMAND_MAP_PAGES];

void arch_init_demand_mapping_area(unsigned long cur_pfn)
{
    int i;
    cur_pfn++;
#ifdef CONFIG_VERBOSE_BOOT
    printk("Next pfn = 0x%lx (== %p VA)\n", cur_pfn, pfn_to_virt(cur_pfn));
#endif

    demand_map_area_start = (unsigned long) pfn_to_virt(cur_pfn);
    cur_pfn += DEMAND_MAP_PAGES;
#ifdef CONFIG_VERBOSE_BOOT
    printk("Demand map memory at 0x%lx-0x%p.\n", 
           demand_map_area_start, pfn_to_virt(cur_pfn));
#endif
    for (i = 0; i < DEMAND_MAP_PAGES; i++ ) {
        demand_map_area_free[i] = 1;
    }
}

unsigned long allocate_ondemand(unsigned long n, unsigned long alignment)
{
    int i, j, found;
    unsigned long addr;
    if (alignment != 1) {
        printk("allocate_ondemand: we only support alignment = 1 (was: %ld)\n", alignment);
        BUG();
    }
    for (i = 0, j = 0; i < DEMAND_MAP_PAGES - n; i += j+1 ) {
        /* Is the contiguous section free? */
	found = 0;
        for (j = 0; j < n; j++ )
          if (!demand_map_area_free[i+j]) {
              found = 1;
	      break;
	  }
	if (found) continue;
        /* It's free, so allocate it. */
	for (j = 0; j < n; j++ )
          demand_map_area_free[i+j] = 0;
        addr = demand_map_area_start + i * PAGE_SIZE;
        printk("allocate_ondemand(%ld, %ld) returning %lx\n", n, alignment, addr);
        return addr;
    }
    printk("allocate_ondemand: demand map area is full.\n");
    BUG();
}

/* Get Xen's suggested physical page assignments for the grant table. */
static paddr_t get_gnttab_base(void)
{
    int hypervisor;
    int len = 0;
    const uint64_t *regs;
    paddr_t gnttab_base;

    hypervisor = fdt_node_offset_by_compatible(device_tree, -1, "xen,xen");
    BUG_ON(hypervisor < 0);

    regs = fdt_getprop(device_tree, hypervisor, "reg", &len);
    /* The property contains the address and size, 8-bytes each. */
    if (regs == NULL || len < 16) {
        printk("Bad 'reg' property: %p %d\n", regs, len);
        BUG();
    }

    gnttab_base = fdt64_to_cpu(regs[0]);

#ifdef CONFIG_VERBOSE_BOOT
    printk("FDT suggests grant table base %llx\n", (unsigned long long) gnttab_base);
#endif

    return gnttab_base;
}

grant_entry_t *arch_init_gnttab(int nr_grant_frames)
{
    struct xen_add_to_physmap xatp;
    struct gnttab_setup_table setup;
    xen_pfn_t frames[nr_grant_frames];
    paddr_t gnttab_table;
    int i, rc;

    gnttab_table = get_gnttab_base();

    for (i = 0; i < nr_grant_frames; i++)
    {
        xatp.domid = DOMID_SELF;
        xatp.size = 0;      /* Seems to be unused */
        xatp.space = XENMAPSPACE_grant_table;
        xatp.idx = i;
        xatp.gpfn = (gnttab_table >> PAGE_SHIFT) + i;
        rc = HYPERVISOR_memory_op(XENMEM_add_to_physmap, &xatp);
        BUG_ON(rc != 0);
    }

    setup.dom = DOMID_SELF;
    setup.nr_frames = nr_grant_frames;
    set_xen_guest_handle(setup.frame_list, frames);
    HYPERVISOR_grant_table_op(GNTTABOP_setup_table, &setup, 1);
    if (setup.status != 0)
    {
        printk("GNTTABOP_setup_table failed; status = %d\n", setup.status);
        BUG();
    }

    return to_virt(gnttab_table);
}
