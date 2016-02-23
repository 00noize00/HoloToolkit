//////////////////////////////////////////////////////////////////////////
// Syncable.h
//
// Copyright (C) 2014 Microsoft Corp.  All Rights Reserved
//////////////////////////////////////////////////////////////////////////

#pragma once

XTOOLS_NAMESPACE_BEGIN

// Base class for types that can by synchronized with the XTools sync system
class Syncable XTABSTRACT : public Reflection::XTObject
{
	XTOOLS_REFLECTION_DECLARE(Syncable)
		
public:
	virtual XGuid GetGUID() const { return kInvalidXGuid; }

	virtual ElementType GetType() const { return ElementType::UnknownType; }

protected:
	Syncable() {}

	// Create a new element for this instance in the sync system
	virtual void BindLocal(const ObjectElementPtr& parent, const std::string& name, const UserPtr& owner) = 0;

	// Bind this instance to an element that already exists in the sync system
	virtual void BindRemote(const ElementPtr& element) = 0;

	// Set the value of this instance in response to the value being changed remotely
	virtual void SetValue(int ) {}
	virtual void SetValue(float ) {}
	virtual void SetValue(std::string ) {}
	void SetValue(const XStringPtr& s) { SetValue(s->GetString()); }

	friend class SyncObject;
};

XTOOLS_NAMESPACE_END
