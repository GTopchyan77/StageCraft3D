// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for where an acquired stage may go (Docs/ADR/0022-dcc-bridge.md §11.2): the one rule the acquire dialog and the dispatcher
// share. Pure: no world, no purchases.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Dispatch; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Marketplace/StageDispatchRules.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace StageDispatchTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FString Str(EStageDispatchAvailability Availability)
	{
		return StaticEnum<EStageDispatchAvailability>()->GetNameStringByValue(static_cast<int64>(Availability));
	}

	FStageDispatchTarget Make(EStageDispatchKind Kind)
	{
		FStageDispatchTarget Target;
		Target.Kind = Kind;
		return Target;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDispatchRulesTest, "StageCraft.Dispatch.Rules", StageDispatchTests::Flags)

bool FStageDispatchRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageDispatchTests;

	FStageDispatchContext Context;
	Context.bBridgeRunning = true;
	FStageBridgeClientInfo& Blender = Context.Clients.AddDefaulted_GetRef();
	Blender.ClientId = 3;
	Blender.App = EStageBridgeApp::Blender;
	Blender.bImportsGlb = true;
	FStageBridgeClientInfo& Maya = Context.Clients.AddDefaulted_GetRef();
	Maya.ClientId = 4;
	Maya.App = EStageBridgeApp::Maya;
	Maya.bImportsGlb = false;
	FStageAIToolInfo& Tool = Context.Tools.AddDefaulted_GetRef();
	Tool.ProviderId = TEXT("mock");
	Tool.Capabilities = static_cast<uint8>(EStageAIToolCapability::AcceptModel);
	Tool.KeyState = EStageAIToolKeyState::Saved;
	FStageAIToolInfo& Generator = Context.Tools.AddDefaulted_GetRef();
	Generator.ProviderId = TEXT("textonly");
	Generator.Capabilities = static_cast<uint8>(EStageAIToolCapability::TextTo3D);
	Generator.KeyState = EStageAIToolKeyState::Saved;

	FStageDispatchTarget ToBlender = Make(EStageDispatchKind::BridgeApp);
	ToBlender.BridgeClientId = 3;
	FStageDispatchTarget ToMaya = Make(EStageDispatchKind::BridgeApp);
	ToMaya.BridgeClientId = 4;
	FStageDispatchTarget ToTool = Make(EStageDispatchKind::AITool);
	ToTool.ProviderId = TEXT("mock");
	const auto Eval = [&Context](const FStageDispatchTarget& Target) { return Str(StageDispatchRules::EvaluateTarget(Target, Context)); };

	// The publisher keeps the stage inside: only StageCraft itself.
	Context.bListingAllowsExport = false;
	TestEqual(TEXT("Open is always allowed"), Eval(Make(EStageDispatchKind::OpenInStageCraft)), Str(EStageDispatchAvailability::Available));
	TestEqual(TEXT("No file without rights"), Eval(Make(EStageDispatchKind::SaveFile)), Str(EStageDispatchAvailability::NeedsExportRights));
	TestEqual(TEXT("No app without rights"), Eval(ToBlender), Str(EStageDispatchAvailability::NeedsExportRights));
	TestEqual(TEXT("No AI tool without rights"), Eval(ToTool), Str(EStageDispatchAvailability::NeedsExportRights));

	Context.bListingAllowsExport = true;
	TestEqual(TEXT("File"), Eval(Make(EStageDispatchKind::SaveFile)), Str(EStageDispatchAvailability::Available));
	TestEqual(TEXT("Blender"), Eval(ToBlender), Str(EStageDispatchAvailability::Available));
	TestEqual(TEXT("Maya needs USD"), Eval(ToMaya), Str(EStageDispatchAvailability::NeedsUsd));
	FStageDispatchTarget Gone = ToBlender;
	Gone.BridgeClientId = 9;
	TestEqual(TEXT("A client that left"), Eval(Gone), Str(EStageDispatchAvailability::NotConnected));
	Context.bBridgeSending = true;
	TestEqual(TEXT("The bridge is sending something else"), Eval(ToBlender), Str(EStageDispatchAvailability::Busy));
	Context.bBridgeSending = false;
	Context.bBridgeRunning = false;
	TestEqual(TEXT("Bridge off: no app is connected"), Eval(ToBlender), Str(EStageDispatchAvailability::NotConnected));
	Context.bBridgeRunning = true;

	TestEqual(TEXT("AI tool with a saved key"), Eval(ToTool), Str(EStageDispatchAvailability::Available));
	Context.Tools[0].KeyState = EStageAIToolKeyState::Missing;
	TestEqual(TEXT("AI tool without a key"), Eval(ToTool), Str(EStageDispatchAvailability::NeedsKey));
	Context.Tools[0].KeyState = EStageAIToolKeyState::Rejected;
	TestEqual(TEXT("AI tool that refused the key"), Eval(ToTool), Str(EStageDispatchAvailability::KeyRejected));
	Context.Tools[0].KeyState = EStageAIToolKeyState::Verified;
	Context.bToolBusy = true;
	TestEqual(TEXT("AI tool busy"), Eval(ToTool), Str(EStageDispatchAvailability::Busy));
	Context.bToolBusy = false;
	FStageDispatchTarget ToGenerator = ToTool;
	ToGenerator.ProviderId = TEXT("textonly");
	TestEqual(TEXT("A generator that takes no models"), Eval(ToGenerator), Str(EStageDispatchAvailability::Unavailable));
	FStageDispatchTarget ToNobody = ToTool;
	ToNobody.ProviderId = TEXT("nobody");
	TestEqual(TEXT("An unknown tool"), Eval(ToNobody), Str(EStageDispatchAvailability::Unavailable));

	// File paths: absolute .glb only; other kinds need none.
	FStageDispatchTarget File = Make(EStageDispatchKind::SaveFile);
	TestFalse(TEXT("No path"), StageDispatchRules::HasValidPath(File));
	File.FilePath = TEXT("relative/out.glb");
	TestFalse(TEXT("Relative path"), StageDispatchRules::HasValidPath(File));
	File.FilePath = FPaths::ConvertRelativePathToFull(TEXT("out.exe"));
	TestFalse(TEXT("Wrong extension"), StageDispatchRules::HasValidPath(File));
	File.FilePath = FPaths::ConvertRelativePathToFull(TEXT("out.GLB"));
	TestTrue(TEXT("Absolute .glb"), StageDispatchRules::HasValidPath(File));
	TestTrue(TEXT("An app needs no path"), StageDispatchRules::HasValidPath(ToBlender));
	TestTrue(TEXT("Every refusal has a reason"), !StageDispatchRules::Describe(EStageDispatchAvailability::NeedsExportRights).IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
