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

#include <glidix/usb/usb.h>
#include <glidix/util/string.h>
#include <glidix/util/memory.h>
#include <glidix/thread/sched.h>
#include <glidix/display/console.h>
#include <glidix/thread/mutex.h>

static uint8_t addrBitmap[16];
static USBCtrl ctrlNop;

static uint64_t nextDevID = 1;

static Mutex mtxDevList;
static USBDevice *devHead;
static USBDriver *drvHead;
static Semaphore semDevUpdate;

static USBTransferInfo tiGetDeviceDescriptor[] = {
	{USB_SETUP, 0, sizeof(USBSetupPacket)},
	{USB_IN, 1, sizeof(USBDeviceDescriptor)},
	{USB_OUT, 0, 0},
	{USB_END}
};

static USBTransferInfo tiGetConfigurationDescriptor[] = {
	{USB_SETUP, 0, sizeof(USBSetupPacket)},
	{USB_IN, 1, sizeof(USBConfigurationDescriptor)},
	{USB_OUT, 0, 0},
	{USB_END}
};

static USBTransferInfo tiSetConfig[] = {
	{USB_SETUP, 0, sizeof(USBSetupPacket)},
	{USB_IN, 1, 0},
	{USB_END}
};

void usbUp(USBDevice *dev)
{
	__sync_fetch_and_add(&dev->refcount, 1);
};

void usbDown(USBDevice *dev)
{
	if (__sync_add_and_fetch(&dev->refcount, -1) == 0)
	{
		usbReleaseAddr(dev->addr);
		kfree(dev);
	};
};

void usbDevInitFunc(void *context)
{
	// the device is alread upreffered for us
	USBDevice *dev = (USBDevice*) context;
	detachMe();
	
	// set up the default control pipe
	USBControlPipe *pipe = usbCreateControlPipe(dev, 0, 64);
	if (pipe == NULL)
	{
		kprintf_debug("usb: failed to create default control pipe\n");
		usbDown(dev);
		return;
	};
	
	dev->ctrlPipe = pipe;
	
	// get the device descriptor
	struct
	{
		USBSetupPacket setup;
		USBDeviceDescriptor desc;
	} reqDev;
	
	memset(&reqDev, 0, sizeof(reqDev));
	reqDev.setup.bmRequestType = USB_REQ_DEVICE_TO_HOST | USB_REQ_STANDARD | USB_REQ_DEVICE;
	reqDev.setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	reqDev.setup.wValue = 0x0100;				// device descriptor zero
	reqDev.setup.wIndex = 0;
	reqDev.setup.wLength = sizeof(USBDeviceDescriptor);

	if (usbControlTransfer(pipe, tiGetDeviceDescriptor, &reqDev) != 0)
	{
		kprintf_debug("[USB] failed to perform GET_DESCRIPTOR command (for device descriptor)\n");
		usbDeleteControlPipe(pipe);
		usbDown(dev);
		return;
	};
	
	kprintf_debug("[USB] device connected:\n");
	usbPrintDeviceDescriptor(&reqDev.desc);
	
	// copy the device descriptor into the device struct
	memcpy(&dev->descDev, &reqDev.desc, sizeof(USBDeviceDescriptor));
	
	// make sure there is at least one configuration descriptor (sanity check), then get it
	if (reqDev.desc.bNumConfigurations < 1)
	{
		kprintf_debug("[USB] This device has no configurations!\n");
		usbDeleteControlPipe(pipe);
		usbDown(dev);
		return;
	};
	
	struct
	{
		USBSetupPacket setup;
		USBConfigurationDescriptor desc;
	} reqConf;
	
	memset(&reqConf, 0, sizeof(reqConf));
	reqConf.setup.bmRequestType = USB_REQ_DEVICE_TO_HOST | USB_REQ_STANDARD | USB_REQ_DEVICE;
	reqConf.setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	reqConf.setup.wValue = 0x0200;				// first configuration descriptor
	reqConf.setup.wIndex = 0;
	reqConf.setup.wLength = sizeof(USBConfigurationDescriptor);
	
	if (usbControlTransfer(pipe, tiGetConfigurationDescriptor, &reqConf) != 0)
	{
		kprintf_debug("[USB] Failed to get the default configuration descriptor\n");
		usbDeleteControlPipe(pipe);
		usbDown(dev);
		return;
	};
	
	kprintf_debug("[USB] Default configuration descriptor:\n");
	usbPrintConfigurationDescriptor(&reqConf.desc);
	
	// set the configuration
	USBSetupPacket setconf;
	memset(&setconf, 0, sizeof(USBSetupPacket));
	setconf.bmRequestType = USB_REQ_HOST_TO_DEVICE | USB_REQ_STANDARD | USB_REQ_DEVICE;
	setconf.bRequest = USB_REQ_SET_CONFIGURATION;
	setconf.wValue = reqConf.desc.bConfigurationValue;
	
	if (usbControlTransfer(pipe, tiSetConfig, &setconf) != 0)
	{
		kprintf_debug("[USB] Failed to set configuration to default\n");
		usbDeleteControlPipe(pipe);
		usbDown(dev);
		return;
	};
	
	kprintf_debug("[USB] Device configured.\n");
	
	// scan through the interface descriptors and create interface objects
	uint8_t *confbuf = (uint8_t*) kmalloc(reqConf.desc.wTotalLength);
	if (usbControlIn(pipe, USB_REQ_DEVICE_TO_HOST | USB_REQ_STANDARD | USB_REQ_DEVICE, USB_REQ_GET_DESCRIPTOR,
				0x0200, 0, reqConf.desc.wTotalLength, confbuf) != 0)
	{
		kprintf_debug("[USB] Failed to read full configuration\n");
		kfree(confbuf);
		usbDeleteControlPipe(pipe);
		usbDown(dev);
	};
	
	kprintf_debug("[USB] The following configuration is in use:\n");
	
	uint8_t *scan = confbuf + reqConf.desc.bLength;
	size_t numInterfaces = reqConf.desc.bNumInterfaces;
	
	while (numInterfaces--)
	{
		USBInterfaceDescriptor *descIntf = (USBInterfaceDescriptor*) scan;
		kprintf_debug("INTF (len=0x%02hhX, type=0x%02hhX, intno=0x%02hhX, alt=0x%02hhX, epcount=%hhu, class=0x%02hhX:0x%02hhX:0x%02hhX)\n",
			descIntf->bLength,
			descIntf->bDescriptorType,
			descIntf->bInterfaceNumber,
			descIntf->bAlternateSetting,
			descIntf->bNumEndpoints,
			descIntf->bInterfaceClass,
			descIntf->bInterfaceSubClass,
			descIntf->bInterfaceProtocol
		);
		
		scan += descIntf->bLength;
		
		size_t numEndpoints = descIntf->bNumEndpoints;
		while (numEndpoints--)
		{
			USBEndpointDescriptor *descEp = (USBEndpointDescriptor*) scan;
			kprintf_debug("  ENDPOINT (addr=0x%02hhX, attr=0x%02hhX, maxpacket=%hu, interval=%hhu)\n",
				descEp->bEndpointAddress,
				descEp->bmAttributes,
				descEp->wMaxPacketSize,
				descEp->bInterval
			);
			
			scan += descEp->bLength;
		};
		
		kprintf_debug("\n");
	};
	
	// done
	kfree(confbuf);
	
	// put this device on the device list. we keep the reference to it
	mutexLock(&mtxDevList);
	semWait(&dev->lock);
	
	if (dev->flags & USB_DEV_HANGUP)
	{
		// the device already hanged up
		semSignal(&dev->lock);
		mutexUnlock(&mtxDevList);
		return;
	};
	
	dev->next = devHead;
	devHead = dev;
	
	semSignal(&dev->lock);
	mutexUnlock(&mtxDevList);
	
	// report that the list has been update
	semSignal(&semDevUpdate);
};

void usbPrintDeviceDescriptor(USBDeviceDescriptor *desc)
{
	kprintf_debug("  USB Version:               %hu.%hu\n", desc->bcdUSB >> 8, desc->bcdUSB & 0xFF);
	kprintf_debug("  USB Device Class:          0x%02hhX\n", desc->bDeviceClass);
	kprintf_debug("  USB Device Subclass:       0x%02hhX\n", desc->bDeviceSubClass);
	kprintf_debug("  USB Device Protocol:       0x%02hhX\n", desc->bDeviceProtocol);
	kprintf_debug("  Max. Packet Size:          %hhu bytes\n", desc->bMaxPacketSize);
	kprintf_debug("  USB Vendor ID:             0x%04hX\n", desc->idVendor);
	kprintf_debug("  USB Product ID:            0x%04hX\n", desc->idProduct);
	kprintf_debug("  USB Device Version:        %hu.%hu\n", desc->bcdDevice >> 8, desc->bcdDevice & 0xFF);
	kprintf_debug("  Manufacturer String ID:    %hhu\n", desc->iManufacturer);
	kprintf_debug("  Product String ID:         %hhu\n", desc->iProduct);
	kprintf_debug("  Serial Number String ID:   %hhu\n", desc->iSerialNumber);
	kprintf_debug("  Number of Configurations:  %hhu\n", desc->bNumConfigurations);
};

void usbPrintConfigurationDescriptor(USBConfigurationDescriptor *desc)
{
	kprintf_debug("  Total length:              %hu bytes\n", desc->wTotalLength);
	kprintf_debug("  Number of Interfaces:      %hhu\n", desc->bNumInterfaces);
	kprintf_debug("  Configuration Value:       0x%02hhX\n", desc->bConfigurationValue);
	kprintf_debug("  Configuration String ID:   %hhu\n", desc->iConfiguration);
	kprintf_debug("  Attributes:                0x%02hhX\n", desc->bmAttributes);
	kprintf_debug("  Max. Power:                %hu mA\n", 2 * (uint16_t) desc->bMaxPower);
};

void usbDriverFunc(void *context)
{
	USBDevice *dev = (USBDevice*) context;
	detachMe();
	
	dev->driver->attach(dev->driver->drvdata, dev);
	
	// OK, "undrive" me
	mutexLock(&mtxDevList);
	semWait(&dev->lock);
	dev->driver = NULL;
	semSignal(&dev->lock);
	mutexUnlock(&mtxDevList);
	
	usbDown(dev);
};

void usbDriverAllocFunc(void *context)
{
	// the job of this thread is to simply loop, waiting for device/driver list updates,
	// and then scan the lists to attach devices to drivers
	while (1)
	{
		semWait(&semDevUpdate);
		
		mutexLock(&mtxDevList);
		
		USBDevice *dev;
		for (dev=devHead; dev!=NULL; dev=dev->next)
		{
			semWait(&dev->lock);
			if (dev->driver != NULL)
			{
				semSignal(&dev->lock);
				continue;
			};
			
			USBDriver *drv;
			for (drv=drvHead; drv!=NULL; drv=drv->next)
			{
				if (drv->isSupportedDev(drv->drvdata, dev))
				{
					// attach the driver
					usbUp(dev);
					dev->driver = drv;
					
					KernelThreadParams pars;
					memset(&pars, 0, sizeof(KernelThreadParams));
					pars.stackSize = DEFAULT_STACK_SIZE;
					pars.name = "USB Driver Thread";
					CreateKernelThread(usbDriverFunc, &pars, dev);
					
					break;
				};
			};
			
			semSignal(&dev->lock);
		};
		
		mutexUnlock(&mtxDevList);
	};
};

void usbInit()
{
	addrBitmap[0] = 1;		// address 0 reserved
	mutexInit(&mtxDevList);
	semInit2(&semDevUpdate, 0);

	KernelThreadParams pars;
	memset(&pars, 0, sizeof(KernelThreadParams));
	pars.stackSize = DEFAULT_STACK_SIZE;
	pars.name = "USB Driver Allocator Thread";
	CreateKernelThread(usbDriverAllocFunc, &pars, NULL);
};

usb_addr_t usbAllocAddr()
{
	usb_addr_t i;
	for (i=1; i<128; i++)
	{
		uint8_t index = i / 8;
		uint8_t mask = (1 << (i % 8));
		
		if ((__sync_fetch_and_or(&addrBitmap[index], mask) & mask) == 0)
		{
			break;
		};
	};
	
	return i;
};

void usbReleaseAddr(usb_addr_t addr)
{
	uint8_t index = addr / 8;
	uint8_t mask = (1 << (addr % 8));
	__sync_fetch_and_and(&addrBitmap[index], ~mask);
};

USBDevice* usbCreateDevice(usb_addr_t addr, USBCtrl *ctrl, int flags, void *data, int usbver)
{
	USBDevice *dev = NEW(USBDevice);
	memset(dev, 0, sizeof(USBDevice));
	dev->addr = addr;
	dev->ctrl = ctrl;
	dev->flags = flags;
	dev->data = data;
	dev->usbver = usbver;
	dev->refcount = 2;		/* one returned, one for the init thread */
	semInit(&dev->lock);
	dev->devid = __sync_fetch_and_add(&nextDevID, 1);
	
	KernelThreadParams pars;
	memset(&pars, 0, sizeof(KernelThreadParams));
	pars.stackSize = DEFAULT_STACK_SIZE;
	pars.name = "USB Device Initializer Thread";
	CreateKernelThread(usbDevInitFunc, &pars, dev);
	
	return dev;
};

void usbHangup(USBDevice *dev)
{
	mutexLock(&mtxDevList);
	semWait(&dev->lock);
	dev->flags |= USB_DEV_HANGUP;
	dev->ctrl = &ctrlNop;
	semSignal(&dev->lock);
	
	int doubleDown = 0;
	if (dev->prev != NULL || devHead == dev)
	{
		if (dev->prev != NULL) dev->prev->next = dev->next;
		if (dev->next != NULL) dev->next->prev = dev->prev;
		if (devHead == dev) devHead = dev->next;
		
		doubleDown = 1;
	};
	
	mutexUnlock(&mtxDevList);
	
	usbDown(dev);
	if (doubleDown) usbDown(dev);
	
	// NOTE: here we do not need to signal the list update semaphore. this is because no action needs to
	//       be taken by the attachment thread when a device hangs up; only if a new device is connected,
	//       or if a new driver becomes available.
};

USBControlPipe* usbCreateControlPipe(USBDevice *dev, usb_epno_t epno, size_t maxPacketLen)
{
	// create the pipe driver data
	semWait(&dev->lock);
	if (dev->ctrl->createControlPipe == NULL)
	{
		semSignal(&dev->lock);
		return NULL;
	};
	
	void *pipedata = dev->ctrl->createControlPipe(dev, epno, maxPacketLen);
	semSignal(&dev->lock);
	
	if (pipedata == NULL)
	{
		return NULL;
	};
	
	// upref the device to place into the pipe object
	usbUp(dev);
	
	// create the pipe
	USBControlPipe *pipe = NEW(USBControlPipe);
	pipe->dev = dev;
	pipe->data = pipedata;
	pipe->maxPacketLen = maxPacketLen;
	
	return pipe;
};

void usbDeleteControlPipe(USBControlPipe *pipe)
{
	USBDevice *dev = pipe->dev;
	
	semWait(&dev->lock);
	if (dev->ctrl->deleteControlPipe != NULL)
	{
		dev->ctrl->deleteControlPipe(pipe->data);
	};
	semSignal(&dev->lock);
	
	usbDown(dev);
	kfree(pipe);
};

USBRequest* usbCreateControlRequest(USBControlPipe *pipe, USBTransferInfo *packets, void *buffer, Semaphore *semComplete)
{
	USBRequest *urb = (USBRequest*) kmalloc(sizeof(urb->control));
	memset(urb, 0, sizeof(urb->control));
	urb->header.type = USB_TRANS_CONTROL;
	urb->header.semComplete = semComplete;
	urb->header.dev = pipe->dev;
	usbUp(pipe->dev);
	urb->header.size = sizeof(urb->control);
	
	urb->control.pipe = pipe;
	urb->control.packets = packets;
	urb->control.buffer = buffer;
	
	return urb;
};

int usbSubmitRequest(USBRequest *urb)
{
	USBCtrl *ctrl = urb->header.dev->ctrl;
	if (ctrl->submit == NULL)
	{
		return ENOSYS;
	};
	
	return ctrl->submit(urb);
};

void usbDeleteRequest(USBRequest *urb)
{
	kfree(urb);
};

int usbControlTransfer(USBControlPipe *pipe, USBTransferInfo *packets, void *buffer)
{
	int status;
	
	Semaphore sem;
	semInit2(&sem, 0);
	USBRequest *urb = usbCreateControlRequest(pipe, packets, buffer, &sem);
	if (urb == NULL)
	{
		return -1;
	};
	
	if ((status = usbSubmitRequest(urb)) != 0)
	{
		usbDeleteRequest(urb);
		return status;
	};
	
	semWait(&sem);
	status = usbGetRequestStatus(urb);
	usbDeleteRequest(urb);
	return status;
};

int usbControlIn(USBControlPipe *pipe, uint8_t bmRequestType, uint8_t bRequest,
			uint16_t wValue, uint16_t wIndex, uint16_t wLength,
			void *buffer)
{
	uint8_t *transbuf = (uint8_t*) kmalloc(sizeof(USBSetupPacket) + (size_t) wLength);
	USBSetupPacket *setup = (USBSetupPacket*) transbuf;
	
	setup->bmRequestType = bmRequestType;
	setup->bRequest = bRequest;
	setup->wValue = wValue;
	setup->wIndex = wIndex;
	setup->wLength = wLength;
	
	USBTransferInfo *ti = (USBTransferInfo*) kmalloc(sizeof(USBTransferInfo)
					* (((size_t)wLength + pipe->maxPacketLen-1) / pipe->maxPacketLen + 3));
	
	USBTransferInfo *put = ti;
	put->type = USB_SETUP;
	put->dt = 0;
	put->size = sizeof(USBSetupPacket);
	put++;
	
	usb_dt_t nextDT = 1;
	size_t sizeToGo = (size_t) wLength;
	while (sizeToGo > 0)
	{
		size_t sizeNow = sizeToGo;
		if (sizeNow > pipe->maxPacketLen) sizeNow = pipe->maxPacketLen;
		
		put->type = USB_IN;
		put->dt = nextDT;
		nextDT ^= 1;
		put->size = sizeNow;
		
		sizeToGo -= sizeNow;
		put++;
	};
	
	put->type = USB_OUT;
	put->dt = nextDT;
	put->size = 0;
	
	put++;
	put->type = USB_END;
	
	int status = usbControlTransfer(pipe, ti, transbuf);
	memcpy(buffer, transbuf + sizeof(USBSetupPacket), (size_t) wLength);
	kfree(ti);
	kfree(transbuf);
	return status;
};

void usbPostComplete(USBRequest *urb)
{
	if (urb->header.semComplete != NULL) semSignal(urb->header.semComplete);
};

int usbGetRequestStatus(USBRequest *urb)
{
	return urb->header.status;
};

USBDriver* usbCreateDriver(Module *mod, void *drvdata,
	int (*isSupportedDev)(void *drvdata, USBDevice *dev),
	void (*attach)(void *drvdata, USBDevice *dev)
)
{
	USBDriver *drv = NEW(USBDriver);
	memset(drv, 0, sizeof(USBDriver));
	drv->mod = mod;
	drv->drvdata = drvdata;
	drv->isSupportedDev = isSupportedDev;
	drv->attach = attach;
	
	mutexLock(&mtxDevList);
	drv->next = drvHead;
	drvHead = drv;
	mutexUnlock(&mtxDevList);
	
	semSignal(&semDevUpdate);
	
	return drv;
};

void usbDestroyDriver(USBDriver *drv)
{
	mutexLock(&mtxDevList);
	if (drv->prev != NULL) drv->prev->next = drv->next;
	if (drv->next != NULL) drv->next->prev = drv->prev;
	if (drvHead == drv) drvHead = drv->next;
	mutexUnlock(&mtxDevList);
	
	kfree(drv);
};

void usbList(uint64_t *list)
{
	mutexLock(&mtxDevList);
	
	USBDevice *dev;
	for (dev=devHead; dev!=NULL; dev=dev->next)
	{
		*list++ = dev->devid;
	};
	
	mutexUnlock(&mtxDevList);
};

int usbGetDeviceDesc(uint64_t devid, USBDeviceDescriptor *desc)
{
	mutexLock(&mtxDevList);
	
	USBDevice *dev;
	for (dev=devHead; dev!=NULL; dev=dev->next)
	{
		if (dev->devid == devid)
		{
			memcpy(desc, &dev->descDev, sizeof(USBDeviceDescriptor));
			mutexUnlock(&mtxDevList);
			return 0;
		};
	};
	
	mutexUnlock(&mtxDevList);
	return ENOENT;
};

int usbGetLangIDs(uint64_t devid, uint16_t *langids, int *numOut)
{
	mutexLock(&mtxDevList);
	
	USBDevice *dev;
	for (dev=devHead; dev!=NULL; dev=dev->next)
	{
		if (dev->devid == devid)
		{
			usbUp(dev);
			break;
		};
	};
	
	mutexUnlock(&mtxDevList);
	
	if (dev == NULL) return ENOENT;

	// get the language list
	USBLangList desc;
	
	if (usbControlIn(dev->ctrlPipe, USB_REQ_DEVICE_TO_HOST | USB_REQ_STANDARD | USB_REQ_DEVICE,
				USB_REQ_GET_DESCRIPTOR,
				0x0300,				// string descriptor zero
				0,				// language list
				2,
				&desc) != 0)
	{
		usbDown(dev);
		return EIO;
	};
	
	// actually get it now that we know its size
	if (usbControlIn(dev->ctrlPipe, USB_REQ_DEVICE_TO_HOST | USB_REQ_STANDARD | USB_REQ_DEVICE,
				USB_REQ_GET_DESCRIPTOR,
				0x0300,
				0,
				desc.bLength,
				&desc) != 0)
	{
		usbDown(dev);
		return EIO;
	};
	
	usbDown(dev);
	memcpy(langids, desc.wLANGID, desc.bLength-2);
	*numOut = (desc.bLength-2) >> 1;
	return 0;
};

int usbGetString(uint64_t devid, uint8_t index, uint16_t langid, char *buffer)
{
	if (index == 0) return EINVAL;
	
	mutexLock(&mtxDevList);
	
	USBDevice *dev;
	for (dev=devHead; dev!=NULL; dev=dev->next)
	{
		if (dev->devid == devid)
		{
			usbUp(dev);
			break;
		};
	};
	
	mutexUnlock(&mtxDevList);
	
	if (dev == NULL) return ENOENT;
	
	USBStringDescriptor desc;
	
	if (usbControlIn(dev->ctrlPipe, USB_REQ_DEVICE_TO_HOST | USB_REQ_STANDARD | USB_REQ_DEVICE,
				USB_REQ_GET_DESCRIPTOR,
				0x0300 | (uint16_t)index,
				langid,
				2,
				&desc) != 0)
	{
		usbDown(dev);
		return EIO;
	};
	
	if (usbControlIn(dev->ctrlPipe, USB_REQ_DEVICE_TO_HOST | USB_REQ_STANDARD | USB_REQ_DEVICE,
				USB_REQ_GET_DESCRIPTOR,
				0x0300 | (uint16_t)index,
				langid,
				desc.bLength,
				&desc) != 0)
	{
		usbDown(dev);
		return EIO;
	};
	
	usbDown(dev);
	
	size_t numChars = (desc.bLength-2) >> 1;
	const uint16_t *scan = desc.wString;
	
	while (numChars--)
	{
		*buffer++ = (uint8_t) *scan++;
	};
	
	*buffer = 0;
	return 0;
};
