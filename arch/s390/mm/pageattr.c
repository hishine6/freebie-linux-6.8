// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corp. 2011
 * Author(s): Jan Glauber <jang@linux.vnet.ibm.com>
 */
#include <linux/hugetlb.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <asm/cacheflush.h>
#include <asm/facility.h>
#include <asm/pgalloc.h>
#include <asm/kfence.h>
#include <asm/page.h>
#include <asm/set_memory.h>

static inline unsigned long sske_frame(unsigned long addr, unsigned char skey)
{
	asm volatile(".insn rrf,0xb22b0000,%[skey],%[addr],1,0"
		     : [addr] "+a" (addr) : [skey] "d" (skey));
	return addr;
}

void __storage_key_init_range(unsigned long start, unsigned long end)
{
	unsigned long boundary, size;

	while (start < end) {
		if (MACHINE_HAS_EDAT1) {
			/* set storage keys for a 1MB frame */
			size = 1UL << 20;
			boundary = (start + size) & ~(size - 1);
			if (boundary <= end) {
				do {
					start = sske_frame(start, PAGE_DEFAULT_KEY);
				} while (start < boundary);
				continue;
			}
		}
		page_set_storage_key(start, PAGE_DEFAULT_KEY, 1);
		start += PAGE_SIZE;
	}
}

#ifdef CONFIG_PROC_FS
atomic_long_t __bootdata_preserved(direct_pages_count[PG_DIRECT_MAP_MAX]);

void arch_report_meminfo(struct seq_file *m)
{
	seq_printf(m, "DirectMap4k:    %8lu kB\n",
		   atomic_long_read(&direct_pages_count[PG_DIRECT_MAP_4K]) << 2);
	seq_printf(m, "DirectMap1M:    %8lu kB\n",
		   atomic_long_read(&direct_pages_count[PG_DIRECT_MAP_1M]) << 10);
	seq_printf(m, "DirectMap2G:    %8lu kB\n",
		   atomic_long_read(&direct_pages_count[PG_DIRECT_MAP_2G]) << 21);
}
#endif /* CONFIG_PROC_FS */

static void pgt_set(unsigned long *old, unsigned long new, unsigned long addr,
		    unsigned long dtt)
{
	unsigned long *table, mask;

	mask = 0;
	if (MACHINE_HAS_EDAT2) {
		switch (dtt) {
		case CRDTE_DTT_REGION3:
			mask = ~(PTRS_PER_PUD * sizeof(pud_t) - 1);
			break;
		case CRDTE_DTT_SEGMENT:
			mask = ~(PTRS_PER_PMD * sizeof(pmd_t) - 1);
			break;
		case CRDTE_DTT_PAGE:
			mask = ~(PTRS_PER_PTE * sizeof(pte_t) - 1);
			break;
		}
		table = (unsigned long *)((unsigned long)old & mask);
		crdte(*old, new, table, dtt, addr, S390_lowcore.kernel_asce.val);
	} else if (MACHINE_HAS_IDTE) {
		cspg(old, *old, new);
	} else {
		csp((unsigned int *)old + 1, *old, new);
	}
}

static int walk_pte_level(pmd_t *pmdp, unsigned long addr, unsigned long end,
			  unsigned long flags)
{
	pte_t *ptep, new;

	if (flags == SET_MEMORY_4K)
		return 0;
	ptep = pte_offset_kernel(pmdp, addr);
	do {
		new = *ptep;
		if (pte_none(new))
			return -EINVAL;
		if (flags & SET_MEMORY_RO)
			new = pte_wrprotect(new);
		else if (flags & SET_MEMORY_RW)
			new = pte_mkwrite_novma(pte_mkdirty(new));
		if (flags & SET_MEMORY_NX)
			new = set_pte_bit(new, __pgprot(_PAGE_NOEXEC));
		else if (flags & SET_MEMORY_X)
			new = clear_pte_bit(new, __pgprot(_PAGE_NOEXEC));
		if (flags & SET_MEMORY_INV) {
			new = set_pte_bit(new, __pgprot(_PAGE_INVALID));
		} else if (flags & SET_MEMORY_DEF) {
			new = __pte(pte_val(new) & PAGE_MASK);
			new = set_pte_bit(new, PAGE_KERNEL);
			if (!MACHINE_HAS_NX)
				new = clear_pte_bit(new, __pgprot(_PAGE_NOEXEC));
		}
		pgt_set((unsigned long *)ptep, pte_val(new), addr, CRDTE_DTT_PAGE);
		ptep++;
		addr += PAGE_SIZE;
		cond_resched();
	} while (addr < end);
	return 0;
}

static int split_pmd_page(pmd_t *pmdp, unsigned long addr)
{
	unsigned long pte_addr, prot;
	pte_t *pt_dir, *ptep;
	pmd_t new;
	int i, ro, nx;

	pt_dir = vmem_pte_alloc();
	if (!pt_dir)
		return -ENOMEM;
	pte_addr = pmd_pfn(*pmdp) << PAGE_SHIFT;
	ro = !!(pmd_val(*pmdp) & _SEGMENT_ENTRY_PROTECT);
	nx = !!(pmd_val(*pmdp) & _SEGMENT_ENTRY_NOEXEC);
	prot = pgprot_val(ro ? PAGE_KERNEL_RO : PAGE_KERNEL);
	if (!nx)
		prot &= ~_PAGE_NOEXEC;
	ptep = pt_dir;
	for (i = 0; i < PTRS_PER_PTE; i++) {
		set_pte(ptep, __pte(pte_addr | prot));
		pte_addr += PAGE_SIZE;
		ptep++;
	}
	new = __pmd(__pa(pt_dir) | _SEGMENT_ENTRY);
	pgt_set((unsigned long *)pmdp, pmd_val(new), addr, CRDTE_DTT_SEGMENT);
	update_page_count(PG_DIRECT_MAP_4K, PTRS_PER_PTE);
	update_page_count(PG_DIRECT_MAP_1M, -1);
	return 0;
}

static void modify_pmd_page(pmd_t *pmdp, unsigned long addr,
			    unsigned long flags)
{
	pmd_t new = *pmdp;

	if (flags & SET_MEMORY_RO)
		new = pmd_wrprotect(new);
	else if (flags & SET_MEMORY_RW)
		new = pmd_mkwrite_novma(pmd_mkdirty(new));
	if (flags & SET_MEMORY_NX)
		new = set_pmd_bit(new, __pgprot(_SEGMENT_ENTRY_NOEXEC));
	else if (flags & SET_MEMORY_X)
		new = clear_pmd_bit(new, __pgprot(_SEGMENT_ENTRY_NOEXEC));
	if (flags & SET_MEMORY_INV) {
		new = set_pmd_bit(new, __pgprot(_SEGMENT_ENTRY_INVALID));
	} else if (flags & SET_MEMORY_DEF) {
		new = __pmd(pmd_val(new) & PMD_MASK);
		new = set_pmd_bit(new, SEGMENT_KERNEL);
		if (!MACHINE_HAS_NX)
			new = clear_pmd_bit(new, __pgprot(_SEGMENT_ENTRY_NOEXEC));
	}
	pgt_set((unsigned long *)pmdp, pmd_val(new), addr, CRDTE_DTT_SEGMENT);
}

static int walk_pmd_level(pud_t *pudp, unsigned long addr, unsigned long end,
			  unsigned long flags)
{
	unsigned long next;
	int need_split;
	pmd_t *pmdp;
	int rc = 0;

	pmdp = pmd_offset(pudp, addr);
	do {
		if (pmd_none(*pmdp))
			return -EINVAL;
		next = pmd_addr_end(addr, end);
		if (pmd_large(*pmdp)) {
			need_split  = !!(flags & SET_MEMORY_4K);
			need_split |= !!(addr & ~PMD_MASK);
			need_split |= !!(addr + PMD_SIZE > next);
			if (need_split) {
				rc = split_pmd_page(pmdp, addr);
				if (rc)
					return rc;
				continue;
			}
			modify_pmd_page(pmdp, addr, flags);
		} else {
			rc = walk_pte_level(pmdp, addr, next, flags);
			if (rc)
				return rc;
		}
		pmdp++;
		addr = next;
		cond_resched();
	} while (addr < end);
	return rc;
}

static int split_pud_page(pud_t *pudp, unsigned long addr)
{
	unsigned long pmd_addr, prot;
	pmd_t *pm_dir, *pmdp;
	pud_t new;
	int i, ro, nx;

	pm_dir = vmem_crst_alloc(_SEGMENT_ENTRY_EMPTY);
	if (!pm_dir)
		return -ENOMEM;
	pmd_addr = pud_pfn(*pudp) << PAGE_SHIFT;
	ro = !!(pud_val(*pudp) & _REGION_ENTRY_PROTECT);
	nx = !!(pud_val(*pudp) & _REGION_ENTRY_NOEXEC);
	prot = pgprot_val(ro ? SEGMENT_KERNEL_RO : SEGMENT_KERNEL);
	if (!nx)
		prot &= ~_SEGMENT_ENTRY_NOEXEC;
	pmdp = pm_dir;
	for (i = 0; i < PTRS_PER_PMD; i++) {
		set_pmd(pmdp, __pmd(pmd_addr | prot));
		pmd_addr += PMD_SIZE;
		pmdp++;
	}
	new = __pud(__pa(pm_dir) | _REGION3_ENTRY);
	pgt_set((unsigned long *)pudp, pud_val(new), addr, CRDTE_DTT_REGION3);
	update_page_count(PG_DIRECT_MAP_1M, PTRS_PER_PMD);
	update_page_count(PG_DIRECT_MAP_2G, -1);
	return 0;
}

static void modify_pud_page(pud_t *pudp, unsigned long addr,
			    unsigned long flags)
{
	pud_t new = *pudp;

	if (flags & SET_MEMORY_RO)
		new = pud_wrprotect(new);
	else if (flags & SET_MEMORY_RW)
		new = pud_mkwrite(pud_mkdirty(new));
	if (flags & SET_MEMORY_NX)
		new = set_pud_bit(new, __pgprot(_REGION_ENTRY_NOEXEC));
	else if (flags & SET_MEMORY_X)
		new = clear_pud_bit(new, __pgprot(_REGION_ENTRY_NOEXEC));
	if (flags & SET_MEMORY_INV) {
		new = set_pud_bit(new, __pgprot(_REGION_ENTRY_INVALID));
	} else if (flags & SET_MEMORY_DEF) {
		new = __pud(pud_val(new) & PUD_MASK);
		new = set_pud_bit(new, REGION3_KERNEL);
		if (!MACHINE_HAS_NX)
			new = clear_pud_bit(new, __pgprot(_REGION_ENTRY_NOEXEC));
	}
	pgt_set((unsigned long *)pudp, pud_val(new), addr, CRDTE_DTT_REGION3);
}

static int walk_pud_level(p4d_t *p4d, unsigned long addr, unsigned long end,
			  unsigned long flags)
{
	unsigned long next;
	int need_split;
	pud_t *pudp;
	int rc = 0;

	pudp = pud_offset(p4d, addr);
	do {
		if (pud_none(*pudp))
			return -EINVAL;
		next = pud_addr_end(addr, end);
		if (pud_leaf(*pudp)) {
			need_split  = !!(flags & SET_MEMORY_4K);
			need_split |= !!(addr & ~PUD_MASK);
			need_split |= !!(addr + PUD_SIZE > next);
			if (need_split) {
				rc = split_pud_page(pudp, addr);
				if (rc)
					break;
				continue;
			}
			modify_pud_page(pudp, addr, flags);
		} else {
			rc = walk_pmd_level(pudp, addr, next, flags);
		}
		pudp++;
		addr = next;
		cond_resched();
	} while (addr < end && !rc);
	return rc;
}

static int walk_p4d_level(pgd_t *pgd, unsigned long addr, unsigned long end,
			  unsigned long flags)
{
	unsigned long next;
	p4d_t *p4dp;
	int rc = 0;

	p4dp = p4d_offset(pgd, addr);
	do {
		if (p4d_none(*p4dp))
			return -EINVAL;
		next = p4d_addr_end(addr, end);
		rc = walk_pud_level(p4dp, addr, next, flags);
		p4dp++;
		addr = next;
		cond_resched();
	} while (addr < end && !rc);
	return rc;
}

DEFINE_MUTEX(cpa_mutex);

static int change_page_attr(unsigned long addr, unsigned long end,
			    unsigned long flags)
{
	unsigned long next;
	int rc = -EINVAL;
	pgd_t *pgdp;

	pgdp = pgd_offset_k(addr);
	do {
		if (pgd_none(*pgdp))
			break;
		next = pgd_addr_end(addr, end);
		rc = walk_p4d_level(pgdp, addr, next, flags);
		if (rc)
			break;
		cond_resched();
	} while (pgdp++, addr = next, addr < end && !rc);
	return rc;
}

static int change_page_attr_alias(unsigned long addr, unsigned long end,
				  unsigned long flags)
{
	unsigned long alias, offset, va_start, va_end;
	struct vm_struct *area;
	int rc = 0;

	/*
	 * Changes to read-only permissions on kernel VA mappings are also
	 * applied to the kernel direct mapping. Execute permissions are
	 * intentionally not transferred to keep all allocated pages within
	 * the direct mapping non-executable.
	 */
	flags &= SET_MEMORY_RO | SET_MEMORY_RW;
	if (!flags)
		return 0;
	area = NULL;
	while (addr < end) {
		if (!area)
			area = find_vm_area((void *)addr);
		if (!area || !(area->flags & VM_ALLOC))
			return 0;
		va_start = (unsigned long)area->addr;
		va_end = va_start + area->nr_pages * PAGE_SIZE;
		offset = (addr - va_start) >> PAGE_SHIFT;
		alias = (unsigned long)page_address(area->pages[offset]);
		rc = change_page_attr(alias, alias + PAGE_SIZE, flags);
		if (rc)
			break;
		addr += PAGE_SIZE;
		if (addr >= va_end)
			area = NULL;
	}
	return rc;
}

int __set_memory(unsigned long addr, unsigned long numpages, unsigned long flags)
{
	unsigned long end;
	int rc;

	if (!MACHINE_HAS_NX)
		flags &= ~(SET_MEMORY_NX | SET_MEMORY_X);
	if (!flags)
		return 0;
	if (!numpages)
		return 0;
	addr &= PAGE_MASK;
	end = addr + numpages * PAGE_SIZE;
	mutex_lock(&cpa_mutex);
	rc = change_page_attr(addr, end, flags);
	if (rc)
		goto out;
	rc = change_page_attr_alias(addr, end, flags);
out:
	mutex_unlock(&cpa_mutex);
	return rc;
}

int set_direct_map_invalid_noflush(struct page *page)
{
	return __set_memory((unsigned long)page_to_virt(page), 1, SET_MEMORY_INV);
}

int set_direct_map_default_noflush(struct page *page)
{
	return __set_memory((unsigned long)page_to_virt(page), 1, SET_MEMORY_DEF);
}

bool kernel_page_present(struct page *page)
{
	unsigned long addr;
	unsigned int cc;

	addr = (unsigned long)page_address(page);
	asm volatile(
		"	lra	%[addr],0(%[addr])\n"
		"	ipm	%[cc]\n"
		: [cc] "=d" (cc), [addr] "+a" (addr)
		:
		: "cc");
	return (cc >> 28) == 0;
}

#if defined(CONFIG_DEBUG_PAGEALLOC) || defined(CONFIG_KFENCE)

static void ipte_range(pte_t *pte, unsigned long address, int nr)
{
	int i;

	if (test_facility(13)) {
		__ptep_ipte_range(address, nr - 1, pte, IPTE_GLOBAL);
		return;
	}
	for (i = 0; i < nr; i++) {
		__ptep_ipte(address, pte, 0, 0, IPTE_GLOBAL);
		address += PAGE_SIZE;
		pte++;
	}
}

void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	unsigned long address;
	pte_t *ptep, pte;
	int nr, i, j;

	for (i = 0; i < numpages;) {
		address = (unsigned long)page_to_virt(page + i);
		ptep = virt_to_kpte(address);
		nr = (unsigned long)ptep >> ilog2(sizeof(long));
		nr = PTRS_PER_PTE - (nr & (PTRS_PER_PTE - 1));
		nr = min(numpages - i, nr);
		if (enable) {
			for (j = 0; j < nr; j++) {
				pte = clear_pte_bit(*ptep, __pgprot(_PAGE_INVALID));
				set_pte(ptep, pte);
				address += PAGE_SIZE;
				ptep++;
			}
		} else {
			ipte_range(ptep, address, nr);
		}
		i += nr;
	}
}

#endif /* CONFIG_DEBUG_PAGEALLOC */
