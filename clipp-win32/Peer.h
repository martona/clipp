#pragma once

class Peer {
public:
	Peer(const wchar_t* hostName, const unsigned char* hostID, const char* ip, unsigned short port);
	~Peer();
	const wchar_t* hostName() const;
	const unsigned char* hostID() const;
	std::wstring ipw() const;
	std::string ip() const;
	unsigned short port() const;
private:
	std::wstring hostName_;
	unsigned char hostID_[32];
	std::string ip_;
	unsigned short port_;
};
