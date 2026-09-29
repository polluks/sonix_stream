/*
 * capture.c
 *
 * Sonix sn9c102 bridge with a PAS106B or TAS5110C1B sensor, 352x288 only.
 * Refactored from the original MorphOS Sonix port (src/sonix.c): the USB
 * layer now lives in psdusb.c, the logging goes away and the driver state
 * moved into struct SonixCam so more than one camera can be driven.
 */

#define SYSTEM_PRIVATE

#include <proto/exec.h>

#include "capture.h"
#include "psdusb.h"

/*
 * USB control transfers.  The literal values come from the original driver:
 * request 0x00 reads a controller register and request 0x08 writes one, with
 * the register number in wValue.
 */

/*
 * bmRequestType, also from the original driver: a vendor request directed at
 * the device, 0xc1 to read and 0x41 to write.  The three parts are the
 * direction, the vendor type and the device recipient.
 */

#define SNX_DIR_OUT           0x00
#define SNX_DIR_IN            0x80
#define SNX_TYPE_VENDOR       0x40
#define SNX_RECIP_DEVICE      0x01

#define SNX_CTRL_READ         (SNX_DIR_IN  | SNX_TYPE_VENDOR | SNX_RECIP_DEVICE)  /* 0xc1 */
#define SNX_CTRL_WRITE        (SNX_DIR_OUT | SNX_TYPE_VENDOR | SNX_RECIP_DEVICE)  /* 0x41 */

#define SNX_REQ_READ          0x00
#define SNX_REQ_WRITE         0x08

/* Controller registers. */

#define SNX_REG_SENSOR_CTRL   0x01
#define SNX_REG_I2C           0x08
#define SNX_REG_I2C_RESULT    0x0a
#define SNX_REG_H_START       0x12
#define SNX_REG_V_START       0x13
#define SNX_REG_OFFSET        0x14
#define SNX_REG_H_SIZE        0x15
#define SNX_REG_V_SIZE        0x16
#define SNX_REG_CLK_CTRL      0x17
#define SNX_REG_COMPRESSION   0x18
#define SNX_REG_TEST          0x19
#define SNX_REG_HO_SIZE       0x1a
#define SNX_REG_VO_SIZE       0x1b

#define SNX_SENSOR_POWER_DOWN 0x01
#define SNX_VIDEO_ENABLE      0x04
#define SNX_I2C_READY         0x04
#define SNX_I2C_ERROR         0x08

/* TAS5110C1B gains live in the top nibble pairs of register 0x10/0x11. */

#define SNX_TAS5110_RED_GAIN  0x0c
#define SNX_TAS5110_BLUE_GAIN 0x09
#define SNX_TAS5110_G1_GAIN   0x0a
#define SNX_TAS5110_G2_GAIN   0x0b
#define SNX_TAS5110_GLOBAL    0x0e
#define SNX_TAS5110_BRIGHT    0x0d
#define SNX_TAS5110_CONTRAST  0x0f
#define SNX_TAS1110_VALIDATE  0x13

/* Scratch space the USB read needs on top of the picture itself.  A sn9c102
 * bulk transfer of 352x288 arrives with a 12 byte packet header. */

#define SNX_RAW_OVERHEAD      128
#define SNX_RAW_SIZE          (SONIX_FRAME_BYTES + SNX_RAW_OVERHEAD)
#define SNX_HEADER_SIZE      12

/* PAS106B and TAS5110C1B register defaults, 352x288. */

static const UBYTE pas106b_regs[][2] =
{
	{SNX_REG_SENSOR_CTRL, 0x00},
	{0x10, 0x00},              /* red and blue gain  */
	{0x11, 0x00},              /* green gain         */
	{SNX_REG_OFFSET,    0x00},
	{0x17, 0x20},
	{0x19, 0x20},
	{SNX_REG_COMPRESSION, 0x0e},
	{SNX_REG_H_START,   0x57},
	{SNX_REG_V_START,   0x01},
	{SNX_REG_OFFSET,    0x00},
	{SNX_REG_H_SIZE,    0x0d},   /* 352 / 32 */
	{SNX_REG_V_SIZE,    0x0b},   /* 288 / 32 */
	{SNX_REG_CLK_CTRL,  0x60},   /* SEN_CLK_EN */
	{0x19, 0x24},
	{SNX_REG_HO_SIZE,   0x0b},
	{SNX_REG_VO_SIZE,   0x09},
	{0x1e, 0x0b},              /* AE_ENDX */
	{0x1f, 0x09}               /* AE_ENDY */
};

static const UBYTE pas106b_sensor_init[][2] =
{
	{ 2, 0x0c},   /* pixel clock divider   */
	{ 3, 0x16},   /* frame time, MSB       */
	{ 4, 0x00},   /* frame time, LSB       */
	{ 5, 0x65},   /* shutter line offset   */
	{ 6, 0x88},   /* shutter pixel offset  */
	{ 8, 0x01},   /* black level subtract  */
	{16, 0x06},
	{17, 0x06},
	{18, 0x00},
	{20, 0x02}
};

static const UBYTE pas106b_validate[][2] =
{
	{SNX_TAS1110_VALIDATE, 0x01}
};

/* TAS5110C1B wants its setup written as eight byte I2C cycles. */

static const UBYTE tas5110_sensor_init[][8] =
{
	{0x30, 0x11, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x10},
	{0x30, 0x11, 0x02, 0x20, 0xa9, 0x00, 0x00, 0x10},
	{0xa0, 0x61, 0x9a, 0xca, 0x00, 0x00, 0x00, 0x17}
};

static const UBYTE tas5110_gain_init[][2] =
{
	{SNX_REG_SENSOR_CTRL, 0x01},
	{SNX_REG_SENSOR_CTRL, 0x44},
	{0x02, 0x03},
	{0x08, 0x20},
	{0x09, 0x11},   /* I2C slave id of the sensor */
	{0x0a, 0x00},
	{0x0b, 0x00},
	{0x0c, 0x00},
	{0x0d, 0x00},
	{0x0e, 0x00},
	{0x0f, 0x00},
	{0x10, 0x00},   /* red and blue gain */
	{0x11, 0x00},   /* green gain        */
	{SNX_REG_H_START,   67},      /* 0x43 */
	{SNX_REG_V_START,   9},
	{SNX_REG_OFFSET,    0x0a},
	{SNX_REG_H_SIZE,    0x16},    /* 352 / 16 */
	{SNX_REG_V_SIZE,    0x12},    /* 288 / 16 */
	{0x17, 0x60},
	{SNX_REG_COMPRESSION, 0x06},  /* bit 7 would enable compression */
	{SNX_REG_TEST,      0xfb},
	{SNX_REG_HO_SIZE,   0x14},
	{SNX_REG_VO_SIZE,   0x0a},
	{0x1c, 0x02},
	{0x1d, 0x02},
	{0x1e, 0x09},
	{0x1f, 0x07}
};

struct SonixCam
{
	APTR  sc_Usb;
	UBYTE sc_Sensor;
	UBYTE sc_Raw[SNX_RAW_SIZE];
	LONG  sc_Red;
	LONG  sc_Green;
	LONG  sc_Blue;
	ULONG sc_LastStamp;
	ULONG sc_FrameTime;
};

/*
 * clamp()
 */

static LONG clamp(LONG value, LONG low, LONG high)
{
	if (value < low) return low;
	if (value > high) return high;
	return value;
}

/*
 * reg_read() / reg_write()
 */

static LONG reg_read(struct SonixCam *cam, UWORD reg)
{
	UBYTE buf[8];

	if (sonix_usb_control(cam->sc_Usb, SNX_CTRL_READ, SNX_REQ_READ,
	                      reg, 0, buf, 1) < 0)
	{
		return -1;
	}

	return buf[0];
}

static LONG reg_write(struct SonixCam *cam, UWORD reg, UWORD value)
{
	UBYTE buf[8];

	buf[0] = (UBYTE)value;

	if (sonix_usb_control(cam->sc_Usb, SNX_CTRL_WRITE, SNX_REQ_WRITE,
	                      reg, 0, buf, 1) < 0)
	{
		return -1;
	}

	return 0;
}

static LONG reg_write_buf(struct SonixCam *cam, UWORD reg, const UBYTE *buf, ULONG len)
{
	if (sonix_usb_control(cam->sc_Usb, SNX_CTRL_WRITE, SNX_REQ_WRITE,
	                      reg, 0, (UBYTE *)buf, len) < 0)
	{
		return -1;
	}

	return 0;
}

/*
 * video_enable()
 *
 * The sn9c102 only streams while bit 2 of register 1 is set, so every frame
 * is bracketed by clearing and setting it again.
 */

static LONG video_enable(struct SonixCam *cam, BOOL on)
{
	return reg_write(cam, SNX_REG_SENSOR_CTRL, on ? SNX_VIDEO_ENABLE : 0x00);
}

/*
 * i2c_wait()
 */

static LONG i2c_wait(struct SonixCam *cam)
{
	int i;

	for (i = 0; i < 5; i++)
	{
		LONG r = reg_read(cam, SNX_REG_I2C);

		if (r < 0) return -1;
		if (r & SNX_I2C_READY) return 0;

		sonix_usb_delay_ms(1);
	}

	return -1;
}

/*
 * i2c_error()
 */

static LONG i2c_error(struct SonixCam *cam)
{
	LONG r = reg_read(cam, SNX_REG_I2C);

	if (r < 0) return -1;

	return (r & SNX_I2C_ERROR) ? -1 : 0;
}

/*
 * i2c_read()
 */

static LONG i2c_read(struct SonixCam *cam, UBYTE address)
{
	UBYTE buf[8];
	LONG err = 0;

	buf[0] = 0x91;      /* 400 kHz, two wires */
	buf[1] = 0x40;
	buf[2] = address;
	buf[3] = 0x00;
	buf[4] = 0x00;
	buf[5] = 0x00;
	buf[6] = 0x00;
	buf[7] = 0x10;

	if (reg_write_buf(cam, SNX_REG_I2C, buf, 8) < 0) err = -1;
	if (i2c_wait(cam) < 0)        err = -1;

	buf[0] = 0x93;      /* same bus, now reading */
	buf[1] = 0x40;
	buf[7] = 0x10;

	if (reg_write_buf(cam, SNX_REG_I2C, buf, 8) < 0) err = -1;
	if (i2c_wait(cam) < 0) err = -1;

	if (sonix_usb_control(cam->sc_Usb, SNX_CTRL_READ, SNX_REQ_READ,
	                      SNX_REG_I2C_RESULT, 0, buf, 5) < 0) err = -1;
	if (i2c_error(cam) < 0) err = -1;

	return err ? -1 : (LONG)buf[4];
}

/*
 * i2c_read_cached()
 *
 * Reading a sensor register costs four control transfers.  The gains are
 * polled often enough by applications that the value is worth keeping, but
 * only for a short while, so a stale entry expires on its own.
 */

/*
 * i2c_write()
 */

static LONG i2c_write(struct SonixCam *cam, UBYTE address, UBYTE value)
{
	UBYTE buf[8];
	LONG err = 0;

	buf[0] = 0xa1;
	buf[1] = 0x40;
	buf[2] = address;
	buf[3] = value;
	buf[4] = 0x00;
	buf[5] = 0x00;
	buf[6] = 0x00;
	buf[7] = 0x14;

	if (reg_write_buf(cam, SNX_REG_I2C, buf, 8) < 0) err = -1;
	if (i2c_wait(cam) < 0) err = -1;
	if (i2c_error(cam) < 0) err = -1;

	return err;
}

/*
 * i2c_write_buf()
 */

static LONG i2c_write_buf(struct SonixCam *cam, const UBYTE *buf)
{
	LONG err = 0;

	if (reg_write_buf(cam, SNX_REG_I2C, buf, 8) < 0) err = -1;
	if (i2c_wait(cam) < 0) err = -1;
	if (i2c_error(cam) < 0) err = -1;

	return err;
}

/*
 * sensor_validate()
 *
 * PAS106B and TAS5110C1B both need an explicit "settings changed" write to
 * latch the register block, 0x13/0x01 on the sensor.
 */

static void sensor_validate(struct SonixCam *cam)
{
	i2c_write(cam, SNX_TAS1110_VALIDATE, 0x01);
}

/*
 * pas106b_probe()
 */

static LONG pas106b_probe(struct SonixCam *cam)
{
	LONG r0, r1;
	ULONG pid;

	/* Minimal init to get the I2C block running, do not touch these. */

	if (reg_write(cam, SNX_REG_SENSOR_CTRL, SNX_SENSOR_POWER_DOWN) < 0) return -1;
	if (reg_write(cam, SNX_REG_SENSOR_CTRL, 0x00) < 0) return -1;
	if (reg_write(cam, SNX_REG_CLK_CTRL, 0x28) < 0) return -1;

	r0 = i2c_read(cam, 0x00);
	r1 = i2c_read(cam, 0x01);

	if (r0 < 0 || r1 < 0) return -1;

	pid = ((ULONG)r0 << 11) | (((ULONG)r1 & 0xf0) >> 4);

	return pid == 0x0007 ? 0 : -1;
}

/*
 * pas106b_init()
 */

static LONG pas106b_init(struct SonixCam *cam)
{
	ULONG i;
	LONG err = 0;

	for (i = 0; i < sizeof(pas106b_regs) / 2; i++)
	{
		if (reg_write(cam, pas106b_regs[i][0], pas106b_regs[i][1]) < 0) err = -1;
	}

	for (i = 0; i < sizeof(pas106b_sensor_init) / 2; i++)
	{
		if (i2c_write(cam, pas106b_sensor_init[i][0],
		              pas106b_sensor_init[i][1]) < 0) err = -1;
	}

	sensor_validate(cam);

	sonix_usb_delay_ms(300);

	return err;
}

/*
 * tas5110c1b_init()
 */

static LONG tas5110c1b_init(struct SonixCam *cam)
{
	ULONG i;
	LONG err = 0;

	for (i = 0; i < sizeof(tas5110_gain_init) / 2; i++)
	{
		if (reg_write(cam, tas5110_gain_init[i][0],
		              tas5110_gain_init[i][1]) < 0) err = -1;
	}

	for (i = 0; i < sizeof(tas5110_sensor_init) / 8; i++)
	{
		if (i2c_write_buf(cam, tas5110_sensor_init[i]) < 0) err = -1;
	}

	/* Throw one frame away, the first real one is never pretty. */

	video_enable(cam, TRUE);
	video_enable(cam, FALSE);

	sonix_usb_delay_ms(200);

	return err;
}

/*
 * sonix_cam_open()
 */

struct SonixCam *sonix_cam_open(UWORD vendor, UWORD product,
                                 STRPTR productname, ULONG buflen,
                                 LONG *error)
{
	struct SonixCam *cam;
	LONG err = SONIX_OK;
	LONG id;

	if (error) *error = SONIX_OK;

	if (!sonix_usb_init())
	{
		if (error) *error = SONIX_ERR_NO_LIBRARY;
		return NULL;
	}

	/* The camera state has to outlive this call and is too big for a local,
	 * so allocate it up front and fill it in once the device is claimed. */

	cam = (struct SonixCam *)AllocMem(sizeof(struct SonixCam), MEMF_ANY | MEMF_CLEAR);

	if (!cam)
	{
		if (error) *error = SONIX_ERR_USB;
		return NULL;
	}

	cam->sc_Usb = sonix_usb_open(vendor, product, productname, buflen);

	if (!cam->sc_Usb)
	{
		FreeMem(cam, sizeof(struct SonixCam));
		if (error) *error = SONIX_ERR_NO_DEVICE;
		return NULL;
	}

	id = reg_read(cam, 0x00);

	if (id < 0 || id != 0x10)
	{
		err = SONIX_ERR_CONTROLLER;
	}
	else if (pas106b_probe(cam) == 0)
	{
		cam->sc_Sensor = SNXS_PAS106B;
		if (pas106b_init(cam) < 0) err = SONIX_ERR_SENSOR;
	}
	else if (tas5110c1b_init(cam) == 0)
	{
		cam->sc_Sensor = SNXS_TAS5110C1B;
		cam->sc_Red = cam->sc_Green = cam->sc_Blue = 0;
	}
	else
	{
		err = SONIX_ERR_SENSOR;
	}

	if (err != SONIX_OK)
	{
		sonix_usb_close(cam->sc_Usb);
		FreeMem(cam, sizeof(struct SonixCam));
		if (error) *error = err;
		return NULL;
	}

	return cam;
}

/*
 * sonix_cam_close()
 */

void sonix_cam_close(struct SonixCam *cam)
{
	if (!cam) return;

	video_enable(cam, FALSE);
	sonix_usb_close(cam->sc_Usb);
	FreeMem(cam, sizeof(struct SonixCam));
}

/*
 * sonix_cam_ids()
 */

void sonix_cam_ids(struct SonixCam *cam, UWORD *vendor, UWORD *product)
{
	if (vendor)  *vendor  = 0;
	if (product) *product = 0;

	if (cam) sonix_usb_ids(cam->sc_Usb, vendor, product);
}

/*
 * sonix_cam_sensor()
 */

UBYTE sonix_cam_sensor(struct SonixCam *cam)
{
	return cam ? cam->sc_Sensor : SNXS_NONE;
}

/*
 * sonix_cam_sensor_name()
 */

CONST_STRPTR sonix_cam_sensor_name(struct SonixCam *cam)
{
	switch (sonix_cam_sensor(cam))
	{
		case SNXS_PAS106B:    return "PAS106B";
		case SNXS_TAS5110C1B: return "TAS5110C1B";
		case SNXS_PAC207:     return "PAC207";
		default:              return "unknown";
	}
}

/*
 * sonix_cam_width() / sonix_cam_height()
 */

ULONG sonix_cam_width(struct SonixCam *cam)
{
	cam = cam;
	return SONIX_WIDTH;
}

ULONG sonix_cam_height(struct SonixCam *cam)
{
	cam = cam;
	return SONIX_HEIGHT;
}

/*
 * sonix_cam_capture()
 *
 * One bulk transfer of a complete 352x288 frame.  The first 12 bytes of the
 * transfer are a packet header, the Bayer pattern follows immediately, so
 * the picture is handed back in cam->sc_Raw + SNX_HEADER_SIZE.
 */

LONG sonix_cam_capture(struct SonixCam *cam, UBYTE *raw)
{
	LONG got;
	ULONG now, delta;

	if (!cam || !raw) return SONIX_ERR_USB;

	video_enable(cam, TRUE);

	got = sonix_usb_bulk_read(cam->sc_Usb, cam->sc_Raw, SNX_RAW_SIZE);

	video_enable(cam, FALSE);

	if (got < 0) return SONIX_ERR_USB;

	/* The camera prefixes the picture with a 12 byte packet header, but the
	 * transfer length only counts payload on some stack versions, so accept
	 * the data at either offset. */

	if (got >= SONIX_FRAME_BYTES + SNX_HEADER_SIZE)
		CopyMem(cam->sc_Raw + SNX_HEADER_SIZE, raw, SONIX_FRAME_BYTES);
	else if (got >= SONIX_FRAME_BYTES)
		CopyMem(cam->sc_Raw, raw, SONIX_FRAME_BYTES);
	else
		return SONIX_ERR_USB;

	now = sonix_usb_now_us();

	if (cam->sc_LastStamp)
	{
		delta = now - cam->sc_LastStamp;

		/* Smooth the reading a little, a single frame can be late. */
		if (delta > 0 && delta < 2000000)
		{
			cam->sc_FrameTime = cam->sc_FrameTime
			                  ? (cam->sc_FrameTime * 3 + delta) / 4
			                  : delta;
		}
	}

	cam->sc_LastStamp = now;

	return SONIX_OK;
}

/*
 * sonix_cam_frame_time()
 */

ULONG sonix_cam_frame_time(struct SonixCam *cam)
{
	return cam ? cam->sc_FrameTime : 0;
}

/*
 * sonix_cam_now_us()
 */

ULONG sonix_cam_now_us(void)
{
	return sonix_usb_now_us();
}

/*
 * Component getters and setters.
 *
 * On a PAS106B every component lives in a sensor register and can be read
 * back.  A TAS5110C1B only exposes two gain registers of the bridge, red and
 * blue share register 0x10, so the component values are remembered instead.
 */

static LONG cached_get(LONG *cache)
{
	return *cache;
}

static void red_blue_apply(struct SonixCam *cam)
{
	if (cam->sc_Sensor == SNXS_PAS106B) return;

	reg_write(cam, 0x10, (UBYTE)(((cam->sc_Red / 2) & 0x0f) |
	                             (((cam->sc_Blue / 2) & 0x0f) << 4)));
}

void sonix_cam_set_red(struct SonixCam *cam, LONG value)
{
	if (!cam) return;

	value = clamp(value, 0, 31);
	cam->sc_Red = value;

	if (cam->sc_Sensor == SNXS_PAS106B)
	{
		i2c_write(cam, SNX_TAS5110_RED_GAIN, (UBYTE)value);
		sensor_validate(cam);
	}
	else red_blue_apply(cam);
}

void sonix_cam_set_green(struct SonixCam *cam, LONG value)
{
	if (!cam) return;

	value = clamp(value, 0, 31);
	cam->sc_Green = value;

	if (cam->sc_Sensor == SNXS_PAS106B)
	{
		i2c_write(cam, SNX_TAS5110_G1_GAIN, (UBYTE)value);
		i2c_write(cam, SNX_TAS5110_G2_GAIN, (UBYTE)value);
		sensor_validate(cam);
	}
	else
	{
		reg_write(cam, 0x11, (UBYTE)(value / 2));
	}
}

void sonix_cam_set_blue(struct SonixCam *cam, LONG value)
{
	if (!cam) return;

	value = clamp(value, 0, 31);
	cam->sc_Blue = value;

	if (cam->sc_Sensor == SNXS_PAS106B)
	{
		i2c_write(cam, SNX_TAS5110_BLUE_GAIN, (UBYTE)value);
		sensor_validate(cam);
	}
	else red_blue_apply(cam);
}

LONG sonix_cam_get_red(struct SonixCam *cam)
{
	LONG v;

	if (!cam) return 0;
	if (cam->sc_Sensor != SNXS_PAS106B) return cached_get(&cam->sc_Red);

	v = i2c_read(cam, SNX_TAS5110_RED_GAIN);

	return v < 0 ? 0 : v;
}

LONG sonix_cam_get_green(struct SonixCam *cam)
{
	LONG v;

	if (!cam) return 0;
	if (cam->sc_Sensor != SNXS_PAS106B) return cached_get(&cam->sc_Green);

	v = i2c_read(cam, SNX_TAS5110_G1_GAIN);

	return v < 0 ? 0 : v;
}

LONG sonix_cam_get_blue(struct SonixCam *cam)
{
	LONG v;

	if (!cam) return 0;
	if (cam->sc_Sensor != SNXS_PAS106B) return cached_get(&cam->sc_Blue);

	v = i2c_read(cam, SNX_TAS5110_BLUE_GAIN);

	return v < 0 ? 0 : v;
}

void sonix_cam_set_gain(struct SonixCam *cam, LONG value)
{
	if (!cam) return;

	value = clamp(value, 0, 255);

	if (cam->sc_Sensor == SNXS_PAS106B)
	{
		i2c_write(cam, SNX_TAS5110_GLOBAL, (UBYTE)(value >> 3));
		sensor_validate(cam);
	}
}

LONG sonix_cam_get_gain(struct SonixCam *cam)
{
	LONG v;

	if (!cam) return 0;
	if (cam->sc_Sensor != SNXS_PAS106B) return 15;

	v = i2c_read(cam, SNX_TAS5110_GLOBAL);

	return v < 0 ? 0 : v;
}

void sonix_cam_set_brightness(struct SonixCam *cam, LONG value)
{
	if (!cam) return;

	if (cam->sc_Sensor != SNXS_PAS106B) return;

	i2c_write(cam, SNX_TAS5110_BRIGHT, (UBYTE)(0x1f - clamp(value, 0, 0x1f)));
	sensor_validate(cam);
}

LONG sonix_cam_get_brightness(struct SonixCam *cam)
{
	LONG v;

	if (!cam || cam->sc_Sensor != SNXS_PAS106B) return 0;

	v = i2c_read(cam, SNX_TAS5110_BRIGHT);

	/* the register counts down, so a value above 0x1f would hand out a
	 * negative brightness the attribute promises to be 0 to 31 */

	if (v < 0)    return 0;
	if (v > 0x1f) return 0;

	return 0x1f - v;
}

void sonix_cam_set_contrast(struct SonixCam *cam, LONG value)
{
	if (!cam) return;

	if (cam->sc_Sensor != SNXS_PAS106B) return;

	i2c_write(cam, SNX_TAS5110_CONTRAST, (UBYTE)clamp(value, 0, 0x1f));
	sensor_validate(cam);
}

LONG sonix_cam_get_contrast(struct SonixCam *cam)
{
	LONG v;

	if (!cam || cam->sc_Sensor != SNXS_PAS106B) return 0;

	v = i2c_read(cam, SNX_TAS5110_CONTRAST);

	return v < 0 ? 0 : v;
}
