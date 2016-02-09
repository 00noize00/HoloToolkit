//////////////////////////////////////////////////////////////////////////
// NetworkConnectionImpl.cpp
//
// Implementation of the NetworkConnection interface
//
// Copyright (C) 2014 Microsoft Corp.  All Rights Reserved
//////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "NetworkConnectionImpl.h"
#include "NetworkInMessageImpl.h"


XTOOLS_NAMESPACE_BEGIN

static_assert(static_cast<int>(MessageID::Start) == static_cast<int>(ID_USER_PACKET_ENUM), "XTools internal MessageID::Start must match RakNet's ID_USER_PACKET_ENUM");


//////////////////////////////////////////////////////////////////////////
// Implement a message interceptor to route messages to a specific listener based on the senders guid and the messageID
class NetworkConnectionImpl::NetConnectionInterceptor : public MessageInterceptor
{
public:
	NetConnectionInterceptor(const PeerPtr& peer, const RakNet::RakNetGUID& connectionGUID, NetworkConnectionListener* callback, NetworkConnection* connection, byte messageID)
		: MessageInterceptor(peer)
		, m_guid(connectionGUID)
		, m_callback(callback)
		, m_connection(connection)
		, m_messageID(messageID)
	{}

protected:
	// User code inheriting from this class should implement this class to check the packets to 
	// see if they want to handle it.  Return true if your handling it yourself, false if the message
	// should continue to be routed to the appropriate listener
	virtual bool HandlePacket(RakNet::RakNetGUID guid, const byte* packetData, uint32 packetSize) XTOVERRIDE
	{
		if (guid.g == m_guid.g &&			// the incoming packet is from the remote machine we care about
		packetData[0] == m_messageID)	// the message is the one we're listening for
		{
			// Wrap the message in a NetworkMessage on the stack and call the callback.
			NetworkInMessageImpl msg(packetData, packetSize);

			// Read the message ID off the front before passing it off to the callback
			msg.ReadByte();

			m_callback->OnMessageReceived(m_connection, msg);

			return true;
		}
		else
		{
			return false;
		}
	}

private:
	RakNet::RakNetGUID			m_guid;
	NetworkConnectionListener*			m_callback;
	NetworkConnection*			m_connection;
	byte						m_messageID;
};


class NetworkConnectionImpl::InterceptorProxy : public RefCounted
{
public:
	InterceptorProxy(XSocketManagerImpl* socketMgr, const PeerPtr& peer, const RakNet::RakNetGUID& connectionGUID, NetworkConnectionListener* callback, NetworkConnection* connection, byte messageID)
		: m_interceptor(new NetConnectionInterceptor(peer, connectionGUID, callback, connection, messageID))
		, m_socketMgr(socketMgr)
	{}

	virtual ~InterceptorProxy()
	{
		m_socketMgr->RemoveInterceptor(m_interceptor);
	}

	MessageInterceptorPtr m_interceptor;
	XSocketManagerImpl* m_socketMgr;
};


//////////////////////////////////////////////////////////////////////////
struct NetworkConnectionImpl::AsyncCallback
{
	AsyncCallback() : m_callback(NULL) {}
	NetworkConnectionListener* m_callback;
	ref_ptr<InterceptorProxy> m_interceptorProxy;
};


//////////////////////////////////////////////////////////////////////////
NetworkConnectionImpl::NetworkConnectionImpl(const NetworkMessagePoolPtr& messagePool)
	: m_messagePool(messagePool)
	, m_messageBuffer(new byte[kDefaultMessageBufferSize])
	, m_messageBufferSize(kDefaultMessageBufferSize)
{
	m_connectionGUID = RakNet::RakPeerInterface::Get64BitUniqueRandomNumber();
}


ConnectionGUID NetworkConnectionImpl::GetConnectionGUID() const
{
	return m_connectionGUID;
}


bool NetworkConnectionImpl::IsConnected() const
{
	return (m_socket != NULL && m_socket->GetStatus() == XSocket::Connected);
}


void NetworkConnectionImpl::Send(const NetworkOutMessagePtr& msg, MessagePriority priority, MessageReliability reliability, MessageChannel channel, bool releaseMessage)
{
	if (IsConnected())
	{
		m_socket->Send(msg->GetData(), msg->GetSize(), priority, reliability, channel);
	}
	else
	{
		LogError("Trying to send a message to a remote host that is not connected");
	}

	if (releaseMessage)
	{
		m_messagePool->ReturnMessage(msg);
	}
}


// Instruct the recipient to sent this messages on to all other connected peers
void NetworkConnectionImpl::Broadcast(const NetworkOutMessagePtr& msg, MessagePriority priority, MessageReliability reliability, MessageChannel channel, bool releaseMessage)
{
	if (IsConnected())
	{
		// Append a broadcast header to the outgoing message

		const uint32 msgSize = msg->GetSize();

		const uint32 sendPacketSize = msgSize + sizeof(NetworkHeader);
		if (m_messageBufferSize < sendPacketSize)
		{
			m_messageBuffer = new byte[sendPacketSize];
		}

		// Write a header onto the front of the buffer
		NetworkHeader* header = reinterpret_cast<NetworkHeader*>(m_messageBuffer.get());
		header->m_messageID = MessageID::Broadcast;
		header->m_priority = priority;
		header->m_reliability = reliability;
		header->m_channel = channel;

		// Write the message onto the rest of the buffer
		byte* payload = m_messageBuffer.get() + sizeof(NetworkHeader);
		memcpy(payload, msg->GetData(), msgSize);

		// Send the constructed packet
		m_socket->Send(m_messageBuffer.get(), sendPacketSize, priority, reliability, channel);
	}
	else
	{
		LogError("Trying to send a message to a remote host that is not connected");
	}

	if (releaseMessage)
	{
		m_messagePool->ReturnMessage(msg);
	}
}


void NetworkConnectionImpl::AddListener(byte messageType, NetworkConnectionListener* newListener)
{
	// If the message ID being registered for is outside the valid range, then set it to be
	// StatusOnly messages: the listener will still get connect/disconnect notifications, but will not receive any messages
	if (messageType < MessageID::Start)
	{
		messageType = MessageID::StatusOnly;
	}

	ListenerListPtr list = m_listeners[messageType];
	if (!list)
	{
		list = ListenerList::Create();
		m_listeners[messageType] = list;
	}

	list->AddListener(newListener);
}


void NetworkConnectionImpl::RemoveListener(byte messageType, NetworkConnectionListener* oldListener)
{
	// If the message ID being registered for is outside the valid range, then set it to be
	// StatusOnly messages: the listener will still get connect/disconnect notifications, but will not receive any messages
	if (messageType < MessageID::Start)
	{
		messageType = MessageID::StatusOnly;
	}

	ListenerListPtr list = m_listeners[messageType];
	if (list)
	{
		list->RemoveListener(oldListener);
	}
}


bool NetworkConnectionImpl::RegisterAsyncCallback(byte messageType, NetworkConnectionListener* cb)
{
	auto callbackIter = m_asyncCallbacks.find(messageType);

	if (messageType >= MessageID::Start &&	// Make sure this message type is in the valid range
		(callbackIter == m_asyncCallbacks.end() || callbackIter->second.m_callback == NULL)) // Make sure the message type does not already have a callback registered
	{
		// If a callback is not registered, then allow this one to register
		AsyncCallback callbackInfo;
		callbackInfo.m_callback = cb;

		if (IsConnected())
		{
			XSocketImpl* socketImpl = static_cast<XSocketImpl*>(m_socket.get());
			callbackInfo.m_interceptorProxy = new InterceptorProxy(socketImpl->GetSocketManager(), socketImpl->GetPeer(), socketImpl->GetRakNetGUID(), cb, this, messageType);
			socketImpl->GetSocketManager()->AddInterceptor(callbackInfo.m_interceptorProxy->m_interceptor);
		}

		m_asyncCallbacks[messageType] = callbackInfo;

		return true;
	}
	else
	{
		// Return NULL to indicate that registration failed
		return false;
	}
}


void NetworkConnectionImpl::UnregisterAsyncCallback(byte messageType)
{
	auto callback = m_asyncCallbacks.find(messageType);
	if (XTVERIFY(callback != m_asyncCallbacks.end()))
	{
		m_asyncCallbacks.erase(callback);
	}
}


NetworkOutMessagePtr NetworkConnectionImpl::CreateMessage(byte messageType)
{
	NetworkOutMessagePtr newMessage = m_messagePool->AcquireMessage();
	newMessage->Write(messageType);

	return newMessage;
}


void NetworkConnectionImpl::ReturnMessage(const NetworkOutMessagePtr& msg)
{
	XTASSERT(msg);
	m_messagePool->ReturnMessage(msg);
}


void NetworkConnectionImpl::Disconnect()
{
	if (m_socket != NULL && 
		m_socket->GetStatus() != XSocket::Disconnected && 
		m_socket->GetStatus() != XSocket::Disconnecting)
	{
		LogInfo("Intentionally closing connection");
		OnDisconnected(m_socket);
	}
}


XSocketPtr NetworkConnectionImpl::GetConnection() const
{
	return m_socket;
}


XStringPtr NetworkConnectionImpl::GetRemoteAddress() const
{
	if (m_socket)
	{
		return new XString(m_socket->GetRemoteSystemName());
	}
	else
	{
		return nullptr;
	}
}


const XSocketPtr& NetworkConnectionImpl::GetSocket() const
{
	return m_socket;
}


void NetworkConnectionImpl::SetSocket(const XSocketPtr& connection)
{
    if (connection)
    {
		if (m_socket)
		{
			LogInfo("NetworkConnection: Replacing an existing socket with a new one");
		}

        m_socket = connection;
        m_listenerReceipt = m_socket->RegisterListener(this);

        // If this connection is already connected, then trigger an OnConnected callback
        if (m_socket->GetStatus() == XSocket::Connected)
        {
            OnConnected(m_socket);
        }
    }
    else
    {
		if (m_socket)
		{
			if (m_socket->GetStatus() == XSocket::Connected)
			{
				LogInfo("NetworkConnection: Clearing open socket");
				OnDisconnected(m_socket);
			}
			else
			{
				m_listenerReceipt = NULL;
				m_socket = NULL;
			}
		}
    }
}


const NetworkMessagePoolPtr& NetworkConnectionImpl::GetMessagePool()
{
	return m_messagePool;
}


void NetworkConnectionImpl::OnConnected(const XSocketPtr& connection)
{
	XTASSERT(connection == m_socket);
	XT_UNREFERENCED_PARAM(connection);	// Necessary for when XTASSET is not defined

	// Prevent this object from getting destroyed while iterating through callbacks
	NetworkConnectionPtr thisPtr(this);

	for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it)
	{
		it->second->NotifyListeners(&NetworkConnectionListener::OnConnected, thisPtr);
	}

	for (auto it = m_asyncCallbacks.begin(); it != m_asyncCallbacks.end(); ++it)
	{
		if (it->second.m_callback)
		{
			it->second.m_callback->OnConnected(thisPtr);

			XSocketImpl* socketImpl = static_cast<XSocketImpl*>(connection.get());

			it->second.m_interceptorProxy = new InterceptorProxy(socketImpl->GetSocketManager(), socketImpl->GetPeer(), socketImpl->GetRakNetGUID(), it->second.m_callback, this, it->first);
			socketImpl->GetSocketManager()->AddInterceptor(it->second.m_interceptorProxy->m_interceptor);
		}
	}
}


void NetworkConnectionImpl::OnConnectionFailed(const XSocketPtr& connection, FailureReason)
{
	XTASSERT(connection == m_socket);
	XT_UNREFERENCED_PARAM(connection);	// Necessary for when XTASSET is not defined

	// Prevent this object from getting destroyed while iterating through callbacks
	NetworkConnectionPtr thisPtr(this);

	m_socket = NULL;
	m_listenerReceipt = NULL;

	for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it)
	{
		it->second->NotifyListeners(&NetworkConnectionListener::OnConnectFailed, thisPtr);
	}

	for (auto it = m_asyncCallbacks.begin(); it != m_asyncCallbacks.end(); ++it)
	{
		if (it->second.m_callback)
		{
			if (it->second.m_interceptorProxy)
			{
				it->second.m_interceptorProxy = NULL;
			}

			it->second.m_callback->OnConnectFailed(thisPtr);
		}
	}
}


void NetworkConnectionImpl::OnDisconnected(const XSocketPtr& connection)
{
	XTASSERT(connection == m_socket);
	XT_UNREFERENCED_PARAM(connection);	// Necessary for when XTASSET is not defined

	LogInfo("NetworkConnection Disconnected");

	// Prevent this object from getting destroyed while iterating through callbacks
	NetworkConnectionPtr thisPtr(this);

	m_socket = NULL;
	m_listenerReceipt = NULL;

	for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it)
	{
		it->second->NotifyListeners(&NetworkConnectionListener::OnDisconnected, thisPtr);
	}

	for (auto it = m_asyncCallbacks.begin(); it != m_asyncCallbacks.end(); ++it)
	{
		if (it->second.m_callback)
		{
			if (it->second.m_interceptorProxy)
			{
				it->second.m_interceptorProxy = NULL;
			}

			it->second.m_callback->OnDisconnected(thisPtr);
		}
	}
}


void NetworkConnectionImpl::OnMessageReceived(const XSocketPtr& connection, const byte* message, uint32 messageLength)
{
	XTASSERT(connection == m_socket);
	XT_UNREFERENCED_PARAM(connection);	// Necessary for when XTASSET is not defined

	auto callbackIter = m_listeners.find(message[0]);
	if (callbackIter != m_listeners.end())
	{
		// Make sure we pass a different message instance to each listener so that they can independently
		// consume content from the message.  Otherwise, two listeners for the same message type will each
		// advance the state of the NetworkInMessageImpl.
		for (int32 i = callbackIter->second->GetListenerCount() - 1; i >= 0; i--)
		{
		// Wrap the message in a NetworkMessage on the stack and call the callback.
		NetworkInMessageImpl msg(message, messageLength);

		// Read the message ID off the front before passing it off to the callback
		msg.ReadByte();

			callbackIter->second->NotifyListener(i, &NetworkConnectionListener::OnMessageReceived, this, msg);
		}
	}
}


XTOOLS_NAMESPACE_END
