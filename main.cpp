#include <QCoreApplication>
#include "WebRtcClient.h"
#include <QThread>



int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    qRegisterMetaType<QAbstractSocket::SocketState>();
    qRegisterMetaType<QList<QSslError>>();

    QString fileName = "/home/liang/Documents/videos/tongren.g711a";
    WebRtcClient *client = new WebRtcClient(fileName, "135");

    QThread *myThread = new QThread;

    QObject::connect(myThread, &QThread::started, client, &WebRtcClient::doWork);
    client->moveToThread(myThread);

    myThread->start();

    return a.exec();
}
