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

#include <libgwm.h>
#include <libddi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define	BUTTON_HEIGHT		30

enum
{
	BUTTON_STATE_NORMAL,
	BUTTON_STATE_HOVERING,
	BUTTON_STATE_CLICKED
};

static DDISurface *imgButton = NULL;

typedef struct
{
	char			*text;
	int			flags;
	int			state;
	GWMButtonCallback	callback;
	void			*callbackParam;
	int			symbol;
	int			prefWidth;
} GWMButtonData;

static void gwmRedrawButton(GWMWindow *button)
{
	GWMButtonData *data = (GWMButtonData*) button->data;
	DDISurface *canvas = gwmGetWindowCanvas(button);
	if (canvas->width == 0 && data->prefWidth != 0) return;

	static DDIColor transparent = {0, 0, 0, 0};
	ddiFillRect(canvas, 0, 0, canvas->width, canvas->height, &transparent);
	
	int whichImg = data->state;
	if (data->flags & GWM_BUTTON_DISABLED)
	{
		whichImg = 3;
	};
	
	if (imgButton == NULL)
	{
		const char *error;
		imgButton = ddiLoadAndConvertPNG(&canvas->format, "/usr/share/images/button.png", &error);
		if (imgButton == NULL)
		{
			fprintf(stderr, "Failed to load button image (/usr/share/images/button.png): %s\n", error);
			abort();
		};
	};
	
	ddiBlit(imgButton, 0, BUTTON_HEIGHT*whichImg, canvas, 0, 0, 8, BUTTON_HEIGHT);
	
	int x;
	for (x=8; x<canvas->width-8; x++)
	{
		ddiBlit(imgButton, 8, BUTTON_HEIGHT*whichImg, canvas, x, 0, 1, BUTTON_HEIGHT);
	};
	
	ddiBlit(imgButton, 9, BUTTON_HEIGHT*whichImg, canvas, canvas->width-8, 0, 8, BUTTON_HEIGHT);
	
	DDIPen *pen = ddiCreatePen(&canvas->format, gwmGetDefaultFont(), 0, 0, canvas->width, canvas->height-11, 0, 0, NULL);
	ddiSetPenAlignment(pen, DDI_ALIGN_CENTER);
	ddiSetPenWrap(pen, 0);
	ddiWritePen(pen, data->text);
	
	int txtWidth, txtHeight;
	ddiGetPenSize(pen, &txtWidth, &txtHeight);
	ddiSetPenPosition(pen, 0, 15-(txtHeight/2));
	ddiExecutePen(pen, canvas);
	ddiDeletePen(pen);
	
	data->prefWidth = txtWidth + 16;
	gwmPostDirty(button);
};

int gwmButtonHandler(GWMEvent *ev, GWMWindow *button, void *context)
{
	int retval = GWM_EVSTATUS_OK;
	GWMButtonData *data = (GWMButtonData*) button->data;
	switch (ev->type)
	{
	case GWM_EVENT_ENTER:
		data->state = BUTTON_STATE_HOVERING;
		gwmRedrawButton(button);
		return GWM_EVSTATUS_OK;
	case GWM_EVENT_LEAVE:
		data->state = BUTTON_STATE_NORMAL;
		gwmRedrawButton(button);
		return GWM_EVSTATUS_OK;
	case GWM_EVENT_DOWN:
		if (ev->keycode == GWM_KC_MOUSE_LEFT)
		{
			data->state = BUTTON_STATE_CLICKED;
			gwmRedrawButton(button);
		};
		return GWM_EVSTATUS_OK;
	case GWM_EVENT_UP:
		if (ev->keycode == GWM_KC_MOUSE_LEFT)
		{
			if (data->state == BUTTON_STATE_CLICKED)
			{
				if (data->callback != NULL)
				{
					if ((data->flags & GWM_BUTTON_DISABLED) == 0)
					{
						retval = data->callback(data->callbackParam);
					};
				};
				data->state = BUTTON_STATE_HOVERING;
			}
			else
			{
				data->state = BUTTON_STATE_NORMAL;
			};
			gwmRedrawButton(button);
		};
		return retval;
	default:
		return GWM_EVSTATUS_CONT;
	};
};

static int defaultButtonCallback(void *context)
{
	GWMWindow *button = (GWMWindow*) context;
	GWMButtonData *data = (GWMButtonData*) button->data;
	
	GWMCommandEvent event;
	memset(&event, 0, sizeof(GWMCommandEvent));
	event.header.type = GWM_EVENT_COMMAND;
	event.symbol = data->symbol;
	
	return gwmPostEvent((GWMEvent*) &event, button);
};

static void gwmSizeButton(GWMWindow *button, int *width, int *height)
{
	GWMButtonData *data = (GWMButtonData*) button->data;
	*width = data->prefWidth;
	*height = BUTTON_HEIGHT;
};

static void gwmPositionButton(GWMWindow *button, int x, int y, int width, int height)
{
	y += (BUTTON_HEIGHT - height) / 2;
	gwmMoveWindow(button, x, y);
	gwmResizeWindow(button, width, BUTTON_HEIGHT);
	gwmRedrawButton(button);
};

GWMWindow* gwmCreateButton(GWMWindow *parent, const char *text, int x, int y, int width, int flags)
{
	GWMWindow *button = gwmCreateWindow(parent, "GWMButton", x, y, width, BUTTON_HEIGHT, 0);
	if (button == NULL) return NULL;
	
	GWMButtonData *data = (GWMButtonData*) malloc(sizeof(GWMButtonData));
	button->data = data;
	
	data->text = strdup(text);
	data->flags = flags;
	data->state = BUTTON_STATE_NORMAL;
	data->callback = defaultButtonCallback;
	data->callbackParam = button;
	data->symbol = 0;
	data->prefWidth = 0;
	
	button->getMinSize = button->getPrefSize = gwmSizeButton;
	button->position = gwmPositionButton;
	
	gwmRedrawButton(button);
	gwmPushEventHandler(button, gwmButtonHandler, NULL);
	return button;
};

void gwmSetButtonSymbol(GWMWindow *button, int symbol)
{
	GWMButtonData *data = (GWMButtonData*) button->data;
	data->symbol = symbol;
};

void gwmDestroyButton(GWMWindow *button)
{
	GWMButtonData *data = (GWMButtonData*) button->data;
	free(data->text);
	free(data);
	gwmDestroyWindow(button);
};

void gwmSetButtonCallback(GWMWindow *button, GWMButtonCallback callback, void *param)
{
	GWMButtonData *data = (GWMButtonData*) button->data;
	data->callback = callback;
	data->callbackParam = param;
};

GWMWindow* gwmCreateStockButton(GWMWindow *parent, int symbol)
{
	return gwmCreateButton(parent, gwmGetStockLabel(symbol), 0, 0, 0, 0);
};
