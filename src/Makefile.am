bin_PROGRAMS = aircat

aircat_SOURCES = main.c \
		 modules.c \
		 config_file.c \
		 httpd.c \
		 avahi.c \
		 http.c \
		 fs/fs.c \
		 fs/fs_posix.c \
		 fs/fs_http.c \
		 fs/fs_smb.c \
		 demux/demux.c \
		 demux/demux_mp3.c \
		 demux/demux_mp4.c \
		 demux/id3.c \
		 file.c \
		 meta/meta.c \
		 shoutcast.c \
		 rtsp.c \
		 rtp.c \
		 sdp.c \
		 decoder/decoder.c \
		 decoder/decoder_pcm.c \
	 	 decoder/decoder_aac.c \
		 decoder/decoder_mp3.c \
		 decoder/decoder_alac.c \
		 outputs/outputs.c \
		 outputs/output_alsa.c \
		 resample.c \
		 cache.c \
		 db.c \
		 timers.c \
		 events.c \
		 vring.c \
		 utils.c

aircat_LDADD = $(libssl_LIBS) \
	       $(libmad_LIBS) \
	       $(libfaad_LIBS) \
	       $(libsoxr_LIBS) \
	       $(libasound2_LIBS) \
	       $(libavahi_LIBS) \
	       $(libmicrohttpd_LIBS) \
	       $(libjsonc_LIBS) \
	       $(libtag_LIBS) \
	       $(libsqlite_LIBS) \
	       $(libsmbclient_LIBS) \
	       -lpthread -ldl

aircat_LDFLAGS = -export-dynamic

aircat_CFLAGS = -DMODULES_PATH='"${modules_DIR}"' \
		 $(libssl_CFLAGS) \
		 $(libmad_CFLAGS) \
		 $(libsoxr_CFLAGS) \
		 $(libasound2_CFLAGS) \
		 $(libavahi_CFLAGS) \
		 $(libmicrohttpd_CFLAGS) \
		 $(libjsonc_CFLAGS) \
		 $(libsqlite_CFLAGS) \
		 $(libsmbclient_CFLAGS) \
		 -Wall

# C++ support and TagLib support
aircat_CPPFLAGS = -I$(top_srcdir)/include \
		  $(libtag_CFLAGS)
aircat_SOURCES += meta/meta_taglib.cpp \
		  meta/meta_taglib_file.cpp

EXTRA_DIST = modules.h \
	     outputs/outputs.h \
	     outputs/output_alsa.h \
	     fs/fs_posix.h \
	     fs/fs_http.h \
	     fs/fs_smb.h \
	     demux/demux_mp3.h \
	     demux/demux_mp4.h \
	     demux/id3.h \
	     meta/meta_taglib_file.h \
	     decoder/decoder_pcm.h \
	     decoder/decoder_aac.h \
	     decoder/decoder_mp3.h \
	     decoder/decoder_alac.h \
	     events.h \
	     timers.h


