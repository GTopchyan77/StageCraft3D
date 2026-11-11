// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrExportGather.h"

#include "Actors/LightingFixtureActor.h"
#include "Actors/ModularBaseActor.h"
#include "Actors/StageSpanActor.h"
#include "Data/BaseItemData.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageItemTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Misc/App.h"
#include "Placement/StageSpanMath.h"
#include "Show/ShowControlTypes.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"

namespace StageMvrExportGather
{
	namespace
	{
		constexpr double DefaultTrussSizeCm = 30.0;

		class FIds
		{
		public:
			FGuid Take(const AModularBaseActor& Item)
			{
				for (const FGuid& Candidate : { Item.GetMvrId(), Item.GetInstanceId() })
				{
					if (Candidate.IsValid() && !Used.Contains(Candidate))
					{
						Used.Add(Candidate);
						return Candidate;
					}
				}
				return Fresh();
			}

			FGuid Claim(const FGuid& Wanted)
			{
				if (Wanted.IsValid() && !Used.Contains(Wanted))
				{
					Used.Add(Wanted);
					return Wanted;
				}
				return Fresh();
			}

		private:
			FGuid Fresh()
			{
				FGuid Id = FGuid::NewGuid();
				while (Used.Contains(Id))
				{
					Id = FGuid::NewGuid();
				}
				Used.Add(Id);
				return Id;
			}

			TSet<FGuid> Used;
		};

		bool IsTruss(const AModularBaseActor& Item)
		{
			const UBaseItemData* Data = Item.GetItemData();
			return Data && Data->CategoryTag.MatchesTag(StageCraftTags::Category_Truss);
		}

		FString LabelOf(const AModularBaseActor& Item)
		{
			return Item.GetInstanceLabel().ToString();
		}
	}

	FGuid LegId(const FGuid& ItemId, int32 Leg)
	{
		return Leg == 0 ? ItemId : FGuid(ItemId.A, ItemId.B, ItemId.C, ItemId.D ^ HashCombineFast(GetTypeHash(Leg), 0x9E3779B9u));
	}

	FGathered Gather(const UWorld& World)
	{
		FGathered Out;
		Out.Scene.ProviderVersion = FApp::GetBuildVersion();
		const UStageSessionSubsystem* Session = World.GetSubsystem<UStageSessionSubsystem>();
		if (!Session)
		{
			return Out;
		}
		const UGameInstance* GameInstance = World.GetGameInstance();
		const UStageFixtureLibrarySubsystem* Library = GameInstance ? GameInstance->GetSubsystem<UStageFixtureLibrarySubsystem>() : nullptr;
		const UStageShowLibrarySubsystem* ShowLibrary = World.GetSubsystem<UStageShowLibrarySubsystem>();

		FIds Ids;
		TMap<int32, int32> LayerOfFixture;
		if (ShowLibrary)
		{
			for (const FStageFixtureGroup& Group : ShowLibrary->GetGroups())
			{
				if (Group.FixtureIds.IsEmpty())
				{
					continue;
				}
				const int32 Layer = Out.Scene.Layers.Add({ Ids.Claim(Group.Id), Group.Name.ToString() });
				for (const int32 FixtureId : Group.FixtureIds)
				{
					LayerOfFixture.FindOrAdd(FixtureId, Layer);
				}
			}
		}
		int32 DefaultLayer = INDEX_NONE;
		const auto GetDefaultLayer = [&Out, &Ids, &DefaultLayer]()
		{
			if (DefaultLayer == INDEX_NONE)
			{
				DefaultLayer = Out.Scene.Layers.Add({ Ids.Claim(FGuid()), TEXT("StageCraft") });
			}
			return DefaultLayer;
		};

		TMap<FGuid, FString> GdtfNameOfType;
		TSet<FString> GdtfNames;
		const auto GdtfNameFor = [&](const FStageFixtureLibraryType& Type)
		{
			if (const FString* Known = GdtfNameOfType.Find(Type.FixtureTypeId))
			{
				return *Known;
			}
			const FString Hex = Type.FixtureTypeId.ToString(EGuidFormats::Digits);
			FString Name = StageMvrExport::MakeGdtfFileName(Type.SourceFileName, Hex);
			if (GdtfNames.Contains(Name.ToLower()))
			{
				Name = StageMvrExport::MakeGdtfFileName(FString(), Hex);
			}
			GdtfNames.Add(Name.ToLower());
			GdtfNameOfType.Add(Type.FixtureTypeId, Name);
			Out.GdtfPaths.Add(Name, StageFixtureLibrary::GetLibraryFilePath(Type.FixtureTypeId));
			return Name;
		};

		for (const AModularBaseActor* Item : Session->GetPlacedItems())
		{
			if (!IsValid(Item))
			{
				continue;
			}
			if (const ALightingFixtureActor* Light = Cast<ALightingFixtureActor>(Item))
			{
				StageMvrExport::FFixture& Fixture = Out.Scene.Fixtures.AddDefaulted_GetRef();
				Fixture.Id = Ids.Take(*Light);
				Fixture.Name = LabelOf(*Light);
				Fixture.Transform = Light->GetActorTransform();
				Fixture.FixtureId = Light->GetFixtureId();
				const FStageDMXPatch Patch = Light->GetPatch();
				if (Patch.IsPatched())
				{
					Fixture.Universe = Patch.Universe;
					Fixture.Address = Patch.Address;
				}
				const int32* GroupLayer = LayerOfFixture.Find(Fixture.FixtureId);
				Fixture.Layer = GroupLayer ? *GroupLayer : GetDefaultLayer();
				const ULightingFixtureData* Data = Cast<ULightingFixtureData>(Light->GetItemData());
				const FStageFixtureLibraryType* Type = Data && Data->IsLibraryType() && Library ? Library->FindType(Data->FixtureTypeId) : nullptr;
				if (Type && !Type->bSessionOnly)
				{
					Fixture.GdtfSpec = GdtfNameFor(*Type);
					Fixture.GdtfMode = Data->DMXModeName.ToString();
				}
				++Out.Fixtures;
				continue;
			}

			const FBox Bounds = Item->CalculateComponentsBoundingBoxInLocalSpace(/*bNonColliding*/ true);
			if (IsTruss(*Item) && Cast<AStageSpanActor>(Item))
			{
				const FStageSpanPathLayout Layout = CastChecked<AStageSpanActor>(Item)->GetPathLayout();
				if (!Layout.IsValid())
				{
					++Out.Skipped;
					continue;
				}
				const double Height = Bounds.IsValid ? FMath::Max(Bounds.GetSize().Z, 1.0) : DefaultTrussSizeCm;
				const double Bottom = Bounds.IsValid ? Bounds.Min.Z : 0.0;
				const FGuid ItemId = Ids.Take(*Item);
				const FString Label = LabelOf(*Item);
				const FTransform ActorTransform(Item->GetActorQuat(), Item->GetActorLocation());
				for (int32 Leg = 0; Leg < Layout.Legs.Num(); ++Leg)
				{
					const FVector Start = Layout.GetLegStart(Leg);
					const FVector Run = Layout.GetLegEnd(Leg) - Start;
					if (Run.IsNearlyZero())
					{
						continue;
					}
					StageMvrExport::FBoxObject& Truss = Out.Scene.Objects.AddDefaulted_GetRef();
					Truss.Kind = StageMvrExport::EBoxKind::Truss;
					Truss.Id = Leg == 0 ? ItemId : Ids.Claim(LegId(ItemId, Leg));
					Truss.Name = Layout.Legs.Num() == 1 ? Label : FString::Printf(TEXT("%s (%d/%d)"), *Label, Leg + 1, Layout.Legs.Num());
					Truss.Layer = GetDefaultLayer();
					Truss.Transform = FTransform(FRotationMatrix::MakeFromX(Run).ToQuat(), Start) * ActorTransform;
					Truss.LocalBounds = FBox(FVector(0.0, -Height * 0.5, Bottom), FVector(Run.Size(), Height * 0.5, Bottom + Height));
				}
				++Out.Trusses;
				continue;
			}

			if (!Bounds.IsValid || Bounds.GetSize().IsNearlyZero())
			{
				++Out.Skipped;
				continue;
			}
			StageMvrExport::FBoxObject& Object = Out.Scene.Objects.AddDefaulted_GetRef();
			const bool bTrussPiece = IsTruss(*Item);
			Object.Kind = bTrussPiece ? StageMvrExport::EBoxKind::Truss : StageMvrExport::EBoxKind::SceneObject;
			Object.Id = Ids.Take(*Item);
			Object.Name = LabelOf(*Item);
			Object.Layer = GetDefaultLayer();
			Object.Transform = Item->GetActorTransform();
			Object.LocalBounds = Bounds;
			if (bTrussPiece)
			{
				++Out.Trusses;
			}
			else
			{
				++Out.OtherItems;
			}
		}

		TArray<int32> Remap;
		Remap.Init(INDEX_NONE, Out.Scene.Layers.Num());
		for (const StageMvrExport::FFixture& Fixture : Out.Scene.Fixtures)
		{
			Remap[Fixture.Layer] = 0;
		}
		for (const StageMvrExport::FBoxObject& Object : Out.Scene.Objects)
		{
			Remap[Object.Layer] = 0;
		}
		TArray<StageMvrExport::FLayer> UsedLayers;
		for (int32 Layer = 0; Layer < Remap.Num(); ++Layer)
		{
			if (Remap[Layer] != INDEX_NONE)
			{
				Remap[Layer] = UsedLayers.Add(Out.Scene.Layers[Layer]);
			}
		}
		Out.Scene.Layers = MoveTemp(UsedLayers);
		for (StageMvrExport::FFixture& Fixture : Out.Scene.Fixtures)
		{
			Fixture.Layer = Remap[Fixture.Layer];
		}
		for (StageMvrExport::FBoxObject& Object : Out.Scene.Objects)
		{
			Object.Layer = Remap[Object.Layer];
		}
		return Out;
	}
}
