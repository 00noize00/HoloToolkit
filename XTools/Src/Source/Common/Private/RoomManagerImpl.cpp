//////////////////////////////////////////////////////////////////////////
// RoomManagerImpl.cpp
//
// Copyright (C) 2016 Microsoft Corp.  All Rights Reserved
//////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "RoomManagerImpl.h"

XTOOLS_NAMESPACE_BEGIN

RoomManagerImpl::RoomManagerImpl(const ClientContextConstPtr& context)
	: m_context(context)
{
	// Create the sync element representing this object.  
	// NOTE: the room manager should be constructed before a connection to another device is made, so there should never 
	// be another object with the same name already in the sync system
	m_element = m_context->GetInternalSyncManager()->GetRootObject()->CreateObjectElement(new XString("RoomMgr"));
	XTASSERT(m_element);

	m_element->AddListener(this);
}


void RoomManagerImpl::AddListener(RoomManagerListener* newListener)
{
	m_listenerList->AddListener(newListener);
}

 
void RoomManagerImpl::RemoveListener(RoomManagerListener* oldListener)
{
	m_listenerList->RemoveListener(oldListener);
}


int32 RoomManagerImpl::GetRoomCount() const
{
	return (int32)m_roomList.size();
}


RoomPtr RoomManagerImpl::GetRoom(int32 index)
{
	if (index >= 0 && index < GetRoomCount())
	{
		return m_roomList[index];
	}
	else
	{
		LogError("Tried to access room at invalid index %i", index);
		return nullptr;
	}
}


RoomPtr RoomManagerImpl::GetCurrentRoom()
{
	return m_currentRoom;
}


RoomPtr RoomManagerImpl::CreateRoom(const XStringPtr& roomName, RoomID roomID)
{
	// If the user is currently in a room, leave it first
	LeaveRoom();

	// Create a Room object for this element
	RoomImplPtr newRoom = new RoomImpl(m_listenerList, roomName, roomID);

	if (newRoom->BindLocal(m_element, roomName->GetString(), nullptr))
	{
		// Add it to the list of rooms
		m_roomList.push_back(newRoom);

		// Notify listeners that the new room was added
		m_listenerList->NotifyListeners(&RoomManagerListener::OnRoomAdded, newRoom);

		// Set the newly created room as the current room
		m_currentRoom = newRoom;

		// Add the local user to the room
		UserID userID = m_context->GetLocalUser()->GetID();
		newRoom->GetUserArray().Insert(0, userID);

		// Notify listeners that the we've joined the room
		m_listenerList->NotifyListeners(&RoomManagerListener::OnUserJoinedRoom, newRoom, userID);

		return newRoom;
	}
	else
	{
		return nullptr;
	}
}


bool RoomManagerImpl::JoinRoom(const RoomPtr& room)
{
	// Check that we aren't already in the room
	if (room == m_currentRoom)
	{
		LogWarning("Trying to join a room that you are already in");
		return false;
	}

	// Leave the current room
	LeaveRoom();

	// Validate that the room passed in is in the list of rooms we know about
	XTASSERT(m_currentRoom == nullptr);
	for (size_t i = 0; i < m_roomList.size(); ++i)
	{
		if (m_roomList[i] == room)
		{
			// Set the new room as the current room
			m_currentRoom = m_roomList[i];
			break;
		}
	}

	if (m_currentRoom)
	{
		// Add the local user to the room
		UserID userID = m_context->GetLocalUser()->GetID();
		m_currentRoom->GetUserArray().Insert(m_currentRoom->GetUserCount(), userID);

		// Notify listeners that we've joined the room
		m_listenerList->NotifyListeners(&RoomManagerListener::OnUserJoinedRoom, m_currentRoom, userID);

		return true;
	}
	else
	{
		LogError("Attempting to join an invalid room");
		return false;
	}
}


bool RoomManagerImpl::LeaveRoom()
{
	if (m_currentRoom != nullptr)
	{
		const UserID localUserID = m_context->GetLocalUser()->GetID();

		const int32 userCount = m_currentRoom->GetUserCount();
		for (int32 i = 0; i < userCount; ++i)
		{
			if (m_currentRoom->GetUserID(i) == localUserID)
			{
				m_currentRoom->GetUserArray().Remove(i);

				m_listenerList->NotifyListeners(&RoomManagerListener::OnUserLeftRoom, m_currentRoom, localUserID);

				break;
			}
		}

		m_currentRoom = nullptr;
		return true;
	}
	else
	{
		return false;
	}
}


int32 RoomManagerImpl::GetAnchorCount(const RoomPtr& room)
{
	XT_UNREFERENCED_PARAM(room);
	return 0;
}


XStringPtr RoomManagerImpl::GetAnchorName(const RoomPtr& room, int32 anchorIndex)
{
	XT_UNREFERENCED_PARAM(room);
	XT_UNREFERENCED_PARAM(anchorIndex);
	return nullptr;
}


AnchorDownloadRequestPtr RoomManagerImpl::DownloadAnchor(const RoomPtr& room, const XStringPtr& anchorName)
{
	XT_UNREFERENCED_PARAM(room);
	XT_UNREFERENCED_PARAM(anchorName);
	return nullptr;
}

 
bool RoomManagerImpl::UploadAnchor(const RoomPtr& room, const XStringPtr& anchorName, const byte* data, int32 dataSize)
{
	XT_UNREFERENCED_PARAM(room);
	XT_UNREFERENCED_PARAM(anchorName);
	XT_UNREFERENCED_PARAM(data);
	XT_UNREFERENCED_PARAM(dataSize);
	return false;
}


void RoomManagerImpl::OnElementAdded(const ElementPtr& element)
{
	// Create a Room object for this element
	RoomImplPtr newRoom = new RoomImpl(m_listenerList);
	newRoom->BindRemote(element);

	// Add it to the list of rooms
	m_roomList.push_back(newRoom);

	// Notify the listeners about the new room
	m_listenerList->NotifyListeners(&RoomManagerListener::OnRoomAdded, newRoom);
}


void RoomManagerImpl::OnElementDeleted(const ElementPtr& element)
{
	// Find the room that was deleted
	RoomImplPtr closedRoom = nullptr;

	for (size_t i = 0; i < m_roomList.size(); ++i)
	{
		if (m_roomList[i]->GetGUID() == element->GetGUID())
		{
			if (m_currentRoom != nullptr &&
				m_currentRoom->GetID() == m_roomList[i]->GetID())
			{
				m_currentRoom = nullptr;
			}

			closedRoom = m_roomList[i];
			m_roomList.erase(m_roomList.begin() + i);
			break;
		}
	}

	if (closedRoom != nullptr)
	{
		m_listenerList->NotifyListeners(&RoomManagerListener::OnRoomClosed, closedRoom);
	}
}

XTOOLS_NAMESPACE_END
