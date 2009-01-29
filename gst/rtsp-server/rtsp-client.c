/* GStreamer
 * Copyright (C) 2008 Wim Taymans <wim.taymans at gmail.com>
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

#include <sys/ioctl.h>

#include "rtsp-client.h"
#include "rtsp-sdp.h"

#undef DEBUG

static void gst_rtsp_client_finalize (GObject * obj);

G_DEFINE_TYPE (GstRTSPClient, gst_rtsp_client, G_TYPE_OBJECT);

static void
gst_rtsp_client_class_init (GstRTSPClientClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_rtsp_client_finalize;
}

static void
gst_rtsp_client_init (GstRTSPClient * client)
{
}

static void
gst_rtsp_client_finalize (GObject * obj)
{
  G_OBJECT_CLASS (gst_rtsp_client_parent_class)->finalize (obj);
}

/**
 * gst_rtsp_client_new:
 *
 * Create a new #GstRTSPClient instance.
 */
GstRTSPClient *
gst_rtsp_client_new (void)
{
  GstRTSPClient *result;

  result = g_object_new (GST_TYPE_RTSP_CLIENT, NULL);

  return result;
}

static void
handle_response (GstRTSPClient *client, GstRTSPMessage *response)
{
#ifdef DEBUG
    gst_rtsp_message_dump (response);
#endif

  gst_rtsp_connection_send (client->connection, response, NULL);
}

static void
handle_generic_response (GstRTSPClient *client, GstRTSPStatusCode code, 
    GstRTSPMessage *request)
{
  GstRTSPMessage response = { 0 };

  gst_rtsp_message_init_response (&response, code, 
	gst_rtsp_status_as_text (code), request);

  handle_response (client, &response);
}

static GstRTSPMedia *
find_media (GstRTSPClient *client, const GstRTSPUrl *uri, GstRTSPMessage *request)
{
  GstRTSPMediaFactory *factory;
  GstRTSPMedia *media;

  /* find the factory for the uri first */
  if (!(factory = gst_rtsp_media_mapping_find_factory (client->media_mapping, uri)))
    goto no_factory;

  /* prepare the media and add it to the pipeline */
  if (!(media = gst_rtsp_media_factory_construct (factory, uri)))
    goto no_media;

  /* prepare the media */
  if (!(gst_rtsp_media_prepare (media)))
    goto no_prepare;

  return media;

  /* ERRORS */
no_factory:
  {
    handle_generic_response (client, GST_RTSP_STS_NOT_FOUND, request);
    return NULL;
  }
no_media:
  {
    handle_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, request);
    g_object_unref (factory);
    return NULL;
  }
no_prepare:
  {
    handle_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, request);
    g_object_unref (media);
    g_object_unref (factory);
    return NULL;
  }
}

/* Get the session or NULL when there was no session */
static GstRTSPSession *
ensure_session (GstRTSPClient *client, GstRTSPMessage *request)
{
  GstRTSPResult res;
  GstRTSPSession *session;
  gchar *sessid;

  res = gst_rtsp_message_get_header (request, GST_RTSP_HDR_SESSION, &sessid, 0);
  if (res == GST_RTSP_OK) {
    /* we had a session in the request, find it again */
    if (!(session = gst_rtsp_session_pool_find (client->session_pool, sessid)))
      goto session_not_found;
  }
  else
    goto service_unavailable;

  return session;

  /* ERRORS */
session_not_found:
  {
    handle_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, request);
    return NULL;
  }
service_unavailable:
  {
    handle_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, request);
    return NULL;
  }
}

static gboolean
handle_teardown_response (GstRTSPClient *client, GstRTSPUrl *uri, GstRTSPMessage *request)
{
  GstRTSPSessionMedia *media;
  GstRTSPSession *session;
  GstRTSPMessage response = { 0 };
  GstRTSPStatusCode code;

  if (!(session = ensure_session (client, request)))
    goto no_session;

  /* get a handle to the configuration of the media in the session */
  media = gst_rtsp_session_get_media (session, uri);
  if (!media)
    goto not_found;

  gst_rtsp_session_media_stop (media);

  gst_rtsp_session_pool_remove (client->session_pool, session);
  g_object_unref (session);

  /* remove the session id from the request, which will also remove it from the
   * response */
  gst_rtsp_message_remove_header (request, GST_RTSP_HDR_SESSION, -1);

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (&response, code, gst_rtsp_status_as_text (code), request);

  handle_response (client, &response);

  return FALSE;

  /* ERRORS */
no_session:
  {
    /* error was sent already */
    return FALSE;
  }
not_found:
  {
    handle_generic_response (client, GST_RTSP_STS_NOT_FOUND, request);
    return FALSE;
  }
}

static gboolean
handle_pause_response (GstRTSPClient *client, GstRTSPUrl *uri, GstRTSPMessage *request)
{
  GstRTSPSessionMedia *media;
  GstRTSPSession *session;
  GstRTSPMessage response = { 0 };
  GstRTSPStatusCode code;

  if (!(session = ensure_session (client, request)))
    goto no_session;

  /* get a handle to the configuration of the media in the session */
  media = gst_rtsp_session_get_media (session, uri);
  if (!media)
    goto not_found;

  gst_rtsp_session_media_pause (media);
  g_object_unref (session);

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (&response, code, gst_rtsp_status_as_text (code), request);

  handle_response (client, &response);

  return FALSE;

  /* ERRORS */
no_session:
  {
    return FALSE;
  }
not_found:
  {
    handle_generic_response (client, GST_RTSP_STS_NOT_FOUND, request);
    return FALSE;
  }
}

static gboolean
handle_play_response (GstRTSPClient *client, GstRTSPUrl *uri, GstRTSPMessage *request)
{
  GstRTSPSessionMedia *media;
  GstRTSPSession *session;
  GstRTSPMessage response = { 0 };
  GstRTSPStatusCode code;
  GString *rtpinfo;
  guint n_streams, i;
  guint timestamp, seqnum;

  if (!(session = ensure_session (client, request)))
    goto no_session;

  /* get a handle to the configuration of the media in the session */
  media = gst_rtsp_session_get_media (session, uri);
  if (!media)
    goto not_found;

  /* grab RTPInfo from the payloaders now */
  rtpinfo = g_string_new ("");

  n_streams = gst_rtsp_media_n_streams (media->media);
  for (i = 0; i < n_streams; i++) {
    GstRTSPMediaStream *stream;
    gchar *uristr;

    stream = gst_rtsp_media_get_stream (media->media, i);

    g_object_get (G_OBJECT (stream->payloader), "seqnum", &seqnum, NULL);
    g_object_get (G_OBJECT (stream->payloader), "timestamp", &timestamp, NULL);

    if (i > 0)
      g_string_append (rtpinfo, ", ");

    uristr = gst_rtsp_url_get_request_uri (uri);
    g_string_append_printf (rtpinfo, "url=%s/stream=%d;seq=%u;rtptime=%u", uristr, i, seqnum, timestamp);
    g_free (uristr);
  }

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (&response, code, gst_rtsp_status_as_text (code), request);

  /* add the RTP-Info header */
  gst_rtsp_message_add_header (&response, GST_RTSP_HDR_RTP_INFO, rtpinfo->str);
  g_string_free (rtpinfo, TRUE);

  handle_response (client, &response);

  /* start playing after sending the request */
  gst_rtsp_session_media_play (media);
  g_object_unref (session);

  return FALSE;

  /* ERRORS */
no_session:
  {
    /* error was sent */
    return FALSE;
  }
not_found:
  {
    handle_generic_response (client, GST_RTSP_STS_NOT_FOUND, request);
    return FALSE;
  }
}

static gboolean
handle_setup_response (GstRTSPClient *client, GstRTSPUrl *uri, GstRTSPMessage *request)
{
  GstRTSPResult res;
  gchar *sessid;
  gchar *transport;
  gchar **transports;
  gboolean have_transport;
  GstRTSPTransport *ct, *st;
  GstRTSPSession *session;
  gint i;
  GstRTSPLowerTrans supported;
  GstRTSPMessage response = { 0 };
  GstRTSPStatusCode code;
  GstRTSPSessionStream *stream;
  gchar *trans_str, *pos;
  guint streamid;
  GstRTSPSessionMedia *media;
  gboolean need_session;

  /* the uri contains the stream number we added in the SDP config, which is
   * always /stream=%d so we need to strip that off 
   * parse the stream we need to configure, look for the stream in the abspath
   * first and then in the query. */
  if (!(pos = strstr (uri->abspath, "/stream="))) {
    if (!(pos = strstr (uri->query, "/stream=")))
      goto bad_request;
  }

  /* we can mofify the parse uri in place */
  *pos = '\0';

  pos += strlen ("/stream=");
  if (sscanf (pos, "%u", &streamid) != 1)
    goto bad_request;

  /* parse the transport */
  res = gst_rtsp_message_get_header (request, GST_RTSP_HDR_TRANSPORT, &transport, 0);
  if (res != GST_RTSP_OK)
    goto no_transport;

  transports = g_strsplit (transport, ",", 0);
  gst_rtsp_transport_new (&ct);  

  /* loop through the transports, try to parse */
  have_transport = FALSE;
  for (i = 0; transports[i]; i++) {

    gst_rtsp_transport_init (ct);  
    res = gst_rtsp_transport_parse (transports[i], ct);
    if (res == GST_RTSP_OK) {
      have_transport = TRUE;
      break;
    }
  }
  g_strfreev (transports);

  g_free (ct->destination);
  ct->destination = g_strdup (inet_ntoa (client->address.sin_addr));

  /* we have not found anything usable, error out */
  if (!have_transport) {
    goto unsupported_transports;
  }

  /* we have a valid transport, check if we can handle it */
  if (ct->trans != GST_RTSP_TRANS_RTP)
    goto unsupported_transports;
  if (ct->profile != GST_RTSP_PROFILE_AVP)
    goto unsupported_transports;
  supported = GST_RTSP_LOWER_TRANS_UDP |
	GST_RTSP_LOWER_TRANS_UDP_MCAST | GST_RTSP_LOWER_TRANS_TCP;
  if (!(ct->lower_transport & supported))
    goto unsupported_transports;

  /* a setup request creates a session for a client, check if the client already
   * sent a session id to us */
  res = gst_rtsp_message_get_header (request, GST_RTSP_HDR_SESSION, &sessid, 0);
  if (res == GST_RTSP_OK) {
    /* we had a session in the request, find it again */
    if (!(session = gst_rtsp_session_pool_find (client->session_pool, sessid)))
      goto session_not_found;
    need_session = FALSE;
  }
  else {
    /* create a session if this fails we probably reached our session limit or
     * something. */
    if (!(session = gst_rtsp_session_pool_create (client->session_pool)))
      goto service_unavailable;
    need_session = TRUE;
  }

  if (need_session) {
    GstRTSPMedia *m;

    /* get a handle to the configuration of the media in the session */
    if ((m = find_media (client, uri, request))) {
      media = gst_rtsp_session_manage_media (session, uri, m);
    }
  }
  /* get a handle to the configuration of the media in the session */
  if (!(media = gst_rtsp_session_get_media (session, uri)))
    goto not_found;

  /* get a handle to the stream in the media */
  if (!(stream = gst_rtsp_session_media_get_stream (media, streamid)))
    goto no_stream;

  /* setup the server transport from the client transport */
  st = gst_rtsp_session_stream_set_transport (stream, ct);

  /* serialize the server transport */
  trans_str = gst_rtsp_transport_as_text (st);

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (&response, code, gst_rtsp_status_as_text (code), request);

  if (need_session)
    gst_rtsp_message_add_header (&response, GST_RTSP_HDR_SESSION, session->sessionid);

  gst_rtsp_message_add_header (&response, GST_RTSP_HDR_TRANSPORT, trans_str);
  g_free (trans_str);
  g_object_unref (session);

  handle_response (client, &response);

  return TRUE;

  /* ERRORS */
bad_request:
  {
    handle_generic_response (client, GST_RTSP_STS_BAD_REQUEST, request);
    return FALSE;
  }
not_found:
  {
    handle_generic_response (client, GST_RTSP_STS_NOT_FOUND, request);
    return FALSE;
  }
no_stream:
  {
    handle_generic_response (client, GST_RTSP_STS_NOT_FOUND, request);
    return FALSE;
  }
session_not_found:
  {
    handle_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, request);
    return FALSE;
  }
no_transport:
  {
    handle_generic_response (client, GST_RTSP_STS_UNSUPPORTED_TRANSPORT, request);
    return FALSE;
  }
unsupported_transports:
  {
    handle_generic_response (client, GST_RTSP_STS_UNSUPPORTED_TRANSPORT, request);
    gst_rtsp_transport_free (ct);  
    return FALSE;
  }
service_unavailable:
  {
    handle_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, request);
    return FALSE;
  }
}

/* for the describe we must generate an SDP */
static gboolean
handle_describe_response (GstRTSPClient *client, GstRTSPUrl *uri, GstRTSPMessage *request)
{
  GstRTSPMessage response = { 0 };
  GstRTSPResult res;
  GstSDPMessage *sdp;
  guint i;
  gchar *str;
  GstRTSPMedia *media;

  /* check what kind of format is accepted, we don't really do anything with it
   * and always return SDP for now. */
  for (i = 0; i++; ) {
    gchar *accept;

    res = gst_rtsp_message_get_header (request, GST_RTSP_HDR_ACCEPT, &accept, i);
    if (res == GST_RTSP_ENOTIMPL)
      break;

    if (g_ascii_strcasecmp (accept, "application/sdp") == 0)
      break;
  }

  /* find the media object for the uri */
  if (!(media = find_media (client, uri, request)))
    goto no_media;

  /* create an SDP for the media object */
  if (!(sdp = gst_rtsp_sdp_from_media (media)))
    goto no_sdp;

  gst_rtsp_message_init_response (&response, GST_RTSP_STS_OK, 
	gst_rtsp_status_as_text (GST_RTSP_STS_OK), request);

  gst_rtsp_message_add_header (&response, GST_RTSP_HDR_CONTENT_TYPE, "application/sdp");

  str = g_strdup_printf ("rtsp://%s:%u%s/", uri->host, uri->port, uri->abspath);
  gst_rtsp_message_add_header (&response, GST_RTSP_HDR_CONTENT_BASE, str);
  g_free (str);

  /* add SDP to the response body */
  str = gst_sdp_message_as_text (sdp);
  gst_rtsp_message_take_body (&response, (guint8 *)str, strlen (str));
  gst_sdp_message_free (sdp);

  handle_response (client, &response);

  return TRUE;

  /* ERRORS */
no_media:
  {
    /* error reply is already sent */
    return FALSE;
  }
no_sdp:
  {
    handle_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, request);
    g_object_unref (media);
    return FALSE;
  }
}

static void
handle_options_response (GstRTSPClient *client, GstRTSPUrl *uri, GstRTSPMessage *request)
{
  GstRTSPMessage response = { 0 };
  GstRTSPMethod options;
  gchar *str;

  options = GST_RTSP_DESCRIBE |
	    GST_RTSP_OPTIONS |
    //        GST_RTSP_PAUSE |
            GST_RTSP_PLAY |
            GST_RTSP_SETUP |
            GST_RTSP_TEARDOWN;

  str = gst_rtsp_options_as_text (options);

  gst_rtsp_message_init_response (&response, GST_RTSP_STS_OK, 
	gst_rtsp_status_as_text (GST_RTSP_STS_OK), request);

  gst_rtsp_message_add_header (&response, GST_RTSP_HDR_PUBLIC, str);
  g_free (str);

  handle_response (client, &response);
}

/* remove duplicate and trailing '/' */
static void
santize_uri (GstRTSPUrl *uri)
{
  gint i, len;
  gchar *s, *d;
  gboolean have_slash, prev_slash;

  s = d = uri->abspath;
  len = strlen (uri->abspath);

  prev_slash = FALSE;

  for (i = 0; i < len; i++) {
    have_slash = s[i] == '/';
    *d = s[i];
    if (!have_slash || !prev_slash)
      d++;
    prev_slash = have_slash;
  }
  len = d - uri->abspath;
  /* don't remove the first slash if that's the only thing left */
  if (len > 1 && *(d-1) == '/')
    d--;
  *d = '\0';
}

/* this function runs in a client specific thread and handles all rtsp messages
 * with the client */
static gpointer
handle_client (GstRTSPClient *client)
{
  GstRTSPMessage request = { 0 };
  GstRTSPResult res;
  GstRTSPMethod method;
  const gchar *uristr;
  GstRTSPUrl *uri;
  GstRTSPVersion version;

  while (TRUE) {
    /* start by waiting for a message from the client */
    res = gst_rtsp_connection_receive (client->connection, &request, NULL);
    if (res < 0)
      goto receive_failed;

#ifdef DEBUG
    gst_rtsp_message_dump (&request);
#endif

    gst_rtsp_message_parse_request (&request, &method, &uristr, &version);

    if (version != GST_RTSP_VERSION_1_0) {
      /* we can only handle 1.0 requests */
      handle_generic_response (client, GST_RTSP_STS_RTSP_VERSION_NOT_SUPPORTED, &request);
      continue;
    }

    /* we always try to parse the url first */
    if ((res = gst_rtsp_url_parse (uristr, &uri)) != GST_RTSP_OK) {
      handle_generic_response (client, GST_RTSP_STS_BAD_REQUEST, &request);
      continue;
    }

    /* sanitize the uri */
    santize_uri (uri);

    /* now see what is asked and dispatch to a dedicated handler */
    switch (method) {
      case GST_RTSP_OPTIONS:
        handle_options_response (client, uri, &request);
        break;
      case GST_RTSP_DESCRIBE:
        handle_describe_response (client, uri, &request);
        break;
      case GST_RTSP_SETUP:
        handle_setup_response (client, uri, &request);
        break;
      case GST_RTSP_PLAY:
        handle_play_response (client, uri, &request);
        break;
      case GST_RTSP_PAUSE:
        handle_pause_response (client, uri, &request);
        break;
      case GST_RTSP_TEARDOWN:
        handle_teardown_response (client, uri, &request);
        break;
      case GST_RTSP_ANNOUNCE:
      case GST_RTSP_GET_PARAMETER:
      case GST_RTSP_RECORD:
      case GST_RTSP_REDIRECT:
      case GST_RTSP_SET_PARAMETER:
        handle_generic_response (client, GST_RTSP_STS_NOT_IMPLEMENTED, &request);
        break;
      case GST_RTSP_INVALID:
      default:
        handle_generic_response (client, GST_RTSP_STS_BAD_REQUEST, &request);
        break;
    }
    gst_rtsp_url_free (uri);
  }
  g_object_unref (client);
  return NULL;

  /* ERRORS */
receive_failed:
  {
    g_message ("receive failed %d (%s), disconnect client %p", res, 
	    gst_rtsp_strresult (res), client);
    gst_rtsp_connection_close (client->connection);
    g_object_unref (client);
    return NULL;
  }
}

/* called when we need to accept a new request from a client */
static gboolean
client_accept (GstRTSPClient *client, GIOChannel *channel)
{
  /* a new client connected. */
  int server_sock_fd, fd;
  unsigned int address_len;
  GstRTSPConnection *conn;

  server_sock_fd = g_io_channel_unix_get_fd (channel);

  address_len = sizeof (client->address);
  memset (&client->address, 0, address_len);

  fd = accept (server_sock_fd, (struct sockaddr *) &client->address,
      &address_len);
  if (fd == -1)
    goto accept_failed;

  /* now create the connection object */
  gst_rtsp_connection_create (NULL, &conn);
  conn->fd.fd = fd;

  /* FIXME some hackery, we need to have a connection method to accept server
   * connections */
  gst_poll_add_fd (conn->fdset, &conn->fd);

  g_message ("added new client %p ip %s with fd %d", client,
	        inet_ntoa (client->address.sin_addr), conn->fd.fd);

  client->connection = conn;

  return TRUE;

  /* ERRORS */
accept_failed:
  {
    g_error ("Could not accept client on server socket %d: %s (%d)",
            server_sock_fd, g_strerror (errno), errno);
    return FALSE;
  }
}

/**
 * gst_rtsp_client_set_session_pool:
 * @client: a #GstRTSPClient
 * @pool: a #GstRTSPSessionPool
 *
 * Set @pool as the sessionpool for @client which it will use to find
 * or allocate sessions. the sessionpool is usually inherited from the server
 * that created the client but can be overridden later.
 */
void
gst_rtsp_client_set_session_pool (GstRTSPClient *client, GstRTSPSessionPool *pool)
{
  GstRTSPSessionPool *old;

  old = client->session_pool;
  if (old != pool) {
    if (pool)
      g_object_ref (pool);
    client->session_pool = pool;
    if (old)
      g_object_unref (old);
  }
}

/**
 * gst_rtsp_client_get_session_pool:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPSessionPool object that @client uses to manage its sessions.
 *
 * Returns: a #GstRTSPSessionPool, unref after usage.
 */
GstRTSPSessionPool *
gst_rtsp_client_get_session_pool (GstRTSPClient *client)
{
  GstRTSPSessionPool *result;

  if ((result = client->session_pool))
    g_object_ref (result);

  return result;
}

/**
 * gst_rtsp_client_set_media_mapping:
 * @client: a #GstRTSPClient
 * @mapping: a #GstRTSPMediaMapping
 *
 * Set @mapping as the media mapping for @client which it will use to map urls
 * to media streams. These mapping is usually inherited from the server that
 * created the client but can be overriden later.
 */
void
gst_rtsp_client_set_media_mapping (GstRTSPClient *client, GstRTSPMediaMapping *mapping)
{
  GstRTSPMediaMapping *old;

  old = client->media_mapping;

  if (old != mapping) {
    if (mapping)
      g_object_ref (mapping);
    client->media_mapping = mapping;
    if (old)
      g_object_unref (old);
  }
}

/**
 * gst_rtsp_client_get_media_mapping:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPMediaMapping object that @client uses to manage its sessions.
 *
 * Returns: a #GstRTSPMediaMapping, unref after usage.
 */
GstRTSPMediaMapping *
gst_rtsp_client_get_media_mapping (GstRTSPClient *client)
{
  GstRTSPMediaMapping *result;

  if ((result = client->media_mapping))
    g_object_ref (result);

  return result;
}

/**
 * gst_rtsp_client_attach:
 * @client: a #GstRTSPClient
 * @channel: a #GIOChannel
 *
 * Accept a new connection for @client on the socket in @source. 
 *
 * This function should be called when the client properties and urls are fully
 * configured and the client is ready to start.
 *
 * Returns: %TRUE if the client could be accepted.
 */
gboolean
gst_rtsp_client_accept (GstRTSPClient *client, GIOChannel *channel)
{
  if (!client_accept (client, channel))
    goto accept_failed;

  /* client accepted, spawn a thread for the client */
  g_object_ref (client);
  client->thread = g_thread_create ((GThreadFunc)handle_client, client, TRUE, NULL);

  return TRUE;

  /* ERRORS */
accept_failed:
  {
    return FALSE;
  }
}
