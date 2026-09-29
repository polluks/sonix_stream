/* recognition code for sonix.stream framed byte streams */

#define SYSTEM_PRIVATE

#include <proto/intuition.h>
#include <proto/multimedia.h>
#include <clib/alib_protos.h>

#include "class_version.h"
#include "sonix.demuxer.h"
#include "../sonixwire.h"

ULONG Recognize(struct DtCodeContext *dcc, ULONG recog_type);
const struct TagItem* ClassAttributes(void);

const struct TagItem ClassTags[] = {
	{MMA_RecognizeCode, (ULONG)Recognize},
	{MMA_MediaType, MMT_VIDEO},
	{MMA_ClassType, MMCLASS_DEMUXER},
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
	 * A demuxer sees the stream behind it, so the check is on the
	 * SnxStreamHeader its source hands out.  The source is peeked and not
	 * pulled, which keeps recognition free of side effects: the header is
	 * still the first 32 bytes a pull returns.
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

	/* only hand back what a peek actually took, and only if it took
	 * something, a source without a method for it must not be called */

	if (dcc->dcc_Source)
	{
		DoMethod(dcc->dcc_Source, MMM_Restore);
	}

	return probability;
}
