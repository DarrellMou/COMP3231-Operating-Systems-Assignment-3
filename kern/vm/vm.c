#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <current.h>
#include <spl.h>
#include <proc.h>
#include <synch.h>

/* Place your page table functions here */


int vm_add_l1_entry(paddr_t **pagetable, uint32_t pt1_index) {
    
    pagetable[pt1_index] = kmalloc(PT_SIZE * (sizeof(paddr_t)));

    if (pagetable[pt1_index] == NULL) {
        return ENOMEM;
    }

    // initialize all pt1 level 2 to 0
    for (int i = 0; i < PT_SIZE; i++) {
        pagetable[pt1_index][i] = 0;
    }

    return 0;
}

int vm_add_l2_entry(paddr_t **pagetable, uint32_t pt1_index, uint32_t pt2_index, uint32_t dirty) {

    vaddr_t v_page_addrs = alloc_kpages(1);
    if (v_page_addrs == 0) {
        return ENOMEM;
    }
    bzero((void *)v_page_addrs, PAGE_SIZE);
    // get physical frame number from virtual page number
    paddr_t p_frame_num = KVADDR_TO_PADDR(v_page_addrs) & PAGE_FRAME;

    pagetable[pt1_index][pt2_index] = p_frame_num | dirty | TLBLO_VALID;

    return 0;
}

int vm_copy_pt(paddr_t **old_pt, paddr_t **new_pt) {

    for (int i = 0; i < PT_SIZE; i++) {
        if (old_pt[i] == NULL) {
            continue;
        }

        new_pt[i] = kmalloc(PT_SIZE * (sizeof(paddr_t)));

        for (int j = 0; j < PT_SIZE; j++) {
            if (old_pt[i][j] != 0) {
                vaddr_t v_page_addrs = alloc_kpages(1);
                if (v_page_addrs == 0) {
                    return ENOMEM;
                }
                bzero((void *)v_page_addrs, PAGE_SIZE);
                memmove((void *) v_page_addrs, (const void *) PADDR_TO_KVADDR(old_pt[i][j] & PAGE_FRAME), PAGE_SIZE);
                // check if old_pt is dirty
                uint32_t dirty = old_pt[i][j] & TLBLO_DIRTY;               
                new_pt[i][j] = (KVADDR_TO_PADDR(v_page_addrs) & PAGE_FRAME) | dirty | TLBLO_VALID;             
            }
            else {
                new_pt[i][j] = 0;
            }
        }
    }
    return 0;

}

void vm_bootstrap(void)
{
    /* Initialise any global components of your VM sub-system here.  
     *  
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    if (curproc == NULL){
        return EFAULT;
    }

    switch (faulttype) {
	    case VM_FAULT_READONLY:
            return EFAULT;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		    break;
	    default:
		    return EINVAL;
	}

    struct addrspace *as = proc_getas();

    if (as == NULL) {
		return EFAULT;
	}

    paddr_t p_frame_num =  KVADDR_TO_PADDR(faultaddress);

    // 2-lvl translation virtual address: |10 bits (first lvl)| 10 bits(second level)| 12 bits(offset)| 
    uint32_t pt1_bits = p_frame_num >> 22;
    uint32_t pt2_bits = (p_frame_num << 10) >> 22;

    bool alloc_pt1 = false;
    lock_acquire(as->pt_lock);
    // check if 1-lvl is NULL
    if (as->pagetable[pt1_bits] == NULL) {
        int check = vm_add_l1_entry(as->pagetable, pt1_bits);
        if (check) {
            lock_release(as->pt_lock);
            return check;
        }
        alloc_pt1 = true;
    }

    uint32_t dirty = 0;
    // valid translation
    if (as->pagetable[pt1_bits][pt2_bits] == 0) {
        struct region *cur_reg = as->region_list;
        // look up region
        while (cur_reg != NULL) {
            // check valid region
            if ((faultaddress >= cur_reg->vaddr) && (faultaddress < cur_reg->vaddr + cur_reg->memsize)) {
                if (cur_reg->writeable) {
                    dirty = TLBLO_DIRTY;
                }
                else {
                    dirty = 0;
                }
                break;
            }
            cur_reg = cur_reg->next;
        }

        // invalid region
        if (cur_reg == NULL) {
            if (alloc_pt1) {
                kfree(as->pagetable[pt1_bits]);
            }
            lock_release(as->pt_lock);
            return EFAULT;
        }

        // allocate frame, zero fill, insert
        int check = vm_add_l2_entry(as->pagetable, pt1_bits, pt2_bits, dirty);
        if (check) {
            if (alloc_pt1) {
                kfree(as->pagetable[pt1_bits]);
            }
            lock_release(as->pt_lock);
            return check;
        }
    }
    // load tlb
    uint32_t entryHi = faultaddress & PAGE_FRAME;
    uint32_t entryLo = as->pagetable[pt1_bits][pt2_bits];
    load_tlb(entryHi, entryLo);
    lock_release(as->pt_lock);

    return 0;
}

void load_tlb(uint32_t entryHi, uint32_t entryLo) {
    // disable interrupt
    int spl = splhigh();
    tlb_random(entryHi, entryLo);
    splx(spl);
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

