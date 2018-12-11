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


#define LEADER 1
#define CANDIDATE 2
#define FOLLOWER 3

#define HEARTBEATTIME 50 //msec

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
    void attemptToForwardNextClientRequest();
    void sendHeartbeat();

private:
	QTextEdit *textview;
	QLineEdit *textline;

    quint16 myPort;
    quint32 mySeqNo = 1;

    quint16 timeToWaitForHeartbeat;
    QTimer *electionTimer;
    QTimer *sendHeartbeatTimer;
    QTimer *forwardClientRequestTimer;

    qint32 myCandidateID;
    quint16 myRole = FOLLOWER;
    QList<quint16> nodesThatVotedForMe; // Used by candidate in election

    qint32 myLeader = -1;
    qint32 votedFor = -1;

    quint16 myCurrentTerm = 0;
    quint16 myCommitIndex = 0; // Everyone has a dummy entry at log index 0
    quint16 myLastApplied = 0;

    // Log entry should be QVariantMap that stores messageID, term, and message
    // messageID = concat(myPort, mySeqNo)
    // Note: at startup, append a "dummy" entry into index 0 of log, so that prevLogIndex checks work
    QVariantList log;
    QVariantList queuedClientRequests;

    // should these be calculated every time? IDK, not sure about thread safety
    quint16 myLastLogIndex = 0;
    quint16 myLastLogTerm = 0;

    // After commands are committed, they are executed i.e. messages are appended
    // to the state machine
    QList<QString> stateMachine;
    QList<QString> stateMachineMessageIDs;

    // Used by leader. initializes at leader myLastLogIndex+1
    // <Server, next log entry to send to that server>
    QMap<quint16, quint16> nextIndex;

    // Used by leader. used to keep track of commit status
    // <Server, highest log entry known to be replicated on server>
    QMap<quint16, quint16> matchIndex;


    void processRequestVote(QVariantMap msg, quint16 sourcePort);
    void replyToRequestForVote(bool voteGranted, quint16 sourcePort);
    void processReplyRequestVote(QVariantMap inMap, quint16 sourcePort);

    void sendAppendEntriesMsg(quint16 destPort, bool heartbeat);
    void processAppendEntriesMsg(QVariantMap inMap, quint16 sourcePort);
    void replyToAppendEntries(bool success, quint16 prevIndex, quint16 entryLen, quint16 destPort);
    void processAppendEntriesMsgReply(QVariantMap inMap, quint16 sourcePort);
    void processSupportCommand(QString command);

    void processClientRequestFromFollower(QVariantMap inMap);
    void attemptToCommitNextClientRequest();
    void removeMessageIDFromQueuedClientRequests(QString messageID);

    void serializeMessage(QVariantMap &myMap, quint16 destPort);
    void sendMessageToAll(QVariantMap msgMap);
    void refreshTextView();


};

#endif // P2PAPP_MAIN_HH
