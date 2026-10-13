// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "VersionSync/DlFmVersion.h"

bool FDlFmLiteVersion_Entity::IsInitialized() const
{
	return (CurVersion!=0);
}

void FDlFmLiteVersion_Entity::Update()
{
	CurVersion+=1;
}

FDlFmLiteVersion_Observer::FDlFmLiteVersion_Observer()
{
	CurVersion=-1;
}

bool FDlFmLiteVersion_Observer::IsInitialized() const
{
	return (CurVersion!=-1);
}

bool FDlFmLiteVersion_Observer::Observe_TriggerUpdate(const FDlFmLiteVersion_Entity& Entity)
{
	if(Entity.CurVersion!=CurVersion)
	{
		CurVersion=Entity.CurVersion;
		return true;
	}
	else
	{
		return false;
	}
}

bool FDlFmLiteVersion_Observer::Observe_Updated(const FDlFmLiteVersion_Entity& Entity) const
{
	if(Entity.CurVersion!=CurVersion)
	{
		return true;
	}
	else
	{
		return false;
	}
}

void FDlFmLiteVersion_Observer::Sync_AfterExternalUpdate(const FDlFmLiteVersion_Entity& Entity)
{
	CurVersion=Entity.CurVersion;
}
