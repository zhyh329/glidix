/*
	Glidix GUI

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

#include <sys/glidix.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <libgwm.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <fstools.h>

static int queueFD;
static volatile int initFinished = 0;
static uint64_t nextWindowID;
static uint64_t nextSeq;
static pthread_t listenThread;
static DDIFont *defaultFont;
static DDIPixelFormat screenFormat;

typedef struct GWMWaiter_
{
	struct GWMWaiter_*		prev;
	struct GWMWaiter_*		next;
	uint64_t			seq;
	GWMMessage			resp;
	sem_t				lock;
} GWMWaiter;

typedef struct EventBuffer_
{
	struct EventBuffer_*		prev;
	struct EventBuffer_*		next;
	GWMEvent			payload;
} EventBuffer;

typedef struct FileIconCache_
{
	struct FileIconCache_*		next;
	char*				name;
	DDISurface*			small;
	DDISurface*			large;
} FileIconCache;

static pthread_mutex_t waiterLock;
static GWMWaiter* waiters = NULL;
static sem_t semEventCounter;
static pthread_mutex_t eventLock;
static EventBuffer *firstEvent;
static EventBuffer *lastEvent;
static GWMHandlerInfo *firstHandler = NULL;
static GWMInfo *gwminfo = NULL;
static FileIconCache *fileIconCache = NULL;

DDIColor gwmColorSelection = {0, 0xAA, 0, 0xFF};
DDIColor gwmBackColor = {0xDD, 0xDD, 0xDD, 0xFF};		// window background color

static void gwmPostWaiter(uint64_t seq, GWMMessage *resp, const GWMCommand *cmd)
{
	GWMWaiter *waiter = (GWMWaiter*) malloc(sizeof(GWMWaiter));
	waiter->prev = NULL;
	waiter->next = NULL;
	waiter->seq = seq;
	sem_init(&waiter->lock, 0, 0);
	
	pthread_mutex_lock(&waiterLock);
	if (write(queueFD, cmd, sizeof(GWMCommand)) != sizeof(GWMCommand))
	{
		pthread_mutex_unlock(&waiterLock);
		perror("write(queueFD)");
	};
	
	waiter->next = waiters;
	waiter->prev = NULL;
	waiters = waiter;
	pthread_mutex_unlock(&waiterLock);
	
	// wait for response
	sem_wait(&waiter->lock);
	memcpy(resp, &waiter->resp, sizeof(GWMMessage));
	free(waiter);
};

static void* listenThreadFunc(void *ignore)
{
	(void)ignore;
	char msgbuf[65536];
	
	// block all signals. we don't want signal handlers to be invoked in the GWM
	// listening thread, because if the application is buggy it might create undebuggable
	// chaos.
	sigset_t sigset;
	sigfillset(&sigset);
	pthread_sigmask(SIG_BLOCK, &sigset, NULL);
	
	initFinished = 1;
	while (1)
	{
		ssize_t size = read(queueFD, msgbuf, 65536);
		if (size == -1)
		{
			if (errno == EINTR) continue;
			else break;
		};

		if (size > 0)
		{
			if (size < sizeof(GWMMessage))
			{
				continue;
			};
		
			GWMMessage *msg = (GWMMessage*) msgbuf;
			if (msg->generic.seq != 0)
			{
				pthread_mutex_lock(&waiterLock);
				GWMWaiter *waiter;
				for (waiter=waiters; waiter!=NULL; waiter=waiter->next)
				{
					if (waiter->seq == msg->generic.seq)
					{
						memcpy(&waiter->resp, msg, sizeof(GWMMessage));
						if (waiter->prev != NULL) waiter->prev->next = waiter->next;
						if (waiter->next != NULL) waiter->next->prev = waiter->prev;
						if (waiter->prev == NULL) waiters = waiter->next;
						sem_post(&waiter->lock);
						break;
					};
				};
				pthread_mutex_unlock(&waiterLock);
			}
			else if (msg->generic.type == GWM_MSG_EVENT)
			{
				EventBuffer *buf = (EventBuffer*) malloc(sizeof(EventBuffer));
				memcpy(&buf->payload, &msg->event.payload, sizeof(GWMEvent));
				
				pthread_mutex_lock(&eventLock);
				if (firstEvent == NULL)
				{
					buf->prev = buf->next = NULL;
					firstEvent = lastEvent = buf;
				}
				else
				{
					buf->prev = lastEvent;
					buf->next = NULL;
					lastEvent->next = buf;
					lastEvent = buf;
				};
				pthread_mutex_unlock(&eventLock);
				sem_post(&semEventCounter);
			};
		}
		else
		{
			// size is 0; hangup
			fprintf(stderr, "libgwm: connection terminated by server\n");
			abort();
		};
	};
	
	return NULL;
};

int gwmInit()
{
	fsInit();
	
	int fileFlags = O_RDONLY;
	if (geteuid() == 0) fileFlags = O_RDWR;
	int fd = open("/run/gwminfo", fileFlags);
	if (fd == -1)
	{
		return -1;
	};
	
	int mapFlags = PROT_READ;
	if (geteuid() == 0) mapFlags = PROT_READ | PROT_WRITE;
	gwminfo = (GWMInfo*) mmap(NULL, sizeof(GWMInfo), mapFlags, MAP_SHARED, fd, 0);
	
	nextWindowID = 1;
	nextSeq = 1;
	sem_init(&semEventCounter, 0, 0);
	
	pthread_mutex_init(&waiterLock, NULL);
	pthread_mutex_init(&eventLock, NULL);
	
	char dispdev[1024];
	char linebuf[1024];
	dispdev[0] = 0;
	
	FILE *fp = fopen("/etc/gwm.conf", "r");
	if (fp == NULL)
	{
		fprintf(stderr, "gwm: failed to open configuration file /etc/gwm.conf: %s\n", strerror(errno));
		return -1;
	};
	
	char *saveptr;
	char *line;
	int lineno = 0;
	while ((line = fgets(linebuf, 1024, fp)) != NULL)
	{
		lineno++;
		
		char *endline = strchr(line, '\n');
		if (endline != NULL)
		{
			*endline = 0;
		};
		
		if (strlen(line) >= 1023)
		{
			fprintf(stderr, "/etc/gwm.conf:%d: buffer overflow\n", lineno);
			fclose(fp);
			return -1;
		};
		
		if ((line[0] == 0) || (line[0] == '#'))
		{
			continue;
		}
		else
		{
			char *cmd = strtok_r(line, " \t", &saveptr);
			if (cmd == NULL)
			{
				continue;
			};
			
			if (strcmp(cmd, "display") == 0)
			{
				char *name = strtok_r(NULL, " \t", &saveptr);
				if (name == NULL)
				{
					fprintf(stderr, "/etc/gwm.conf:%d: 'display' needs a parameter\n", lineno);
					fclose(fp);
					return -1;
				};
				
				strcpy(dispdev, name);
			};
		};
	};
	fclose(fp);
	
	if (dispdev[0] == 0)
	{
		fprintf(stderr, "gwm: display device not found\n");
		return -1;
	};
	
	if (ddiInit(dispdev, O_RDONLY) != 0)
	{
		fprintf(stderr, "gwm: failed to initialize DDI: %s\n", strerror(errno));
		return -1;
	};

	const char *errmsg;
	defaultFont = ddiLoadFont("DejaVu Sans", 12, DDI_STYLE_REGULAR, &errmsg);
	if (defaultFont == NULL)
	{
		fprintf(stderr, "gwm: failed to load default font (DejaVu Sans 12): %s\n", errmsg);
		ddiQuit();
		return -1;
	};

	queueFD = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (queueFD == -1)
	{
		fprintf(stderr, "gwm: could not create socket: %s\n", strerror(errno));
		ddiQuit();
		return -1;
	};
	
	struct sockaddr_un srvaddr;
	srvaddr.sun_family = AF_UNIX;
	strcpy(srvaddr.sun_path, "/run/gwmserver");
	
	if (connect(queueFD, (struct sockaddr*) &srvaddr, sizeof(struct sockaddr_un)) != 0)
	{
		fprintf(stderr, "gwm: cannot connect to /run/gwmserver: %s\n", strerror(errno));
		ddiQuit();
		close(queueFD);
		return -1;
	};
	
	if (pthread_create(&listenThread, NULL, listenThreadFunc, NULL) != 0)
	{
		close(queueFD);
		ddiQuit();
		return -1;
	};
	
	gwmGetScreenFormat(&screenFormat);
	return 0;
};

void gwmQuit()
{
	close(queueFD);
	ddiQuit();
	fsQuit();
};

static int gwmDefaultHandler(GWMEvent *ev, GWMWindow *win, void *context)
{
	switch (ev->type)
	{
	case GWM_EVENT_CLOSE:
		return GWM_EVSTATUS_BREAK;
	default:
		if (ev->type & GWM_EVENT_CASCADING)
		{
			if (win->parent != NULL) return gwmPostEvent(ev, win->parent);
		};
		return GWM_EVSTATUS_OK;
	};
};

GWMWindow* gwmCreateWindow(
	GWMWindow* parent,
	const char *caption,
	int x, int y,
	int width, int height,
	int flags)
{
	uint64_t id = __sync_fetch_and_add(&nextWindowID, 1);
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	DDISurface *canvas = ddiCreateSurface(&screenFormat, width, height, NULL, DDI_SHARED);
	if (canvas == NULL)
	{
		return NULL;
	};

	ddiFillRect(canvas, 0, 0, width, height, &gwmBackColor);
	
	GWMCommand cmd;
	cmd.createWindow.cmd = GWM_CMD_CREATE_WINDOW;
	cmd.createWindow.id = id;
	cmd.createWindow.parent = 0;
	if (parent != NULL) cmd.createWindow.parent = parent->id;
	strcpy(cmd.createWindow.pars.caption, caption);
	strcpy(cmd.createWindow.pars.iconName, caption);
	cmd.createWindow.pars.flags = flags;
	cmd.createWindow.pars.width = width;
	cmd.createWindow.pars.height = height;
	cmd.createWindow.pars.x = x;
	cmd.createWindow.pars.y = y;
	cmd.createWindow.seq = seq;
	cmd.createWindow.surfID = canvas->id;
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	if (resp.createWindowResp.status == 0)
	{
		GWMWindow *win = (GWMWindow*) malloc(sizeof(GWMWindow));
		win->id = id;
		win->canvas = canvas;
		win->lastClickTime = 0;
		
		win->modalID = 0;
		if (parent != NULL) win->modalID = parent->modalID;
		win->icon = NULL;
		
		win->parent = parent;
		win->layout = NULL;
		win->position = NULL;
		win->getMinSize = NULL;
		win->getPrefSize = NULL;
		win->flags = flags;
		
		gwmPushEventHandler(win, gwmDefaultHandler, NULL);
		return win;
	};
	
	// failed to create it
	ddiDeleteSurface(canvas);
	return NULL;
};

DDISurface* gwmGetWindowCanvas(GWMWindow *win)
{
	return win->canvas;
};

static void gwmHandlerDecref(GWMHandlerInfo *info)
{
	if (--info->refcount == 0) free(info);
};

void gwmDestroyWindow(GWMWindow *win)
{
	ddiDeleteSurface(win->canvas);
	
	GWMCommand cmd;
	cmd.destroyWindow.cmd = GWM_CMD_DESTROY_WINDOW;
	cmd.destroyWindow.id = win->id;
	
	if (write(queueFD, &cmd, sizeof(GWMCommand)) != sizeof(GWMCommand))
	{
		perror("write(queueFD)");
		return;
	};

	GWMHandlerInfo *info = firstHandler;
	while (info != NULL)
	{
		GWMHandlerInfo *next = info->next;
		if (info->win == win)
		{
			if (info->prev != NULL) info->prev->next = info->next;
			if (info->next != NULL) info->next->prev = info->prev;
			if (firstHandler == info) firstHandler = info->next;
			info->win = NULL;
			gwmHandlerDecref(info);
		};
		
		info = next;
	};
	
	free(win);
};

void gwmPostDirty(GWMWindow *win)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.postDirty.cmd = GWM_CMD_POST_DIRTY;
	cmd.postDirty.id = win->id;
	cmd.postDirty.seq = seq;
	write(queueFD, &cmd, sizeof(GWMCommand));
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
};

void gwmWaitEvent(GWMEvent *ev)
{
	while (sem_wait(&semEventCounter) != 0);

	pthread_mutex_lock(&eventLock);
	memcpy(ev, &firstEvent->payload, sizeof(GWMEvent));
	firstEvent = firstEvent->next;
	if (firstEvent != NULL) firstEvent->prev = NULL;
	pthread_mutex_unlock(&eventLock);
};

void gwmClearWindow(GWMWindow *win)
{
	ddiFillRect(win->canvas, 0, 0, win->canvas->width, win->canvas->height, &gwmBackColor);
};

void gwmPostUpdate(GWMWindow *win)
{
	EventBuffer *buf = (EventBuffer*) malloc(sizeof(EventBuffer));
	buf->payload.type = GWM_EVENT_UPDATE;
	buf->payload.win = 0;
	if (win != NULL)
	{
		buf->payload.win = win->id;
	};
	
	pthread_mutex_lock(&eventLock);
	if (firstEvent == NULL)
	{
		buf->prev = buf->next = NULL;
		firstEvent = lastEvent = buf;
	}
	else
	{
		buf->prev = lastEvent;
		buf->next = NULL;
		lastEvent->next = buf;
		lastEvent = buf;
	};
	pthread_mutex_unlock(&eventLock);
	sem_post(&semEventCounter);
};

void gwmPushEventHandler(GWMWindow *win, GWMEventHandler handler, void *context)
{
	GWMHandlerInfo *info = (GWMHandlerInfo*) malloc(sizeof(GWMHandlerInfo));
	info->win = win;
	info->callback = handler;
	info->prev = NULL;
	info->next = firstHandler;
	info->context = context;
	info->refcount = 1;
	if (firstHandler != NULL) firstHandler->prev = info;
	firstHandler = info;
};

static int gwmPostEventByWindowID(GWMEvent *ev, uint64_t modalID)
{
	GWMHandlerInfo *info;
	for (info=firstHandler; info!=NULL; info=info->next)
	{
		if ((ev->win == info->win->id) && (info->win->modalID == modalID))
		{
			info->refcount++;
			int status = info->callback(ev, info->win, info->context);
			if (status == GWM_EVSTATUS_BREAK)
			{
				gwmHandlerDecref(info);
				return GWM_EVSTATUS_BREAK;
			}
			else if (info->win != NULL) // info->win may be set to NULL by callback() if the window was destroyed.
			{
				if ((ev->type == GWM_EVENT_UP) && (ev->keycode == GWM_KC_MOUSE_LEFT))
				{
					clock_t now = clock();
					if (info->win->lastClickTime != 0)
					{
						if ((now-info->win->lastClickTime) <= GWM_DOUBLECLICK_TIMEOUT)
						{
							if ((info->win->lastClickX == ev->x)
								&& (info->win->lastClickY == ev->y))
							{
								// add a double-click event to queue
								EventBuffer *buf = (EventBuffer*) malloc(sizeof(EventBuffer));
								memcpy(&buf->payload, ev, sizeof(GWMEvent));
								buf->payload.type = GWM_EVENT_DOUBLECLICK;
								pthread_mutex_lock(&eventLock);
								if (firstEvent == NULL)
								{
									buf->prev = buf->next = NULL;
									firstEvent = lastEvent = buf;
								}
								else
								{
									buf->prev = lastEvent;
									buf->next = NULL;
									lastEvent->next = buf;
									lastEvent = buf;
								};
								pthread_mutex_unlock(&eventLock);
								sem_post(&semEventCounter);
							};
						};
					};
			
					info->win->lastClickX = ev->x;
					info->win->lastClickY = ev->y;
					info->win->lastClickTime = now;
				};
			};
			
			GWMWindow *winWas = info->win;
			gwmHandlerDecref(info);
			if (status == GWM_EVSTATUS_OK) break;
			if (winWas == NULL) break;
		};
	};
	
	return GWM_EVSTATUS_OK;
};

int gwmPostEvent(GWMEvent *ev, GWMWindow *win)
{
	ev->win = win->id;
	return gwmPostEventByWindowID(ev, win->modalID);
};

static void gwmModalLoop(uint64_t modalID)
{
	GWMEvent ev;
	
	while (1)
	{
		gwmWaitEvent(&ev);
		if (gwmPostEventByWindowID(&ev, modalID) == GWM_EVSTATUS_BREAK) break;
	};
};

void gwmMainLoop()
{
	gwmModalLoop(0);
};

void gwmScreenSize(int *width, int *height)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.screenSize.cmd = GWM_CMD_SCREEN_SIZE;
	cmd.screenSize.seq = seq;
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	
	if (width != NULL) *width = resp.screenSizeResp.width;
	if (height != NULL) *height = resp.screenSizeResp.height;
};

int gwmSetWindowFlags(GWMWindow *win, int flags)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.setFlags.cmd = GWM_CMD_SET_FLAGS;
	cmd.setFlags.seq = seq;
	cmd.setFlags.win = win->id;
	cmd.setFlags.flags = flags;
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	
	if (resp.setFlagsResp.status == 0)
	{
		win->flags = flags;
	};
	
	return resp.setFlagsResp.status;
};

int gwmSetWindowCursor(GWMWindow *win, int cursor)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.setCursor.cmd = GWM_CMD_SET_CURSOR;
	cmd.setCursor.seq = seq;
	cmd.setCursor.win = win->id;
	cmd.setCursor.cursor = cursor;
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	
	return resp.setCursorResp.status;
};

int gwmSetWindowIcon(GWMWindow *win, DDISurface *icon)
{	
	if (win->icon != NULL)
	{
		ddiDeleteSurface(win->icon);
	};
	
	win->icon = ddiCreateSurface(&win->canvas->format, 16, 16, NULL, DDI_SHARED);
	if (win->icon == NULL)
	{
		return GWM_ERR_NOSURF;
	};
	
	ddiOverlay(icon, 0, 0, win->icon, 0, 0, 16, 16);
	
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.setIcon.cmd = GWM_CMD_SET_ICON;
	cmd.setIcon.surfID = win->icon->id;
	cmd.setIcon.seq = seq;
	cmd.setIcon.win = win->id;
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	
	return resp.setIconResp.status;
};

DDIFont *gwmGetDefaultFont()
{
	return defaultFont;
};

void gwmGetScreenFormat(DDIPixelFormat *format)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.getFormat.cmd = GWM_CMD_GET_FORMAT;
	cmd.getFormat.seq = seq;
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	
	memcpy(format, &resp.getFormatResp.format, sizeof(DDIPixelFormat));
};

int gwmGetDesktopWindows(GWMGlobWinRef *focused, GWMGlobWinRef *winlist)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.getWindowList.cmd = GWM_CMD_GET_WINDOW_LIST;
	cmd.getWindowList.seq = seq;
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	
	if (winlist != NULL)
	{
		memcpy(winlist, resp.getWindowListResp.wins, sizeof(GWMGlobWinRef)*resp.getWindowListResp.count);
	};
	
	if (focused != NULL)
	{
		memcpy(focused, &resp.getWindowListResp.focused, sizeof(GWMGlobWinRef));
	};
	
	return resp.getWindowListResp.count;
};

int gwmGetGlobWinParams(GWMGlobWinRef *ref, GWMWindowParams *pars)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.getWindowParams.cmd = GWM_CMD_GET_WINDOW_PARAMS;
	cmd.getWindowParams.seq = seq;
	memcpy(&cmd.getWindowParams.ref, ref, sizeof(GWMGlobWinRef));
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	
	if (resp.getWindowParamsResp.status == 0)
	{
		memcpy(pars, &resp.getWindowParamsResp.params, sizeof(GWMWindowParams));
	};
	
	return resp.getWindowParamsResp.status;
};

void gwmToggleWindow(GWMGlobWinRef *ref)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.toggleWindow.cmd = GWM_CMD_TOGGLE_WINDOW;
	cmd.toggleWindow.seq = seq;
	memcpy(&cmd.toggleWindow.ref, ref, sizeof(GWMGlobWinRef));
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
};

void gwmSetListenWindow(GWMWindow *win)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.setListenWindow.cmd = GWM_CMD_SET_LISTEN_WINDOW;
	cmd.setListenWindow.seq = seq;
	cmd.setListenWindow.win = win->id;
	
	write(queueFD, &cmd, sizeof(GWMCommand));
};

void gwmResizeWindow(GWMWindow *win, int width, int height)
{
	DDIPixelFormat format;
	memcpy(&format, &win->canvas->format, sizeof(DDIPixelFormat));
	
	// resize the canvas
	DDISurface *newCanvas = ddiCreateSurface(&format, width, height, NULL, DDI_SHARED);
	ddiFillRect(newCanvas, 0, 0, width, height, &gwmBackColor);
	ddiBlit(win->canvas, 0, 0, newCanvas, 0, 0, win->canvas->width, win->canvas->height);
	ddiDeleteSurface(win->canvas);
	win->canvas = newCanvas;
	
	// send the configuration command
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	GWMCommand cmd;
	cmd.atomicConfig.cmd = GWM_CMD_ATOMIC_CONFIG;
	cmd.atomicConfig.seq = seq;
	cmd.atomicConfig.win = win->id;
	cmd.atomicConfig.which = GWM_AC_WIDTH | GWM_AC_HEIGHT | GWM_AC_CANVAS;
	cmd.atomicConfig.width = width;
	cmd.atomicConfig.height = height;
	cmd.atomicConfig.canvasID = newCanvas->id;
	
	write(queueFD, &cmd, sizeof(GWMCommand));
};

void gwmMoveWindow(GWMWindow *win, int x, int y)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);

	GWMCommand cmd;
	cmd.atomicConfig.cmd = GWM_CMD_ATOMIC_CONFIG;
	cmd.atomicConfig.seq = seq;
	cmd.atomicConfig.win = win->id;
	cmd.atomicConfig.which = GWM_AC_X | GWM_AC_Y;
	cmd.atomicConfig.x = x;
	cmd.atomicConfig.y = y;
	
	write(queueFD, &cmd, sizeof(GWMCommand));
};

void gwmRelToAbs(GWMWindow *win, int relX, int relY, int *absX, int *absY)
{
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.relToAbs.cmd = GWM_CMD_REL_TO_ABS;
	cmd.relToAbs.seq = seq;
	cmd.relToAbs.win = win->id;
	cmd.relToAbs.relX = relX;
	cmd.relToAbs.relY = relY;
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	
	if (absX != NULL) *absX = resp.relToAbsResp.absX;
	if (absY != NULL) *absY = resp.relToAbsResp.absY;
};

int gwmClassifyChar(char c)
{
	if ((c == ' ') || (c == '\t') || (c == '\n'))
	{
		return 0;
	};
	
	return 1;
};

GWMWindow* gwmCreateModal(const char *caption, int x, int y, int width, int height)
{
	static uint64_t nextModalID = 1;
	GWMWindow *win = gwmCreateWindow(NULL, caption, x, y, width, height, GWM_WINDOW_NOTASKBAR | GWM_WINDOW_HIDDEN);
	win->modalID = nextModalID++;
	return win;
};

void gwmRunModal(GWMWindow *modal, int flags)
{
	gwmSetWindowFlags(modal, flags);
	gwmModalLoop(modal->modalID);
};

GWMInfo *gwmGetInfo()
{
	return gwminfo;
};

void gwmRedrawScreen()
{
	GWMCommand cmd;
	cmd.cmd = GWM_CMD_REDRAW_SCREEN;
	
	write(queueFD, &cmd, sizeof(GWMCommand));
};

void gwmGetGlobRef(GWMWindow *win, GWMGlobWinRef *ref)
{
	fprintf(stderr, "gwmGetGlobRef: implement me!\n");
	abort();
};

GWMWindow *gwmScreenshotWindow(GWMGlobWinRef *ref)
{
#if 0
	uint64_t id = __sync_fetch_and_add(&nextWindowID, 1);
	uint64_t seq = __sync_fetch_and_add(&nextSeq, 1);
	
	GWMCommand cmd;
	cmd.screenshotWindow.cmd = GWM_CMD_SCREENSHOT_WINDOW;
	cmd.screenshotWindow.id = id;
	cmd.screenshotWindow.seq = seq;
	memcpy(&cmd.screenshotWindow.ref, ref, sizeof(GWMGlobWinRef));
	
	GWMMessage resp;
	gwmPostWaiter(seq, &resp, &cmd);
	if (resp.screenshotWindowResp.status == 0)
	{
		GWMWindow *win = (GWMWindow*) malloc(sizeof(GWMWindow));
		win->id = id;

		win->canvases[0] = ddiOpenSurface(resp.screenshotWindowResp.clientID[0]);
		win->canvases[1] = ddiOpenSurface(resp.screenshotWindowResp.clientID[1]);
		
		win->handlerInfo = NULL;
		win->currentBuffer = 0;
		win->lastClickTime = 0;
		
		win->modalID = 0;
		return win;
	};
#endif

	// TODO: implement this with the new protocol
	return NULL;
};

DDISurface* gwmGetFileIcon(const char *iconName, int size)
{
	FileIconCache *scan;
	for (scan=fileIconCache; scan!=NULL; scan=scan->next)
	{
		if (strcmp(scan->name, iconName) == 0)
		{
			if (size == GWM_FICON_SMALL)
			{
				return scan->small;
			}
			else
			{
				return scan->large;
			};
		};
	};
	
	// cache miss; load the icon or make a dummy one
	FileIconCache *cache = (FileIconCache*) malloc(sizeof(FileIconCache));
	cache->name = strdup(iconName);
	
	char pathname[PATH_MAX];
	sprintf(pathname, "/usr/share/mime/icons/%s.png", iconName);
	
	DDIPixelFormat format;
	gwmGetScreenFormat(&format);
	
	cache->large = ddiLoadAndConvertPNG(&format, pathname, NULL);
	if (cache->large == NULL)
	{
		cache->large = ddiCreateSurface(&format, 64, 64, NULL, 0);
		if (cache->large == NULL)
		{
			fprintf(stderr, "libgwm: ddiCreateSurface failed unexpectedly!");
			abort();
		};
		
		static DDIColor purple = {0x77, 0x00, 0x77, 0xFF};
		static DDIColor black = {0x00, 0x00, 0x00, 0xFF};
		
		ddiFillRect(cache->large, 0, 0, 64, 64, &purple);
		ddiFillRect(cache->large, 32, 0, 32, 32, &black);
		ddiFillRect(cache->large, 0, 32, 32, 32, &black);
	};
	
	cache->small = ddiScale(cache->large, 16, 16, DDI_SCALE_BEST);
	if (cache->small == NULL)
	{
		fprintf(stderr, "libgwm: unexpected scaling fialure!");
		abort();
	};

	cache->next = fileIconCache;
	fileIconCache = cache;
	
	if (size == GWM_FICON_SMALL)
	{
		return cache->small;
	}
	else
	{
		return cache->large;
	};
};

void gwmRetheme()
{
	GWMCommand cmd;
	cmd.cmd = GWM_CMD_RETHEME;
	
	write(queueFD, &cmd, sizeof(GWMCommand));
};
