/*
 * Copyright (c) 2025,  Systems Group, ETH Zurich
 * All rights reserved.
 *
 * This file is part of the Coyote device driver for Linux.
 * Coyote can be found at: https://github.com/fpgasystems/Coyote
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING". If not found, a copy of the GNU General Public  
 * License can be found <https://www.gnu.org/licenses/>.
 */

#include "vfpga_hmm.h"

#ifdef HMM_KERNEL

/*
███╗   ███╗███╗   ███╗██╗   ██╗
████╗ ████║████╗ ████║██║   ██║
██╔████╔██║██╔████╔██║██║   ██║
██║╚██╔╝██║██║╚██╔╝██║██║   ██║
██║ ╚═╝ ██║██║ ╚═╝ ██║╚██████╔╝
╚═╝     ╚═╝╚═╝     ╚═╝ ╚═════╝ 
*/

#define DEVMEM_CHUNK_SIZE (256 * 1024 * 1024U)

struct list_head migrated_pages[MAX_N_REGIONS][N_CTID_MAX];

/**
 * @brief The mmu handler does the heavy lifting in case of a page fault.
 * Takes the page fault struct and handles it.
 *
 * @param d
 * @param pf
 * @param pid
 * @return int
 */
int mmu_handler_hmm(struct vfpga_dev *d, uint64_t vaddr, uint64_t len, int32_t ctid, int32_t stream, pid_t hpid)
{
    int ret_val = 0;
    struct task_struct *curr_task;
    struct mm_struct *curr_mm;
    struct vm_area_struct *vma;
    bool hugepages;
    uint64_t first, last;
    uint64_t n_pages;
    struct bus_driver_data *bd_data = d->bd_data;
    struct tlb_metadata *tlb_meta;
    struct cyt_migrate *args;

    args = kzalloc(sizeof(*args), GFP_KERNEL);
    if (!args) {
        dbg_info("could not allocate args\n");
        return -ENOMEM;
    }

    // lock mmu
    curr_task = pid_task(find_vpid(hpid), PIDTYPE_PID);
    BUG_ON(!curr_task);
    curr_mm = curr_task->mm;
    BUG_ON(!curr_mm);

    // THP?
    vma = vma_lookup(curr_mm, vaddr);
    hugepages = is_thp(vma, vaddr, NULL);

    // take a reference and a lock on the current mm struct
    // this will avoid that we mm actually changes during the procedure
    mmget(curr_mm);
    mmap_read_lock(curr_mm);

    // hugepages?
    tlb_meta = hugepages ? bd_data->ltlb_meta : bd_data->stlb_meta;
    dbg_info("passed region thp %d\n", hugepages);

    // number of pages (huge or regular)
    first = (vaddr & tlb_meta->page_mask) >> PAGE_SHIFT;
    last = ((vaddr + len - 1) & tlb_meta->page_mask) >> PAGE_SHIFT;
    n_pages = last - first + 1;
    if (hugepages)
        n_pages = n_pages * bd_data->n_pages_in_huge;
    dbg_info("first page: %#llx, last_page: %#llx, n_pages: %llu\n", first, last, n_pages);

    // populate the args struct that is used to propagte information to other function calls 
    args->ctid = ctid;
    args->hpid = hpid;
    args->vaddr = first << PAGE_SHIFT;
    args->hugepages = hugepages;
    args->n_pages = n_pages;
    args->vma = vma;

    // we have a host access, map update or migration back
    if (stream == HOST_ACCESS) {
        dbg_info("calling host fault handler\n");
        //ret_val = fpga_do_host_fault(d, args);
        ret_val = fpga_migrate_to_host(d, args);
    }
    // we have a card access, migrate pages to the fpga, install mapping, and make sure that the
    // ptes on the CPU mmu are replaced with migration entries
    else if (stream == CARD_ACCESS) {
        dbg_info("calling migrate handler");
        ret_val = fpga_migrate_to_card(d, args);
    }
    // this should never happen
    else {
        ret_val = -EINVAL;
        pr_err("access not supported, vFPGA %d\n", d->id);
        goto err_access;
    }

err_access:
    mmap_read_unlock(curr_mm);
    mmput(curr_mm);
    kfree(args);
    return ret_val;
}

//
// MMU notifers
//

/**
 * @brief Callback for the mmu_interval_notifier,
 * invalidates mapping on the fpga. Only returns true
 * if it is safe to procceed and all mappings are removed
 * from the fpga.
 *
 * @param interval_sub - notifier
 * @param range - range for invalidation
 * @param cur_seq 
 */
bool cyt_interval_invalidate(struct mmu_interval_notifier *interval_sub, const struct mmu_notifier_range *range, unsigned long cur_seq)
{
    struct hpid_ctid_pages *p = container_of(interval_sub, struct hpid_ctid_pages, mmu_not);
    struct vfpga_dev *d = p->d;
    struct vm_area_struct *vma = range->vma;
    bool huge = is_thp(vma, range->start, NULL);
    struct bus_driver_data *bd_data = d->bd_data;
    struct tlb_metadata *order = huge ? bd_data->ltlb_meta : bd_data->stlb_meta;
    uint64_t start = range->start & order->page_mask;
    uint64_t end = (range->end + order->page_size - 1) & order->page_mask;
    uint64_t first, last;
    uint32_t n_pages;
    pid_t hpid = p->hpid;

    dbg_info("called invalidate with range [%#lx, %#lx] with owner %p\n", range->start, range->end, range->owner);

    if (range->event == MMU_NOTIFY_MIGRATE && range->owner == d) {
        dbg_info("invalidation call on migration range, returning true\n");
        return true;
    }

    if (mmu_notifier_range_blockable(range))
        mutex_lock(&d->mmu_lock);
    else if (!mutex_trylock(&d->mmu_lock))
        return false;

    dbg_info("took mmu_lock\n");

    mmu_interval_set_seq(interval_sub, cur_seq);

    // clear TLB
    first = start >> PAGE_SHIFT;
    last = end >> PAGE_SHIFT;
    n_pages = last - first;
    
    tlb_unmap_hmm(d, start << PAGE_SHIFT, n_pages, hpid, huge);

    mutex_unlock(&d->mmu_lock);
    return true;
}

//
// User migration
//

/**
 * @brief Migrate to host, user executed
 * 
 * @param d - vFPGA
 * @param args - migration args
 */
int user_migrate_to_host(struct vfpga_dev *d, struct cyt_migrate *args)
{
    struct vm_area_struct *vma;
    struct mm_struct *mm;
    struct task_struct *curr;
    int ret_val = 0;

    curr = get_current();
    mm = curr->mm;
    vma = find_vma(mm, args->vaddr);

    args->hugepages = is_thp(vma, args->vaddr, 0);

    // lock
    mutex_lock(&d->mmu_lock);
    change_tlb_lock(d);

    mmget(mm);
    mmap_read_lock(mm);
    //ret_val = fpga_do_host_fault(d, args);
    ret_val = fpga_migrate_to_host(d, args);
    mmap_read_unlock(mm);
    mmput(mm);

    // unlock
    change_tlb_lock(d);
    mutex_unlock(&d->mmu_lock);

    return ret_val;
}

/**
 * @brief Migrate to card, user executed
 * 
 * @param d - vFPGA
 * @param args - migration args
 */
int user_migrate_to_card(struct vfpga_dev *d, struct cyt_migrate *args)
{
    struct vm_area_struct *vma;
    struct mm_struct *mm;
    struct task_struct *curr;
    int ret_val = 0;

    curr = get_current();
    mm = curr->mm;
    vma = find_vma(mm, args->vaddr);

    args->hugepages = is_thp(vma, args->vaddr, 0);

    // lock
    mutex_lock(&d->mmu_lock);
    change_tlb_lock(d);

    mmget(mm);
    mmap_read_lock(mm);
    ret_val = fpga_migrate_to_card(d, args);
    mmap_read_unlock(mm);
    mmput(mm);

    // unlock
    change_tlb_lock(d);
    mutex_unlock(&d->mmu_lock);

    return ret_val;
}

//
// Migrations
//

/**
 * @brief Perform a host fault and map tlb
 * onto fpga. This function will cause a fault on the CPU
 * to ensure that the memory is actually mapped that we try to access.
 * This might even cause the migration of migrated pages back to the system.
 *
 * @param d - vFPGA
 * @param args - migration args
 */
int fpga_do_host_fault(struct vfpga_dev *d, struct cyt_migrate *args)
{
    int ret_val = 0;
    uint64_t start = args->vaddr;
    uint32_t n_pages = args->n_pages;
    pid_t hpid = args->hpid;
    unsigned long timeout = jiffies + msecs_to_jiffies(HMM_RANGE_DEFAULT_TIMEOUT);
    struct mmu_interval_notifier *not = NULL;
    struct hpid_ctid_pages *tmp_entry;

    // fault range
    struct hmm_range range = {
        .start = start,
        .end = start + (n_pages << PAGE_SHIFT),
        .dev_private_owner = d,
        .pfn_flags_mask = 0,
        .default_flags = HMM_PFN_REQ_FAULT | HMM_PFN_REQ_WRITE
    };

    // grab the notifier
    hash_for_each_possible(hpid_ctid_map[d->id], tmp_entry, entry, hpid) {
        if (tmp_entry->hpid == hpid) {
            not = &tmp_entry->mmu_not;
        }
    }

    if(!not) {
        dbg_info("mmu notifier not found\n");
        return -EINVAL;
    }
    range.notifier = not;

    dbg_info("host fault, start %#lx, end %#lx, notifer %p, hpid %d", range.start, range.end, range.notifier, hpid);

    // allocate array for page numbers
    range.hmm_pfns = vmalloc(n_pages, sizeof(unsigned long));
    if (!range.hmm_pfns) {
        dbg_info("failed to allocate pfn array\n");
        return -ENOMEM;
    }
    dbg_info("allocated %u pages at %p\n", n_pages, range.hmm_pfns);

    // retry until timeout
    while (true) {
        if (time_after(jiffies, timeout)) {
            dbg_info("timed out while faulting\n");
            ret_val = -EBUSY;
            goto out;
        }
        dbg_info("trying to fault on range\n");

        // fault on pu to either swap back in or migrate back
        range.notifier_seq = mmu_interval_read_begin(range.notifier);
        ret_val = hmm_range_fault(&range);
        if (ret_val) {
            if (ret_val == -EBUSY)
                continue;

            pr_warn("range fault failed with code %d\n", ret_val);
            goto out;
        }

        // protected by mmu lock, determines if the action was actually safe
        if (mmu_interval_read_retry(range.notifier, range.notifier_seq)) {
            continue;
        }

        break;
    }

    // install stream mapping
    dbg_info("faulted on range, installing mapping");
    tlb_map_hmm(d, start << PAGE_SHIFT, (uint64_t *)range.hmm_pfns, n_pages, STRM_ACCESS, args->ctid, hpid, args->hugepages);

out:
    vfree(range.hmm_pfns);
    return ret_val;
}

/**
 * @brief CPU page fault
 * 
 * @param vmf - vm fault
 * 
*/
vm_fault_t cpu_migrate_to_host(struct vm_fault *vmf)
{
    struct migrate_vma mig_args;
    struct hmm_prvt_info *zone_data = (struct hmm_prvt_info *)vmf->page->zone_device_data;
    bool hugepages = zone_data->huge;
    struct vfpga_dev *d = vmf->page->pgmap->owner;
    struct bus_driver_data *bd_data = d->bd_data;
    struct tlb_metadata *order = hugepages ? bd_data->ltlb_meta : bd_data->stlb_meta;
    uint64_t start = vmf->address & order->page_mask;
    uint64_t end = start + order->page_size;
    uint32_t n_pages = hugepages ? bd_data->n_pages_in_huge : 1;
    vm_fault_t ret = 0;
    int i = 0, j;
    struct page **spages, **dpages;
    pid_t hpid = d->pid_array[zone_data->ctid];
    uint64_t *host_address, *card_address, *calloc;
    struct page *tmp_dpages;
    bool dpages_fail = false;

    dbg_info("migrating back to host vaddr %#llx, huge %d, ctid %d, hpid %d\n", start, hugepages, zone_data->ctid, hpid);

    // src pfn array
    mig_args.src = vmalloc(n_pages, sizeof(*mig_args.src));
    if (!mig_args.src) {
        pr_err("failed to allocate src array\n");
        ret = VM_FAULT_SIGBUS;
        goto out;
    }

    // dst pfn array
    mig_args.dst = vmalloc(n_pages, sizeof(*mig_args.dst));
    if (!mig_args.dst) {
        pr_err("could not allocate dst array\n");
        ret = VM_FAULT_SIGBUS;
        goto err_dst_alloc;
    }

    // alloc array for source pages
    spages = vmalloc(n_pages, sizeof(*spages));
    if (!spages)
    {
        pr_err("failed to allocate source pages array\n");
        ret = VM_FAULT_SIGBUS;
        goto err_spages_alloc;
    }

    // alloc array for destionation pages
    dpages = vmalloc(n_pages, sizeof(*dpages));
    if (!dpages)
    {
        pr_err("failed to allocate destination pages array\n");
        ret = VM_FAULT_SIGBUS;
        goto err_dpages_alloc;
    }

    mig_args.start = start;
    mig_args.end = end;
    mig_args.flags = MIGRATE_VMA_SELECT_DEVICE_PRIVATE;
    mig_args.pgmap_owner = d;
    mig_args.vma = vmf->vma;

    dbg_info("setting up migration ... \n");
    if (migrate_vma_setup(&mig_args)) {
        pr_err("failed to setup migration\n");
        ret = VM_FAULT_SIGBUS;
        goto err_mig_setup;
    }

    dbg_info("set up migration, cpages %lu\n", mig_args.cpages);

    // invalidate
    dbg_info("invalidation started ...\n");
    tlb_unmap_hmm(d, start >> PAGE_SHIFT, mig_args.npages, hpid, hugepages);

    // set up pages for migration
    if (!(mig_args.src[0] & MIGRATE_PFN_MIGRATE)) {
        pr_err("migration not possible\n");
        goto err_no_mig_pages;
    } else {
        if(hugepages) {
            tmp_dpages = alloc_pages_vma(GFP_HIGHUSER_MOVABLE, bd_data->dif_order_page_shift,  mig_args.vma, start, numa_node_id(), false);
            if (!tmp_dpages) {
                dpages_fail = true;
            } else {
                for(i = 0; i < bd_data->n_pages_in_huge; i++) {
                    spages[i] = migrate_pfn_to_page(mig_args.src[i]);
                    dpages[i] = tmp_dpages++;

                    if(i != 0)
                        get_page(dpages[i]);

                    lock_page(dpages[i]);
                    mig_args.dst[i] = migrate_pfn(page_to_pfn(dpages[i])) | MIGRATE_PFN_LOCKED;
                    if (mig_args.src[i] & MIGRATE_PFN_WRITE)
                        mig_args.dst[i] |= MIGRATE_PFN_WRITE;
                }
            }
        } else {
            spages[0] = migrate_pfn_to_page(mig_args.src[0]);
            dpages[0] = alloc_page_vma(GFP_HIGHUSER_MOVABLE,  mig_args.vma, start);
            if (!dpages[0]) {
                dpages_fail = true;
            } else {
                lock_page(dpages[0]);
                mig_args.dst[0] = migrate_pfn(page_to_pfn(dpages[0])) | MIGRATE_PFN_LOCKED;
                if (mig_args.src[0] & MIGRATE_PFN_WRITE)
                    mig_args.dst[0] |= MIGRATE_PFN_WRITE;
            }
        }
    }

    // undo code in case of a failure, if the previous loop was successfull this will be skipped
    if (dpages_fail) {
        pr_err("invalidating all destination page entries\n");
        ret = -ENOMEM;
        mig_args.cpages = 0;
        for (i = 0; i < n_pages; i++) {
            if (!dpages[i]) {
                continue;
            }

            unlock_page(dpages[i]);
            put_page(dpages[i]);
            cpu_free_private_page(dpages[i]);
            mig_args.dst[i] = 0;
        }
        pr_err("restoring original page table\n");
        migrate_vma_pages(&mig_args);
        migrate_vma_finalize(&mig_args);
        goto err_dpages;
    }   

    // host memory addresses
    host_address = vmalloc(mig_args.npages, sizeof(uint64_t));
    if (!host_address) {
        pr_err("failed to allocate host address array");
        ret = -ENOMEM;
        goto err_host_address_alloc;
    }

    // card memory addresses
    card_address = vmalloc(mig_args.npages, sizeof(uint64_t));
    if (!card_address) {
        pr_err("failed to allocate card address array");
        ret = -ENOMEM;
        goto err_card_address_alloc;
    }

    // set physical addresses
    for(i = 0; i < mig_args.npages; i++) {
        if (spages[i]) {
            host_address[i] = ((struct hmm_prvt_info *)spages[i])->card_address;
        }
        if (dpages[i]) {
            card_address[i] = page_to_pfn(dpages[i]);
        }
    }

    // free card memory
    j = 0;
    if(mig_args.cpages > 0) {
        calloc = kcalloc(mig_args.cpages, sizeof(uint64_t), GFP_KERNEL);
        BUG_ON(!calloc);

        for(i = 0; i < mig_args.npages; i++) {
            if (spages[i]) {
                calloc[j++] = ((struct hmm_prvt_info *)(spages[i]->zone_device_data))->card_address;
                list_del(&((struct hmm_prvt_info *)(spages[i]->zone_device_data))->entry);
            }            
        }

        free_card_memory(d, calloc, mig_args.cpages, hugepages);
    }

    // wait for completion
    wait_event_interruptible(d->waitqueue_invldt, atomic_read(&d->wait_invldt) == FLAG_SET);
    atomic_set(&d->wait_invldt, FLAG_CLR);

    // dma
    mutex_lock(&d->sync_lock);

    dbg_info("starting dma ... \n");
    trigger_dma_sync(d, host_address, card_address, n_pages, hugepages);

    // wait for completion
    wait_event_interruptible(d->waitqueue_sync, atomic_read(&d->wait_sync) == FLAG_SET);
    atomic_set(&d->wait_sync, FLAG_CLR);
    dbg_info("dma sync completed\n");

    mutex_unlock(&d->sync_lock);

    dbg_info("finishing migration ... \n");
    migrate_vma_pages(&mig_args);
    migrate_vma_finalize(&mig_args);

    dbg_info("migration back to ram handled, setting hugepage, vma flags hugepage %d, no_hugepage %d\n",
             (vmf->vma->vm_flags & VM_HUGEPAGE) != 0, (vmf->vma->vm_flags & VM_NOHUGEPAGE) != 0);

    vfree(card_address);
err_card_address_alloc:
    vfree(host_address);
err_host_address_alloc:
err_dpages:
err_no_mig_pages:
err_mig_setup:
    vfree(dpages);
err_dpages_alloc:
    vfree(spages);
err_spages_alloc:
    vfree(mig_args.dst);
err_dst_alloc:
    vfree(mig_args.src);
out:
    return ret;
}

/**
 * CPU faulting
*/
static struct dev_pagemap_ops cyt_devmem_ops = {
    .page_free = cpu_free_private_page,
    .migrate_to_ram = cpu_migrate_to_host
};

/**
 * Host page table walk
 *
 * @param vaddr virtual address
 */
struct page *host_ptw(uint64_t vaddr, pid_t hpid)
{
    struct task_struct *curr_task;
    struct mm_struct *curr_mm;
	struct page *page;

    pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	swp_entry_t swp;

    curr_task = pid_task(find_vpid(hpid), PIDTYPE_PID);
    curr_mm = curr_task->mm;

	pgd = pgd_offset(curr_mm, vaddr);
	if(pgd_none(*pgd) || unlikely(pgd_bad(*pgd))) {
        pr_err("ptw exit at pgd\n");
		return NULL;
	}

	p4d = p4d_offset(pgd, vaddr);
	if(p4d_none(*p4d) || unlikely(p4d_bad(*p4d))) {
        pr_err("ptw exit at p4d\n");
		return NULL;
	}

	pud = pud_offset(p4d, vaddr);
	if(pud_none(*pud) || unlikely(pud_bad(*pud))) {
        pr_err("ptw exit at pud\n");
		return NULL;
	}

	pmd = pmd_offset(pud, vaddr);
	if(pmd_none(*pmd)) {
        pr_err("ptw exit at pmd\n");
        return NULL;
    }
		
	pte = pte_offset_map_lock(curr_mm, pmd, vaddr, &ptl);
    if(pte_none(*pte) || pte_present(*pte)) {
        pte_unmap_unlock(pte, ptl);
        pr_err("ptw exit at pte\n");
        return NULL;
    }
    
    swp = pte_to_swp_entry(*pte);
    if(!is_device_private_entry(swp)) {
        pte_unmap_unlock(pte, ptl);
        pr_err("ptw exit at swp\n");
        return NULL;
    }

    page = pfn_swap_entry_to_page(swp);
    pte_unmap_unlock(pte, ptl);
    return page;
}

/**
 * @brief Migrates the memory array that is described by the args argument
 * to the fpga card memory. Furthermore, it takes care that the memory is
 * no longer accessible to the CPU.
 *
 * @param d
 * @param args
 * @return int
 */
int fpga_migrate_to_host(struct vfpga_dev *d, struct cyt_migrate *args) 
{
    int ret_val = 0;
    int i, j;
    struct mm_struct *curr_mm = args->vma->vm_mm;
    uint64_t start = args->vaddr;
    uint64_t end = args->vaddr + (args->n_pages << PAGE_SHIFT);
    struct migrate_vma mig_args;
    struct page **spages, **dpages;
    bool dpages_fail = false;
    uint64_t *card_address, *host_address, *calloc;
    struct page *tmp_dpages;
    struct bus_driver_data *bd_data = d->bd_data;
    uint32_t pg_inc = args->hugepages ? bd_data->n_pages_in_huge : 1;
    

    dbg_info("migration to host, vaddr start %llx, end %llx, ctid %d, hpid %d, vFPGA %d",
             args->vaddr, end, args->ctid, args->hpid, d->id);

    // alloc array for src pfns
    mig_args.src = vmalloc(args->n_pages, sizeof(*mig_args.src));
    if (!mig_args.src)
    {
        pr_err("failed to allocate source resources\n");
        ret_val = -ENOMEM;
        goto err_src_alloc;
    }

    // alloc array for dst pfns
    mig_args.dst = vmalloc(args->n_pages, sizeof(*mig_args.dst));
    if (!mig_args.dst)
    {
        pr_err("failed to allocate destination resources\n");
        ret_val = -ENOMEM;
        goto err_dst_alloc;
    }

    // alloc array for source pages
    spages = vmalloc(args->n_pages, sizeof(*spages));
    if (!spages)
    {
        pr_err("failed to allocate source pages array\n");
        ret_val = -ENOMEM;
        goto err_spages_alloc;
    }

    // alloc array for destionation pages
    dpages = vmalloc(args->n_pages, sizeof(*dpages));
    if (!dpages)
    {
        pr_err("failed to allocate destination pages array\n");
        ret_val = -ENOMEM;
        goto err_dpages_alloc;
    }

    // allocate the migrate_vma struct for the call to the hmm api
    mig_args.start = start;
    mig_args.end = end;
    mig_args.vma = find_vma_intersection(curr_mm, start, end);
    if (!mig_args.vma) {
        pr_err("failed to match vma\n");
        ret_val = -EFAULT;
        goto err_vma;
    }

    mig_args.pgmap_owner = d;
    mig_args.flags = MIGRATE_VMA_SELECT_DEVICE_PRIVATE;
    dbg_info("setting up migration...\n");
    ret_val = migrate_vma_setup(&mig_args);
    if (ret_val) {
        pr_err("failed to setup migration\n");
        goto err_mig_setup;
    }
    dbg_info("set up migration, cpages: %lu\n", mig_args.cpages);

    // invalidate
    dbg_info("invalidation started ...\n");
    tlb_unmap_hmm(d, start >> PAGE_SHIFT, mig_args.npages, args->hpid, args->hugepages);

    // set up pages for migration
    for(i = 0; i < mig_args.npages; i+=pg_inc) {
        // check if page is ready to migrate or if a new mapping might be necessary
        if (!(mig_args.src[i] & MIGRATE_PFN_MIGRATE)) {
            dbg_info("page table walk, entry %d", i);
            dpages[i] = host_ptw(start + (i << PAGE_SHIFT), args->hpid);
        } else {
            if (args->hugepages) {
                tmp_dpages = alloc_pages_vma(GFP_HIGHUSER_MOVABLE, bd_data->dif_order_page_shift, args->vma, start + i, numa_node_id(), false);
                if (!tmp_dpages) {
                    dpages_fail = true;
                    continue;
                }

                for(j = 0; j < bd_data->n_pages_in_huge; i++) {
                    spages[i+j] = migrate_pfn_to_page(mig_args.src[i+j]);
                    dpages[i+j] = tmp_dpages++;

                    if(j != 0)
                        get_page(dpages[i+j]);

                    lock_page(dpages[i+j]);
                    mig_args.dst[i+j] = migrate_pfn(page_to_pfn(dpages[i+j])) | MIGRATE_PFN_LOCKED;
                    if (mig_args.src[i+j] & MIGRATE_PFN_WRITE)
                        mig_args.dst[i+j] |= MIGRATE_PFN_WRITE;
                }
            } else {
                spages[i] = migrate_pfn_to_page(mig_args.src[i]);
                dpages[i] = alloc_page_vma(GFP_HIGHUSER_MOVABLE, args->vma, start + i);
                if (!dpages[i]) {
                    dpages_fail = true;
                    continue;
                }

                lock_page(dpages[i]);
                mig_args.dst[i] = migrate_pfn(page_to_pfn(dpages[i])) | MIGRATE_PFN_LOCKED;
                if (mig_args.src[i] & MIGRATE_PFN_WRITE)
                    mig_args.dst[i] |= MIGRATE_PFN_WRITE;
            }
        }
    }

    // undo code in case of a failure, if the previous loop was successfull this will be skipped
    if (dpages_fail) {
        pr_err("invalidating all destination page entries\n");
        ret_val = -ENOMEM;
        mig_args.cpages = 0;
        for (i = 0; i < args->n_pages; i++) {
            if (!dpages[i] || mig_args.dst[i] == 0) {
                continue;
            }

            unlock_page(dpages[i]);
            put_page(dpages[i]);
            cpu_free_private_page(dpages[i]);
            mig_args.dst[i] = 0;
        }
        pr_err("restoring original page table\n");
        migrate_vma_pages(&mig_args);
        migrate_vma_finalize(&mig_args);
        goto err_dpages;
    }   

    // host memory addresses
    host_address = vmalloc(mig_args.npages, sizeof(uint64_t));
    if (!host_address) {
        pr_err("failed to allocate host address array");
        ret_val = -ENOMEM;
        goto err_host_address_alloc;
    }

    // card memory addresses
    card_address = vmalloc(mig_args.npages, sizeof(uint64_t));
    if (!card_address) {
        pr_err("failed to allocate card address array");
        ret_val = -ENOMEM;
        goto err_card_address_alloc;
    }

    // set physical addresses
    for(i = 0; i < args->n_pages; i++) {
        if (spages[i]) {
            host_address[i] = ((struct hmm_prvt_info *)(spages[i]->zone_device_data))->card_address;
        }
        if (dpages[i]) {
            card_address[i] = page_to_pfn(dpages[i]);
        }
    }

    // free card memory
    j = 0;
    if(mig_args.cpages > 0) {
        calloc = vmalloc(mig_args.cpages, sizeof(uint64_t));
        BUG_ON(!calloc);

        for(i = 0; i < mig_args.npages; i++) {
            if (spages[i]) {
                calloc[j++] = ((struct hmm_prvt_info *)(spages[i]->zone_device_data))->card_address;
                list_del(&((struct hmm_prvt_info *)(spages[i]->zone_device_data))->entry);
            }            
        }

        free_card_memory(d, calloc, mig_args.cpages, args->hugepages);
    }

    // wait for completion
    wait_event_interruptible(d->waitqueue_invldt, atomic_read(&d->wait_invldt) == FLAG_SET);
    atomic_set(&d->wait_invldt, FLAG_CLR);

    // dma
    if(mig_args.cpages > 0) {
        // lock
        mutex_lock(&d->sync_lock);

        // dma operation
        dbg_info("starting dma ... \n");
        trigger_dma_sync(d, host_address, card_address, mig_args.npages, args->hugepages);

        // wait for completion
        wait_event_interruptible(d->waitqueue_sync, atomic_read(&d->wait_sync) == FLAG_SET);
        atomic_set(&d->wait_sync, FLAG_CLR);
        dbg_info("dma sync completed\n");

        // unlock
        mutex_unlock(&d->sync_lock);
    }

    // swap pages
    dbg_info("swap out pages\n");
    migrate_vma_pages(&mig_args);
    dbg_info("migrated pages, cpages %lu\n", mig_args.cpages);

    // finalize migration
    dbg_info("finalizing migration ... ");
    migrate_vma_finalize(&mig_args);
    dbg_info("finalized migration, cpages %lu\n", mig_args.cpages);

    // tlb map operation
    tlb_map_hmm(d, start >> PAGE_SHIFT, host_address, mig_args.npages, STRM_ACCESS, args->ctid, args->hpid, args->hugepages);

    if(mig_args.cpages > 0)
        vfree(calloc);
    vfree(card_address);
err_card_address_alloc:
    vfree(host_address);
err_host_address_alloc:
err_dpages:
err_mig_setup:
err_vma:
    vfree(dpages);
err_dpages_alloc:
    vfree(spages);
err_spages_alloc:
    vfree(mig_args.dst);
err_dst_alloc:
    vfree(mig_args.src);
err_src_alloc:
    return ret_val;
}

/**
 * @brief Migrates the memory array that is described by the args argument
 * to the fpga card memory. Furthermore, it takes care that the memory is
 * no longer accessible to the CPU.
 *
 * @param d
 * @param args
 * @return int
 */
int fpga_migrate_to_card(struct vfpga_dev *d, struct cyt_migrate *args) 
{
    int ret_val = 0;
    int i, j;
    struct mm_struct *curr_mm = args->vma->vm_mm;
    uint64_t start = args->vaddr;
    uint64_t end = args->vaddr + (args->n_pages << PAGE_SHIFT);
    struct migrate_vma mig_args;
    struct page **spages, **dpages;
    bool dpages_fail = false;
    struct hmm_prvt_info *new_entry;
    uint64_t *card_address, *host_address, *calloc;

    dbg_info("migration to card, vaddr start %llx, end %llx, ctid %d, hpid %d, vFPGA %d",
             args->vaddr, end, args->ctid, args->hpid, d->id);

    // alloc array for src pfns
    mig_args.src = vmalloc(args->n_pages, sizeof(*mig_args.src));
    if (!mig_args.src)
    {
        pr_err("failed to allocate source resources\n");
        ret_val = -ENOMEM;
        goto err_src_alloc;
    }

    // alloc array for dst pfns
    mig_args.dst = vmalloc(args->n_pages, sizeof(*mig_args.dst));
    if (!mig_args.dst)
    {
        pr_err("failed to allocate destination resources\n");
        ret_val = -ENOMEM;
        goto err_dst_alloc;
    }

    // alloc array for source pages
    spages = vmalloc(args->n_pages, sizeof(*spages));
    if (!spages)
    {
        pr_err("failed to allocate source pages array\n");
        ret_val = -ENOMEM;
        goto err_spages_alloc;
    }

    // alloc array for destionation pages
    dpages = vmalloc(args->n_pages, sizeof(*dpages));
    if (!dpages)
    {
        pr_err("failed to allocate destination pages array\n");
        ret_val = -ENOMEM;
        goto err_dpages_alloc;
    }

    // allocate the migrate_vma struct for the call to the hmm api
    mig_args.start = start;
    mig_args.end = end;
    mig_args.vma = find_vma_intersection(curr_mm, start, end);
    if (!mig_args.vma) {
        pr_err("failed to match vma\n");
        ret_val = -EFAULT;
        goto err_vma;
    }

    mig_args.pgmap_owner = d;
    mig_args.flags = MIGRATE_VMA_SELECT_SYSTEM;
    dbg_info("setting up migration...\n");
    ret_val = migrate_vma_setup(&mig_args);
    if (ret_val) {
        pr_err("failed to setup migration\n");
        goto err_mig_setup;
    }

    dbg_info("set up migration, cpages: %lu\n", mig_args.cpages);

    // invalidate
    dbg_info("invalidation started ...\n");
    tlb_unmap_hmm(d, start >> PAGE_SHIFT, mig_args.npages, args->hpid, args->hugepages);


    for (i = 0; i < mig_args.npages; i++) {
        // check if page is ready to migrate or if a new mapping might be necessary
        if (!(mig_args.src[i] & MIGRATE_PFN_MIGRATE)) {
            dbg_info("page table walk, entry %d", i);
            dpages[i] = host_ptw(start + (i << PAGE_SHIFT), args->hpid);
        } else {
            spages[i] = migrate_pfn_to_page(mig_args.src[i]);
            dbg_info("src pfn is %#lx\n", page_to_pfn(spages[i]));
            dpages[i] = alloc_private_page(d);
            dbg_info("allocated new private page, pfn: %lu\n", page_to_pfn(dpages[i]));
            
            if (!dpages[i]) {
                // when this fails we abort the whole process since it is unlikely that we will recover
                // from it when the private page allocator does not work 
                pr_err("failed to allocate a device private page\n");
                dpages_fail = true;
                continue;
            }

            // get and lock page and mark it as such in the migration args
            get_page(dpages[i]);
            lock_page(dpages[i]);
            mig_args.dst[i] = migrate_pfn(page_to_pfn(dpages[i])) | MIGRATE_PFN_LOCKED;
            if (mig_args.src[i] & MIGRATE_PFN_WRITE)
                mig_args.dst[i] |= MIGRATE_PFN_WRITE;
        }
    }

    // undo code in case of a failure, if the previous loop was successfull this will be skipped
    if (dpages_fail) {
        pr_err("invalidating all destination page entries\n");
        ret_val = -ENOMEM;
        mig_args.cpages = 0;
        for (i = 0; i < args->n_pages; i++) {
            if (!dpages[i] || mig_args.dst[i] == 0) {
                continue;
            }

            unlock_page(dpages[i]);
            put_page(dpages[i]);
            cpu_free_private_page(dpages[i]);
            mig_args.dst[i] = 0;
        }
        pr_err("restoring original page table\n");
        migrate_vma_pages(&mig_args);
        migrate_vma_finalize(&mig_args);
        goto err_dpages;
    }

    // host memory addresses
    host_address = vmalloc(mig_args.npages, sizeof(uint64_t));
    if (!host_address) {
        pr_err("failed to allocate host address array");
        ret_val = -ENOMEM;
        goto err_host_address_alloc;
    }

    // card memory addresses
    card_address = vmalloc(mig_args.npages, sizeof(uint64_t));
    if (!card_address) {
        pr_err("failed to allocate card address array");
        ret_val = -ENOMEM;
        goto err_card_address_alloc;
    }

    // allocate card memory
    if(mig_args.cpages > 0) {
        calloc = vmalloc(mig_args.cpages, sizeof(uint64_t));
        BUG_ON(!calloc);

        ret_val = alloc_card_memory(d, calloc, mig_args.cpages, args->hugepages);
        if (ret_val) {
            pr_err("could not allocate card pages\n");
            ret_val = -ENOMEM;
            goto err_card_alloc;
        }
    }

    // zone setup
    j = 0;
    for (i = 0; i < mig_args.cpages; i++) {
        new_entry = kzalloc(sizeof(struct hmm_prvt_info), GFP_KERNEL);
        BUG_ON(!new_entry);

        new_entry->ctid = args->ctid;
        new_entry->huge = args->hugepages;
        new_entry->card_address = calloc[i];
        while(!dpages[j] || mig_args.dst[i] == 0) {
            j++;
        } 

        list_add(&new_entry->entry, &migrated_pages[d->id][args->ctid]);
        dpages[j]->zone_device_data = new_entry;
    }

    // set physical addresses
    for(i = 0; i < args->n_pages; i++) {
        if (spages[i]) {
            host_address[i] = page_to_pfn(spages[i]);
        }
        if (dpages[i]) {
            card_address[i] =((struct hmm_prvt_info *)(dpages[i]->zone_device_data))->card_address;
        }
    }

    // wait for completion
    wait_event_interruptible(d->waitqueue_invldt, atomic_read(&d->wait_invldt) == FLAG_SET);
    atomic_set(&d->wait_invldt, FLAG_CLR);

    // dma
    if(mig_args.cpages > 0) {
        // lock
        mutex_lock(&d->offload_lock);

        // dma operation
        dbg_info("starting dma ... \n");
        trigger_dma_offload(d, host_address, card_address, mig_args.npages, args->hugepages);

        // wait for completion
        wait_event_interruptible(d->waitqueue_offload, atomic_read(&d->wait_offload) == FLAG_SET);
        atomic_set(&d->wait_offload, FLAG_CLR);
        dbg_info("dma offload completed\n");

        // unlock
        mutex_unlock(&d->offload_lock);
    }

    // swap pages
    dbg_info("swap out pages\n");
    migrate_vma_pages(&mig_args);
    dbg_info("migrated pages, cpages %lu\n", mig_args.cpages);

    // finalize migration
    dbg_info("finalizing migration ... ");
    migrate_vma_finalize(&mig_args);
    dbg_info("finalized migration, cpages %lu\n", mig_args.cpages);

    // tlb map operation
    tlb_map_hmm(d, start >> PAGE_SHIFT, card_address, mig_args.npages, CARD_ACCESS, args->ctid, args->hpid, args->hugepages);

    if(mig_args.cpages > 0)
        vfree(calloc);
err_card_alloc:
    vfree(card_address);
err_card_address_alloc:
    vfree(host_address);
err_host_address_alloc:
err_dpages:
err_mig_setup:
err_vma:
    vfree(dpages);
err_dpages_alloc:
    vfree(spages);
err_spages_alloc:
    vfree(mig_args.dst);
err_dst_alloc:
    vfree(mig_args.src);
err_src_alloc:
    return ret_val;
}

//
// Mapping
// 

/**
 * @brief Create a TLB mapping
 * 
 * @param d - vFPGA
 * @param pfa - aligned read page fault
 * @param user_pg - mapping
 * @param hpid - host pid
*/
void tlb_map_hmm(struct vfpga_dev *d, uint64_t vaddr, uint64_t *paddr, uint32_t n_pages, int32_t host, int32_t ctid, pid_t hpid, bool huge) 
{
    int i;
    struct bus_driver_data *bd_data = d->bd_data;
    uint32_t pg_inc;
    uint32_t n_map_pages;
    uint64_t tmp_vaddr;

    pg_inc = huge ? bd_data->n_pages_in_huge : 1;
    n_map_pages = huge ? n_pages >> bd_data->dif_order_page_shift : n_pages;

    // fill mappings
    tmp_vaddr = vaddr;
    for (i = 0; (i < n_map_pages) && (i < MAX_N_MAP_PAGES); i+=pg_inc) {
        if(paddr[i] == 0)
            continue;
        
        create_tlb_mapping(d, huge ? bd_data->ltlb_meta : bd_data->stlb_meta, tmp_vaddr, paddr[i], host, ctid, hpid);
        
        tmp_vaddr += pg_inc;
    }
}

/**
 * @brief Create unmapping
 * 
 * @param d - vFPGA
 * @param user_pg - mapping
 * @param hpid - host pid
*/
void tlb_unmap_hmm(struct vfpga_dev *d, uint64_t vaddr, uint32_t n_pages, pid_t hpid, bool huge) 
{
    int i, j;
    struct bus_driver_data *bd_data = d->bd_data;
    uint32_t n_map_pages;
    uint64_t *map_array;
    uint32_t pg_inc;
    uint64_t tmp_vaddr;

    pg_inc = huge ? bd_data->n_pages_in_huge : 1;
    n_map_pages = huge ? n_pages >> bd_data->dif_order_page_shift : n_pages;

    tmp_vaddr = vaddr;
    for (i = 0; i < n_map_pages; i+=pg_inc) {
        create_tlb_unmapping(d, huge ? bd_data->ltlb_meta : bd_data->stlb_meta, tmp_vaddr, hpid);
        
        tmp_vaddr += pg_inc;
    }

    // invalidate command
    tmp_vaddr = vaddr;
    for (i = 0; i < n_map_pages; i+=pg_inc) {
        invalidate_tlb_entry(d, tmp_vaddr, pg_inc, hpid, i == (n_map_pages - pg_inc));
        tmp_vaddr += pg_inc;
    }
    
    /*
    // wait for completion
    wait_event_interruptible(d->waitqueue_invldt, atomic_read(&d->wait_invldt) == FLAG_SET);
    atomic_set(&d->wait_invldt, FLAG_CLR);
    */
}

//
// Alloc and checks
//

/**
 * @brief Clear operation. If the ctid (or the whole pid) leaves
 * we want to free all the card memory held by this process.
 *
 * @param d
 * @param ctid
 */
void free_card_mem(struct vfpga_dev *d, int ctid)
{
    struct hmm_prvt_info *info, *tmp;
    struct bus_driver_data *bd_data;

    BUG_ON(!d);
    bd_data = d->bd_data;
    BUG_ON(!bd_data);

    dbg_info("Freeing card memory, prev: %p, next: %p\n", migrated_pages[d->id][ctid].prev, migrated_pages[d->id][ctid].next);

    list_for_each_entry_safe(info, tmp, &migrated_pages[d->id][ctid], entry)
    {
        free_card_memory(d, &info->card_address, 1, info->huge);
        list_del(&info->entry);
    }
}

/**
 * @brief Free all memory regions we allocated for
 * private pages.
 *
 * @param bd_data
 */
void free_mem_regions(struct bus_driver_data *bd_data)
{
    struct hmm_prvt_chunk *tmp_entry, *n;
    int i = 0;
    struct vfpga_dev *d;
    BUG_ON(!bd_data);

    dbg_info("freeing mem regions\n");

    for (i = 0; i < bd_data->n_fpga_reg; i++)
    {
        d = &bd_data->vfpga_dev[i];
        list_for_each_entry_safe(tmp_entry, n, &d->mem_sections, list)
        {
            memunmap_pages(&tmp_entry->pagemap);
            release_mem_region(tmp_entry->resource->start, range_len(&tmp_entry->pagemap.range));
            list_del(&tmp_entry->list);
            kfree(tmp_entry);
        }
    }
}

/**
 * @brief Free a device private_memory page
 *
 * @param page - device private page
 */
void cpu_free_private_page(struct page *page)
{
    struct vfpga_dev *d;

    d = container_of(page->pgmap, struct hmm_prvt_chunk, pagemap)->d;
    BUG_ON(!d);
    spin_lock(&d->page_lock);
    page->zone_device_data = d->free_pages;
    d->free_pages = page;
    spin_unlock(&d->page_lock);
}

/**
 * @brief Allocate a private page to swap out to during the migration
 * 
 * @param d - vFPGA
 * @return page* - allocated page
*/
struct page *alloc_private_page(struct vfpga_dev *d)
{
    int ret_val = 0;
    struct page *dpage;

    if (!d->free_pages) {
        ret_val = alloc_new_prvt_pages(d);
        if (ret_val) {
            spin_unlock(&d->page_lock);
            pr_err("cannot allocate additional device private pages\n");
            return NULL;
        }
    }

    spin_lock(&d->page_lock);
    dpage = d->free_pages;
    d->free_pages = dpage->zone_device_data;
    dpage->zone_device_data = NULL;
    spin_unlock(&d->page_lock);

    return dpage;
}

/**
 * @brief Allocates a new set of device private pages (zone private info)
 * Refill only!
 *
 * @param d - vFPGA
 * @return int - ret_val
 */
int alloc_new_prvt_pages(struct vfpga_dev *d)
{
    int ret_val = 0;
    int i;
    uint64_t n_pages;
    struct hmm_prvt_chunk *devmem;
    struct resource *res;
    struct page *page;
    void *ret_ptr;

    devmem = kzalloc(sizeof(*devmem), GFP_KERNEL);
    if (!devmem)
    {
        pr_err("Cannot allocate devmem mngmt struct\n");
        ret_val = -ENOMEM;
        goto err_region;
    }

    res = request_free_mem_region(&iomem_resource, DEVMEM_CHUNK_SIZE,
                                  "hmm_devmem");
    if (IS_ERR_OR_NULL(res))
    {
        pr_err("cannot obtain private pages memory\n");
        ret_val = -ENOMEM;
        goto err_resource;
    }

    devmem->pagemap.type = MEMORY_DEVICE_PRIVATE;
    devmem->pagemap.range.start = res->start;
    devmem->pagemap.range.end = res->end;
    devmem->pagemap.nr_range = 1;
    devmem->pagemap.ops = &cyt_devmem_ops;
    devmem->pagemap.owner = d;
    devmem->resource = res;
    devmem->d = d;

    dbg_info("allocated resource: [%#llx-%#llx]\n", res->start, res->end);

    ret_ptr = memremap_pages(&devmem->pagemap, numa_node_id());
    if (IS_ERR(ret_ptr))
    {
        pr_err("cannot remap private pages\n");
        goto err_remap;
    }

    // add new section to list
    spin_lock(&d->sections_lock);
    list_add(&devmem->list, &d->mem_sections);
    spin_unlock(&d->sections_lock);

    // add all allocated pages to allocator
    page = pfn_to_page(devmem->resource->start >> PAGE_SHIFT);
    n_pages = range_len(&devmem->pagemap.range) >> PAGE_SHIFT;
    spin_lock(&d->page_lock);
    for (i = 0; i < n_pages; i++)
    {
        page->zone_device_data = d->free_pages;
        d->free_pages = page;
        page++;
    }
    spin_unlock(&d->page_lock);

    return ret_val;

err_remap:
    release_mem_region(devmem->pagemap.range.start, range_len(&devmem->pagemap.range));
err_resource:
    kfree(devmem);
err_region:
    return ret_val;
}


/**
 * @brief Checks if a given range is backed by a
 * transparent huge page.
 *
 * @param vma virtual memory area
 * @param addr address to be checked
 * @return int non zero if backed by thp
 */
int is_thp(struct vm_area_struct *vma, unsigned long addr, int *locked)
{
    struct page *pages[1];
    bool res = false;
    get_user_pages_remote(vma->vm_mm, addr, 1, 1, pages, NULL, locked);
    // if (!locked)
    //     pr_warn("lock got dropped\n");
    res = is_transparent_hugepage(pages[0]);
    put_page(pages[0]);
    return res;
}

#endif

