
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
btdemux
--------
```
Factory Details:
  Long name:	BitTorrent Demuxer
  Class:	Codec/Demuxer
  Description:	Streams a BitTorrent file
  Author(s):	Jorge Luis Zapata <jorgeluis.zapata@gmail.com>
  Rank:		primary + 1 (257)

Plugin Details:
  Name:			bt
  Description:		BitTorrent Plugin
  Filename:		/home/jl/.gstreamer-0.10/plugins/libgstbt.so
  Version:		0.0.1
  License:		unknown
  Source module:	gst-bt
  Binary package:	gst-bt
  Origin URL:		http://www.turran.org

GObject
 +----GstObject
       +----GstElement
             +----GstBtDemux

Pad Templates:
  SINK template: 'sink'
    Availability: Always
    Capabilities:
      application/x-bittorrent

  SRC template: 'src_%02d'
    Availability: Sometimes
    Capabilities:
      ANY


Element Flags:
  no flags set

Element Implementation:
  Has change_state() function: gst_bt_demux_change_state
  Has custom save_thyself() function: gst_element_save_thyself
  Has custom restore_thyself() function: gst_element_restore_thyself

Element has no clocking capabilities.
Element has no indexing capabilities.
Element has no URI handling capabilities.

Pads:
  SINK: 'sink'
    Implementation:
      Has chainfunc(): 0x7f0671ca6470
      Has custom eventfunc(): 0x7f0671ca6810
      Has custom queryfunc(): gst_pad_query_default
      Has custom iterintlinkfunc(): gst_pad_iterate_internal_links_default
      Has acceptcapsfunc(): gst_pad_acceptcaps_default
    Pad Template: 'sink'

Element Properties:
  name                : The name of the object
                        flags: readable, writable
                        String. Default: "btdemux0"
  selector-policy     : Specifies the automatic stream selector policy when no stream is selected
                        flags: readable, writable
                        Enum "GstBtDemuxSelectorPolicy" Default: 1, "larger"
                           (0): all              - All streams
                           (1): larger           - Larger stream
  typefind            : Run typefind before negotiating
                        flags: readable, writable
                        Boolean. Default: true
  n-streams           : Get the total number of available streams
                        flags: readable
                        Integer. Range: 0 - 2147483647 Default: 0 
  temp-location       : Location to store temporary files in
                        flags: readable, writable
                        String. Default: "/tmp/btdemux"
  temp-remove         : Remove temporary files
                        flags: readable, writable
                        Boolean. Default: true

Element Signals:
  "pad-added" :  void user_function (GstElement* object,
                                     GstPad* arg0,
                                     gpointer user_data);
  "pad-removed" :  void user_function (GstElement* object,
                                       GstPad* arg0,
                                       gpointer user_data);
  "no-more-pads" :  void user_function (GstElement* object,
                                        gpointer user_data);
  "streams-changed" :  void user_function (GstElement* object,
                                           gpointer user_data);

Element Actions:
  "get-stream-tags" :  GstTagList user_function (GstElement* object,
                                                 gint arg0);

```

Examples
========
```gst-launch-0.10 playbin2 uri=file:///your.torrent```

Communication
=============
In case something fails, use this github project to create an issue.

