plugin_LTLIBRARIES = libgstrtspsink.la

libgstrtspsink_la_SOURCES = gstrtspsink.c plugin.c
			
libgstrtspsink_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) $(GST_CFLAGS) $(GIO_CFLAGS)

# FIXME: Hack to avoid having to add GETTEXT_PACKAGE to gst-rtsp
libgstrtspsink_la_CFLAGS += -D"GETTEXT_PACKAGE=gst-rtsp-server-1.0"

libgstrtspsink_la_LIBADD = $(top_builddir)/gst/rtsp-server/libgstrtspserver-@GST_API_VERSION@.la  \
			$(GST_PLUGINS_BASE_LIBS) $(GST_BASE_LIBS) \
		       -lgstrtp-@GST_API_VERSION@ -lgstrtsp-@GST_API_VERSION@ \
		       -lgstsdp-@GST_API_VERSION@ $(GST_NET_LIBS) $(GST_LIBS) \
			$(GIO_LIBS)
libgstrtspsink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstrtspsink_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

noinst_HEADERS = gstrtspsink.h
