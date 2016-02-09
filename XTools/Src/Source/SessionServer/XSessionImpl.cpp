//////////////////////////////////////////////////////////////////////////
// XSessionImpl.cpp
//
// The XTools server's representation of an active session
//
// Copyright (C) 2014 Microsoft Corp.  All Rights Reserved
//////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "XSessionImpl.h"
#include "PortmachinePool.h"
#include <Private/NetworkConnectionImpl.h>
#include <Private/Utils/ScopedLock.h>
#include <Private/JSONMessage.h>
#include <Private/TunnelConnection.h>

XTOOLS_NAMESPACE_BEGIN

// The amount of time an adhoc session can be empty (in milliseconds) before it 
// attempts to close itself.  Currently only applies when a user creates a session and
// then fails to join it.  Otherwise it will close itself when the last user disconnects.  
static const uint64 kEmptySessionTimeout = 5000;

struct XSessionImpl::RemoteClient : public RefCounted
{
public:
	RemoteClient(const NetworkConnectionImplPtr& connection)
		: m_desktopConnection(connection)
		, m_barabooConnection(new TunnelConnection(connection))
		, m_userID(0)
		, m_userMuteState(false)
	{}

	NetworkConnectionPtr	m_desktopConnection;
	NetworkConnectionPtr	m_barabooConnection;
	ReceiptPtr				m_listenerReceipt;
	std::string				m_userName;
	uint32					m_userID;
	bool                    m_userMuteState;
};




XSessionImpl::XSessionImpl(const std::string& name, PortMachinePair pmp, SessionType type, unsigned int id)
: m_messagePool(new NetworkMessagePool(kDefaultMessagePoolSize))
, m_broadcaster(new BroadcastForwarder())
, m_audioSessionProcessor(new AudioSessionProcessorServer())
, m_socketMgr(XSocketManager::Create())
, m_name(name)
, m_stopping(0)
, m_portMachinePair(pmp)
, m_TypeOfSession(type)
, m_id(id)
, m_emptyTime(0)
, m_bEmptyCheckApplied(false)
{
	m_syncMgr = Sync::SyncManager::Create(Sync::AuthorityLevel::High, new UserImpl("SessionServer", User::kInvalidUserID, false));

	// Start listening for new connections
	m_listenerReceipt = m_socketMgr->AcceptConnections(pmp.portID, kSessionServerMaxConnections, this);
	
	// Check that we successfully opened a socket to listen for connections on
	if (m_listenerReceipt != NULL)
	{
		m_messageRouter.RegisterHandler(new MessageHandlerProxyT<const JoinSessionRequest&>(CreateCallback2(this, &XSessionImpl::OnJoinSessionRequest)));
		m_messageRouter.RegisterHandler(new MessageHandlerProxyT<const UserChangedSessionMsg&>(CreateCallback2(this, &XSessionImpl::OnUserChanged)));

		// Start a thread to run the main service logic. 
		m_serverThread = new MemberFuncThread(&XSessionImpl::ServerThreadFunc, this);
	}
}


XSessionImpl::~XSessionImpl()
{
	// trigger the thread exit and wait for it...
	m_stopping = 1;
	if (m_serverThread)
	{
		m_serverThread->WaitForThreadExit();
	}
	
	LogInfo("Session %s closed", m_name.c_str());
}


ReceiptPtr XSessionImpl::RegisterCallback(SessionChangeCallback* cb)
{
	byte messageType = 0;
	m_callback = cb; // there is only one of these (for now)
	return CreateRegistrationReceipt(XSessionImplPtr(this), &XSessionImpl::UnregisterCallback, messageType);
}


void XSessionImpl::UnregisterCallback(byte message)
{
	XT_UNREFERENCED_PARAM(message);

	m_callback = NULL;
}


unsigned int XSessionImpl::GetId() const
{
	return m_id;
}


std::string XSessionImpl::GetName() const
{
	return m_name;
}


SessionType XSessionImpl::GetType() const
{
	return m_TypeOfSession;
}


int32 XSessionImpl::GetUserCount() const
{
	return static_cast<int32>(m_clients.size());
}


std::string XSessionImpl::GetSessionUserName(int32 i) const
{
	return m_clients[i]->m_userName;
}


uint32 XSessionImpl::GetSessionUserID(int32 i) const
{
	return m_clients[i]->m_userID;
}

bool XSessionImpl::GetUserMuteState(int32 i) const
{
	return m_clients[i]->m_userMuteState;
}


void XSessionImpl::AddConnection(const XSocketPtr& socket)
{
	NetworkConnectionImplPtr netConnection = new NetworkConnectionImpl(m_messagePool);
	netConnection->SetSocket(socket);

	RemoteClientPtr remoteClient = new RemoteClient(netConnection);

	// Register for callbacks with the SessionStatus id
	remoteClient->m_desktopConnection->AddListener(MessageID::SessionControl, this);
	remoteClient->m_listenerReceipt = CreateRegistrationReceipt(remoteClient->m_desktopConnection, &NetworkConnection::RemoveListener, MessageID::SessionControl, this);

	// Add it to the pending list until we receive the a join request message from it
	m_pendingClients.push_back(remoteClient);
}


void XSessionImpl::OnDisconnected(const NetworkConnectionPtr& connection)
{
	bool found = false;

	// Find the remote client with the given connection and remove it
	for (size_t i = 0; i < m_clients.size(); ++i)
	{
		if (m_clients[i]->m_desktopConnection == connection)
		{
			RemoteClientPtr remoteClient = m_clients[i];

			m_clients.erase(m_clients.begin() + i);

			m_broadcaster->RemoveConnection(remoteClient->m_desktopConnection);
			m_broadcaster->RemoveConnection(remoteClient->m_barabooConnection);
			m_audioSessionProcessor->RemoveConnection(remoteClient->m_barabooConnection);
			m_syncMgr->RemoveConnection(connection);

			// Notify the session server to tell all the clients that this user has left the session
			if (m_callback)
			{
				m_callback->OnUserLeftSession(m_id, remoteClient->m_userID);
			}
			
			found = true;
			break;
		}
	}

	// Check the pending list for the connection if not found
	if (!found)
	{
		for (size_t i = 0; i < m_pendingClients.size(); ++i)
		{
			if (m_pendingClients[i]->m_desktopConnection == connection)
			{
				m_pendingClients.erase(m_pendingClients.begin() + i);
				break;
			}
		}
	}

	CheckIfEmpty(true);
}


void XSessionImpl::OnMessageReceived(const NetworkConnectionPtr& connection, NetworkInMessage& message)
{
	XStringPtr command = message.ReadString();

	JSONMessagePtr jMsg = JSONMessage::CreateFromMessage(command->GetString());

	// Route the incoming message to the appropriate function to handle it
	if (!m_messageRouter.CallHandler(jMsg, connection))
	{
		// There was a problem with the message that was sent.  Boot the connection
		connection->Disconnect();
	}
}


void XSessionImpl::OnNewConnection(const XSocketPtr& newConnection)
{
	LogInfo("Session %s: New connection from %s, starting handshake", m_name.c_str(), newConnection->GetRemoteSystemName().c_str());

	HandshakeCallback callback = CreateCallback3(this, &XSessionImpl::OnHandshakeComplete);

	m_pendingConnections[newConnection->GetID()] = new NetworkHandshake(newConnection, new SessionHandshakeLogic(true), callback);
}


void XSessionImpl::OnHandshakeComplete(const XSocketPtr& newConnection, SocketID socketID, HandshakeResult result)
{
	if (newConnection && result == HandshakeResult::Success)
	{
		AddConnection(newConnection);
	}
	else
	{
		LogInfo("Session %s: Handshake from %s failed with error %u", m_name.c_str(), newConnection ? newConnection->GetRemoteSystemName().c_str() : "unknown machine", result);
	}

	XTVERIFY(m_pendingConnections.erase(socketID));
}


PortMachinePair XSessionImpl::GetPortMachinePair()
{
	return m_portMachinePair;
}


SessionDescriptorImplPtr XSessionImpl::GetSessionDescription() const
{
	SessionDescriptorImplPtr sessionDesc = new SessionDescriptorImpl();

	sessionDesc->SetName(GetName());
	sessionDesc->SetID(GetId());
	sessionDesc->SetSessionType(GetType());
	sessionDesc->SetAddress(m_portMachinePair.address);
	sessionDesc->SetPortID(m_portMachinePair.portID);

	int32 numUsers = GetUserCount();
	sessionDesc->SetUserCount(numUsers);

	for (int32 userIndex = 0; userIndex < numUsers; ++userIndex)
	{
		UserImplPtr tmpUser = new UserImpl(
			GetSessionUserName(userIndex),
			GetSessionUserID(userIndex),
			GetUserMuteState(userIndex));

		sessionDesc->SetUser(userIndex, tmpUser);
	}

	return sessionDesc;
}


// Returns false if the session was unable to initialize itself correctly
bool XSessionImpl::IsInitialized() const
{
	return (m_listenerReceipt != NULL && m_serverThread != NULL);
}


// The entry point for the main thread of the session server
void XSessionImpl::ServerThreadFunc()
{
	m_lastEmptyCheckTime = std::chrono::high_resolution_clock::now();

	// Periodically check if the service is stopping.
	while (m_stopping == 0)
	{
		m_socketMgr->Update();
		m_syncMgr->Update();

		CheckIfEmpty(false);

		// Don't hog the whole CPU
		Platform::SleepMS(10);
	}
}


void XSessionImpl::OnUserChanged(const UserChangedSessionMsg& request, const NetworkConnectionPtr& connection)
{
	RemoteClientPtr remoteClient = GetExistingClientForConnection(connection);

	if (XTVERIFY(remoteClient))
	{
		// Copy the updated information in to the remoteClient.
		remoteClient->m_userName = request.GetSessionUserName();
		remoteClient->m_userID = request.GetSessionUserID();
		remoteClient->m_userMuteState = request.GetSessionUserMuteState();
		m_callback->OnUserChanged(m_id, remoteClient->m_userName, remoteClient->m_userID, remoteClient->m_userMuteState);
	}
}


void XSessionImpl::OnJoinSessionRequest(const JoinSessionRequest& request, const NetworkConnectionPtr& connection)
{
	// Note: this call will remove the remote client from the list of pending connections
	RemoteClientPtr remoteClient = GetPendingClientForConnection(connection);

	if (remoteClient)
	{
		// Fill in the rest of the info about the user
		remoteClient->m_userName = request.GetUserName();
		remoteClient->m_userID = request.GetUserID();
		remoteClient->m_userMuteState = request.GetMuteState();

		// Check to see if this userID is already in use by someone in the session
		bool bDuplicateUserID = false;
		for (size_t clientIndex = 0; clientIndex < m_clients.size(); ++clientIndex)
		{
			if (m_clients[clientIndex]->m_userID == remoteClient->m_userID)
			{
				bDuplicateUserID = true;
				LogError("UserID %i in session join request is a duplicate of a user already in this session.  ", remoteClient->m_userID);
				break;
			}
		}

		// Check to see if the userID is valid
		bool bIsInvalidUserID = false;
		if (remoteClient->m_userID == User::kInvalidUserID)
		{
			LogError("Received invalid userID");
			bIsInvalidUserID = true;
		}

		// If the connecting user is invalid, then send a failure response and shut down the connection
		if (bIsInvalidUserID || bDuplicateUserID)
		{
			// Reply to the user that they have failed to join the session
			{
				JoinSessionReply reply(false);

				NetworkOutMessagePtr msg = connection->CreateMessage(MessageID::SessionControl);
				msg->Write(reply.ToJSONString());
				connection->Send(msg);
			}

			// Disconnect
			connection->Disconnect();
		}
		else
		{
			// Add the user to the real list of clients in the session
			m_clients.push_back(remoteClient);

			// Reply to the user that they have now joined the session successfully
			{
				JoinSessionReply reply(true);

				NetworkOutMessagePtr msg = connection->CreateMessage(MessageID::SessionControl);
				msg->Write(reply.ToJSONString());
				connection->Send(msg);
			}

			// Add the remoteClient to the list that can send and receive broadcasts
			m_broadcaster->AddConnection(remoteClient->m_desktopConnection);
			m_broadcaster->AddConnection(remoteClient->m_barabooConnection);

			// Add the remoteClient to the audio packet processor
			m_audioSessionProcessor->AddConnection(remoteClient->m_barabooConnection);

			// Add the remoteClient to the list that can share the session's sync data
			m_syncMgr->AddConnection(remoteClient->m_desktopConnection);

			// Notify the session server to tell all the clients that the new user has joined this session
			m_callback->OnUserJoinedSession(m_id, remoteClient->m_userName, remoteClient->m_userID, remoteClient->m_userMuteState);
		}
	}
}


XSessionImpl::RemoteClientPtr XSessionImpl::GetPendingClientForConnection(const NetworkConnectionPtr& connection)
{
	// Find this remote peer in the list of pending connections
	RemoteClientPtr remoteClient = NULL;
	for (size_t i = 0; i < m_pendingClients.size(); ++i)
	{
		if (m_pendingClients[i]->m_desktopConnection == connection)
		{
			remoteClient = m_pendingClients[i];

			m_pendingClients.erase(m_pendingClients.begin() + i);

			break;
		}
	}

	return remoteClient;
}


XSessionImpl::RemoteClientPtr XSessionImpl::GetExistingClientForConnection(const NetworkConnectionPtr& connection)
{
	// Find this remote peer in the list of pending connections
	RemoteClientPtr remoteClient = NULL;
	for (size_t i = 0; i < m_clients.size(); ++i)
	{
		if (m_clients[i]->m_desktopConnection == connection)
		{
			remoteClient = m_clients[i];
			break;
		}
	}

	return remoteClient;
}


void XSessionImpl::CheckIfEmpty(bool resetImmediately)
{
	std::chrono::high_resolution_clock::time_point currentTime = std::chrono::high_resolution_clock::now();

	uint64 timeDelta = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - m_lastEmptyCheckTime).count();

	m_lastEmptyCheckTime = currentTime;

	if (m_clients.empty())
	{
		m_emptyTime += timeDelta;
		if (!m_bEmptyCheckApplied && (resetImmediately || m_emptyTime > kEmptySessionTimeout))
		{
			LogInfo("No more clients, sending OnSessionEmpty message to parent.");

			// Delete the old sync manager and create a new one to ensure that all the old
			// sync data is cleaned up
			UserPtr serverUser = m_syncMgr->GetLocalUser();
			m_syncMgr = Sync::SyncManager::Create(Sync::AuthorityLevel::High, serverUser);

			m_callback->OnSessionEmpty(this);

			// If we're an adhoc session, and we're empty, then start closing our connections
			if (m_TypeOfSession == SessionType::ADHOC)
			{
				// Stop receiving new connections
				m_listenerReceipt = NULL;

				// Close any pending connections
				m_pendingClients.clear();
				m_pendingConnections.clear();
			}

			m_bEmptyCheckApplied = true;
		}
	}
	else
	{
		m_bEmptyCheckApplied = false;
		m_emptyTime = 0;
	}
}


XTOOLS_NAMESPACE_END
