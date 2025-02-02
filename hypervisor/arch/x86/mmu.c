/*-
 * Copyright (c) 2011 NetApp, Inc.
 * Copyright (c) 2017 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <hypervisor.h>
#include <reloc.h>
#include <e820.h>

static void *ppt_mmu_pml4_addr;
static uint8_t sanitized_page[PAGE_SIZE] __aligned(PAGE_SIZE);

static struct vmx_capability {
	uint32_t ept;
	uint32_t vpid;
} vmx_caps;

/*
 * If the logical processor is in VMX non-root operation and
 * the "enable VPID" VM-execution control is 1, the current VPID
 * is the value of the VPID VM-execution control field in the VMCS.
 * (VM entry ensures that this value is never 0000H).
 */
static uint16_t vmx_vpid_nr = VMX_MIN_NR_VPID;

#define INVEPT_TYPE_SINGLE_CONTEXT      1UL
#define INVEPT_TYPE_ALL_CONTEXTS        2UL
#define VMFAIL_INVALID_EPT_VPID				\
	"       jnc 1f\n"				\
	"       mov $1, %0\n"      /* CF: error = 1 */	\
	"       jmp 3f\n"				\
	"1:     jnz 2f\n"				\
	"       mov $2, %0\n"      /* ZF: error = 2 */	\
	"       jmp 3f\n"				\
	"2:     mov $0, %0\n"				\
	"3:"

struct invept_desc {
	uint64_t eptp;
	uint64_t res;
};

static inline void local_invvpid(uint64_t type, uint16_t vpid, uint64_t gva)
{
	int32_t error = 0;

	struct {
		uint32_t vpid : 16;
		uint32_t rsvd1 : 16;
		uint32_t rsvd2 : 32;
		uint64_t gva;
	} operand = { vpid, 0U, 0U, gva };

	asm volatile ("invvpid %1, %2\n"
			VMFAIL_INVALID_EPT_VPID
			: "=r" (error)
			: "m" (operand), "r" (type)
			: "memory");

	ASSERT(error == 0, "invvpid error");
}

static inline void local_invept(uint64_t type, struct invept_desc desc)
{
	int32_t error = 0;

	asm volatile ("invept %1, %2\n"
			VMFAIL_INVALID_EPT_VPID
			: "=r" (error)
			: "m" (desc), "r" (type)
			: "memory");

	ASSERT(error == 0, "invept error");
}

static inline bool cpu_has_vmx_ept_cap(uint32_t bit_mask)
{
	return ((vmx_caps.ept & bit_mask) != 0U);
}

static inline bool cpu_has_vmx_vpid_cap(uint32_t bit_mask)
{
	return ((vmx_caps.vpid & bit_mask) != 0U);
}

int32_t check_vmx_mmu_cap(void)
{
	uint64_t val;

	/* Read the MSR register of EPT and VPID Capability -  SDM A.10 */
	val = msr_read(MSR_IA32_VMX_EPT_VPID_CAP);
	vmx_caps.ept = (uint32_t) val;
	vmx_caps.vpid = (uint32_t) (val >> 32U);

	if (!cpu_has_vmx_ept_cap(VMX_EPT_INVEPT)) {
		pr_fatal("%s, invept not supported\n", __func__);
		return -ENODEV;
	}

	if (!cpu_has_vmx_vpid_cap(VMX_VPID_INVVPID) ||
		!cpu_has_vmx_vpid_cap(VMX_VPID_INVVPID_SINGLE_CONTEXT) ||
		!cpu_has_vmx_vpid_cap(VMX_VPID_INVVPID_GLOBAL_CONTEXT)) {
		pr_fatal("%s, invvpid not supported\n", __func__);
		return -ENODEV;
	}

	if (!cpu_has_vmx_ept_cap(VMX_EPT_1GB_PAGE)) {
		pr_fatal("%s, ept not support 1GB large page\n", __func__);
		return -ENODEV;
	}

	return 0;
}

uint16_t allocate_vpid(void)
{
	uint16_t vpid = atomic_xadd16(&vmx_vpid_nr, 1U);

	/* TODO: vpid overflow */
	if (vpid >= VMX_MAX_NR_VPID) {
		pr_err("%s, vpid overflow\n", __func__);
		/*
		 * set vmx_vpid_nr to VMX_MAX_NR_VPID to disable vpid
		 * since next atomic_xadd16 will always large than
		 * VMX_MAX_NR_VPID.
		 */
		vmx_vpid_nr = VMX_MAX_NR_VPID;
		vpid = 0U;
	}

	return vpid;
}

void flush_vpid_single(uint16_t vpid)
{
	if (vpid != 0U) {
		local_invvpid(VMX_VPID_TYPE_SINGLE_CONTEXT, vpid, 0UL);
	}
}

void flush_vpid_global(void)
{
	local_invvpid(VMX_VPID_TYPE_ALL_CONTEXT, 0U, 0UL);
}

void invept(const struct acrn_vcpu *vcpu)
{
	struct invept_desc desc = {0};

	if (cpu_has_vmx_ept_cap(VMX_EPT_INVEPT_SINGLE_CONTEXT)) {
		desc.eptp = hva2hpa(vcpu->vm->arch_vm.nworld_eptp) |
				(3UL << 3U) | 6UL;
		local_invept(INVEPT_TYPE_SINGLE_CONTEXT, desc);
		if (vcpu->vm->sworld_control.flag.active != 0UL) {
			desc.eptp = hva2hpa(vcpu->vm->arch_vm.sworld_eptp)
				| (3UL << 3U) | 6UL;
			local_invept(INVEPT_TYPE_SINGLE_CONTEXT, desc);
		}
	} else if (cpu_has_vmx_ept_cap(VMX_EPT_INVEPT_GLOBAL_CONTEXT)) {
		local_invept(INVEPT_TYPE_ALL_CONTEXTS, desc);
	} else {
		/* Neither type of INVEPT is supported. Skip. */
	}
}

static inline uint64_t get_sanitized_page(void)
{
	return hva2hpa(sanitized_page);
}

void sanitize_pte_entry(uint64_t *ptep)
{
	set_pgentry(ptep, get_sanitized_page());
}

void sanitize_pte(uint64_t *pt_page)
{
	uint64_t i;
	for (i = 0UL; i < PTRS_PER_PTE; i++) {
		sanitize_pte_entry(pt_page + i);
	}
}

void enable_paging(void)
{
	uint64_t tmp64 = 0UL;

	/*
	 * Enable MSR IA32_EFER.NXE bit,to prevent
	 * instruction fetching from pages with XD bit set.
	 */
	tmp64 = msr_read(MSR_IA32_EFER);
	tmp64 |= MSR_IA32_EFER_NXE_BIT;
	msr_write(MSR_IA32_EFER, tmp64);

	/* Enable Write Protect, inhibiting writing to read-only pages */
	CPU_CR_READ(cr0, &tmp64);
	CPU_CR_WRITE(cr0, tmp64 | CR0_WP);

	CPU_CR_WRITE(cr3, hva2hpa(ppt_mmu_pml4_addr));

}

void enable_smep(void)
{
	uint64_t val64 = 0UL;

	/* Enable CR4.SMEP*/
	CPU_CR_READ(cr4, &val64);
	CPU_CR_WRITE(cr4, val64 | CR4_SMEP);
}

void enable_smap(void)
{
	uint64_t val64 = 0UL;

	/* Enable CR4.SMAP*/
	CPU_CR_READ(cr4, &val64);
	CPU_CR_WRITE(cr4, val64 | CR4_SMAP);
}

/*
 * Update memory pages to be owned by hypervisor.
 */
void hv_access_memory_region_update(uint64_t base, uint64_t size)
{
	mmu_modify_or_del((uint64_t *)ppt_mmu_pml4_addr, (base & PDE_MASK),
		((size + PDE_SIZE - 1UL) & PDE_MASK), 0UL, PAGE_USER,
		&ppt_mem_ops, MR_MODIFY);
}

void init_paging(void)
{
	uint64_t hv_hpa, text_end, size;
	uint32_t i;
	uint64_t low32_max_ram = 0UL;
	uint64_t high64_max_ram;
	uint64_t attr_uc = (PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_CACHE_UC | PAGE_NX);

	const struct e820_entry *entry;
	uint32_t entries_count = get_e820_entries_count();
	const struct e820_entry *p_e820 = get_e820_entry();
	const struct e820_mem_params *p_e820_mem_info = get_e820_mem_info();

	pr_dbg("HV MMU Initialization");

	/* align to 2MB */
	high64_max_ram = (p_e820_mem_info->mem_top + PDE_SIZE - 1UL) & PDE_MASK;
	if ((high64_max_ram > (CONFIG_PLATFORM_RAM_SIZE + PLATFORM_LO_MMIO_SIZE)) ||
			(high64_max_ram < (1UL << 32U))) {
		panic("Please configure HV_ADDRESS_SPACE correctly!\n");
	}

	/* Allocate memory for Hypervisor PML4 table */
	ppt_mmu_pml4_addr = ppt_mem_ops.get_pml4_page(ppt_mem_ops.info);

	/* Map all memory regions to UC attribute */
	mmu_add((uint64_t *)ppt_mmu_pml4_addr, 0UL, 0UL, high64_max_ram - 0UL, attr_uc, &ppt_mem_ops);

	/* Modify WB attribute for E820_TYPE_RAM */
	for (i = 0U; i < entries_count; i++) {
		entry = p_e820 + i;
		if (entry->type == E820_TYPE_RAM) {
			if (entry->baseaddr < (1UL << 32U)) {
				uint64_t end = entry->baseaddr + entry->length;
				if ((end < (1UL << 32U)) && (end > low32_max_ram)) {
					low32_max_ram = end;
				}
			}
		}
	}

	mmu_modify_or_del((uint64_t *)ppt_mmu_pml4_addr, 0UL, (low32_max_ram + PDE_SIZE - 1UL) & PDE_MASK,
			PAGE_CACHE_WB, PAGE_CACHE_MASK, &ppt_mem_ops, MR_MODIFY);

	mmu_modify_or_del((uint64_t *)ppt_mmu_pml4_addr, (1UL << 32U), high64_max_ram - (1UL << 32U),
			PAGE_CACHE_WB, PAGE_CACHE_MASK, &ppt_mem_ops, MR_MODIFY);

	/*
	 * set the paging-structure entries' U/S flag to supervisor-mode for hypervisor owned memroy.
	 * (exclude the memory reserve for trusty)
	 */
	hv_hpa = get_hv_image_base();
	mmu_modify_or_del((uint64_t *)ppt_mmu_pml4_addr, hv_hpa & PDE_MASK,
			CONFIG_HV_RAM_SIZE + (((hv_hpa & (PDE_SIZE - 1UL)) != 0UL) ? PDE_SIZE : 0UL),
			PAGE_CACHE_WB, PAGE_CACHE_MASK | PAGE_USER, &ppt_mem_ops, MR_MODIFY);

	size = ((uint64_t)&ld_text_end - CONFIG_HV_RAM_START);
	text_end = hv_hpa + size;
	/*round up 'text_end' to 2MB aligned.*/
	text_end = (text_end + PDE_SIZE - 1UL) & PDE_MASK;
	/*
	 * remove 'NX' bit for pages that contain hv code section, as by default XD bit is set for
	 * all pages, including pages for guests.
	 */
	mmu_modify_or_del((uint64_t *)ppt_mmu_pml4_addr, hv_hpa & PDE_MASK,
			text_end - (hv_hpa & PDE_MASK), 0UL, PAGE_NX, &ppt_mem_ops, MR_MODIFY);

	mmu_modify_or_del((uint64_t *)ppt_mmu_pml4_addr, (uint64_t)get_reserve_sworld_memory_base(),
			TRUSTY_RAM_SIZE * (CONFIG_MAX_VM_NUM - 1U), PAGE_USER, 0UL, &ppt_mem_ops, MR_MODIFY);

#ifdef CONFIG_EFI_STUB
	/*Hypvervisor need access below memory region on UEFI platform.*/
	for (i = 0U; i < entries_count; i++) {
		entry = p_e820 + i;
		if (entry->type == E820_TYPE_ACPI_RECLAIM) {
			hv_access_memory_region_update(entry->baseaddr, entry->length);
		}
	}
#endif
	/* Enable paging */
	enable_paging();

	/* set ptep in sanitized_page point to itself */
	sanitize_pte((uint64_t *)sanitized_page);
}
