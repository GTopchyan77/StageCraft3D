// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the DMX patch and the Art-Net / sACN input (Docs/ADR/0019). Patch and input changes go
// through AModularPlayerController (RequestPatchFixtures, RequestSetDMXInput), the Patch panel's path. SendTest sends real
// packets to this PC, for scripted runs without a console. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Actors/LightingFixtureActor.h"
#include "Common/UdpSocketBuilder.h"
#include "Components/SelectionComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "IPAddress.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Show/StageDMXNetProtocol.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Subsystems/StageDMXNetworkSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"

namespace StageDMXCommands
{
	AModularPlayerController* GetController(UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Controller)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX: no AModularPlayerController in this world."));
		}
		return Controller;
	}

	void LogStatus(const UStageDMXNetworkSubsystem& Network)
	{
		const FStageDMXInputStatus Status = Network.GetStatus();
		TArray<FString> Universes;
		for (const int32 Universe : Status.LiveUniverses)
		{
			Universes.Add(FString::FromInt(Universe));
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.Input: %s%s%s; Art-Net %lld, sACN %lld, invalid %lld, ignored %lld; live universes [%s] from %d sources."),
			*UEnum::GetValueAsString(Status.State), Status.Endpoint.IsEmpty() ? TEXT("") : TEXT(", "), *Status.Endpoint,
			Status.ArtNetPackets, Status.SACNPackets, Status.InvalidPackets, Status.IgnoredFrames, *FString::Join(Universes, TEXT(", ")), Status.LiveSources);
		if (!Status.Error.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.Input: %s"), *Status.Error.ToString());
		}
	}

	/** On | Off | Status */
	void Input(const TArray<FString>& Args, UWorld* World)
	{
		const UStageDMXNetworkSubsystem* Network = World ? World->GetSubsystem<UStageDMXNetworkSubsystem>() : nullptr;
		AModularPlayerController* Controller = GetController(World);
		if (!Network || !Controller)
		{
			return;
		}
		if (!Args.IsEmpty() && (Args[0] == TEXT("On") || Args[0] == TEXT("Off")))
		{
			const FStageEconomyResultInfo Result = Controller->RequestSetDMXInput(Args[0] == TEXT("On"));
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.Input %s: %s"), *Args[0], Result.IsSuccess() ? TEXT("done") : *Result.Message.ToString());
		}
		LogStatus(*Network);
	}

	/** Key=Value pairs: Bind=<IPv4 or empty>, ArtNetFirst=<port-address>, SACNFirst=<universe>, ArtNet|SACN|ArtPoll=On|Off. */
	void Settings(const TArray<FString>& Args, UWorld* World)
	{
		UStageDMXNetworkSubsystem* Network = World ? World->GetSubsystem<UStageDMXNetworkSubsystem>() : nullptr;
		if (!Network)
		{
			return;
		}

		FStageDMXInputSettings Settings = Network->GetSettings();
		for (const FString& Arg : Args)
		{
			FString Key;
			FString Value;
			if (!Arg.Split(TEXT("="), &Key, &Value))
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.Settings: \"%s\" is not Key=Value."), *Arg);
				return;
			}
			const bool bOn = Value == TEXT("On");
			if (Key == TEXT("Bind"))
			{
				Settings.BindAddress = Value;
			}
			else if (Key == TEXT("ArtNetFirst"))
			{
				LexTryParseString(Settings.ArtNetFirstPortAddress, *Value);
			}
			else if (Key == TEXT("SACNFirst"))
			{
				LexTryParseString(Settings.SACNFirstUniverse, *Value);
			}
			else if (Key == TEXT("ArtNet"))
			{
				Settings.bArtNet = bOn;
			}
			else if (Key == TEXT("SACN"))
			{
				Settings.bSACN = bOn;
			}
			else if (Key == TEXT("ArtPoll"))
			{
				Settings.bReplyToArtPoll = bOn;
			}
			else
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.Settings: unknown key \"%s\"."), *Key);
				return;
			}
		}

		if (!Args.IsEmpty())
		{
			FText Error;
			if (!Network->ApplySettings(Settings, Error))
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.Settings: %s"), *Error.ToString());
			}
		}
		const FStageDMXInputSettings Saved = Network->GetSettings();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.Settings: Bind=%s ArtNetFirst=%d SACNFirst=%d ArtNet=%s SACN=%s ArtPoll=%s, input %s."),
			Saved.BindAddress.IsEmpty() ? TEXT("(every card)") : *Saved.BindAddress, Saved.ArtNetFirstPortAddress, Saved.SACNFirstUniverse,
			Saved.bArtNet ? TEXT("On") : TEXT("Off"), Saved.bSACN ? TEXT("On") : TEXT("Off"), Saved.bReplyToArtPoll ? TEXT("On") : TEXT("Off"),
			Saved.bEnabled ? TEXT("on") : TEXT("off"));
	}

	void LogPlan(const TCHAR* Command, const FStageEconomyResultInfo& Result, int32 Count)
	{
		UE_LOG(LogStageCraft, Display, TEXT("%s: %s (%d fixtures planned)."), Command, Result.IsSuccess() ? TEXT("done") : *Result.Message.ToString(), Count);
	}

	void AutoPatch(const TArray<FString>& Args, UWorld* World)
	{
		const UStageDMXPatchSubsystem* Patching = World ? World->GetSubsystem<UStageDMXPatchSubsystem>() : nullptr;
		if (AModularPlayerController* Controller = Patching ? GetController(World) : nullptr)
		{
			const TArray<FStageDMXPatchAssignment> Plan = Patching->PlanUnpatched();
			LogPlan(TEXT("StageCraft.DMX.AutoPatch"), Controller->RequestPatchFixtures(Plan, FText::FromString(TEXT("nothing to patch or no room"))), Plan.Num());
		}
	}

	void FixConflicts(const TArray<FString>& Args, UWorld* World)
	{
		const UStageDMXPatchSubsystem* Patching = World ? World->GetSubsystem<UStageDMXPatchSubsystem>() : nullptr;
		if (AModularPlayerController* Controller = Patching ? GetController(World) : nullptr)
		{
			const TArray<FStageDMXPatchAssignment> Plan = Patching->PlanConflictRepair();
			LogPlan(TEXT("StageCraft.DMX.FixConflicts"), Controller->RequestPatchFixtures(Plan, FText::FromString(TEXT("no conflicts or no room"))), Plan.Num());
		}
	}

	/** <universe> <address>: the selected fixtures, one after another. */
	void PatchSelection(const TArray<FString>& Args, UWorld* World)
	{
		const UStageDMXPatchSubsystem* Patching = World ? World->GetSubsystem<UStageDMXPatchSubsystem>() : nullptr;
		AModularPlayerController* Controller = Patching ? GetController(World) : nullptr;
		FStageDMXPatch Start;
		if (!Controller || Args.Num() < 2 || !LexTryParseString(Start.Universe, *Args[0]) || !LexTryParseString(Start.Address, *Args[1]))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.PatchSelection <universe> <address>"));
			return;
		}
		TArray<ALightingFixtureActor*> Selected;
		for (AActor* Actor : Controller->GetSelection()->GetSelectedActors())
		{
			if (ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Actor))
			{
				Selected.Add(Fixture);
			}
		}
		const TArray<FStageDMXPatchAssignment> Plan = Patching->PlanConsecutive(Selected, Start);
		LogPlan(TEXT("StageCraft.DMX.PatchSelection"), Controller->RequestPatchFixtures(Plan, FText::FromString(TEXT("nothing selected or it does not fit"))), Plan.Num());
	}

	/** ArtNet|sACN <wire universe> <first channel> <value> [count]: one frame to 127.0.0.1 on the protocol's port. */
	void SendTest(const TArray<FString>& Args, UWorld* World)
	{
		int32 Universe = 0;
		int32 Channel = 0;
		int32 Value = 0;
		int32 Count = 1;
		const bool bArtNet = !Args.IsEmpty() && Args[0] == TEXT("ArtNet");
		const bool bSACN = !Args.IsEmpty() && Args[0] == TEXT("sACN");
		if ((!bArtNet && !bSACN) || Args.Num() < 4 || !LexTryParseString(Universe, *Args[1]) || !LexTryParseString(Channel, *Args[2])
			|| !LexTryParseString(Value, *Args[3]) || (Args.Num() >= 5 && !LexTryParseString(Count, *Args[4])))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.SendTest ArtNet|sACN <wire universe> <first channel> <value 0-255> [count]"));
			return;
		}

		TArray<uint8> Slots;
		Slots.SetNumZeroed(512);
		for (int32 Index = FMath::Clamp(Channel, 1, 512) - 1; Index < FMath::Min(Channel - 1 + FMath::Max(Count, 1), 512); ++Index)
		{
			Slots[Index] = static_cast<uint8>(FMath::Clamp(Value, 0, 255));
		}

		static uint8 Sequence = 0;
		Sequence = Sequence == 255 ? 1 : Sequence + 1;
		const TArray<uint8> Packet = bArtNet
			? StageDMXNet::BuildArtDmx(Universe, Sequence, Slots)
			: StageDMXNet::BuildSACNData(Universe, FGuid(0x53544147, 0x45435246, 0x54544553, 0x54000001), 100, Sequence, Slots, false, TEXT("StageCraft SendTest"));

		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		FSocket* Socket = FUdpSocketBuilder(TEXT("StageCraft DMX SendTest")).Build();
		if (!Sockets || !Socket)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.SendTest: no socket."));
			return;
		}
		const TSharedRef<FInternetAddr> Destination = Sockets->CreateInternetAddr();
		Destination->SetLoopbackAddress();
		Destination->SetPort(bArtNet ? StageDMXNet::ArtNetPort : StageDMXNet::SACNPort);
		int32 Sent = 0;
		const bool bSent = Socket->SendTo(Packet.GetData(), Packet.Num(), Sent, *Destination);
		Socket->Close();
		Sockets->DestroySocket(Socket);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.DMX.SendTest: %s universe %d, channels %d..%d = %d, %d bytes %s."), *Args[0], Universe, Channel,
			Channel + FMath::Max(Count, 1) - 1, Value, Sent, bSent ? TEXT("sent") : TEXT("NOT sent"));
	}

	FAutoConsoleCommandWithWorldAndArgs InputCommand(TEXT("StageCraft.DMX.Input"), TEXT("On | Off | Status: Art-Net / sACN input, like the Patch panel's DMX IN."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Input));
	FAutoConsoleCommandWithWorldAndArgs SettingsCommand(TEXT("StageCraft.DMX.Settings"), TEXT("[Bind=<IPv4>] [ArtNetFirst=<n>] [SACNFirst=<n>] [ArtNet|SACN|ArtPoll=On|Off]: logs or changes the input settings (saved on this PC; a running input restarts)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Settings));
	FAutoConsoleCommandWithWorldAndArgs AutoPatchCommand(TEXT("StageCraft.DMX.AutoPatch"), TEXT("Patches every unpatched fixture (validated, one undo step)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AutoPatch));
	FAutoConsoleCommandWithWorldAndArgs FixConflictsCommand(TEXT("StageCraft.DMX.FixConflicts"), TEXT("Moves overlapping fixtures to free addresses (validated, one undo step)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FixConflicts));
	FAutoConsoleCommandWithWorldAndArgs PatchSelectionCommand(TEXT("StageCraft.DMX.PatchSelection"), TEXT("<universe> <address>: patches the selected fixtures one after another."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PatchSelection));
	FAutoConsoleCommandWithWorldAndArgs SendTestCommand(TEXT("StageCraft.DMX.SendTest"), TEXT("ArtNet|sACN <wire universe> <first channel> <value> [count]: sends one DMX frame to this PC."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SendTest));
}

#endif // !UE_BUILD_SHIPPING
