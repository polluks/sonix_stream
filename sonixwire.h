#ifndef SONIXWIRE_H
#define SONIXWIRE_H

/*
  Wire format spoken between sonix.stream (source) and sonix.demuxer.

  sonix.stream is a Reggae MMCLASS_STREAM object: its single port 0 always
  carries MMF_STREAM, so something has to strip the framing below before the
  pictures can reach a decoder.  sonix.demuxer does that and republishes the
  frame data as MMF_VIDEO_RGB24 / MMF_VIDEO_GRAY8.

  All fields are big endian.  MorphOS is a big endian machine, so the two
  structures below are stored exactly as laid out in memory and need no
  conversion; the field order is nevertheless chosen to stay big endian
  correct if the file is ever byte swapped for debugging.
*/

#include <exec/types.h>

/* Pixel formats of the frame data. */

#define SNXF_GRAY8   1
#define SNXF_RGB24   2

/* Sensor behind the sn9c102 controller, as probed over I2C. */

#define SNXS_NONE        0
#define SNXS_PAS106B     1
#define SNXS_TAS5110C1B  2
#define SNXS_PAC207      3

/* Stream header flags. */

#define SNXSH_LIVE       0x01  /* stream does not end on its own */

/* Frame header flags. */

#define SNXFF_KEYFRAME   0x01
#define SNXFF_SYNTHETIC  0x02  /* no camera data, buffer is zeroed    */
#define SNXFF_DROPPED    0x04  /* camera dropped frames, timing slipped */

/* Used in SnxStreamHeader::sh_FrameCount for an endless stream. */

#define SNX_FRAMECOUNT_UNKNOWN  0xFFFFFFFFUL

struct SnxStreamHeader
{
	UBYTE sh_Magic[4];        /* "SNX1"                                  */
	UWORD sh_HeaderSize;      /* sizeof(struct SnxStreamHeader), 32       */
	UWORD sh_Version;         /* wire format revision, currently 1       */
	UWORD sh_Width;           /* picture width in pixels                 */
	UWORD sh_Height;          /* picture height in pixels                */
	UBYTE sh_Format;          /* SNXF_*                                  */
	UBYTE sh_BytesPerPixel;   /* 1 for SNXF_GRAY8, 3 for SNXF_RGB24       */
	UWORD sh_VendorID;        /* USB vendor id of the camera             */
	UWORD sh_ProductID;       /* USB product id of the camera            */
	UBYTE sh_Sensor;          /* SNXS_*                                  */
	UBYTE sh_Flags;           /* SNXSH_*                                 */
	ULONG sh_FrameCount;      /* 0 / SNX_FRAMECOUNT_UNKNOWN for a live    */
	                         /* stream                                  */
	ULONG sh_FrameTime;       /* nominal inter frame time in microseconds */
	ULONG sh_Reserved;
};

struct SnxFrameHeader
{
	ULONG fh_FrameSize;       /* 16 + width * height * bytes per pixel   */
	ULONG fh_Index;           /* frame counter, first frame is 0          */
	ULONG fh_Timestamp;       /* microseconds since the stream was opened */
	ULONG fh_Flags;           /* SNXFF_*                                 */
};

/* Picture geometry and size of a single frame, decoded from a stream header.
 * The geometry arrives over the wire, so the product is taken in 64 bits: a
 * 32 bit multiply of 65535 * 65535 * 3 wraps to a small number.  A caller
 * which has established a sane geometry may cast the result down. */

#define SNX_FRAME_BYTES(hdr) \
	((UQUAD)(hdr)->sh_Width * (UQUAD)(hdr)->sh_Height * (UQUAD)(hdr)->sh_BytesPerPixel)

#endif /* SONIXWIRE_H */
