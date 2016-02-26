//////////////////////////////////////////////////////////////////////////
// RoomManagerImpl.h
//
// Copyright (C) 2016 Microsoft Corp.  All Rights Reserved
//////////////////////////////////////////////////////////////////////////

#pragma once

#include "RoomImpl.h"

XTOOLS_NAMESPACE_BEGIN

class RoomManagerImpl : public RoomManager, public ObjectElementListener
{
public:
	RoomManagerImpl(const ClientContextConstPtr& context);

	//////////////////////////////////////////////////////////////////////////
	// RoomManager Functions:

	/// Register an object to receive callbacks when an async operation is complete.  
	/// Multiple listeners can be registered.  The wrapper class will hold a reference to the listener
	/// to ensure it is not garbage collected until this class is destroyed or the listener is removed. 
	/// \param newListener The listener object that will receive callbacks
	virtual void AddListener(RoomManagerListener* newListener) XTOVERRIDE;

	/// Remove a previously registered listener.  The wrapper class will release its reference to the given listener.  
	/// \param oldListener The listener object that will no longer receive callbacks  
	virtual void RemoveListener(RoomManagerListener* oldListener) XTOVERRIDE;

	/// Returns the number of rooms that are available in the current session
	virtual int32 GetRoomCount() const XTOVERRIDE;

	/// Returns the room at the given index
	virtual RoomPtr GetRoom(int32 index) XTOVERRIDE;

	/// Returns the room that the local user is currently a part of
	virtual RoomPtr GetCurrentRoom() XTOVERRIDE;

	/// Creates a new room with the given name and ID and adds the local user to that room.
	/// The room is created immediately and the remote devices are notified asynchronously.  
	/// Returns the newly created room if successful, or null if a room with the same name or ID already exists 
	virtual RoomPtr CreateRoom(const XStringPtr& roomName, RoomID roomID) XTOVERRIDE;

	/// Add the local user to the given room.  If the user is currently in another room, they will automatically 
	/// leave the old room before joining the new one.  
	/// Returns true on success
	virtual bool JoinRoom(const RoomPtr& room) XTOVERRIDE;

	/// Remove the local user from the given room.  
	/// Returns true on success
	virtual bool LeaveRoom() XTOVERRIDE;

	/// Returns the number of anchors for a particular room
	virtual int32 GetAnchorCount(const RoomPtr& room) XTOVERRIDE;

	/// Returns the name of the anchor at the given index for the given room
	virtual XStringPtr GetAnchorName(const RoomPtr& room, int32 anchorIndex) XTOVERRIDE;

	/// Begins the asynchronous download of an anchor in the given room from the session server.  
	/// Returns an AnchorDownloadRequest object that allows the user to track or cancel the download and retrieve the
	/// data once its finished downloading
	virtual AnchorDownloadRequestPtr DownloadAnchor(const RoomPtr& room, const XStringPtr& anchorName) XTOVERRIDE;

	/// Begin the asynchronous upload of an anchor.  A copy of the data is created internally,
	/// so it is safe to release once this function returns.  
	/// Returns false if an upload is already in progress.  
	virtual bool UploadAnchor(const RoomPtr& room, const XStringPtr& anchorName, const byte* data, int32 dataSize) XTOVERRIDE;


private:
	//////////////////////////////////////////////////////////////////////////
	// ObjectElementListener Functions: 
	virtual void OnElementAdded(const ElementPtr& element) XTOVERRIDE;
	virtual void OnElementDeleted(const ElementPtr& element) XTOVERRIDE;

	typedef ListenerList<RoomManagerListener> RoomListenerList;
	DECLARE_PTR(RoomListenerList);

	ClientContextConstPtr					m_context;
	RoomListenerListPtr						m_listenerList;
	ObjectElementPtr						m_element;
	std::vector<RoomImplPtr>				m_roomList;
	RoomImplPtr								m_currentRoom;
};

DECLARE_PTR(RoomManagerImpl)

XTOOLS_NAMESPACE_END
