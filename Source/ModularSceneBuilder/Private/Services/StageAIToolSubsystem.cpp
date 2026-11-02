// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/StageAIToolSubsystem.h"

#include "ModularSceneBuilder.h"
#include "Services/StageAIToolProvider.h"
#include "Services/StageCredentialVault.h"

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
}

void UStageAIToolSubsystem::Deinitialize()
{
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
	Vault.Reset();
	Super::Deinitialize();
}

void UStageAIToolSubsystem::InitializeForTests(const FString& VaultDirectory)
{
#if WITH_DEV_AUTOMATION_TESTS
	Vault = MakeUnique<FStageCredentialVault>(VaultDirectory);
#endif
}

void UStageAIToolSubsystem::RegisterProviderForDevelopment(TSubclassOf<UStageAIToolProvider> Class)
{
#if !UE_BUILD_SHIPPING
	if (AddProvider(Class))
	{
		OnToolsChanged.Broadcast();
	}
#endif
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
	if (!Vault || !Vault->Contains(Id))
	{
		return EStageAIToolKeyState::Missing;
	}
	const EStageAIToolKeyState* Checked = CheckedKeys.Find(Id);
	return Checked ? *Checked : EStageAIToolKeyState::Saved;
}

TArray<FStageAIToolInfo> UStageAIToolSubsystem::GetTools() const
{
	TArray<FStageAIToolInfo> Tools;
	Tools.Reserve(Providers.Num());
	for (const UStageAIToolProvider* Provider : Providers)
	{
		if (Provider)
		{
			FStageAIToolInfo& Info = Tools.AddDefaulted_GetRef();
			Info.ProviderId = Provider->GetProviderId();
			Info.DisplayName = Provider->GetDisplayName();
			Info.Capabilities = static_cast<uint8>(Provider->GetCapabilities());
			Info.KeyState = GetKeyState(*Provider);
		}
	}
	return Tools;
}

bool UStageAIToolSubsystem::FindTool(FName ProviderId, FStageAIToolInfo& OutInfo) const
{
	for (const FStageAIToolInfo& Info : GetTools())
	{
		if (Info.ProviderId == ProviderId)
		{
			OutInfo = Info;
			return true;
		}
	}
	return false;
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
	return TDelegate<void(const FStageAIToolResultInfo&)>::CreateWeakLambda(this, [this, Serial](const FStageAIToolResultInfo& Result)
	{
		HandleAnswer(Result, Serial);
	});
}

void UStageAIToolSubsystem::HandleAnswer(const FStageAIToolResultInfo& Answer, uint32 ForSerial)
{
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
