#include "main.hh"

ChatDialog::ChatDialog(){

    mySocket = new NetSocket();
    if(!  mySocket->bind()) {
		exit(1);
	}
    else {
		myPort = mySocket->port;
	}

	mySeqNo = 0;

    // myOrigin
	//myOrigin = QString::number(rand() % 100 + 1) + QString::number(0) + QString::number(myPort);
	myOrigin = QString::number(myPort);
	qDebug() << "myOrigin: " << myOrigin;
	qDebug() << "-------------------";

    setWindowTitle("P2Papp " + myOrigin);

	textview = new QTextEdit(this);
	textview->setReadOnly(true);
	textline = new QLineEdit(this);
	QVBoxLayout *layout = new QVBoxLayout();
	layout->addWidget(textview);
	layout->addWidget(textline);
	setLayout(layout);

	// Register a callback on the textline's returnPressed signal
	connect(textline, SIGNAL(returnPressed()),
		this, SLOT(gotReturnPressed()));

    // Register a callback when another p2papp sends us a message over UDP
    connect(mySocket, SIGNAL(readyRead()),
    		this, SLOT(processPendingDatagrams()));

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(reinvokeRumorMongering()));
}

void ChatDialog::gotReturnPressed(){

	textview->append(myOrigin + ": " + textline->text());
	QString msg = textline->text();

	QMap<quint32, QString> chatLogEntry;
	chatLogEntry.insert(mySeqNo, msg);

	if(!chatLogs.contains(myOrigin)){
		chatLogs.insert(myOrigin, chatLogEntry);
		statusMap.insert(myOrigin, QVariant(mySeqNo +1));
	}
	else{
		chatLogs[myOrigin].insert(mySeqNo, msg);
		statusMap[myOrigin] = QVariant(mySeqNo + 1);
	}

	sendRumorMessage(myOrigin, mySeqNo);
	mySeqNo += 1;

	textline->clear();
}

void ChatDialog::sendRumorMessage(QString origin, quint32 seqNo){

	if(!chatLogs.contains(origin) || !chatLogs[origin].contains(seqNo))
		return;

	QVariantMap rumorMap;

	rumorMap.insert(QString("ChatText"), chatLogs[origin].value(seqNo));
	rumorMap.insert(QString("Origin"), origin);
	rumorMap.insert(QString("SeqNo"), seqNo);

	serializeMessage(rumorMap);
}

void ChatDialog::serializeMessage(QVariantMap &outMap){

	QByteArray outData;
	QDataStream outStream(&outData, QIODevice::WriteOnly);
	outStream << outMap;

	// Pick a random neighbor to send this to   TO DO !!!!
	for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
		if(i != myPort){
			//qDebug() << "Sent message to: " + QString::number(i);
			mySocket->writeDatagram(outData.data(), outData.size(), QHostAddress::LocalHost, i);
		}
	}
	activateTimeout();
}

void ChatDialog::activateTimeout(){
    timer->start(3000);
}

void ChatDialog::reinvokeRumorMongering(){
    sendRumorMessage(myOrigin, mySeqNo);
}

void ChatDialog::processPendingDatagrams(){

	while(mySocket->hasPendingDatagrams()){
		QByteArray datagram;
		datagram.resize(mySocket->pendingDatagramSize());

		if(mySocket->readDatagram(datagram.data(), datagram.size(), NULL, NULL) != -1)
			deserializeMessage(datagram);
		else
			return;
	}
}

void ChatDialog::deserializeMessage(QByteArray datagram) {

	QDataStream inStream(&datagram, QIODevice::ReadOnly);
	QVariantMap inMap;
	inStream >> inMap;

    if(inMap.contains("ChatText"))
    	receiveRumorMessage(inMap);
    else if (inMap.contains("Want"))
		receiveStatusMessage(inMap);

}

void ChatDialog::receiveRumorMessage(QVariantMap inMap){

	QString origin = inMap.value("Origin").value <QString> ();
	quint32 seqNo = inMap.value("SeqNo").value <quint32> ();
	QString msg = inMap.value("ChatText").value <QString> ();

	QMap<quint32, QString> chatLogEntry;
	chatLogEntry.insert(seqNo, msg);

	// For convenience, discard any message with OOO seq number
	// If chatLogs does not contain messages from this origin, we expect message with seqNo 0
	if(!chatLogs.contains(origin)){
		if(seqNo == 0){
			chatLogs.insert(origin, chatLogEntry);
			statusMap.insert(origin, QVariant(seqNo + 1));
			textview->append(origin + ": " + msg);

			//qDebug() << "Logged new entry with seq no: 0";
			//qDebug() << "Status map: expecting next seq num: " + QString::number(statusMap[origin].value <quint32> ());
		}
	}
	else{
		//If chatLogs *does* contain this origin, we expect message with seqNo = last seq num + 1
		quint32 lastSeqNum = chatLogs[origin].keys().last();

		if (seqNo == lastSeqNum + 1){
			chatLogs.insert(origin, chatLogEntry);
			statusMap[origin] = QVariant(seqNo + 1);
			textview->append(origin + ": " + msg);

			//qDebug() << "Logged new entry with seq no: " << QString::number(seqNo);
            //qDebug() << "Status map: expecting next seq num: " + QString::number(statusMap[origin].value <quint32> ());
		}
	}
	sendStatusMessage();
}



void ChatDialog::sendStatusMessage(){

	QVariantMap statusMessage;
	statusMessage.insert(QString("Want"), statusMap);
	serializeMessage(statusMessage);

}


void ChatDialog::receiveStatusMessage(QVariantMap inMap){

    qDebug() << "I got a status message!";

	QVariantMap recvStatusMap = inMap["Want"].value <QVariantMap> ();
	QList<QString> recvOriginList = recvStatusMap.keys();

	for(int i=0; i< recvOriginList.count(); i++){

	    qDebug() << "recvStatusMap origin " + recvOriginList[i] + " with expected seqNo: " +
	    QString::number(recvStatusMap[recvOriginList[i]].value <quint32> ());
	}
}




/* Pick a range of four UDP ports to try to allocate by default, computed based on my Unix user ID.*/
NetSocket::NetSocket(){
	// This makes it trivial for up to four P2Papp instances per user to find each other on the same host,
	// barring UDP port conflicts with other applications (which are quite possible). We use the range from
	// 32768 to 49151 for this purpose.
	myPortMin = 32768 + (getuid() % 4096)*4;
	myPortMax = myPortMin + 3;

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

