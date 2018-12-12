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
    qDebug() << "------------------------------------------";

    setWindowTitle("RAFT Chat: " + QString::number(myPort));

	textview = new QTextEdit(this);
	textview->setReadOnly(true);
	textline = new QLineEdit(this);
	QVBoxLayout *layout = new QVBoxLayout();
	layout->addWidget(textview);
	layout->addWidget(textline);
	setLayout(layout);

	// Register a callback on the textline for returnPressed signal
	connect(textline, SIGNAL(returnPressed()), this, SLOT(gotReturnPressed()));

    // Register a callback when another p2p app sends us a message over UDP
    connect(mySocket, SIGNAL(readyRead()), this, SLOT(processPendingDatagrams()));

    // Timer if server has not received a heartbeat, they start an election
    electionTimer = new QTimer(this);
    connect(electionTimer, SIGNAL(timeout()), this, SLOT(sendRequestForVotes()));
    qsrand(QTime::currentTime().msec());
    // Time to wait for heartbeat should be much larger than heartbeat time (currently 1500 msec)
    timeToWaitForHeartbeat = qrand() % 1500 + 1500;
    qDebug() << "My election timeout is " + QString::number(timeToWaitForHeartbeat);
    electionTimer->start(timeToWaitForHeartbeat);

    // Timer for leader to keep track of when to send heartbeats.
    // Started when follower becomes leader, timeout defined in header definitions
    sendHeartbeatTimer = new QTimer(this);
    connect(sendHeartbeatTimer, SIGNAL(timeout()), this, SLOT(sendHeartbeat()));

    // Timer if server has not received a heartbeat, they start an election
    forwardClientRequestTimer = new QTimer(this);
    connect(forwardClientRequestTimer, SIGNAL(timeout()), this, SLOT(attemptToForwardNextClientRequest()));
    forwardClientRequestTimer->start(500);
}

/* Called by follower whose waitForHeartbeatTimer timed out. Starts a new election */
void ChatDialog::sendRequestForVotes(){

    if(!participatingInRAFT)
        return;

    qDebug() << "Sending RequestVote message: attempting to become leader";
    electionTimer->start(timeToWaitForHeartbeat);

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
        votedFor = -1;
        sendHeartbeatTimer->stop();
    }

    if(candidateTerm < myCurrentTerm) {
        qDebug() << "Vote was denied b/c candidate term is less than mine!";
        replyToRequestForVote(false, sourcePort);
        return;
    }

    // Check if candidate's log is at least as up-to-date as receiver's log
    if(candidateLastLogTerm < myLastLogTerm ||
    (candidateLastLogTerm==myLastLogTerm && candidateLastLogIndex < myLastLogIndex)){
        qDebug() << "Vote was denied b/c candidate's log is not as up to date as mine!";
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
        qDebug() << "Vote was denied b/c I have already voted for someone else this term!";
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
        qDebug() << "Vote to " + QString::number(sourcePort) + " was granted!";
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
        myCurrentTerm = termFromReply;
        myRole = FOLLOWER;
        votedFor = -1;
        sendHeartbeatTimer->stop();
        return;
    }

    // If vote is not for the current election I am running, ignore reply
    if(termFromReply != myCurrentTerm)
        return;

    if(voteGranted){
        if(!nodesThatVotedForMe.contains(sourcePort))
            nodesThatVotedForMe.append(sourcePort);

        // Check if I have a majority of votes
        if(nodesThatVotedForMe.length() >= 3){

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

    if(participatingInRAFT)
        qDebug() << "Sending heartbeat as leader";

    for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
        if(i!= myPort)
            sendAppendEntriesMsg(i, true); //2nd arg: true if heartbeat
    }

    // Prevent leader from trying to depose themselves
    electionTimer->start(timeToWaitForHeartbeat);

    // Do not attempt to commit next client request if there are still outstanding entries
    // in the log that have not yet been committed by a majority of replicas
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
        myCurrentTerm = leaderTerm;
        myRole = FOLLOWER;
        votedFor = -1;
        sendHeartbeatTimer->stop();
        return;
    }

    // Only reset election timer if we receive RPC from CURRENT leader
    if(leaderTerm == myCurrentTerm) {
        myLeader = leaderID;
        electionTimer->start(timeToWaitForHeartbeat);
    }

    if(leaderTerm < myCurrentTerm){
        qDebug() << "Replied false to Append Entries b/c my current term > leader term";
        replyToAppendEntries(false, 0, 0, sourcePort);
        return;
    }

    // Reply false if log is shorter than prevLogIndex
    if(leaderPrevLogIndex >= log.length()){
        qDebug() << "Replied false to Append Entries b/c leaderPrevLogIndex longer than my log length";
        replyToAppendEntries(false, 0, 0, sourcePort);
        return;
    }

    // Reply false if log doesn't contain an entry at prevLogIndex whose term matches prevLogTerm
    quint16 myTermAtPrevLogIndex = log.at(leaderPrevLogIndex).toMap().value("term").toInt();

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

    if(inMap.contains("entries"))
        qDebug() << "Append entries (non-heartbeat) success!";
    else
        qDebug() << "Append entries (heartbeat) success!";

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

    // Load parameters from message
    quint16 replyTerm = inMap.value("term").toInt();
    bool success = inMap.value("success").toBool();
    quint16 prevIndex = inMap.value("prevIndex").toInt();
    quint16 entryLen = inMap.value("entryLen").toInt();

    QString successString = success ? "true" : "false";
    if(entryLen == 0)
        qDebug() << "Leader: Received " + successString + " reply to my (heartbeat) Append Entries Msg";
    else
        qDebug() << "Leader: Received " + successString + " reply to my (non-heartbeat) Append Entries Msg";

    // Rules for All Servers 5.1: If RPC request or response contains term T > currentTerm,
    // set currentTerm = T and convert to follower
    if(replyTerm > myCurrentTerm){
        myCurrentTerm = replyTerm;
        myRole = FOLLOWER;
        votedFor = -1;
        sendHeartbeatTimer->stop();
        return;
    }

    if(success){
        // https://groups.google.com/forum/#!topic/raft-dev/vCJcFi769x4

        // Match Index shouldn't ever decrease
        if(prevIndex + entryLen > matchIndex[sourcePort]){
            matchIndex[sourcePort] = prevIndex + entryLen;
        }

        if(nextIndex[sourcePort] == 1)  // Necessary because log index 0 is a dummy index
            nextIndex[sourcePort]++;
        else
            nextIndex[sourcePort] = matchIndex[sourcePort] + 1;
    }
    else{ // follower replied with false
        // nextIndex should never decrease past matchIndex
        if(nextIndex[sourcePort] > matchIndex[sourcePort])
            nextIndex[sourcePort]--;

        //qDebug() << "nextIndex: " + QString::number(nextIndex[sourcePort]);
        sendAppendEntriesMsg(sourcePort, false);
    }

    // See if we can update commit index based on content of matchIndex
    QList<quint16> matchIndexes = matchIndex.values();
    matchIndexes.append(myLastLogIndex);
    qSort(matchIndexes);

    // By definition of list being sorted, first entry must be a valid "min majority"
    quint16 majorityMax = matchIndexes.value(0);

    for(int i=0; i< matchIndexes.length(); i++){

        quint16 value = matchIndexes.value(i);
        quint16 countOfValuesLargerOrEqualTo = 0;

        for(int j=0; j< matchIndexes.length(); j++){
            if(value <= matchIndexes.value(j))
                countOfValuesLargerOrEqualTo++;
        }
        if(countOfValuesLargerOrEqualTo >= 3)
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

        qDebug("Applying to state machine");

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

    // Check for <support command>
    if(input.length() > 0 && input.startsWith("<") && input.endsWith(">")){
        processSupportCommand(input.mid(1, input.length()-2));
        textline->clear();
        return;
    }

    textline->clear();
    processClientRequest(input);
}

void ChatDialog::processClientRequest(QString input){

    QVariantMap clientRequest;
    QString messageID = QString(myPort) + QString(mySeqNo);
    QString message = QString::number(myPort) + ": " + input;
    mySeqNo++;

    // Do NOT include term here. Term should be inserted by leader when
    // they are attempting to commit it to their log
    clientRequest.insert("type", "clientRequest");
    clientRequest.insert("messageID", messageID);
    clientRequest.insert("message", message);
    queuedClientRequests.append(clientRequest);

    if(myRole == LEADER){
        attemptToCommitNextClientRequest();
    }
    else{
        attemptToForwardNextClientRequest();
    }
}

void ChatDialog::processSupportCommand(QString command){

    qDebug() << "------------------------------------------";
    qDebug() << "Support command: " << command;

    if(command.contains("START")){
        // Give myself some buffer to receive a heartbeat if there is a current leader
        electionTimer->start(timeToWaitForHeartbeat);
        participatingInRAFT = true;
    }
    else if(command.contains("STOP")){
        participatingInRAFT = false;
    }

    else if(command.contains("MSG")){

        QString input = command.mid(4, -1);
        qDebug() << "Support command had message: " + input;
        processClientRequest(input);
    }

    else if(command.contains("DROP")){

        QString port = command.mid(4, -1);
        qDebug() << "Dropping port: " + port;
        nodesIHaveDropped.append(port.toInt());

    }
    else if(command.contains("RESTORE")){

        QString port = command.mid(4, -1);
        qDebug() << "Restoring port: " + port;
        nodesIHaveDropped.removeOne(port.toInt());

    }
    else if(command.contains("GET_NODES")){

        // Get all node ids, show Raft state (if leader is elected, provide leader id),
        // and the state of the current node (follower, leader, candidate);

        QString listOfNodes;
        for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
            listOfNodes.append(QString::number(i) + "  ");
        }
        qDebug() << "All node ID's: " + listOfNodes;

        if(myLeader == -1)
            qDebug() << "I'm not aware of a current leader";
        else
            qDebug() << "There is a leader. Leader ID is: " << QString::number(myLeader);

        QString myRoleString;
        if(myRole == LEADER)
            myRoleString = "Leader";
        else if(myRole == CANDIDATE)
            myRoleString = "Candidate";
        else
            myRoleString = "Follower";

        qDebug() << "My state (" + QString::number(myPort) + ") is " + myRoleString;

    }
    else if(command.contains("GET_CHAT")){
        for(int i=0; i<stateMachine.length(); i++){
            qDebug() << stateMachine.value(i);
        }
    }
    else
        qDebug() << "Invalid support command ";

    qDebug() << "------------------------------------------";
}

/* Called by follower to forward client requests to leader */
void ChatDialog::attemptToForwardNextClientRequest(){

    if(!participatingInRAFT)
        return;

    if(!queuedClientRequests.isEmpty() && myRole != LEADER && myLeader != -1){

        qDebug() << "Forwarding client request to leader " + QString::number(myLeader);

        QVariantMap firstClientRequest = queuedClientRequests.first().toMap();
        serializeMessage(firstClientRequest, myLeader);
    }
}

/* Called by leader to process client request from follower */
void ChatDialog::processClientRequestFromFollower(QVariantMap inMap, quint16 sourcePort){

    // Do not add client request if messageID already in state machine or already in queuedClientRequests
    QString messageID = inMap.value("messageID").toString();

    if(stateMachineMessageIDs.contains(messageID))
        return;

    for(int i=0; i<queuedClientRequests.length(); i++){
        if(queuedClientRequests.value(i).toMap().value("messageID").toString() == messageID)
            return;
    }

    qDebug()<< "I added client request forwarded from " + QString::number(sourcePort) + " to my queuedClientRequests";
    queuedClientRequests.append(inMap);
}

/* Called by leader to attempt to commit the next client request */
void ChatDialog::attemptToCommitNextClientRequest(){

    if(!queuedClientRequests.isEmpty()){

        QVariantMap nextLogEntry = queuedClientRequests.first().toMap();
        nextLogEntry.insert("term", myCurrentTerm);
        log.append(nextLogEntry);
        myLastLogIndex++;
        myLastLogTerm = nextLogEntry.value("term").toInt();

        // Send messages AppendEntries RPC everyone
        qDebug() << "Sending non-heartbeat Append Entries as leader";
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

    while(mySocket->hasPendingDatagrams()){
        QByteArray datagram;
        datagram.resize(mySocket->pendingDatagramSize());
        QHostAddress source;
        quint16 sourcePort;

        if(mySocket->readDatagram(datagram.data(), datagram.size(), &source, &sourcePort) != -1){

            if(!participatingInRAFT)
                return;

            if(nodesIHaveDropped.contains(sourcePort))
                return;

            QDataStream inStream(&datagram, QIODevice::ReadOnly);
            QVariantMap inMap;
            inStream >> inMap;

            if(inMap.contains("type")){
                if(inMap.value("type") == "requestVote"){
                    qDebug() << "Received a request vote from " + QString::number(sourcePort);
                    processRequestVote(inMap, sourcePort);
                }

                else if(inMap.value("type") == "replyToRequestVote"){
                    qDebug() << "Received a reply to my request for votes " + QString::number(sourcePort);
                    processReplyRequestVote(inMap, sourcePort);
                }

                else if(inMap.value("type") == "appendEntries"){
                    processAppendEntriesMsg(inMap, sourcePort);
                }

                else if(inMap.value("type") == "replyToAppendEntries"){
                    processAppendEntriesMsgReply(inMap, sourcePort);
                }

                else if(inMap.value("type") == "clientRequest"){
                    processClientRequestFromFollower(inMap, sourcePort);
                }
            }
        }
    }
}

void ChatDialog::serializeMessage(QVariantMap &outMap, quint16 destPort){

    if(!participatingInRAFT)
        return;

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

