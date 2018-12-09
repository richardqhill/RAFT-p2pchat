#include "main.hh"

ChatDialog::ChatDialog(){

    mySocket = new NetSocket();
    if(!  mySocket->bind()) {
		exit(1);
	}
    else {
		myPort = mySocket->port;
	}


    myCandidateId = myPort;

	mySeqNo = 1;
	myOrigin = QString::number(myPort);

	qDebug() << "myOrigin: " << myOrigin;
	qDebug() << "-------------------";

    setWindowTitle("RAFT Chat: " + myOrigin);

	textview = new QTextEdit(this);
	textview->setReadOnly(true);
	textline = new QLineEdit(this);
	QVBoxLayout *layout = new QVBoxLayout();
	layout->addWidget(textview);
	layout->addWidget(textline);
	setLayout(layout);

	// Register a callback on the textline for returnPressed signal
	connect(textline, SIGNAL(returnPressed()),
		this, SLOT(gotReturnPressed()));

    // Register a callback when another p2p app sends us a message over UDP
    connect(mySocket, SIGNAL(readyRead()),
    		this, SLOT(processPendingDatagrams()));


    // Timeout waiting for leader heartbeat. If no heartbeat within waitForHeartbeat msec, attempt to become leader
    qsrand(QTime::currentTime().msec());

    //timeToWaitForHeartbeat = qrand() % 150 + 150;
    timeToWaitForHeartbeat = qrand() % 150 + 1000;

    waitForHeartbeatTimer = new QTimer(this);
    connect(waitForHeartbeatTimer, SIGNAL(timeout()), this, SLOT(sendRequestForVotes()));
    waitForHeartbeatTimer->start(timeToWaitForHeartbeat);

    // Timer for leader to keep track of when to send heartbeats.
    // Started when follower becomes leader, timeout defined in header
    sendHeartbeatTimer = new QTimer(this);
    connect(sendHeartbeatTimer, SIGNAL(timeout()), this, SLOT(sendHeartbeat()));

}

/* Called by follower who's waitForHeartbeatTimer timed out. Starts a new election */
void ChatDialog::sendRequestForVotes(){

    qDebug() << "Attempting to become leader!";

    currentTerm++;
    nodesThatVotedForMe.clear();
    votedFor = myCandidateId; // probably doesn't matter
    nodesThatVotedForMe.append(myPort);
    myRole = CANDIDATE;

    // Create requestVote message
    QVariantMap requestVoteMap;
    requestVoteMap.insert(QString("type"), QVariant(QString("requestVote")));
    requestVoteMap.insert(QString("term"), QVariant(currentTerm));
    requestVoteMap.insert(QString("candidateId"), QVariant(myCandidateId));
    requestVoteMap.insert(QString("lastLogIndex"), QVariant(lastLogIndex));
    requestVoteMap.insert(QString("lastLogTerm"), QVariant(lastLogTerm));

    sendMessageToAll(requestVoteMap);
}

/* Called by follower to process a received a RequestVote message */
void ChatDialog::processRequestVote(QVariantMap inMap, quint16 sourcePort){

    qDebug()<< "Processing Request Vote";

    quint16 candidateTerm = inMap["term"].toInt();
    quint16 candidateID = inMap["candidateId"].toInt();
    quint16 candidateLastLogIndex = inMap["lastLogIndex"].toInt();
    quint16 candidateLastLogTerm = inMap["lastLogTerm"].toInt();


    if(candidateTerm < currentTerm) {
        qDebug() << "Vote was denied because candidate term is less than mine!";
        replyToRequestForVote(false, sourcePort);
        return;
    }

    // Check if candidate's log is at least as up-to-date as receiver's log
    if(candidateLastLogTerm < lastLogTerm ||
    (candidateLastLogTerm==lastLogTerm && candidateLastLogIndex < lastLogIndex)){
        qDebug() << "Vote was denied because candidate's log is not as up to date as mine!";
        replyToRequestForVote(false, sourcePort);
        return;
    }

    qDebug() << "Voted for " + QString::number(votedFor);
    qDebug() << "Candidate ID " + QString::number(candidateID);
    if(votedFor == -1 || votedFor == candidateID) {
        currentTerm = candidateTerm;
        votedFor = candidateID;
        replyToRequestForVote(true, sourcePort);
        return;
    }
    else{
        qDebug() << "Vote was denied because I have already voted for someone else this term!";
        replyToRequestForVote(false, sourcePort);
        return;
    }
}

/* Called by follower to reply to a received RequestVote message */
void ChatDialog::replyToRequestForVote(bool voteGranted, quint16 sourcePort){

    // reset waitForHeartbeatTimer, according to Raft visual
    waitForHeartbeatTimer->start(timeToWaitForHeartbeat);

    // Send reply to RequestVote
    QVariantMap replyRequestVoteMap;
    replyRequestVoteMap["type"] = QString::fromStdString("replyRequestVote");
    replyRequestVoteMap["term"] = QVariant(currentTerm);

    if(voteGranted) {
        qDebug() << "Vote was granted!";
        replyRequestVoteMap["voteGranted"] = QVariant(true);
    }
    else {
        replyRequestVoteMap["voteGranted"] = QVariant(false);
    }

    serializeMessage(replyRequestVoteMap, sourcePort);


}

/* Called by candidate to process a reply to their RequestVote message */
void ChatDialog::processReplyRequestVote(QVariantMap inMap, quint16 sourcePort){

    quint16 term = inMap["term"].toInt();
    bool voteGranted = inMap["voteGranted"].toBool();




    if(term == currentTerm){
        if(voteGranted){
            qDebug() << "Vote was granted!";
            nodesThatVotedForMe.append(sourcePort); //could potentially check first is list already contains sourcePort

            if(nodesThatVotedForMe.length() >= 2){ //should be 3, changing to 2 for debugging
                qDebug() << "I became leader!";

                myRole = LEADER;
                myLeader = myCandidateId; //important?

                // Initialize next index
                nextIndex.clear();
                for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
                    if(i!= myPort)
                        nextIndex[i] = lastLogIndex+1;
                }

                // send initial heartbeat and start a timer to continually send heartbeats
                sendHeartbeat();
                sendHeartbeatTimer->start(HEARTBEATTIME);
            }
        }
    }
}




/* Called by leader to attempt to commit most recent entry in log */
void ChatDialog::sendAppendEntriesMsg(quint16 prevLogIndex, quint16 destPort){ //THIS NEEDS PARAMETERS

    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("appendEntries")));
    outMap.insert(QString("term"), QVariant(currentTerm));
    outMap.insert(QString("leaderId"), QVariant(myCandidateId));

    outMap.insert(QString("prevLogIndex"), QVariant(prevLogIndex));
    quint16 prevLogTerm = log.at(prevLogIndex).second.second;
    outMap.insert(QString("prevLogTerm"), QVariant(prevLogTerm));

    outMap.insert(QString("leadCommit"), QVariant(commitIndex));


    // ADD VALUES
    qDebug() << "hi";

}





/* Called by follower to process an AppendEntries message from leader */
void ChatDialog::processAppendEntriesMsg(QVariantMap inMap, quint16 sourcePort){

    qDebug() << "Received heartbeat as follower";

    waitForHeartbeatTimer->start(timeToWaitForHeartbeat); //restart timer
    votedFor = -1;

    // NEED TO CHECK FOR OBSOLETE LEADER, IF MY LOG IS MORE UP TO DATE THAN YOURS


    // check if this is a heartbeat
    // update my leader to be source port?

    // CHECK TERM IS HIGHER THAN MY OWN
    if(myRole == CANDIDATE){
        myRole = FOLLOWER;
    }
    // WHEN RECEIVING APPEND ENTRIES, CHECK IF YOU ARE A CANDIDATE


    // REPLY TRUE OR FALSE BASED ON STUFF

    // IF TRUE, I SHOULD UPDATE THE DISPLAY WITH THE NEW LOG
//    textview->setTextColor(QColor("blue"));
//    textview->append("Some junk");
//    textview->setTextColor(QColor("black"));


    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("replyAppendEntries")));
    sendMessageToAll(outMap);

}

/* Called by leader to process a follower's reply to their AppendEntries message */
void ChatDialog::processAppendEntriesMsgReply(QVariantMap inMap, quint16 sourcePort){

    // DECIDE IF I NEED TO SEND MORE APPEND ENTRIES with more entries (farther back)


    //if true
    nodesThatVotedForCommit.append(sourcePort);

    //check if majority

    // if majority, remove this from
    // add to committed
    // UPDATE COMMIT INDEX PLZ
    // remove from commmited




    return;
}




void ChatDialog::processPendingDatagrams(){

    while(mySocket->hasPendingDatagrams()){
        QByteArray datagram;
        datagram.resize(mySocket->pendingDatagramSize());
        QHostAddress source;
        quint16 sourcePort;

        if(mySocket->readDatagram(datagram.data(), datagram.size(), &source, &sourcePort) != -1){

            QDataStream inStream(&datagram, QIODevice::ReadOnly);
            QVariantMap inMap;
            inStream >> inMap;

            if(inMap.contains("type")){
                if(inMap["type"] == "requestVote"){
                    qDebug() << "Received a request vote!";
                    processRequestVote(inMap, sourcePort);
                }

                else if(inMap["type"] == "replyRequestVote"){
                    qDebug() << "Received a request vote reply!";
                    processReplyRequestVote(inMap, sourcePort);
                }

                else if(inMap["type"] == "appendEntries"){
                    //qDebug() << "Received an append Entries message!";
                    processAppendEntriesMsg(inMap, sourcePort);
                }

                else if(inMap["type"] == "replyAppendEntries"){
                    //qDebug() << "Received an append Entries reply!";
                    processAppendEntriesMsgReply(inMap, sourcePort);
                }
            }
        }
    }
}

void ChatDialog::gotReturnPressed(){




    // NEED TO PARSE FOR SUPPORT COMMANDS




    // Store "Client" request

    QString messageID = QString(myPort) + QString(mySeqNo);
    mySeqNo++;

    QString message = QString::number(myPort) + ": " + textline->text();

    queuedClientRequests.append(qMakePair(message, qMakePair(messageID, currentTerm)));

    if(myRole == LEADER){


        attemptToCommitMsg();
    }




    else{    // I AM A CLIENT


        // SEND TO "SERVER" but wait for Leader to respond with success until I move on


        //attempt to forward messages from uncommitted to leader
        qDebug() << "hi";



    }


    textline->clear();
}

void ChatDialog::attemptToCommitMsg(){


    log.append(queuedClientRequests.first());

    // Send messages AppendEntries RPC everyone


    stateMachine.append(queuedClientRequests.first().first);
    refreshTextView();


//    while(!uncommittedMsgs.isEmpty()){
//
//        nodesThatVotedForCommit.clear();
//
//        uncommittedMsgs.first();
//
//        sendAppendEntriesMsg();
//
//        processPendingDatagrams();
//
//    }


}










/* ---------------------------------------------------------------------------------------------------------*/
// Probably good ///

void ChatDialog::sendHeartbeat(){

    qDebug() << "Sending heartbeat as leader";

    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("appendEntries")));
    sendMessageToAll(outMap);

    // Prevent leader from trying to depose themselves
    waitForHeartbeatTimer->start(timeToWaitForHeartbeat);
}

void ChatDialog::refreshTextView(){

    textview->clear();
    for(int i=0; i< stateMachine.length(); i++){

        textview->append(stateMachine.at(i));
    }
}

void ChatDialog::serializeMessage(QVariantMap &outMap, quint16 destPort){

    QByteArray outData;
    QDataStream outStream(&outData, QIODevice::WriteOnly);
    outStream << outMap;

    mySocket->writeDatagram(outData.data(), outData.size(), QHostAddress::LocalHost, destPort);
}

void ChatDialog::sendMessageToAll(QVariantMap msgMap){

    for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
        if(i!= myPort)
            serializeMessage(msgMap, i);
    }
}

/* ---------------------------------------------------------------------------------------------------------*/
// PURGATORY OF OLD CODE ///

quint16 ChatDialog::pickClosestNeighbor(){

    return -1;

    qDebug() << "Picking closest neighbor";

    int maxAttempts = 10;
    quint16 neighborPort;

    if(myPort == mySocket->myPortMin)
        neighborPort = myPort+1;

    else if(myPort == mySocket->myPortMax)
        neighborPort = myPort-1;

    // Send ping, collect ping reply to determine which neighbor is closer
    else{
        quint16 n1 = myPort - 1;
        quint16 n2 = myPort + 1;

        n1Timer = new QElapsedTimer();
        n1Timer->start();
        n2Timer = new QElapsedTimer();
        n2Timer->start();

        QVariantMap pingMapN1, pingMapN2;
        pingMapN1.insert(QString("Ping"), QVariant(1));
        pingMapN2.insert(QString("Ping"), QVariant(2));

        // It's possible to NOT get a response from one neighbor or both neighbors.
        // Need max attempts in case both neighbors are down or else hangs forever.
        int attempts = 0;
        while (n1Time == QINT64MAX && n2Time == QINT64MAX && attempts <= maxAttempts) {
            qDebug() << "Sending pings to both neighbors";
            serializeMessage(pingMapN1, n1);
            serializeMessage(pingMapN2, n2);
            processPendingDatagrams();

            // Sleep for 0.1 seconds to allow for time for ping reply
            usleep(100);
            attempts++;
        }

        qDebug() << "N1 response took" << QString::number(n1Time) << "milliseconds";
        qDebug() << "N2 response took" << QString::number(n2Time) << "milliseconds";

        if(attempts == maxAttempts)
            neighborPort = pickRandomNeighbor();
        else if (n1Time < n2Time)
            neighborPort = n1;
        else
            neighborPort = n2;

        // Reset timer states
        attempts =0;
        delete n1Timer;
        delete n2Timer;
        n1Timer = nullptr;
        n2Timer = nullptr;
        n1Time = QINT64MAX;
        n2Time = QINT64MAX;
    }
    qDebug() << "Picked neighbor" << QString::number(neighborPort);
    return neighborPort;
}

quint16 ChatDialog::pickRandomNeighbor() {

    return -1;

    quint16 neighborPort;

    if(myPort == mySocket->myPortMin)
        neighborPort = myPort+1;

    else if(myPort == mySocket->myPortMax)
        neighborPort = myPort-1;

    else if (qrand() % 2 == 0)
        neighborPort = myPort+1;

    else
        neighborPort = myPort-1;

    //qDebug() << "Picked neighbor " + QString::number(neighborPort);
    return neighborPort;
}

void ChatDialog::sendRumorMessage(QString origin, quint32 seqNo, quint16 destPort){

	if(!chatLogs.contains(origin) || !chatLogs[origin].contains(seqNo))
		return;

	QVariantMap rumorMap;
	rumorMap.insert(QString("ChatText"), chatLogs[origin].value(seqNo));
	rumorMap.insert(QString("Origin"), origin);
	rumorMap.insert(QString("SeqNo"), seqNo);

	qDebug() << "Sending Origin/seqNo " + origin + "/" + QString::number(seqNo) + " to: " + QString::number(destPort);

	serializeMessage(rumorMap, destPort);

    // Start Timer looking for response
    resendTimer->start(2000);
    lastRumorPort = destPort;
    lastRumorOrigin = origin;
    lastRumorSeqNo = seqNo;
}

void ChatDialog::antiEntropy() {
    //qDebug() << "Anti Entropy!";
    sendStatusMessage(pickRandomNeighbor());
}

void ChatDialog::resendRumor(){
    qDebug() << "Resending rumor because no status message reply!";
    sendRumorMessage(lastRumorOrigin, lastRumorSeqNo, lastRumorPort);
}

void ChatDialog::receiveRumorMessage(QVariantMap inMap, quint16 sourcePort){

    QString origin = inMap.value("Origin").value <QString> ();
	quint32 seqNo = inMap.value("SeqNo").value <quint32> ();
	QString msg = inMap.value("ChatText").value <QString> ();

    qDebug() << "I got a rumor message: " + msg + " from: " + QString::number(sourcePort);

	QMap<quint32, QString> chatLogEntry;
	chatLogEntry.insert(seqNo, msg);

	// For convenience, do not store any message with OOO seq number but reply with status message
	// If chatLogs does not contain messages from this origin, we expect message with seqNo 1
	if(!chatLogs.contains(origin)){
		if(seqNo != SEQNOSTART) {
            sendStatusMessage(sourcePort);
            sendRumorMessage(origin, seqNo, pickRandomNeighbor());
            return;
        }

        chatLogs.insert(origin, chatLogEntry);
        statusMap.insert(origin, QVariant(seqNo + 1));
        textview->append(origin + ": " + msg);

	}
	//If chatLogs *does* contain this origin, we expect message with seqNo = last seq num + 1
	else{
		quint32 lastSeqNum = chatLogs[origin].keys().last();

		if (seqNo == lastSeqNum + 1){
			chatLogs[origin].insert(seqNo, msg);
			statusMap[origin] = QVariant(seqNo + 1);
			textview->append(origin + ": " + msg);
		}
	}
	sendStatusMessage(sourcePort);
	sendRumorMessage(origin, seqNo, pickRandomNeighbor());
}

void ChatDialog::sendStatusMessage(quint16 destPort){

	QVariantMap statusMessage;
	statusMessage.insert(QString("Want"), statusMap);
	serializeMessage(statusMessage, destPort);

}

void ChatDialog::receiveStatusMessage(QVariantMap inMap, quint16 sourcePort){

	QVariantMap recvStatusMap = inMap["Want"].value <QVariantMap> ();
	QList<QString> recvOriginList = recvStatusMap.keys();
    QList<QString> myOriginList = statusMap.keys();

    //qDebug() << "I got a status message from: " + QString::number(sourcePort);
    //qDebug() << "recvStatusMap: " << recvStatusMap;
    //qDebug() << "myStatusMap: " << statusMap;

	for(int i=0; i < myOriginList.count(); i++){
	    if(!recvOriginList.contains(myOriginList[i])) {
            sendRumorMessage(myOriginList[i], SEQNOSTART, sourcePort);
            return;
        }
	}

	for(int i=0; i< recvOriginList.count(); i++){

	    quint32 sourceSeqNoForOrigin = recvStatusMap[recvOriginList[i]].value <quint32> ();
	    quint32 mySeqNoForOrigin = statusMap[recvOriginList[i]].value <quint32> ();

        if(sourceSeqNoForOrigin == mySeqNoForOrigin)
            continue;

        else if(sourceSeqNoForOrigin > mySeqNoForOrigin) {
            sendStatusMessage(sourcePort);
            return;
        }

        else if(sourceSeqNoForOrigin < mySeqNoForOrigin) {

            // 0 means sourceMap is empty for this Origin since seqNo starts at 1!
            if(sourceSeqNoForOrigin == 0)
                sendRumorMessage(recvOriginList[i], SEQNOSTART, sourcePort);
            else
                sendRumorMessage(recvOriginList[i], sourceSeqNoForOrigin, sourcePort);
            return;
        }
    }

    // Neither peer appears to have any messages the other has not yet seen.
    // Flip a coin: pick new random neighbor to start rumormongering with or ceases the rumormongering process.
    if (qrand() % 2 == 0){
        sendStatusMessage(pickRandomNeighbor());
    }
}




// BASE CODE

/* Pick a range of four UDP ports to try to allocate by default, computed based on my Unix user ID.*/
NetSocket::NetSocket(){
	// This makes it trivial for up to four P2Papp instances per user to find each other on the same host,
	// barring UDP port conflicts with other applications (which are quite possible). We use the range from
	// 32768 to 49151 for this purpose.
	myPortMin = 32768 + (getuid() % 4096)*4;
	myPortMax = myPortMin + 4; // Edit to support 5 max users!

}

/* Bind one of ports between myPortMin and myPortMax */
bool NetSocket::bind(){
	for (int p = myPortMin; p <= myPortMax; p++) {
		if (QUdpSocket::bind(p)) {
			qDebug() << "bound to UDP port " << p;
			port = p;
			return true;
		}
	}
	qDebug() << "Oops, no ports in my default range " << myPortMin << "-" << myPortMax << " available";
	return false;
}

int main(int argc, char **argv){
	// Initialize Qt toolkit
	QApplication app(argc,argv);

	// Create an initial chat dialog window
	ChatDialog dialog;
	dialog.show();

	// Enter the Qt main loop; everything else is event driven
	return app.exec();
}

