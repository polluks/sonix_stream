/* recognition code for the framed byte stream sonix.stream produces */

#define SYSTEM_PRIVATE

#include <proto/intuition.h>
#include <proto/multimedia.h>
#include <clib/alib_protos.h>

#include "class_version.h"
#include "sonix.stream.h"
#include "../sonixwire.h"

ULONG Recognize(struct DtCodeContext *dcc, ULONG recog_type);
const struct TagItem* ClassAttributes(void);

const struct TagItem ClassTags[] = {
	{MMA_RecognizeCode, (ULONG)Recognize},
	{MMA_MediaType, MMT_VIDEO},
	{MMA_ClassType, MMCLASS_STREAM},
	{TAG_END, 0}
};

const struct TagItem* ClassAttributes(void)
{
	return ClassTags;
}

ULONG Recognize(struct DtCodeContext *dcc, ULONG recog_type)
{
	struct Library *IntuitionBase = dcc->dcc_IntuitionBase;
	struct Library *MultimediaBase = dcc->dcc_MultimediaBase;
	LONG probability = 0;
	UBYTE header[32];

	IntuitionBase = IntuitionBase;
	MultimediaBase = MultimediaBase;
	recog_type = recog_type;

	/*
	 * A source has no data before it has been set up, so the peek is what
	 * claims the camera.  sonix.stream peeks without moving its read
	 * position, which means recognising a source costs one frame interval
	 * and does not cost a single byte of the stream: the SnxStreamHeader is
	 * still the first thing a pull hands out afterwards.
	 *
	 * A live stream has no length to check against, so the header itself
	 * carries the whole confidence.
	 */

	if (dcc->dcc_Source && DoMethod(dcc->dcc_Source, MMM_Peek, dcc->dcc_Port, (ULONG)header, 32) == 32)
	{
		if (header[0] == 'S' && header[1] == 'N' && header[2] == 'X' && header[3] == '1')
		{
			probability = 5000;

			/* header size, 32 bytes for version 1 */

			if (header[4] == 0 && header[5] == 32)
			{
				probability += 2000;
			}

			/* version, currently 1 */

			if (header[6] == 0 && header[7] == 1)
			{
				probability += 3000;
			}

			/* pixel format and bytes per pixel have to agree */

			if ((header[12] == SNXF_GRAY8 && header[13] == 1) ||
			    (header[12] == SNXF_RGB24 && header[13] == 3))
			{
				probability += 1500;
			}

			/* 352x288, the only geometry the sn9c102 driver produces */

			if (header[8] == 0x01 && header[9] == 0x60 &&   /* 352 */
			    header[10] == 0x01 && header[11] == 0x20 && /* 288 */
			    header[19] & SNXSH_LIVE)
			{
				probability += 500;
			}
		}
	}

	return probability;
}
