#ifndef SONIX_BAYER_H
#define SONIX_BAYER_H

#include <exec/types.h>

/*
 * Convert one interleaved 8 bit Bayer frame of WIDTH*HEIGHT bytes into packed
 * RGB24 (three bytes per pixel, no alpha).  Taken verbatim from the original
 * Sonix driver, see bayer.c for the license it has to be shipped under.
 */

void bayer2rgb24(UBYTE *dst, UBYTE *src, long WIDTH, long HEIGHT);

/*
 * Convert the same frame into one byte of luminance per pixel, which is what
 * MMF_VIDEO_GRAY8 claims the demuxer hands out: a 2x2 average of the mosaic,
 * four times cheaper than the RGB24 interpolation and colour is lost either
 * way.  WIDTH and HEIGHT have to be even.  Written for this driver, so it
 * carries no licence of its own.
 */

void bayer2gray8(UBYTE *dst, UBYTE *src, long WIDTH, long HEIGHT);

#endif /* SONIX_BAYER_H */
