
What is it?
===========
Gst-Bt is a collection of plugins that integrates BitTorrent into GStreamer

Dependencies
============
+ GStreamer 0.10 or GStreamer 1.0
+ [libtorrent](http://www.rasterbar.com/products/libtorrent/)

Building and Installation
=========================
```bash
# For GStreamer 1.0
./configure --with-gstreamer-api=1.0

# For GStreamer 0.10
./configure --with-gstreamer-api=0.10
make
```

For system wide installation:
```bash
make install
```

For user installation:
```bash
# For GStreamer 1.0
cp src/.libs/libgstbt.so ~/.local/share/gstreamer-1.0/plugins/

# For GStreamer 0.10
cp src/.libs/libgstbt.so ~/.gstreamer-0.10/plugins/
```

Available plugins
=================
+ btdemux BitTorrent demuxer
+ btsrc Magnet URI source

Examples
========
```bash
# In GStreamer 1.0
gst-launch-1.0 filesrc location=your.torrent ! btdemux ! decodebin ! autovideosink
gst-launch-1.0 btsrc uri=magnet:yourmagnet ! btdemux ! decodebin ! autovideosink

# In GStreamer 0.10
gst-launch-0.10 filesrc location=your.torrent ! btdemux ! decodebin2 ! autovideosink
gst-launch-0.10 btsrc uri=magnet:yourmagnet ! btdemux ! decodebin2 ! autovideosink
```

Communication
=============
In case something fails, use this github project to create an issue.

