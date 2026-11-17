// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrStageReader.h"

#include "Actors/LightingFixtureActor.h"
#include "Actors/ModularBaseActor.h"
#include "Actors/StageSpanActor.h"
#include "Data/StageSpanData.h"
#include "Engine/World.h"
#include "Sketch/StageSketchSubsystem.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"

namespace StageMvrStageReader
{
	FStageMvrExistingItem Describe(const AModularBaseActor& Item)
	{
		FStageMvrExistingItem Out;
		Out.MvrId = Item.GetMvrId();
		Out.InstanceId = Item.GetInstanceId();
		Out.Item = Item.GetItemData();
		Out.Transform = Item.GetActorTransform();
		Out.Label = Item.GetInstanceLabel().ToString();
		Out.Quantity = Item.GetQuantity();
		Out.Origin = Item.GetMvrOrigin();
		Out.bLocked = Item.IsTransformLocked();
		if (const ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(&Item))
		{
			Out.bFixture = true;
			Out.FixtureId = Fixture->GetFixtureId();
			Out.Patch = Fixture->GetPatch();
		}
		else if (const AStageSpanActor* Run = Cast<AStageSpanActor>(&Item))
		{
			Out.TrussPoints = Run->GetPathLayout().Points;
		}
		return Out;
	}

	FStageMvrStage Read(const UWorld& World)
	{
		FStageMvrStage Stage;
		if (const UStageSketchSubsystem* Sketch = World.GetSubsystem<UStageSketchSubsystem>())
		{
			Stage.TrussItem = Cast<UStageSpanData>(Sketch->GetItem(EStageSketchElement::Truss));
			Stage.GenericFixture = Sketch->GetItem(EStageSketchElement::Fixture);
		}
		if (const UShowControlSubsystem* ShowControl = World.GetSubsystem<UShowControlSubsystem>())
		{
			for (const ALightingFixtureActor* Fixture : ShowControl->GetFixtures())
			{
				Stage.UsedFixtureIds.Add(Fixture->GetFixtureId());
			}
		}
		if (const UStageSessionSubsystem* Session = World.GetSubsystem<UStageSessionSubsystem>())
		{
			for (const AModularBaseActor* Item : Session->GetPlacedItems())
			{
				if (IsValid(Item) && Item->GetMvrId().IsValid())
				{
					Stage.Items.Add(Describe(*Item));
				}
			}
		}
		return Stage;
	}
}
