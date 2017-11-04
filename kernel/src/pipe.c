/*
	Glidix kernel

	Copyright (c) 2014-2017, Madd Games.
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

#include <glidix/pipe.h>
#include <glidix/sched.h>
#include <glidix/memory.h>
#include <glidix/errno.h>
#include <glidix/string.h>
#include <glidix/syscall.h>
#include <glidix/signal.h>
#include <glidix/console.h>

static File *openPipe(Pipe *pipe, int mode);

static ssize_t pipe_read(File *fp, void *buffer, size_t size)
{
	if (size == 0) return 0;

	Pipe *pipe = (Pipe*) fp->fsdata;

	int count = semWaitGen(&pipe->counter, (int) size, SEM_W_FILE(fp->oflag), 0);
	if (count < 0)
	{
		ERRNO = -count;
		return -1;
	};
	
	ssize_t sizeCanRead = (ssize_t) count;
	if (sizeCanRead == 0)
	{
		// EOF
		return sizeCanRead;
	};
	
	semWait(&pipe->sem);

	char *put = (char*) buffer;
	ssize_t bytesLeft = (ssize_t)size;
	if (bytesLeft > sizeCanRead) bytesLeft = sizeCanRead;
	ssize_t willRead = bytesLeft;
	while (bytesLeft--)
	{
		*put++ = pipe->buffer[pipe->roff];
		pipe->roff = (pipe->roff + 1) % PIPE_BUFFER_SIZE;
	};
	
	semSignal2(&pipe->semWriteBuffer, (int) willRead);
	semSignal(&pipe->sem);
	return sizeCanRead;
};

static ssize_t pipe_write(File *fp, const void *buffer, size_t size)
{
	Pipe *pipe = (Pipe*) fp->fsdata;
	
	// determine the maximum amount of data we can currently write.
	ssize_t haveWritten = 0;
	while (haveWritten < size)
	{
		ssize_t willWrite = semWaitGen(&pipe->semWriteBuffer, (int) (size - haveWritten), SEM_W_FILE(fp->oflag), 0);
		if (willWrite < 0)
		{
			ERRNO = -willWrite;
			if (haveWritten == 0) return -1;
			else return haveWritten;
		};

		haveWritten += willWrite;
			
		semWait(&pipe->sem);

		if ((pipe->sides & SIDE_READ) == 0)
		{
			// the pipe is not readable!
			siginfo_t siginfo;
			siginfo.si_signo = SIGPIPE;
			siginfo.si_code = 0;

			cli();
			lockSched();
			sendSignal(getCurrentThread(), &siginfo);
			unlockSched();
			sti();
		
			ERRNO = EPIPE;
			semSignal(&pipe->sem);
			return -1;
		};	
	
		// write it
		const char *scan = (const char*) buffer;
		ssize_t bytesLeft = willWrite;
		while (bytesLeft--)
		{
			pipe->buffer[pipe->woff] = *scan++;
			pipe->woff = (pipe->woff + 1) % PIPE_BUFFER_SIZE;
		};
	
		// wake up waiting threads
		semSignal2(&pipe->counter, (int) willWrite);
	
		semSignal(&pipe->sem);
	};
	
	return haveWritten;
};

static void pipe_close(File *fp)
{
	Pipe *pipe = (Pipe*) fp->fsdata;
	semWait(&pipe->sem);

	if (fp->oflag & O_WRONLY)
	{
		pipe->sides &= ~SIDE_WRITE;
		semTerminate(&pipe->counter);
		semSignal(&pipe->semHangup);
	}
	else
	{
		pipe->sides &= ~SIDE_READ;
		semTerminate(&pipe->semWriteBuffer);
	};
	
	if (pipe->sides == 0)
	{
		kfree(pipe);
	}
	else
	{
		semSignal(&pipe->sem);
	};
};

static int pipe_fstat(File *fp, struct kstat *st)
{
	Pipe *pipe = (Pipe*) fp->fsdata;
	semWait(&pipe->sem);

	st->st_dev = 0;
	st->st_ino = (ino_t) fp->fsdata;
	st->st_mode = 0600 | VFS_MODE_FIFO;
	st->st_nlink = ((pipe->sides >> 1) & 1) + (pipe->sides & 1);
	st->st_uid = 0;
	st->st_gid = 0;
	st->st_rdev = 0;
	st->st_size = PIPE_BUFFER_SIZE;
	st->st_blksize = 1;
	st->st_blocks = PIPE_BUFFER_SIZE;
	st->st_atime = 0;
	st->st_mtime = 0;
	st->st_ctime = 0;

	semSignal(&pipe->sem);
	return 0;
};

static void pipe_pollinfo(File *fp, Semaphore **sems)
{
	Pipe *pipe = (Pipe*) fp->fsdata;
	sems[PEI_READ] = &pipe->counter;
	sems[PEI_WRITE] = vfsGetConstSem();
	sems[PEI_HANGUP] = &pipe->semHangup;
};

static File *openPipe(Pipe *pipe, int mode)
{
	semWait(&pipe->sem);
	File *fp = (File*) kmalloc(sizeof(File));
	memset(fp, 0, sizeof(File));

	fp->oflag = mode;
	fp->fsdata = pipe;
	fp->oflag = mode;
	if (mode == O_RDONLY)
	{
		fp->read = pipe_read;
	}
	else
	{
		fp->write = pipe_write;
	};
	fp->close = pipe_close;
	fp->fstat = pipe_fstat;
	fp->pollinfo = pipe_pollinfo;
	fp->refcount = 1;

	semSignal(&pipe->sem);
	return fp;
};

int sys_pipe(int *upipefd)
{
	return sys_pipe2(upipefd, 0);
};

int sys_pipe2(int *upipefd, int flags)
{
	int allFlags = O_CLOEXEC;
	if ((flags & ~allFlags) != 0)
	{
		ERRNO = EINVAL;
		return -1;
	};
	
	int pipefd[2];

	int rfd = ftabAlloc(getCurrentThread()->ftab);
	if (rfd == -1)
	{
		ERRNO = EMFILE;
		return -1;
	};
	
	int wfd = ftabAlloc(getCurrentThread()->ftab);
	if (wfd == -1)
	{
		ftabSet(getCurrentThread()->ftab, rfd, NULL, 0);
		ERRNO = EMFILE;
		return -1;
	};
	
	Pipe *pipe = (Pipe*) kmalloc(sizeof(Pipe));
	semInit(&pipe->sem);
	semInit2(&pipe->counter, 0);
	semInit2(&pipe->semWriteBuffer, PIPE_BUFFER_SIZE);
	semInit2(&pipe->semHangup, 0);
	pipe->roff = 0;
	pipe->woff = 0;
	pipe->sides = SIDE_READ | SIDE_WRITE;

	// O_CLOEXEC == FD_CLOEXEC
	ftabSet(getCurrentThread()->ftab, rfd, openPipe(pipe, O_RDONLY), flags);
	ftabSet(getCurrentThread()->ftab, wfd, openPipe(pipe, O_WRONLY), flags);
	
	pipefd[0] = rfd;
	pipefd[1] = wfd;

	if (memcpy_k2u(upipefd, pipefd, sizeof(int)*2) != 0)
	{
		ERRNO = EFAULT;
		return -1;
	};
	
	return 0;
};
