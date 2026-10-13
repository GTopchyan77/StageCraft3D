// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Fragment/DlFmObjectSelector.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"

void UDlFmObjectSelector::HandleOpening()
{
	Cache_OptionString.Empty(Cache_OptionString.Num());
	switch (Range)
	{
	case EDlFmObjectSelector_Range::AllObject:
		{
			for (TObjectIterator<UObject> It; It; ++It)
			{
				if (It->IsA(FilterClass))
				{
					Cache_OptionString.Emplace(GetShowNameFromObj(*It));
				}
			}
		}
		break;
	case EDlFmObjectSelector_Range::ActorInWorld:
		{
			for (TActorIterator<AActor> It(GetWorld()); It; ++It)
			{
				if (It->IsA(FilterClass))
				{
					Cache_OptionString.Emplace(GetShowNameFromObj(*It));
				}
			}
		}
		break;
	case EDlFmObjectSelector_Range::ObjectInWorld:
		{
			const UWorld* TmpWorld = GetWorld();
			for (TObjectIterator<UObject> It; It; ++It)
			{
				if (It->IsA(FilterClass))
				{
					if (It->GetWorld() == TmpWorld)
					{
						Cache_OptionString.Emplace(GetShowNameFromObj(*It));
					}
				}
			}
		}
		break;
	default:
		break;
	}

	//Do Reverse sort, place None to first;
	if(bSortOptions)
	{
		Algo::Sort (Cache_OptionString,[](const FString& T1,const FString& T2)
		{
			return T1>T2;
		});
	}
	Cache_OptionString.Emplace(GetShowNameFromObj(nullptr));
	
	ClearOptions();
	for (int32 Index=Cache_OptionString.Num()-1;Index>=0;Index--)
	{
		Options.Add(MakeShareable(new FString(Cache_OptionString[Index])));
	}
	RefreshOptions();

	Super::HandleOpening();
}

void UDlFmObjectSelector::HandleSelectionChanged(TSharedPtr<FString> Item, ESelectInfo::Type SelectionType)
{
	if(bDlFmAction_HandleSelectionChanged)
	{
		if (Item.IsValid())
		{
			Cache_SelectedObject = nullptr;
			switch (Range)
			{
			case EDlFmObjectSelector_Range::AllObject:
				{
					for (TObjectIterator<UObject> It; It; ++It)
					{
						if (It->IsA(FilterClass))
						{							
							if (Item->Equals(GetShowNameFromObj(*It)))
							{
								SetSelectedObject_Pri(*It, SelectionType);
								break;
							}
						}
					}
				}
				break;
			case EDlFmObjectSelector_Range::ActorInWorld:
				{
					for (TActorIterator<AActor> It(GetWorld()); It; ++It)
					{
						if (It->IsA(FilterClass))
						{
							if (Item->Equals(GetShowNameFromObj(*It)))
							{
								SetSelectedObject_Pri(*It, SelectionType);
								break;
							}
							
						}
					}
				}
				break;
			case EDlFmObjectSelector_Range::ObjectInWorld:
				{
					const UWorld* TmpWorld = GetWorld();
					for (TObjectIterator<UObject> It; It; ++It)
					{
						if (It->IsA(FilterClass))
						{
							if (It->GetWorld() == TmpWorld)
							{
								if (Item->Equals(GetShowNameFromObj(*It)))
								{
									SetSelectedObject_Pri(*It, SelectionType);
									break;
								}
							}
						}
					}
				}
				break;
			default:
				break;
			}
		}
	}

	Super::HandleSelectionChanged(Item, SelectionType);
}

void UDlFmObjectSelector::Refresh_SelectString()
{
	SetOrCreateSelectedOption(GetShowNameFromObj(Cache_SelectedObject));
}

void UDlFmObjectSelector::SetSelectedObject(UObject* Val, bool bBroadcast)
{
	if (Cache_SelectedObject != Val)
	{
		bDlFmAction_HandleSelectionChanged = false;
		
		Cache_SelectedObject = Val;
		Refresh_SelectString();
		
		if(bBroadcast)
		{
			OnSelectedObjectChangedEvent.Broadcast(Cache_SelectedObject, ESelectInfo::Type::Direct);
		}
		
		bDlFmAction_HandleSelectionChanged = true;
	}
}

FString UDlFmObjectSelector::GetShowNameFromObj(UObject* Val) const
{
	if(IsValid(Val))
	{
		switch (NameType)
		{
		case EDlFmObjectSelector_NameType::ActorLabel:
			{
				const AActor* TmpA=Cast<AActor>(Val);
				if(TmpA!=nullptr)
				{
					return TmpA->GetActorNameOrLabel();
				}
				else
				{
					return Val->GetName();
				}
			}
		case EDlFmObjectSelector_NameType::ObjectName:
			return Val->GetName();
		case EDlFmObjectSelector_NameType::ObjectPathName:
			return Val->GetPathName();
		default:
			return Val->GetName();
		}
	}
	else
	{
		return TEXT("None");
	}
}

void UDlFmObjectSelector::SetSelectedObject_Pri(UObject* Val, ESelectInfo::Type SelectionType)
{
		Cache_SelectedObject = Val;
		OnSelectedObjectChangedEvent.Broadcast(Cache_SelectedObject, SelectionType);
	
}

void UDlFmObjectSelector::SetOrCreateSelectedOption(const FString& Option)
{
	const int32 InitialIndex = FindOptionIndex(Option);
	if(InitialIndex==INDEX_NONE)
	{
		Options.Add(MakeShareable(new FString(Option)));
	}
	SetSelectedOption(Option);
}
