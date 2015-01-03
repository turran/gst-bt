#ifndef GST_BT_DEMUX_H
#define GST_BT_DEMUX_H

#include <gst/gst.h>
#include <gst/base/gstadapter.h>

G_BEGIN_DECLS

#define GST_TYPE_BT_DEMUX            (gst_bt_demux_get_type())
#define GST_BT_DEMUX(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),\
                                         GST_TYPE_BT_DEMUX, GstBtDemux))
#define GST_BT_DEMUX_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),\
                                         GST_TYPE_BT_DEMUX, GstBtDemuxClass))
#define GST_BT_DEMUX_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),\
                                         GST_TYPE_BT_DEMUX, GstBtDemuxClass))
#define GST_IS_BT_DEMUX(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),\
                                         GST_TYPE_BT_DEMUX))
#define GST_IS_BT_DEMUX_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),\
                                         GST_TYPE_BT_DEMUX))

typedef struct _GstBtDemux
{
  GstElement parent;
  GstAdapter *adapter;

  gpointer session;
  GstTask *task;
  GStaticRecMutex task_lock;
} GstBtDemux;

typedef struct _GstBtDemuxClass
{
  GstElementClass parent_class;
} GstBtDemuxClass;

GType gst_bt_demux_get_type (void);

G_END_DECLS

#endif
