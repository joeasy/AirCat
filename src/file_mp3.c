/*
 * file_mp3.c - A MP3 file demuxer for File module
 *
 * Copyright (c) 2014   A. Dilly
 *
 * AirCat is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * AirCat is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with AirCat.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "file_private.h"

unsigned int bitrates[2][3][15] = {
	{ /* MPEG-1 */
		{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384,
		 416, 448},
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
		 384},
		{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256,
		 320}
	},
	{ /* MPEG-2 LSF, MPEG-2.5 */
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224,
		 256},
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}
	}
};
unsigned int samplerates[3][4] = {
	{44100, 48000, 32000, 0},
	{22050, 24000, 16000, 0},
	{11025, 8000, 8000, 0}
};

struct mp3_frame {
	unsigned char mpeg; /* 0: MPEG 1, 1: MPEG 2, 2: MPEG 2.5 */
	unsigned char layer; /* 0: layer 1, 1: layer 2, 2: layer 3 */
	unsigned int bitrate;
	unsigned long samplerate;
	unsigned char padding;
	unsigned char channels; /* 0: Mono, 1: Stereo, 2: Joint Stereo,
				   3: Dual channel */
	unsigned int length;
};

struct mp3_demux {
	unsigned long nb_bytes;
	unsigned int nb_frame;
	unsigned int quality;
	unsigned long offset;
};

static int file_mp3_parse_header(unsigned char *buffer, unsigned int size,
				 struct mp3_frame *f)
{
	if(size < 4)
		return -1;

	/* Check syncword */
	if(buffer[0] != 0xFF || (buffer[1] & 0xE0) != 0xE0)
		return -1;

	/* Get Mpeg version */
	f->mpeg = 3 - ((buffer[1] >> 3) & 0x03);
	if(f->mpeg == 2)
		return -1;
	if(f->mpeg == 3)
		f->mpeg = 2;

	/* Get Layer */
	f->layer = 3 - ((buffer[1] >> 1) & 0x03);
	if(f->layer == 3)
		return -1;

	/* Get bitrate */
	f->bitrate = (buffer[2] >> 4) & 0x0F;
	if(f->bitrate == 0 || f->bitrate == 15)
		return -1;
	else
	{
		if(f->mpeg != 2)
			f->bitrate = bitrates[f->mpeg][f->layer][f->bitrate];
		else
			f->bitrate = bitrates[1][f->layer][f->bitrate];
	}

	/* Get samplerate */
	f->samplerate = (buffer[2] >> 2) & 0x03;
	if(f->samplerate == 3)
		return -1;
	else
		f->samplerate = samplerates[f->mpeg][f->samplerate];

	/* Get padding */
	f->padding = (buffer[2] >> 1) & 0x01;

	/* Get channel count */
	f->channels = (((buffer[3] >> 6) & 0x03) + 1) % 4;

	/* Calculate length */
	if(f->layer == 0)
	{
		/* Layer I */
		f->length = ((12 * f->bitrate * 1000 / f->samplerate) +
			    f->padding) * 4;
	}
	else
	{
		/* Layer II or III */
		f->length = (144 * f->bitrate * 1000 / f->samplerate) +
			     f->padding;
	}

	return 0;
}

#define READ32(b) (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; b += 4;
#define READ16(b) (b[0] << 8) | b[1]; b += 2;

static int file_mp3_parse_xing(struct mp3_frame *f, unsigned char *buffer,
			       size_t len, struct mp3_demux *d)
{
	unsigned int offset;
	unsigned char flags;

	/* Needs all frame */
	if(f->length > len)
		return -1;

	/* Calculate header position */
	if(f->channels == 0)
	{
		offset = 13;
		if(f->mpeg == 0)
			offset += 8;
	}
	else
	{
		offset = 21;
		if(f->mpeg == 0)
			offset += 15;
	}

	/* Check header ID */
	if(offset > f->length ||
	   strncasecmp((char*)&buffer[offset], "Xing", 4) != 0 ||
	   strncasecmp((char*)&buffer[offset], "Info", 4) != 0)
		return -1;

	/* Set buffer ptr */
	buffer += offset;

	/* Get flags */
	flags = (unsigned char) READ32(buffer);

	/* Number of Frames */
	if(flags & 0x0001)
		d->nb_frame  = READ32(buffer);

	/* Number of Bytes */
	if(flags & 0x0002)
		d->nb_bytes  = READ32(buffer);

	/* TOC entries */
	if(flags & 0x0004)
	{
		/* Copy TOC */
		buffer += 100;
	}

	/* Quality indicator */
	if(flags & 0x0008)
		d->quality  = READ32(buffer);

	return 0;
}

static int file_mp3_parse_vbri(struct mp3_frame *f, unsigned char *buffer,
			       size_t len, struct mp3_demux *d)
{
	unsigned int frames_toc;
	unsigned int scale_toc;
	unsigned int size_toc;
	unsigned int nb_toc;
	unsigned int version;
	unsigned int delay;

	/* Needs all frame */
	if(f->length > len)
		return -1;

	/* Go to VBRI header */
	buffer += 36;

	/* Check header ID */
	if(strncasecmp((char*)buffer, "VBRI", 4) != 0)
		return -1;

	/* Update buffer position */
	buffer += 4;

	/* Get version ID */
	version = READ16(buffer);

	/* Get delay */
	delay = READ16(buffer);

	/* Quality indicator */
	d->quality = READ16(buffer);

	/* Number of Bytes */
	d->nb_bytes = READ32(buffer);

	/* Number of Frames */
	d->nb_frame = READ32(buffer);

	/* Number of entries within TOC */
	nb_toc = READ16(buffer);

	/* Scale factor of TOC */
	scale_toc = READ16(buffer);

	/* Size per table entry in bytes (max 4) */
	size_toc = READ16(buffer);
	if(size_toc > 4)
		return -1;

	/* Frames per table entry */
	frames_toc = READ16(buffer);

	/* TOC entries */

	return 0;
}

static int file_mp3_parse_lame(struct mp3_frame *f, unsigned char *buffer,
			       size_t len, struct mp3_demux *d)
{
	/* Not yet implemented */
	return -1;
}

int file_mp3_init(struct file_handle *h, unsigned long *samplerate,
		  unsigned char *channels)
{
	struct mp3_frame frame;
	struct mp3_demux *d;
	unsigned long size;
	long first = -1;
	int i;

	/* Read 10 first bytes for ID3 header */
	if(file_read_input(h, 10) != 10)
		return -1;

	/* Check ID3V2 tag */
	if(memcmp(h->in_buffer, "ID3", 3) == 0)
	{
		/* Get ID3 size */
		size = (h->in_buffer[6] << 21) | (h->in_buffer[7] << 14) |
		       (h->in_buffer[8] << 7) | h->in_buffer[9];
		size += 10;

		/* Add footer size */
		if(h->in_buffer[5] & 0x20)
			size += 10;

		/* Skip ID3 in file */
		file_seek_input(h, size, SEEK_CUR);
	}

	/* Complete input buffer */
	file_complete_input(h, 0);

	/* Sync to first frame */
	for(i = 0; i < h->in_size-3; i++)
	{
		if(h->in_buffer[i] == 0xFF &&
		   (h->in_buffer[i+1] & 0xE0) == 0xE0)
		{
			/* Check header */
			if(file_mp3_parse_header(&h->in_buffer[i], 4, &frame)
			   != 0)
				continue;

			/* Check next frame */
			if(i + frame.length + 2 > h->in_size ||
			   h->in_buffer[i+frame.length] != 0xFF ||
			   (h->in_buffer[i+frame.length+1] & 0xE0) != 0xE0)
				continue;

			first = i;
			break;
		}
	}
	if(first < 0)
		return -1;

	/* Allocate demux data structure */
	h->demux_data = malloc(sizeof(struct mp3_demux));
	if(h->demux_data == NULL)
		return -1;
	d = h->demux_data;
	memset(d, 0, sizeof(struct mp3_demux));

	/* Move to first frame */
	file_seek_input(h, first, SEEK_CUR);
	file_complete_input(h, 0);

	/* Parse Xing/Lame/VBRI header */
	if(file_mp3_parse_xing(&frame, h->in_buffer, h->in_size, d) == 0 ||
	   file_mp3_parse_lame(&frame, h->in_buffer, h->in_size, d) == 0 ||
	   file_mp3_parse_vbri(&frame, h->in_buffer, h->in_size, d) == 0)
	{
		/* Move to next frame */
		first += frame.length;
		file_seek_input(h, first, SEEK_CUR);
	}

	/* Update position of stream */
	d->offset = first;

	/* Update samplerate and channels */
	*samplerate = frame.samplerate;
	if(frame.channels == 0)
		*channels = 1;
	else
		*channels = 2;

	/* Update decoder config */
	h->decoder_config = NULL;
	h->decoder_config_size = 0;

	return 0;
}

int file_mp3_get_next_frame(struct file_handle *h)
{
	return file_complete_input(h, 0);
}

int file_mp3_set_pos(struct file_handle *h, unsigned long pos)
{
	struct mp3_demux *d = h->demux_data;
	unsigned long f_pos;

	/* Calculate new position */
	f_pos = (h->file_size - d->offset) * pos / h->length;
	if(f_pos > h->file_size)
		return -1;

	/* Seek in file */
	if(file_seek_input(h, f_pos, SEEK_SET) != 0)
		return -1;

	/* Update position */
	h->pos = pos * h->samplerate * h->channels;

	return 0;
}

void file_mp3_free(struct file_handle *h)
{
	if(h == NULL || h->demux_data == NULL)
		return;

	free(h->demux_data);
	h->demux_data = NULL;
}

struct file_demux file_mp3_demux = {
	.init = &file_mp3_init,
	.get_next_frame = &file_mp3_get_next_frame,
	.set_pos = &file_mp3_set_pos,
	.free = &file_mp3_free,
};