#include "main.hh"

ChatDialog::ChatDialog(){

    mySocket = new NetSocket();
    if(!  mySocket->bind()) {
		exit(1);
	}

    myPort = mySocket->port;
    myCandidateID = myPort;

    // Append a "dummy" entry into index 0 of log, so that prevLogIndex checks work
    QVariantMap dummyEntry;
    log.append(dummyEntry); //myLastLogIndex=0

	qDebug() << "myPort: " << QString::number(myPort);
	qDebug() << "-------------------";

    setWindowTitle("RAFT Chat: " + QString::number(myPort));

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

    // Timer if server has not received a heartbeat, they start an election
    electionTimer = new QTimer(this);
    connect(electionTimer, SIGNAL(timeout()), this, SLOT(sendRequestForVotes()));
    timeToWaitForHeartbeat = qrand() % 150 + 150;
    electionTimer->start(timeToWaitForHeartbeat);

    // Timer for leader to keep track of when to send heartbeats.
    // Started when follower becomes leader, timeout defined in header definitions
    sendHeartbeatTimer = new QTimer(this);
    connect(sendHeartbeatTimer, SIGNAL(timeout()), this, SLOT(sendHeartbeat()));

    // Timer if server has not received a heartbeat, they start an election
    forwardClientRequestTimer = new QTimer(this);
    connect(forwardClientRequestTimer, SIGNAL(timeout()), this, SLOT(attemptToForwardNextClientRequest()));
    forwardClientRequestTimer->start(50);
}

/* Called by follower whose waitForHeartbeatTimer timed out. Starts a new election */
void ChatDialog::sendRequestForVotes(){

    qDebug() << "Attempting to become leader!";

    myCurrentTerm++;
    nodesThatVotedForMe.clear();
    nodesThatVotedForMe.append(myPort);
    votedFor = myCandidateID;
    myRole = CANDIDATE;

    // Create requestVote message
    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("requestVote")));
    outMap.insert(QString("term"), QVariant(myCurrentTerm));
    outMap.insert(QString("candidateID"), QVariant(myCandidateID));
    outMap.insert(QString("lastLogIndex"), QVariant(myLastLogIndex));
    outMap.insert(QString("lastLogTerm"), QVariant(myLastLogTerm));

    sendMessageToAll(outMap);
}

/* Called by follower to process a received a RequestVote message */
void ChatDialog::processRequestVote(QVariantMap inMap, quint16 sourcePort){

    qDebug()<< "Processing Request Vote";

    // Load parameters from message
    quint16 candidateTerm = inMap.value("term").toInt();
    qint32  candidateID = inMap.value("candidateID").toInt();
    quint16 candidateLastLogIndex = inMap.value("lastLogIndex").toInt();
    quint16 candidateLastLogTerm = inMap.value("lastLogTerm").toInt();


    // Rules for All Servers 5.1: If RPC request or response contains term T > currentTerm,
    // set currentTerm = T and convert to follower
    if(candidateTerm > myCurrentTerm){
        myCurrentTerm = candidateTerm;
        myRole = FOLLOWER;
        sendHeartbeatTimer->stop();
    }

    if(candidateTerm < myCurrentTerm) {
        qDebug() << "Vote was denied because candidate term is less than mine!";
        replyToRequestForVote(false, sourcePort);
        return;
    }

    // Check if candidate's log is at least as up-to-date as receiver's log
    if(candidateLastLogTerm < myLastLogTerm ||
    (candidateLastLogTerm==myLastLogTerm && candidateLastLogIndex < myLastLogIndex)){
        qDebug() << "Vote was denied because candidate's log is not as up to date as mine!";
        replyToRequestForVote(false, sourcePort);
        return;
    }

    if(votedFor == -1 || votedFor == candidateID) {

        // Convert to follower
        myRole = FOLLOWER;
        sendHeartbeatTimer->stop();

        // Vote for candidate
        votedFor = candidateID;
        replyToRequestForVote(true, sourcePort);

        // 5.2 Reset election timer when you grant a vote to another peer
        electionTimer->start(timeToWaitForHeartbeat);
        return;
    }
    else{
        qDebug() << "Vote was denied because I have already voted for someone else this term!";
        replyToRequestForVote(false, sourcePort);
        return;
    }
}

/* Helper fx: Called by follower to reply to a received RequestVote message */
void ChatDialog::replyToRequestForVote(bool voteGranted, quint16 sourcePort){

    // Send reply to RequestVote
    QVariantMap outMap;
    outMap.insert("type", QVariant("replyToRequestVote"));
    outMap.insert("term", QVariant(myCurrentTerm));

    if(voteGranted) {
        qDebug() << "Vote was granted!";
        outMap.insert("voteGranted", QVariant(true));
    }
    else {
        outMap.insert("voteGranted", QVariant(false));
    }

    serializeMessage(outMap, sourcePort);
}

/* Called by candidate to process a reply to their RequestVote message */
void ChatDialog::processReplyRequestVote(QVariantMap inMap, quint16 sourcePort){

    // Load parameters from message
    quint16 termFromReply = inMap.value("term").toInt();
    bool voteGranted = inMap.value("voteGranted").toBool();

    // Rules for All Servers 5.1: If RPC request or response contains term T > currentTerm,
    // set currentTerm = T and convert to follower
    if(termFromReply > myCurrentTerm){
        myRole = FOLLOWER;
        sendHeartbeatTimer->stop();
        myCurrentTerm = termFromReply;
        return;
    }

    // If vote is not for the current election I am running, ignore reply
    if(termFromReply != myCurrentTerm)
        return;

    if(voteGranted){
        if(!nodesThatVotedForMe.contains(sourcePort))
            nodesThatVotedForMe.append(sourcePort);

        // Check if I have a majority of votes
        if(nodesThatVotedForMe.length() >= 2){        //should be 3, changing to 2 for debugging         !!!!!!!!!

            qDebug() << "I became leader!";

            myRole = LEADER;
            myLeader = myCandidateID;

            // Initialize next index: tracks for each server the next log entry I should send them
            nextIndex.clear();
            for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
                if(i!= myPort)
                    nextIndex[i] = myLastLogIndex+1;
            }

            // Initialize match index: tracks for each server the index of highest log entry known
            // to be replicated on server
            matchIndex.clear();
            for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
                if(i!= myPort)
                    matchIndex[i] = 0;
            }

            // send initial heartbeat and start a timer to continually send heartbeats
            sendHeartbeat();
            sendHeartbeatTimer->start(HEARTBEATTIME);
        }
    }
}

/* Called by leader sends a heartbeat to all followers every 50 msec*/
void ChatDialog::sendHeartbeat(){

    for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
        if(i!= myPort)
            sendAppendEntriesMsg(i, true); //2nd arg: true if heartbeat
    }

    // Prevent leader from trying to depose themselves
    electionTimer->start(timeToWaitForHeartbeat);

    if(myCommitIndex == myLastLogIndex){
        attemptToCommitNextClientRequest();
    }
}

/* Called by leader to replicate its log onto its followers */
void ChatDialog::sendAppendEntriesMsg(quint16 destPort, bool heartbeat){

    // We have a separate heartbeat function.
    // Calling this function guarantees there is at least one real entry in the log

    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("appendEntries")));
    outMap.insert(QString("term"), QVariant(myCurrentTerm));
    outMap.insert(QString("leaderID"), QVariant(myCandidateID));
    outMap.insert(QString("leaderCommit"), QVariant(myCommitIndex));

    // nextIndex default at 1 for empty log
    quint16 nextIndexForPort = nextIndex[destPort];
    quint16 prevLogIndex = nextIndexForPort-1; // is 0
    outMap.insert(QString("prevLogIndex"), QVariant(prevLogIndex));

    // prevLogTerm = term of the entry at prevLogIndex
    quint16 myPrevLogTerm = log.value(prevLogIndex).toMap().value("term").toInt();
    outMap.insert(QString("prevLogTerm"), QVariant(myPrevLogTerm));
    //qDebug() << "The prevLogTerm to the entries is: " + QString::number(myPrevLogTerm);    // Come back and check this once we are updating nextIndex

    // Copies a list of values starting at 1st arg, if 2nd arg = -1, copy to end
    // Note, if nextIndexForPort is larger than actual indexes in log, mid fx will return a blank list
    if(!heartbeat){
        QVariantList entries = log.mid(nextIndexForPort, -1);
        outMap.insert(QString("entries"), QVariant(entries));
    }

    serializeMessage(outMap, destPort);
}

/* Called by follower to process an AppendEntries message from leader */
void ChatDialog::processAppendEntriesMsg(QVariantMap inMap, quint16 sourcePort){

    // Load parameters from message
    quint16 leaderTerm = inMap.value("term").toInt();
    qint32  leaderID = inMap.value("leaderID").toInt();
    quint16 leaderCommitIndex = inMap.value("leaderCommit").toInt();
    quint16 leaderPrevLogIndex = inMap.value("prevLogIndex").toInt();
    quint16 leaderPrevLogTerm = inMap.value("prevLogTerm").toInt();
    QVariantList entries = inMap.value("entries").toList(); // Entries might be blank if heartbeat

    // Rules for All Servers 5.1: If RPC request or response contains term T > currentTerm,
    // set currentTerm = T and convert to follower
    if(leaderTerm > myCurrentTerm){
        myRole = FOLLOWER;
        sendHeartbeatTimer->stop();
        myCurrentTerm = leaderTerm;
        myLeader = leaderID;  // ?????
        votedFor = -1; // The previous election we voted in is over, reset votedFor so we can vote in the next election
        return;
    }

    // Only reset election timer if we receive RPC from CURRENT leader
    if(leaderTerm == myCurrentTerm) {
        myLeader = leaderID;
        electionTimer->start(timeToWaitForHeartbeat);
    }

    if(leaderTerm < myCurrentTerm){
        qDebug() << "Replied false to Append Entries because my current term > leader term";
        replyToAppendEntries(false, 0, 0, sourcePort);
        return;
    }

    if(inMap.contains("entries")){
        //qDebug() << entries;
        qDebug() << entries.value(0).toMap().value("message").toString();
    }

    // Reply false if log is shorter than prevLogIndex
    if(leaderPrevLogIndex >= log.length()){
        qDebug() << "Replied false to Append Entries because leaderPrevLogIndex longer that my log length";
        qDebug() << "leaderPrevLogIndex: " + QString::number(leaderPrevLogIndex);
        qDebug() << "log.length(): " + QString::number(log.length());
        replyToAppendEntries(false, 0, 0, sourcePort);
        return;
    }

    // Reply false if log doesn't contain an entry at prevLogIndex whose term matches prevLogTerm
    quint16 myTermAtPrevLogIndex = log.at(leaderPrevLogIndex).toMap().value("term").toInt();
    //qDebug() << "myTermAtPrevLogIndex: " + QString::number(myTermAtPrevLogIndex);     // Come back and check this once we are updating nextIndex

    if(myTermAtPrevLogIndex != leaderPrevLogTerm){
        qDebug() << "Replied false to Append Entries b/c terms at prevLogIndex didn't match, need more entries";
        replyToAppendEntries(false, 0, 0, sourcePort);
        return;
    }

    // We're going to accept entries from leader!

    // If leaderPrevLogIndex is the last index we have, then append all entries to end
    if(log.length() == leaderPrevLogIndex + 1){
        log.append(entries);
        myLastLogIndex = log.length() - 1;
        myLastLogTerm = log.value(myLastLogIndex).toMap().value("term").toInt();
    }
    else{
        // For every entry in entries
        for(int i = 0; i < entries.length(); i++){

            qint16 leaderEntryTerm = entries.first().toMap().value("term").toInt();
            qint16 myEntryTerm = log.value(leaderPrevLogIndex+1+i).toMap().value("term").toInt();

            // If terms at index match, we assume content matches. Do not need to append
            if(myEntryTerm == leaderEntryTerm){
                entries.removeFirst();
            }
            else{ // We found a mismatch, so we delete the mismatch entry and all that follow it
                log = log.mid(0, leaderPrevLogIndex + i + 1); // hard to test
                break;
            }
        }
        if(entries.length() > 0){
            log.append(entries);
            myLastLogIndex = log.length() - 1;
            myLastLogTerm = log.value(myLastLogIndex).toMap().value("term").toInt();
        }
    }


    // Debugging Print statement
    if(inMap.contains("entries"))
        qDebug() << "Append entries (non-heartbeat) success!";


    // Update commit index and apply any newly committed entries to the state machine
    quint16 indexOfLastNewEntry = leaderPrevLogIndex + entries.length();
    myCommitIndex = qMin(leaderCommitIndex, indexOfLastNewEntry);

    while(myLastApplied < myCommitIndex){

        QString message = log.value(myLastApplied+1).toMap().value("message").toString();
        QString messageID = log.value(myLastApplied+1).toMap().value("messageID").toString();
        stateMachine.append(message);
        stateMachineMessageIDs.append(messageID);
        refreshTextView();
        myLastApplied++;

        removeMessageIDFromQueuedClientRequests(messageID);
    }

    replyToAppendEntries(true, leaderPrevLogIndex, entries.length(), sourcePort);
}

/* Helper fx: Called by follower to reply to an AppendEntries message from leader */
void ChatDialog::replyToAppendEntries(bool success, quint16 prevIndex, quint16 entryLen, quint16 destPort){

    QVariantMap outMap;
    outMap.insert("type", QVariant("replyToAppendEntries"));
    outMap.insert("term", QVariant(myCurrentTerm));
    outMap.insert("prevIndex", QVariant(prevIndex));
    outMap.insert("entryLen", QVariant(entryLen));

    if(success) {
        outMap.insert("success", QVariant(true));
    }
    else {
        outMap.insert("success", QVariant(false));
    }

    serializeMessage(outMap, destPort);
}

/* Called by leader to process a follower's reply to their AppendEntries message */
void ChatDialog::processAppendEntriesMsgReply(QVariantMap inMap, quint16 sourcePort){

    //qDebug() << "processAppendEntriesMsgReply";

    // Load parameters from message
    quint16 replyTerm = inMap.value("term").toInt();
    bool success = inMap.value("success").toBool();
    quint16 prevIndex = inMap.value("prevIndex").toInt();
    quint16 entryLen = inMap.value("entryLen").toInt();

    // Rules for All Servers 5.1: If RPC request or response contains term T > currentTerm,
    // set currentTerm = T and convert to follower
    if(replyTerm > myCurrentTerm){
        myCurrentTerm = replyTerm;
        myRole = FOLLOWER;
        sendHeartbeatTimer->stop();
        return; // return????
    }

    if(success){
        // https://groups.google.com/forum/#!topic/raft-dev/vCJcFi769x4
        quint16 prevMatchIndex = matchIndex[sourcePort];

        if(prevMatchIndex < prevIndex + entryLen){
            matchIndex[sourcePort] = prevIndex + entryLen;
            nextIndex[sourcePort] = matchIndex[sourcePort] + 1;
        }
    }
    else{ // follower replied with false
        // nextIndex should never decrease past matchIndex
        if(nextIndex[sourcePort] > matchIndex[sourcePort])
            nextIndex[sourcePort]--;

        qDebug() << "nextIndex: " + QString::number(nextIndex[sourcePort]);
        sendAppendEntriesMsg(sourcePort, false);
    }

    // go through matchIndex
    QList<quint16> matchIndexes = matchIndex.values();
    matchIndexes.append(myLastLogIndex);
    qSort(matchIndexes);

    // By definition of list being sorted, first entry must be valid "majority"
    quint16 majorityMax = matchIndexes.value(0);

    for(int i=0; i< matchIndexes.length(); i++){

        quint16 value = matchIndexes.value(i);
        quint16 countOfValuesLargerOrEqualTo = 0;

        for(int j=0; j< matchIndexes.length(); j++){
            if(value <= matchIndexes.value(j))
                countOfValuesLargerOrEqualTo++;
        }
        if(countOfValuesLargerOrEqualTo >= 2)     // FOR DEBUGGING, CHANGING FROM 3 to 2
            majorityMax= value;
    }

    if(entryLen >0)
        qDebug() << matchIndexes << " My N is: " + QString::number(majorityMax);

    if(majorityMax > myCommitIndex){
        if(log.value(majorityMax).toMap().value("term").toInt() == myCurrentTerm){
            myCommitIndex = majorityMax;
        }
    }

    while(myLastApplied < myCommitIndex){
        QString message = log.value(myLastApplied+1).toMap().value("message").toString();
        QString messageID = log.value(myLastApplied+1).toMap().value("messageID").toString();
        stateMachine.append(message);
        stateMachineMessageIDs.append(messageID);
        refreshTextView();
        myLastApplied++;

        removeMessageIDFromQueuedClientRequests(messageID);
    }
}

/* Called by server to process client requests */
void ChatDialog::gotReturnPressed(){

    QString input = textline->text();
    // NEED TO PARSE FOR SUPPORT COMMANDS

    // is first character < and is last character >
    if(input.length() > 0 && input.startsWith("<") && input.endsWith(">")){
        processSupportCommand(input.mid(1, input.length()-2));
        textline->clear();
        return;
    }



    // Store "Client" request
    QVariantMap clientRequest;
    QString messageID = QString(myPort) + QString(mySeqNo);
    QString message = QString::number(myPort) + ": " + input;
    mySeqNo++;

    clientRequest.insert("type", "clientRequest");
    clientRequest.insert("messageID", messageID);
    clientRequest.insert("term", myCurrentTerm); //Is this included?
    clientRequest.insert("message", message);

    queuedClientRequests.append(clientRequest);

    textline->clear();

    if(myRole == LEADER){
        attemptToCommitNextClientRequest();
    }
    else{    // I AM A CLIENT
        attemptToForwardNextClientRequest(); // what timer do I use to retry this?
    }
}

void ChatDialog::processSupportCommand(QString command){

    qDebug() << "Hey, it's a support command: " << command;


}

/* Called by follower to forward client requests to leader */
void ChatDialog::attemptToForwardNextClientRequest(){

    if(!queuedClientRequests.isEmpty()) {

        qDebug() << "Forwarding client request to leader";

        QVariantMap firstClientRequest = queuedClientRequests.first().toMap();
        serializeMessage(firstClientRequest, myLeader);
    }
}

/* Called by leader to process client request from follower */
void ChatDialog::processClientRequestFromFollower(QVariantMap inMap){

    // Do not add client request if messageID already in state machine or already in queuedClientRequests
    QString messageID = inMap.value("messageID").toString();

    if(stateMachineMessageIDs.contains(messageID))
        return;

    for(int i=0; i<queuedClientRequests.length(); i++){
        if(queuedClientRequests.value(i).toMap().value("messageID").toString() == messageID)
            return;
    }

    qDebug()<< "I added the forwarded client request to my queuedClientRequests";
    queuedClientRequests.append(inMap);
}

/* Called by leader to attempt to commit the next client request */
void ChatDialog::attemptToCommitNextClientRequest(){

    if(!queuedClientRequests.isEmpty()){

        QVariantMap nextLogEntry = queuedClientRequests.first().toMap();
        log.append(nextLogEntry);
        myLastLogIndex++;
        myLastLogTerm = nextLogEntry.value("term").toInt();

        qDebug() << "myLastLogIndex is now " + QString::number(myLastLogIndex);
        qDebug() << "myLastLogTerm is now " + QString::number(myLastLogTerm);

        // Send messages AppendEntries RPC everyone
        for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
            if(i!= myPort)
                sendAppendEntriesMsg(i, false);
        }

        refreshTextView();
    }
}

/* Helper fx: called by any server to remove a recently executed client request from queue */
void ChatDialog::removeMessageIDFromQueuedClientRequests(QString messageID){

    for(int i=0; i<queuedClientRequests.length(); i++){
        if(queuedClientRequests.value(i).toMap().value("messageID").toString() == messageID)
            queuedClientRequests.removeAt(i);
    }
}

void ChatDialog::processPendingDatagrams(){

    // check if we're participating in raft

    while(mySocket->hasPendingDatagrams()){
        QByteArray datagram;
        datagram.resize(mySocket->pendingDatagramSize());
        QHostAddress source;
        quint16 sourcePort;

        if(mySocket->readDatagram(datagram.data(), datagram.size(), &source, &sourcePort) != -1){


            // check if I've dropped this source port


            QDataStream inStream(&datagram, QIODevice::ReadOnly);
            QVariantMap inMap;
            inStream >> inMap;

            if(inMap.contains("type")){
                if(inMap.value("type") == "requestVote"){
                    qDebug() << "Received a request vote!";
                    processRequestVote(inMap, sourcePort);
                }

                else if(inMap.value("type") == "replyToRequestVote"){
                    qDebug() << "Received a reply to my request for votes!";
                    processReplyRequestVote(inMap, sourcePort);
                }

                else if(inMap.value("type") == "appendEntries"){
                    //qDebug() << "Received an Append Entries message!";
                    processAppendEntriesMsg(inMap, sourcePort);
                }

                else if(inMap.value("type") == "replyToAppendEntries"){
                    //qDebug() << "Received a reply to my Append Entries message!";
                    processAppendEntriesMsgReply(inMap, sourcePort);
                }

                else if(inMap.value("type") == "clientRequest"){
                    qDebug() << "Received a clientRequest from a follower!";
                    processClientRequestFromFollower(inMap);
                }
            }
        }
    }
}

void ChatDialog::serializeMessage(QVariantMap &outMap, quint16 destPort){

    // check if we're participating in raft

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

void ChatDialog::refreshTextView(){

    textview->clear();
    for(int i=0; i< stateMachine.length(); i++){

        if(stateMachine.at(i).contains(QString::number(myPort))){
            textview->setTextColor(QColor("blue"));
            textview->append(stateMachine.at(i));
            textview->setTextColor(QColor("black"));
        }
        else
            textview->append(stateMachine.at(i));
    }
}

/* Pick a range of five UDP ports to try to allocate by default, computed based on my Unix user ID.*/
NetSocket::NetSocket(){
	// This makes it trivial for up to five P2Papp instances per user to find each other on the same host,
	// barring UDP port conflicts with other applications (which are quite possible). We use the range from
	// 32768 to 49152 for this purpose.
	myPortMin = 32768 + (getuid() % 4096)*4;
	myPortMax = myPortMin + 4; // Edited to support 5 max users!
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

