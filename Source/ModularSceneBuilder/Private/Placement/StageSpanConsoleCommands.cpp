// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for spans (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §3). Runs are drawn through the
// real click path with StageCraft.Edit.Arm / PointerAt / Click; these commands report the tool and edit a run's length through
// the same validated request the inspector uses. Compiled out of Shipping.

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
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Span.Status: phase %s, item \"%s\", run %s (%.1f cm, %d segments, %s), ghost %s."),
			*UEnum::GetValueAsString(ToolStatus.Phase), *ToolStatus.ItemName.ToString(), ToolStatus.bHasRun ? TEXT("shown") : TEXT("none"),
			ToolStatus.Length, ToolStatus.SegmentCount, ToolStatus.bRefused ? TEXT("refused") : TEXT("allowed"),
			Preview && Preview->IsPreviewVisible() ? TEXT("visible") : TEXT("hidden"));

		if (const AStageSpanActor* Run = Cast<AStageSpanActor>(Controller->GetSelection()->GetSelectedActor()))
		{
			const FStageSpanLayout Layout = Run->GetLayout();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Span.Status: selected %s \"%s\": %.1f cm = %d x %.1f cm, %d instances, at %s yaw %.1f, locked %s, hidden %s."),
				*Run->GetName(), *Run->GetInstanceLabel().ToString(), Layout.Length, Layout.SegmentCount, Layout.SegmentPitch, Run->GetGeometryInstanceCount(),
				*Run->GetActorLocation().ToCompactString(), Run->GetActorRotation().Yaw,
				Run->IsTransformLocked() ? TEXT("yes") : TEXT("no"), Run->IsHiddenOnStage() ? TEXT("yes") : TEXT("no"));
		}
	}

	void SetLength(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		AStageSpanActor* Run = Controller ? Cast<AStageSpanActor>(Controller->GetSelection()->GetSelectedActor()) : nullptr;
		double Length = 0.0;
		if (!Run || !Args.IsValidIndex(0) || !LexTryParseString(Length, *Args[0]))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: select a span, then StageCraft.Span.SetLength <cm>"));
			return;
		}

		// The same validated, undoable path the inspector's Length field uses on Enter.
		const FStageEconomyResultInfo Result = Controller->RequestParameterChange(Run, StageCraftTags::Param_Span_Length, FStageParameterValue::MakeFloat(Length));
		const FStageSpanLayout Layout = Run->GetLayout();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Span.SetLength %.1f: %s; now %.1f cm = %d segments, %d instances."), Length,
			*UEnum::GetValueAsString(Result.Code), Layout.Length, Layout.SegmentCount, Run->GetGeometryInstanceCount());
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Span.Status"), TEXT("Logs the span tool (phase, run under the cursor, ghost) and the selected span."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs SetLengthCommand(TEXT("StageCraft.Span.SetLength"), TEXT("<cm> Inspector-equivalent length edit of the selected span (validated, undoable)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetLength));
}

#endif // !UE_BUILD_SHIPPING
