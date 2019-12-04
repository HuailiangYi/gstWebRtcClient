#ifndef WEBRTCCLIENT_H
#define WEBRTCCLIENT_H
#include <QThread>

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <gst/app/gstappsrc.h>
#include <string.h>

#define GST_USE_UNSTABLE_API

#include <QWebSocket>
#include <QList>
#include <QSslError>
#include <QString>
#include <QUrl>
#include <QObject>


enum AppState {
  APP_STATE_UNKNOWN = 0,
  APP_STATE_ERROR = 1, /* generic error */
  SERVER_CONNECTING = 1000,
  SERVER_CONNECTION_ERROR,
  SERVER_CONNECTED, /* Ready to register */
  SERVER_REGISTERING = 2000,
  SERVER_REGISTRATION_ERROR,
  SERVER_REGISTERED, /* Ready to call a peer */
  SERVER_CLOSED, /* server connection closed by us or the server */
  PEER_CONNECTING = 3000,
  PEER_CONNECTION_ERROR,
  PEER_CONNECTED,
  PEER_CALL_NEGOTIATING = 4000,
  PEER_CALL_STARTED,
  PEER_CALL_STOPPING,
  PEER_CALL_STOPPED,
  PEER_CALL_ERROR,
};

class WebRtcClient : public QObject
{
public:
    WebRtcClient(QString fileName, QString peerId, QString wssUrl="wss://localhost:8443");
    ~WebRtcClient();


protected:
    static void start_feed(GstElement* appsrc, guint unused_size, gpointer userData);
    static void stop_feed(GstElement* appsrc,  gpointer userData);
    static gboolean push_buffer(gpointer userData);


    static void on_negotiation_needed (GstElement * element, gpointer user_data);
    static void on_offer_created (GstPromise * promise, gpointer user_data);
    static void send_ice_candidate_message (GstElement * webrtc G_GNUC_UNUSED, guint mlineindex,
                                            gchar * candidate, gpointer user_data);

    static void connect_data_channel_signals (GObject * data_channel);
    static void on_data_channel (GstElement * webrtc, GObject * data_channel, gpointer user_data);

    static void on_incoming_stream (GstElement * webrtc, GstPad * pad, GstElement * pipe);
    static void on_incoming_decodebin_stream (GstElement * decodebin, GstPad * pad, GstElement * pipe);
    static void handle_media_stream (GstPad * pad, GstElement * pipe, const char * convert_name, const char * sink_name);

public:
    void send_sdp_offer (GstWebRTCSessionDescription * offer);


private:
    bool check_plugins ();
    bool register_with_server();

    bool setupCall();
    bool startPipeline();

    bool sendMsg(QString msg);

public slots:
    void doWork();
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(QString message);
    void onSslErrors(const QList<QSslError> &errors);
    void onStateChanged(QAbstractSocket::SocketState state);


public:
    GstElement *m_pipe1;
    GstElement *m_webrtc1;
    GObject *m_sendChannel;
    GObject *m_receiveChannel;

    QWebSocket  *m_webSocket;
    enum AppState m_appState;
    QString  m_peerId;
    QString  m_wssUrl;
    gboolean disable_ssl = FALSE;


    GMappedFile *m_file;
    guint8 *m_data;
    gsize m_length;
    guint64 m_offset;
    QString m_fileName;

    GstAppSrc *m_audioSrc;
    guint     m_source_id;
};

#endif // WEBRTCCLIENT_H
