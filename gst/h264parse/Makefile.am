plugin_LTLIBRARIES = libgstrrh264parse.la

# sources used to compile this plug-in
libgstrrh264parse_la_SOURCES = gstrrh264parse.c

libgstrrh264parse_la_CFLAGS = \
	$(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_BASE_CFLAGS) \
	$(GST_CFLAGS)
libgstrrh264parse_la_LIBADD = \
	$(GST_PLUGINS_BASE_LIBS) \
	$(GST_BASE_LIBS)
	$(GST_LIBS)
libgstrrh264parse_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstrrh264parse_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstrrh264parse.h
