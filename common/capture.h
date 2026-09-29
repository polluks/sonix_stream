#ifndef SONIX_CAPTURE_H
#define SONIX_CAPTURE_H

#include <exec/types.h>

#include "sonixwire.h"

/*
 * sn9c102 controller driver, refactored from the original MorphOS Sonix port
 * (src/sonix.c).  Supported combinations are the Sonix sn9c102 bridge with a
 * PAS106B or a TAS5110C1B sensor, at 352x288 only, exactly as in the
 * original.  The PAC207 appears for completeness but is not reachable
 * through this bulk mode backend, so it is refused.
 */

struct SonixCam;

/* Geometry of every frame this driver produces. */

#define SONIX_WIDTH        352
#define SONIX_HEIGHT       288
#define SONIX_FRAME_BYTES  (SONIX_WIDTH * SONIX_HEIGHT)

/* Error codes reported by sonix_cam_open(). */

#define SONIX_OK                0
#define SONIX_ERR_NO_LIBRARY   -1  /* poseidon.library could not be opened */
#define SONIX_ERR_NO_DEVICE    -2  /* no matching camera attached            */
#define SONIX_ERR_USB          -3  /* the device could not be claimed        */
#define SONIX_ERR_CONTROLLER   -4  /* the device is not an sn9c102            */
#define SONIX_ERR_SENSOR       -5  /* neither PAS106B nor TAS5110C1B         */

/*
 * Open the first attached camera, or the one with the given ids when vendor
 * or product is not zero.  Returns NULL on failure and stores one of the
 * SONIX_ERR_* codes in *error when error is not NULL.  productname receives
 * the USB product string of the device, truncated to buflen-1 bytes.
 */
struct SonixCam *sonix_cam_open(UWORD vendor, UWORD product,
                                 STRPTR productname, ULONG buflen,
                                 LONG *error);

/* Stop the sensor, release the USB pipes and the device binding. */
void sonix_cam_close(struct SonixCam *cam);

/* USB ids of an open camera. */
void sonix_cam_ids(struct SonixCam *cam, UWORD *vendor, UWORD *product);

/* SNXS_* of the sensor found behind the controller. */
UBYTE sonix_cam_sensor(struct SonixCam *cam);

/* Human readable sensor name, "unknown" when the camera is gone. */
CONST_STRPTR sonix_cam_sensor_name(struct SonixCam *cam);

/* 352x288, the only mode this driver knows. */
ULONG sonix_cam_width(struct SonixCam *cam);
ULONG sonix_cam_height(struct SonixCam *cam);

/*
 * Grab one frame.  The first SONIX_WIDTH*SONIX_HEIGHT bytes of raw receive
 * the interleaved Bayer pattern of the current frame, the rest is scratch.
 * Returns SONIX_OK, or a negative SONIX_ERR_* code when the camera stopped
 * delivering data.
 */
LONG sonix_cam_capture(struct SonixCam *cam, UBYTE *raw);

/* Colour processing, 0..31 on the original driver's scale. */

void sonix_cam_set_red(struct SonixCam *cam, LONG value);
void sonix_cam_set_green(struct SonixCam *cam, LONG value);
void sonix_cam_set_blue(struct SonixCam *cam, LONG value);
LONG sonix_cam_get_red(struct SonixCam *cam);
LONG sonix_cam_get_green(struct SonixCam *cam);
LONG sonix_cam_get_blue(struct SonixCam *cam);

/* Exposure and contrast on the original driver's scale: gain takes 0..255,
 * brightness and contrast 0..31, and every setter clamps to its range.  A
 * PAS106B carries them in sensor registers and is read back from there, a
 * TAS5110C1B has no register for gain, brightness and contrast, so those
 * three do nothing there and report 0.  Gain reaches the camera as the
 * setting divided by 8, so a PAS106B reads back the divided value. */

void sonix_cam_set_gain(struct SonixCam *cam, LONG value);
LONG sonix_cam_get_gain(struct SonixCam *cam);
void sonix_cam_set_brightness(struct SonixCam *cam, LONG value);
LONG sonix_cam_get_brightness(struct SonixCam *cam);
void sonix_cam_set_contrast(struct SonixCam *cam, LONG value);
LONG sonix_cam_get_contrast(struct SonixCam *cam);

/* Last measured inter frame time in microseconds, 0 until two frames are in. */
ULONG sonix_cam_frame_time(struct SonixCam *cam);

/*
 * Monotonic wall clock in microseconds, for frame timestamps.  Wraps around
 * roughly every hour, so use it for differences and not for absolute times.
 */
ULONG sonix_cam_now_us(void);

#endif /* SONIX_CAPTURE_H */
