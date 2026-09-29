/* recognition code for sonix.stream framed byte streams */

#define SYSTEM_PRIVATE

#include <proto/intuition.h>
#include <proto/multimedia.h>
#include <clib/alib_protos.h>

#include "class_version.h"
#include "sonix.demuxer.h"

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

	/* Enough of the stream header to check the magic, the version and that
	 * the announced geometry is one a decoder can be pointed at.  A live
	 * stream has no length to check against, the header itself has to carry
	 * the whole confidence. */

	if (DoMethod(dcc->dcc_Source, MMM_Peek, dcc->dcc_Port, (ULONG)header, 32) == 32)
	{
		if (header[0] == 'S' && header[1] == 'N' && header[2] == 'X' && header[3] == '1')
		{
			probability = 5000;

			/* version, currently 1 */
			if (header[7] == 0 && header[6] == 1)
			{
				probability += 3000;
			}

			/* pixel format, byte per pixel have to agree */
			if ((header[12] == SNXF_GRAY8 && header[13] == 1) ||
			    (header[12] == SNXF_RGB24 && header[13] == 3))
			{
				probability += 1500;
			}

			/* a geometry the sn9c102 driver actually produces */
			if (header[8] == 0 && header[9] == 0x01 &&   /* 256   */
			    header[10] == 0 && header[11] == 0x01 &&  /* 256   */
			    header[19] & 0x01)                       /* live flag */
			{
				probability += 500;
			}
		}
	}

	DoMethod(dcc->dcc_Source, MMM_Restore);
	return probability;
}
