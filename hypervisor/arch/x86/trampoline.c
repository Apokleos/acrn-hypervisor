/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <hypervisor.h>
#include <reloc.h>
#include <trampoline.h>
#include <vm0_boot.h>
#include <e820.h>

uint64_t trampoline_start16_paddr;

/*
 * Because trampoline code is relocated in different way, if HV code
 * accesses trampoline using relative addressing, it needs to take
 * out the HV relocation delta
 *
 * This function is valid if:
 *  - The hpa of HV code is always higher than trampoline code
 *  - The HV code is always relocated to higher address, compared
 *    with CONFIG_HV_RAM_START
 */
static uint64_t trampoline_relo_addr(const void *addr)
{
	return (uint64_t)addr - get_hv_image_delta();
}

uint64_t read_trampoline_sym(const void *sym)
{
	uint64_t *hva = (uint64_t *)(hpa2hva(trampoline_start16_paddr) + trampoline_relo_addr(sym));
	return *hva;
}

void write_trampoline_sym(const void *sym, uint64_t val)
{
	uint64_t *hva = (uint64_t *)(hpa2hva(trampoline_start16_paddr) + trampoline_relo_addr(sym));
	*hva = val;
	clflush(hva);
}

static void update_trampoline_code_refs(uint64_t dest_pa)
{
	void *ptr;
	uint64_t val;
	int32_t i;

	/*
	 * calculate the fixup CS:IP according to fixup target address
	 * dynamically.
	 *
	 * trampoline code starts in real mode,
	 * so the target addres is HPA
	 */
	val = dest_pa + trampoline_relo_addr(&trampoline_fixup_target);

	ptr = hpa2hva(dest_pa + trampoline_relo_addr(&trampoline_fixup_cs));
	*(uint16_t *)(ptr) = (uint16_t)((val >> 4U) & 0xFFFFU);

	ptr = hpa2hva(dest_pa + trampoline_relo_addr(&trampoline_fixup_ip));
	*(uint16_t *)(ptr) = (uint16_t)(val & 0xfU);

	/* Update temporary page tables */
	ptr = hpa2hva(dest_pa + trampoline_relo_addr(&cpu_boot_page_tables_ptr));
	*(uint32_t *)(ptr) += (uint32_t)dest_pa;

	ptr = hpa2hva(dest_pa + trampoline_relo_addr(&cpu_boot_page_tables_start));
	*(uint64_t *)(ptr) += dest_pa;

	ptr = hpa2hva(dest_pa + trampoline_relo_addr(&trampoline_pdpt_addr));
	for (i = 0; i < 4; i++) {
		*(uint64_t *)(ptr + sizeof(uint64_t) * i) += dest_pa;
	}

	/* update the gdt base pointer with relocated offset */
	ptr = hpa2hva(dest_pa + trampoline_relo_addr(&trampoline_gdt_ptr));
	*(uint64_t *)(ptr + 2) += dest_pa;

	/* update trampoline jump pointer with relocated offset */
	ptr = hpa2hva(dest_pa + trampoline_relo_addr(&trampoline_start64_fixup));
	*(uint32_t *)ptr += (uint32_t)dest_pa;

	/* update trampoline's main entry pointer */
	ptr = hpa2hva(dest_pa + trampoline_relo_addr(main_entry));
	*(uint64_t *)ptr += get_hv_image_delta();

	/* update trampoline's spinlock pointer */
	ptr = hpa2hva(dest_pa + trampoline_relo_addr(&trampoline_spinlock_ptr));
	*(uint64_t *)ptr += get_hv_image_delta();
}

uint64_t prepare_trampoline(void)
{
	uint64_t size, dest_pa, i;

	size = (uint64_t)(&ld_trampoline_end - &ld_trampoline_start);
#ifndef CONFIG_EFI_STUB
	dest_pa = e820_alloc_low_memory(CONFIG_LOW_RAM_SIZE);
#else
	dest_pa = (uint64_t)get_ap_trampoline_buf();
#endif

	pr_dbg("trampoline code: %llx size %x", dest_pa, size);

	/* Copy segment for AP initialization code below 1MB */
	stac();
	(void)memcpy_s(hpa2hva(dest_pa), (size_t)size, &ld_trampoline_load,
			(size_t)size);
	update_trampoline_code_refs(dest_pa);

	for (i = 0UL; i < size; i = i + CACHE_LINE_SIZE) {
		clflush(hpa2hva(dest_pa + i));
	}
	clac();

	trampoline_start16_paddr = dest_pa;

	return dest_pa;
}
