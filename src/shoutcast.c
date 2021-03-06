/*
 * shoutcast.c - A ShoutCast Client
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
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#include "http.h"
#include "decoder.h"
#include "shoutcast.h"
#include "vring.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/**
 * Cache settings
 *  DEFAULT_CACHE_SIZE: Default ache size (in s).
 *  DEFAULT_BITRATE: Default bitrate when icy-br is not available (in kb/s).
 *  MIN_CACHE_LEN: Minimum length of data in cache before buffering.
 *  MAX_RW_SIZE: Maximum read/write for ring buffer (see vring.h).
 * The default bitrate is set to highest bitrate possible (even if AAC can 
 * support higher).
 * Maximum read/write size must be upper than biggest frame size.
 */
#define DEFAULT_CACHE_SIZE 1
#define DEFAULT_BITRATE 320
#define MIN_CACHE_LEN 2048
#define MAX_RW_SIZE 8192

/**
 * Synchronization settings: minimum size needed for stream synchronization on
 * first frame. (Best value is 2x the maximum frame size + frame header)
 *  SYNC_TOTAL_TIMEOUT: total timeout for synchronization (must be a multiple 
 *                      of SYNC_TIMEOUT. This value is in s).
 *  SYNC_TIMEOUT: timeout for HTTP read (in ms).
 */
#define MP3_SYNC_SIZE (2881 * 2) + 3
#define AAC_SYNC_SIZE MAX_RW_SIZE
#define SYNC_TOTAL_TIMEOUT 5
#define SYNC_TIMEOUT 1

/**
 * All synchronize size must be lower or equal to minimum cache length
 */
#if (AAC_SYNC_SIZE < MIN_CACHE_LEN) || (MP3_SYNC_SIZE < MIN_CACHE_LEN)
#error MIN_CACHE_LEN must be greater than all syncronization size
#endif

/**
 * Thread timeout in ms.
 */
#define THREAD_TIMEOUT 100

/**
 * Pause buffer settings:
 *  BLOCK_SIZE: basic block size.
 *  CHECK_POOL: pool is check every CHECK_POOL seconds and only two block are 
 *              kept. Others are freed.
 */
#define BLOCK_SIZE 8192
#define CHECK_POOL 30

/**
 * State of metadata demultiplexing in stream
 */
enum shout_state {
	SHOUT_DATA,	/*!< Stream data: frames */
	SHOUT_META_LEN,	/*!< Length part of metadata field */
	SHOUT_META_DATA /*!< Data part of metadata field */
};

/**
 * Shoutcast data cache: used for metadata cache and pause buffer
 */
struct shout_data {
	struct shout_data *next;	/*!< Next data block in cache */
	size_t remaining;		/*!< Remaining bytes before next
					     data block */
	char data[0];			/*!< Data block */
};

/**
 * Shoutcast handler
 */
struct shout_handle {
	/* HTTP Client */
	struct http_handle *http;	/*!< HTTP client handler */
	/* Input ring buffer: cache */
	struct vring_handle *ring;	/*!< Ring buffer cache */
	unsigned long cache_len;	/*!< Cache size in seconds */
	size_t cache_size;		/*!< Cache size in bytes */
	int is_ready;			/*!< Flag for cache status */
	struct shout_data *metas;	/*!< Metadata cache (first) */
	struct shout_data *metas_last;	/*!< Last metadata in cache */
	/* Pause buffer */
	struct shout_data *pauses;	/*!< First block in pause buffer */
	struct shout_data *pauses_last;	/*!< Last block in pause buffer */
	struct shout_data *pool;	/*!< Current pause buffer block */
	struct shout_data *pool_last;	/*!< Last pause buffer block in pool */
	unsigned long pause_count;	/*!< Count of pause buffer block */
	int is_paused;			/*!< Stream is paused: buffering */
	struct timeval start_pause;	/*!< Timestamp of pause start */
	unsigned long pause_len;	/*!< Duration of pause buffer (ms) */
	time_t last_pool_check;		/*!< Last pool check */
	size_t skip_size;		/*!< Len to skip in pause buffer */
	int end_pause;			/*!< End of stream during pause */
	/* Metadata handling */
	enum shout_state state;		/*!< State of stream demultiplexing */
	unsigned int metaint;		/*!< Bytes between two meta data */
	unsigned int remaining;		/*!< Remaining bytes before next
					     state */
	int meta_len;			/*!< Read length from current meta data 
					     field */
	int meta_size;			/*!< Size of current meta data field */
	struct shout_data *meta;	/*!< Current metadata */
	/* Radio info */
	struct radio_info info;		/*!< Radio information */
	/* Decoder and stream properties */
	struct decoder_handle *dec;	/*!< Decoder handler */
	unsigned long samplerate;	/*!< Output samplerate of stream */
	unsigned char channels;		/*!< Channel number of stream */
	/* PCM output cache */
	unsigned long pcm_remaining;	/*!< Bytes remaining in decoder output 
					     buffer */
	/* Event callback */
	shoutcast_event_cb event_cb;	/*!< Callback for event */
	void *event_udata;		/*!< User data for event callback */
	/* Synchrinzation */
	ssize_t (*sync_fn)(struct shout_handle *, unsigned char *, size_t);
					/*!< Callback for synchronization */
	size_t sync_size;		/*!< Minimal size for synchronization */
	int resync;			/*!< Stream must be synchronized */
	/* Internal thread */
	int use_thread;			/*!< Internal thread usage */
	int stop;			/*!< Stop signal for thread */
	pthread_t thread;		/*!< Internal thread */
	pthread_mutex_t mutex;		/*!< Mutex for thread */
	pthread_mutex_t meta_mutex;		/*!< Mutex for metadata */
	pthread_mutex_t pause_mutex;		/*!< Mutex for pause buffer */
};

static inline int shoutcast_sync(struct shout_handle *h);
static ssize_t shoutcast_sync_mp3_stream(struct shout_handle *h,
					 unsigned char *buffer, size_t in_len);
static ssize_t shoutcast_sync_aac_stream(struct shout_handle *h,
					 unsigned char *buffer, size_t in_len);
static ssize_t shoutcast_fill_buffer(struct shout_handle *h,
				     unsigned long timeout);
static ssize_t shoutcast_get_buffer(struct shout_handle *h,
				    unsigned char **buffer);
static ssize_t shoutcast_forward_buffer(struct shout_handle *h, size_t size);
static void *shoutcast_thread(void *user_data);

int shoutcast_open(struct shout_handle **handle, const char *url,
		   unsigned long cache_size, int use_thread)
{
	enum a_codec type = CODEC_NO;
	struct shout_handle *h;
	unsigned char *buffer;
	ssize_t len = 0;
	int code = 0;
	char *p;

	/* Allocate handler */
	*handle = malloc(sizeof(struct shout_handle));
	if(*handle == NULL)
		return -1;
	h = *handle;

	/* Set to zero the handler */
	memset(h, 0, sizeof(struct shout_handle));

	/* Init structure */
	h->cache_len = cache_size > 0 ? cache_size : DEFAULT_CACHE_SIZE;
	h->state = SHOUT_DATA;
	h->is_ready = 1;

	/* Init thread mutex */
	pthread_mutex_init(&h->mutex, NULL);
	pthread_mutex_init(&h->meta_mutex, NULL);
	pthread_mutex_init(&h->pause_mutex, NULL);

	/* Init HTTP client */
	if(http_open(&h->http, 1) != 0)
	{
		shoutcast_close(h);
		return -1;
	}

	/* Set options */
	http_set_option(h->http, HTTP_EXTRA_HEADER, "Icy-MetaData: 1\r\n", 0);
	http_set_option(h->http, HTTP_FOLLOW_REDIRECT, NULL, 1);

	/* Connect and get header from server */
	code = http_get(h->http, url);
	if(code != 200)
		return -1;

	/* Fill info radio structure */
	h->info.description = http_get_header(h->http, "icy-description", 0);
	h->info.genre = http_get_header(h->http, "icy-genre", 0);
	h->info.name = http_get_header(h->http, "icy-name", 0);
	h->info.url = http_get_header(h->http, "icy-url", 0);
	p = http_get_header(h->http, "icy-br", 0);
	if(p != NULL)
		h->info.bitrate = atoi(p);
	p = http_get_header(h->http, "icy-pub", 0);
	if(p != NULL)
		h->info.pub = atoi(p);
	p = http_get_header(h->http, "icy-private", 0);
	if(p != NULL)
		h->info.private = atoi(p);
	p = http_get_header(h->http, "icy-metaint", 0);
	if(p != NULL)
		h->info.metaint = atoi(p);
	/* TODO: ice-audio-info: */
	p = http_get_header(h->http, "content-type", 0);
	if(p != NULL)
	{
		if(strncmp(p, "audio/mpeg", 10) == 0)
		{
			h->info.type = MPEG_STREAM;
			type = CODEC_MP3;
		}
		else if(strncmp(p, "audio/aac", 9) == 0)
		{
			h->info.type = AAC_STREAM;
			type = CODEC_AAC;
		}
		else
		{
			h->info.type = NONE_STREAM;
			return -1;
		}
	}
	else
		return -1;

	/* Update metaint with extracted info */
	h->metaint = h->info.metaint;
	h->remaining = h->metaint;

	/* Calculate input buffer size */
	h->cache_size = h->cache_len * 1000;
	if(h->info.bitrate > 0)
		h->cache_size *= h->info.bitrate / 8;
	else
		h->cache_size *= DEFAULT_BITRATE / 8;

	/* Create a ring buffer for input data */
	if(vring_open(&h->ring, h->cache_size, MAX_RW_SIZE) != 0)
		return -1;

	/* Synchronize to first frame in stream */
	if(shoutcast_sync(h) < 0)
		return -1;

	/* Get data buffer */
	len = shoutcast_get_buffer(h, &buffer);
	if(len <= 0)
		return -1;

	/* Open decoder */
	if(decoder_open(&h->dec, type, buffer, len, &h->samplerate,
			&h->channels) != 0)
		return -1;

	/* Set buffer not ready */
	h->is_ready = 0;

	/* Start internal thread */
	if(use_thread)
	{
		if(pthread_create(&h->thread, NULL, shoutcast_thread, h) != 0)
			return -1;
		h->use_thread = 1;
	}

	return 0;
}

static inline int shoutcast_sync(struct shout_handle *h)
{
	unsigned char *buffer;
	time_t now = time(NULL);
	ssize_t len = 0;

	/* Get setting for synchronization */
	switch(h->info.type)
	{
		case MPEG_STREAM:
			h->sync_size = MP3_SYNC_SIZE;
			h->sync_fn = shoutcast_sync_mp3_stream;
			break;
		case AAC_STREAM:
			h->sync_size = AAC_SYNC_SIZE;
			h->sync_fn = shoutcast_sync_aac_stream;
			break;
		default:
			return -1;
	}

	/* Fill as possible */
	while(len < h->sync_size && time(NULL) - now < SYNC_TOTAL_TIMEOUT)
		len = shoutcast_fill_buffer(h, SYNC_TIMEOUT);

	/* Get buffer */
	len = shoutcast_get_buffer(h, &buffer);
	if(len <= 0)
		return -1;

	/* Find first frame in stream */
	len = h->sync_fn(h, buffer, len);
	if(len < 0)
		return -1;

	/* Forward to first frame */
	shoutcast_forward_buffer(h, len);

	/* Complete buffer as possible */
	len = shoutcast_get_buffer(h, &buffer);
	while(len < h->sync_size && time(NULL) - now < SYNC_TOTAL_TIMEOUT)
		len = shoutcast_fill_buffer(h, SYNC_TIMEOUT);

	return 0;
}

unsigned long shoutcast_get_samplerate(struct shout_handle *h)
{
	if(h == NULL)
		return 0;

	return h->samplerate;
}

unsigned char shoutcast_get_channels(struct shout_handle *h)
{
	if(h == NULL)
		return 0;

	return h->channels;
}

static ssize_t shoutcast_sync_mp3_stream(struct shout_handle *h,
					 unsigned char *buffer, size_t in_len)
{
	unsigned int bitrates[2][3][15] = {
		{ /* MPEG-1 */
			{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352,
			 384, 416, 448},
			{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224,
			 256, 320, 384},
			{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224,
			 256, 320}
		},
		{ /* MPEG-2 LSF, MPEG-2.5 */
			{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176,
			 192, 224, 256},
			{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
			 144, 160},
			{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
			 144, 160}
		}
	};
	unsigned int samplerates[3][4] = {
		{44100, 48000, 32000, 0},
		{22050, 24000, 16000, 0},
		{11025, 8000, 8000, 0}
	};
	int mp, mpeg, layer, padding;
	unsigned long samplerate;
	unsigned int bitrate;
	int tmp, len, i;

	/* Sync input buffer to first MP3 header */
	for(i = 0; i < in_len - 3; i++)
	{
		if(buffer[i] == 0xFF && buffer[i+1] != 0xFF &&
		   (buffer[i+1] & 0xE0) == 0xE0)
		{
			/* Get Mpeg version */
			mpeg = 3 - ((buffer[i+1] >> 3) & 0x03);
			mp = mpeg;
			if(mpeg == 2)
				continue;
			if(mpeg == 3)
			{
				mpeg = 2;
				mp = 1;
			}

			/* Get Layer */
			layer = 3 - ((buffer[i+1] >> 1) & 0x03);
			if(layer == 3)
				continue;

			/* Get bitrate */
			tmp = (buffer[i+2] >> 4) & 0x0F;
			if(tmp == 0 || tmp == 15)
				continue;
			bitrate = bitrates[mp][layer][tmp];

			/* Get samplerate */
			tmp = (buffer[i+2] >> 2) & 0x03;
			if(tmp == 3)
				continue;
			samplerate = samplerates[mpeg][tmp];

			/* Get padding */
			padding = (buffer[i+2] >> 1) & 0x01;

			/* Calculate length */
			if(layer == 0)
			{
				/* Layer I */
				len = ((12 * bitrate * 1000 / samplerate) +
				       padding) * 4;
			}
			else if(mpeg > 0 && layer == 2)
			{
				/* MPEG 2 and 2.5 in layer III */
				len = (72 * bitrate * 1000 / samplerate) +
				      padding;
			}
			else
			{
				/* Layer II or III */
				len = (144 * bitrate * 1000 / samplerate) +
				      padding;
			}

			/* Check presence of next frame */
			if(i + len + 2 > in_len || buffer[i+len] != 0xFF ||
			   buffer[i+len+1] == 0xFF ||
			   (buffer[i+len+1] & 0xE0) != 0xE0)
				continue;

			return i;
		}
	}

	return -1;
}

static ssize_t shoutcast_sync_aac_stream(struct shout_handle *h,
					 unsigned char *buffer, size_t in_len)
{
	int len, i;

	/* Sync input buffer to first ADTS header */
	for(i = 0; i < in_len - 5; i++)
	{
		if(buffer[i] == 0xFF && (buffer[i+1] & 0xF6) == 0xF0)
		{
			/* Get frame length */
			len = ((buffer[i+3] & 0x3) << 11) | (buffer[i+4] << 3) |
			      (buffer[i+5] >> 5);

			/* Check next frame */
			if(i + len + 2 > in_len || buffer[i+len] != 0xFF ||
			   (buffer[i+len+1] & 0xF6) != 0xF0)
				continue;

			return i;
		}
	}

	return -1;
}

static void *shoutcast_thread(void *user_data)
{
	struct shout_handle *h = user_data;
	ssize_t len;

	/* Fill buffer until end */
	while(!h->stop)
	{
		/* Fill cache from stream */
		len = shoutcast_fill_buffer(h, THREAD_TIMEOUT);
		if(len < 0)
			break;
	}

	/* End of stream */
	h->stop = 1;

	return NULL;
}

int shoutcast_read(void *user_data, unsigned char *buffer, size_t size,
		   struct a_format *fmt)
{
	struct shout_handle *h = (struct shout_handle *) user_data;
	struct decoder_info info;
	unsigned char *in_buffer;
	int total_samples = 0;
	ssize_t len = 0;
	int samples;

	if(h == NULL)
		return -1;

	/* Process remaining pcm data */
	if(h->pcm_remaining > 0)
	{
		/* Get remaining pcm data from decoder */
		samples = decoder_decode(h->dec, NULL, 0, buffer, size, &info);
		if(samples < 0)
			return -1;

		/* Update audio format */
		if(info.samplerate != h->samplerate ||
		   info.channels != h->channels)
		{
			h->samplerate = info.samplerate;
			h->channels = info.channels;
		}

		h->pcm_remaining -= samples;
		total_samples += samples;
	}

	/* Fill output buffer */
	while(total_samples < size)
	{
		/* Fill input buffer as possible */
		if(!h->use_thread)
			len = shoutcast_fill_buffer(h, 0);

		/* Lock pause buffer access */
		pthread_mutex_lock(&h->pause_mutex);

		/* Get data */
		if((!h->is_ready && !h->resync) || h->is_paused ||
		   (len = shoutcast_get_buffer(h, &in_buffer)) <= 0)
		{
			/* Unlock pause buffer access */
			pthread_mutex_unlock(&h->pause_mutex);
			break;
		}

		/* Resynchronize stream */
		if(h->resync)
		{
			/* Not enough data or failed to synchronize */
			if(len < h->sync_size ||
			   (len = h->sync_fn(h, in_buffer, len)) < 0)
			{
				/* Unlock pause buffer access */
				pthread_mutex_unlock(&h->pause_mutex);
				break;
			}

			/* Update stream position */
			shoutcast_forward_buffer(h, len);
			h->resync = 0;

			/* Unlock pause buffer access */
			pthread_mutex_unlock(&h->pause_mutex);
			continue;
		}

		/* Decode next frame */
		samples = decoder_decode(h->dec, in_buffer, len > 0 ? len : 0,
					 &buffer[total_samples * 4],
					 size - total_samples, &info);
		if(samples <= 0)
		{
			/* Forward used bytes in ring buffer */
			if(len > 0 && info.used > 0)
				shoutcast_forward_buffer(h, info.used);

			/* Unlock pause buffer access */
			pthread_mutex_unlock(&h->pause_mutex);
			break;
		}

		/* Forward ring buffer to next frame */
		shoutcast_forward_buffer(h, info.used);

		/* Unlock pause buffer access */
		pthread_mutex_unlock(&h->pause_mutex);

		/* Update remaining counter */
		h->pcm_remaining = info.remaining;

		/* Check audio format */
		if(info.samplerate != h->samplerate ||
		   info.channels != h->channels)
		{
			/* Reset internal position */
			decoder_decode(h->dec, NULL, 0, NULL, 0, NULL);
			h->pcm_remaining += samples;
			break;
		}

		/* Update samples returned */
		total_samples += samples;
	}

	/* Fill audio format */
	if(fmt != NULL)
	{
		fmt->samplerate = h->samplerate;
		fmt->channels = h->channels;
	}

	/* End of stream */
	if((len < 0 || h->stop) && total_samples <= 0)
	{
		/* Set end of stream */
		h->stop = 1;

		/* Lock event access */
		pthread_mutex_lock(&h->mutex);

		/* Notify end of stream */
		if(h->event_cb != NULL)
			h->event_cb(h->event_udata, SHOUT_EVENT_END, NULL);

		/* Unlock event access */
		pthread_mutex_unlock(&h->mutex);

		return -1;
	}

	return total_samples;
}

const struct radio_info *shoutcast_get_info(struct shout_handle *h)
{
	return &h->info;
}

char *shoutcast_get_metadata(struct shout_handle *h)
{
	char *str = NULL;

	/* Lock meta string access */
	pthread_mutex_lock(&h->meta_mutex);

	/* Copy current metadata */
	if(h->metas != NULL)
		str = strdup(h->metas->data);

	/* Unlock meta string access */
	pthread_mutex_unlock(&h->meta_mutex);

	return str;
}

enum shout_status shoutcast_get_status(struct shout_handle *h)
{
	if(h->is_paused)
		return SHOUT_PAUSED;
	else if(h->stop)
		return SHOUT_STOPPED;
	else if(!h->is_ready)
		return SHOUT_BUFFERING;
	return SHOUT_PLAYING;
}

int shoutcast_get_filling(struct shout_handle *h)
{
	/* Stream buffering: calculate ratio in % */
	if(!h->is_ready)
		return vring_get_length(h->ring) * 100 / h->cache_size;
	return 100;
}

int shoutcast_play(struct shout_handle *h)
{
	/* Update pause duration */
	shoutcast_get_pause(h);

	/* Lock pause buffer access */
	pthread_mutex_lock(&h->pause_mutex);

	/* Play stream */
	if(h->is_paused)
		h->is_paused = 0;

	/* Unlock pause buffer access */
	pthread_mutex_unlock(&h->pause_mutex);

	return 0;
}

int shoutcast_pause(struct shout_handle *h)
{
	/* Lock pause buffer access */
	pthread_mutex_lock(&h->pause_mutex);

	/* Pause stream */
	if(!h->is_paused)
	{
		h->is_paused = 1;
		h->is_ready = 0;
		gettimeofday(&h->start_pause, NULL);
	}

	/* Unlock pause buffer access */
	pthread_mutex_unlock(&h->pause_mutex);

	return 0;
}

unsigned long shoutcast_get_pause(struct shout_handle *h)
{
	struct timeval now;
	unsigned long len;

	/* Lock pause buffer access */
	pthread_mutex_lock(&h->pause_mutex);

	if(h->is_paused && !h->end_pause)
	{
		/* Add current pause time */
		gettimeofday(&now, NULL);
		h->pause_len += ((now.tv_sec - h->start_pause.tv_sec) * 1000) +
			     ((now.tv_usec - h->start_pause.tv_usec) / 1000);
		memcpy(&h->start_pause, &now, sizeof(struct timeval));
	}

	/* Copy len */
	len = h->pause_len;

	/* Unlock pause buffer access */
	pthread_mutex_unlock(&h->pause_mutex);

	return len;
}

unsigned long shoutcast_skip(struct shout_handle *h, unsigned long skip)
{
	/* Update pause duration */
	shoutcast_get_pause(h);

	/* Lock pause buffer access */
	pthread_mutex_lock(&h->pause_mutex);

	/* Update skip value */
	if(h->pause_len < skip)
		skip = h->pause_len;

	/* Update skip size if pause buffer is bigger */
	if(h->pause_len > 0)
	{
		h->skip_size += skip * (((h->pause_count + 1) * BLOCK_SIZE) -
				h->skip_size) / h->pause_len;
		h->pause_len -= skip;
		h->is_ready = 0;
	}

	/* Unlock pause buffer access */
	pthread_mutex_unlock(&h->pause_mutex);

	return skip;
}

void shoutcast_reset(struct shout_handle *h)
{
	/* Lock pause buffer access */
	pthread_mutex_lock(&h->pause_mutex);

	/* Reset pause buffer */
	h->skip_size = ~1L;
	h->is_ready = 0;

	/* Unlock pause buffer access */
	pthread_mutex_unlock(&h->pause_mutex);
}

int shoutcast_set_event_cb(struct shout_handle *h, shoutcast_event_cb cb,
			   void *user_data)
{
	/* Lock event access */
	pthread_mutex_lock(&h->mutex);

	/* Set event callback */
	h->event_cb = cb;
	h->event_udata = user_data;

	/* Unlock event access */
	pthread_mutex_unlock(&h->mutex);

	return 0;
}

int shoutcast_close(struct shout_handle *h)
{
	struct shout_data *m;

	if(h == NULL)
		return 0;

	/* Stop internal thread */
	if(h->use_thread)
	{
		h->stop = 1;
		pthread_join(h->thread, NULL);
	}

	/* Close decoder */
	if(h->dec != NULL)
		decoder_close(h->dec);

	/* Close HTTP */
	if(h->http != NULL)
		http_close(h->http);

	/* Close and free ring buffer */
	if(h->ring != NULL)
		vring_close(h->ring);

	/* Free meta cache */
	while(h->metas != NULL)
	{
		m = h->metas;
		h->metas = m->next;
		free(m);
	}
	if(h->meta != NULL)
		free(h->meta);

	/* Free pause buffer */
	while(h->pauses != NULL)
	{
		m = h->pauses;
		h->pauses = m->next;
		free(m);
	}
	while(h->pool != NULL)
	{
		m = h->pool;
		h->pool = m->next;
		free(m);
	}

	/* Free handler */
	free(h);

	return 0;
}

static ssize_t shoutcast_read_stream(struct shout_handle *h,
				     unsigned char *buffer, size_t size,
				     unsigned long timeout, int skip)
{
	struct shout_data *b, *b2;
	ssize_t len;
	size_t pos;
	size_t r_len = 0;
	int eos = 0;

	/* No pause has been performed */
	if(buffer != NULL && h->pauses == NULL && h->pool == NULL)
		return http_read_timeout(h->http, buffer, size, timeout);

	/* Fill pause buffer */
	do {
		/* Skipping: do not retrieve data from HTTP stream */
		if(skip)
			break;

		/* No block is ready for filling */
		if(h->pool == NULL)
		{
			/* Allocate a block */
			h->pool = malloc(sizeof(struct shout_data)+BLOCK_SIZE);
			if(h->pool == NULL)
				break;
			h->pool->remaining = BLOCK_SIZE;
			h->pool->next = NULL;
			h->pool_last = h->pool;
		}

		/* Get data from HTTP stream */
		pos = BLOCK_SIZE - h->pool->remaining;
		len = http_read_timeout(h->http,
					(unsigned char*) h->pool->data + pos,
					h->pool->remaining, 0);
		if(len <= 0)
		{
			/* Sleep the timeout */
			usleep(timeout*1000);

			/* End of stream */
			if(len < 0)
				eos = 1;
			break;
		}

		/* Update remaining space in block */
		h->pool->remaining -= len;
		if(h->pool->remaining == 0)
		{
			/* Remove block from pool */
			b = h->pool;
			h->pool = b->next;
			if(h->pool == NULL)
				h->pool_last = NULL;

			/* Add block to buffer pause */
			if(h->pauses_last != NULL)
				h->pauses_last->next = b;
			else
				h->pauses = b;
			h->pauses_last = b;
			b->remaining = BLOCK_SIZE;
			b->next = NULL;
			h->pause_count++;
		}
	} while(h->pool != NULL);

	/* Stream is paused: do not return data */
	if(buffer == NULL)
		return eos;

	/* Skipping: flush pool */
	if(skip && h->pool != NULL)
	{
		/* Some data are remaining */
		if(h->pool->remaining < BLOCK_SIZE)
		{
			/* Remove from pool */
			b = h->pool;
			h->pool = b->next;
			if(h->pool == NULL)
				h->pool_last = NULL;

			/* Add to pause buffer */
			if(h->pauses_last != NULL)
				h->pauses_last->next = b;
			else
				h->pauses = b;
			h->pauses_last = b;

			/* Move data to end of block */
			len = BLOCK_SIZE - b->remaining;
			memmove(b->data + b->remaining, b->data, len);
			b->remaining = len;
			b->next = NULL;
			h->pause_count++;
		}

		/* Free all other blocks in pool */
		while(h->pool != NULL)
		{
			b = h->pool;
			h->pool = b->next;
			free(b);
		}
		h->pool_last = NULL;
	}

	/* Get data from pause buffer */
	while(h->pauses != NULL && size > 0)
	{
		/* Copy data from pause buffer */
		b = h->pauses;
		pos = BLOCK_SIZE - b->remaining;
		len = size > b->remaining ? b->remaining : size;
		memcpy(buffer + r_len, b->data + pos, len);

		/* Update block values */
		b->remaining -= len;
		r_len += len;
		size -= len;

		/* Block is empty: get next */
		if(b->remaining == 0)
		{
			/* Remove block from pause buffer */
			h->pauses = b->next;
			if(h->pauses == NULL)
				h->pauses_last = NULL;

			/* Update block and set to pause block */
			b->remaining = BLOCK_SIZE;
			b->next = NULL;
			if(!skip)
			{
				/* Add block to pool */
				if(h->pool_last != NULL)
					h->pool_last->next = b;
				else
					h->pool = b;
				h->pool_last = b;
			}
			else
				free(b);
			h->pause_count--;

			/* Check pool size */
			if(h->last_pool_check + CHECK_POOL <= time(NULL) &&
			   !skip)
			{
				/* Update last check pool timer */
				h->last_pool_check = time(NULL);

				/* Free all blocks which are useless */
				if(h->pool != NULL && h->pool->next != NULL)
				{
					/* Keep only two block */
					b = h->pool->next->next;
					h->pool_last = h->pool->next;
					h->pool_last->next = NULL;

					/* Free all blocks */
					while(b != NULL)
					{
						b2 = b;
						b = b->next;
						free(b2);
					}
				}
			}
		}
	}

	/* End of stream */
	if(eos && r_len == 0)
		return -1;

	return r_len;
}

static ssize_t shoutcast_fill_buffer(struct shout_handle *h,
				     unsigned long timeout)
{
	unsigned char *buffer;
	int skip = 0;
	ssize_t size;
	ssize_t len;

	/* Lock pause buffer access */
	pthread_mutex_lock(&h->pause_mutex);

	/* Skipping time in pause buffer */
	if(h->skip_size > 0)
		skip = 1;

	/* Copy current stream status */
	if(h->is_paused && !skip)
	{
		/* Unlock pause buffer access */
		pthread_mutex_unlock(&h->pause_mutex);

		/* Fill pause buffer */
		len = shoutcast_read_stream(h, NULL, 0, timeout, 0);

		/* End of stream */
		if(len != 0)
		{
			/* Update pause */
			shoutcast_get_pause(h);
			h->end_pause = 1;
		}

		return 0;
	}

	/* Unlock pause buffer access */
	pthread_mutex_unlock(&h->pause_mutex);

	/* Fill cache */
	do
	{
		/* Lock pause buffer access */
		pthread_mutex_lock(&h->pause_mutex);

		/* Forward in cache */
		while(h->skip_size > 0)
		{
			len = shoutcast_forward_buffer(h, h->skip_size);
			if(len <= 0)
				break;
			h->skip_size -= len;
		}

		/* Unlock pause buffer access */
		pthread_mutex_unlock(&h->pause_mutex);

		/* Get buffer write */
		size = vring_write(h->ring, &buffer);
		if(size <= 0)
		{
			/* Lock pause buffer access */
			pthread_mutex_lock(&h->pause_mutex);

			/* Cache is full */
			if(size == 0 && h->is_ready == 0 && !h->is_paused)
			{
				/* Lock event access */
				pthread_mutex_lock(&h->mutex);

				/* Notify cache is ready */
				if(h->event_cb != NULL)
					h->event_cb(h->event_udata,
						    SHOUT_EVENT_READY, NULL);

				/* Unlock event access */
				pthread_mutex_unlock(&h->mutex);

				/* Update cache status */
				h->is_ready = 1;
				if(skip)
					h->resync = 1;
			}

			/* Unlock pause buffer access */
			pthread_mutex_unlock(&h->pause_mutex);

			/* Wait timeout */
			usleep(timeout*1000);
			break;
		}
		else if(h->remaining > 0 && size > h->remaining)
			size = h->remaining;

		/* Read data from HTTP stream */
		len = shoutcast_read_stream(h, buffer, size, timeout, skip);
		if(len <= 0)
		{
			/* Lock pause buffer access */
			pthread_mutex_lock(&h->pause_mutex);

			/* Pause buffer is empty */
			if(skip && h->pauses == NULL)
			{
				h->pause_len = 0;
				h->skip_size = 0;
				h->resync = 1;
			}

			/* Unlock pause buffer access */
			pthread_mutex_unlock(&h->pause_mutex);

			if(len < 0)
				return -1;

			break;
		}

		/* No meta data */
		if(h->metaint == 0)
			continue;

		/* Update remaining length */
		h->remaining -= len;

		/* Process data */
		switch(h->state)
		{
			case SHOUT_DATA:
				/* Meta data field reached */
				if(h->remaining == 0)
				{
					h->remaining = 1;
					h->state = SHOUT_META_LEN;
				}

				/* Forward in ring buffer */
				vring_write_forward(h->ring, len);
				break;
			case SHOUT_META_LEN:
				/* Set meta data size */
				h->meta_size = buffer[0] * 16;
				h->meta_len = 0;

				/* Lock meta string access */
				pthread_mutex_lock(&h->meta_mutex);

				if(h->meta_size > 0)
				{
					h->remaining = h->meta_size;
					h->state = SHOUT_META_DATA;

					/* Allocate a new meta */
					h->meta = calloc(1,
						     sizeof(struct shout_data) +
						     h->meta_size);
				}
				else
				{
					h->remaining = h->metaint;
					h->state = SHOUT_DATA;

					/* Update bytes before next metadata */
					if(h->metas_last != NULL)
						h->metas_last->remaining +=
								     h->metaint;
				}

				/* Unlock meta string access */
				pthread_mutex_unlock(&h->meta_mutex);

				break;
			case SHOUT_META_DATA:
				/* Lock meta string access */
				pthread_mutex_lock(&h->meta_mutex);

				/* Copy data to metadata */
				if(h->meta != NULL)
					memcpy(h->meta->data+h->meta_len, 
					       buffer, len);
				h->meta_len += len;

				/* Meta data field end */
				if(h->remaining == 0)
				{
					h->remaining = h->metaint;
					h->state = SHOUT_DATA;

					/* Bad metadata */
					if(h->meta == NULL)
					{
						/* Unlock meta string access */
						pthread_mutex_unlock(
								&h->meta_mutex);
						break;
					}

					/* Update remaining bytes */
					h->meta->remaining = h->metaint;

					/* Add meta to cache */
					if(h->metas_last != NULL)
						h->metas_last->next = h->meta;
					else
						h->metas = h->meta;
					h->metas_last = h->meta;
					h->meta = NULL;
				}

				/* Unlock meta string access */
				pthread_mutex_unlock(&h->meta_mutex);

				break;
		}
	} while(!h->is_ready);

	return vring_get_length(h->ring);
}

static ssize_t shoutcast_get_buffer(struct shout_handle *h,
				    unsigned char **buffer)
{
	ssize_t len;

	/* Get data from ring buffer */
	len = vring_read(h->ring, buffer, 0, 0);
	if(len > 0 && len <= MIN_CACHE_LEN)
	{
		/* Lock event access */
		pthread_mutex_lock(&h->mutex);

		/* Notify cache is empty */
		if(h->event_cb != NULL && h->is_ready == 1)
			h->event_cb(h->event_udata, SHOUT_EVENT_BUFFERING,
				    NULL);

		/* Unlock event access */
		pthread_mutex_unlock(&h->mutex);

		h->is_ready = 0;
		len = 0;
	}

	return len;
}

static ssize_t shoutcast_forward_buffer(struct shout_handle *h, size_t size)
{
	struct shout_data *m;
	ssize_t len;

	/* Forward ring buffer */
	len = vring_read_forward(h->ring, size);

	/* Lock meta string access */
	pthread_mutex_lock(&h->meta_mutex);

	/* Update first meta */
	while(h->metas != NULL && size >= h->metas->remaining)
	{
		/* Go to next meta */
		size -= h->metas->remaining;
		h->metas->remaining = 0;

		/* No more metadata in cache */
		if(h->metas->next == NULL)
			break;

		/* Get next metadata in cache */
		m = h->metas;
		h->metas = m->next;
		free(m);

		/* Lock event access */
		pthread_mutex_lock(&h->mutex);

		/* Notify new metadata */
		if(h->event_cb != NULL)
			h->event_cb(h->event_udata, SHOUT_EVENT_META,
				    h->metas->data);

		/* Unlock event access */
		pthread_mutex_unlock(&h->mutex);
	}

	/* Update remaining bytes before next metadata */
	if(h->metas != NULL && size < h->metas->remaining)
		h->metas->remaining -= size;

	/* Unlock meta string access */
	pthread_mutex_unlock(&h->meta_mutex);

	return len;
}

