// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/StageAIToolSubsystem.h"

#include "Engine/GameInstance.h"
#include "ModularSceneBuilder.h"
#include "Services/StageAIToolProvider.h"
#include "Services/StageCredentialVault.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAIToolSubsystem)

#define LOCTEXT_NAMESPACE "StageCraftAITools"

namespace StageAITools
{
	FStageAIToolResultInfo NotFound(FName ProviderId)
	{
		return FStageAIToolResultInfo::Fail(EStageAIToolResult::NotFound, ProviderId, LOCTEXT("NotFound", "That AI tool is not set up in this build."));
	}

	FStageAIToolResultInfo Busy(FName ProviderId)
	{
		return FStageAIToolResultInfo::Fail(EStageAIToolResult::Busy, ProviderId, LOCTEXT("Busy", "An AI tool is still working; try again when it has answered."));
	}

	FStageAIToolResultInfo FromVault(EStageVaultResult Result, FName ProviderId)
	{
		switch (Result)
		{
		case EStageVaultResult::Success:
			return FStageAIToolResultInfo::Ok(ProviderId);
		case EStageVaultResult::NotFound:
			return FStageAIToolResultInfo::Fail(EStageAIToolResult::NeedsKey, ProviderId, LOCTEXT("NeedsKey", "This AI tool needs an API key. Add it in Window > Services & Keys."));
		case EStageVaultResult::InvalidRequest:
			return FStageAIToolResultInfo::Fail(EStageAIToolResult::InvalidRequest, ProviderId,
				FText::Format(LOCTEXT("BadKey", "Enter a key of 1-{0} characters."), FStageCredentialVault::MaxSecretLength));
		case EStageVaultResult::Unavailable:
			return FStageAIToolResultInfo::Fail(EStageAIToolResult::Unavailable, ProviderId, LOCTEXT("NoVault", "Keys cannot be stored safely on this system, so none is saved."));
		case EStageVaultResult::Failed:
		default:
			return FStageAIToolResultInfo::Fail(EStageAIToolResult::Failed, ProviderId, LOCTEXT("VaultFailed", "The saved key could not be read or written. Save it again."));
		}
	}
}

UStageAIToolSubsystem::UStageAIToolSubsystem() = default;
UStageAIToolSubsystem::~UStageAIToolSubsystem() = default;

void UStageAIToolSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Vault = MakeUnique<FStageCredentialVault>(FStageCredentialVault::GetDefaultDirectory());

	for (const TSoftClassPtr<UStageAIToolProvider>& ClassPtr : ProviderClasses)
	{
		UClass* Class = ClassPtr.IsNull() ? nullptr : ClassPtr.LoadSynchronous();
		if (!AddProvider(Class))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("AI tools: ProviderClasses entry '%s' is not a concrete UStageAIToolProvider with a valid, unique id; skipped."), *ClassPtr.ToString());
		}
	}
	if (OperationTimeoutSeconds <= 0.f)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("AI tools: OperationTimeoutSeconds %.1f is not positive; using %.0f."), OperationTimeoutSeconds, GetOperationTimeoutSeconds());
	}
	RefreshSavedKeys();
}

void UStageAIToolSubsystem::Deinitialize()
{
	ClearOperationTimer();
	OnToolsChanged.Clear();
	OnKeyVerified.Clear();
	OnJobCompleted.Clear();
	for (UStageAIToolProvider* Provider : Providers)
	{
		if (Provider)
		{
			Provider->Cancel();
		}
	}
	Operation = EOperation::None;
	++OperationSerial;
	Providers.Reset();
	CheckedKeys.Reset();
	SavedKeys.Reset();
	Vault.Reset();
	Super::Deinitialize();
}

float UStageAIToolSubsystem::GetOperationTimeoutSeconds() const
{
	constexpr float DefaultTimeoutSeconds = 120.f;
	return OperationTimeoutSeconds > 0.f ? OperationTimeoutSeconds : DefaultTimeoutSeconds;
}

void UStageAIToolSubsystem::InitializeForTests(const FString& VaultDirectory)
{
#if WITH_DEV_AUTOMATION_TESTS
	Vault = MakeUnique<FStageCredentialVault>(VaultDirectory);
	CheckedKeys.Reset();
	RefreshSavedKeys();
#endif
}

void UStageAIToolSubsystem::RegisterProviderForDevelopment(TSubclassOf<UStageAIToolProvider> Class)
{
#if !UE_BUILD_SHIPPING
	if (AddProvider(Class))
	{
		RefreshSavedKeys();
		OnToolsChanged.Broadcast();
	}
#endif
}

void UStageAIToolSubsystem::RefreshSavedKeys()
{
	SavedKeys.Reset();
	if (!Vault)
	{
		return;
	}
	for (const UStageAIToolProvider* Provider : Providers)
	{
		if (Provider && Provider->RequiresApiKey() && Vault->Contains(Provider->GetProviderId()))
		{
			SavedKeys.Add(Provider->GetProviderId());
		}
	}
}

bool UStageAIToolSubsystem::AddProvider(TSubclassOf<UStageAIToolProvider> Class)
{
	if (!Class || Class->HasAnyClassFlags(CLASS_Abstract))
	{
		return false;
	}
	UStageAIToolProvider* Provider = NewObject<UStageAIToolProvider>(this, Class);
	const FName Id = Provider->GetProviderId();
	if (!FStageCredentialVault::IsValidId(Id) || FindProvider(Id))
	{
		return false;
	}
	Providers.Add(Provider);
	return true;
}

UStageAIToolProvider* UStageAIToolSubsystem::FindProvider(FName ProviderId) const
{
	const TObjectPtr<UStageAIToolProvider>* Found = Providers.FindByPredicate([ProviderId](const UStageAIToolProvider* Provider)
	{
		return Provider && Provider->GetProviderId() == ProviderId;
	});
	return Found ? Found->Get() : nullptr;
}

EStageAIToolKeyState UStageAIToolSubsystem::GetKeyState(const UStageAIToolProvider& Provider) const
{
	if (!Provider.RequiresApiKey())
	{
		return EStageAIToolKeyState::NotNeeded;
	}
	const FName Id = Provider.GetProviderId();
	if (!Vault || !SavedKeys.Contains(Id))
	{
		return EStageAIToolKeyState::Missing;
	}
	const EStageAIToolKeyState* Checked = CheckedKeys.Find(Id);
	return Checked ? *Checked : EStageAIToolKeyState::Saved;
}

FStageAIToolInfo UStageAIToolSubsystem::DescribeProvider(const UStageAIToolProvider& Provider) const
{
	FStageAIToolInfo Info;
	Info.ProviderId = Provider.GetProviderId();
	Info.DisplayName = Provider.GetDisplayName();
	Info.Capabilities = static_cast<uint8>(Provider.GetCapabilities());
	Info.KeyState = GetKeyState(Provider);
	return Info;
}

TArray<FStageAIToolInfo> UStageAIToolSubsystem::GetTools() const
{
	TArray<FStageAIToolInfo> Tools;
	Tools.Reserve(Providers.Num());
	for (const UStageAIToolProvider* Provider : Providers)
	{
		if (Provider)
		{
			Tools.Add(DescribeProvider(*Provider));
		}
	}
	return Tools;
}

bool UStageAIToolSubsystem::FindTool(FName ProviderId, FStageAIToolInfo& OutInfo) const
{
	const UStageAIToolProvider* Provider = FindProvider(ProviderId);
	if (!Provider)
	{
		return false;
	}
	OutInfo = DescribeProvider(*Provider);
	return true;
}

bool UStageAIToolSubsystem::IsKeyStorageAvailable() const
{
	return Vault.IsValid() && FStageCredentialVault::IsSupported();
}

FStageAIToolResultInfo UStageAIToolSubsystem::StoreKey(FName ProviderId, const FString& ApiKey)
{
	const UStageAIToolProvider* Provider = FindProvider(ProviderId);
	if (!Provider || !Vault)
	{
		return StageAITools::NotFound(ProviderId);
	}
	if (!Provider->RequiresApiKey())
	{
		return FStageAIToolResultInfo::Fail(EStageAIToolResult::InvalidRequest, ProviderId, LOCTEXT("NoKeyNeeded", "This AI tool does not use a key."));
	}

	FString Trimmed = ApiKey.TrimStartAndEnd();
	const EStageVaultResult Stored = Vault->Store(ProviderId, Trimmed);
	FStageCredentialVault::WipeString(Trimmed);
	if (Stored != EStageVaultResult::Success)
	{
		return StageAITools::FromVault(Stored, ProviderId);
	}
	// A new key has not been checked yet.
	CheckedKeys.Remove(ProviderId);
	SavedKeys.Add(ProviderId);
	UE_LOG(LogStageCraft, Log, TEXT("AI tools: key saved for '%s'."), *ProviderId.ToString());
	OnToolsChanged.Broadcast();
	return FStageAIToolResultInfo::Ok(ProviderId, LOCTEXT("KeySaved", "Key saved."));
}

FStageAIToolResultInfo UStageAIToolSubsystem::RemoveKey(FName ProviderId)
{
	if (!FindProvider(ProviderId) || !Vault)
	{
		return StageAITools::NotFound(ProviderId);
	}
	const EStageVaultResult Removed = Vault->Remove(ProviderId);
	if (Removed != EStageVaultResult::Success)
	{
		return StageAITools::FromVault(Removed, ProviderId);
	}
	CheckedKeys.Remove(ProviderId);
	SavedKeys.Remove(ProviderId);
	UE_LOG(LogStageCraft, Log, TEXT("AI tools: key removed for '%s'."), *ProviderId.ToString());
	OnToolsChanged.Broadcast();
	return FStageAIToolResultInfo::Ok(ProviderId, LOCTEXT("KeyRemoved", "Key removed."));
}

FStageAIToolResultInfo UStageAIToolSubsystem::ReadKey(const UStageAIToolProvider& Provider, FString& OutKey) const
{
	FStageCredentialVault::WipeString(OutKey);
	const FName Id = Provider.GetProviderId();
	if (!Provider.RequiresApiKey())
	{
		return FStageAIToolResultInfo::Ok(Id);
	}
	if (!Vault)
	{
		return StageAITools::FromVault(EStageVaultResult::Unavailable, Id);
	}
	return StageAITools::FromVault(Vault->Load(Id, OutKey), Id);
}

FStageAIToolResultInfo UStageAIToolSubsystem::VerifyKey(FName ProviderId)
{
	UStageAIToolProvider* Provider = FindProvider(ProviderId);
	if (!Provider)
	{
		return StageAITools::NotFound(ProviderId);
	}
	if (IsBusy())
	{
		return StageAITools::Busy(ProviderId);
	}
	FString Key;
	const FStageAIToolResultInfo Read = ReadKey(*Provider, Key);
	if (!Read.IsSuccess())
	{
		return Read;
	}
	Provider->VerifyKey(Key, BeginOperation(EOperation::Verify, ProviderId));
	FStageCredentialVault::WipeString(Key);
	return FStageAIToolResultInfo::Ok(ProviderId);
}

FStageAIToolResultInfo UStageAIToolSubsystem::SubmitModel(FName ProviderId, const FStageAIToolJob& Job)
{
	UStageAIToolProvider* Provider = FindProvider(ProviderId);
	if (!Provider)
	{
		return StageAITools::NotFound(ProviderId);
	}
	if (!EnumHasAnyFlags(Provider->GetCapabilities(), EStageAIToolCapability::AcceptModel))
	{
		return FStageAIToolResultInfo::Fail(EStageAIToolResult::InvalidRequest, ProviderId, LOCTEXT("NoAcceptModel", "This AI tool does not take finished models."));
	}
	if (IsBusy())
	{
		return StageAITools::Busy(ProviderId);
	}
	if (GetKeyState(*Provider) == EStageAIToolKeyState::Rejected)
	{
		return FStageAIToolResultInfo::Fail(EStageAIToolResult::KeyRejected, ProviderId, LOCTEXT("Rejected", "This AI tool refused the saved key. Save a new one in Window > Services & Keys."));
	}
	FString Key;
	const FStageAIToolResultInfo Read = ReadKey(*Provider, Key);
	if (!Read.IsSuccess())
	{
		return Read;
	}
	Provider->SubmitModel(Job, Key, BeginOperation(EOperation::Submit, ProviderId));
	FStageCredentialVault::WipeString(Key);
	return FStageAIToolResultInfo::Ok(ProviderId);
}

void UStageAIToolSubsystem::CancelOperation()
{
	if (!IsBusy())
	{
		return;
	}
	if (UStageAIToolProvider* Provider = FindProvider(OperationProvider))
	{
		Provider->Cancel();
	}
	// The provider may have answered inside Cancel; only a still-open operation is closed here.
	if (IsBusy())
	{
		HandleAnswer(FStageAIToolResultInfo::Fail(EStageAIToolResult::Cancelled, OperationProvider, LOCTEXT("Cancelled", "Cancelled.")), OperationSerial);
	}
}

TDelegate<void(const FStageAIToolResultInfo&)> UStageAIToolSubsystem::BeginOperation(EOperation InOperation, FName ProviderId)
{
	// Set before the provider is called: a provider may answer synchronously.
	Operation = InOperation;
	OperationProvider = ProviderId;
	const uint32 Serial = ++OperationSerial;
	ClearOperationTimer();
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().SetTimer(OperationTimer, FTimerDelegate::CreateUObject(this, &ThisClass::HandleOperationTimeout, Serial),
			GetOperationTimeoutSeconds(), /*bLoop*/ false);
	}
	return TDelegate<void(const FStageAIToolResultInfo&)>::CreateWeakLambda(this, [this, Serial](const FStageAIToolResultInfo& Result)
	{
		HandleAnswer(Result, Serial);
	});
}

void UStageAIToolSubsystem::ClearOperationTimer()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(OperationTimer);
	}
	OperationTimer.Invalidate();
}

void UStageAIToolSubsystem::HandleOperationTimeout(uint32 ForSerial)
{
	if (!IsBusy() || ForSerial != OperationSerial)
	{
		return;
	}
	UE_LOG(LogStageCraft, Warning, TEXT("AI tools: '%s' did not answer within %.0f s; cancelling."), *OperationProvider.ToString(), GetOperationTimeoutSeconds());
	if (UStageAIToolProvider* Provider = FindProvider(OperationProvider))
	{
		Provider->Cancel();
	}
	// The provider may have answered inside Cancel; only a still-open operation is closed here.
	if (IsBusy() && ForSerial == OperationSerial)
	{
		HandleAnswer(FStageAIToolResultInfo::Fail(EStageAIToolResult::TimedOut, OperationProvider,
			FText::Format(LOCTEXT("TimedOut", "The AI tool did not answer within {0} seconds."), FText::AsNumber(FMath::RoundToInt(GetOperationTimeoutSeconds())))), ForSerial);
	}
}

void UStageAIToolSubsystem::HandleAnswer(const FStageAIToolResultInfo& Answer, uint32 ForSerial)
{
	// The provider contract (StageAIToolProvider.h) is game-thread answers; state here is not guarded for any other thread.
	if (!ensureMsgf(IsInGameThread(), TEXT("AI tool '%s' answered off the game thread; the answer is ignored."), *Answer.ProviderId.ToString()))
	{
		return;
	}
	if (!IsBusy() || ForSerial != OperationSerial)
	{
		UE_LOG(LogStageCraft, Verbose, TEXT("AI tools: ignored an answer from '%s' that is no longer awaited."), *Answer.ProviderId.ToString());
		return;
	}

	const EOperation Finished = Operation;
	FStageAIToolResultInfo Result = Answer;
	// The answer speaks for the provider that was asked, whatever it says about itself.
	Result.ProviderId = OperationProvider;
	Operation = EOperation::None;
	OperationProvider = NAME_None;
	ClearOperationTimer();

	if (Result.Code == EStageAIToolResult::KeyRejected || (Finished == EOperation::Verify && Result.IsSuccess()))
	{
		CheckedKeys.Add(Result.ProviderId, Result.IsSuccess() ? EStageAIToolKeyState::Verified : EStageAIToolKeyState::Rejected);
		OnToolsChanged.Broadcast();
	}

	UE_LOG(LogStageCraft, Log, TEXT("AI tools: %s for '%s' ended: %s."), Finished == EOperation::Verify ? TEXT("key check") : TEXT("model hand-off"),
		*Result.ProviderId.ToString(), *StaticEnum<EStageAIToolResult>()->GetNameStringByValue(static_cast<int64>(Result.Code)));
	(Finished == EOperation::Verify ? OnKeyVerified : OnJobCompleted).Broadcast(Result);
}

#undef LOCTEXT_NAMESPACE
