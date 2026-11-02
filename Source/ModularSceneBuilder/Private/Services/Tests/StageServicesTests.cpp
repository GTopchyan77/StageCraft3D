// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the credential vault and the AI tool seam (Docs/ADR/0023-ai-tool-providers-and-credential-vault.md). Every test uses a
// temporary vault folder, never the user's own keys.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Services; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Services/StageAIToolSubsystem.h"
#include "Services/StageCredentialVault.h"
#include "Services/StageMockAIToolProvider.h"
#include "UObject/Package.h"

namespace StageServicesTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FString MakeTempDirectory()
	{
		return FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageServicesTests"), FGuid::NewGuid().ToString());
	}

	FString Str(EStageVaultResult Result)
	{
		switch (Result)
		{
		case EStageVaultResult::Success: return TEXT("Success");
		case EStageVaultResult::NotFound: return TEXT("NotFound");
		case EStageVaultResult::InvalidRequest: return TEXT("InvalidRequest");
		case EStageVaultResult::Unavailable: return TEXT("Unavailable");
		case EStageVaultResult::Failed: return TEXT("Failed");
		}
		return TEXT("?");
	}

	FString Str(EStageAIToolResult Result) { return StaticEnum<EStageAIToolResult>()->GetNameStringByValue(static_cast<int64>(Result)); }
	FString Str(EStageAIToolKeyState State) { return StaticEnum<EStageAIToolKeyState>()->GetNameStringByValue(static_cast<int64>(State)); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCredentialVaultTest, "StageCraft.Services.CredentialVault", StageServicesTests::Flags)

bool FStageCredentialVaultTest::RunTest(const FString& Parameters)
{
	using namespace StageServicesTests;

	const FString Directory = MakeTempDirectory();
	const FStageCredentialVault Vault(Directory);
	const FName Id(TEXT("meshy"));
	const FString Secret = TEXT("msy_TEST_9f2c4d7e1a");

	TestFalse(TEXT("Nothing saved yet"), Vault.Contains(Id));
	FString Loaded = TEXT("stale");
	TestEqual(TEXT("Missing key"), Str(Vault.Load(Id, Loaded)), Str(EStageVaultResult::NotFound));
	TestTrue(TEXT("A failed load leaves nothing behind"), Loaded.IsEmpty());

	TestEqual(TEXT("Invalid id"), Str(Vault.Store(FName(TEXT("../evil")), Secret)), Str(EStageVaultResult::InvalidRequest));
	TestEqual(TEXT("Empty key"), Str(Vault.Store(Id, TEXT(""))), Str(EStageVaultResult::InvalidRequest));
	TestEqual(TEXT("Over-long key"), Str(Vault.Store(Id, FString::ChrN(FStageCredentialVault::MaxSecretLength + 1, TEXT('k')))), Str(EStageVaultResult::InvalidRequest));

	if (!FStageCredentialVault::IsSupported())
	{
		TestEqual(TEXT("No protected storage: nothing is written"), Str(Vault.Store(Id, Secret)), Str(EStageVaultResult::Unavailable));
		TestFalse(TEXT("... at all"), Vault.Contains(Id));
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		return true;
	}

	TestEqual(TEXT("Stored"), Str(Vault.Store(Id, Secret)), Str(EStageVaultResult::Success));
	TestTrue(TEXT("Contains"), Vault.Contains(Id));
	TestEqual(TEXT("Loaded"), Str(Vault.Load(Id, Loaded)), Str(EStageVaultResult::Success));
	TestEqual(TEXT("Round trip"), Loaded, Secret);
	FStageCredentialVault::WipeString(Loaded);
	TestTrue(TEXT("Wiped"), Loaded.IsEmpty());

	// The file on disk is not the key.
	const FString Path = FPaths::Combine(Directory, TEXT("meshy.key"));
	TArray<uint8> File;
	TestTrue(TEXT("File exists"), FFileHelper::LoadFileToArray(File, *Path));
	const FTCHARToUTF8 Plain(*Secret);
	bool bPlainFound = false;
	for (int32 Index = 0; Index + Plain.Length() <= File.Num() && !bPlainFound; ++Index)
	{
		bPlainFound = FMemory::Memcmp(File.GetData() + Index, Plain.Get(), Plain.Length()) == 0;
	}
	TestFalse(TEXT("The key is not stored in plain text"), bPlainFound);

	// A blob copied to another id does not decrypt: the id is part of the protection.
	const FName Other(TEXT("tripo"));
	TestTrue(TEXT("Copied"), IFileManager::Get().Copy(*FPaths::Combine(Directory, TEXT("tripo.key")), *Path) == COPY_OK);
	TestEqual(TEXT("Swapped file is refused"), Str(Vault.Load(Other, Loaded)), Str(EStageVaultResult::Failed));
	TestTrue(TEXT("... and yields nothing"), Loaded.IsEmpty());

	// Damage is refused, never read as a different key.
	TestTrue(TEXT("Damaged"), FFileHelper::SaveArrayToFile(TArray<uint8>{ 'S', 'C', 'K', 'V', 1, 0, 1, 2 }, *Path));
	TestEqual(TEXT("Damaged file is refused"), Str(Vault.Load(Id, Loaded)), Str(EStageVaultResult::Failed));

	TestEqual(TEXT("Removed"), Str(Vault.Remove(Id)), Str(EStageVaultResult::Success));
	TestFalse(TEXT("Gone"), Vault.Contains(Id));
	TestEqual(TEXT("Removing again is fine"), Str(Vault.Remove(Id)), Str(EStageVaultResult::Success));

	IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIToolMockTest, "StageCraft.Services.AIToolMock", StageServicesTests::Flags)

bool FStageAIToolMockTest::RunTest(const FString& Parameters)
{
	using namespace StageServicesTests;

	const FString Directory = MakeTempDirectory();
	// A bare game instance as the outer the subsystem class requires; it is never initialised, so no other subsystem runs.
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UStageAIToolSubsystem* Tools = NewObject<UStageAIToolSubsystem>(GameInstance);
	Tools->InitializeForTests(Directory);
	TestEqual(TEXT("No tools are configured by default"), Tools->GetTools().Num(), 0);

	Tools->RegisterProviderForDevelopment(UStageMockAIToolProvider::StaticClass());
	Tools->RegisterProviderForDevelopment(UStageMockAIToolProvider::StaticClass());
	TestEqual(TEXT("The mock is registered once"), Tools->GetTools().Num(), 1);

	const FName Id = UStageMockAIToolProvider::ProviderId;
	const auto KeyState = [Tools, Id]()
	{
		FStageAIToolInfo Info;
		return Tools->FindTool(Id, Info) ? Str(Info.KeyState) : FString(TEXT("NotFound"));
	};
	FStageAIToolInfo Info;
	TestTrue(TEXT("Found"), Tools->FindTool(Id, Info));
	TestTrue(TEXT("It takes models"), Info.Can(EStageAIToolCapability::AcceptModel));
	TestEqual(TEXT("No key yet"), KeyState(), Str(EStageAIToolKeyState::Missing));

	// A model to hand over.
	const FString Model = FPaths::ConvertRelativePathToFull(FPaths::Combine(Directory, TEXT("Test Stage.glb")));
	TestTrue(TEXT("Model written"), FFileHelper::SaveStringToFile(TEXT("glTF"), *Model));
	FStageAIToolJob Job;
	Job.ModelPath = Model;
	Job.Name = TEXT("Test Stage");

	TestEqual(TEXT("Unknown tool"), Str(Tools->SubmitModel(FName(TEXT("nope")), Job).Code), Str(EStageAIToolResult::NotFound));
	TestEqual(TEXT("No key: refused before the tool is asked"), Str(Tools->SubmitModel(Id, Job).Code), Str(EStageAIToolResult::NeedsKey));

	if (!Tools->IsKeyStorageAvailable())
	{
		TestEqual(TEXT("No protected storage"), Str(Tools->StoreKey(Id, TEXT("mock-1")).Code), Str(EStageAIToolResult::Unavailable));
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		return true;
	}

	TestEqual(TEXT("A wrong key is saved"), Str(Tools->StoreKey(Id, TEXT("  wrong  ")).Code), Str(EStageAIToolResult::Success));
	TestEqual(TEXT("... unchecked"), KeyState(), Str(EStageAIToolKeyState::Saved));
	TestEqual(TEXT("Check started"), Str(Tools->VerifyKey(Id).Code), Str(EStageAIToolResult::Success));
	TestFalse(TEXT("The synchronous answer ended the check"), Tools->IsBusy());
	TestEqual(TEXT("... as rejected"), KeyState(), Str(EStageAIToolKeyState::Rejected));
	TestEqual(TEXT("A rejected key is not sent again"), Str(Tools->SubmitModel(Id, Job).Code), Str(EStageAIToolResult::KeyRejected));

	TestEqual(TEXT("A new key"), Str(Tools->StoreKey(Id, TEXT("mock-ok")).Code), Str(EStageAIToolResult::Success));
	TestEqual(TEXT("... forgets the old check"), KeyState(), Str(EStageAIToolKeyState::Saved));
	Tools->VerifyKey(Id);
	TestEqual(TEXT("... and is accepted"), KeyState(), Str(EStageAIToolKeyState::Verified));

	const FString Received = FPaths::Combine(UStageMockAIToolProvider::GetReceivedDirectory(), FPaths::GetCleanFilename(Model));
	IFileManager::Get().Delete(*Received, false, true, true);
	TestEqual(TEXT("Hand-off started"), Str(Tools->SubmitModel(Id, Job).Code), Str(EStageAIToolResult::Success));
	TestFalse(TEXT("... and answered"), Tools->IsBusy());
	TestTrue(TEXT("The tool received the model"), IFileManager::Get().FileExists(*Received));
	TestTrue(TEXT("The caller's file is kept"), IFileManager::Get().FileExists(*Model));

	TestEqual(TEXT("Key removed"), Str(Tools->RemoveKey(Id).Code), Str(EStageAIToolResult::Success));
	TestEqual(TEXT("... missing again"), KeyState(), Str(EStageAIToolKeyState::Missing));
	Tools->CancelOperation();
	TestFalse(TEXT("Cancel with nothing running is harmless"), Tools->IsBusy());

	IFileManager::Get().Delete(*Received, false, true, true);
	IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
