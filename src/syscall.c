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

#include <glidix/syscall.h>
#include <glidix/sched.h>
#include <glidix/ftab.h>
#include <glidix/vfs.h>
#include <glidix/elf64.h>

static uint64_t sys_write(int fd, const void *buf, size_t size)
{
	ssize_t out;
	if (fd >= MAX_OPEN_FILES)
	{
		out = VFS_BAD_FD;
	}
	else
	{
		FileTable *ftab = getCurrentThread()->ftab;
		spinlockAcquire(&ftab->spinlock);
		File *fp = ftab->entries[fd];
		if (fp == NULL)
		{
			spinlockRelease(&ftab->spinlock);
			out = VFS_BAD_FD;
		}
		else
		{
			if (fp->write == NULL)
			{
				spinlockRelease(&ftab->spinlock);
				out = VFS_PERM;
			}
			else
			{
				out = fp->write(fp, buf, size);
				spinlockRelease(&ftab->spinlock);
			};
		};
	};

	return *((uint64_t*)&out);
};

void syscallDispatch(Regs *regs, uint16_t num)
{
	regs->rip += 4;

	switch (num)
	{
	case 1:
		regs->rax = sys_write(*((int*)&regs->rdi), (const void*) regs->rsi, *((size_t*)&regs->rdx));
		break;
	case 2:
		regs->rax = elfExec(regs, (const char*) regs->rdi);
		break;
	default:
		panic("invalid syscall: %d\n", num);
		break;
	};
};