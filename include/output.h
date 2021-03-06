/*
 * output.h - Audio output module
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

#ifndef _OUTPUT_H
#define _OUTPUT_H

#include "format.h"

#define OUTPUT_VOLUME_MAX 65535

enum output_stream_key {
	/* Stream status (see enum stream_status) */
	OUTPUT_STREAM_STATUS,
	/* Time played on stream (in ms) */
	OUTPUT_STREAM_PLAYED,
	/* Stream cache status (see enum stream_cache_status) */
	OUTPUT_STREAM_CACHE_STATUS,
	/* Stream cache fill (in %) */
	OUTPUT_STREAM_CACHE_FILLING,
	/* Stream cache current delay (in ms) */
	OUTPUT_STREAM_CACHE_DELAY
};

enum stream_status {
	STREAM_PLAYING,
	STREAM_PAUSED,
	STREAM_ENDED
};

enum streamcache_status {
	CACHE_READY,
	CACHE_BUFFERING
};

struct output_handle;
struct output_stream_handle;

/* Global output volume control */
int output_set_volume(struct output_handle *h, unsigned int volume);
unsigned int output_get_volue(struct output_handle *h);

/* Add/Remove output stream */
struct output_stream_handle *output_add_stream(struct output_handle *h,
					       const char *name,
					       unsigned long samplerate,
					       unsigned char channels,
					       unsigned long cache,
					       int use_cache_thread,
					       a_read_cb input_callback,
					       void *user_data);
void output_remove_stream(struct output_handle *h,
			  struct output_stream_handle *s);

/* Play/Pause output stream */
int output_play_stream(struct output_handle *h, struct output_stream_handle *s);
int output_pause_stream(struct output_handle *h,
			struct output_stream_handle *s);
void output_flush_stream(struct output_handle *h,
			 struct output_stream_handle *s);

/* Write to stream */
ssize_t output_write_stream(struct output_handle *h, 
			    struct output_stream_handle *s,
			    const unsigned char *buffer, size_t size,
			    struct a_format *fmt);

/* Volume output stream control */
int output_set_volume_stream(struct output_handle *h,
			     struct output_stream_handle *s,
			     unsigned int volume);
unsigned int output_get_volume_stream(struct output_handle *h,
				      struct output_stream_handle *s);

/* Cache stream control */
int output_set_cache_stream(struct output_handle *h,
			    struct output_stream_handle *s,
			    unsigned long cache);

/* Output stream status */
unsigned long output_get_status_stream(struct output_handle *h,
				       struct output_stream_handle *s,
				       enum output_stream_key key);

/* Output stream event */
enum stream_event {
	STREAM_EVENT_READY,	/*!< Stream is ready to play (cache is full) */
	STREAM_EVENT_BUFFERING, /*!< Stream is buffering (cache not ready) */
	STREAM_EVENT_END	/*!< End of stream has been reached */
};
typedef void (*output_stream_event_cb)(void *user_data, enum stream_event event,
				       void *data);
int output_set_stream_event_cb(struct output_handle *h,
			       struct output_stream_handle *s,
			       output_stream_event_cb cb, void *user_data);

#endif
