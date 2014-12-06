/*
	Glidix kernel

	Copyright (c) 2014, Madd Games.
	All rights reserved.
	
	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:
	
	* Redistributions of source code must retain the above copyright notice, this
	  list of conditions and the following disclaimer.
	
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	
	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
	DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
	FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
	DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
	SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
	CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
	OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
	OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <glidix/isp.h>
#include <glidix/common.h>
#include <glidix/pagetab.h>
#include <glidix/memory.h>
#include <glidix/string.h>
#include <stdint.h>

static PTe *ispPTE;

void ispInit()
{
	// TODO: xd (execute disable, not the stupid face), we'll look at that shit in a bit.

	// note that for now, kxmalloc() returns physical addresses,
	// so we can be sure that the virtual address returned is equal
	// to the physical address.
	PML4 *pml4 = getPML4();
	PDPT *pdpt = kxmalloc(sizeof(PDPT), MEM_PAGEALIGN);
	memset(pdpt, 0, sizeof(PDPT));
	PD *pd = kxmalloc(sizeof(PD), MEM_PAGEALIGN);
	memset(pd, 0, sizeof(PD));
	pdpt->entries[0].present = 1;
	pdpt->entries[0].rw = 1;
	pdpt->entries[0].pdPhysAddr = ((uint64_t)pd >> 12);

	PT *pt = kxmalloc(sizeof(PT), MEM_PAGEALIGN);
	memset(pt, 0, sizeof(PT));
	pd->entries[0].present = 1;
	pd->entries[0].rw = 1;
	pd->entries[0].ptPhysAddr = ((uint64_t)pt >> 12);

	// get the page for virtual address 0x18000000000.
	ispPTE = &pt->entries[0];
	ispPTE->present = 1;
	ispPTE->rw = 1;
	ispPTE->framePhysAddr = 0;

	// set it in the PML4 (so it maps from 0x18000000000 up).
	pml4->entries[3].present = 1;
	pml4->entries[3].rw = 1;
	pml4->entries[3].pdptPhysAddr = ((uint64_t)pdpt >> 12);

	// refresh the address space
	refreshAddrSpace();
};

void *ispGetPointer()
{
	return (void*) 0x18000000000;
};

void ispSetFrame(uint64_t frame)
{
	ispPTE->framePhysAddr = frame;
	refreshAddrSpace();
};
