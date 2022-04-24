/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <synch.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

/*
 * allocate a data structure used to keep track of an address space
 * i.e. regions
 */
struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	/*
	 * Initialize as needed.
	 */
	as->region_list = NULL;

	// initialize first-level page table
	as->pagetable = (paddr_t **) alloc_kpages(1);

	if (as->pagetable == NULL) {
		kfree(as);
		return NULL;
	}

	// set 1st lvl page table entries to NULL using index as offsets
	for (int i = 0; i < PT_SIZE; i++) {
		as->pagetable[i] = NULL;
	}
	as->pt_lock = lock_create("page_table_lock");

	return as;
}

/*
 * allocates a new (destination) address space
 * adds all the same regions as source
 * roughly, for each mapped page in source
 * 	allocate a frame in dest
 * 	copy contents from frame to dest frame
 * 	add PT entry for dest
 */
int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas == NULL) {
		return ENOMEM;
	}

	/*
	 * Write this.
	 */

	struct region *curr = old->region_list;
	while (curr != NULL) {
		int result = as_define_region(newas, curr->vaddr, curr->memsize, curr->readable, curr->writeable, curr->executable);
		if (result) {
			as_destroy(newas);
			return result;
		}
		curr = curr->next;
	}

	/*
	 * roughly, for each mapped page in source
	 *  allocate a frame in dest
	 *  copy contents from frame to dest frame
	 *  add PT entry for dest
	 */
	curr = old->region_list;
	lock_acquire(old->pt_lock);
	vm_copy_pt(old->pagetable, newas->pagetable);;
	lock_release(old->pt_lock);
	*ret = newas;
	return 0;
}

/* 
 * deallocate book keeping (region linked list) and page tables
 * deallocate frames used
 */
void
as_destroy(struct addrspace *as)
{
	/*
	 * Clean up as needed.
	 */
	lock_acquire(as->pt_lock);
	for (int i = 0; i < PAGETABLE_SIZE; i++) {
		if (as->pagetable[i] != NULL) {
			for (int j = 0; j < PAGETABLE_SIZE; j++) {
				if (as->pagetable[i][j] != 0) {
					free_kpages(PADDR_TO_KVADDR(as->pagetable[i][j]) & PAGE_FRAME);
				}
			}
		}
	}
	kfree(as->pagetable);

	struct region *tmp;
	struct region *curr = as->region_list;
	while (curr != NULL) {
		tmp = curr;
		curr = curr->next;
		kfree(tmp);
	}
	lock_release(as->pt_lock);
	lock_destroy(as->pt_lock);
	kfree(as);
}

/*
 * flush TLB
 */
void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/*
	 * Write this.
	 */
	/* Disable interrupts on this CPU while frobbing the tlb. */
	spl = splhigh();

	for (i = 0; i < NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

/*
 * flush TLB
 */
void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
	as_activate();
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	/*
	 * Write this.
	 */

	memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;
	memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;

	// Check for region within kuseg
	if ((vaddr + memsize) > MIPS_KSEG0) {
		return EFAULT;
	}

	// Check for overlapping regions
	struct region *curr = as->region_list;
	while (curr != NULL) {
		if ((vaddr <= curr->vaddr + curr->memsize) &&
			(vaddr + memsize) >= curr->vaddr) {
			return EINVAL;
		}
		curr = curr->next;
	}

	struct region *new_region = kmalloc(sizeof(struct region));
	if (new_region == NULL) {
		return ENOMEM;
	}

	new_region->vaddr = vaddr;
	new_region->memsize = memsize;
	new_region->readable = readable;
	new_region->writeable = writeable;
	new_region->executable = executable;
	new_region->old_writeable = writeable;
	new_region->next = NULL;

	new_region->next = as->region_list;
	as->region_list = new_region;

	return 0;
}

/*
 * make READONLY regions READWRITE for loading purposes
 */
int
as_prepare_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	struct region *curr = as->region_list;
	while (curr != NULL) {
		curr->old_writeable = curr->writeable;
		curr->writeable = true;
		curr = curr->next;
	}

	return 0;
}

/*
 * make regions READONLY
 */
int
as_complete_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */
	as_activate();
	struct region *curr = as->region_list;
	while (curr != NULL) {
		curr->writeable = curr->old_writeable;
		curr = curr->next;
	}

	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/*
	 * Write this.
	 */

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return as_define_region(as, *stackptr - PAGE_SIZE * NUM_STACK_PAGES, PAGE_SIZE * NUM_STACK_PAGES, true, true, false);
}

