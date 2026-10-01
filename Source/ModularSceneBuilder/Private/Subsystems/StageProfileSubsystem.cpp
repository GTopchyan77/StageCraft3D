// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageProfileSubsystem.h"

#include "Economy/StageProfileSaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/SecureHash.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageProfileSubsystem)

namespace StageProfile
{
	constexpr int32 SaveUserIndex = 0;

	// Obfuscation only: a salt compiled into the client is readable by anyone with the binary.
	// It makes hand-editing the save pointless, nothing more (see the class comment).
	const TCHAR* IntegritySalt = TEXT("StageCraft.Profile.v1|7f3c1e9a");
}

void UStageProfileSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LoadOrCreateProfile();
}

void UStageProfileSubsystem::Deinitialize()
{
	// An async save still queued at shutdown would be lost; write the latest state synchronously.
	if (bPersistProfile && bSaveQueued)
	{
		UStageProfileSaveGame* Save = Cast<UStageProfileSaveGame>(UGameplayStatics::CreateSaveGameObject(UStageProfileSaveGame::StaticClass()));
		Save->Profile = Profile;
		Save->IntegrityHash = ComputeIntegrityHash();
		UGameplayStatics::SaveGameToSlot(Save, SaveSlotName, StageProfile::SaveUserIndex);
	}

	OnBalanceChanged.Clear();
	OnEntitlementsChanged.Clear();
	OnProfileLoaded.Clear();

	Super::Deinitialize();
}

int64 UStageProfileSubsystem::GetBalance(FGameplayTag Currency) const
{
	const int64* Balance = Profile.Balances.Find(Currency);
	return Balance ? *Balance : 0;
}

bool UStageProfileSubsystem::HasEntitlement(FGameplayTag Entitlement) const
{
	return Entitlement.IsValid() && Profile.Entitlements.HasTagExact(Entitlement);
}

void UStageProfileSubsystem::SetDisplayName(const FString& NewName)
{
	Profile.DisplayName = NewName.Left(64).TrimStartAndEnd();
	RequestSave();
}

void UStageProfileSubsystem::LoadOrCreateProfile()
{
	bIntegrityValid = true;

	UStageProfileSaveGame* Save = nullptr;
	if (bPersistProfile && UGameplayStatics::DoesSaveGameExist(SaveSlotName, StageProfile::SaveUserIndex))
	{
		Save = Cast<UStageProfileSaveGame>(UGameplayStatics::LoadGameFromSlot(SaveSlotName, StageProfile::SaveUserIndex));
	}

	if (!Save || Save->Version != UStageProfileSaveGame::CurrentVersion)
	{
		if (Save)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Profile save '%s' has version %d (expected %d); starting a new profile."),
				*SaveSlotName, Save->Version, UStageProfileSaveGame::CurrentVersion);
		}
		ResetToNewProfile();
		return;
	}

	Profile = Save->Profile;
	bIntegrityValid = Save->IntegrityHash == ComputeIntegrityHash();
	if (!bIntegrityValid)
	{
		// Not destructive: the player keeps what they had, they just cannot spend until a reset.
		UE_LOG(LogStageCraft, Warning, TEXT("Profile save '%s' failed its integrity check; purchases are disabled."), *SaveSlotName);
	}

	UE_LOG(LogStageCraft, Log, TEXT("Profile loaded from '%s': %d currencies, %d entitlements, %d purchases."),
		*SaveSlotName, Profile.Balances.Num(), Profile.Entitlements.Num(), Profile.PurchaseHistory.Num());
	OnProfileLoaded.Broadcast();
}

void UStageProfileSubsystem::ResetToNewProfile()
{
	Profile = FStagePlayerProfile();
	for (const FStageCurrencyAmount& Start : StartingBalances)
	{
		if (Start.Currency.MatchesTag(StageCraftTags::Currency) && Start.Amount > 0)
		{
			Profile.Balances.FindOrAdd(Start.Currency) += Start.Amount;
		}
	}
	bIntegrityValid = true;

	UE_LOG(LogStageCraft, Log, TEXT("New profile created (slot '%s')."), *SaveSlotName);
	RequestSave();
	OnProfileLoaded.Broadcast();
	OnEntitlementsChanged.Broadcast();
	for (const TPair<FGameplayTag, int64>& Balance : Profile.Balances)
	{
		OnBalanceChanged.Broadcast(Balance.Key, Balance.Value);
	}
}

bool UStageProfileSubsystem::CommitPurchase(const FStagePurchaseRecord& Record)
{
	// Check every debit first so a purchase is never half-applied.
	for (const FStageCurrencyAmount& Paid : Record.AmountPaid)
	{
		if (Paid.Amount < 0 || GetBalance(Paid.Currency) < Paid.Amount)
		{
			return false;
		}
	}

	for (const FStageCurrencyAmount& Paid : Record.AmountPaid)
	{
		Profile.Balances.FindOrAdd(Paid.Currency) -= Paid.Amount;
	}
	Profile.Entitlements.AppendTags(Record.EntitlementsGranted);
	Profile.PurchaseHistory.Add(Record);

	RequestSave();

	for (const FStageCurrencyAmount& Paid : Record.AmountPaid)
	{
		OnBalanceChanged.Broadcast(Paid.Currency, GetBalance(Paid.Currency));
	}
	OnEntitlementsChanged.Broadcast();
	return true;
}

bool UStageProfileSubsystem::AdjustBalance(const FGameplayTag& Currency, int64 Delta)
{
	if (!Currency.MatchesTag(StageCraftTags::Currency))
	{
		return false;
	}

	const int64 Current = GetBalance(Currency);
	// Overflow-safe: an int64 wallet should never wrap into a negative (or huge) balance.
	if ((Delta > 0 && Current > MAX_int64 - Delta) || Current + Delta < 0)
	{
		return false;
	}

	Profile.Balances.FindOrAdd(Currency) = Current + Delta;
	RequestSave();
	OnBalanceChanged.Broadcast(Currency, Current + Delta);
	return true;
}

FString UStageProfileSubsystem::ComputeIntegrityHash() const
{
	// Canonical text of every economy-relevant field, in a stable order (maps and containers sorted).
	TArray<FString> Parts;

	TArray<FGameplayTag> Currencies;
	Profile.Balances.GetKeys(Currencies);
	Currencies.Sort([](const FGameplayTag& A, const FGameplayTag& B) { return A.GetTagName().LexicalLess(B.GetTagName()); });
	for (const FGameplayTag& Currency : Currencies)
	{
		Parts.Add(FString::Printf(TEXT("B:%s=%lld"), *Currency.ToString(), Profile.Balances[Currency]));
	}

	TArray<FString> Entitlements;
	for (const FGameplayTag& Tag : Profile.Entitlements)
	{
		Entitlements.Add(Tag.ToString());
	}
	Entitlements.Sort();
	for (const FString& Tag : Entitlements)
	{
		Parts.Add(TEXT("E:") + Tag);
	}

	for (const FStagePurchaseRecord& Record : Profile.PurchaseHistory)
	{
		Parts.Add(TEXT("P:") + Record.TransactionId.ToString() + TEXT("/") + Record.ProductId.ToString());
	}
	Parts.Add(FString::Printf(TEXT("L:%d"), Profile.Level));
	Parts.Add(StageProfile::IntegritySalt);

	const FString Canonical = FString::Join(Parts, TEXT("|"));
	const FTCHARToUTF8 Utf8(*Canonical);
	return FSHA1::HashBuffer(Utf8.Get(), Utf8.Length()).ToString();
}

void UStageProfileSubsystem::RequestSave()
{
	if (!bPersistProfile)
	{
		return;
	}
	if (!bIntegrityValid)
	{
		// Re-hashing a tampered profile would launder it; keep the failing save as it is.
		return;
	}

	bSaveQueued = true;
	if (bSaveInFlight)
	{
		return; // HandleSaveFinished writes the newest state next.
	}

	UStageProfileSaveGame* Save = Cast<UStageProfileSaveGame>(UGameplayStatics::CreateSaveGameObject(UStageProfileSaveGame::StaticClass()));
	Save->Profile = Profile;
	Save->IntegrityHash = ComputeIntegrityHash();

	bSaveQueued = false;
	bSaveInFlight = true;
	UGameplayStatics::AsyncSaveGameToSlot(Save, SaveSlotName, StageProfile::SaveUserIndex,
		FAsyncSaveGameToSlotDelegate::CreateUObject(this, &ThisClass::HandleSaveFinished));
}

void UStageProfileSubsystem::HandleSaveFinished(const FString& SlotName, int32 UserIndex, bool bSuccess)
{
	bSaveInFlight = false;
	if (!bSuccess)
	{
		UE_LOG(LogStageCraft, Error, TEXT("Saving profile slot '%s' failed."), *SlotName);
	}
	if (bSaveQueued)
	{
		RequestSave();
	}
}

void UStageProfileSubsystem::DevResetProfile()
{
#if !UE_BUILD_SHIPPING
	ResetToNewProfile();
#endif
}

void UStageProfileSubsystem::DevReloadProfile()
{
#if !UE_BUILD_SHIPPING
	LoadOrCreateProfile();
	OnEntitlementsChanged.Broadcast();
	for (const TPair<FGameplayTag, int64>& Balance : Profile.Balances)
	{
		OnBalanceChanged.Broadcast(Balance.Key, Balance.Value);
	}
#endif
}
