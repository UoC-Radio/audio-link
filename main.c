/*
    Copyright (C) 2020 George Kiagiadakis <mail@gkiagia.gr>
    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include <glib-unix.h>

#define DEFAULT_LATENCY 200
#define DEFAULT_REMOTE_PORT 5000
#define DEFAULT_BIND_PORT 5000
#define DEFAULT_BITRATE 48000
#define DEFAULT_CHANNELS 2
#define DEFAULT_REMOTE_ADDRESS ""
#define DEFAULT_BIND_ADDRESS "0.0.0.0"

GST_DEBUG_CATEGORY_STATIC (audiolink_debug);
#define GST_CAT_DEFAULT audiolink_debug

struct audio_link
{
  GMainLoop *loop;
  GstElement *pipeline;
  GstElement *media_bin;
  GstCaps *payload_caps;
};

struct options
{
  gboolean send;
  gboolean receive;
  gint latency;
  gint remote_port;
  gint bind_port;
  gint bitrate;
  gint channels;
  gchar *remote_address;
  gchar *bind_address;
  gchar *jack_name;
};

static void
print_statistics (struct audio_link *self)
{
  g_autoptr (GObject) rtpbin = NULL;
  g_autoptr (GObject) session = NULL;
  g_autoptr (GstStructure) stats = NULL;
  g_autofree gchar *str = NULL;

  rtpbin = gst_child_proxy_get_child_by_name (
    GST_CHILD_PROXY (self->pipeline), "rtpbin");
  g_signal_emit_by_name (rtpbin, "get-session", 0, &session);
  g_object_get (session, "stats", &stats, NULL);

  /* simply dump the stats structure */
  str = gst_structure_to_string (stats);
  g_print ("Statistics: %s\n", str);
}

static gboolean
sigusr1_cb (gpointer user_data)
{
  struct audio_link *self = user_data;
  print_statistics (self);
  return G_SOURCE_CONTINUE;
}

static gboolean
signal_cb (gpointer user_data)
{
  struct audio_link *self = user_data;

  g_print ("Audio Link exiting...\n");

  g_main_loop_quit (self->loop);
  return G_SOURCE_REMOVE;
}

static void
error_cb (GstBus *bus, GstMessage *msg, gpointer user_data)
{
  g_autoptr (GError) err = NULL;
  g_autofree gchar *debug_info = NULL;
  struct audio_link *self = user_data;

  /* Print error details */
  gst_message_parse_error (msg, &err, &debug_info);
  g_printerr ("Error received from element %s: %s\n",
      GST_OBJECT_NAME(msg->src), err->message);
  g_printerr ("Debugging information: %s\n",
      debug_info ? debug_info : "none");

  g_main_loop_quit (self->loop);
}

static GstElement *
request_aux_receiver (GstElement *rtpbin, guint sessid, gpointer user_data)
{
  GstElement *rtx, *bin;
  GstPad *pad;
  gchar *name;
  GstStructure *pt_map;

  bin = gst_bin_new (NULL);
  rtx = gst_element_factory_make ("rtprtxreceive", NULL);
  pt_map = gst_structure_new ("application/x-rtp-pt-map",
    "96", G_TYPE_UINT, 97, NULL);
  g_object_set (rtx, "payload-type-map", pt_map, NULL);
  gst_structure_free (pt_map);
  gst_bin_add (GST_BIN (bin), rtx);

  pad = gst_element_get_static_pad (rtx, "src");
  name = g_strdup_printf ("src_%u", sessid);
  gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
  g_free (name);
  gst_object_unref (pad);

  pad = gst_element_get_static_pad (rtx, "sink");
  name = g_strdup_printf ("sink_%u", sessid);
  gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
  g_free (name);
  gst_object_unref (pad);

  return bin;
}

static GstElement *
request_aux_sender (GstElement *rtpbin, guint sessid, gpointer user_data)
{
  GstElement *rtx, *bin;
  GstPad *pad;
  gchar *name;
  GstStructure *pt_map;

  bin = gst_bin_new (NULL);
  rtx = gst_element_factory_make ("rtprtxsend", NULL);
  pt_map = gst_structure_new ("application/x-rtp-pt-map",
    "96", G_TYPE_UINT, 97, NULL);
  g_object_set (rtx, "payload-type-map", pt_map, NULL);
  gst_structure_free (pt_map);
  gst_bin_add (GST_BIN (bin), rtx);

  pad = gst_element_get_static_pad (rtx, "src");
  name = g_strdup_printf ("src_%u", sessid);
  gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
  g_free (name);
  gst_object_unref (pad);

  pad = gst_element_get_static_pad (rtx, "sink");
  name = g_strdup_printf ("sink_%u", sessid);
  gst_element_add_pad (bin, gst_ghost_pad_new (name, pad));
  g_free (name);
  gst_object_unref (pad);

  return bin;
}

static GstCaps *
request_pt_map (GstElement *rtpbin, guint session, guint pt, gpointer user_data)
{
  struct audio_link *self = user_data;
  return (pt == 96) ? gst_caps_ref (self->payload_caps) : NULL;
}

static void
rtpbin_pad_added (GstElement *rtpbin, GstPad *src, gpointer user_data)
{
  struct audio_link *self = user_data;
  g_autoptr (GstPad) sink = NULL;

  if (g_str_has_prefix (GST_PAD_NAME (src), "recv_rtp_src_")) {
    sink = gst_element_get_static_pad (self->media_bin, "sink");
    if (G_UNLIKELY (gst_pad_is_linked (sink))) {
      g_autoptr (GstPad) old_src = gst_pad_get_peer (sink);
      gst_pad_unlink (old_src, sink);
    }

    gst_pad_link (src, sink);
    gst_element_sync_state_with_parent (self->media_bin);
  }
  else if (g_str_has_prefix (GST_PAD_NAME (src), "send_rtp_src_")) {
    g_autoptr (GstElement) rtpsink =
        gst_bin_get_by_name (GST_BIN (self->pipeline), "rtpsink");
    sink = gst_element_get_static_pad (rtpsink, "sink");
    gst_pad_link (src, sink);
  }
}

static void
rtpbin_pad_removed (GstElement *rtpbin, GstPad *src, gpointer user_data)
{
  struct audio_link *self = user_data;
  g_autoptr (GstPad) sink = NULL;

  if (g_str_has_prefix (GST_PAD_NAME (src), "recv_rtp_src_")) {
    sink = gst_element_get_static_pad (self->media_bin, "sink");
    gst_pad_unlink (src, sink);
    gst_element_set_state (self->media_bin, GST_STATE_PAUSED);
  }
}

static gboolean
init_receive (struct audio_link *self, const struct options *options)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GstBus) bus = NULL;
  g_autoptr (GstElement) rtpbin = NULL;
  g_autoptr (GstElement) rtpsrc = NULL;

  if (!(self->pipeline = gst_parse_launch ("rtpbin name=rtpbin "
    "udpsrc name=rtpsrc "
    "udpsrc name=rtcpsrc ! rtpbin.recv_rtcp_sink_0 "
    "rtpbin.send_rtcp_src_0 ! udpsink name=rtcpsink", &error)))
  {
    g_printerr ("constructing the pipeline failed: %s\n",
        error->message);
    return FALSE;
  }

  if (!(self->media_bin = gst_parse_bin_from_description (
    "rtpgstdepay name=depayloader ! rawaudioparse name=parser"
    " ! jackaudiosink name=audio_sink", TRUE, &error)))
  {
    g_printerr ("constructing the sink bin failed: %s\n",
        error->message);
    g_object_unref (self->pipeline);
    return FALSE;
  }

  /* consume the floating reference so that we always hold one ref */
  g_object_ref_sink (self->media_bin);
  gst_bin_add (GST_BIN (self->pipeline), self->media_bin);

  self->payload_caps = gst_caps_new_simple ("application/x-rtp",
    "media", G_TYPE_STRING, "application",
    "clock-rate", G_TYPE_INT, 90000,
    "encoding-name", G_TYPE_STRING, "X-GST", NULL);

  gst_child_proxy_set (GST_CHILD_PROXY (self->pipeline),
    "rtpbin::latency", options->latency,
    "rtpbin::do-retransmission", TRUE,
    "rtpbin::rtp-profile", GST_RTP_PROFILE_AVPF,
    "rtpsrc::caps", self->payload_caps,
    "rtpsrc::address", options->bind_address,
    "rtpsrc::port", options->bind_port,
    "rtcpsrc::address", options->bind_address,
    "rtcpsrc::port", options->bind_port + 1,
    "rtcpsink::host", options->remote_address,
    "rtcpsink::port", options->remote_port + 1,
    "rtcpsink::sync", FALSE,
    "rtcpsink::async", FALSE,
    NULL);
  gst_child_proxy_set (GST_CHILD_PROXY (self->media_bin),
    "parser::pcm-format", 28 /* f32le */,
    "parser::sample-rate", options->bitrate,
    "parser::num-channels", options->channels,
    "audio_sink::connect", 0 /* Don't automatically connect ports to physical ports */,
    "audio_sink::client-name", options->jack_name,
    NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
  g_signal_connect (bus, "message::error", G_CALLBACK (error_cb), self);
  gst_bus_add_signal_watch (bus);

  rtpbin = gst_bin_get_by_name (GST_BIN (self->pipeline), "rtpbin");
  g_signal_connect (rtpbin, "request-aux-receiver",
    G_CALLBACK (request_aux_receiver), self);
  g_signal_connect (rtpbin, "request-pt-map",
    G_CALLBACK (request_pt_map), self);
  g_signal_connect (rtpbin, "pad-added",
    G_CALLBACK (rtpbin_pad_added), self);
  g_signal_connect (rtpbin, "pad-removed",
    G_CALLBACK (rtpbin_pad_removed), self);

  /* This link needs to happen after we have connected the
   * "request-aux-receiver" signal, because rtpbin internally
   * calls our callback to create rtprtxreceive while it is
   * creating the "recv_rtp_sink_0" pad
   */
  rtpsrc = gst_bin_get_by_name (GST_BIN (self->pipeline), "rtpsrc");
  gst_element_link_pads (rtpsrc, "src", rtpbin, "recv_rtp_sink_0");

  return TRUE;
}

static gboolean
init_send (struct audio_link *self, const struct options *options)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GstCaps) media_caps = NULL;
  g_autoptr (GstBus) bus = NULL;
  g_autoptr (GstElement) rtpbin = NULL;

  if (!(self->pipeline = gst_parse_launch ("rtpbin name=rtpbin "
    "udpsink name=rtpsink "
    "udpsrc name=rtcpsrc ! rtpbin.recv_rtcp_sink_0 "
    "rtpbin.send_rtcp_src_0 ! udpsink name=rtcpsink", &error)))
  {
    g_printerr ("constructing the pipeline failed: %s\n",
        error->message);
    return FALSE;
  }

  if (!(self->media_bin = gst_parse_bin_from_description (
    "jackaudiosrc name=audio_src ! capsfilter name=capsfilter"
    " ! rtpgstpay name=payloader", TRUE, &error)))
  {
    g_printerr ("constructing the sink bin failed: %s\n",
        error->message);
    g_object_unref (self->pipeline);
    return FALSE;
  }

  /* consume the floating reference so that we always hold one ref */
  g_object_ref_sink (self->media_bin);
  gst_bin_add (GST_BIN (self->pipeline), self->media_bin);

  self->payload_caps = gst_caps_new_simple ("application/x-rtp",
    "media", G_TYPE_STRING, "application",
    "clock-rate", G_TYPE_INT, 90000,
    "encoding-name", G_TYPE_STRING, "X-GST",
    NULL);

  media_caps = gst_caps_new_simple ("audio/x-raw",
    "format", G_TYPE_STRING, "F32LE",
    "layout", G_TYPE_STRING, "interleaved",
    "rate", G_TYPE_INT, options->bitrate,
    "channels", G_TYPE_INT, options->channels,
    NULL);

  gst_child_proxy_set (GST_CHILD_PROXY (self->pipeline),
    "rtpbin::latency", options->latency,
    "rtpbin::do-retransmission", TRUE,
    "rtpbin::rtp-profile", GST_RTP_PROFILE_AVPF,
    "rtcpsrc::address", options->bind_address,
    "rtcpsrc::port", options->bind_port + 1,
    "rtpsink::host", options->remote_address,
    "rtpsink::port", options->remote_port,
    "rtcpsink::host", options->remote_address,
    "rtcpsink::port", options->remote_port + 1,
    "rtcpsink::sync", FALSE,
    "rtcpsink::async", FALSE,
    NULL);
  gst_child_proxy_set (GST_CHILD_PROXY (self->media_bin),
    "payloader::config-interval", 2,
    "capsfilter::caps", media_caps,
    "audio_src::connect", 0 /* Don't automatically connect ports to physical ports */,
    "audio_src::client-name", options->jack_name,
    NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
  g_signal_connect (bus, "message::error", G_CALLBACK (error_cb), self);
  gst_bus_add_signal_watch (bus);

  rtpbin = gst_bin_get_by_name (GST_BIN (self->pipeline), "rtpbin");
  g_signal_connect (rtpbin, "request-aux-sender",
    G_CALLBACK (request_aux_sender), self);
  g_signal_connect (rtpbin, "request-pt-map",
    G_CALLBACK (request_pt_map), self);
  g_signal_connect (rtpbin, "pad-added",
    G_CALLBACK (rtpbin_pad_added), self);
  g_signal_connect (rtpbin, "pad-removed",
    G_CALLBACK (rtpbin_pad_removed), self);

  /* This link needs to happen after we have connected the
   * "request-aux-sender" signal, because rtpbin internally
   * calls our callback to create rtprtxsend while it is
   * creating the "send_rtp_sink_0" pad
   */
  gst_element_link_pads (self->media_bin, "src", rtpbin, "send_rtp_sink_0");

  return TRUE;
}

gint
main (gint argc, gchar **argv)
{
  struct audio_link self = { 0 };
  struct options options = {
    .send = FALSE,
    .receive = FALSE,
    .latency = DEFAULT_LATENCY,
    .remote_address = DEFAULT_REMOTE_ADDRESS,
    .remote_port = DEFAULT_REMOTE_PORT,
    .bind_address = DEFAULT_BIND_ADDRESS,
    .bind_port = DEFAULT_BIND_PORT,
    .bitrate = DEFAULT_BITRATE,
    .channels = DEFAULT_CHANNELS,
    .jack_name = NULL,
  };
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (GError) error = NULL;

  const GOptionEntry entries[] = {
    { "send", 's', 0, G_OPTION_ARG_NONE, &options.send,
      "Enable sending audio to the remote node", NULL },
    { "receive", 'c', 0, G_OPTION_ARG_NONE, &options.receive,
      "Enable receiving audio from the remote node", NULL},
    { "latency", 'l', 0, G_OPTION_ARG_INT, &options.latency,
      "Amount of ms to buffer in the jitterbuffers",
      G_STRINGIFY (DEFAULT_LATENCY) },
    { "remote-address", 'a', 0, G_OPTION_ARG_STRING,
      &options.remote_address,
      "Address (IPv4 / IPv6) to send packets to", "" },
    { "remote-port", 'p', 0, G_OPTION_ARG_INT,
      &options.remote_port,
      "Port to send RTP packets (and RTCP in port+1)",
      G_STRINGIFY (DEFAULT_REMOTE_PORT) },
    { "bind-address", 'b', 0, G_OPTION_ARG_STRING,
      &options.bind_address,
      "Local address (IPv4 / IPv6) to bind to",
      G_STRINGIFY (DEFAULT_BIND_ADDRESS) },
    { "bind-port", 't', 0, G_OPTION_ARG_INT,
        &options.bind_port, "Port to bind to",
      G_STRINGIFY (DEFAULT_BIND_PORT) },
    { "bitrate", 'r', 0, G_OPTION_ARG_INT,
        &options.bitrate, "Audio bitrate",
        G_STRINGIFY (DEFAULT_BITRATE) },
    { "channels", 'n', 0, G_OPTION_ARG_INT,
        &options.channels, "Number of audio channels",
        G_STRINGIFY (DEFAULT_CHANNELS) },
    { "jack-name", 'j', 0, G_OPTION_ARG_STRING,
       &options.jack_name, "The name of the Jack client", "" },
    {NULL}
  };

  GST_DEBUG_CATEGORY_INIT (audiolink_debug, "audiolink", 0, "Audio Link");

  /* cmd line option parsing */

  context = g_option_context_new (NULL);
  g_option_context_set_summary (context, "stream audio from one network node to another");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_add_group (context, gst_init_get_option_group ());

  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_printerr ("Option parsing failed: %s\n", error->message);
    return 1;
  }

  if ((options.send && options.receive) ||
      (!options.send && !options.receive)) {
    g_printerr ("--receive or --send must be specified (but not both)\n");
    return 2;
  }

  /* initialize */
  self.loop = g_main_loop_new (NULL, FALSE);

  if (options.receive)
    init_receive (&self, &options);
  else
    init_send (&self, &options);

  g_print ("Ready.\n");

  /* install signal handler to exit gracefully */
  g_unix_signal_add (SIGHUP, signal_cb, &self);
  g_unix_signal_add (SIGINT, signal_cb, &self);
  g_unix_signal_add (SIGTERM, signal_cb, &self);
  g_unix_signal_add (SIGUSR1, sigusr1_cb, &self);

  /* run the pipeline */
  gst_element_set_state (self.pipeline, GST_STATE_PLAYING);
  g_main_loop_run (self.loop);
  gst_element_set_state (self.pipeline, GST_STATE_NULL);

  /* cleanup */
  gst_caps_unref (self.payload_caps);
  gst_object_unref (self.pipeline);
  g_main_loop_unref (self.loop);

  return 0;
}
