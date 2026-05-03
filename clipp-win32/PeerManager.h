#pragma once

class PeerManager {
public:
	PeerManager();
	~PeerManager();
	void AddPeer(const wchar_t* hostName, const unsigned char* hostID, const char* ip, unsigned short port);
	void RemovePeer(const unsigned char* hostID);
	void ClearPeers();
private:
	std::vector<std::unique_ptr<Peer>> peers_;
};
