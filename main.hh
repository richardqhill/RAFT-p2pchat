#ifndef P2PAPP_MAIN_HH
#define P2PAPP_MAIN_HH

#include <QDialog>
#include <QTextEdit>
#include <QLineEdit>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QDataStream>
#include <QTimer>
#include <QTime>
#include <QElapsedTimer>
#include <unistd.h>
#include <stdlib.h>
#include <limits>

// We start Seq No at 1 because an empty entry in QVariantMap returns 0
#define SEQNOSTART 1
#define QINT64MAX std::numeric_limits<qint64>::max()

#define LEADER 1
#define CANDIDATE 2
#define FOLLOWER 3
#define HEARTBEATTIME 50

class NetSocket : public QUdpSocket
{
    Q_OBJECT

public:
    NetSocket();
    bool bind(); // Bind this socket to a P2Papp-specific default port.
    quint16 myPortMin, myPortMax, port;
};

class ChatDialog : public QDialog
{
	Q_OBJECT

public:
    ChatDialog();
    NetSocket* mySocket;

public slots:
	void gotReturnPressed();
    void processPendingDatagrams();


    void sendRequestForVotes();
    void heartbeat();

    // OLD
    void antiEntropy();
    void resendRumor();

private:
	QTextEdit *textview;
	QLineEdit *textline;

    quint16 myCandidateId;
    quint16 myRole = FOLLOWER;
    quint16 myLeader = -1;
    quint16 votedFor = -1;
    quint16 lastTermIVotedTrueIn = -1;

    quint16 timeToWaitForHeartbeat;
	QTimer *waitForHeartbeatTimer;
    QTimer *sendHeartbeatTimer;

    quint16 lastLogIndex=0;
    quint16 lastLogTerm=0;
    quint16 currentTerm=0;

    QList<quint16> nodesThatVotedForMe;

    // List of (message, term) pairs
    QList<QPair<QString, quint16>> committedMsgs;

    //QMap<quint32, QList> uncommittedMsgs;


    void sendMessageToAll(QVariantMap msgMap);
    void processRequestVote(QVariantMap msg, quint16 sourcePort);
    void processReplyRequestVote(QVariantMap inMap, quint16 sourcePort);
    void sendAppendEntriesMsg();
    void processAppendEntriesMsg(QVariantMap inMap, quint16 sourcePort);
    void processAppendEntriesMsgReply(QVariantMap inMap, quint16 sourcePort);


	// OLD CODE
    QTimer *resendTimer;
    QTimer *antiEntropyTimer;
    QElapsedTimer *n1Timer = nullptr;
    QElapsedTimer *n2Timer = nullptr;
    qint64 n1Time = QINT64MAX;
    qint64 n2Time = QINT64MAX;

    quint16 myPort;
    QString myOrigin;
    quint32 mySeqNo;

    // chatLogs: <Origin, <SeqNo, Message>>
    QMap<QString, QMap<quint32, QString>> chatLogs;

    // statusMap: <Origin, QVariant(LastSeqNo + 1)>
    QVariantMap statusMap;

    // Last sent rumor message
    quint16 lastRumorPort;
    QString lastRumorOrigin;
    quint32 lastRumorSeqNo;

    quint16 pickClosestNeighbor();
    quint16 pickRandomNeighbor();
    void sendRumorMessage(QString origin, quint32 seqNo, quint16 destPort);
    void serializeMessage(QVariantMap &myMap, quint16 destPort);
    void deserializeMessage(QByteArray datagram);
    void receiveRumorMessage(QVariantMap inMap, quint16 sourcePort);
    void sendStatusMessage(quint16 destPort);
    void receiveStatusMessage(QVariantMap inMap, quint16 sourcePort);

};

#endif // P2PAPP_MAIN_HH
