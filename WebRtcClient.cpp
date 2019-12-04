#include "WebRtcClient.h"
#include <QDebug>
#include <QUuid>
#include <QJsonObject>
#include <QJsonDocument>
#define CHUNK_SIZE  4096
#define STUN_SERVER " stun-server=stun://stun.l.google.com:19302 "
#define RTP_CAPS_PCMA "application/x-rtp,media=audio,encoding-name=PCMA,payload="
#define RTP_CAPS_VP8 "application/x-rtp,media=video,encoding-name=VP8,payload="

WebRtcClient::WebRtcClient(QString fileName, QString peerId, QString wssUrl) :
    m_sendChannel(nullptr),
    m_receiveChannel(nullptr),
    m_webSocket(nullptr),
    m_appState(APP_STATE_UNKNOWN),
    m_peerId(peerId),
    m_wssUrl(wssUrl),
    m_fileName(fileName),
    m_source_id(0)

{

}

WebRtcClient::~WebRtcClient()
{
    if(m_file){
        /* free the file */
        g_mapped_file_unref (m_file);
    }
}


void WebRtcClient::start_feed(GstElement* appsrc, guint unused_size, gpointer userData)
{
    WebRtcClient *client = (WebRtcClient*)userData;
    if(client->m_source_id == 0)
    {
        qDebug() << "start feeding audio data!";
        client->m_source_id = g_idle_add((GSourceFunc)push_buffer, userData);
    }

}


void WebRtcClient::stop_feed(GstElement* appsrc,  gpointer userData)
{
    WebRtcClient *client = (WebRtcClient*)userData;
    if(client->m_source_id != 0)
    {
        qDebug() << "stop feeding audio data!";
        g_source_remove(client->m_source_id);
        client->m_source_id = 0;
    }
}

gboolean WebRtcClient::push_buffer(gpointer userData)
{
    GstBuffer *buffer;
    guint len;
    GstFlowReturn ret;

    WebRtcClient *client = (WebRtcClient*)userData;

    if (client->m_offset >= client->m_length)
    {
        /* we are EOS, send end-of-stream and remove the source */
        g_signal_emit_by_name (client->m_audioSrc, "end-of-stream", &ret);
        return FALSE;
    }

    /* read the next chunk */
    buffer = gst_buffer_new ();

    len = CHUNK_SIZE;
    if (client->m_offset + len > client->m_length)
    {
        len = client->m_length - client->m_offset;
    }

    gst_buffer_append_memory (buffer,
     gst_memory_new_wrapped (GST_MEMORY_FLAG_READONLY,
                             client->m_data, client->m_length, client->m_offset, len, nullptr, nullptr));

    GST_DEBUG ("feed buffer %p, offset %" G_GUINT64_FORMAT "-%u", buffer, client->m_offset, len);
    g_signal_emit_by_name (client->m_audioSrc, "push-buffer", buffer, &ret);
    gst_buffer_unref (buffer);
    if (ret != GST_FLOW_OK)
    {
        /* some error, stop sending data */
        return FALSE;
    }

    client->m_offset += len;

    return TRUE;

}


void WebRtcClient::on_negotiation_needed (GstElement * element, gpointer user_data)
{
    GstPromise *promise;

    WebRtcClient *client = (WebRtcClient*)user_data;

    client->m_appState = PEER_CALL_NEGOTIATING;
    promise = gst_promise_new_with_change_func (on_offer_created, user_data, NULL);;
    g_signal_emit_by_name (client->m_webrtc1, "create-offer", NULL, promise);
}


void WebRtcClient::on_offer_created (GstPromise * promise, gpointer user_data)
{
    WebRtcClient *client = (WebRtcClient*)user_data;

    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply;

    g_assert_cmphex (client->m_appState, ==, PEER_CALL_NEGOTIATING);

    g_assert_cmphex (gst_promise_wait(promise), ==, GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply (promise);
    gst_structure_get (reply, "offer",
        GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref (promise);

    promise = gst_promise_new ();
    g_signal_emit_by_name (client->m_webrtc1, "set-local-description", offer, promise);
    gst_promise_interrupt (promise);
    gst_promise_unref (promise);

    /* Send offer to peer */
    client->send_sdp_offer (offer);


    gst_webrtc_session_description_free (offer);

}


void WebRtcClient::send_sdp_offer (GstWebRTCSessionDescription * offer)
{
    gchar *text;
    QJsonObject jsonMsg;
    QJsonObject jsonSdp;

    if (m_appState < PEER_CALL_NEGOTIATING)
    {
      qDebug() << "Can't send offer, not in call";
      return;
    }

    text = gst_sdp_message_as_text (offer->sdp);
    g_print ("Sending offer:\n%s\n", text);

    jsonSdp.insert("type", "offer");
    jsonSdp.insert("sdp", text);

//    QJsonDocument sdpDcument;
//    sdpDcument.setObject(jsonSdp);
//    QByteArray sdpByteArray = sdpDcument.toJson(QJsonDocument::Compact);
//    jsonMsg.insert("sdp", QString(sdpByteArray));

    jsonMsg.insert("sdp", QJsonValue(jsonSdp));

    QJsonDocument msgDcument;
    msgDcument.setObject(jsonMsg);
    QByteArray msgByteArray = msgDcument.toJson(QJsonDocument::Compact);

    sendMsg(QString(msgByteArray));

}

void WebRtcClient::send_ice_candidate_message (GstElement * webrtc G_GNUC_UNUSED, guint mlineindex,
                                               gchar * candidate, gpointer user_data)
{
    WebRtcClient *client = (WebRtcClient*)user_data;

    gchar *text;

    QJsonObject iceJson;
    QJsonObject msgJson;

    if (client->m_appState < PEER_CALL_NEGOTIATING)
    {
      qDebug() << "Can't send ICE, not in call";
      return;
    }

    iceJson.insert("candidate", candidate);
    iceJson.insert("sdpMLineIndex", (int)mlineindex);

    msgJson.insert("ice", QJsonValue(iceJson));
    QJsonDocument msgDcument;
    msgDcument.setObject(msgJson);
    QByteArray msgByteArray = msgDcument.toJson(QJsonDocument::Compact);

    client->sendMsg(QString(msgByteArray));
}


void WebRtcClient::on_incoming_stream (GstElement * webrtc, GstPad * pad, GstElement * pipe)
{
    GstElement *decodebin;
    GstPad *sinkpad;

    if (GST_PAD_DIRECTION (pad) != GST_PAD_SRC)
      return;

    decodebin = gst_element_factory_make ("decodebin", NULL);
    g_signal_connect (decodebin, "pad-added",
        G_CALLBACK (on_incoming_decodebin_stream), pipe);
    gst_bin_add (GST_BIN (pipe), decodebin);
    gst_element_sync_state_with_parent (decodebin);

    sinkpad = gst_element_get_static_pad (decodebin, "sink");
    gst_pad_link (pad, sinkpad);
    gst_object_unref (sinkpad);

}

void WebRtcClient::on_incoming_decodebin_stream (GstElement * decodebin, GstPad * pad, GstElement * pipe)
{
    GstCaps *caps;
    const gchar *name;

    if (!gst_pad_has_current_caps (pad)) {
      g_printerr ("Pad '%s' has no caps, can't do anything, ignoring\n",
          GST_PAD_NAME (pad));
      return;
    }

    caps = gst_pad_get_current_caps (pad);
    name = gst_structure_get_name (gst_caps_get_structure (caps, 0));

    if (g_str_has_prefix (name, "video")) {
      handle_media_stream (pad, pipe, "videoconvert", "autovideosink");
    } else if (g_str_has_prefix (name, "audio")) {
      handle_media_stream (pad, pipe, "audioconvert", "autoaudiosink");
    } else {
      g_printerr ("Unknown pad %s, ignoring", GST_PAD_NAME (pad));
    }

}


void WebRtcClient::handle_media_stream (GstPad * pad, GstElement * pipe, const char * convert_name, const char * sink_name)
{
  GstPad *qpad;
  GstElement *q, *conv, *resample, *sink;
  GstPadLinkReturn ret;

  g_print ("Trying to handle stream with %s ! %s", convert_name, sink_name);

  q = gst_element_factory_make ("queue", NULL);
  g_assert_nonnull (q);
  conv = gst_element_factory_make (convert_name, NULL);
  g_assert_nonnull (conv);
  sink = gst_element_factory_make (sink_name, NULL);
  g_assert_nonnull (sink);

  if (g_strcmp0 (convert_name, "audioconvert") == 0) {
    /* Might also need to resample, so add it just in case.
     * Will be a no-op if it's not required. */
    resample = gst_element_factory_make ("audioresample", NULL);
    g_assert_nonnull (resample);
    gst_bin_add_many (GST_BIN (pipe), q, conv, resample, sink, NULL);
    gst_element_sync_state_with_parent (q);
    gst_element_sync_state_with_parent (conv);
    gst_element_sync_state_with_parent (resample);
    gst_element_sync_state_with_parent (sink);
    gst_element_link_many (q, conv, resample, sink, NULL);
  } else {
    gst_bin_add_many (GST_BIN (pipe), q, conv, sink, NULL);
    gst_element_sync_state_with_parent (q);
    gst_element_sync_state_with_parent (conv);
    gst_element_sync_state_with_parent (sink);
    gst_element_link_many (q, conv, sink, NULL);
  }

  qpad = gst_element_get_static_pad (q, "sink");

  ret = gst_pad_link (pad, qpad);
  g_assert_cmphex (ret, ==, GST_PAD_LINK_OK);
}


void WebRtcClient::doWork()
{
    GError *error = nullptr;
    gst_init(nullptr, nullptr);
    if(!check_plugins()){
        exit(-1);
    }

    m_file = g_mapped_file_new(m_fileName.toLatin1().data(),FALSE, &error);
    if(error)
    {
        g_print ("failed to open file: %s\n", error->message);
        g_error_free (error);
        exit(-1);
    }

    m_length = g_mapped_file_get_length (m_file);
    m_data = (guint8 *) g_mapped_file_get_contents (m_file);
    m_offset = 0;

    m_webSocket = new QWebSocket;

    connect(m_webSocket, &QWebSocket::connected, this, &WebRtcClient::onConnected);
    connect(m_webSocket, QOverload<const QList<QSslError>&>::of(&QWebSocket::sslErrors),
            this, &WebRtcClient::onSslErrors);
    connect(m_webSocket, &QWebSocket::stateChanged, this, &WebRtcClient::onStateChanged);

    m_webSocket->open(QUrl(m_wssUrl));
    qDebug() << "Websocket 启动成功!";
    m_appState = SERVER_CONNECTING;
}


bool WebRtcClient::check_plugins()
{
    int i;
    bool ret;

    GstPlugin *plugin;
    GstRegistry *registry;
    const gchar *needed[] = { "opus", "vpx", "alaw", "nice", "webrtc", "dtls", "srtp",
                              "rtpmanager", "videotestsrc", "audiotestsrc", nullptr};

    registry = gst_registry_get ();
    ret = true;
    for (i = 0; i < g_strv_length ((gchar **) needed); i++) {
      plugin = gst_registry_find_plugin (registry, needed[i]);
      if (!plugin)
      {
          g_print ("Required gstreamer plugin '%s' not found\n", needed[i]);
          ret = false;
          continue;
      }
      gst_object_unref (plugin);
    }
    return ret;
}


bool WebRtcClient::register_with_server()
{

    // 生成uid
    QString our_id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    m_appState = SERVER_REGISTERING;


    QString hello = "HELLO " + our_id;
    return sendMsg(hello);
}

bool WebRtcClient::setupCall()
{


    qDebug() << "Setting up signalling server call with : " << m_peerId;
    m_appState = PEER_CONNECTING;

    QString msg = "SESSION " + m_peerId;
    return sendMsg(msg);
}


bool WebRtcClient::startPipeline()
{
    GstStateChangeReturn ret;
    GError *error = nullptr;

    QString pipeStr = "webrtcbin bundle-policy=max-bundle name=sendrecv " STUN_SERVER
                "appsrc name=audiosrc "
                "! rtppcmapay ! queue ! " RTP_CAPS_PCMA "8 ! sendrecv. ";

    m_pipe1 = gst_parse_launch(pipeStr.toLatin1().data(), &error);

    if (error) {
      g_printerr ("Failed to parse launch: %s\n", error->message);
      g_error_free (error);
      goto err;
    }

    // appsrc
    m_audioSrc = (GstAppSrc*)gst_bin_get_by_name(GST_BIN(m_pipe1), "audiosrc");
    g_assert(m_audioSrc);

    /* audio setup */
    g_object_set (G_OBJECT (m_audioSrc), "caps",
          gst_caps_new_simple ("audio/x-alaw",
                       "rate", G_TYPE_INT, 8000,
                       "channels", G_TYPE_INT, 1,
                       NULL), NULL);
    g_object_set (G_OBJECT (m_audioSrc),
          "stream-type", 0, // GST_APP_STREAM_TYPE_STREAM
          "format", GST_FORMAT_TIME,
          "is-live", TRUE,
          NULL);

    //gst_app_src_set_max_bytes(m_audioSrc, 1024 * 1024);
    g_signal_connect(m_audioSrc, "need-data", G_CALLBACK(start_feed), this);
    g_signal_connect(m_audioSrc, "enough-data", G_CALLBACK(stop_feed), this);

    // webrtc
    m_webrtc1 = gst_bin_get_by_name (GST_BIN (m_pipe1), "sendrecv");
    g_assert_nonnull (m_webrtc1);

    /* This is the gstwebrtc entry point where we create the offer and so on. It
     * will be called when the pipeline goes to PLAYING. */
    g_signal_connect (m_webrtc1, "on-negotiation-needed",
        G_CALLBACK (on_negotiation_needed), this);


    /* We need to transmit this ICE candidate to the browser via the websockets
     * signalling server. Incoming ice candidates from the browser need to be
     * added by us too, see on_server_message() */
    g_signal_connect (m_webrtc1, "on-ice-candidate",
        G_CALLBACK (send_ice_candidate_message), this);

    /* Incoming streams will be exposed via this signal */
    g_signal_connect (m_webrtc1, "pad-added", G_CALLBACK (on_incoming_stream),
        m_pipe1);
    /* Lifetime is the same as the pipeline itself */
    gst_object_unref (m_webrtc1);

    g_print ("Starting pipeline\n");
    ret = gst_element_set_state (GST_ELEMENT (m_pipe1), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
      goto err;

    return true;

err:
  if (m_pipe1)
    g_clear_object (&m_pipe1);
  if (m_webrtc1)
    m_webrtc1 = nullptr;
  return FALSE;

}


bool WebRtcClient::sendMsg(QString msg)
{
    if(m_webSocket->state() == QAbstractSocket::ConnectedState)
    {
        m_webSocket->sendTextMessage(msg);
        return true;
    }

    return false;
}

void WebRtcClient::onConnected()
{
    m_appState = SERVER_CONNECTED;
    connect(m_webSocket, &QWebSocket::textMessageReceived,
            this, &WebRtcClient::onTextMessageReceived);

    connect(m_webSocket, &QWebSocket::disconnected,
            this, &WebRtcClient::onDisconnected);

    // 注册到信令服务器
    register_with_server();
}


void WebRtcClient::onDisconnected()
{
    m_appState = SERVER_CLOSED;
}


void WebRtcClient::onTextMessageReceived(QString message)
{
    qDebug() << "receive message: " << message;

    if(message.startsWith("HELLO"))
    {
        if(m_appState != SERVER_REGISTERING)
        {
            qDebug() << "ERROR: Received HELLO when not registering";
            return;
        }
        m_appState = SERVER_REGISTERED;
        qDebug() << "Registerered with server!";

        setupCall();
    }
    else if(message.startsWith("SESSION_OK"))
    {
        if(m_appState != PEER_CONNECTING)
        {
            qDebug() << "ERROR: Received SESSION_OK when not calling";
            return ;
        }

        m_appState = PEER_CONNECTED;
        if(!startPipeline())
        {
           qDebug() << "ERROR: failed to start pipeline";
        }
    }
    else if(message.startsWith("ERROR"))
    {
        switch (m_appState) {
          case SERVER_CONNECTING:
            m_appState = SERVER_CONNECTION_ERROR;
            break;
          case SERVER_REGISTERING:
            m_appState = SERVER_REGISTRATION_ERROR;
            break;
          case PEER_CONNECTING:
            m_appState = PEER_CONNECTION_ERROR;
            break;
          case PEER_CONNECTED:
          case PEER_CALL_NEGOTIATING:
            m_appState = PEER_CALL_ERROR;
          default:
            m_appState = APP_STATE_ERROR;
        }
    }
    else
    {
        QJsonDocument jsonDoc = QJsonDocument::fromJson(message.toLocal8Bit().data());

        QJsonObject jsonMsg = jsonDoc.object();
        QJsonObject jsonChild;

        if(jsonMsg.find("sdp") != jsonMsg.end())
        {
            int ret;
            GstSDPMessage *sdp;
            QString text;
            QString sdptype;
            GstWebRTCSessionDescription *answer;

            g_assert_cmphex (m_appState, ==, PEER_CALL_NEGOTIATING);

            jsonChild = jsonMsg.value("sdp").toObject();

            if(jsonChild.find("type") == jsonChild.end())
            {
                qDebug() << "ERROR: received SDP without 'type'";
                return;
            }

            sdptype = jsonChild.value("type").toString();

            /* In this example, we always create the offer and receive one answer.
             * See tests/examples/webrtcbidirectional.c in gst-plugins-bad for how to
             * handle offers from peers and reply with answers using webrtcbin. */

            if(sdptype != "answer")
            {
                return;
            }



            text = jsonChild.value("sdp").toString();

            qDebug() << "Received answer:" << text;

            ret = gst_sdp_message_new (&sdp);
            g_assert_cmphex (ret, ==, GST_SDP_OK);

            ret = gst_sdp_message_parse_buffer ((guint8 *) text.toStdString().c_str(), strlen ( text.toStdString().c_str()), sdp);
            g_assert_cmphex (ret, ==, GST_SDP_OK);

            answer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_ANSWER,sdp);
            g_assert_nonnull (answer);

            /* Set remote description on our pipeline */
            {
              GstPromise *promise = gst_promise_new ();
              g_signal_emit_by_name (m_webrtc1, "set-remote-description", answer, promise);
              gst_promise_interrupt (promise);
              gst_promise_unref (promise);
            }

            m_appState = PEER_CALL_STARTED;


        }
        else if(jsonMsg.find("ice") != jsonMsg.end())
        {
            QString candidate;
            gint sdpmlineindex;

            jsonChild = jsonMsg.value("ice").toObject();
            candidate = jsonChild.value("candidate").toString();
            sdpmlineindex = jsonChild.value("sdpMLineIndex").toInt();

            /* Add ice candidate sent by remote peer */
            g_signal_emit_by_name (m_webrtc1, "add-ice-candidate", sdpmlineindex, candidate.toStdString().c_str());
        }
        else
        {
            qDebug() << "Ignoring unknown JSON message:" << message;
        }
    }

}

void WebRtcClient::onSslErrors(const QList<QSslError> &errors)
{
    // 测试环境
    m_webSocket->ignoreSslErrors();
}


void WebRtcClient::onStateChanged(QAbstractSocket::SocketState state)
{

}
