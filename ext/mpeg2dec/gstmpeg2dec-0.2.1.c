/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <string.h>

#include <inttypes.h>

#include <mpeg2dec/mm_accel.h>
#include <mpeg2dec/video_out.h>
#include "gstmpeg2dec.h"

/* elementfactory information */
static GstElementDetails gst_mpeg2dec_details = {
  "mpeg1 and mpeg2 video decoder",
  "Codec/Video/Decoder",
  "GPL",
  "Uses libmpeg2 to decode MPEG video streams",
  VERSION,
  "David I. Lehn <dlehn@users.sourceforge.net>",
  "(C) 2000",
};

/* Mpeg2dec signals and args */
enum {
  /* FILL ME */
  LAST_SIGNAL
};

enum {
  ARG_0,
  ARG_FRAME_RATE,
  /* FILL ME */
};

static double video_rates[16] =
{
  0.0,
  24000.0/1001.,
  24.0,
  25.0,
  30000.0/1001.,
  30.0,
  50.0,
  60000.0/1001.,
  60.0,
  1.0,
  5.0,
  10.0,
  12.0,
  15.0,
  0.0,
  0.0
};

GST_PAD_TEMPLATE_FACTORY (src_template_factory,
  "src",
  GST_PAD_SRC,
  GST_PAD_ALWAYS,
  GST_CAPS_NEW (
    "mpeg2dec_src",
    "video/raw",
      "format",    GST_PROPS_FOURCC (GST_MAKE_FOURCC ('I','4','2','0')),
      "width",     GST_PROPS_INT_RANGE (16, 4096),
      "height",    GST_PROPS_INT_RANGE (16, 4096)
  )
);

GST_PAD_TEMPLATE_FACTORY (sink_template_factory,
  "sink",
  GST_PAD_SINK,
  GST_PAD_ALWAYS,
  GST_CAPS_NEW (
    "mpeg2dec_sink",
    "video/mpeg",
      "mpegversion",  GST_PROPS_INT_RANGE (1, 2),
      "systemstream", GST_PROPS_BOOLEAN (FALSE)
  )
);

static void	gst_mpeg2dec_class_init		(GstMpeg2decClass *klass);
static void	gst_mpeg2dec_init		(GstMpeg2dec *mpeg2dec);

static void	gst_mpeg2dec_dispose		(GObject *object);

static void	gst_mpeg2dec_set_property	(GObject *object, guint prop_id, 
						 const GValue *value, GParamSpec *pspec);
static void	gst_mpeg2dec_get_property	(GObject *object, guint prop_id, 
						 GValue *value, GParamSpec *pspec);

static gboolean gst_mpeg2dec_src_event       	(GstPad *pad, GstEvent *event);
static gboolean gst_mpeg2dec_src_query 		(GstPad *pad, GstQueryType type,
		       				 GstFormat *format, gint64 *value);

static gboolean gst_mpeg2dec_convert_sink 	(GstPad *pad, GstFormat src_format, gint64 src_value,
		         			 GstFormat *dest_format, gint64 *dest_value);
static gboolean gst_mpeg2dec_convert_src 	(GstPad *pad, GstFormat src_format, gint64 src_value,
		        	 		 GstFormat *dest_format, gint64 *dest_value);

static GstElementStateReturn
		gst_mpeg2dec_change_state	(GstElement *element);

static void	gst_mpeg2dec_chain		(GstPad *pad, GstBuffer *buffer);

static GstElementClass *parent_class = NULL;
/*static guint gst_mpeg2dec_signals[LAST_SIGNAL] = { 0 };*/

GType
gst_mpeg2dec_get_type (void)
{
  static GType mpeg2dec_type = 0;

  if (!mpeg2dec_type) {
    static const GTypeInfo mpeg2dec_info = {
      sizeof(GstMpeg2decClass),      
      NULL,
      NULL,
      (GClassInitFunc)gst_mpeg2dec_class_init,
      NULL,
      NULL,
      sizeof(GstMpeg2dec),
      0,
      (GInstanceInitFunc)gst_mpeg2dec_init,
    };
    mpeg2dec_type = g_type_register_static(GST_TYPE_ELEMENT, "GstMpeg2dec", &mpeg2dec_info, 0);
  }
  return mpeg2dec_type;
}

static void
gst_mpeg2dec_class_init(GstMpeg2decClass *klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass*)klass;
  gstelement_class = (GstElementClass*)klass;

  parent_class = g_type_class_ref(GST_TYPE_ELEMENT);

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_FRAME_RATE,
    g_param_spec_float ("frame_rate","frame_rate","frame_rate",
                        0.0, 1000.0, 0.0, G_PARAM_READABLE)); 
  
  gobject_class->set_property 	= gst_mpeg2dec_set_property;
  gobject_class->get_property 	= gst_mpeg2dec_get_property;
  gobject_class->dispose 	= gst_mpeg2dec_dispose;

  gstelement_class->change_state = gst_mpeg2dec_change_state;
}

typedef struct gst_mpeg2dec_vo_frame_s {
  vo_frame_t vo;

  GstBuffer *buffer;
  gboolean sent;
} gst_mpeg2dec_vo_frame_t;

#define NUM_FRAMES 3

typedef struct gst_mpeg2dec_vo_instance_s {
  vo_instance_t vo;
  GstMpeg2dec *mpeg2dec;
  gint prediction_index;
  gst_mpeg2dec_vo_frame_t frames[NUM_FRAMES];
} gst_mpeg2dec_vo_instance_t;

static void
gst_mpeg2dec_vo_frame_draw (vo_frame_t * frame)
{
  gst_mpeg2dec_vo_instance_t *_instance;
  gst_mpeg2dec_vo_frame_t *_frame;
  GstMpeg2dec *mpeg2dec;
  gint64 pts = -1;

  g_return_if_fail (frame != NULL);
  g_return_if_fail (((gst_mpeg2dec_vo_frame_t *)frame)->buffer != NULL);

  _frame = (gst_mpeg2dec_vo_frame_t *)frame;
  _instance = (gst_mpeg2dec_vo_instance_t *)frame->instance;

  mpeg2dec = GST_MPEG2DEC (_instance->mpeg2dec);


  /* we have to be carefull here. we do mpeg2_close in the READY state
   * but it can send a few frames still. We have to make sure we are playing
   * when we send frames. we do have to free those last frames though */
  if (GST_STATE (GST_ELEMENT (mpeg2dec)) != GST_STATE_PLAYING) {
    gst_buffer_unref (_frame->buffer);
    /* pretend we have sent the frame */
    _frame->sent = TRUE;
    return;
  }

  if (mpeg2dec->frame_rate_code != mpeg2dec->decoder->frame_rate_code)
  {
    mpeg2dec->frame_rate_code = mpeg2dec->decoder->frame_rate_code;

    g_object_notify (G_OBJECT (mpeg2dec), "frame_rate");
  }

  pts = mpeg2dec->next_time - 3 * (GST_SECOND / video_rates[mpeg2dec->decoder->frame_rate_code]);

  GST_BUFFER_TIMESTAMP (_frame->buffer) = pts;

  GST_DEBUG ("out: %lld %d %lld", GST_BUFFER_TIMESTAMP (_frame->buffer),
		  mpeg2dec->decoder->frame_rate_code,
                  (long long)(GST_SECOND / video_rates[mpeg2dec->decoder->frame_rate_code]));

  mpeg2dec->next_time += (GST_SECOND / video_rates[mpeg2dec->decoder->frame_rate_code]) + mpeg2dec->adjust;

  GST_BUFFER_FLAG_SET (_frame->buffer, GST_BUFFER_READONLY);
  mpeg2dec->frames_per_PTS++;
  mpeg2dec->first = FALSE;
  _frame->sent = TRUE;
  mpeg2dec->total_frames++;
  gst_pad_push (mpeg2dec->srcpad, _frame->buffer);
}

static int
gst_mpeg2dec_vo_setup (vo_instance_t * instance, int width, int height)
{
  gst_mpeg2dec_vo_instance_t * _instance;
  GstMpeg2dec *mpeg2dec;

  g_return_val_if_fail (instance != NULL, -1);

  GST_INFO ( "VO: setup w=%d h=%d", width, height);

  _instance = (gst_mpeg2dec_vo_instance_t*)instance;
  mpeg2dec = _instance->mpeg2dec;
  _instance->prediction_index = 1;
  mpeg2dec->width = width;
  mpeg2dec->height = height;
  mpeg2dec->total_frames = 0;

  gst_pad_try_set_caps (mpeg2dec->srcpad, 
		    gst_caps_new (
		      "mpeg2dec_caps",
		      "video/raw",
		      gst_props_new (
			"format",   GST_PROPS_FOURCC (GST_MAKE_FOURCC ('I','4','2','0')),
			  "width",  GST_PROPS_INT (width),
			  "height", GST_PROPS_INT (height),
			  NULL)));


  return 0;
}


static void
gst_mpeg2dec_vo_close (vo_instance_t * instance)
{
  gst_mpeg2dec_vo_instance_t * _instance;

  GST_INFO ( "VO: close");

  _instance = (gst_mpeg2dec_vo_instance_t*)instance;
  /* FIXME */
}

static vo_frame_t *
gst_mpeg2dec_vo_get_frame (vo_instance_t * instance, int flags)
{
  gst_mpeg2dec_vo_instance_t * _instance;
  gst_mpeg2dec_vo_frame_t *frame;
  size_t size0;
  uint8_t *data = NULL;
  GstMpeg2dec *mpeg2dec;

  g_return_val_if_fail (instance != NULL, NULL);

  GST_INFO ( "VO: get_frame");

  _instance = (gst_mpeg2dec_vo_instance_t *)instance;

  mpeg2dec = _instance->mpeg2dec;
  
  if (flags & VO_PREDICTION_FLAG) {
    _instance->prediction_index ^= 1;
    frame = &_instance->frames[_instance->prediction_index];
  } else {
    frame = &_instance->frames[2];
  }

  /* we are reusing this frame */
  if (frame->buffer != NULL) {
    /* if the frame wasn't sent, we have to unref twice */
    if (!frame->sent)
      gst_buffer_unref (frame->buffer);
    gst_buffer_unref (frame->buffer);
    frame->buffer = NULL;
  }

  size0 = mpeg2dec->width * mpeg2dec->height / 4;

  if (mpeg2dec->peerpool) {
    frame->buffer = gst_buffer_new_from_pool (mpeg2dec->peerpool, 0, 0);
  } else {
    size_t size = 6 * size0;
    size_t offset;
    GstBuffer *parent;

    parent = gst_buffer_new ();

    GST_BUFFER_SIZE(parent) = size + 0x10;
    GST_BUFFER_DATA(parent) = data = g_new(uint8_t, size + 0x10);

    offset = 0x10 - (((unsigned long)data) & 0xf);
    frame->buffer = gst_buffer_create_sub(parent, offset, size);

    gst_buffer_unref(parent);
  }
  data = GST_BUFFER_DATA(frame->buffer);

  /* need ref=2						*/
  /* 1 - unref when reusing this frame			*/
  /* 2 - unref when other elements done with buffer	*/
  gst_buffer_ref (frame->buffer);

  frame->vo.base[0] = data;
  frame->vo.base[1] = data + 4 * size0;
  frame->vo.base[2] = data + 5 * size0;
  /*printf("base[0]=%p\n", frame->vo.base[0]); */
  frame->sent = FALSE;

  return (vo_frame_t *)frame;
}

static void
gst_mpeg2dec_vo_open (GstMpeg2dec *mpeg2dec)
{
  gst_mpeg2dec_vo_instance_t * instance;
  gint i,j;

  GST_INFO ( "VO: open");

  instance = g_new (gst_mpeg2dec_vo_instance_t, 1);
  
  instance->vo.setup = gst_mpeg2dec_vo_setup;
  instance->vo.close = gst_mpeg2dec_vo_close;
  instance->vo.get_frame = gst_mpeg2dec_vo_get_frame;
  instance->mpeg2dec = mpeg2dec;

  for (i=0; i<NUM_FRAMES; i++) {
    for (j=0; j<3; j++) {
      instance->frames[j].vo.base[j] = NULL;
    }
    instance->frames[i].vo.copy = NULL;
    instance->frames[i].vo.field = NULL;
    instance->frames[i].vo.draw = gst_mpeg2dec_vo_frame_draw;
    instance->frames[i].vo.instance = (vo_instance_t *)instance;
    instance->frames[i].buffer = NULL;
  }

  mpeg2dec->vo = (vo_instance_t *) instance;
}

static void
gst_mpeg2dec_vo_destroy (GstMpeg2dec *mpeg2dec)
{
  gst_mpeg2dec_vo_instance_t * instance;
  gint i;

  GST_INFO ( "VO: destroy");

  instance = (gst_mpeg2dec_vo_instance_t *) mpeg2dec->vo;
  
  for (i=0; i<NUM_FRAMES; i++) {
    if (instance->frames[i].buffer) {
      if (!instance->frames[i].sent) {
        gst_buffer_unref (instance->frames[i].buffer);
      }
      gst_buffer_unref (instance->frames[i].buffer);
    } 
  }

  g_free (instance);
  mpeg2dec->vo = NULL;
}

static void
gst_mpeg2dec_init (GstMpeg2dec *mpeg2dec)
{
  /* create the sink and src pads */
  mpeg2dec->sinkpad = gst_pad_new_from_template (
		  GST_PAD_TEMPLATE_GET (sink_template_factory), "sink");
  gst_element_add_pad (GST_ELEMENT (mpeg2dec), mpeg2dec->sinkpad);
  gst_pad_set_chain_function (mpeg2dec->sinkpad, gst_mpeg2dec_chain);
  gst_pad_set_convert_function (mpeg2dec->sinkpad, gst_mpeg2dec_convert_sink);

  mpeg2dec->srcpad = gst_pad_new_from_template (
		  GST_PAD_TEMPLATE_GET (src_template_factory), "src");
  gst_element_add_pad (GST_ELEMENT (mpeg2dec), mpeg2dec->srcpad);
  gst_pad_set_event_function (mpeg2dec->srcpad, GST_DEBUG_FUNCPTR (gst_mpeg2dec_src_event));
  gst_pad_set_query_function (mpeg2dec->srcpad, GST_DEBUG_FUNCPTR (gst_mpeg2dec_src_query));
  gst_pad_set_convert_function (mpeg2dec->srcpad, gst_mpeg2dec_convert_src);

  /* initialize the mpeg2dec decoder state */
  mpeg2dec->decoder = g_new (mpeg2dec_t, 1);
  mpeg2dec->decoder->frame_rate_code = 0;
  mpeg2dec->accel = mm_accel();

  GST_FLAG_SET (GST_ELEMENT (mpeg2dec), GST_ELEMENT_EVENT_AWARE);
}

static void
gst_mpeg2dec_dispose (GObject *object)
{
  GstMpeg2dec *mpeg2dec = GST_MPEG2DEC (object);

  g_free (mpeg2dec->decoder);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_mpeg2dec_chain (GstPad *pad, GstBuffer *buf)
{
  GstMpeg2dec *mpeg2dec = GST_MPEG2DEC (gst_pad_get_parent (pad));
  guint32 size;
  guchar *data;
  guint num_frames;
  gint64 pts;

  GST_DEBUG ("MPEG2DEC: chain called");

  if (GST_IS_EVENT (buf)) {
    GstEvent *event = GST_EVENT (buf);

    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_DISCONTINUOUS:
      {
	//gint64 value = GST_EVENT_DISCONT_OFFSET (event, 0).value;
	//mpeg2dec->decoder->is_sequence_needed = 1;
	GST_DEBUG ("mpeg2dec: discont\n"); 
        mpeg2dec->first = TRUE;
        mpeg2dec->frames_per_PTS = 0;
        mpeg2dec->last_PTS = -1;
        mpeg2dec->adjust = 0;
        mpeg2dec->next_time = 0;
        gst_pad_event_default (pad, event);
	return;
      }
      case GST_EVENT_EOS:
        if (!mpeg2dec->closed) {
          /* close flushes the last few frames */
          mpeg2_close (mpeg2dec->decoder); 
	  mpeg2dec->closed = TRUE;
        }
      default:
        gst_pad_event_default (pad, event);
	return;
    }
  }

  size = GST_BUFFER_SIZE (buf);
  data = GST_BUFFER_DATA (buf);
  pts = GST_BUFFER_TIMESTAMP (buf);
    /* rationale for these heuristics;
     * - we keep our own timestamp guestimate in next_time, this is based on the
     *   frame rate of the video stream.
     * - we receive PTS values in the buffer timestamp.
     * - we only accept new pts values if they are monotonically increasing.
     * - if we have more than 10 frames without a new PTS value, we compare our
     *   internal counter to the PTS and calculate a diff. This is usefull when the
     *   framerate in the stream is wrong.
     * - if the PTS and our own counter are adrift bu more than 10 frames, we assume
     *   a discontinuity in the PTS and adjust our own counter.
     */
  GST_DEBUG ("mpeg2dec: pts %llu\n", pts);
  if (!mpeg2dec->first) {
    if (mpeg2dec->last_PTS < pts) {

      if (pts != mpeg2dec->next_time && mpeg2dec->frames_per_PTS > 10) {
	gint64 diff = ABS (pts - mpeg2dec->last_PTS);

	if (diff > (GST_SECOND / video_rates[mpeg2dec->decoder->frame_rate_code]) + GST_SECOND/1000) {
          mpeg2dec->adjust = (diff / mpeg2dec->frames_per_PTS +1) - 
	    (GST_SECOND / video_rates[mpeg2dec->decoder->frame_rate_code]);
	}
        mpeg2dec->next_time = pts;
      }
      mpeg2dec->frames_per_PTS = 0;
    }
    if (ABS (pts - mpeg2dec->last_PTS) > (GST_SECOND / video_rates[mpeg2dec->decoder->frame_rate_code])*10) {

      mpeg2dec->frames_per_PTS = 0;
      mpeg2dec->next_time = pts;
    }
  }
  if (mpeg2dec->next_time < pts) {
    mpeg2dec->next_time = pts;
  }
  /*
  if (mpeg2dec->last_PTS < pts) {
    mpeg2dec->next_time = pts;
    g_print ("** adjust next_time %lld %lld\n", mpeg2dec->last_PTS, pts);
  }
  */
	    
  mpeg2dec->last_PTS = pts;


/* fprintf(stderr, "MPEG2DEC: in timestamp=%llu\n",GST_BUFFER_TIMESTAMP(buf)); */
/* fprintf(stderr, "MPEG2DEC: have buffer of %d bytes\n",size);		*/
  num_frames = mpeg2_decode_data(mpeg2dec->decoder, data, data + size);
/*fprintf(stderr, "MPEG2DEC: decoded %d frames\n", num_frames);*/

  gst_buffer_unref(buf);
}

static gboolean
gst_mpeg2dec_convert_sink (GstPad *pad, GstFormat src_format, gint64 src_value,
		           GstFormat *dest_format, gint64 *dest_value)
{
  gboolean res = TRUE;
  GstMpeg2dec *mpeg2dec;
	      
  mpeg2dec = GST_MPEG2DEC (gst_pad_get_parent (pad));

  switch (src_format) {
    case GST_FORMAT_BYTES:
      switch (*dest_format) {
        case GST_FORMAT_DEFAULT:
          *dest_format = GST_FORMAT_TIME;
        case GST_FORMAT_TIME:
        default:
          res = FALSE;
      }
      break;
    case GST_FORMAT_TIME:
      switch (*dest_format) {
        case GST_FORMAT_DEFAULT:
          *dest_format = GST_FORMAT_BYTES;
        case GST_FORMAT_BYTES:
        default:
          res = FALSE;
      }
      break;
    default:
      res = FALSE;
  }
  return res;
}

static gboolean
gst_mpeg2dec_convert_src (GstPad *pad, GstFormat src_format, gint64 src_value,
		          GstFormat *dest_format, gint64 *dest_value)
{
  gboolean res = TRUE;
  GstMpeg2dec *mpeg2dec;
	      
  mpeg2dec = GST_MPEG2DEC (gst_pad_get_parent (pad));

  switch (src_format) {
    case GST_FORMAT_BYTES:
      switch (*dest_format) {
        case GST_FORMAT_DEFAULT:
          *dest_format = GST_FORMAT_TIME;
        case GST_FORMAT_TIME:
        default:
          res = FALSE;
      }
      break;
    case GST_FORMAT_TIME:
      switch (*dest_format) {
        case GST_FORMAT_DEFAULT:
          *dest_format = GST_FORMAT_BYTES;
        case GST_FORMAT_BYTES:
	  *dest_value = src_value * 6 * (mpeg2dec->width * mpeg2dec->height >> 2) *  
		  video_rates[mpeg2dec->decoder->frame_rate_code] / GST_SECOND;
	  break;
        case GST_FORMAT_UNITS:
	  *dest_value = src_value * video_rates[mpeg2dec->decoder->frame_rate_code] / GST_SECOND;
	  break;
        default:
          res = FALSE;
      }
      break;
    case GST_FORMAT_UNITS:
      switch (*dest_format) {
        case GST_FORMAT_DEFAULT:
          *dest_format = GST_FORMAT_TIME;
        case GST_FORMAT_TIME:
	  if (video_rates[mpeg2dec->decoder->frame_rate_code] != 0.0) {
	    *dest_value = src_value * GST_SECOND /
	      video_rates[mpeg2dec->decoder->frame_rate_code];
	  }
	  else
	    res = FALSE;
	  break;
        case GST_FORMAT_BYTES:
	  *dest_value = src_value * 6 * (mpeg2dec->width * mpeg2dec->height >> 2);
	  break;
        case GST_FORMAT_UNITS:
	  *dest_value = src_value;
	  break;
        default:
          res = FALSE;
      }
      break;
    default:
      res = FALSE;
  }
  return res;
}

static gboolean 
gst_mpeg2dec_src_query (GstPad *pad, GstQueryType type,
		        GstFormat *format, gint64 *value)
{
  gboolean res = TRUE;
  GstMpeg2dec *mpeg2dec;
  static const GstFormat formats[] = { GST_FORMAT_TIME, GST_FORMAT_BYTES };
#define MAX_SEEK_FORMATS 1 /* we can only do time seeking for now */
  gint i;

  mpeg2dec = GST_MPEG2DEC (gst_pad_get_parent (pad));

  switch (type) {
    case GST_QUERY_TOTAL:
    {
      switch (*format) {
        case GST_FORMAT_DEFAULT:
          *format = GST_FORMAT_TIME;
          /* fallthrough */
        case GST_FORMAT_TIME:
        case GST_FORMAT_BYTES:
        case GST_FORMAT_UNITS:
	{
          res = FALSE;

          for (i = 0; i < MAX_SEEK_FORMATS && !res; i++) {
	    GstFormat peer_format;
	    gint64 peer_value;
		  
	    peer_format = formats[i];
	  
            /* do the probe */
            if (gst_pad_query (GST_PAD_PEER (mpeg2dec->sinkpad), GST_QUERY_TOTAL,
			       &peer_format, &peer_value)) 
	    {
              GstFormat conv_format;

              /* convert to TIME */
              conv_format = GST_FORMAT_TIME;
              res = gst_pad_convert (mpeg2dec->sinkpad,
                              peer_format, peer_value,
                              &conv_format, value);
              /* and to final format */
              res &= gst_pad_convert (pad,
                         GST_FORMAT_TIME, *value,
                         format, value);
            }
	  }
          break;
	}
        default:
	  res = FALSE;
          break;
      }
      break;
    }
    case GST_QUERY_POSITION:
    {
      switch (*format) {
        case GST_FORMAT_DEFAULT:
          *format = GST_FORMAT_TIME;
          /* fallthrough */
        default:
          res = gst_pad_convert (pad,
                          GST_FORMAT_TIME, mpeg2dec->next_time,
                          format, value);
          break;
      }
      break;
    }
    default:
      res = FALSE;
      break;
  }

  return res;
}

static gboolean 
gst_mpeg2dec_src_event (GstPad *pad, GstEvent *event)
{
  gboolean res = TRUE;
  GstMpeg2dec *mpeg2dec;
  static const GstFormat formats[] = { GST_FORMAT_TIME, GST_FORMAT_BYTES };
#define MAX_SEEK_FORMATS 1 /* we can only do time seeking for now */
  gint i;

  mpeg2dec = GST_MPEG2DEC (gst_pad_get_parent (pad));

  switch (GST_EVENT_TYPE (event)) {
    /* the all-formats seek logic */
    case GST_EVENT_SEEK:
    {
      gint64 src_offset;
      gboolean flush;
      GstFormat format;
			                
      format = GST_FORMAT_TIME;

      /* first bring the src_format to TIME */
      if (!gst_pad_convert (pad,
                GST_EVENT_SEEK_FORMAT (event), GST_EVENT_SEEK_OFFSET (event),
                &format, &src_offset))
      {
        /* didn't work, probably unsupported seek format then */
        res = FALSE;
        break;
      }

      /* shave off the flush flag, we'll need it later */
      flush = GST_EVENT_SEEK_FLAGS (event) & GST_SEEK_FLAG_FLUSH;

      /* assume the worst */
      res = FALSE;

      /* while we did not exhaust our seek formats without result */
      for (i = 0; i < MAX_SEEK_FORMATS && !res; i++) {
        gint64 desired_offset;

        format = formats[i];

        /* try to convert requested format to one we can seek with on the sinkpad */
        if (gst_pad_convert (mpeg2dec->sinkpad, GST_FORMAT_TIME, src_offset, &format, &desired_offset))
        {
          GstEvent *seek_event;

          /* conversion succeeded, create the seek */
          seek_event = gst_event_new_seek (formats[i] | GST_SEEK_METHOD_SET | flush, desired_offset);
          /* do the seekk */
          if (gst_pad_send_event (GST_PAD_PEER (mpeg2dec->sinkpad), seek_event)) {
            /* seek worked, we're done, loop will exit */
            res = TRUE;
          }
        }
        /* at this point, either the seek worked or res == FALSE */
      }
      break;
    }
    default:
      res = FALSE;
      break;
  }
  gst_event_unref (event);
  return res;
}

static GstElementStateReturn
gst_mpeg2dec_change_state (GstElement *element)
{
  GstMpeg2dec *mpeg2dec = GST_MPEG2DEC (element);

  switch (GST_STATE_TRANSITION (element)) { 
    case GST_STATE_NULL_TO_READY:
      break;
    case GST_STATE_READY_TO_PAUSED:
    {
      gst_mpeg2dec_vo_open (mpeg2dec);
      mpeg2_init (mpeg2dec->decoder, mpeg2dec->accel, mpeg2dec->vo);

      mpeg2dec->decoder->is_sequence_needed = 1;
      mpeg2dec->decoder->frame_rate_code = 0;
      mpeg2dec->next_time = 0;
      mpeg2dec->peerpool = NULL;
      mpeg2dec->closed = FALSE;

      /* reset the initial video state */
      mpeg2dec->format = -1;
      mpeg2dec->width = -1;
      mpeg2dec->height = -1;
      mpeg2dec->first = TRUE;
      mpeg2dec->frames_per_PTS = 0;
      mpeg2dec->last_PTS = -1;
      mpeg2dec->adjust = 0;
      break;
    }
    case GST_STATE_PAUSED_TO_PLAYING:
      /* try to get a bufferpool */
      mpeg2dec->peerpool = gst_pad_get_bufferpool (mpeg2dec->srcpad);
      if (mpeg2dec->peerpool)
        GST_INFO ( "got pool %p", mpeg2dec->peerpool);
      break;
    case GST_STATE_PLAYING_TO_PAUSED:
      /* need to clear things we get from other plugins, since we could be reconnected */
      if (mpeg2dec->peerpool) {
	gst_buffer_pool_unref (mpeg2dec->peerpool);
	mpeg2dec->peerpool = NULL;
      }
      break;
    case GST_STATE_PAUSED_TO_READY:
      /* if we are not closed by an EOS event do so now, this cen send a few frames but
       * we are prepared to not really send them (see above) */
      if (!mpeg2dec->closed) {
        /*mpeg2_close (mpeg2dec->decoder); */
	mpeg2dec->closed = TRUE;
      }
      gst_mpeg2dec_vo_destroy (mpeg2dec);
      break;
    case GST_STATE_READY_TO_NULL:
      break;
    default:
      break;
  }

  GST_ELEMENT_CLASS (parent_class)->change_state (element);

  return GST_STATE_SUCCESS;
}

static void
gst_mpeg2dec_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  GstMpeg2dec *src;

  /* it's not null if we got it, but it might not be ours */
  g_return_if_fail (GST_IS_MPEG2DEC (object));
  src = GST_MPEG2DEC (object);

  switch (prop_id) {
    default:
      break;
  }
}

static void
gst_mpeg2dec_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GstMpeg2dec *mpeg2dec;

  /* it's not null if we got it, but it might not be ours */
  g_return_if_fail (GST_IS_MPEG2DEC (object));
  mpeg2dec = GST_MPEG2DEC (object);

  switch (prop_id) {
    case ARG_FRAME_RATE:
      g_value_set_float (value, video_rates[mpeg2dec->frame_rate_code]);
      break;
    default:
      break;
  }
}

static gboolean
plugin_init (GModule *module, GstPlugin *plugin)
{
  GstElementFactory *factory;

  /* create an elementfactory for the mpeg2dec element */
  factory = gst_element_factory_new("mpeg2dec",GST_TYPE_MPEG2DEC,
                                   &gst_mpeg2dec_details);
  g_return_val_if_fail(factory != NULL, FALSE);
  gst_element_factory_set_rank (factory, GST_ELEMENT_RANK_PRIMARY);

  gst_element_factory_add_pad_template (factory, GST_PAD_TEMPLATE_GET (src_template_factory));
  gst_element_factory_add_pad_template (factory, GST_PAD_TEMPLATE_GET (sink_template_factory));

  gst_plugin_add_feature (plugin, GST_PLUGIN_FEATURE (factory));

  return TRUE;
}

GstPluginDesc plugin_desc = {
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  "mpeg2dec",
  plugin_init
};
