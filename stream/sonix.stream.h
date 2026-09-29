#ifndef SONIX_STREAM_H
#define SONIX_STREAM_H

#include <exec/types.h>
#include <classes/multimedia/multimedia.h>

/*
 * Public interface of the sonix.stream Reggae class.
 *
 * sonix.stream is an MMCLASS_STREAM object.  Its single port 0 is an output
 * carrying MMF_STREAM, so it has to be followed by a demultiplexer; the one
 * shipped here is sonix.demuxer, which turns the framed byte stream into
 * MMF_VIDEO_RGB24 (or MMFC_VIDEO_GRAY8) pictures.
 *
 * The object is created and configured like this:
 *
 *   obj = NewObject(NULL, "sonix.stream",
 *                   MMA_StreamName, (ULONG)"0c45:6009",
 *                   MMA_Sonix_Format, SNXF_RGB24,
 *                   TAG_END);
 *
 * The camera is only claimed when the object is set up, so creating an object
 * for a camera which is not plugged in succeeds and only MMM_Setup() fails.
 * That lets an application build a pipeline first and open the hardware once
 * the chain is complete.
 *
 * The stream never ends by itself: MMM_Pull() on port 0 blocks in the USB
 * transfer until the next frame has been read.  It stops only when the camera
 * is unplugged or stops answering, which is reported as MMERR_IO_ERROR.
 *
 * MMM_Peek() is answered by the class and does not move the read position:
 * it returns the bytes the next pull would return.  The first call therefore
 * gives out the SnxStreamHeader, which can be read without losing it.
 */

/* Requested USB ids, a value of zero matches any device.  They are only of
 * use before the camera has been claimed: asking for a camera which is not
 * there succeeds, and MMM_Setup() is what reports it.  The ids of the camera
 * which was claimed end up in the stream header and, through the demuxer, in
 * MMA_Sonix_VendorID and MMA_Sonix_ProductID again. */

#define MMA_Sonix_VendorID   (MMA_Dummy + 1600)  /* [I.S.G], UWORD  */
#define MMA_Sonix_ProductID  (MMA_Dummy + 1601)  /* [I.S.G], UWORD  */

/* Format the frame data is converted to, one of SNXF_*.  Defaults to
 * SNXF_RGB24; SNXF_GRAY8 skips the Bayer interpolation and is roughly three
 * times faster, at the price of colour.  Changing it while the stream is
 * running discards the frame in flight and restarts the stream header. */

#define MMA_Sonix_Format     (MMA_Dummy + 1602)  /* [I.S.G], ULONG  */

/* USB product string of the camera that was claimed, valid after setup. */

#define MMA_Sonix_ProductName (MMA_Dummy + 1603) /* [..S.G], STRPTR  */

/* SNXS_* of the sensor found behind the controller, valid after setup. */

#define MMA_Sonix_Sensor     (MMA_Dummy + 1604)  /* [..S.G], ULONG   */
#define MMA_Sonix_SensorName (MMA_Dummy + 1605)  /* [..S.G], STRPTR  */

/* White balance and exposure, all settable while the stream is running.
 * Ranges match the original driver and the values are clamped to them:
 * 0..31 for the colour components, 0..255 for gain, 0..31 for brightness
 * and contrast.  Brightness and contrast are implemented by a PAS106B
 * sensor only, a TAS5110C1B has no register for them and reports 0. */

#define MMA_Sonix_Red        (MMA_Dummy + 1610)  /* [..S.G], LONG    */
#define MMA_Sonix_Green      (MMA_Dummy + 1611)  /* [..S.G], LONG    */
#define MMA_Sonix_Blue       (MMA_Dummy + 1612)  /* [..S.G], LONG    */
#define MMA_Sonix_Gain       (MMA_Dummy + 1613)  /* [..S.G], LONG    */
#define MMA_Sonix_Brightness (MMA_Dummy + 1614)  /* [..S.G], LONG    */
#define MMA_Sonix_Contrast   (MMA_Dummy + 1615)  /* [..S.G], LONG    */

/*
 * MMA_StreamName carries an optional device selector.  Accepted forms are
 * "vendor:product" with both numbers in hex without a prefix, for example
 * "0c45:6009" for a Sweex 100K, and a bare hex product id, for example
 * "6009", which matches any camera with that product.  An empty or absent
 * name takes the first supported camera Poseidon reports.
 *
 * Devices the driver knows about are the Sonix sn9c102 bridge with a PAS106B
 * or TAS5110C1B sensor, that is USB 0c45:6009 (Sweex 100K, PAS106B),
 * 0c45:6005 (Sweex Mini 100K / Macally IceCam, TAS5110C1B), 0c45:6007
 * (Macally IceCam Portable) and 0c45:6029 (Trust SpaceCam 150).  Any other
 * 0c45 device is probed the same way and accepted if it answers as an
 * sn9c102 with a supported sensor.
 */

#endif /* SONIX_STREAM_H */
