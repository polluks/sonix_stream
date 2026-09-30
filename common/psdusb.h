#ifndef SONIX_PSDUSB_H
#define SONIX_PSDUSB_H

#include <exec/types.h>

/*
 * Minimal Poseidon (poseidon.library) USB backend, refactored out of the
 * original driver's src/libusb_poseidon.c.  It provides just enough of the
 * libusb-0.1 surface for the sn9c102 driver: device enumeration, claiming an
 * application binding, control transfers and one bulk IN pipe.
 *
 * All calls are blocking and must not be made from an interrupt handler.
 */

/* Open poseidon.library.  Returns TRUE on success. */
BOOL sonix_usb_init(void);

/* Close poseidon.library.  Safe to call without a preceding init. */
void sonix_usb_exit(void);

/*
 * Look for the first attached camera with the given ids.  Pass 0 for vid or
 * pid to match anything.  Returns an opaque handle or NULL, and fills in the
 * product string of the device it picked (truncated to buflen-1 bytes).
 */
APTR sonix_usb_open(UWORD vendor, UWORD product, STRPTR productname, ULONG buflen);

/* Release a handle obtained from sonix_usb_open(). */
void sonix_usb_close(APTR handle);

/* Vendor/product id of an open handle. */
void sonix_usb_ids(APTR handle, UWORD *vendor, UWORD *product);

/*
 * Synchronous control transfer.  Returns the number of bytes transferred, or
 * -1 on error.  Mirrors libusb's usb_control_msg() including the direction
 * and type bits packed into requesttype and the wValue/wIndex pair.
 */
LONG sonix_usb_control(APTR handle, ULONG requesttype, ULONG request,
                       ULONG value, ULONG index, UBYTE *bytes, ULONG size);

/*
 * Synchronous read of the bulk IN pipe.  Loops until size bytes have arrived,
 * so a single short transfer does not truncate a frame.  Returns the number of
 * bytes actually received, or -1 if the very first transfer failed.  The
 * webcam has a single bulk endpoint and ignores the endpoint number.
 */
LONG sonix_usb_bulk_read(APTR handle, UBYTE *bytes, ULONG size);

/* Size of a single bulk packet, used to size the read timeout. */
ULONG sonix_usb_bulk_packetsize(APTR handle);

/* FALSE once Poseidon has reported the device gone, so the caller can skip
 * transfers that could only fail. */
BOOL sonix_usb_alive(APTR handle);

/* Sleep, so the capture loop can back off without a busy spin. */
void sonix_usb_delay_ms(LONG ms);

/*
 * Monotonic wall clock in microseconds, used to timestamp frames.  Valid
 * after sonix_usb_init(), wraps around roughly every hour.
 */
ULONG sonix_usb_now_us(void);

#endif /* SONIX_PSDUSB_H */
