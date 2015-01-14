
What is it?
===========
Gst-Bt is a collection of plugins that integrates BitTorrent into GStreamer

Dependencies
============
+ GStreamer
+ [libtorrent](http://www.rasterbar.com/products/libtorrent/)

Building and Installation
=========================
```
./configure
make
```

For system wide installation:
```make install```

For user installation:
```cp src/.libs/libgstbt.so ~/.gstreamer-0.10/plugins/```

Available plugins
=================
+ btdemux

Examples
========
```gst-launch-0.10 filesrc location=your.torrent ! btdemux ! decodebin2 ! autovideosink```

Communication
=============
In case something fails, use this github project to create an issue.

