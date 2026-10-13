// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for spans (Docs/ADR/0005 §3, Docs/ADR/0007 §4.3). Paths are drawn through the real click path
// with StageCraft.Edit.Arm / PointerAt / Click; these commands stand in for the Enter and Backspace keys, report the tool, and
// edit a span's length or sag through the same validated request the inspector uses. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Actors/StageSpanActor.h"
#include "Components/SelectionComponent.h"
#include "Data/StageParameterTypes.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Placement/StageSpanPreview.h"
#include "Placement/StageSpanToolComponent.h"
#include "Player/ModularPlayerController.h"

namespace StageSpanCommands
{
	AModularPlayerController* GetController(UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Controller)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Span: no AModularPlayerController in this world."));
		}
		return Controller;
	}

	AStageSpanActor* GetSelectedSpan(const AModularPlayerController* Controller)
	{
		return Controller ? Cast<AStageSpanActor>(Controller->GetSelection()->GetSelectedActor()) : nullptr;
	}

	void LogSpan(const TCHAR* Prefix, const AStageSpanActor& Span)
	{
		const FStageSpanPathLayout Layout = Span.GetPathLayout();
		FString Vertices;
		for (const FVector& Point : Layout.Points)
		{
			Vertices += FString::Printf(TEXT(" (%.0f, %.0f, %.0f)"), Point.X, Point.Y, Point.Z);
		}
		UE_LOG(LogStageCraft, Display, TEXT("%s %s \"%s\": %.1f cm in %d %s, %d segments, sag %.1f cm, %d instances, vertices%s, at %s yaw %.1f, locked %s, hidden %s."),
			Prefix, *Span.GetName(), *Span.GetInstanceLabel().ToString(), Layout.GetLength(), Layout.Legs.Num(), Layout.bClosed ? TEXT("legs (closed)") : TEXT("legs"),
			Layout.GetSegmentCount(), Layout.SagDepth, Span.GetGeometryInstanceCount(), *Vertices, *Span.GetActorLocation().ToCompactString(), Span.GetActorRotation().Yaw,
			Span.IsTransformLocked() ? TEXT("yes") : TEXT("no"), Span.IsHiddenOnStage() ? TEXT("yes") : TEXT("no"));
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const AModularPlayerController* Controller = GetController(World);
		const UStageSpanToolComponent* Tool = Controller ? Controller->GetSpanTool() : nullptr;
		if (!Tool)
		{
			return;
		}

		const FStageSpanToolStatus& ToolStatus = Tool->GetStatus();
		const AStageSpanPreview* Preview = Tool->GetPreviewActor();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Span.Status: phase %s, item \"%s\", %d points, path %s (%.1f cm, %d legs, %d segments%s, %s), ghost %s."),
			*UEnum::GetValueAsString(ToolStatus.Phase), *ToolStatus.ItemName.ToString(), ToolStatus.PointCount, ToolStatus.bHasRun ? TEXT("shown") : TEXT("none"),
			ToolStatus.Length, ToolStatus.LegCount, ToolStatus.SegmentCount, ToolStatus.bClosing ? TEXT(", closing") : TEXT(""),
			ToolStatus.bRefused ? TEXT("refused") : TEXT("allowed"), Preview && Preview->IsPreviewVisible() ? TEXT("visible") : TEXT("hidden"));

		if (const AStageSpanActor* Span = GetSelectedSpan(Controller))
		{
			LogSpan(TEXT("StageCraft.Span.Status: selected"), *Span);
		}
	}

	void Finish(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		UStageSpanToolComponent* Tool = Controller ? Controller->GetSpanTool() : nullptr;
		if (!Tool)
		{
			return;
		}
		// The Enter key's action, without needing keyboard focus in the viewport.
		const AModularBaseActor* Placed = Tool->FinishPath();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Span.Finish: %s."), Placed ? *FString::Printf(TEXT("placed %s"), *Placed->GetName()) : TEXT("nothing placed (fewer than two points, or refused)"));
		if (const AStageSpanActor* Span = Cast<AStageSpanActor>(Placed))
		{
			LogSpan(TEXT("StageCraft.Span.Finish:"), *Span);
		}
	}

	void Back(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		UStageSpanToolComponent* Tool = Controller ? Controller->GetSpanTool() : nullptr;
		if (Tool)
		{
			// The Backspace key's action.
			const bool bRemoved = Tool->RemoveLastPoint();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Span.Back: %s; %d points left."), bRemoved ? TEXT("removed the last point") : TEXT("nothing to remove"), Tool->GetPendingPoints().Num());
		}
	}

	void SetParameter(const TArray<FString>& Args, UWorld* World, const FGameplayTag& ParameterId, const TCHAR* CommandName)
	{
		AModularPlayerController* Controller = GetController(World);
		AStageSpanActor* Span = GetSelectedSpan(Controller);
		double Value = 0.0;
		if (!Span || !Args.IsValidIndex(0) || !LexTryParseString(Value, *Args[0]))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: select a span, then %s <cm>"), CommandName);
			return;
		}
		// The same validated, undoable path the inspector's field uses on Enter.
		const FStageEconomyResultInfo Result = Controller->RequestParameterChange(Span, ParameterId, FStageParameterValue::MakeFloat(Value));
		UE_LOG(LogStageCraft, Display, TEXT("%s %.1f: %s."), CommandName, Value, *UEnum::GetValueAsString(Result.Code));
		LogSpan(CommandName, *Span);
	}

	void SetLength(const TArray<FString>& Args, UWorld* World)
	{
		SetParameter(Args, World, StageCraftTags::Param_Span_Length, TEXT("StageCraft.Span.SetLength"));
	}

	void SetSag(const TArray<FString>& Args, UWorld* World)
	{
		SetParameter(Args, World, StageCraftTags::Param_Span_Sag, TEXT("StageCraft.Span.SetSag"));
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Span.Status"), TEXT("Logs the span tool (phase, points, path under the cursor, ghost) and the selected span."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs FinishCommand(TEXT("StageCraft.Span.Finish"), TEXT("Places the path being drawn (the Enter key)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Finish));
	FAutoConsoleCommandWithWorldAndArgs BackCommand(TEXT("StageCraft.Span.Back"), TEXT("Removes the last point of the path being drawn (the Backspace key)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Back));
	FAutoConsoleCommandWithWorldAndArgs SetLengthCommand(TEXT("StageCraft.Span.SetLength"), TEXT("<cm> Inspector-equivalent length edit of the selected one-leg span (validated, undoable)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetLength));
	FAutoConsoleCommandWithWorldAndArgs SetSagCommand(TEXT("StageCraft.Span.SetSag"), TEXT("<cm> Inspector-equivalent sag edit of the selected cable span (validated, undoable)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetSag));
}

#endif // !UE_BUILD_SHIPPING
