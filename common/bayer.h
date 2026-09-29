#ifndef SONIX_BAYER_H
#define SONIX_BAYER_H

#include <exec/types.h>

/*
 * Convert one interleaved 8 bit Bayer frame of WIDTH*HEIGHT bytes into packed
 * RGB24 (three bytes per pixel, no alpha).  Taken verbatim from the original
 * Sonix driver, see bayer.c for the license it has to be shipped under.
 */

void bayer2rgb24(UBYTE *dst, UBYTE *src, long WIDTH, long HEIGHT);

#endif /* SONIX_BAYER_H */
