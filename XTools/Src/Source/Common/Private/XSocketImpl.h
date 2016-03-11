//////////////////////////////////////////////////////////////////////////
// XSocketImpl.h
//
// Implementation of the Connection interface.  Allows us to limit the 
// exposure of the RakNet APIs to code outside the common library
//
// Copyright (C) 2014 Microsoft Corp.  All Rights Reserved
//////////////////////////////////////////////////////////////////////////

#pragma once

#include "Message.h"

XTOOLS_NAMESPACE_BEGIN

class XSocketManagerImpl;

class XSocketImpl : public XSocket
{
	XTOOLS_REFLECTION_DECLARE(XSocketImpl)

public:
	XSocketImpl(const std::string& address, uint16 port);

	//////////////////////////////////////////////////////////////////////////
	// XSocket Functions:

	virtual SocketID			GetID() const XTOVERRIDE;

	// Send message to the remote peer
	virtual void				Send(const byte* message, uint32 messageSize, MessagePriority priority, MessageReliability reliability, MessageChannel channel) XTOVERRIDE;

	// Register the given listener to receive callbacks when events happen on this XSocket.
	// Releasing the given Receipt will automatically unregister the listener
	virtual ReceiptPtr			RegisterListener(XSocketListener* listener) XTOVERRIDE;

	// Returns the (simplified) status of the connection
	virtual Status				GetStatus() const XTOVERRIDE;

	virtual bool				IsConnected() const XTOVERRIDE;

	virtual std::string			GetRemoteSystemName() const XTOVERRIDE;


	//////////////////////////////////////////////////////////////////////////
	// Local Functions:

	// Called by XSocketManager on the main thread when a packet has arrived on this socket. 
	// Returns true if this message was consumed by this function
	bool						OnReceiveMessage(const MessageConstPtr& msg);

	// Called by XSocketManager on the network thread when a packet has arrived on this socket. 
	// Returns true if this message was consumed by this function
	bool						OnReceiveMessageAsync(const MessageConstPtr& msg);

	void						OnOpenFailed();

	const PeerPtr&				GetPeer();
	PeerConstPtr				GetPeer() const;
	void						SetPeer(const PeerPtr& peer);

	PeerID						GetPeerID() const { return m_peerID; }

	RakNet::SystemAddress		GetAddress() const;
	void						SetAddress(const RakNet::SystemAddress& newAddress);

	const RakNet::RakNetGUID&	GetRakNetGUID() const;
	void						SetRakNetGUID(const RakNet::RakNetGUID& guid);

	void						SetRegistrationReceipt(const ReceiptPtr& receipt);

private:
	void						UnregisterListener(XSocketListener* listener);

	void						OnConnected();
	void						OnLostConnection();
	void						OnConnectionAttemptFailed(byte failureID);

	SocketID	                m_id;
	PeerID						m_peerID;
	PeerPtr						m_peer;
	XSocketListener*			m_listener;
	RakNet::SystemAddress		m_address;
	RakNet::RakNetGUID			m_raknetGuid;
	Status						m_status;
	ReceiptPtr					m_receipt;

	static std::atomic<SocketID>	m_sCounter;
};

DECLARE_PTR(XSocketImpl)

XTOOLS_NAMESPACE_END