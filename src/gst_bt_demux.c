#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst_bt_demux.h"

GST_DEBUG_CATEGORY_EXTERN (gst_bt_demux_debug);
#define GST_CAT_DEFAULT gst_bt_demux_debug

#define gst_bt_demux_parent_class parent_class
G_DEFINE_TYPE (GstBtDemux, gst_bt_demux, GST_TYPE_ELEMENT);

static GstStaticPadTemplate sink_factory =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-bittorrent"));

static GstStaticPadTemplate src_factory =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);


/* Accumulate every buffer for processing later */
static GstFlowReturn
gst_bt_demux_sink_chain (GstPad * pad, GstBuffer * buffer)
{
  GstBtDemux *thiz;
  GstFlowReturn ret;

  thiz = GST_BT_DEMUX (gst_pad_get_parent (pad));

  GST_DEBUG_OBJECT (thiz, "Received buffer");
  gst_adapter_push (thiz->adapter, gst_buffer_ref (buffer));
  gst_object_unref (thiz);

  return GST_FLOW_OK;
}

/* On EOS, start processing the file */
static gboolean
gst_bt_demux_sink_event (GstPad * pad, GstEvent * event)
{
  GstBtDemux *thiz;
  GstPad *peer;
  GstBuffer *buf;
  GstMessage *message;
  gint len;
  gboolean res = TRUE;

  if (GST_EVENT_TYPE (event) != GST_EVENT_EOS)
    goto beach;

  thiz = GST_BT_DEMUX (gst_pad_get_parent (pad));

  GST_DEBUG_OBJECT (thiz, "Received EOS");
  len = gst_adapter_available (thiz->adapter);
  buf = gst_adapter_take_buffer (thiz->adapter, len);

  /* Time to process */
  gst_buffer_unref (buf);

  gst_object_unref (thiz);
beach:
  return res;
}

static void
gst_bt_demux_dispose (GObject * object)
{
  GstBtDemux *thiz;

  thiz = GST_BT_DEMUX (object);

  GST_DEBUG_OBJECT (thiz, "Disposing");

  if (thiz->adapter) {
    g_object_unref (thiz->adapter);
    thiz->adapter = NULL;
  }
  GST_CALL_PARENT (G_OBJECT_CLASS, dispose, (object));
}

static void
gst_bt_demux_class_init (GstBtDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  /* initialize the object class */
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_bt_demux_dispose);

  /* initialize the element class */
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));

  gst_element_class_set_details_simple (element_class,
      "BitTorrent Demuxer", "Codec/Demuxer",
      "Streams a BitTorrent file",
      "Jorge Luis Zapata <jorgeluis.zapata@gmail.com>");
}
 
static void
gst_bt_demux_init (GstBtDemux * thiz)
{
  GstPad *pad;

  pad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_chain_function (pad, gst_bt_demux_sink_chain);
  gst_pad_set_event_function (pad, gst_bt_demux_sink_event);

  gst_element_add_pad (GST_ELEMENT (thiz), pad);

  thiz->adapter = gst_adapter_new ();
}
