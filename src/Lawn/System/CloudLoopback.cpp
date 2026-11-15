/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

// The loopback listener the browser comes back to after an OAuth sign-in (RFC 8252 section 7.3): plain sockets on
// 127.0.0.1 and ::1, which nothing outside this computer can reach.

#ifdef PVZP_CLOUD_SAVE

#include "CloudSync.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <format>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace CloudSync
{

namespace
{

using Clock = std::chrono::steady_clock;

constexpr int TICK_MS = 100;					// how often theKeepWaiting is asked
constexpr size_t MAX_LINE = 64 * 1024;			// the request line, which carries the answer: a few KB from a browser
constexpr size_t MAX_HEADERS = 1024 * 1024;		// the rest is only looked through, cookies of other localhost pages and all
constexpr size_t MAX_CLIENTS = 16;
constexpr auto IDLE_LIMIT = std::chrono::seconds(10);	// browsers open spare connections that never send anything

#ifdef _WIN32
using Socket = SOCKET;
const Socket NO_SOCKET = INVALID_SOCKET;
using AddressLength = int;

int LastError() { return WSAGetLastError(); }
bool WouldBlock(int theError) { return theError == WSAEWOULDBLOCK; }
bool InUse(int theError) { return theError == WSAEADDRINUSE || theError == WSAEACCES; }
std::string ErrorText(int theError) { return std::format("Winsock error {}", theError); }
void CloseSocket(Socket theSocket) { closesocket(theSocket); }

void Prepare(Socket theSocket)
{
	u_long aNonBlocking = 1;
	ioctlsocket(theSocket, FIONBIO, &aNonBlocking);
	SetHandleInformation(reinterpret_cast<HANDLE>(theSocket), HANDLE_FLAG_INHERIT, 0);	// not into the browser it starts
}
#else
using Socket = int;
const Socket NO_SOCKET = -1;
using AddressLength = socklen_t;

int LastError() { return errno; }
bool WouldBlock(int theError) { return theError == EAGAIN || theError == EWOULDBLOCK || theError == EINTR; }
bool InUse(int theError) { return theError == EADDRINUSE; }
std::string ErrorText(int theError) { return std::strerror(theError); }
void CloseSocket(Socket theSocket) { close(theSocket); }

void Prepare(Socket theSocket)
{
	fcntl(theSocket, F_SETFL, fcntl(theSocket, F_GETFL, 0) | O_NONBLOCK);
	fcntl(theSocket, F_SETFD, FD_CLOEXEC);		// not into the browser it starts
#ifdef SO_NOSIGPIPE
	int anOn = 1;
	setsockopt(theSocket, SOL_SOCKET, SO_NOSIGPIPE, &anOn, sizeof(anOn));	// a closed tab must not kill the game
#endif
}
#endif

#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

// Listens on the loopback address of theFamily at thePort (0: any free one). theError is the socket error on failure.
Socket Listen(int theFamily, int thePort, int& theBoundPort, int& theError)
{
	Socket aSocket = socket(theFamily, SOCK_STREAM, IPPROTO_TCP);
	if (aSocket == NO_SOCKET)
	{
		theError = LastError();
		return NO_SOCKET;
	}

	int anOn = 1;
#ifdef _WIN32
	setsockopt(aSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&anOn), sizeof(anOn));
#endif
	if (theFamily == AF_INET6)
		setsockopt(aSocket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&anOn), sizeof(anOn));

	sockaddr_storage anAddress{};
	AddressLength aLength;
	if (theFamily == AF_INET)
	{
		sockaddr_in* anIPv4 = reinterpret_cast<sockaddr_in*>(&anAddress);
		anIPv4->sin_family = AF_INET;
		anIPv4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		anIPv4->sin_port = htons(static_cast<uint16_t>(thePort));
		aLength = sizeof(sockaddr_in);
	}
	else
	{
		sockaddr_in6* anIPv6 = reinterpret_cast<sockaddr_in6*>(&anAddress);
		anIPv6->sin6_family = AF_INET6;
		anIPv6->sin6_addr.s6_addr[15] = 1;		// ::1
		anIPv6->sin6_port = htons(static_cast<uint16_t>(thePort));
		aLength = sizeof(sockaddr_in6);
	}

	if (bind(aSocket, reinterpret_cast<sockaddr*>(&anAddress), aLength) != 0 || listen(aSocket, 8) != 0 ||
		getsockname(aSocket, reinterpret_cast<sockaddr*>(&anAddress), &aLength) != 0)
	{
		theError = LastError();
		CloseSocket(aSocket);
		return NO_SOCKET;
	}
	Prepare(aSocket);
	theBoundPort = ntohs(theFamily == AF_INET ? reinterpret_cast<sockaddr_in*>(&anAddress)->sin_port :
		reinterpret_cast<sockaddr_in6*>(&anAddress)->sin6_port);
	return aSocket;
}

// Ports a browser does not connect to, not even on the loopback interface: the "bad ports" of the Fetch standard,
// and 65535, which Safari turns down too. Every port below 1024 counts as one here: many of those are, and a free
// port is seldom there.
bool BrowsersRefuse(int thePort)
{
	static constexpr int REFUSED[] = { 1719, 1720, 1723, 2049, 3659, 4045, 4190, 5060, 5061, 6000, 6566, 6665, 6666, 6667,
		6668, 6669, 6679, 6697, 10080, 65535 };
	return thePort < 1024 || std::find(std::begin(REFUSED), std::end(REFUSED), thePort) != std::end(REFUSED);
}

// Which of theSockets can be read (or accepted from) within theMilliseconds. False on an error of the wait itself.
bool WaitReadable(const std::vector<Socket>& theSockets, int theMilliseconds, std::vector<bool>& theReady)
{
	theReady.assign(theSockets.size(), false);
#ifdef _WIN32
	// FD_SETSIZE counts sockets on Windows, and there are never more than MAX_CLIENTS + 2 here
	fd_set aSet;
	FD_ZERO(&aSet);
	for (Socket aSocket : theSockets)
		FD_SET(aSocket, &aSet);
	timeval aTimeout{ theMilliseconds / 1000, (theMilliseconds % 1000) * 1000 };
	if (select(0, &aSet, nullptr, nullptr, &aTimeout) == SOCKET_ERROR)
		return false;
	for (size_t i = 0; i < theSockets.size(); i++)
		theReady[i] = FD_ISSET(theSockets[i], &aSet) != 0;
#else
	std::vector<pollfd> aPolls;
	for (Socket aSocket : theSockets)
		aPolls.push_back(pollfd{ aSocket, POLLIN, 0 });
	if (poll(aPolls.data(), aPolls.size(), theMilliseconds) < 0)
		return errno == EINTR;
	for (size_t i = 0; i < aPolls.size(); i++)
		theReady[i] = (aPolls[i].revents & (POLLIN | POLLERR | POLLHUP)) != 0;
#endif
	return true;
}

void SendAll(Socket theSocket, const std::string& theData)
{
	size_t aSent = 0;
	for (int aStalls = 0; aSent < theData.size() && aStalls < 20;)
	{
		int aCount = static_cast<int>(send(theSocket, theData.data() + aSent, static_cast<int>(theData.size() - aSent), SEND_FLAGS));
		if (aCount > 0)
		{
			aSent += static_cast<size_t>(aCount);
			continue;
		}
		if (aCount < 0 && !WouldBlock(LastError()))
			return;
		aStalls++;
#ifdef _WIN32
		Sleep(50);
#else
		usleep(50 * 1000);
#endif
	}
}

std::string UrlDecode(std::string_view theText)
{
	auto aHex = [](char theChar) -> int
	{
		if (theChar >= '0' && theChar <= '9')
			return theChar - '0';
		theChar = static_cast<char>(std::tolower(static_cast<unsigned char>(theChar)));
		if (theChar >= 'a' && theChar <= 'f')
			return theChar - 'a' + 10;
		return -1;
	};

	std::string aText;
	for (size_t i = 0; i < theText.size(); i++)
	{
		if (theText[i] == '+')
			aText += ' ';
		else if (theText[i] == '%' && i + 2 < theText.size() && aHex(theText[i + 1]) >= 0 && aHex(theText[i + 2]) >= 0)
		{
			aText += static_cast<char>(aHex(theText[i + 1]) * 16 + aHex(theText[i + 2]));
			i += 2;
		}
		else
			aText += theText[i];
	}
	return aText;
}

Query ParseQuery(std::string_view theQuery)
{
	Query aQuery;
	while (!theQuery.empty())
	{
		size_t anEnd = theQuery.find('&');
		std::string_view aPair = theQuery.substr(0, anEnd);
		theQuery = anEnd == std::string_view::npos ? std::string_view() : theQuery.substr(anEnd + 1);
		if (aPair.empty())
			continue;
		size_t anEquals = aPair.find('=');
		std::string aKey = UrlDecode(aPair.substr(0, anEquals));
		if (!aQuery.contains(aKey))		// the first one counts, as a browser would send it
			aQuery[aKey] = anEquals == std::string_view::npos ? std::string() : UrlDecode(aPair.substr(anEquals + 1));
	}
	return aQuery;
}

// In a time that does not depend on where they differ, so a page guessing the state learns nothing from the answers.
bool SameText(std::string_view theA, std::string_view theB)
{
	if (theA.size() != theB.size())
		return false;
	unsigned char aDifference = 0;
	for (size_t i = 0; i < theA.size(); i++)
		aDifference |= static_cast<unsigned char>(theA[i] ^ theB[i]);
	return aDifference == 0;
}

std::string Page(int theStatus, const char* theReason, std::string_view theText)
{
	// the empty icon keeps the browser from asking for /favicon.ico
	std::string aBody = std::format(
		"<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>PvZ-Portable</title><link rel=\"icon\" href=\"data:,\"></head>"
		"<body style=\"font-family: sans-serif; margin: 3em; font-size: 1.2em\"><p>{}</p></body></html>\n", theText);
	return std::format(
		"HTTP/1.1 {} {}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nCache-Control: no-store\r\n"
		"Referrer-Policy: no-referrer\r\nConnection: close\r\n\r\n{}", theStatus, theReason, aBody.size(), aBody);
}

class LoopbackListener final : public RedirectListener
{
	struct Client
	{
		Socket				mSocket;
		Clock::time_point	mSince;
		size_t				mRead = 0;			// bytes so far
		std::string			mLine;				// the request line, and what follows it until the line is whole
		bool				mHasLine = false;
		std::string			mTail;				// the last bytes so far, for an end of the headers split between reads
	};

	enum class Progress
	{
		MORE,
		COMPLETE,
		LINE_TOO_LONG,
		TOO_LARGE,
		NOT_HTTP,
	};

	std::vector<Socket>	mListeners;
	std::vector<Client>	mClients;
	Socket				mAnswer = NO_SOCKET;	// the browser waiting for Reply
	int					mPort = 0;
#ifdef _WIN32
	bool				mStarted = false;
#endif

	void Close()
	{
		for (Socket aSocket : mListeners)
			CloseSocket(aSocket);
		for (const Client& aClient : mClients)
			CloseSocket(aClient.mSocket);
		mListeners.clear();
		mClients.clear();
	}

	void Drop(size_t theIndex)
	{
		CloseSocket(mClients[theIndex].mSocket);
		mClients.erase(mClients.begin() + static_cast<ptrdiff_t>(theIndex));
	}

	// False when accept() failed for another reason than none waiting, such as no descriptors left: the connection
	// then stays queued, and the listener keeps looking readable.
	bool Accept(Socket theListener)
	{
		for (size_t i = 0; i < MAX_CLIENTS; i++)
		{
			Socket aSocket = accept(theListener, nullptr, nullptr);
			if (aSocket == NO_SOCKET)
				return WouldBlock(LastError());		// none waiting any more, if so
			Prepare(aSocket);
			if (mClients.size() >= MAX_CLIENTS)
				Drop(0);	// the oldest is the likeliest to be a spare connection
			mClients.push_back(Client{ aSocket, Clock::now() });
		}
		return true;
	}

	static void Answer(Socket theSocket, const std::string& theResponse)
	{
		SendAll(theSocket, theResponse);
#ifdef _WIN32
		shutdown(theSocket, SD_SEND);
#else
		shutdown(theSocket, SHUT_WR);
#endif
		// Closing with bytes of the request unread resets the connection, which can lose the answer on its way
		// (RFC 9112 section 9.6), so what has arrived is read away. More of a request too large to take is not
		// waited for.
		char aBuffer[4096];
		for (int i = 0; i < 64 && recv(theSocket, aBuffer, sizeof(aBuffer), 0) > 0; i++)
		{
		}
	}

	// Takes in what arrived on theClient. Only the request line is kept, since it carries the answer: the headers
	// after it are just looked through for their end.
	static Progress Feed(Client& theClient, std::string_view theData)
	{
		if (theClient.mRead == 0 && theData.front() == '\x16')
			return Progress::NOT_HTTP;	// a TLS handshake: a browser trying https first falls back to http once it fails
		theClient.mRead += theData.size();
		if (theClient.mRead > MAX_HEADERS)
			return Progress::TOO_LARGE;		// however the reads fell

		std::string aRest;
		if (!theClient.mHasLine)
		{
			size_t aFrom = theClient.mLine.empty() ? 0 : theClient.mLine.size() - 1;	// a "\r" may end the last read
			theClient.mLine.append(theData);
			size_t anEnd = theClient.mLine.find("\r\n", aFrom);
			if (anEnd == std::string::npos)
				return theClient.mLine.size() > MAX_LINE ? Progress::LINE_TOO_LONG : Progress::MORE;
			if (anEnd > MAX_LINE)
				return Progress::LINE_TOO_LONG;
			aRest = theClient.mLine.substr(anEnd);	// from the "\r\n" of the line, which also ends a request without headers
			theClient.mLine.resize(anEnd);
			theClient.mHasLine = true;
		}
		else
			aRest = theClient.mTail + std::string(theData);

		if (aRest.find("\r\n\r\n") != std::string::npos)
			return Progress::COMPLETE;
		theClient.mTail = aRest.substr(aRest.size() - std::min<size_t>(aRest.size(), 3));
		return Progress::MORE;
	}

	// Answers the request of theLine, unless it is the one being waited for: then its query goes to theQuery.
	static bool Handle(Socket theSocket, std::string_view theLine, const std::string& theState, Query& theQuery)
	{
		size_t aSpace = theLine.find(' ');
		size_t aSecondSpace = aSpace == std::string_view::npos ? std::string_view::npos : theLine.find(' ', aSpace + 1);
		if (aSecondSpace == std::string_view::npos)
		{
			Answer(theSocket, Page(400, "Bad Request", "This is not a request PvZ-Portable understands."));
			return false;
		}
		std::string_view aMethod = theLine.substr(0, aSpace);
		std::string_view aTarget = theLine.substr(aSpace + 1, aSecondSpace - aSpace - 1);
		if (aMethod != "GET")
		{
			Answer(theSocket, Page(405, "Method Not Allowed", "This is not a request PvZ-Portable understands."));
			return false;
		}

		size_t aQueryAt = aTarget.find('?');
		if (aTarget.substr(0, aQueryAt) != "/")
		{
			Answer(theSocket, Page(404, "Not Found", "There is nothing here."));
			return false;
		}

		Query aQuery = aQueryAt == std::string_view::npos ? Query() : ParseQuery(aTarget.substr(aQueryAt + 1));
		bool anAnswer = aQuery.contains("code") || aQuery.contains("error");
		if (!anAnswer || !SameText(aQuery["state"], theState))
		{
			// a stale tab of an earlier sign-in, or another page guessing the port: not ours to act on
			Answer(theSocket, Page(400, "Bad Request",
				"This page does not belong to the sign-in PvZ-Portable is waiting for. Close it and sign in again from the game."));
			return false;
		}
		theQuery = std::move(aQuery);
		return true;
	}

public:
	~LoopbackListener() override
	{
		Reply(Outcome::FAILED);
		Close();
#ifdef _WIN32
		if (mStarted)
			WSACleanup();
#endif
	}

	bool Open(std::string& theError) override
	{
#ifdef _WIN32
		if (!mStarted)
		{
			WSADATA aData;
			int aFailure = WSAStartup(MAKEWORD(2, 2), &aData);
			if (aFailure != 0)
			{
				theError = std::format("could not start networking: {}", ErrorText(aFailure));
				return false;
			}
			mStarted = true;
		}
#endif
		Reply(Outcome::FAILED);
		Close();

		// 127.0.0.1 on a free port, and ::1 on the same port when this computer has IPv6, since a browser may look
		// "localhost" up as either
		for (int aTry = 0; aTry < 16; aTry++)
		{
			int aPort = 0;
			int anError = 0;
			Socket anIPv4 = Listen(AF_INET, 0, aPort, anError);
			if (anIPv4 == NO_SOCKET)
			{
				theError = std::format("could not listen on 127.0.0.1 for the browser: {}", ErrorText(anError));
				return false;
			}
			if (BrowsersRefuse(aPort))
			{
				CloseSocket(anIPv4);	// the browser would never come back to it
				continue;
			}

			int aPort6 = 0;
			Socket anIPv6 = Listen(AF_INET6, aPort, aPort6, anError);
			if (anIPv6 == NO_SOCKET && InUse(anError))
			{
				CloseSocket(anIPv4);	// taken on IPv6 only: try another port
				continue;
			}

			mListeners.push_back(anIPv4);
			if (anIPv6 != NO_SOCKET)
				mListeners.push_back(anIPv6);
			mPort = aPort;
			return true;
		}
		theError = "could not find a free port on 127.0.0.1 for the browser";
		return false;
	}

	int Port() const override
	{
		return mPort;
	}

	bool Wait(const std::string& theState, Query& theQuery, const std::function<bool()>& theKeepWaiting, std::string& theError) override
	{
		theQuery.clear();
		theError.clear();
		if (mListeners.empty())
		{
			theError = "the game is not listening for the browser";
			return false;
		}

		std::vector<Socket> aSockets;
		std::vector<bool> aReady;
		while (theKeepWaiting())
		{
			aSockets = mListeners;
			for (const Client& aClient : mClients)
				aSockets.push_back(aClient.mSocket);
			if (!WaitReadable(aSockets, TICK_MS, aReady))
			{
				theError = std::format("waiting for the browser failed: {}", ErrorText(LastError()));
				return false;
			}

			// read what arrived on the connections there were, then take the new ones
			for (size_t i = mClients.size(); i-- > 0;)
			{
				if (!aReady[mListeners.size() + i])
				{
					if (Clock::now() - mClients[i].mSince > IDLE_LIMIT)
						Drop(i);
					continue;
				}

				char aBuffer[4096];
				int aCount = static_cast<int>(recv(mClients[i].mSocket, aBuffer, sizeof(aBuffer), 0));
				if (aCount < 0 && WouldBlock(LastError()))
					continue;
				if (aCount <= 0)
				{
					Drop(i);	// closed, or reset by the browser
					continue;
				}

				Progress aProgress = Feed(mClients[i], std::string_view(aBuffer, static_cast<size_t>(aCount)));
				if (aProgress == Progress::MORE)
					continue;
				if (aProgress == Progress::COMPLETE && Handle(mClients[i].mSocket, mClients[i].mLine, theState, theQuery))
				{
					mAnswer = mClients[i].mSocket;
					mClients.erase(mClients.begin() + static_cast<ptrdiff_t>(i));
					Close();	// nothing more to listen for
					return true;
				}
				if (aProgress == Progress::LINE_TOO_LONG)
					Answer(mClients[i].mSocket, Page(414, "URI Too Long", "This request is too large."));
				else if (aProgress == Progress::TOO_LARGE)
					Answer(mClients[i].mSocket, Page(431, "Request Header Fields Too Large", "This request is too large."));
				Drop(i);
			}

			bool aStuck = false;
			for (size_t i = 0; i < mListeners.size(); i++)
				if (aReady[i] && !Accept(mListeners[i]))
					aStuck = true;
			if (aStuck)
				std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));	// rather than spin on a readable listener
		}
		return false;
	}

	void Reply(Outcome theOutcome) override
	{
		if (mAnswer == NO_SOCKET)
			return;
		const char* aText = "The sign-in did not go through. Go back to PvZ-Portable to see why. You can close this tab.";
		if (theOutcome == Outcome::DONE)
			aText = "Done. You can close this tab and go back to PvZ-Portable.";
		else if (theOutcome == Outcome::DENIED)
			aText = "PvZ-Portable was not given access. You can close this tab.";
		Answer(mAnswer, Page(200, "OK", aText));
		CloseSocket(mAnswer);
		mAnswer = NO_SOCKET;
	}
};

}

std::unique_ptr<RedirectListener> MakeLoopbackListener()
{
	return std::make_unique<LoopbackListener>();
}

}

#endif
