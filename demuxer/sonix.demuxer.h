#ifndef SONIX_DEMUXER_H
#define SONIX_DEMUXER_H

#include <exec/types.h>

#ifndef MMA_VIDEOMASK
#define MMA_VIDEOMASK 0x00002000
#endif

/*
 * Reggae class pair for Sonix sn9c102 webcams.
 *
 *   sonix.stream   output port 0 : MMF_STREAM       (framed, see sonixwire.h)
 *   sonix.demuxer  input  port 0 : MMF_STREAM
 *                  output port 1 : MMF_VIDEO_RGB24 or MMF_VIDEO_GRAY8
 *
 * The demuxer publishes the frames of a live source, so unlike the file
 * demuxers it never reports MMERR_END_OF_DATA: the input ends only when the
 * camera is unplugged, which comes back as MMERR_IO_ERROR.
 *
 * The wire format is described in sonixwire.h, one SnxStreamHeader followed
 * by any number of SnxFrameHeader plus pixel data pairs.  The demuxer
 * forwards the frames untouched and only republishes the stream header as
 * video attributes, so a frame which arrives with a broken header is refused
 * instead of being shown as garbage.
 */

/* Extra attributes the demuxer adds on top of the video ones. */

#define MMA_Sonix_VendorID    (MMA_Dummy + 1700)  /* [..G], UWORD  */
#define MMA_Sonix_ProductID   (MMA_Dummy + 1701)  /* [..G], UWORD  */
#define MMA_Sonix_Sensor      (MMA_Dummy + 1702)  /* [..G], ULONG, SNXS_* */
#define MMA_Sonix_FrameIndex  (MMA_Dummy + 1703)  /* [..G], ULONG, last frame served */
#define MMA_Sonix_FrameTime   (MMA_Dummy + 1704)  /* [..G], ULONG, inter frame time in us */

#endif /* SONIX_DEMUXER_H */
