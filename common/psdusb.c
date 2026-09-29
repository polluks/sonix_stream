/*
 * psdusb.c
 *
 * MorphOS Poseidon backend for the Sonix sn9c102 webcam driver.  Refactored
 * from the original src/libusb_poseidon.c of the Sonix MorphOS port: instead
 * of emulating the whole of libusb-0.1 it exposes the handful of primitives
 * the capture code actually needs, and it does not keep a global device list
 * around, so several capture objects can be alive at once.
 */

#define SYSTEM_PRIVATE

#include <string.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <proto/poseidon.h>
#include <proto/timer.h>
#include <libraries/poseidon.h>
#include <clib/poseidon_protos.h>
#include <clib/timer_protos.h>
#include <devices/timer.h>
#include <devices/usbhardware.h>
#include <exec/libraries.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>

#include "psdusb.h"

/* SysBase is defined by the class that pulls this code in, these two are
 * ours.  The libbase variables of the included proto headers are extern. */

struct Library *PsdBase, *TimerBase;

/* libusb USB_CLASS_* codes, see www.usb.org. */

#define SONIX_USB_CLASS_VIDEO   10

/* Bulk transfer timeout in milliseconds.  A sn9c102 frame needs a bit more
 * than one frame time, 2000 ms is a generous upper bound for one NAK period. */

#define SONIX_USB_TIMEOUT_MS    2000

struct SonixUsb
{
	APTR                    su_Device;
	struct PsdAppBinding  *su_AppBinding;
	struct PsdPipe        *su_CtrlPipe;
	struct PsdPipe        *su_BulkPipe;
	struct MsgPort        *su_MsgPort;
	ULONG                  su_VendorID;
	ULONG                  su_ProductID;
	ULONG                  su_BulkPacketSize;
	BOOL                   su_DeviceGone;
};

static struct Library *sonix_psdbase;
static struct Library *sonix_timerbase;

/* Called by Poseidon when the device we hold a binding on goes away. */

/*
 * The release hook.  Poseidon passes the object given as ABA_UserData, which
 * is the handle, so the flag it sets is the one the next transfer looks at.
 */

static LONG releasehook(APTR arg, APTR obj, APTR message)
{
	struct SonixUsb *su = (struct SonixUsb *)arg;

	arg = arg; obj = obj; message = message;

	if (su) su->su_DeviceGone = TRUE;

	return 0;
}

static struct Hook release_hook;
static BOOL release_hook_ready;

/*
 * init_hook()
 *
 * MorphOS has no MakeHook() in plain C, build the hook by hand.  HookEntry
 * dispatches to h_SubEntry with (object, message), h_Data is what that gives
 * back to the subentry as its first argument.
 *
 * HookEntry is the SDK's assembly shim in exec/hooks.h, and that header only
 * declares it for assembly, so C has to declare it itself.  The call signature
 * is a call the shim makes, never one made from here, hence the void.
 */

#ifndef HookEntry
extern void HookEntry(void);
#endif

static void init_hook(struct Hook *hook, APTR data)
{
	hook->h_MinNode.mln_Succ = NULL;
	hook->h_MinNode.mln_Pred = NULL;
	hook->h_Entry            = (ULONG (*)(void))HookEntry;
	hook->h_SubEntry         = (ULONG (*)(void))releasehook;
	hook->h_Data             = data;
}

/*
 * sonix_usb_init()
 */

BOOL sonix_usb_init(void)
{
	if (sonix_psdbase) return TRUE;

	sonix_timerbase = OpenLibrary("timer.library", 0);
	sonix_psdbase = OpenLibrary("poseidon.library", 0);

	return sonix_psdbase ? TRUE : FALSE;
}

/*
 * sonix_usb_exit()
 */

void sonix_usb_exit(void)
{
	if (sonix_psdbase)
	{
		CloseLibrary(sonix_psdbase);
		sonix_psdbase = NULL;
	}

	if (sonix_timerbase)
	{
		CloseLibrary(sonix_timerbase);
		sonix_timerbase = NULL;
	}
}

/*
 * sonix_usb_now_us()
 */

ULONG sonix_usb_now_us(void)
{
	/*
	 * Spelled the AmigaOS way on purpose.  SDK 3.20 and up renamed the
	 * structure to TimeVal and kept a "timeval" alias for compatibility, so
	 * this name is the only one that works on both an old and a new SDK.
	 */

	struct timeval tv;

	if (!sonix_timerbase) return 0;

	GetUTCSysTime(&tv);

	return tv.tv_secs * 1000000UL + tv.tv_micro;
}

/*
 * copy_string()
 */

static void copy_string(STRPTR dst, ULONG dstlen, STRPTR src)
{
	ULONG n = 0;

	if (!dst || !dstlen) return;
	if (!src) src = "";

	while (src[n] && n < dstlen - 1)
	{
		dst[n] = src[n];
		n++;
	}
	dst[n] = '\0';
}

/*
 * find_device()
 *
 * Return the first attached device matching the given ids.  A vid or pid of
 * zero matches anything.
 */

static APTR find_device(UWORD vendor, UWORD product, STRPTR productname, ULONG buflen,
                         UWORD *foundvendor, UWORD *foundproduct)
{
	APTR pd, found = NULL;

	psdLockReadPBase();

	for (pd = (APTR)psdGetNextDevice(NULL); pd && !found; pd = (APTR)psdGetNextDevice(pd))
	{
		ULONG vid = 0, pid = 0;
		STRPTR name = NULL;

		psdLockReadDevice(pd);

		psdGetAttrs(PGA_DEVICE, pd,
		            DA_VendorID,    &vid,
		            DA_ProductID,   &pid,
		            DA_ProductName, &name,
		            TAG_END);

		if ((!vendor || vid == vendor) && (!product || pid == product))
		{
			copy_string(productname, buflen, name);
			if (foundvendor) *foundvendor  = (UWORD)vid;
			if (foundproduct) *foundproduct = (UWORD)pid;
			found = pd;
		}

		psdUnlockDevice(pd);
	}

	psdUnlockPBase();

	return found;
}

/*
 * find_bulk_in()
 *
 * Return the bulk IN endpoint of one interface or alternate interface, or
 * NULL when it only offers isochronous or interrupt endpoints.
 */

static struct PsdEndpoint *find_bulk_in(struct PsdInterface *pif, ULONG *pktsize)
{
	struct List *eplist = NULL;
	struct Node *node;

	psdGetAttrs(PGA_INTERFACE, pif, IFA_EndpointList, &eplist, TAG_END);
	if (!eplist) return NULL;

	for (node = eplist->lh_Head; node && node->ln_Succ; node = node->ln_Succ)
	{
		struct PsdEndpoint *pep = (struct PsdEndpoint *)node;
		ULONG transfer = 0, size = 0, isin = 0;

		psdGetAttrs(PGA_ENDPOINT, pep,
		            EA_TransferType, &transfer,
		            EA_MaxPktSize,   &size,
		            EA_IsIn,         &isin,
		            TAG_END);

		if (transfer != USEAF_BULK || !isin) continue;

		*pktsize = size;
		return pep;
	}

	return NULL;
}

/*
 * alloc_bulk_pipe()
 *
 * Find an alternate interface carrying a bulk IN endpoint, select it on the
 * control pipe and allocate a pipe bound to it.  Falls back to the endpoints
 * of the interface itself when no alternate list is exposed.
 */

static BOOL alloc_bulk_pipe(struct SonixUsb *su, struct PsdInterface *pif)
{
	struct List *altlist = NULL;
	struct Node *alt;
	struct PsdEndpoint *pep;
	ULONG pktsize = 0;

	psdGetAttrs(PGA_INTERFACE, pif, IFA_AlternateIfList, &altlist, TAG_END);

	for (alt = altlist ? altlist->lh_Head : NULL; alt && alt->ln_Succ; alt = alt->ln_Succ)
	{
		struct PsdInterface *palt = (struct PsdInterface *)alt;
		ULONG altclass = 0;

		pep = find_bulk_in(palt, &pktsize);
		if (!pep) continue;

		psdGetAttrs(PGA_INTERFACE, palt, IFA_Class, &altclass, TAG_END);
		if (altclass != SONIX_USB_CLASS_VIDEO) continue;

		if (!psdSetAltInterface(su->su_CtrlPipe, palt)) continue;

		su->su_BulkPipe = (struct PsdPipe *)
			psdAllocPipe(su->su_Device, su->su_MsgPort, pep);

		if (su->su_BulkPipe)
		{
			su->su_BulkPacketSize = pktsize;
			return TRUE;
		}
	}

	pep = find_bulk_in(pif, &pktsize);
	if (!pep) return FALSE;

	su->su_BulkPipe = (struct PsdPipe *)
		psdAllocPipe(su->su_Device, su->su_MsgPort, pep);

	if (!su->su_BulkPipe) return FALSE;

	su->su_BulkPacketSize = pktsize;
	return TRUE;
}

/*
 * claim_video_interface()
 *
 * Pick the video class interface of the device which exposes a usable bulk
 * endpoint.  Isochronous only cameras (PAC207 and friends) are rejected, they
 * cannot be read with psdDoPipe().
 */

static BOOL claim_video_interface(struct SonixUsb *su)
{
	struct List *iflist = NULL;
	struct Node *node;
	struct PsdConfig *pcfg = NULL;

	psdGetAttrs(PGA_DEVICE, su->su_Device, DA_CurrConfig, &pcfg, TAG_END);
	if (!pcfg) return FALSE;

	psdGetAttrs(PGA_CONFIG, pcfg, CA_InterfaceList, &iflist, TAG_END);
	if (!iflist) return FALSE;

	for (node = iflist->lh_Head; node && node->ln_Succ; node = node->ln_Succ)
	{
		struct PsdInterface *pif = (struct PsdInterface *)node;
		ULONG ifclass = 0;

		psdGetAttrs(PGA_INTERFACE, pif, IFA_Class, &ifclass, TAG_END);
		if (ifclass != SONIX_USB_CLASS_VIDEO) continue;

		if (alloc_bulk_pipe(su, pif)) return TRUE;
	}

	return FALSE;
}

/*
 * sonix_usb_open()
 */

APTR sonix_usb_open(UWORD vendor, UWORD product, STRPTR productname, ULONG buflen)
{
	struct SonixUsb *su;
	UWORD foundvendor = 0, foundproduct = 0;
	APTR pd;

	if (productname && buflen) productname[0] = '\0';
	if (!sonix_psdbase) return NULL;

	pd = find_device(vendor, product, productname, buflen, &foundvendor, &foundproduct);
	if (!pd) return NULL;

	su = (struct SonixUsb *)psdAllocVec(sizeof(struct SonixUsb));
	if (!su) return NULL;

	memset(su, 0, sizeof(*su));

	su->su_Device     = pd;
	su->su_VendorID   = foundvendor;
	su->su_ProductID  = foundproduct;

	su->su_MsgPort = CreateMsgPort();
	if (!su->su_MsgPort)
	{
		psdFreeVec(su);
		return NULL;
	}

	if (!release_hook_ready)
	{
		init_hook(&release_hook, su);
		release_hook_ready = TRUE;
	}
	else release_hook.h_Data = su;

	su->su_AppBinding = (struct PsdAppBinding *)
		psdClaimAppBinding(ABA_Device,      pd,
		                   ABA_ReleaseHook, &release_hook,
		                   ABA_UserData,    su,
		                   TAG_END);
	if (!su->su_AppBinding)
	{
		DeleteMsgPort(su->su_MsgPort);
		psdFreeVec(su);
		return NULL;
	}

	su->su_CtrlPipe = (struct PsdPipe *)psdAllocPipe(pd, su->su_MsgPort, NULL);
	if (!su->su_CtrlPipe || !claim_video_interface(su))
	{
		sonix_usb_close(su);
		return NULL;
	}

	/* A bulk IN pipe on a webcam NAKs while the camera has no frame ready,
	 * so let Poseidon retry instead of failing the transfer right away. */

	psdSetAttrs(PGA_PIPE, su->su_BulkPipe,
	            PPA_AllowRuntPackets, TRUE,
	            PPA_NakTimeout,       TRUE,
	            PPA_NakTimeoutTime,   SONIX_USB_TIMEOUT_MS,
	            TAG_END);

	return su;
}

/*
 * sonix_usb_close()
 */

void sonix_usb_close(APTR handle)
{
	struct SonixUsb *su = (struct SonixUsb *)handle;

	if (!su) return;

	if (su->su_BulkPipe) psdFreePipe(su->su_BulkPipe);
	if (su->su_CtrlPipe) psdFreePipe(su->su_CtrlPipe);
	if (su->su_AppBinding) psdReleaseAppBinding(su->su_AppBinding);
	if (su->su_MsgPort) DeleteMsgPort(su->su_MsgPort);

	psdFreeVec(su);
}

/*
 * sonix_usb_ids()
 */

void sonix_usb_ids(APTR handle, UWORD *vendor, UWORD *product)
{
	struct SonixUsb *su = (struct SonixUsb *)handle;

	if (vendor)  *vendor  = su ? (UWORD)su->su_VendorID  : 0;
	if (product) *product = su ? (UWORD)su->su_ProductID : 0;
}

/*
 * sonix_usb_control()
 */

LONG sonix_usb_control(APTR handle, ULONG requesttype, ULONG request,
                       ULONG value, ULONG index, UBYTE *bytes, ULONG size)
{
	struct SonixUsb *su = (struct SonixUsb *)handle;
	LONG result;

	if (!su || !su->su_CtrlPipe || su->su_DeviceGone) return -1;

	psdPipeSetup(su->su_CtrlPipe, requesttype, request, value, index);
	result = psdDoPipe(su->su_CtrlPipe, bytes, size);

	return result < 0 ? -1 : result;
}

/*
 * sonix_usb_bulk_read()
 */

LONG sonix_usb_bulk_read(APTR handle, UBYTE *bytes, ULONG size)
{
	struct SonixUsb *su = (struct SonixUsb *)handle;
	LONG result;

	if (!su || !su->su_BulkPipe || su->su_DeviceGone) return -1;

	result = psdDoPipe(su->su_BulkPipe, bytes, size);

	return result < 0 ? -1 : result;
}

/*
 * sonix_usb_bulk_packetsize()
 */

ULONG sonix_usb_bulk_packetsize(APTR handle)
{
	struct SonixUsb *su = (struct SonixUsb *)handle;

	return su ? su->su_BulkPacketSize : 0;
}

/*
 * sonix_usb_delay_ms()
 */

void sonix_usb_delay_ms(LONG ms)
{
	if (ms > 0) psdDelayMS((ULONG)ms);
}
