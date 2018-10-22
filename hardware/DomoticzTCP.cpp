#include "stdafx.h"
#include "DomoticzTCP.h"
#include "../main/Logger.h"
#include "../main/Helper.h"
#include "../main/localtime_r.h"
#include "../main/mainworker.h"
#include "../main/WebServerHelper.h"
#include "../webserver/proxyclient.h"

#ifdef WIN32
#define SHUT_RDWR SD_BOTH
#endif

#define RETRY_DELAY 30

extern http::server::CWebServerHelper m_webservers;

DomoticzTCP::DomoticzTCP(const int ID, const std::string &IPAddress, const unsigned short usIPPort, const std::string &username, const std::string &password) :
	m_username(username), m_password(password), m_szIPAddress(IPAddress)
{
	m_HwdID = ID;
	m_usIPPort = usIPPort;
	m_bIsStarted = false;
#ifndef NOCLOUD
	b_useProxy = IsValidAPIKey(m_szIPAddress);
	b_ProxyConnected = false;
#endif
}

DomoticzTCP::~DomoticzTCP(void)
{
}

#ifndef NOCLOUD
bool DomoticzTCP::IsValidAPIKey(const std::string &IPAddress)
{
	if (IPAddress.find(".") != std::string::npos) {
		// we assume an IPv4 address or host name
		return false;
	}
	if (IPAddress.find(":") != std::string::npos) {
		// we assume an IPv6 address
		return false;
	}
	// just a simple check
	return IPAddress.length() == 15;
}
#endif

bool DomoticzTCP::StartHardware()
{
	RequestStart();

#ifndef NOCLOUD
	b_useProxy = IsValidAPIKey(m_szIPAddress);
	if (b_useProxy) {
		return StartHardwareProxy();
	}
#endif
	//Start worker thread
	m_thread = std::make_shared<std::thread>(&DomoticzTCP::Do_Work, this);
	SetThreadName(m_thread->native_handle(), "DomoticzTCP");

	return (m_thread != nullptr);
}

bool DomoticzTCP::StopHardware()
{
#ifndef NOCLOUD
	if (b_useProxy) {
		return StopHardwareProxy();
	}
#endif

	if (m_thread)
	{
		RequestStop();
		m_thread->join();
		m_thread.reset();
	}
	m_bIsStarted = false;
	return true;
}

void DomoticzTCP::OnConnect()
{
	_log.Log(LOG_STATUS, "DomoticzTCP: connected to: %s:%d", m_szIPAddress.c_str(), m_usIPPort);
}

void DomoticzTCP::OnDisconnect()
{
	_log.Log(LOG_STATUS, "DomoticzTCP: disconnected from: %s:%d", m_szIPAddress.c_str(), m_usIPPort);
}

void DomoticzTCP::OnData(const unsigned char *pData, size_t length)
{
	std::lock_guard<std::mutex> l(readQueueMutex);
	onInternalMessage((const unsigned char *)pData, length, false); // Do not check validity, this might be non RFX-message
}

void DomoticzTCP::OnError(const std::exception e)
{
	_log.Log(LOG_ERROR, "DomoticzTCP: Error: %s", e.what());
}

void DomoticzTCP::OnError(const boost::system::error_code& error)
{
	if (
		(error == boost::asio::error::address_in_use) ||
		(error == boost::asio::error::connection_refused) ||
		(error == boost::asio::error::access_denied) ||
		(error == boost::asio::error::host_unreachable) ||
		(error == boost::asio::error::timed_out)
		)
	{
		_log.Log(LOG_ERROR, "DomoticzTCP: Can not connect to: %s:%d", m_szIPAddress.c_str(), m_usIPPort);
	}
	else if (
		(error == boost::asio::error::eof) ||
		(error == boost::asio::error::connection_reset)
		)
	{
		_log.Log(LOG_STATUS, "DomoticzTCP: Connection reset!");
	}
	else
	{
		_log.Log(LOG_ERROR, "DomoticzTCP: %s", error.message().c_str());
	}
}

void DomoticzTCP::ConnectInternal()
{
	connect(m_szIPAddress, m_usIPPort);
	while (!IsStopRequested(1000))
	{
		if (ASyncTCP::isConnected())
			break;
	}

	if (m_username != "")
	{
		char szAuth[300];
		snprintf(szAuth, sizeof(szAuth), "AUTH;%s;%s", m_username.c_str(), m_password.c_str());
		WriteToHardware((const char*)&szAuth, (const unsigned char)strlen(szAuth));
	}
	sOnConnected(this);
}

void DomoticzTCP::Do_Work()
{
	int sec_counter = 0;
	ConnectInternal();
	while (!IsStopRequested(1000))
	{
		sec_counter++;
		if (sec_counter % 12 == 0) {
			mytime(&m_LastHeartbeat);
		}
	}
	terminate();

	_log.Log(LOG_STATUS, "DomoticzTCP: Worker stopped...");
}

bool DomoticzTCP::WriteToHardware(const char *pdata, const unsigned char length)
{
#ifndef NOCLOUD
	if (b_useProxy)
	{
		if (isConnectedProxy())
		{
			writeProxy(pdata, length);
			return true;
		}
	}
	else if (ASyncTCP::isConnected())
	{
		write(std::string((const char*)pdata, length));
		return true;
	}
#else
	if (ASyncTCP::isConnected())
	{
		write(std::string((const char*)pdata, length));
		return true;
	}
#endif
	return false;
}

bool DomoticzTCP::isConnected()
{
#ifndef NOCLOUD
	if (b_useProxy)
		return isConnectedProxy();
	else
		return ASyncTCP::isConnected();
#else
	return ASyncTCP::isConnected();
#endif
}

#ifndef NOCLOUD
bool DomoticzTCP::CompareToken(const std::string &aToken)
{
	return (aToken == token);
}

bool DomoticzTCP::CompareId(const std::string &instanceid)
{
	return (m_szIPAddress == instanceid);
}

bool DomoticzTCP::StartHardwareProxy()
{
	if (m_bIsStarted) {
		return false; // dont start twice
	}
	m_bIsStarted = true;
	return ConnectInternalProxy();
}

bool DomoticzTCP::ConnectInternalProxy()
{
	std::shared_ptr<http::server::CProxyClient> proxy;
	const int version = 1;
	// we temporarily use the instance id as an identifier for this connection, meanwhile we get a token from the proxy
	// this means that we connect connect twice to the same server
	token = m_szIPAddress;
	proxy = m_webservers.GetProxyForMaster(this);
	if (proxy) {
		proxy->ConnectToDomoticz(m_szIPAddress, m_username, m_password, this, version);
		sOnConnected(this); // we do need this?
	}
	else {
		_log.Log(LOG_STATUS, "Delaying Domoticz master login");
	}
	return true;
}

bool DomoticzTCP::StopHardwareProxy()
{
	DisconnectProxy();
	m_bIsStarted = false;
	// Avoid dangling pointer if this hardware is removed.
	m_webservers.RemoveMaster(this);
	return true;
}

void DomoticzTCP::DisconnectProxy()
{
	std::shared_ptr<http::server::CProxyClient> proxy;

	proxy = m_webservers.GetProxyForMaster(this);
	if (proxy) {
		proxy->DisconnectFromDomoticz(token, this);
	}
	b_ProxyConnected = false;
}

bool DomoticzTCP::isConnectedProxy()
{
	return b_ProxyConnected;
}

void DomoticzTCP::writeProxy(const char *data, size_t size)
{
	/* send data to slave */
	if (isConnectedProxy()) {
		std::shared_ptr<http::server::CProxyClient> proxy = m_webservers.GetProxyForMaster(this);
		if (proxy) {
			proxy->WriteMasterData(token, data, size);
		}
	}
}

void DomoticzTCP::FromProxy(const unsigned char *data, size_t datalen)
{
	/* data received from slave */
	std::lock_guard<std::mutex> l(readQueueMutex);
	onInternalMessage(data, datalen);
}

std::string DomoticzTCP::GetToken()
{
	return token;
}

void DomoticzTCP::Authenticated(const std::string &aToken, bool authenticated)
{
	b_ProxyConnected = authenticated;
	token = aToken;
	if (authenticated) {
		_log.Log(LOG_STATUS, "Domoticz TCP connected via Proxy.");
	}
}

void DomoticzTCP::SetConnected(bool connected)
{
	if (connected) {
		ConnectInternalProxy();
	}
	else {
		b_ProxyConnected = false;
	}
}
#endif

