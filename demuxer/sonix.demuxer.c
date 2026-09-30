/// autodoc

/****** sonix.demuxer/background ********************************************
*
* DESCRIPTION
*   Demuxer for the framed stream produced by sonix.stream.  It reads the
*   SnxStreamHeader once, republishes the geometry and the format as video
*   attributes and then hands out the pixel data of one frame per call on
*   port 1, without the framing.
*
*   Two-port model: port 0 input MMF_STREAM, port 1 output MMF_VIDEO_RGB24 or
*   MMF_VIDEO_GRAY8, depending on what the stream header announces.
*
* NEW ATTRIBUTES
*   MMA_Video_Width        (V1)  [..G], ULONG
*   MMA_Video_Height       (V1)  [..G], ULONG
*   MMA_Video_BitsPerPixel (V1)  [..G], ULONG
*   MMA_Video_FrameCount   (V1)  [..G], UQUAD
*   MMA_Video_FpsNumerator (V1)  [..G], ULONG
*   MMA_Video_FpsDenominator (V1) [..G], ULONG
*   MMA_DataFormat         (V1)  [..G], STRPTR
*   MMA_MediaType          (V1)  [..G], ULONG
*   MMA_Sonix_VendorID     (V1)  [..G], UWORD
*   MMA_Sonix_ProductID    (V1)  [..G], UWORD
*   MMA_Sonix_Sensor       (V1)  [..G], ULONG
*   MMA_Sonix_FrameIndex   (V1)  [..G], ULONG
*   MMA_Sonix_FrameTime    (V1)  [..G], ULONG
*
* NEW METHODS
*   MMM_Pull(port, buffer, length) (V1)
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

#include "sonix.demuxer.h"
#include "../sonixwire.h"

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
 * Instance data.  A frame is served straight out of od_Frame, which is filled
 * from the input stream in whatever chunks MMM_Pull on port 0 hands over, so
 * the demuxer works the same whether the source is a camera or a file.
 */

struct ObjData
{
	struct SnxStreamHeader dd_Header;
	BOOL                  dd_HaveHeader;
	UBYTE                *dd_Frame;      /* one complete frame of pixel data  */
	ULONG                 dd_FrameLen;   /* valid bytes in dd_Frame            */
	ULONG                 dd_FrameSize;  /* size the stream header promises   */
	ULONG                 dd_FramePos;   /* how much of it has been served     */
	ULONG                 dd_FrameIndex;
	ULONG                 dd_Format;
	ULONG                 dd_Vendor;
	ULONG                 dd_Product;
	ULONG                 dd_Sensor;
	ULONG                 dd_FrameTime;   /* measured, from frame timestamps */
	ULONG                 dd_LastTime;    /* fh_Timestamp of the last frame   */
	BOOL                  dd_HaveTime;
	BOOL                  dd_Failed;      /* the stream desynchronised, stop  */
	STRPTR                dd_DataFormat;
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
LONG Setup(Class *cl, Object *obj, struct mmopPort *msg);
LONG PullInput(Class *cl, Object *obj, UBYTE *buffer, ULONG length);
BOOL ReadHeader(Class *cl, Object *obj);
BOOL ReadFrame(Class *cl, Object *obj);
ULONG FormatOf(ULONG format);

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
	{QUERYINFOATTR_DESCRIPTION, (ULONG)"Sonix webcam stream demuxer"},
	{QUERYINFOATTR_COPYRIGHT, (ULONG)"(c) 2026"},
	{QUERYINFOATTR_AUTHOR, (ULONG)"Reggae contributors"},
	{QUERYINFOATTR_DATE, (ULONG)DATE},
	{QUERYINFOATTR_VERSION, VERSION},
	{QUERYINFOATTR_REVISION, REVISION},
	{QUERYINFOATTR_SUBTYPE, QUERYSUBTYPE_LIBRARY},
	{QUERYINFOATTR_CLASS, QUERYCLASS_MULTIMEDIA},
	{QUERYINFOATTR_SUBCLASS, QUERYSUBCLASS_MULTIMEDIA_DEMUXER},
	{MMA_MediaType, MMT_VIDEO},
	{MMA_SupportedFormats, (ULONG)"M"},
	{TAG_END, 0}
};

static const ULONG InputFormats[]  = { MMF_STREAM, 0 };
static const ULONG OutputFormats[] = { MMF_VIDEO_RGB24, MMF_VIDEO_GRAY8, 0 };

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
/// FormatOf()
///
/// Maps an SNXF_* pixel format from the stream header onto a Reggae format.

ULONG FormatOf(ULONG format)
{
	return format == SNXF_GRAY8 ? MMF_VIDEO_GRAY8 : MMF_VIDEO_RGB24;
}

///
/// PullInput()
///
/// One pull on the input port, going straight to the superclass.
///
/// This has to bypass our own MMM_Pull.  The callers hold the object lock, and
/// a DoMethod() on our own object would come back through this class and try to
/// take that same lock a second time, which a plain semaphore does not allow:
/// the object would deadlock on its own first frame.  The superclass is what
/// actually owns port 0, so asking it directly is both correct and free of the
/// re-entry.  It also keeps the passthrough in Pull() out of the way of our
/// own reads.

LONG PullInput(Class *cl, Object *obj, UBYTE *buffer, ULONG length)
{
	struct mmopData pull;

	pull.mdm_MethodID = MMM_Pull;
	pull.Port         = 0;
	pull.Buffer       = buffer;
	pull.Length       = (LONG)length;

	return DoSuperMethodA(cl, obj, (Msg)&pull);
}

///
/// ReadHeader()
///
/// Pulls the SnxStreamHeader off port 0 and allocates the frame buffer.  A
/// stream which does not start with "SNX1" is refused here, so a wrong class
/// in a pipeline shows up as MMERR_WRONG_DATA rather than as noise.
///
/// A source which has not started yet hands out nothing at all, and that is
/// not a failure: the pull is refused and the next one reads the header.  As
/// soon as one byte has been taken off the port, though, the stream is out of
/// step and no later pull can make it line up again, so that is a failure for
/// good in the same way a bad frame size is.

BOOL ReadHeader(Class *cl, Object *obj)
{
	GET_DATA;
	ULONG got = 0;
	ULONG pixels;
	UQUAD framebytes;

	while (got < sizeof(struct SnxStreamHeader))
	{
		LONG rv = PullInput(cl, obj, (UBYTE *)&d->dd_Header + got,
		                    (ULONG)(sizeof(struct SnxStreamHeader) - got));

		if (rv <= 0)
		{
			/* nothing at all came back, so the error which stopped the
			 * pull is the one worth reporting: a source which is not
			 * running yet says so itself, and overwriting that with
			 * MMERR_WRONG_DATA would blame the data for it.  Bytes have
			 * been taken off the port, on the other hand, so the stream
			 * can never line up again */

			if (got)
			{
				seterr(MMERR_WRONG_DATA);
				d->dd_Failed = TRUE;
			}

			return FALSE;
		}

		got += (ULONG)rv;
	}

	/* the whole header is in hand and none of it is one, so there is nothing
	 * to wait for either: whatever is on the other end of port 0 is not a
	 * Sonix stream, and reading on would only be guessing where the frames
	 * of a stream we never found could begin */

#define BAD_STREAM_HEADER() \
	do { seterr(MMERR_WRONG_DATA); d->dd_Failed = TRUE; return FALSE; } while (0)

	if (d->dd_Header.sh_Magic[0] != 'S' ||
	    d->dd_Header.sh_Magic[1] != 'N' ||
	    d->dd_Header.sh_Magic[2] != 'X' ||
	    d->dd_Header.sh_Magic[3] != '1')
	{
		BAD_STREAM_HEADER();
	}

	if (d->dd_Header.sh_HeaderSize != sizeof(struct SnxStreamHeader) ||
	    d->dd_Header.sh_Width == 0 || d->dd_Header.sh_Height == 0)
	{
		BAD_STREAM_HEADER();
	}

	if (d->dd_Header.sh_Format != SNXF_GRAY8 &&
	    d->dd_Header.sh_Format != SNXF_RGB24)
	{
		BAD_STREAM_HEADER();
	}

	if (d->dd_Header.sh_BytesPerPixel !=
	    (d->dd_Header.sh_Format == SNXF_GRAY8 ? 1 : 3))
	{
		BAD_STREAM_HEADER();
	}

	/* the macro takes the product in 64 bits, a 32 bit multiply of
	 * 65535 * 65535 * 3 wraps to a small number and would sail past the
	 * sanity check straight into a tiny allocation */

	framebytes = SNX_FRAME_BYTES(&d->dd_Header);

	if (framebytes == 0 || framebytes > 16 * 1024 * 1024)
	{
		BAD_STREAM_HEADER();
	}

	pixels = (ULONG)framebytes;

	if (d->dd_Frame) MediaFreeVec(d->dd_Frame);

	d->dd_Frame = (UBYTE *)MediaAllocVec(pixels);
	if (!d->dd_Frame)
	{
		/* no memory is a different thing to be out of: the header was
		 * read and the stream is in step, so a later pull may still get
		 * the frame out of it */

		seterr(MMERR_OUT_OF_MEMORY);
		return FALSE;
	}

	d->dd_Format     = d->dd_Header.sh_Format;
	d->dd_FrameSize  = pixels;
	d->dd_FrameLen   = 0;
	d->dd_FramePos   = 0;
	d->dd_FrameIndex = 0;
	d->dd_Vendor     = d->dd_Header.sh_VendorID;
	d->dd_Product    = d->dd_Header.sh_ProductID;
	d->dd_Sensor     = d->dd_Header.sh_Sensor;
	d->dd_DataFormat = d->dd_Format == SNXF_GRAY8 ? "Sonix gray8" : "Sonix RGB24";

	/* the nominal frame time of the header is only a hint and a source which
	 * has just been opened has none, so the running average is measured from
	 * the frame timestamps instead, starting from zero */

	d->dd_FrameTime  = 0;
	d->dd_LastTime   = 0;
	d->dd_HaveTime   = FALSE;

	/* the format of port 1 follows the stream header, and the header can be
	 * taken either by Setup or by a pull on port 1 which gets there first, so
	 * it is published here rather than by one of the two callers */

	DoMethod(obj, MMM_SetPort, 1, MMA_Port_Format, FormatOf(d->dd_Format));

	d->dd_HaveHeader = TRUE;

	return TRUE;
}

#undef BAD_STREAM_HEADER

///
/// ReadFrame()
///
/// Reads one SnxFrameHeader and the pixel data behind it into dd_Frame.  There
/// is no way back from a short read or from a frame size which does not match
/// the stream header, the framing cannot be resynchronised, so an error here
/// puts the object into a failed state: every later pull refuses it with the
/// error of the first failure instead of reading whatever comes next.

BOOL ReadFrame(Class *cl, Object *obj)
{
	GET_DATA;
	struct SnxFrameHeader fh;
	ULONG got = 0;

	if (!d->dd_HaveHeader)
	{
		seterr(MMERR_WRONG_DATA);
		d->dd_Failed = TRUE;
		return FALSE;
	}

	while (got < sizeof(struct SnxFrameHeader))
	{
		LONG rv = PullInput(cl, obj, (UBYTE *)&fh + got,
		                    (ULONG)(sizeof(struct SnxFrameHeader) - got));

		if (rv <= 0)
		{
			seterr(MMERR_IO_ERROR);
			d->dd_Failed = TRUE;
			return FALSE;
		}

		got += (ULONG)rv;
	}

	if (fh.fh_FrameSize != sizeof(struct SnxFrameHeader) + d->dd_FrameSize)
	{
		seterr(MMERR_WRONG_DATA);
		d->dd_Failed = TRUE;
		return FALSE;
	}

	got = 0;
	while (got < d->dd_FrameSize)
	{
		LONG rv = PullInput(cl, obj, d->dd_Frame + got, d->dd_FrameSize - got);

		if (rv <= 0)
		{
			seterr(MMERR_IO_ERROR);
			d->dd_Failed = TRUE;
			return FALSE;
		}

		got += (ULONG)rv;
	}

	d->dd_FrameLen   = got;
	d->dd_FramePos   = 0;
	d->dd_FrameIndex = fh.fh_Index;

	/* the source stamps every frame with the microseconds since it was
	 * opened, so the gap between two of them is the real inter frame time and
	 * that is what the fps attributes have to be built from */

	if (d->dd_HaveTime)
	{
		ULONG delta = fh.fh_Timestamp - d->dd_LastTime;

		/* a clock which wrapped or a source which skipped ahead is not a
		 * frame time, ignore it and keep the running average */

		if (delta > 0 && delta < 2000000)
		{
			d->dd_FrameTime = d->dd_FrameTime
			                ? (d->dd_FrameTime * 3 + delta) / 4
			                : delta;
		}
	}

	d->dd_LastTime = fh.fh_Timestamp;
	d->dd_HaveTime = TRUE;

	return TRUE;
}

///
/// New()

LONG New(Class *cl, Object *obj, struct opSet *msg)
{
	LONG newobj = 0;

	if ((obj = (Object*)DoSuperMethodA(cl, obj, (Msg)msg)))
	{
		GET_DATA;

		DoMethod(obj, MMM_LockObject);

		/* input port */

		DoMethod(obj, MMM_AddPort, 0);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_Type, MDP_TYPE_INPUT);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_FormatsTable, (ULONG)InputFormats);
		DoMethod(obj, MMM_SetPort, 0, MMA_Port_Format, MMF_STREAM);

		/* output port, the exact format follows the stream header */

		DoMethod(obj, MMM_AddPort, 1);
		DoMethod(obj, MMM_SetPort, 1, MMA_Port_Type, MDP_TYPE_OUTPUT);
		DoMethod(obj, MMM_SetPort, 1, MMA_Port_FormatsTable, (ULONG)OutputFormats);
		DoMethod(obj, MMM_SetPort, 1, MMA_Port_Format, MMF_VIDEO_RGB24);

		d->dd_Frame       = NULL;
		d->dd_HaveHeader  = FALSE;
		d->dd_Failed      = FALSE;
		d->dd_FrameTime   = 0;
		d->dd_LastTime    = 0;
		d->dd_HaveTime    = FALSE;
		d->dd_DataFormat  = "Sonix RGB24";

		newobj = (LONG)obj;

		DoMethod(obj, MMM_UnlockObject);
	}

	if (!newobj) CoerceMethod(cl, obj, OM_DISPOSE);

	return newobj;
}

///
/// Dispose()

LONG Dispose(Class *cl, Object *obj, Msg msg)
{
	GET_DATA;

	DoMethod(obj, MMM_LockObject);

	if (d->dd_Frame) MediaFreeVec(d->dd_Frame);

	DoMethod(obj, MMM_UnlockObject);
	return DoSuperMethodA(cl, obj, msg);
}

///
/// Get()

LONG Get(Class *cl, Object *obj, struct opGet *msg)
{
	GET_DATA;

	switch (msg->opg_AttrID)
	{
		case MMA_Video_Width:
			*msg->opg_Storage = d->dd_HaveHeader ? d->dd_Header.sh_Width : 0;
			return TRUE;

		case MMA_Video_Height:
			*msg->opg_Storage = d->dd_HaveHeader ? d->dd_Header.sh_Height : 0;
			return TRUE;

		case MMA_Video_BitsPerPixel:
			*msg->opg_Storage = d->dd_HaveHeader
			                  ? d->dd_Header.sh_BytesPerPixel * 8 : 0;
			return TRUE;

		case MMA_Video_FrameCount:
			/* a live source has no count */
			*(UQUAD*)msg->opg_Storage = 0;
			return TRUE;

		/* the rate is measured in microseconds per frame, and Reggae wants a
		 * plain fraction, so it is a second over the inter frame time.  With
		 * 1 / frameTime as it used to be, a 30 fps camera came out as 1/33333
		 * fps, which is off by a factor of a million.  Nothing is known until
		 * two frames have been read, that is reported as 0 fps. */

		case MMA_Video_FpsNumerator:
			*msg->opg_Storage = d->dd_FrameTime ? 1000000 : 0;
			return TRUE;

		case MMA_Video_FpsDenominator:
			*msg->opg_Storage = d->dd_FrameTime ? d->dd_FrameTime : 1;
			return TRUE;

		case MMA_DataFormat:
			*msg->opg_Storage = (LONG)d->dd_DataFormat;
			return TRUE;

		case MMA_MediaType:
			*msg->opg_Storage = MMT_VIDEO;
			return TRUE;

		case MMA_Sonix_VendorID:
			*msg->opg_Storage = (LONG)d->dd_Vendor;
			return TRUE;

		case MMA_Sonix_ProductID:
			*msg->opg_Storage = (LONG)d->dd_Product;
			return TRUE;

		case MMA_Sonix_Sensor:
			*msg->opg_Storage = (LONG)d->dd_Sensor;
			return TRUE;

		case MMA_Sonix_FrameIndex:
			*msg->opg_Storage = (LONG)d->dd_FrameIndex;
			return TRUE;

		case MMA_Sonix_FrameTime:
			*msg->opg_Storage = (LONG)d->dd_FrameTime;
			return TRUE;

		case MMA_StreamSeekable:
			*msg->opg_Storage = FALSE;
			return TRUE;

		default:
			return DoSuperMethodA(cl, obj, (Msg)msg);
	}
}

///
/// Pull()
///
/// Port 1 serves the frame currently in dd_Frame and fetches the next one when
/// it runs out, which is where the source is waited on.  Port 0 is only
/// pulled by the object itself, an application asking for it gets the
/// passthrough.

LONG Pull(Class *cl, Object *obj, struct mmopData *msg)
{
	GET_DATA;
	ULONG bytes_pulled = 0;

	/* a negative length would be read as a huge unsigned one and copy the
	 * rest of the frame into a buffer the caller never sized for it */

	if (!msg->Buffer || msg->Length <= 0)
	{
		seterr(MMERR_WRONG_ARGUMENTS);
		return 0;
	}

	DoMethod(obj, MMM_LockObject);
	seterr(0);

	/* once the framing is lost there is nothing sensible left to hand out,
	 * the next bytes of the stream would be taken for a frame header */

	if (d->dd_Failed)
	{
		seterr(MMERR_IO_ERROR);
		DoMethod(obj, MMM_UnlockObject);
		return 0;
	}

	switch (msg->Port)
	{
		case 0:
			bytes_pulled = DoSuperMethodA(cl, obj, (Msg)msg);
		break;

		case 1:
			if (!d->dd_HaveHeader)
			{
				if (!ReadHeader(cl, obj)) break;
			}

			if (d->dd_FramePos >= d->dd_FrameLen)
			{
				if (!ReadFrame(cl, obj)) break;
			}

			{
				ULONG avail = d->dd_FrameLen - d->dd_FramePos;
				ULONG len   = (ULONG)msg->Length < avail ? (ULONG)msg->Length : avail;

				memcpy(msg->Buffer, d->dd_Frame + d->dd_FramePos, len);

				d->dd_FramePos += len;
				bytes_pulled    = len;
			}
		break;

		default:
			seterr(MMERR_WRONG_ARGUMENTS);
	}

	DoMethod(obj, MMM_UnlockObject);

	return bytes_pulled;
}

///
/// Setup()
///
/// Reading the stream header on port 0 is all there is, it also picks the
/// output format for port 1.

LONG Setup(Class *cl, Object *obj, struct mmopPort *msg)
{
	GET_DATA;
	LONG rv = FALSE;

	if (msg->Port == 0)
	{
		DoMethod(obj, MMM_LockObject);
		seterr(0);

		if (d->dd_HaveHeader) rv = TRUE;
		else if (ReadHeader(cl, obj)) rv = TRUE;   /* the port format is set by ReadHeader */

		DoMethod(obj, MMM_UnlockObject);
	}
	else if (msg->Port == 1) rv = TRUE;
	else seterr(MMERR_WRONG_ARGUMENTS);

	return rv;
}

///
/// GetPort()

LONG GetPort(Class *cl, Object *obj, struct mmopGetPort *msg)
{
	switch (msg->Attribute)
	{
		case MMA_Video_Width:
		case MMA_Video_Height:
		case MMA_Video_BitsPerPixel:
		case MMA_Video_FrameCount:
		case MMA_Video_FpsNumerator:
		case MMA_Video_FpsDenominator:
		case MMA_DataFormat:
		case MMA_MediaType:
		case MMA_Sonix_VendorID:
		case MMA_Sonix_ProductID:
		case MMA_Sonix_Sensor:
		case MMA_Sonix_FrameIndex:
		case MMA_Sonix_FrameTime:
			return DoMethod(obj, OM_GET, msg->Attribute, (ULONG)msg->Storage);
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
		case MMM_Pull:     return Pull(cl, obj, (struct mmopData*)msg);
		case MMM_Setup:    return Setup(cl, obj, (struct mmopPort*)msg);
		case MMM_GetPort:  return GetPort(cl, obj, (struct mmopGetPort*)msg);
		default:           return DoSuperMethodA(cl, obj, msg);
	}
}

/// end
