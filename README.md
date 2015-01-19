
What is it?
===========
Gst-Bt is a collection of plugins that integrates BitTorrent into GStreamer

Dependencies
============
+ GStreamer 0.10 (GStreamer 1.0 is on the TODO)
+ [libtorrent](http://www.rasterbar.com/products/libtorrent/)

Building and Installation
=========================
```bash
./configure
make
```

For system wide installation:
```bash
make install
```

For user installation:
```bash
cp src/.libs/libgstbt.so ~/.gstreamer-0.10/plugins/
```

Available plugins
=================
+ btdemux BitTorrent demuxer
+ btsrc Magnet URI source

Examples
========
```bash
gst-launch-0.10 filesrc location=your.torrent ! btdemux ! decodebin2 ! autovideosink
gst-launch-0.10 btsrc uri=magnet:yourmagnet ! btdemux ! decodebin2 ! autovideosink
```

Communication
=============
In case something fails, use this github project to create an issue.

