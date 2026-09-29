/// autodoc

/****** sonix.stream/background **********************************************
*
* DESCRIPTION
*   Reggae source class for Sonix sn9c102 based USB webcams (Sweex 100K,
*   Macally IceCam, Trust SpaceCam and friends) speaking through Poseidon.
*
*   The object is an MMCLASS_STREAM with a single output port 0 carrying
*   MMF_STREAM.  The camera is opened on MMM_Setup(), never on OM_NEW, so an
*   application can build the whole pipeline before touching the hardware.
*
*   The byte stream is described by sonixwire.h: one SnxStreamHeader followed
*   by any number of SnxFrameHeader + frame data pairs.  sonix.demuxer
*   consumes it and republishes the pictures as MMF_VIDEO_RGB24 or
*   MMF_VIDEO_GRAY8.
*
*   MMM_Peek() is answered by the class instead of being left to the
*   superclass: the default implementation pulls the bytes and seeks back,
*   which a live camera has no way of doing, and the recognition code of
*   sonix.demuxer peeks the first 32 bytes of the source to find the stream
*   header.  MMM_Restore() is answered with a plain TRUE to go with it.
*
* NEW ATTRIBUTES
*   MMA_Sonix_VendorID     (V1)  [I.S.G], UWORD
*   MMA_Sonix_ProductID    (V1)  [I.S.G], UWORD
*   MMA_Sonix_Format       (V1)  [I.S.G], ULONG
*   MMA_Sonix_ProductName  (V1)  [..S.G], STRPTR
*   MMA_Sonix_Sensor       (V1)  [..S.G], ULONG
*   MMA_Sonix_SensorName   (V1)  [..S.G], STRPTR
*   MMA_Sonix_Red          (V1)  [..S.G], LONG
*   MMA_Sonix_Green        (V1)  [..S.G], LONG
*   MMA_Sonix_Blue         (V1)  [..S.G], LONG
*   MMA_Sonix_Gain         (V1)  [..S.G], LONG
*   MMA_Sonix_Brightness   (V1)  [..S.G], LONG
*   MMA_Sonix_Contrast     (V1)  [..S.G], LONG
*
*   MMA_Sonix_VendorID, MMA_Sonix_ProductID and MMA_Sonix_Format describe what
*   is asked for and are only settable before the camera has been claimed, the
*   ids of the camera which was claimed travel in the stream header.
*
* NEW METHODS
*   MMM_Pull(port, buffer, length) (V1)
*   MMM_Peek(port, buffer, length) (V1)
*   MMM_Restore() (V1)
*
*   1.0  (29.09.2026)
*   - Initial revision.
*
*****************************************************************************
*/

///
/// includes

#define __NOLIBBASE__
#define SYSTEM_PRIVATE

#include <string.h>
#include <ctype.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/multimedia.h>
#include <proto/query.h>
#include <emul/emulregs.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <clib/alib_protos.h>
#include <clib/debug_protos.h>
#include <classes/multimedia/multimedia.h>
#include <classes/multimedia/video.h>
#include <classes/multimedia/streams.h>

#include "sonix.stream.h"
#include "../sonixwire.h"
#include "../common/capture.h"
#include "../common/bayer.h"

///
/// basic defs

#define SUPERCLASS "multimedia.class"

#include "class_version.h"

struct Library *SysBase, *IntuitionBase, *UtilityBase, *MultimediaBase;

struct ClassBase
{
	struct Library          LibNode;
	Class                  *LibClass;
	APTR                    Seglist;
	struct SignalSemaphore  BaseLock;
	BOOL                    InitFlag;
	const struct TagItem   *Attributes;
};

/*
 * Instance data.  os_Output is the byte stream handed to the demuxer, filled
 * from a single captured frame: header first, then frame header and pixels.
 * One frame is captured per MMM_Pull or MMM_Peek, so there is no queue to run
 * dry in the middle of a picture.
 */

struct ObjData
{
	struct SonixCam   *od_Cam;
	UBYTE              *od_Output;    /* SnxStreamHeader + SnxFrameHeader + pixels */
	ULONG               od_OutputLen;
	ULONG               od_OutputPos;
	UBYTE              *od_Raw;       /* Bayer frame straight from the camera       */
	ULONG               od_RawLen;
	ULONG               od_PixelLen;
	ULONG               od_Format;
	ULONG               od_Vendor;
	ULONG               od_Product;
	ULONG               od_FrameIndex;
	ULONG               od_StartTime;
	BOOL                od_HeaderSent;
	BOOL                od_FrameReady;  /* od_Output holds an unserved frame */
	STRPTR              od_ProductName;
	STRPTR              od_DataFormat;
	BOOL                od_Opened;
};

///
/// class macros

#define GET_BASE struct ClassBase *cb = (struct ClassBase*)cl->cl_UserData
#define GET_DATA struct ObjData *d = (struct ObjData*)INST_DATA(cl, obj)

///
/// prototypes

struct Library *LibInit(struct Library *unused, APTR seglist, struct Library *sysb);
struct ClassBase *lib_init(struct ClassBase *cb, APTR seglist, struct Library *SysBase);
APTR lib_expunge(struct ClassBase *cb);
struct Library *LibOpen(void);
ULONG LibClose(void);
APTR LibExpunge(void);
ULONG LibReserved(void);
Class *GetClass(void);
LONG ClassDispatcher(void);
Class *init_class(struct ClassBase *cb);
BOOL InitResources(struct ClassBase *cb);
void FreeResources(struct ClassBase *cb);
LONG New(Class *cl, Object *obj, struct opSet *msg);
LONG Dispose(Class *cl, Object *obj, Msg msg);
LONG Get(Class *cl, Object *obj, struct opGet *msg);

/* class specific */

LONG Pull(Class *cl, Object *obj, struct mmopData *msg);
LONG Peek(Class *cl, Object *obj, struct mmopData *msg);
LONG Restore(Class *cl, Object *obj, Msg msg);
LONG Setup(Class *cl, Object *obj, struct mmopPort *msg);
LONG Seek(Class *cl, Object *obj, struct mmopSeek *msg);
LONG GetPort(Class *cl, Object *obj, struct mmopGetPort *msg);
LONG SetPort(Class *cl, Object *obj, struct mmopSetPort *msg);
LONG Set(Class *cl, Object *obj, struct opSet *msg);
BOOL OpenCamera(Class *cl, Object *obj);
void CloseCamera(struct ObjData *d);
BOOL BuildHeader(struct ObjData *d);
BOOL CaptureFrame(Object *obj, struct ObjData *d);
BOOL PrepareFrame(Class *cl, Object *obj, struct ObjData *d);
BOOL SetFormat(struct ObjData *d, ULONG format);
ULONG ParseSelector(STRPTR selector, UWORD *vendor, UWORD *product);
BOOL hex_value(UBYTE c, UWORD *value);

///
/// dummy_function()

LONG dummy_function(void)
{
	return -1;
}

///
/// resident

const char LibName[] = CLASSNAME;
char VTag[] = VERSTAG;

static const struct TagItem RTags[] =
{
	{QUERYINFOATTR_NAME, (ULONG)LibName},
	{QUERYINFOATTR_IDSTRING, (ULONG)&VTag[1]},
	{QUERYINFOATTR_DESCRIPTION, (ULONG)"Sonix sn9c102 webcam source"},
	{QUERYINFOATTR_COPYRIGHT, (ULONG)"(c) 2026"},
	{QUERYINFOATTR_AUTHOR, (ULONG)"Reggae contributors"},
	{QUERYINFOATTR_DATE, (ULONG)DATE},
	{QUERYINFOATTR_VERSION, VERSION},
	{QUERYINFOATTR_REVISION, REVISION},
	{QUERYINFOATTR_SUBTYPE, QUERYSUBTYPE_LIBRARY},
	{QUERYINFOATTR_CLASS, QUERYCLASS_MULTIMEDIA},
	{QUERYINFOATTR_SUBCLASS, QUERYSUBCLASS_MULTIMEDIA_STREAM},
	{MMA_MediaType, MMT_VIDEO},
	{MMA_SupportedFormats, (ULONG)"M"},
	{TAG_END, 0}
};

static const ULONG OutputFormats[] = { MMF_STREAM, 0 };

struct Resident ROMTag =
{
	RTC_MATCHWORD,
	&ROMTag,
	&ROMTag + 1,
	RTF_EXTENDED | RTF_PPC,
	VERSION,
	NT_LIBRARY,
	0,
	(STRPTR)LibName,
	VSTRING,
	(APTR)LibInit,
	REVISION,
	(struct TagItem *)RTags
};

APTR JumpTable[] =
{
	(APTR)FUNCARRAY_32BIT_NATIVE,
	(APTR)LibOpen,
	(APTR)LibClose,
	(APTR)LibExpunge,
	(APTR)LibReserved,
	(APTR)GetClass,
	(APTR)0xFFFFFFFF
};

///
/// init_class()

static const struct EmulLibEntry ClassDispatcher_gate =
{
	TRAP_LIB,
	0,
	(void(*)(void))ClassDispatcher
};

Class *init_class(struct ClassBase *cb)
{
	Class *cl = NULL;

	if ((cl = MakeClass(LibName, SUPERCLASS, NULL, sizeof(struct ObjData), 0L)))
	{
		cl->cl_Dispatcher.h_Entry = (HOOKFUNC)&ClassDispatcher_gate;
		cl->cl_UserData = (ULONG)cb;
		AddClass(cl);
	}
	cb->LibClass = cl;

	return cl;
}

///
/// InitResources()

BOOL InitResources(struct ClassBase *cb)
{
	if (!(IntuitionBase = OpenLibrary("intuition.library", 50))) return FALSE;
	if (!(UtilityBase = OpenLibrary("utility.library", 50))) return FALSE;
	if (!(MultimediaBase = OpenLibrary("Multimedia/multimedia.class", 50))) return FALSE;
	if (!(init_class(cb))) return FALSE;
	return TRUE;
}

///
/// FreeResources()

void FreeResources(struct ClassBase *cb)
{
	cb = cb;

	if (MultimediaBase) CloseLibrary(MultimediaBase);
	if (UtilityBase) CloseLibrary(UtilityBase);
	if (IntuitionBase) CloseLibrary(IntuitionBase);

	return;
}

///
/// LibInit()

struct ClassBase *lib_init(struct ClassBase *cb, APTR seglist, struct Library *sysbase)
{
	InitSemaphore(&cb->BaseLock);
	cb->Seglist = seglist;
	cb->Attributes = 0;
	sysbase = sysbase;
	return cb;
}

struct Library *LibInit(struct Library *unused, APTR seglist, struct Library *sysbase)
{
	unused = unused;
	SysBase = sysbase;

	return (NewCreateLibraryTags(
		LIBTAG_FUNCTIONINIT, (ULONG)JumpTable,
		LIBTAG_LIBRARYINIT,  (ULONG)lib_init,
		LIBTAG_MACHINE,      MACHINE_PPC,
		LIBTAG_BASESIZE,     sizeof(struct ClassBase),
		LIBTAG_SEGLIST,      (ULONG)seglist,
		LIBTAG_TYPE,         NT_LIBRARY,
		LIBTAG_NAME,         (ULONG)ROMTag.rt_Name,
		LIBTAG_IDSTRING,     (ULONG)ROMTag.rt_IdString,
		LIBTAG_FLAGS,        LIBF_CHANGED | LIBF_SUMUSED,
		LIBTAG_VERSION,      VERSION,
		LIBTAG_REVISION,     REVISION,
		LIBTAG_PUBLIC,       TRUE,
	TAG_END));
}

///
/// LibOpen()

struct Library *LibOpen(void)
{
	struct ClassBase *cb = (struct ClassBase*)REG_A6;
	struct Library *lib = (struct Library*)cb;

	ObtainSemaphore(&cb->BaseLock);

	if (!cb->InitFlag)
	{
		if (InitResources(cb)) cb->InitFlag = TRUE;
		else
		{
			FreeResources(cb);
			lib = NULL;
		}
	}

	if (lib)
	{
		cb->LibNode.lib_Flags &= ~LIBF_DELEXP;
		cb->LibNode.lib_OpenCnt++;
	}

	ReleaseSemaphore(&cb->BaseLock);
	return lib;
}

///
/// LibClose()

ULONG LibClose(void)
{
	struct ClassBase *cb = (struct ClassBase*)REG_A6;
	ULONG ret = 0;

	ObtainSemaphore(&cb->BaseLock);
	if (--cb->LibNode.lib_OpenCnt == 0)
	{
		if (cb->LibNode.lib_Flags & LIBF_DELEXP) ret = (ULONG)lib_expunge(cb);
	}
	ReleaseSemaphore(&cb->BaseLock);

	return ret;
}

///
/// LibExpunge()

APTR LibExpunge(void)
{
	struct ClassBase *cb = (struct ClassBase*)REG_A6;

	return(lib_expunge(cb));
}

APTR lib_expunge(struct ClassBase *cb)
{
	APTR seglist = NULL;

	ObtainSemaphore(&cb->BaseLock);

	if (cb->LibNode.lib_OpenCnt == 0)
	{
		if (!cb->LibClass || FreeClass(cb->LibClass))
		{
			cb->LibClass = NULL;
			Forbid();
			Remove((struct Node*)cb);
			Permit();
			FreeResources(cb);
			seglist = cb->Seglist;
			FreeMem((UBYTE*)cb - cb->LibNode.lib_NegSize, cb->LibNode.lib_NegSize + cb->LibNode.lib_PosSize);
			cb = NULL;
		}
		if (cb && cb->LibClass) AddClass(cb->LibClass);
	}
	else cb->LibNode.lib_Flags |= LIBF_DELEXP;

	if (cb) ReleaseSemaphore(&cb->BaseLock);
	return seglist;
}

///
/// LibReserved()

ULONG LibReserved(void)
{
	return 0;
}

///
/// GetClass()

Class *GetClass(VOID)
{
	struct ClassBase *cb = (struct ClassBase*)REG_A6;

	return cb->LibClass;
}

///
/// hex_value() - decode one hex digit

BOOL hex_value(UBYTE c, UWORD *value)
{
	if (c >= '0' && c <= '9')      *value = c - '0';
	else if (c >= 'a' && c <= 'f') *value = c - 'a' + 10;
	else if (c >= 'A' && c <= 'F') *value = c - 'A' + 10;
	else return FALSE;

	return TRUE;
}

///
/// ParseSelector()
///
/// Accepts "vendor:product" and a bare product id, both in hex.  Spaces around
/// the parts are ignored, everything else is refused so a typo selects the
/// first camera instead of nothing.

static UWORD parse_hex16(STRPTR s, STRPTR end)
{
	UWORD acc = 0;
	int digits = 0;

	while (s < end)
	{
		UWORD v;

		if (*s == ' ') { s++; continue; }
		if (!hex_value(*s, &v)) break;

		acc = (UWORD)((acc << 4) | v);
		s++;
		if (++digits > 4) return 0;
	}

	return digits ? acc : 0;
}

static STRPTR skip_spaces(STRPTR s)
{
	while (*s == ' ') s++;
	return s;
}

ULONG ParseSelector(STRPTR selector, UWORD *vendor, UWORD *product)
{
	STRPTR colon;

	*vendor = 0;
	*product = 0;

	if (!selector) return 0;

	selector = skip_spaces(selector);

	if (!*selector) return 0;

	if ((colon = strchr(selector, ':')))
	{
		*vendor  = parse_hex16(selector, colon);
		*product = parse_hex16(colon + 1, selector + strlen(selector));
	}
	else
	{
		*product = parse_hex16(selector, selector + strlen(selector));
	}

	return (*vendor || *product) ? 1 : 0;
}

///
/// New()
///
/// Everything but the buffers is decided here, the camera itself stays closed
/// until MMM_Setup() runs.

LONG New(Class *cl, Object *obj, struct opSet *msg)
{
	LONG newobj = 0;
	UWORD vendor = 0, product = 0;
	STRPTR name;

	if ((obj = (Object*)DoSuperMethodA(cl, obj, (Msg)msg)))
	{
		GET_DATA;

		DoMethod(obj, MMM_LockObject);

		/* output port */

		DoMethod(obj, MMM_AddPort, 0);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_Type, MDP_TYPE_OUTPUT);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_FormatsTable, (ULONG)OutputFormats);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_Format, MMF_STREAM);

		d->od_Format  = SNXF_RGB24;
		d->od_HeaderSent = FALSE;
		d->od_FrameReady = FALSE;
		d->od_FrameIndex = 0;

		/* a bare MMA_Sonix_ProductID wins over the name, so an application
		 * can address the camera without knowing the string syntax */

		vendor  = (UWORD)GetTagData(MMA_Sonix_VendorID,  0, msg->ops_AttrList);
		product = (UWORD)GetTagData(MMA_Sonix_ProductID, 0, msg->ops_AttrList);
		d->od_Format = (ULONG)GetTagData(MMA_Sonix_Format, SNXF_RGB24, msg->ops_AttrList);

		if (!vendor && !product && (name = (STRPTR)GetTagData(MMA_StreamName, 0, msg->ops_AttrList)))
		{
			ParseSelector(name, &vendor, &product);
		}

		d->od_Format = (d->od_Format == SNXF_GRAY8) ? SNXF_GRAY8 : SNXF_RGB24;

		d->od_Vendor  = vendor;
		d->od_Product = product;
		d->od_PixelLen = (d->od_Format == SNXF_GRAY8)
		              ? (ULONG)SONIX_FRAME_BYTES
		              : (ULONG)SONIX_FRAME_BYTES * 3;

		/* the Bayer frame, the frame buffer handed to the demuxer and the
		 * strings reported through OM_GET */

		d->od_Raw     = (UBYTE *)MediaAllocVec((ULONG)SONIX_FRAME_BYTES);
		d->od_Output  = (UBYTE *)MediaAllocVec((ULONG)sizeof(struct SnxStreamHeader)
		                                     + (ULONG)sizeof(struct SnxFrameHeader)
		                                     + d->od_PixelLen);
		d->od_ProductName = (STRPTR)MediaAllocVec(64);

		if (!d->od_Raw || !d->od_Output || !d->od_ProductName)
		{
			DoMethod(obj, MMM_UnlockObject);
			CoerceMethod(cl, obj, OM_DISPOSE);
			return 0;
		}

		/* the product name is reported before the camera has been claimed,
		 * so it has to be a valid empty string right from the start */

		d->od_ProductName[0] = 0;

		d->od_RawLen     = (ULONG)SONIX_FRAME_BYTES;
		d->od_OutputLen  = (ULONG)sizeof(struct SnxStreamHeader)
		                 + (ULONG)sizeof(struct SnxFrameHeader)
		                 + d->od_PixelLen;
		d->od_OutputPos  = 0;
		d->od_DataFormat = d->od_Format == SNXF_GRAY8 ? "Sonix gray8" : "Sonix RGB24";

		newobj = (LONG)obj;

		DoMethod(obj, MMM_UnlockObject);
	}

	if (!newobj) CoerceMethod(cl, obj, OM_DISPOSE);

	return newobj;
}

///
/// CloseCamera()

void CloseCamera(struct ObjData *d)
{
	if (d->od_Cam)
	{
		sonix_cam_close(d->od_Cam);
		d->od_Cam = NULL;
	}

	d->od_Opened     = FALSE;
	d->od_HeaderSent = FALSE;
	d->od_FrameReady = FALSE;
	d->od_OutputPos  = 0;
	d->od_FrameIndex = 0;
	d->od_StartTime  = 0;
}

///
/// Dispose()

LONG Dispose(Class *cl, Object *obj, Msg msg)
{
	GET_DATA;

	DoMethod(obj, MMM_LockObject);

	CloseCamera(d);

	if (d->od_Raw) MediaFreeVec(d->od_Raw);
	if (d->od_Output) MediaFreeVec(d->od_Output);
	if (d->od_ProductName) MediaFreeVec(d->od_ProductName);

	DoMethod(obj, MMM_UnlockObject);
	return DoSuperMethodA(cl, obj, msg);
}

///
/// OpenCamera()
///
/// Claims the first matching camera and fills in the output buffer with the
/// SnxStreamHeader.  A failure here is what MMM_Setup() reports.

BOOL OpenCamera(Class *cl, Object *obj)
{
	GET_DATA;
	LONG error = SONIX_OK;
	struct SnxStreamHeader *sh;
	ULONG frameTime;

	if (d->od_Cam) return TRUE;

	d->od_Cam = sonix_cam_open((UWORD)d->od_Vendor, (UWORD)d->od_Product,
	                           d->od_ProductName, 64, &error);

	if (!d->od_Cam)
	{
		switch (error)
		{
			case SONIX_ERR_NO_LIBRARY:  seterr(MMERR_RESOURCE_MISSING); break;
			case SONIX_ERR_NO_DEVICE:   seterr(MMERR_NO_STREAM);         break;
			case SONIX_ERR_CONTROLLER:  seterr(MMERR_NO_STREAM_CLASS); break;
			case SONIX_ERR_SENSOR:      seterr(MMERR_NO_STREAM_CLASS); break;
			default:                    seterr(MMERR_IO_ERROR);          break;
		}

		return FALSE;
	}

	d->od_Opened = TRUE;

	/* frame timestamps are counted from the moment the camera was claimed,
	 * not from the first pull, so a stream which is peeked at first still
	 * starts its clock at zero */

	d->od_StartTime = sonix_cam_now_us();

	if (!BuildHeader(d))
	{
		CloseCamera(d);
		seterr(MMERR_OUT_OF_MEMORY);
		return FALSE;
	}

	/* the first two frames have no meaningful inter frame time yet */

	frameTime = sonix_cam_frame_time(d->od_Cam);
	sh = (struct SnxStreamHeader *)d->od_Output;

	sh->sh_FrameTime = frameTime;

	return TRUE;
}

///
/// BuildHeader()
///
/// Lays down the SnxStreamHeader at the front of the output buffer.  MorphOS
/// is big endian and the structure is big endian, so the bytes can go in as
/// they are.

BOOL BuildHeader(struct ObjData *d)
{
	struct SnxStreamHeader *sh = (struct SnxStreamHeader *)d->od_Output;
	UWORD vendor = 0, product = 0;

	/* the ids the device answers with, not the ones that were asked for: a
	 * selector of zero means "the first camera there is" and the header is
	 * the only place where camera and pipeline meet, so it carries the truth */

	sonix_cam_ids(d->od_Cam, &vendor, &product);

	sh->sh_Magic[0] = 'S';
	sh->sh_Magic[1] = 'N';
	sh->sh_Magic[2] = 'X';
	sh->sh_Magic[3] = '1';
	sh->sh_HeaderSize    = sizeof(struct SnxStreamHeader);
	sh->sh_Version       = 1;
	sh->sh_Width         = SONIX_WIDTH;
	sh->sh_Height        = SONIX_HEIGHT;
	sh->sh_Format        = (UBYTE)d->od_Format;
	sh->sh_BytesPerPixel = d->od_Format == SNXF_GRAY8 ? 1 : 3;
	sh->sh_VendorID      = vendor;
	sh->sh_ProductID     = product;
	sh->sh_Sensor        = sonix_cam_sensor(d->od_Cam);
	sh->sh_Flags         = SNXSH_LIVE;
	sh->sh_FrameCount    = SNX_FRAMECOUNT_UNKNOWN;
	sh->sh_FrameTime     = 0;
	sh->sh_Reserved      = 0;

	d->od_OutputPos  = 0;
	d->od_HeaderSent = FALSE;
	d->od_FrameReady = FALSE;

	return TRUE;
}

///
/// CaptureFrame()
///
/// Reads one frame from the camera and lays it out behind the stream header:
/// SnxFrameHeader followed by the pixels.  The stream header stays where it
/// is, the demuxer has already taken it by the time a frame is pulled.

BOOL CaptureFrame(Object *obj, struct ObjData *d)
{
	struct SnxStreamHeader *sh = (struct SnxStreamHeader *)d->od_Output;
	struct SnxFrameHeader  *fh;
	UBYTE *pixels;
	ULONG frametime;
	LONG err;

	if (!d->od_Cam) return FALSE;

	err = sonix_cam_capture(d->od_Cam, d->od_Raw);

	if (err != SONIX_OK)
	{
		seterr(MMERR_IO_ERROR);
		return FALSE;
	}

	frametime = sonix_cam_frame_time(d->od_Cam);
	if (frametime) sh->sh_FrameTime = frametime;

	fh = (struct SnxFrameHeader *)(d->od_Output + sizeof(struct SnxStreamHeader));
	fh->fh_FrameSize = (ULONG)sizeof(struct SnxFrameHeader) + d->od_PixelLen;
	fh->fh_Index     = d->od_FrameIndex++;
	fh->fh_Timestamp = d->od_StartTime ? sonix_cam_now_us() - d->od_StartTime : 0;
	fh->fh_Flags     = SNXFF_KEYFRAME;   /* every frame stands on its own */

	pixels = (UBYTE *)fh + sizeof(struct SnxFrameHeader);

	if (d->od_Format == SNXF_GRAY8)
	{
		/* the Bayer pattern is already one byte per pixel, no interpolation
		 * needed, the decoder sees the same colour twice for every pixel */

		memcpy(pixels, d->od_Raw, SONIX_FRAME_BYTES);
	}
	else
	{
		bayer2rgb24(pixels, d->od_Raw, SONIX_WIDTH, SONIX_HEIGHT);
	}

	return TRUE;
}

///
/// PrepareFrame()
///
/// Makes sure the output buffer holds a frame which has not been served yet,
/// claiming the camera on the way if that has not happened yet.  This is the
/// only place a pull or a peek has to wait for USB time.
///
/// The stream header is served in front of the first frame only, every frame
/// after that starts right behind it.  Moving the cursor to the end of the
/// frame is what makes a pull pick up the next one, so the two are reset
/// together and a partially served frame is never taken for a new one.

BOOL PrepareFrame(Class *cl, Object *obj, struct ObjData *d)
{
	if (d->od_FrameReady) return TRUE;

	if (!d->od_Opened && !OpenCamera(cl, obj)) return FALSE;

	if (!CaptureFrame(obj, d)) return FALSE;

	d->od_OutputPos  = d->od_HeaderSent ? (ULONG)sizeof(struct SnxStreamHeader) : 0;
	d->od_FrameReady = TRUE;

	return TRUE;
}

///
/// SetFormat()
///
/// Switches between SNXF_GRAY8 and SNXF_RGB24.  The frame buffer is the only
/// thing which depends on it, so it is reallocated and the stream starts
/// over; the camera itself does not care and is left alone.

BOOL SetFormat(struct ObjData *d, ULONG format)
{
	ULONG pixelLen, len;
	UBYTE *output;

	if (format != SNXF_GRAY8 && format != SNXF_RGB24)
	{
		seterr(MMERR_WRONG_ARGUMENTS);
		return FALSE;
	}

	if (format == d->od_Format) return TRUE;

	pixelLen = (format == SNXF_GRAY8)
	         ? (ULONG)SONIX_FRAME_BYTES
	         : (ULONG)SONIX_FRAME_BYTES * 3;

	len    = (ULONG)sizeof(struct SnxStreamHeader)
	       + (ULONG)sizeof(struct SnxFrameHeader)
	       + pixelLen;

	if (!(output = (UBYTE *)MediaAllocVec(len)))
	{
		seterr(MMERR_OUT_OF_MEMORY);
		return FALSE;
	}

	MediaFreeVec(d->od_Output);

	d->od_Output     = output;
	d->od_OutputLen  = len;
	d->od_PixelLen   = pixelLen;
	d->od_Format     = format;
	d->od_DataFormat = format == SNXF_GRAY8 ? "Sonix gray8" : "Sonix RGB24";

	/* nothing in the buffer belongs to the old format any more, and the
	 * stream header has to be built again for the new one */

	d->od_OutputPos  = 0;
	d->od_HeaderSent = FALSE;
	d->od_FrameReady = FALSE;

	if (d->od_Opened) BuildHeader(d);

	return TRUE;
}

///
/// Get()

LONG Get(Class *cl, Object *obj, struct opGet *msg)
{
	GET_DATA;

	switch (msg->opg_AttrID)
	{
		/* the name of a live source is the camera it was pointed at, the data
		 * format is what the frame data inside the stream looks like */

		case MMA_StreamName:
		case MMA_ObjectName:
			*msg->opg_Storage = (LONG)d->od_ProductName;
			return TRUE;

		case MMA_DataFormat:
			*msg->opg_Storage = (LONG)d->od_DataFormat;
			return TRUE;

		case MMA_MediaType:
			*msg->opg_Storage = MMT_VIDEO;
			return TRUE;

		/* the ids asked for, a zero here means "any camera"; the ids of the
		 * camera which was actually claimed travel in the stream header and
		 * come back out of the demuxer */

		case MMA_Sonix_VendorID:
			*msg->opg_Storage = (LONG)d->od_Vendor;
			return TRUE;

		case MMA_Sonix_ProductID:
			*msg->opg_Storage = (LONG)d->od_Product;
			return TRUE;

		case MMA_Sonix_Format:
			*msg->opg_Storage = (LONG)d->od_Format;
			return TRUE;

		case MMA_Sonix_ProductName:
			*msg->opg_Storage = (LONG)d->od_ProductName;
			return TRUE;

		case MMA_Sonix_Sensor:
			*msg->opg_Storage = (LONG)(d->od_Cam ? sonix_cam_sensor(d->od_Cam) : SNXS_NONE);
			return TRUE;

		case MMA_Sonix_SensorName:
			*msg->opg_Storage = (LONG)(d->od_Cam ? sonix_cam_sensor_name(d->od_Cam) : "unknown");
			return TRUE;

		case MMA_Sonix_Red:
			*msg->opg_Storage = d->od_Cam ? sonix_cam_get_red(d->od_Cam) : 0;
			return TRUE;

		case MMA_Sonix_Green:
			*msg->opg_Storage = d->od_Cam ? sonix_cam_get_green(d->od_Cam) : 0;
			return TRUE;

		case MMA_Sonix_Blue:
			*msg->opg_Storage = d->od_Cam ? sonix_cam_get_blue(d->od_Cam) : 0;
			return TRUE;

		case MMA_Sonix_Gain:
			*msg->opg_Storage = d->od_Cam ? sonix_cam_get_gain(d->od_Cam) : 0;
			return TRUE;

		case MMA_Sonix_Brightness:
			*msg->opg_Storage = d->od_Cam ? sonix_cam_get_brightness(d->od_Cam) : 0;
			return TRUE;

		case MMA_Sonix_Contrast:
			*msg->opg_Storage = d->od_Cam ? sonix_cam_get_contrast(d->od_Cam) : 0;
			return TRUE;

		/* a live source has no length and cannot be seeked or rewound */

		case MMA_StreamLength:
			*(UQUAD*)msg->opg_Storage = 0;
			return TRUE;

		case MMA_StreamSeekable:
			*msg->opg_Storage = FALSE;
			return TRUE;

		case MMA_StreamPosBytes:
			*(UQUAD*)msg->opg_Storage = 0;
			return TRUE;

		case MMA_StreamPosFrames:
			*(UQUAD*)msg->opg_Storage = d->od_FrameIndex;
			return TRUE;

		case MMA_StreamPosTime:
			*(UQUAD*)msg->opg_Storage = 0;
			return TRUE;

		default:
			return DoSuperMethodA(cl, obj, (Msg)msg);
	}
}

///
/// Set()
///
/// The colour controls are applied to the camera and need it to be open, the
/// device selector and the pixel format are only of use before that and are
/// refused afterwards.  Everything else is refused as well rather than
/// silently dropped, so a mistyped tag is noticed.

LONG Set(Class *cl, Object *obj, struct opSet *msg)
{
	GET_DATA;
	struct TagItem *tag = msg->ops_AttrList;
	BOOL done = FALSE;

	while (tag && tag->ti_Tag != TAG_END)
	{
		LONG value = (LONG)tag->ti_Data;

		switch (tag->ti_Tag)
		{
			/* the camera is claimed with these, so they have to be right
			 * before the first frame is on its way */

			case MMA_Sonix_VendorID:
			case MMA_Sonix_ProductID:
				if (d->od_Cam)
				{
					seterr(MMERR_WRONG_ARGUMENTS);
					return FALSE;
				}

				if (tag->ti_Tag == MMA_Sonix_VendorID) d->od_Vendor  = (ULONG)(UWORD)value;
				else                                       d->od_Product = (ULONG)(UWORD)value;

				done = TRUE;
			break;

			case MMA_Sonix_Format:
				if (!SetFormat(d, (ULONG)value)) return FALSE;
				done = TRUE;
			break;

			case MMA_Sonix_Red:
				if (d->od_Cam) { sonix_cam_set_red(d->od_Cam, value); done = TRUE; }
			break;

			case MMA_Sonix_Green:
				if (d->od_Cam) { sonix_cam_set_green(d->od_Cam, value); done = TRUE; }
			break;

			case MMA_Sonix_Blue:
				if (d->od_Cam) { sonix_cam_set_blue(d->od_Cam, value); done = TRUE; }
			break;

			case MMA_Sonix_Gain:
				if (d->od_Cam) { sonix_cam_set_gain(d->od_Cam, value); done = TRUE; }
			break;

			case MMA_Sonix_Brightness:
				if (d->od_Cam) { sonix_cam_set_brightness(d->od_Cam, value); done = TRUE; }
			break;

			case MMA_Sonix_Contrast:
				if (d->od_Cam) { sonix_cam_set_contrast(d->od_Cam, value); done = TRUE; }
			break;

			default: break;
		}

		tag += 2;
	}

	return done;
}

///
/// Pull()
///
/// Serves the prepared output buffer.  The stream header goes out first, then
/// each frame is served on the calls that follow.  When the buffer runs dry
/// the next frame is captured, which is where MMM_Pull blocks for a frame
/// interval of USB time.

LONG Pull(Class *cl, Object *obj, struct mmopData *msg)
{
	GET_DATA;
	ULONG bytes_pulled = 0;

	if (msg->Port != 0 || !msg->Buffer || !msg->Length)
	{
		seterr(MMERR_WRONG_ARGUMENTS);
		return 0;
	}

	DoMethod(obj, MMM_LockObject);
	seterr(0);

	if (PrepareFrame(cl, obj, d))
	{
		ULONG avail = d->od_OutputLen - d->od_OutputPos;
		ULONG len   = (ULONG)msg->Length < avail ? (ULONG)msg->Length : avail;

		memcpy(msg->Buffer, d->od_Output + d->od_OutputPos, len);

		d->od_OutputPos += len;
		bytes_pulled     = len;

		/* a buffer which has been served to the end holds no frame any
		 * more, so the next call waits for the camera again and starts
		 * behind the stream header */

		if (d->od_OutputPos >= d->od_OutputLen)
		{
			d->od_HeaderSent = TRUE;
			d->od_FrameReady = FALSE;
		}
	}

	DoMethod(obj, MMM_UnlockObject);

	return bytes_pulled;
}

///
/// Peek()
///
/// Hands out the bytes the next pull would hand out without moving the read
/// position.  A file based stream can pull them and seek back, a live camera
/// cannot, so the class has to answer this itself: the recognition code of
/// sonix.demuxer peeks 32 bytes of the source to find the stream header, and
/// with the inherited method that header would be gone before the demuxer
/// ever saw it.

LONG Peek(Class *cl, Object *obj, struct mmopData *msg)
{
	GET_DATA;
	ULONG bytes_peeked = 0;

	if (msg->Port != 0 || !msg->Buffer || !msg->Length)
	{
		seterr(MMERR_WRONG_ARGUMENTS);
		return 0;
	}

	DoMethod(obj, MMM_LockObject);
	seterr(0);

	/* the very same state a pull would work on, prepared the same way */

	if (PrepareFrame(cl, obj, d))
	{
		ULONG avail = d->od_OutputLen - d->od_OutputPos;
		ULONG len   = (ULONG)msg->Length < avail ? (ULONG)msg->Length : avail;

		memcpy(msg->Buffer, d->od_Output + d->od_OutputPos, len);

		bytes_peeked = len;
	}

	DoMethod(obj, MMM_UnlockObject);

	return bytes_peeked;
}

///
/// Restore()
///
/// There is nothing to put back, the peek of this class leaves the read
/// position alone.  Answering it is still worth it: the recognition code of
/// sonix.demuxer restores whatever it peeked, and the inherited method would
/// answer that with MMERR_NOT_SEEKABLE and leave the error behind.

LONG Restore(Class *cl, Object *obj, Msg msg)
{
	msg = msg;
	return TRUE;
}

///
/// Setup()
///
/// Opening the camera is the only work, there is no port 0 input to wait for.

LONG Setup(Class *cl, Object *obj, struct mmopPort *msg)
{
	GET_DATA;
	LONG rv = FALSE;

	if (msg->Port != 0)
	{
		seterr(MMERR_WRONG_ARGUMENTS);
		return FALSE;
	}

	DoMethod(obj, MMM_LockObject);

	if (!d->od_Cam) rv = OpenCamera(cl, obj);
	else           rv = TRUE;

	DoMethod(obj, MMM_UnlockObject);

	return rv;
}

///
/// Seek()
///
/// There is nothing to seek in, a live camera is not a file.

LONG Seek(Class *cl, Object *obj, struct mmopSeek *msg)
{
	msg = msg;
	seterr(MMERR_NOT_SEEKABLE);
	return FALSE;
}

///
/// GetPort()
///
/// The custom tags are answered on the port as well, so an application can
/// read the sensor name without going through OM_GET.

LONG GetPort(Class *cl, Object *obj, struct mmopGetPort *msg)
{
	switch (msg->Attribute)
	{
		case MMA_Sonix_VendorID:
		case MMA_Sonix_ProductID:
		case MMA_Sonix_Format:
		case MMA_Sonix_ProductName:
		case MMA_Sonix_Sensor:
		case MMA_Sonix_SensorName:
		case MMA_Sonix_Red:
		case MMA_Sonix_Green:
		case MMA_Sonix_Blue:
		case MMA_Sonix_Gain:
		case MMA_Sonix_Brightness:
		case MMA_Sonix_Contrast:
		case MMA_StreamLength:
		case MMA_StreamSeekable:
		case MMA_StreamPosBytes:
		case MMA_StreamPosFrames:
		case MMA_StreamPosTime:
			return DoMethod(obj, OM_GET, msg->Attribute, (ULONG)msg->Storage);
	}
	return (DoSuperMethodA(cl, obj, (Msg)msg));
}

///
/// SetPort()
///
/// Sends the class specific tags to OM_SET, where they are applied to the
/// camera or to the device selector, depending on what they are.

LONG SetPort(Class *cl, Object *obj, struct mmopSetPort *msg)
{
	switch (msg->Attribute)
	{
		case MMA_Sonix_VendorID:
		case MMA_Sonix_ProductID:
		case MMA_Sonix_Format:
		case MMA_Sonix_Red:
		case MMA_Sonix_Green:
		case MMA_Sonix_Blue:
		case MMA_Sonix_Gain:
		case MMA_Sonix_Brightness:
		case MMA_Sonix_Contrast:
			return DoMethod(obj, OM_SET, msg->Attribute, msg->Value);
	}
	return (DoSuperMethodA(cl, obj, (Msg)msg));
}

///
/// dispatcher

LONG ClassDispatcher(void)
{
	Class *cl = (Class*)REG_A0;
	Object *obj = (Object*)REG_A2;
	Msg msg = (Msg)REG_A1;

	switch (msg->MethodID)
	{
		case OM_NEW:       return New(cl, obj, (struct opSet*)msg);
		case OM_DISPOSE:   return Dispose(cl, obj, msg);
		case OM_GET:       return Get(cl, obj, (struct opGet*)msg);
		case OM_SET:       return Set(cl, obj, (struct opSet*)msg);
		case MMM_Pull:     return Pull(cl, obj, (struct mmopData*)msg);
		case MMM_Peek:     return Peek(cl, obj, (struct mmopData*)msg);
		case MMM_Restore:  return Restore(cl, obj, msg);
		case MMM_Setup:    return Setup(cl, obj, (struct mmopPort*)msg);
		case MMM_Seek:     return Seek(cl, obj, (struct mmopSeek*)msg);
		case MMM_GetPort:  return GetPort(cl, obj, (struct mmopGetPort*)msg);
		case MMM_SetPort:  return SetPort(cl, obj, (struct mmopSetPort*)msg);
		default:           return DoSuperMethodA(cl, obj, msg);
	}
}

/// end
