


#include "P2PClient.h"

#pragma comment(lib, "ws2_32.lib")

const int BUFFERLENGTH = 1024 * 64;	 // 64 kb buffer for large files
const int MESSAGESIZE = 2048;		 // The rough size of each data packet, in bytes, excluding the header

#define AETHERARK_ADDR "18.18.239.22:5060"

char buffer[BUFFERLENGTH];
char sendbuffer[BUFFERLENGTH];

int blockchainLength = 0;
int peerBlockchainLength = 0;

#if defined(_MSC_VER)
SOCKADDR_IN otherAddr;
	#define WINDOWS true
#else
sockaddr_in otherAddr;
	#define UNIX true
#endif
int otherSize;
std::atomic_bool stop_thread_1 = false;
std::atomic_bool stop_thread_2 = false;
std::atomic_bool thread_running = false;

#if UNIX
int localSocket, newlocalSocket, portno;
socklen_t remoteAddrLen;
struct sockaddr_in serv_addr, remoteAddr;
#endif

#if defined(_WIN32)
	#define GETSOCKETERRORNO() (WSAGetLastError())
#else
	#define GETSOCKETERRORNO() (errno)
#endif

Peer::Peer(std::string& ipPort)
{
	ip = SplitString(ipPort, ":")[0];
	port = stoi(SplitString(ipPort, ":")[1]);
}

bool P2P::InPeerList(std::string& ipPort)
{
	if (p2pConnections.find(ipPort) == p2pConnections.end())
		return false;
	return true;
}

void P2P::AddToPeerList(std::string& ipPort)
{
	if (!InPeerList(ipPort) && ipPort != clientIPPort) {
		// Add peer to collection of connections
		p2pConnections.insert(std::make_pair(ipPort, new Peer(ipPort)));
	}
}

void P2P::RemoveFromPeerList(std::string& ipPort)
{
	p2pConnections.erase(ipPort);
}

std::vector<std::string> P2P::GeneratePeerList()
{
	std::vector<std::string> oList = std::vector<std::string>();
	for (const auto& [key, value] : p2pConnections) {
		oList.push_back(key);
	}
	return oList;
}

#if defined(_MSC_VER)
// Get the IP:Port combination from SOCKADDR_IN struct, and return it as a string
std::string P2P::NormalizedIPString(SOCKADDR_IN addr)
{
	char peerIP[16];
	ZeroMemory(peerIP, 16);
	inet_ntop(AF_INET, &addr.sin_addr, peerIP, 16);

	USHORT port = ntohs(addr.sin_port);

	int realLen = 0;
	for (int i = 0; i < 16; i++) {
		if (peerIP[i] == '\00') {
			break;
		}
		realLen++;
	}

	std::string res(peerIP, realLen);
	res += ":" + std::to_string(port);

	return res;
}
#else
std::string P2P::NormalizedIPString(sockaddr_in remoteAddr)
{
	std::string fromIPString = inet_ntoa(remoteAddr.sin_addr);
	fromIPString += ":";
	fromIPString += std::to_string(ntohs(remoteAddr.sin_port));
	return fromIPString;
}
#endif

bool P2P::isAwaiting()
{
	return reqDat != -1;
}

// Safely send some data as a string, and split large amounts of data into multiple segments to be sent sequentially.
int P2P::mySendTo(int socket, std::string& s, int len, int redundantFlags, sockaddr* to, int toLen)
{

	int total = 0;		  // how many bytes we've sent
	int bytesLeft = len;  // how many we have left to send
	int n = 0;

	const char* p = s.c_str();
	try {

		int segmentCount = 1;
		while (bytesLeft > 0) {
			//std::string segInfo = "seg " + + " of " + + ", " + + " bytes|";
			std::string segInfo = "AG_SEGMENT_HEADER :" + std::to_string(segmentCount) +
								  ": of :" + std::to_string((int)ceil((float)len / (float)MESSAGESIZE)) +
								  ": , :" + std::to_string((bytesLeft < MESSAGESIZE) ? bytesLeft : MESSAGESIZE) +
								  ": BYTES TOTAL\n";

			int segSize = segInfo.size();

			segInfo += (p + total);
			//segInfo = segInfo.substr(0, (bytesLeft < MESSAGESIZE) ? (bytesLeft + segSize) : (MESSAGESIZE + segSize));

			n = sendto(socket,
					segInfo.c_str(),
					//sizeof(segInfo.c_str()),
					(bytesLeft < MESSAGESIZE) ? (bytesLeft + segSize) : (MESSAGESIZE + segSize),
					0,
					to,
					toLen) -
				segSize;  // Don't include segment info when counting data, so subtract this
			if (n + segSize <= -1) {
				ERRORMSG("Sending data failed:\n");
				printf("sendto failed with error: %d: %s\n", GETSOCKETERRORNO(), strerror(GETSOCKETERRORNO()));
				return -1;
				break;
			}

			total += n;
			if (WalletSettingValues::verbose >= 7) {
				std::cout << segInfo.c_str() << std::endl;
				std::cout << std::to_string(n + segSize) << " bytes sent" << std::endl;
			}
			if (bytesLeft < MESSAGESIZE)
				bytesLeft -= n + segSize;
			else
				bytesLeft -= MESSAGESIZE;

			segmentCount++;
		}
		if (WalletSettingValues::verbose >= 7) {
			std::cout << "Done sending chunk" << std::endl;
		}

		len = total;			  // return number actually sent here
		return n == -1 ? -1 : 0;  // return -1 on failure, 0 on success
	}
	catch (const std::exception& e) {
		console::ErrorPrint();
		console::Write("Sending thread encountered an error in (__FILE__, line: __LINE__):\n");
		std::cerr << e.what() << std::endl;
	}
	delete p;

	//#endif
	return 0;
}


// The function that is run in a thread in order to listen for received data in the background
void P2P::ListenerThread(int update_interval)
{
#if defined(_MSC_VER)
	try {
		thread_running = true;
		while (true) {

			SOCKADDR_IN remoteAddr;
			//remoteAddr.sin_family = AF_INET;
			//remoteAddr.sin_addr.s_addr = INADDR_ANY;
			socklen_t remoteAddrLen = sizeof(remoteAddr);

			DWORD timeout = 10 * 1000;
			setsockopt(localSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

			if (stop_thread_1) {
				thread_running = false;
				return;
			}

			bool pendingReceiveData = false;
			int lastReceivedSegment = 0;
			std::string totalMessage = "";
			std::string totalPendingMessage = "";

			while (!stop_thread_1) {
				if (stop_thread_1) {
					thread_running = false;
					break;
				}
				if (false) {
					console::NetworkErrorPrint();
					console::WriteLine("Error: Socket Error, trying again...");
				}
				else {
					int iResult = recvfrom(localSocket, buffer, BUFFERLENGTH, 0, (struct sockaddr*)&remoteAddr, &remoteAddrLen);
				}
			}
		}
	}
	catch (const std::exception& e) {
		ERRORMSG("");
		std::cerr << e.what() << std::endl;
	}
#else

	int n;
	try {
		thread_running = true;
		while (true) {

			if (stop_thread_1) {
				thread_running = false;
				return;
			}

			bool pendingReceiveData = false;
			int lastReceivedSegment = 0;
			std::string totalMessage = "";
			std::map<int, std::string> messageParts = std::map<int, std::string>();
			int maxSegments = 0;
			uint32_t noreceipt = 0;	 // A counter that increments each time tick there is no message

			while (!stop_thread_1) {

				// Set timer of 10 seconds for listening socket
				struct timeval tv;
				tv.tv_sec = 0;
				tv.tv_usec = 10000;

				remoteAddrLen = sizeof(remoteAddr);

				if (setsockopt(localSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
					ERRORMSG("Failed to set socket options");
				int iResult = recvfrom(localSocket, buffer, BUFFERLENGTH, 0, (sockaddr*)&remoteAddr, &remoteAddrLen);

				if (WalletSettingValues::verbose >= 9)
					std::cout << "iResult: " << std::to_string(iResult) << std::endl;

				if (iResult > 0) {

					noreceipt = 0;

					// Get the IPV4 address:port pair of the received data. If it
					// matches the expected one, continue. If it does not, then
					// stop. If the current one is blank or has disconnected,
					// set this one as the current connection and continue.
					std::string fromIPString = inet_ntoa(remoteAddr.sin_addr);
					fromIPString += ":";
					fromIPString += std::to_string(ntohs(remoteAddr.sin_port));

					// See if peer is somewhere in the peerlist
					AddToPeerList(fromIPString);
					// Reset life of this peer to 0
					p2pConnections[fromIPString]->life = 0;

					// If not currently connected, accept this connection.
					if (otherAddrStr == "" || otherAddrStr == "0.0.0.0:0") {
						if (WalletSettingValues::verbose >= 4)
							console::WriteLine("\nConversation Started", console::greenFGColor, "");

						otherAddrStr = fromIPString;
						SetPeer(otherAddrStr);
					}

					// If connected but different, ignore.
					else if (SplitString(fromIPString, ":")[0] != SplitString(otherAddrStr, ":")[0] /* && messageStatus != initial_connect_request*/) {
						// Send blank confirming message
						std::string tmpMsg = "ACK";
						mySendTo(localSocket, tmpMsg, tmpMsg.length(), 0, (sockaddr*)&remoteAddr, remoteAddrLen);
						continue;
					}

					// If at this point the other address is still not known, do not continue
					if (otherAddrStr == "" || otherAddrStr == "0.0.0.0:0")
						continue;

					// Read the received data buffer into a string
					std::string textVal = std::string(buffer, buffer + iResult);

					if (WalletSettingValues::verbose >= 4) {
						console::Write("received: " + NormalizedIPString(remoteAddr) + " -> ");
						console::WriteLine("\"" + textVal + "\"", console::yellowFGColor);
					}

					// Get the segment information from the received data
					std::string segInfo;
					int segNumber;
					maxSegments = 0;
					int segLength;
					try {
						//if(!StringContains(textVal, '&'))
						//	throw 1;
						if (StringStartsWith(textVal, "AG_SEGMENT_HEADER") == false)
							throw std::runtime_error("Header did not start with AG_SEGMENT_HEADER");
						segInfo = SplitString(textVal, "\n")[0];
						std::string s = SplitString(textVal, "\n")[1];	// This value is not used, but tested to ensure the segInfo header is there
						segNumber = std::stoi(SplitString(segInfo, ":")[1]);
						maxSegments = std::stoi(SplitString(segInfo, ":")[3]);
						segLength = std::stoi(SplitString(segInfo, ":")[5]) + segInfo.size() + 1;
					}
					catch (const std::exception& e) {
						//catch(...){ // If invalid segInfo header, ignore
						if (WalletSettingValues::verbose >= 4) {
							console::WriteLine("Received invalid AG_SEGMENT_HEADER ", console::redFGColor, "");
						}
						if (WalletSettingValues::verbose >= 5) {
							ERRORMSG(e.what());
						}
						// If an invalid header is sent, it is safest to assume that this is not a peer associated with the Aether network, so let's remove it from our list.
						RemoveFromPeerList(fromIPString);
						continue;
					}
					//char* tempContent = buffer;
					std::string content = "";
					for (int i = segInfo.size() + 1; i < segLength; i++) {
						content += buffer[i];
					}
					//csubstr(buffer, tempContent, segInfo.size()+2, BUFFERLENGTH, nullptr);
					//std::string content = textVal.substr(segInfo.size()+2); // Get all data after the end of the segment info

					if (WalletSettingValues::verbose >= 8) {
						console::WriteLine("received -- " + segInfo, console::yellowFGColor, "");
						console::WriteLine("\n\n" + content + "\n\n", console::greenFGColor, "");
					}

					// If we are currently still waiting for more data to be received
					if (pendingReceiveData) {
						if (messageParts.find(segNumber) == messageParts.end()) {
							messageParts[segNumber] = content;
							if (messageParts.size() == maxSegments) {
								totalMessage = "";
								for (int part = 1; part <= maxSegments; part++)
									totalMessage += messageParts[part];
								pendingReceiveData = false;
							}
							else
								continue;
						}
						else
							continue;
					}
					// If the current segment is 1, that means
					// this is the first and this needs to wait for more data
					// to arrive.
					else if (segNumber == 1 && segNumber != maxSegments) {
						messageParts.clear();  // Clear total message string and overwrite with current new data
						pendingReceiveData = true;
						totalMessage = "";
						continue;
					}
					// Else, this is a single segment message, and so the
					// totalMessage` variable can be set to the content
					else {
						messageParts.clear();  // Clear total message string and overwrite with current new data
						totalMessage = content;
						pendingReceiveData = false;
						lastReceivedSegment = 0;
					}

					std::string messagePrefix = "";

					// If the peer is requesting to connect
					if (StringStartsWith(totalMessage, "peer~connect")) {
						role = 1;
						if (WalletSettingValues::verbose >= 4) {
							console::DebugPrint();
							console::WriteLine("Received initial connection from (" + otherAddrStr + ")", console::greenFGColor, "");
						}
						if (otherAddrStr == "") {
							if (WalletSettingValues::verbose >= 4) {
								console::DebugPrint();
								console::WriteLine("Cancelled connection from invalid address", console::greenFGColor, "");
							}
							continue;
						}

						messagePrefix += "peer~connect~";
						json announcedInfo = json::parse(totalMessage.substr(messagePrefix.size()));
						// Add peer to collection of connections if not there yet
						AddToPeerList(otherAddrStr);

						p2pConnections[otherAddrStr]->height = announcedInfo["height"];
						p2pConnections[otherAddrStr]->peerList = announcedInfo["peerList"];
						p2pConnections[otherAddrStr]->testedOnline = true;

						if (WalletSettingValues::verbose >= 7) {
							console::WriteLine("answer peerconnect", console::greenFGColor, "");
						}

						messageStatus = announce;
						//messageStatus = await_first_success; // Awaiting confirmation status
						messageAttempt = 0;
						differentPeerAttempts = 0;

						CONNECTED_TO_PEER = true;
					}
					// If the peer is ending the connection
					else if (totalMessage == "peer~disconnect") {
						console::NetworkPrint();
						console::WriteLine("Peer closed.");
						CONNECTED_TO_PEER = false;
						reqDat = -1;
						messageStatus = -1;
						pendingReceiveData = false;
						otherAddrStr = "";
						return;
					}
					// If the peer is requesting message received confirmation
					else if (totalMessage == "peer~success" && (messageStatus >= 0)) {
						if (WalletSettingValues::verbose >= 7) {
							console::DebugPrint();
							console::WriteLine("Dual Confirmation", console::greenFGColor, "");
						}
						//messageStatus = await_second_success; // Confirmed message status, continue sending our own
						p2pConnections[otherAddrStr]->testedOnline = true;
						messageStatus = idle;  // Confirmed message status, continue sending our own
						reqDat = -1;
						messageStatus = -1;
						pendingReceiveData = false;
						otherAddrStr = "";
						// confirm 2 times, then switch to idle state -1
						CONNECTED_TO_PEER = false;
					}
					// If the peer is idling
					else if (totalMessage == "peer~idle") {
						if (WalletSettingValues::verbose >= 5) {
							console::DebugPrint();
							console::WriteLine("idle...", console::yellowFGColor, "");
							p2pConnections[otherAddrStr]->testedOnline = true;
						}
					}
					// If peer is requesting data
					else if (SplitString(totalMessage, "~")[0] == "request") {
						p2pConnections[otherAddrStr]->testedOnline = true;
						messagePrefix += "request~";
						// If peer is asking for blockchain height
						if (SplitString(totalMessage, "~")[1] == "height")
							messageStatus = replying_height;
						// If peer is asking for a pending block's data
						else if (SplitString(totalMessage, "~")[1] == "pendingblock") {
							messageStatus = replying_pendingblock;
							reqDat = std::stoi(SplitString(totalMessage, "~")[2]);
						}
						// If peer is asking for a block's data
						else if (SplitString(totalMessage, "~")[1] == "block") {
							messageStatus = replying_block;
							reqDat = std::stoi(SplitString(totalMessage, "~")[2]);
						}
						// If peer is asking for this peer's peerList
						else if (SplitString(totalMessage, "~")[1] == "peerlist")
							messageStatus = replying_peer_list;
						// If peer is asking for you to process and record a transaction
						else if (SplitString(totalMessage, "~")[1] == "transactionprocess") {
							messageStatus = await_first_success;
							std::string transactionString = SplitString(totalMessage, "~")[2];


							// Verify the transaction:
							//	* First ensure it has a valid signature
							//  * Then see if the user has enough Aether to send

							json transaction = json::parse(transactionString);
							std::string signature = transaction["sec"]["signature"];

							// Check signature length
							if (signature.size() == 0)
								continue;

							// Check if transaction is valid
							if (VerifyTransaction(transaction, 0, true)) {
								// Save transaction data to file
								try {
									json pendingTransactions = json();
									pendingTransactions["transactions"] = json::array();

									// Read existing pending transactions file, if it exists
									std::ifstream transactionsFileRead("./wwwdata/pendingtransactions.agtxs");
									if (transactionsFileRead.is_open()) {
										std::stringstream bufferd;
										bufferd << transactionsFileRead.rdbuf();
										std::string blockText = bufferd.str();
										transactionsFileRead.close();
									}

									// Append the new transaction
									pendingTransactions["transactions"].push_back(transaction);

									// Save the new transaction list
									std::ofstream transactionsFileWrite("./wwwdata/pendingtransactions.agtxs");
									if (transactionsFileWrite.is_open()) {
										transactionsFileWrite << pendingTransactions.dump();
										transactionsFileWrite.close();
										if (WalletSettingValues::verbose >= 4)
											console::WriteLine("\nSaved new transaction");
									}
								}
								catch (const std::exception& e) {
									ERRORMSG("failed to save transaction data to file");
									std::cerr << e.what() << std::endl;
								}

								if (WalletSettingValues::verbose >= 4) {
									console::WriteLine("received transaction: " + (std::string)transaction["tx"]["fromAddr"], console::greenFGColor, "");
								}
							}
						}

						if (WalletSettingValues::verbose >= 7) {
							console::WriteLine("request " + std::to_string(messageStatus), console::greenFGColor, "");
						}
					}
					// If peer is answering request
					else if (SplitString(totalMessage, "~")[0] == "answer") {
						p2pConnections[otherAddrStr]->testedOnline = true;
						messagePrefix += "answer~";
						// If peer is giving blockchain height
						if (SplitString(totalMessage, "~")[1] == "height") {
							peerBlockchainLength = std::stoi(SplitString(totalMessage, "~")[2]);
							messageStatus = await_first_success;
							if (WalletSettingValues::verbose >= 4) {
								console::WriteLine("answer height: " + std::to_string(peerBlockchainLength), console::greenFGColor, "");
							}
						}
						// If peer is responding to an announce
						else if (SplitString(totalMessage, "~")[1] == "announce") {
							messagePrefix += "announce~";
							json announcedInfo = json::parse(totalMessage.substr(messagePrefix.size()));
							// Add peer to collection of connections if not there yet
							AddToPeerList(otherAddrStr);

							p2pConnections[otherAddrStr]->height = announcedInfo["height"];
							p2pConnections[otherAddrStr]->peerList = announcedInfo["peerList"];
							p2pConnections[otherAddrStr]->testedOnline = true;

							messageStatus = await_first_success;
							if (WalletSettingValues::verbose >= 7) {
								console::WriteLine("answer peerannounce", console::greenFGColor, "");
							}
						}
						// If peer is giving peer list
						else if (SplitString(totalMessage, "~")[1] == "peerlist") {
							//std::vector<std::string> receivedPeers = SplitString(SplitString(totalMessage, "~")[2], ":");
							//// Iterate all received peers, and only add them to our list if it is not already on it
							//for (int x = 0; x < receivedPeers.size(); x++) {
							//	bool wasFound = false;
							//	for (int y = 0; y < peerList.size(); y++) {
							//		if (receivedPeers[x] == peerList[y]) {
							//			wasFound = true;
							//			break;
							//		}
							//	}
							//	if (wasFound == false)
							//		peerList.push_back(SplitString(receivedPeers[x], ":")[0] + ":" + SplitString(receivedPeers[x], ":")[1] + ":0");
							//}
							//SavePeerList();
							messageStatus = await_first_success;
						}
						// If peer is giving a block's data
						else if (SplitString(totalMessage, "~")[1] == "block") {
							messageStatus = await_first_success;
							int num = std::stoi(SplitString(totalMessage, "~")[2]);
							std::string blockData = SplitString(totalMessage, "~")[3];

							// Make sure this data is actually being requested; we don't want a forced download.
							if (reqDat != num) {
								if (WalletSettingValues::verbose >= 5)
									console::WriteLine("\nAnswered block does not match request: " + std::to_string(reqDat) + " != " + std::to_string(num));
								continue;
							}

							// Save block data to file
							try {
								if (WalletSettingValues::verbose >= 5)
									console::WriteLine("\nSaved block: " + std::to_string(num));
								std::ofstream blockFile("./wwwdata/blockchain/block" + std::to_string(num) + ".agblock");
								if (blockFile.is_open()) {
									blockFile << blockData;
									blockFile.close();
								}
							}
							catch (const std::exception& e) {
								ERRORMSG("failed to save block data to file");
								std::cerr << e.what() << std::endl;
							}

							if (WalletSettingValues::verbose >= 7) {
								console::WriteLine("received block: " + std::to_string(num), console::greenFGColor, "");
							}
						}
						messageAttempt = 0;
					}
				}
				else if (noreceipt >= 100) {
					if (WalletSettingValues::verbose >= 4 && otherAddrStr != "")
						console::WriteLine("noreceipt timeout check");
					messageStatus = idle;
					messageAttempt = 0;
					otherAddrStr = "";
					noreceipt = 0;
					pendingReceiveData = false;
				}
				else {
					noreceipt++;
				}
	#if WINDOWS
				else if (WSAGetLastError() != WSAETIMEDOUT && WalletSettingValues::verbose >= 5)
				{
					console::NetworkErrorPrint();
					console::WriteLine("Error, Peer closed.");
					CONNECTED_TO_PEER = false;
					reqDat = -1;
					//thread_running = false;
					return;
				}
	#endif
			}
		}
	}
	catch (const std::exception& e) {
		ERRORMSG("");
		std::cerr << e.what() << std::endl;
	}


#endif
}

void P2P::InitPeerList()
{
	std::string line;
	std::ifstream peerFile("./wwwdata/peerlist.list");

	// If the peer list file does not exist, create it
	if (!peerFile) {
		console::ErrorPrint();
		console::WriteLine("Error opening peer file", console::redFGColor, "");

		// Create the peer list file
		std::ofstream peerFileW("./wwwdata/peerlist.list");
		if (peerFileW.is_open()) {
			peerFileW << AETHERARK_ADDR;
			peerFileW.close();
		}
		peerFileW.close();
	}

	// Load peer file
	// Create a new Peer entry in p2pConnections for each address
	while (std::getline(peerFile, line)) {
		if (line[0] != '#' && line != "") {
			AddToPeerList(line);
		}
	}
	// If AETHERARK is not in peerlist, add it.
	std::string arkIP = AETHERARK_ADDR;
	AddToPeerList(arkIP);

	peerFile.close();
	SavePeerList();
}

void P2P::SavePeerList()
{
	std::ofstream peerFile("./wwwdata/peerlist.list");
	// If the peer list file does not exist, create it
	if (!peerFile) {
		console::ErrorPrint();
		console::WriteLine("Failed to save to peer file", console::redFGColor, "");
	}
	else {
		for (const auto& [key, value] : p2pConnections) {
			peerFile << key << "\n";
		}
	}
	peerFile.close();
}


// The function to open the socket required for the P2P connection
int P2P::OpenP2PSocket(int port)
{

	console::NetworkPrint();
	console::Write("Starting P2P Client...");

	p2pConnections = std::unordered_map<std::string, Peer*>();
#if WINDOWS

	Http http;

	// Start winsock
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		return 0;
	}

	stop_thread_1 = false;
	stop_thread_2 = false;

	localSocket = socket(AF_INET, SOCK_DGRAM, 0);

	SOCKADDR_IN clientAddr;
	clientAddr.sin_port = htons(port);
	clientAddr.sin_family = AF_INET;
	clientAddr.sin_addr.s_addr = INADDR_ANY;

	// Bind the socket
	if (bind(localSocket, (LPSOCKADDR)&clientAddr, sizeof(clientAddr)) == SOCKET_ERROR) {
		console::ErrorPrint();
		console::WriteLine("\n!!! Failed to bind socket !!!\n");
		printf("WSA error: #%d\n", WSAGetLastError());
		closesocket(localSocket);
		WSACleanup();
		FatalExit(1);
		return 0;
	}

	int val = 2048;
	setsockopt(localSocket, SOL_SOCKET, SO_SNDBUF, (char*)&val, sizeof(val));
	setsockopt(localSocket, SOL_SOCKET, SO_RCVBUF, (char*)&val, sizeof(val));

#else

	localSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (localSocket < 0) {
		console::ErrorPrint();
		console::WriteLine("ERROR opening socket");
		exit(1);
	}
	//bzero((char*)&serv_addr, sizeof(serv_addr));
	memset(&serv_addr, 0, sizeof(serv_addr));
	portno = port;
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(portno);
	if (bind(localSocket, (const struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
		console::ErrorPrint();
		console::WriteLine("ERROR on binding");
	}

#endif

	console::WriteLine(" ok", console::greenFGColor);

	return 0;
}

// Function to get random peer credentials from the peerList
void P2P::RandomizePeer()
{
	if (!isServer)
		messageStatus = initial_connect_request;
	if (p2pConnections.size() == 0) {
		peerListID = AETHERARK_ADDR;
		peerIP = AETHERARK_ADDR;
		peerPort = 5060;
		return;
	}
	try {
		uint16_t randI = rand() % p2pConnections.size();
		auto it = p2pConnections.begin();
		std::advance(it, randI);
		peerListID = (std::string)(it->second->ip);
		peerIP = (std::string)(it->second->ip);
		peerPort = it->second->port;
	}
	catch (const std::exception& e) {
		ERRORMSG(e.what());
	}
}

// Function to set specific peer credentials from the peerList
void P2P::SetPeer(std::string key)
{
	if (p2pConnections.size() == 0)
		return;
	try {
		if (InPeerList(key)) {
			peerListID = key;
			peerIP = p2pConnections[key]->ip;
			peerPort = p2pConnections[key]->port;
		}
	}
	catch (const std::exception& e) {
		ERRORMSG(e.what());
	}
}

void P2P::PrintP2PInfo()
{
	console::WriteLine("P2P Information:");
	console::WriteLine("peerListID: " + peerListID);
	console::WriteLine("peerIP: " + peerIP);
	console::WriteLine("peerPort: " + std::to_string(peerPort));
	console::WriteLine("messageStatus: " + std::to_string(messageStatus));
}

// The function that is run in a thread in order to reply or send data to a peer in the background
void P2P::SenderThread()
{
	//#if defined(_MSC_VER)

	//Http http;

	stop_thread_2 = false;

	RandomizePeer();
	otherAddrStr = NormalizedIPString(otherAddr);

	while (true) {
		try {

			otherAddr.sin_port = htons(peerPort);
			otherAddr.sin_family = AF_INET;
			otherAddr.sin_addr.s_addr = inet_addr(peerIP.c_str());

			//printf("%s\n", peerIP);

			//otherAddrStr = NormalizedIPString(otherAddr);
			//std::cout << "peer: \""<<peerIP.c_str() << "\"" << std::endl;

			otherSize = sizeof(otherAddr);

			//if (connect(localSocket, (struct sockaddr*)&otherAddr, otherSize) < 0)
			//	if(WalletSettingValues::verbose >= 3){
			//		console::WriteLine("ERROR connecting");
			//		printf("connect() failed with error: %d: %s\n", GETSOCKETERRORNO(), strerror(GETSOCKETERRORNO()));
			//	}

			bool noinput = false;


			// If not replying to messages or requesting something, continue
			if (messageStatus == idle)
				continue;

			int messageMaxAttempts = 10;

			// Begin sending messages, and stop when a reply is received, or max tries exceeded
			for (messageAttempt = 0; messageAttempt < messageMaxAttempts; messageAttempt++) {
				// Stop sending if the message status switches to idle
				if (messageStatus == idle) {
					if (WalletSettingValues::verbose >= 4)
						console::WriteLine("Conversation complete\n", console::greenFGColor, "");
					//otherAddrStr = "\0";
					reqDat = -1;
					role = -1;
					otherAddrStr = "";
					break;
				}

				// If not in idle state, continue sending messages
				std::string msg = "";

				if (WalletSettingValues::verbose >= 4) {
					console::WriteLine("sending: attempt (" + std::to_string(messageAttempt) + ") -> " + otherAddrStr);
				}

				// If doing initial connect request
				if (messageStatus == initial_connect_request) {
					//role = 0;
					//msg = "peer~connect";
					//if (WalletSettingValues::verbose >= 7) {
					//	console::Write(msg + "\n");
					//}
					//role = 1;
					namespace fs = std::filesystem;
					std::vector<std::string> delugeHashes = std::vector<std::string>();
					for (auto deluge : fs::directory_iterator("./wwwdata/deluges/")) {
						std::ifstream delugeFile(deluge.path());
						if (delugeFile.is_open()) {
							std::stringstream delugeFilebuf;
							delugeFilebuf << delugeFile.rdbuf();
							json delugeJson = json::parse(delugeFilebuf.str());
							delugeFile.close();
							delugeHashes.push_back((std::string)delugeJson["_totalHash"]);
						}
					}

					blockchainLength = FileCount("./wwwdata/blockchain/");

					json infoCompilation = json();
					infoCompilation = {
						{"height", blockchainLength},
						{"peerList", GeneratePeerList()},
						{"delugeHashes", delugeHashes},
					};
					msg = "peer~connect~" + infoCompilation.dump();
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
				}
				// If doing disconnect request
				else if (messageStatus == disconnect_request) {
					msg = "peer~disconnect";
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
				}
				// If doing peer confirmation
				else if ((messageStatus == initial_connect_request || messageStatus == await_first_success || messageStatus == await_second_success)) {
					msg = "peer~success";
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);

					// After multiple confirmations have been sent, switch back to idle mode
					if (messageAttempt >= 2) {
						messageStatus = idle;
						messageAttempt = 0;
						otherAddrStr = "";
						continue;
					}
				}
				// If replying to peer connect request with announce
				else if (messageStatus == announce) {
					role = 1;
					namespace fs = std::filesystem;
					std::vector<std::string> delugeHashes = std::vector<std::string>();
					for (auto deluge : fs::directory_iterator("./wwwdata/deluges/")) {
						std::ifstream delugeFile(deluge.path());
						if (delugeFile.is_open()) {
							std::stringstream delugeFilebuf;
							delugeFilebuf << delugeFile.rdbuf();
							json delugeJson = json::parse(delugeFilebuf.str());
							delugeFile.close();
							delugeHashes.push_back((std::string)delugeJson["_totalHash"]);
						}
					}

					blockchainLength = FileCount("./wwwdata/blockchain/");

					json infoCompilation = json();
					infoCompilation = {
						{"height", blockchainLength},
						{"peerList", GeneratePeerList()},
						{"delugeHashes", delugeHashes},
					};
					msg = "answer~announce~" + infoCompilation.dump();
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
				}
				// Else if replying to height request
				else if (messageStatus == replying_height) {
					role = 1;
					msg = "answer~height~" + std::to_string(blockchainLength);
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
				}
				// Else if replying to pending block data request
				else if (messageStatus == replying_pendingblock) {
					role = 1;
					// Open and read requested block
					std::ifstream td("./wwwdata/pendingblocks/block" + std::to_string(reqDat) + ".agblock");
					std::stringstream bufferd;
					bufferd << td.rdbuf();
					std::string blockText = bufferd.str();

					msg = "answer~pendingblock~" + std::to_string(reqDat) + "~" + ReplaceEscapeSymbols(blockText);
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
				}
				// Else if replying to block data request
				else if (messageStatus == replying_block) {
					role = 1;
					// Open and read requested block
					std::ifstream td("./wwwdata/blockchain/block" + std::to_string(reqDat) + ".agblock");
					std::stringstream bufferd;
					bufferd << td.rdbuf();
					std::string blockText = bufferd.str();

					msg = "answer~block~" + std::to_string(reqDat) + "~" + ReplaceEscapeSymbols(blockText);
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
				}
				// Else if replying to peer list request
				else if (messageStatus == replying_peer_list) {
					role = 1;
					std::string totalPeersString = "";
					std::vector<std::string> peerList = GeneratePeerList();
					for (int i = 0; i < peerList.size() && i < 10; i++)
						totalPeersString += peerList[i] + ((i == peerList.size() - 1 || i == 9) ? "" : ":");

					msg = "answer~peerlist~" + totalPeersString;
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
				}
				// Else if requesting chain height
				else if (messageStatus == requesting_height) {
					msg = "request~height";
					role = 0;
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
					//// Wait extra 3 seconds
				}
				// Else if requesting pending block data
				else if (messageStatus == requesting_pendingblock) {
					msg = "request~pendingblock~" + std::to_string(reqDat);
					role = 0;
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
					// Wait extra 3 seconds
					//noinput = true;
				}
				// Else if requesting block data
				else if (messageStatus == requesting_block) {
					msg = "request~block~" + std::to_string(reqDat);
					role = 0;
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
					// Wait extra 3 seconds
					//noinput = true;
				}
				// Else if requesting peer list
				else if (messageStatus == requesting_peer_list) {
					msg = "request~peerlist";
					role = 0;
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
					//// Wait extra 3 seconds
					//noinput = true;
				}
				// Else if requesting other client processes a transaction
				else if (messageStatus == requesting_transaction_process) {
					msg = "request~transactionprocess~" + ReplaceEscapeSymbols(extraData);
					role = 0;
					if (WalletSettingValues::verbose >= 7) {
						console::Write(msg + "\n");
					}
					mySendTo(localSocket, msg, msg.length(), 0, (sockaddr*)&otherAddr, otherSize);
					//// Wait extra 3 seconds
					//noinput = true;
				}

				// Wait 50 milliseconds before sending next message
#if WINDOWS
				Sleep(50);
#else
				//sleep(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
			}

			if (messageAttempt == messageMaxAttempts) {
				if (WalletSettingValues::verbose >= 4) {
					console::NetworkErrorPrint();
					console::Write("Peer Timed Out at ", console::redFGColor, "");
					console::WriteLine(std::to_string(peerPort), console::cyanFGColor, "");
				}
				otherAddrStr = "";
				messageAttempt = 0;

				// If this client is the asker, then try other peers.
				if (role == 0) {

					// Try at least 5 different peers to get answer to request
					if (differentPeerAttempts < 4) {
						if (WalletSettingValues::verbose >= 4) {
							console::NetworkPrint();
							console::WriteLine("Finding another peer...");
						}
						try {
							// Decrease life of current peer
							//char newLife = (SplitString(peerList[peerListID], ":")[2]).c_str()[0] + 1;

							//p2pConnections[peerListID]->life += 1;
							//if (newLife >= '9' && keepPeersAlive == false && SplitString(peerList[peerListID], ":")[0] + ":" + SplitString(peerList[peerListID], ":")[1] != AETHERARK_ADDR) // If life is 9, remove it from list
							//	peerList.erase(peerList.begin() + peerListID);
							//else if (keepPeersAlive)
							//	peerList[peerListID] = SplitString(peerList[peerListID], ":")[0] + ":" + SplitString(peerList[peerListID], ":")[1] + ":0";
							//else // Otherwise just increment by 1
							//	peerList[peerListID] = SplitString(peerList[peerListID], ":")[0] + ":" + SplitString(peerList[peerListID], ":")[1] + ":" + newLife;
						}
						catch (const std::exception& e) {
							std::cerr << "Error:\n";
							std::cerr << e.what() << std::endl;
							std::cerr << __FILE__ << ", line: " << __LINE__ << std::endl;
						}
						// Then select another
						RandomizePeer();
						//SavePeerList();
						differentPeerAttempts++;
					}
					// If at least 5 have been tried, stop.
					else {
						messageStatus = idle;
						console::NetworkErrorPrint();
						console::WriteLine("!! No peers seem to be online. Please try again later. !!", console::redFGColor, "");
						differentPeerAttempts = 0;
						otherAddrStr = "";
						reqDat = -1;
					}
				}
				// Otherwise, this is the answerer, so stop trying to reply to the original asker.
				else {
					role = -1;
					messageStatus = idle;
					otherAddrStr = "";
					if (WalletSettingValues::verbose >= 5) {
						console::NetworkErrorPrint();
						console::WriteLine("Asking peer went offline.", console::redFGColor, "");
					}
					differentPeerAttempts = 0;
					reqDat = -1;
				}
			}
		}
		catch (const std::exception& e) {
			ERRORMSG("");
			console::ErrorPrint();
			console::WriteLine(e.what());
		}
		// Stop the current listening thread and continue
		//stop_thread_1 = true;
		//t1.join();
		//t1.detach();
		//stop_thread_1 = false;

		//closesocket(localSocket);

		continue;
	}

	//closesocket(localSocket);

	//#endif
}

bool VerifyTransaction(json& tx, uint32_t id, bool thorough)
{

	std::string signature = decode64((std::string)tx["sec"]["signature"]);
	std::string fromAddress = (std::string)tx["tx"]["fromAddr"];
	float amountToSend = (float)tx["tx"]["amount"];

	// Hash transaction data
	char sha256OutBuffer[65];
	sha256_string((char*)(tx["tx"].dump()).c_str(), sha256OutBuffer);
	std::string transHash = sha256OutBuffer;

	// Verify signature by decrypting signature with public key
	std::string decryptedSig = rsa_pub_decrypt(signature, tx["sec"]["pubKey"]);

	// Make sure the signature is valid by seeing if the decrypted version
	// is the same as the hash of the transaction
	if (decryptedSig != transHash) {
		//console::Write("  Bad signature on T:" + std::to_string(id), cons.redFGColor, "");
		return false;
	}
	else if (!thorough)
		return true;


	// Now that it passed the signature test, go back through the
	// blockchain to ensure they have enough for the transaction

	int blockCount = FileCount("./wwwdata/blockchain/");


	// Iterate all blocks until the total value for that address is enough to pay
	int i;
	float funds = 0;
	for (i = blockCount - 10; i > 0; i--) {
		std::ifstream tt;
		tt.open("./wwwdata/blockchain/block" + std::to_string(i) + ".agblock");
		//if (!tt.is_open())
		//	ERRORMSG("Could not open file");
		std::stringstream buffert;
		buffert << tt.rdbuf();
		json o = json::parse(buffert.str());

		std::string rewardedAddress;  // The address that is awarded the gas fees and block reward

		// Check all transactions to see if they have a valid signature
		for (int tr = 0; tr < o["transactions"].size(); tr++) {
			std::string fromAddr = (std::string)o["transactions"][tr]["tx"]["fromAddr"];
			std::string toAddr = (std::string)o["transactions"][tr]["tx"]["toAddr"];
			float amount = o["transactions"][tr]["tx"]["amount"];
			std::string signature = decode64((std::string)o["transactions"][tr]["sec"]["signature"]);
			std::string publicKey = (std::string)o["transactions"][tr]["sec"]["pubKey"];

			// If this is the first transaction, that is the block reward, so it should be handled differently:
			if (tr == 0) {
				if (fromAddress == toAddr)	// If this is the receiving address, then give reward
					funds += amount;
				rewardedAddress = toAddr;
				continue;
			}

			// The from address should be the same as the hash of the public key, so check that first:
			char walletBuffer[65];
			sha256_string((char*)(publicKey).c_str(), walletBuffer);
			std::string expectedWallet = walletBuffer;
			if (fromAddr != expectedWallet) {
				o["transactions"].erase(o["transactions"].begin() + tr);
				continue;
			}

			// Hash transaction data
			sha256_string((char*)(o["transactions"][tr]["tx"].dump()).c_str(), sha256OutBuffer);
			transHash = sha256OutBuffer;

			// Verify signature by decrypting signature with public key
			decryptedSig = rsa_pub_decrypt(signature, publicKey);

			// The decrypted signature should be the same as the hash of this transaction we just generated
			if (decryptedSig != transHash) {
				o["transactions"].erase(o["transactions"].begin() + tr);
				//cons.Write("  Bad signature on T:" + std::to_string(tr), cons.redFGColor, "");
				continue;
			}

			// Now check if the sending or receiving address is the same as the user's
			if (fromAddress == fromAddr)  // If sending funds
				funds -= amount;
			else if (fromAddress == toAddr)	 // If receiving funds
				funds += amount;
			else if (rewardedAddress == fromAddress)  // If you are the one that mined this block, add gas fees
				funds += (float)o["transactions"][tr]["tx"]["transactionFee"];
		}

		// Check if the required amount of funds have been counted yet
		// If Yes, the transaction is now counted as valid.
		if (funds >= amountToSend)
			return true;
	}

	// If all blocks are exhausted without enough funds, transaction is invalid.
	return false;
}
