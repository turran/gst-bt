plugindir = $(libdir)/gstreamer-@GST_MAJORMINOR@
plugin_LTLIBRARIES = src/libgstbt.la

src_libgstbt_la_SOURCES = \
src/gst_bt.c \
src/gst_bt_type.c \
src/gst_bt_type.h \
src/gst_bt_src.cpp \
src/gst_bt_src.hpp \
src/gst_bt_demux.cpp \
src/gst_bt_demux.hpp

src_libgstbt_la_CFLAGS = \
$(GST_BT_CFLAGS)

src_libgstbt_la_CXXFLAGS = \
$(GST_BT_CFLAGS)

src_libgstbt_la_LIBADD = \
$(GST_BT_LIBS)

libgstbt_la_LDFLAGS = -no-undefined -module -avoid-version
