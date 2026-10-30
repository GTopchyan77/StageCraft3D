// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/LightingFixtureActor.h"

#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/LightingFixtureData.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Interaction/StageCraftCollision.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ModularSceneBuilder.h"
#include "Show/StageFixtureCells.h"
#include "Show/StageGelLibrary.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "TextureResource.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LightingFixtureActor)

#define LOCTEXT_NAMESPACE "StageCraftLightingFixtureActor"

namespace
{
	/** Moving parts must be selectable like the base, but never block placement or physics. */
	void ConfigureSelectableMesh(UStaticMeshComponent& Mesh)
	{
		Mesh.SetMobility(EComponentMobility::Movable);
		Mesh.SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
		Mesh.SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Mesh.SetCollisionResponseToChannel(StageCraftCollision::StageItemChannel, ECR_Block);
		Mesh.SetGenerateOverlapEvents(false);
		// StageCraft has no navigation. A colliding mesh would otherwise update navigation data on every pan/tilt step,
		// about a fifth of a moving head's game-thread cost (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3).
		Mesh.SetCanEverAffectNavigation(false);
	}

	bool IsColorAttribute(const FGameplayTag& Attribute)
	{
		return Attribute == StageCraftTags::Attribute_ColorR
			|| Attribute == StageCraftTags::Attribute_ColorG
			|| Attribute == StageCraftTags::Attribute_ColorB;
	}

	/** Parameter names of the beam-shaping light-function material (Scripts/Content/create_fixture_library_content.py builds it). */
	namespace BeamShapingParams
	{
		const FName GoboTexture(TEXT("GoboTexture"));
		const FName GoboEnabled(TEXT("GoboEnabled"));
		const FName GoboRotation(TEXT("GoboRotation"));
		const FName GoboSpinRpm(TEXT("GoboSpinRpm"));
		const FName GoboSpinPhase(TEXT("GoboSpinPhase"));
		const FName GoboSpinStart(TEXT("GoboSpinStart"));
		const FName Gobo2Texture(TEXT("Gobo2Texture"));
		const FName Gobo2Enabled(TEXT("Gobo2Enabled"));
		const FName IrisRadius(TEXT("IrisRadius"));
		const FName BladeTop(TEXT("BladeTop"));
		const FName BladeRight(TEXT("BladeRight"));
		const FName BladeBottom(TEXT("BladeBottom"));
		const FName BladeLeft(TEXT("BladeLeft"));
		const FName BladeRotation(TEXT("BladeRotation"));
		const FName BladeSoftness(TEXT("BladeSoftness"));
		const FName Frost(TEXT("Frost"));
		const FName PrismFacets(TEXT("PrismFacets"));
		const FName PrismLinear(TEXT("PrismLinear"));
		const FName PrismSpread(TEXT("PrismSpread"));
		const FName PrismSpinRpm(TEXT("PrismSpinRpm"));
		const FName PrismSpinPhase(TEXT("PrismSpinPhase"));
		const FName PrismSpinStart(TEXT("PrismSpinStart"));
		const FName ConeToBeam(TEXT("ConeToBeam"));
	}

	/** Parameter names of the lens / cell strip material (Scripts/Content/create_fixture_library_content.py builds it). */
	namespace EmitterParams
	{
		const FName EmitterColor(TEXT("EmitterColor"));
		const FName CellTexture(TEXT("CellTexture"));
		const FName CellCount(TEXT("CellCount"));
		const FName Level(TEXT("Level"));
		const FName Gate(TEXT("Gate"));
	}

	/** Barndoor leaves sit far from the lens, so their edge is soft; framing blades sit at the gate and are sharp. */
	constexpr float BarndoorSoftness = 0.25f;

	/** The light colour's luminance is divided out of the intensity; this floor keeps a deep blue from asking for absurd power. */
	constexpr double MinLightColorLuminance = 0.05;

	/** Rotations, strobe and tint move or flash the beam but change no photometric value. */
	bool AffectsPhotometrics(const FGameplayTag& Attribute)
	{
		return Attribute != StageCraftTags::Attribute_GoboRotation
			&& Attribute != StageCraftTags::Attribute_GoboSpin
			&& Attribute != StageCraftTags::Attribute_PrismRotation
			&& Attribute != StageCraftTags::Attribute_BladeRotation
			&& Attribute != StageCraftTags::Attribute_Shutter
			&& Attribute != StageCraftTags::Attribute_ColorTemp;
	}

	bool MovesBeamAxis(const FGameplayTag& Attribute)
	{
		return Attribute == StageCraftTags::Attribute_Pan || Attribute == StageCraftTags::Attribute_Tilt;
	}

	/** Attributes whose edit replaces a gel's colour (the gel picker writes these). */
	bool IsGelColorAttribute(const FGameplayTag& Attribute)
	{
		int32 Cell = INDEX_NONE;
		EStageCellChannel Channel = EStageCellChannel::Count;
		if (StageFixtureCells::ParseCellAttribute(Attribute, Cell, Channel))
		{
			return Channel != EStageCellChannel::Dimmer;
		}
		return IsColorAttribute(Attribute) || Attribute == StageCraftTags::Attribute_ColorW || Attribute == StageCraftTags::Attribute_ColorA;
	}

	/** The composite inspector row a cell colour channel belongs to, or an invalid tag. */
	FGameplayTag GetCellCompositeRow(const FGameplayTag& Attribute)
	{
		int32 Cell = INDEX_NONE;
		EStageCellChannel Channel = EStageCellChannel::Count;
		if (StageFixtureCells::ParseCellAttribute(Attribute, Cell, Channel)
			&& (Channel == EStageCellChannel::ColorR || Channel == EStageCellChannel::ColorG || Channel == EStageCellChannel::ColorB))
		{
			return StageFixtureCells::GetCellAttribute(Cell, EStageCellChannel::ColorRGB);
		}
		return FGameplayTag();
	}

	/** Attributes that pick a wheel slot and so are shown as dropdowns and written as enums. */
	bool IsSlotAttribute(const FGameplayTag& Attribute)
	{
		return ULightingFixtureData::IsSteppedAttribute(Attribute);
	}

	/** Scales a mesh component so its mesh's bounds take Size (cm); an axis of Size at 0 keeps the mesh's own size. */
	void FitMeshToSize(UStaticMeshComponent& Component, const FVector& Size)
	{
		const UStaticMesh* Mesh = Component.GetStaticMesh();
		if (!Mesh)
		{
			Component.SetRelativeScale3D(FVector::OneVector);
			return;
		}
		const FVector MeshSize = Mesh->GetBounds().BoxExtent * 2.0;
		const auto AxisScale = [](double Wanted, double Own) { return Wanted > 0.0 && Own > UE_KINDA_SMALL_NUMBER ? Wanted / Own : 1.0; };
		Component.SetRelativeScale3D(FVector(AxisScale(Size.X, MeshSize.X), AxisScale(Size.Y, MeshSize.Y), AxisScale(Size.Z, MeshSize.Z)));
	}

	/** C/M/Y channels are the inverse of R/G/B, so they read and write the RGB attribute they mirror. */
	FGameplayTag ResolveDMXAttribute(const FGameplayTag& ChannelAttribute, bool& bOutInverted)
	{
		bOutInverted = true;
		if (ChannelAttribute == StageCraftTags::Attribute_ColorC) { return StageCraftTags::Attribute_ColorR; }
		if (ChannelAttribute == StageCraftTags::Attribute_ColorM) { return StageCraftTags::Attribute_ColorG; }
		if (ChannelAttribute == StageCraftTags::Attribute_ColorY) { return StageCraftTags::Attribute_ColorB; }
		bOutInverted = false;
		return ChannelAttribute;
	}

	/** Throw, illuminance and beam size need a surface. A beam that hits nothing in range says so instead of showing "0.0 m". */
	bool IsThrowDependentParameter(const FGameplayTag& ParameterId)
	{
		return ParameterId == StageCraftTags::Param_Photo_Throw || ParameterId == StageCraftTags::Param_Photo_Illuminance
			|| ParameterId == StageCraftTags::Param_Photo_BeamDiameter;
	}

	FText FormatThrowDependent(const StagePhotometry::FResult& Photo, const FGameplayTag& ParameterId)
	{
		if (!Photo.bHasThrow)
		{
			return LOCTEXT("PhotoNoSurface", "No surface");
		}

		FNumberFormattingOptions Options;
		if (ParameterId == StageCraftTags::Param_Photo_Illuminance)
		{
			Options.SetMaximumFractionalDigits(0);
			return FText::Format(LOCTEXT("PhotoLux", "{0} lx"), FText::AsNumber(Photo.IlluminanceLux, &Options));
		}
		// Centimetre precision, as a rigging plot needs.
		Options.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
		const double Metres = ParameterId == StageCraftTags::Param_Photo_Throw ? Photo.ThrowMetres : Photo.BeamDiameterMetres;
		return FText::Format(LOCTEXT("PhotoMetres", "{0} m"), FText::AsNumber(Metres, &Options));
	}
}

ALightingFixtureActor::ALightingFixtureActor()
{
	PanPivot = CreateDefaultSubobject<USceneComponent>(TEXT("PanPivot"));
	PanPivot->SetupAttachment(MeshComponent);

	YokeMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("YokeMesh"));
	YokeMeshComponent->SetupAttachment(PanPivot);
	ConfigureSelectableMesh(*YokeMeshComponent);

	TiltPivot = CreateDefaultSubobject<USceneComponent>(TEXT("TiltPivot"));
	TiltPivot->SetupAttachment(PanPivot);

	HeadMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeadMesh"));
	HeadMeshComponent->SetupAttachment(TiltPivot);
	ConfigureSelectableMesh(*HeadMeshComponent);

	BeamLight = CreateDefaultSubobject<USpotLightComponent>(TEXT("BeamLight"));
	BeamLight->SetupAttachment(TiltPivot);
	// Spot lights shine along +X; pitch it so the beam leaves the head along +Z (straight up for a standing fixture, down when hung).
	BeamLight->SetRelativeRotation(FRotator(90.0, 0.0, 0.0));
	BeamLight->SetMobility(EComponentMobility::Movable);
	BeamLight->IntensityUnits = ELightUnits::Lumens;
	BeamLight->CastShadows = true;

	// The lens glows but is not a part to click or a surface to light: no collision, no shadow, so the beam passes it.
	LensMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("LensMesh"));
	LensMeshComponent->SetupAttachment(TiltPivot);
	LensMeshComponent->SetMobility(EComponentMobility::Movable);
	LensMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	LensMeshComponent->SetGenerateOverlapEvents(false);
	LensMeshComponent->SetCanEverAffectNavigation(false);
	LensMeshComponent->SetCastShadow(false);
	LensMeshComponent->bAffectDistanceFieldLighting = false;
}

ULightingFixtureData* ALightingFixtureActor::GetFixtureData() const
{
	return Cast<ULightingFixtureData>(ItemData);
}

UShowControlSubsystem* ALightingFixtureActor::GetShowControl() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
}

UStageDMXPatchSubsystem* ALightingFixtureActor::GetPatchSubsystem() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UStageDMXPatchSubsystem>() : nullptr;
}

void ALightingFixtureActor::NotifyPatchSubsystem()
{
	// Before BeginPlay the fixture is not registered; registering reads its patch then.
	if (UStageDMXPatchSubsystem* Patching = HasActorBegunPlay() ? GetPatchSubsystem() : nullptr)
	{
		Patching->NotifyPatchChanged(*this);
	}
}

void ALightingFixtureActor::BeginPlay()
{
	Super::BeginPlay();

	if (UShowControlSubsystem* ShowControl = GetShowControl())
	{
		ShowControl->RegisterFixture(this);
	}
	// After the show control: a fixture is patched once it has its fixture ID, so the log and the patch sheet name it.
	if (UStageDMXPatchSubsystem* Patching = GetPatchSubsystem())
	{
		Patching->RegisterFixture(this);
	}
	// Item data (and a restored Shutter) was applied before play, when there was no strobe clock to join yet.
	UpdateStrobeRegistration();
}

void ALightingFixtureActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UShowControlSubsystem* ShowControl = GetShowControl())
	{
		// Unregistering also leaves the strobe clock.
		ShowControl->UnregisterFixture(this);
	}
	if (UStageDMXPatchSubsystem* Patching = GetPatchSubsystem())
	{
		Patching->UnregisterFixture(this);
	}

	Super::EndPlay(EndPlayReason);
}

void ALightingFixtureActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);

	const ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(&Data);
	if (!Fixture)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: item '%s' is not a LightingFixtureData; the fixture will stay dark."), *GetName(), *Data.GetName());
		return;
	}

	PanPivot->SetRelativeLocation(Fixture->PanPivotOffset);
	TiltPivot->SetRelativeLocation(Fixture->TiltPivotOffset);
	BeamLight->SetRelativeLocation(Fixture->BeamOriginOffset);

	YokeMeshComponent->SetStaticMesh(Fixture->bHasPanTilt ? Fixture->YokeMesh.LoadSynchronous() : nullptr);
	HeadMeshComponent->SetStaticMesh(Fixture->HeadMesh.LoadSynchronous());
	ApplyBody(*Fixture);

	BeamLight->SetAttenuationRadius(Fixture->BeamRange);
	CachedThrowCm.Reset();

	// A swapped-in model without beam shaping must not keep projecting the previous model's gobo.
	if (!Fixture->HasBeamShaping())
	{
		BeamLight->SetLightFunctionMaterial(nullptr);
		BeamShapingInstance = nullptr;
	}
	AppliedGoboSlot = INDEX_NONE;
	AppliedStaticGoboSlot = INDEX_NONE;
	// Spin clocks restart from the restored speeds when the outputs are applied below.
	GoboSpinClock = FSpinClock();
	PrismSpinClock = FSpinClock();
	if (!Fixture->AcceptsGelPreset())
	{
		AppliedGel = NAME_None;
	}

	// Keep values the instance already has (re-applied data, loaded shows); fill in new ones; drop ones the fixture cannot do.
	const TArray<FGameplayTag> Supported = Fixture->GetSupportedAttributes();
	for (auto It = Attributes.CreateIterator(); It; ++It)
	{
		if (!Supported.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	for (const FGameplayTag& Attribute : Supported)
	{
		if (!Attributes.Contains(Attribute))
		{
			Attributes.Add(Attribute, Fixture->GetAttributeDefault(Attribute));
		}
	}
	SanitizeRestoredState(*Fixture);
	// A swapped model has another footprint, and a restored state another patch: either changes the addressing.
	NotifyPatchSubsystem();

	ApplyAttributesToComponents();
	UpdateStrobeRegistration();
}

void ALightingFixtureActor::UpdateHighlight()
{
	Super::UpdateHighlight();

	UMaterialInterface* Overlay = MeshComponent->GetOverlayMaterial();
	YokeMeshComponent->SetOverlayMaterial(Overlay);
	HeadMeshComponent->SetOverlayMaterial(Overlay);
}

void ALightingFixtureActor::SanitizeRestoredState(const ULightingFixtureData& Fixture)
{
	// Values arrive unchecked from scene files and undo snapshots; the same limits as every live write apply
	// (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §3.2).
	for (TPair<FGameplayTag, float>& Pair : Attributes)
	{
		float Min = 0.f;
		float Max = 1.f;
		if (!Fixture.GetAttributeRange(Pair.Key, Min, Max))
		{
			continue;
		}
		const float Sanitized = FMath::IsFinite(Pair.Value) ? SanitizeAttributeValue(Pair.Key, Pair.Value, Min, Max) : Fixture.GetAttributeDefault(Pair.Key);
		if (!FMath::IsNearlyEqual(Pair.Value, Sanitized) || !FMath::IsFinite(Pair.Value))
		{
			ReportStateRepair(FString::Printf(TEXT("%s %s -> %s"), *Pair.Key.ToString(), *FString::SanitizeFloat(Pair.Value), *FString::SanitizeFloat(Sanitized)));
			Pair.Value = Sanitized;
		}
	}

	const FStageDMXPatch ClampedPatch = ClampPatch(Patch);
	if (!(ClampedPatch == Patch))
	{
		ReportStateRepair(FString::Printf(TEXT("DMX patch %d.%d -> %d.%d"), Patch.Universe, Patch.Address, ClampedPatch.Universe, ClampedPatch.Address));
		Patch = ClampedPatch;
	}
	if (FixtureId < 0)
	{
		ReportStateRepair(FString::Printf(TEXT("fixture ID %d -> automatic"), FixtureId));
		FixtureId = 0;
	}
}

float ALightingFixtureActor::SanitizeAttributeValue(const FGameplayTag& Attribute, float Value, float Min, float Max)
{
	const float Clamped = FMath::Clamp(Value, Min, Max);
	return ULightingFixtureData::IsSteppedAttribute(Attribute) ? FMath::RoundHalfFromZero(Clamped) : Clamped;
}

FStageDMXPatch ALightingFixtureActor::ClampPatch(const FStageDMXPatch& InPatch)
{
	FStageDMXPatch Clamped;
	Clamped.Universe = FMath::Clamp(InPatch.Universe, 0, 63999);
	Clamped.Address = FMath::Clamp(InPatch.Address, 0, 512);
	return Clamped;
}

bool ALightingFixtureActor::StoreAttribute(const FGameplayTag& Attribute, float Value)
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	float Min = 0.f;
	float Max = 1.f;
	if (!Fixture || !Fixture->GetAttributeRange(Attribute, Min, Max))
	{
		return false;
	}

	if (!FMath::IsFinite(Value))
	{
		return false;
	}

	float& Stored = Attributes.FindOrAdd(Attribute);
	const float Clamped = SanitizeAttributeValue(Attribute, Value, Min, Max);
	if (FMath::IsNearlyEqual(Stored, Clamped))
	{
		return false;
	}
	Stored = Clamped;
	return true;
}

void ALightingFixtureActor::NotifyAttributeChanged(const FGameplayTag& Attribute)
{
	NotifyParameterChanged(Attribute);
	if (IsColorAttribute(Attribute))
	{
		NotifyParameterChanged(StageCraftTags::Attribute_ColorRGB);
	}
	if (const FGameplayTag CellRow = GetCellCompositeRow(Attribute); CellRow.IsValid())
	{
		NotifyParameterChanged(CellRow);
	}
	if (IsGelColorAttribute(Attribute))
	{
		ClearAppliedGel();
	}
	if (MovesBeamAxis(Attribute))
	{
		CachedThrowCm.Reset();
	}
	if (AffectsPhotometrics(Attribute))
	{
		NotifyPhotometricsChanged();
	}
}

void ALightingFixtureActor::NotifyPhotometricsChanged()
{
	NotifyParameterChanged(StageCraftTags::Param_Photo_BeamAngle);
	NotifyParameterChanged(StageCraftTags::Param_Photo_Intensity);
	NotifyParameterChanged(StageCraftTags::Param_Photo_Throw);
	NotifyParameterChanged(StageCraftTags::Param_Photo_Illuminance);
	NotifyParameterChanged(StageCraftTags::Param_Photo_BeamDiameter);
	NotifyParameterChanged(StageCraftTags::Param_Photo_FieldAngle);
}

void ALightingFixtureActor::ClearAppliedGel()
{
	if (bApplyingGel || AppliedGel.IsNone())
	{
		return;
	}
	AppliedGel = NAME_None;
	NotifyParameterChanged(StageCraftTags::Param_Fixture_Gel);
}

void ALightingFixtureActor::OnStageTransformChanged()
{
	Super::OnStageTransformChanged();
	CachedThrowCm.Reset();
	NotifyPhotometricsChanged();
}

bool ALightingFixtureActor::SetAttribute(FGameplayTag Attribute, float Value)
{
	if (!StoreAttribute(Attribute, Value))
	{
		return false;
	}
	ApplyOutputs(GetDrivenOutputs(Attribute));
	if (Attribute == StageCraftTags::Attribute_Shutter)
	{
		UpdateStrobeRegistration();
	}
	NotifyAttributeChanged(Attribute);
	return true;
}

bool ALightingFixtureActor::SetAttributes(const TMap<FGameplayTag, float>& Values)
{
	TArray<FGameplayTag, TInlineAllocator<16>> Changed;
	EFixtureOutputs Outputs = EFixtureOutputs::None;
	for (const TPair<FGameplayTag, float>& Pair : Values)
	{
		if (StoreAttribute(Pair.Key, Pair.Value))
		{
			Changed.Add(Pair.Key);
			Outputs |= GetDrivenOutputs(Pair.Key);
		}
	}

	if (Changed.IsEmpty())
	{
		return false;
	}

	ApplyOutputs(Outputs);
	if (Changed.Contains(StageCraftTags::Attribute_Shutter))
	{
		UpdateStrobeRegistration();
	}

	// Composite and derived rows are announced once per frame of a fade, not once per changed attribute.
	bool bColorNotified = false;
	bool bPhotometricsNotified = false;
	bool bGelColorChanged = false;
	TSet<FGameplayTag, DefaultKeyFuncs<FGameplayTag>, TInlineSetAllocator<8>> CellRows;
	for (const FGameplayTag& Attribute : Changed)
	{
		NotifyParameterChanged(Attribute);
		bColorNotified |= IsColorAttribute(Attribute);
		bPhotometricsNotified |= AffectsPhotometrics(Attribute);
		bGelColorChanged |= IsGelColorAttribute(Attribute);
		if (const FGameplayTag CellRow = GetCellCompositeRow(Attribute); CellRow.IsValid())
		{
			CellRows.Add(CellRow);
		}
		if (MovesBeamAxis(Attribute))
		{
			CachedThrowCm.Reset();
		}
	}
	if (bColorNotified)
	{
		NotifyParameterChanged(StageCraftTags::Attribute_ColorRGB);
	}
	for (const FGameplayTag& CellRow : CellRows)
	{
		NotifyParameterChanged(CellRow);
	}
	if (bGelColorChanged)
	{
		ClearAppliedGel();
	}
	if (bPhotometricsNotified)
	{
		NotifyPhotometricsChanged();
	}
	return true;
}

float ALightingFixtureActor::GetAttribute(FGameplayTag Attribute) const
{
	const float* Value = Attributes.Find(Attribute);
	return Value ? *Value : 0.f;
}

FLinearColor ALightingFixtureActor::GetColorRGB() const
{
	return FLinearColor(
		GetAttribute(StageCraftTags::Attribute_ColorR),
		GetAttribute(StageCraftTags::Attribute_ColorG),
		GetAttribute(StageCraftTags::Attribute_ColorB));
}

void ALightingFixtureActor::ApplyAttributesToComponents()
{
	ApplyOutputs(EFixtureOutputs::All);
}

ALightingFixtureActor::EFixtureOutputs ALightingFixtureActor::GetDrivenOutputs(const FGameplayTag& Attribute)
{
	if (MovesBeamAxis(Attribute))
	{
		return EFixtureOutputs::Pivots;
	}
	// The shutter drives the output through the strobe clock (bStrobeOpen), applied with the dimmer level.
	if (Attribute == StageCraftTags::Attribute_Dimmer || Attribute == StageCraftTags::Attribute_Shutter)
	{
		return EFixtureOutputs::Intensity;
	}
	if (Attribute == StageCraftTags::Attribute_Zoom)
	{
		return EFixtureOutputs::Cone | EFixtureOutputs::BeamShaping;
	}
	// The mix sets the light's colour and, through the share of the flux it carries, its intensity.
	if (IsColorAttribute(Attribute) || Attribute == StageCraftTags::Attribute_ColorW || Attribute == StageCraftTags::Attribute_ColorA
		|| Attribute == StageCraftTags::Attribute_CTO || Attribute == StageCraftTags::Attribute_ColorWheel || StageFixtureCells::IsCellAttribute(Attribute))
	{
		return EFixtureOutputs::Color | EFixtureOutputs::Intensity;
	}
	if (Attribute == StageCraftTags::Attribute_ColorTemp)
	{
		return EFixtureOutputs::Temperature;
	}
	// Rotations only move the projection.
	if (Attribute == StageCraftTags::Attribute_GoboRotation || Attribute == StageCraftTags::Attribute_GoboSpin
		|| Attribute == StageCraftTags::Attribute_PrismRotation || Attribute == StageCraftTags::Attribute_BladeRotation)
	{
		return EFixtureOutputs::BeamShaping;
	}
	// The rest of the optics can switch the projection on or off, and frost and prism widen the cone.
	if (Attribute == StageCraftTags::Attribute_Gobo || Attribute == StageCraftTags::Attribute_Gobo2 || Attribute == StageCraftTags::Attribute_Iris
		|| Attribute == StageCraftTags::Attribute_Prism || Attribute == StageCraftTags::Attribute_Frost
		|| Attribute == StageCraftTags::Attribute_BladeA || Attribute == StageCraftTags::Attribute_BladeB
		|| Attribute == StageCraftTags::Attribute_BladeC || Attribute == StageCraftTags::Attribute_BladeD)
	{
		return EFixtureOutputs::Cone | EFixtureOutputs::BeamShaping;
	}
	// An attribute this list does not know yet still reaches the components.
	return EFixtureOutputs::All;
}

void ALightingFixtureActor::ApplyOutputs(EFixtureOutputs Outputs)
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture)
	{
		BeamLight->SetIntensity(0.f);
		return;
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Pivots) && Fixture->bHasPanTilt)
	{
		// Both rotations first, then one transform update from the pan pivot down: setting them one after the other moved the
		// head, the lens and the beam twice per change.
		TiltPivot->SetRelativeRotation_Direct(FRotator(GetAttribute(StageCraftTags::Attribute_Tilt), 0.0, 0.0));
		PanPivot->SetRelativeRotation_Direct(FRotator(0.0, GetAttribute(StageCraftTags::Attribute_Pan), 0.0));
		PanPivot->UpdateComponentToWorld();

		// Engine cost workaround (UE 5.8, measured with 500 moving heads): a moved light goes through the render thread's
		// per-light UpdateLightTransform, about 87 us per light per frame, while a light whose render state is rebuilt goes
		// through the batched remove/add, about 11 us. A pending rebuild replaces the transform update, so moving heads take
		// the batched path: 500 fixtures panning went from 58 to 27 ms a frame. The light is rebuilt anyway on every zoom
		// change (USpotLightComponent::SetOuterConeAngle). Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3.
		BeamLight->MarkRenderStateDirty();
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Color))
	{
		// Every colour system ends as one linear colour: emitters, or the lamp's white through filters (Docs/ADR/0020 §2).
		BeamLight->SetLightColor(GetBeamMix().Color, /*bSRGB*/ false);
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Intensity | EFixtureOutputs::Color))
	{
		// The strobe clock closes the output between flashes; the dimmer level is kept for the next flash.
		BeamLight->SetIntensity(bStrobeOpen ? ComputeBeamIntensity(*Fixture) : 0.f);
		ApplyEmitterLevel();
		if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Color))
		{
			ApplyEmitterColor(*Fixture);
		}
		// A brighter or darker beam may deserve (or lose) a place among the shadowed fixtures; this only arms a re-ranking.
		if (UShowControlSubsystem* ShowControl = HasActorBegunPlay() ? GetShowControl() : nullptr)
		{
			ShowControl->MarkShadowBudgetDirty();
		}
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Cone))
	{
		// Zoom and frost set the beam and field angles; a prism widens the cone so its copies fit. With a projection the
		// light function draws the edge, so the spot light's own falloff stays out of its way.
		const FConeAngles Angles = ComputeConeAngles(*Fixture);
		BeamLight->SetOuterConeAngle(static_cast<float>(Angles.OuterHalfDegrees));
		BeamLight->SetInnerConeAngle(static_cast<float>(Angles.InnerHalfDegrees));
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::Temperature))
	{
		// UE's temperature tints relative to 6500 K: CTC on colour mixers only. Fixed whites, CMY lamps and CTO are in the mix.
		BeamLight->SetUseTemperature(Fixture->HasColorTemperatureControl());
		if (Fixture->HasColorTemperatureControl())
		{
			BeamLight->SetTemperature(GetAttribute(StageCraftTags::Attribute_ColorTemp));
		}
	}

	if (EnumHasAnyFlags(Outputs, EFixtureOutputs::BeamShaping) && Fixture->HasBeamShaping())
	{
		ApplyBeamShaping(*Fixture);
	}
}

StageColorScience::FMix ALightingFixtureActor::ComputeCellMix(const ULightingFixtureData& Fixture, TConstArrayView<FLinearColor> Outputs, int32 CellIndex) const
{
	StageColorScience::FMix Mix;
	if (Fixture.HasCellColor())
	{
		static constexpr EStageCellChannel ColorChannels[] = {
			EStageCellChannel::ColorR, EStageCellChannel::ColorG, EStageCellChannel::ColorB, EStageCellChannel::ColorW, EStageCellChannel::ColorA };
		TArray<float, TInlineAllocator<5>> Levels;
		for (const EStageCellChannel Channel : ColorChannels)
		{
			const FGameplayTag Attribute = StageFixtureCells::GetCellAttribute(CellIndex, Channel);
			if (Fixture.SupportsAttribute(Attribute))
			{
				Levels.Add(GetAttribute(Attribute));
			}
		}
		Mix = StageColorScience::MixAdditive(Outputs, Levels);
	}
	else
	{
		// White cells (a blinder's lamps): the fixture's white, with CTO if it has one.
		Mix = StageColorScience::MixSubtractive(Fixture.ColorTemperatureK, FLinearColor::White, GetAttribute(StageCraftTags::Attribute_CTO), Fixture.FullCTOMiredShift);
	}
	if (Fixture.HasCellDimmer())
	{
		Mix.LuminousFraction *= FMath::Clamp(GetAttribute(StageFixtureCells::GetCellAttribute(CellIndex, EStageCellChannel::Dimmer)), 0.f, 1.f);
	}
	return Mix;
}

StageColorScience::FMix ALightingFixtureActor::GetCellMix(int32 CellIndex) const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture || !Fixture->HasCells() || CellIndex < 0 || CellIndex >= Fixture->CellCount)
	{
		return StageColorScience::FMix();
	}
	return ComputeCellMix(*Fixture, Fixture->GetEmitterOutputs(), CellIndex);
}

StageColorScience::FMix ALightingFixtureActor::GetBeamMix() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture)
	{
		return StageColorScience::FMix();
	}

	StageColorScience::FMix Mix;
	if (Fixture->HasCells())
	{
		const TArray<FLinearColor> Outputs = Fixture->GetEmitterOutputs();
		TArray<StageColorScience::FMix, TInlineAllocator<StageFixtureCells::MaxCells>> Cells;
		for (int32 Cell = 0; Cell < Fixture->CellCount; ++Cell)
		{
			Cells.Add(ComputeCellMix(*Fixture, Outputs, Cell));
		}
		Mix = StageColorScience::Combine(Cells);
	}
	else if (Fixture->IsAdditive())
	{
		TArray<float, TInlineAllocator<5>> Levels;
		for (const FGameplayTag& Attribute : Fixture->GetColorAttributes())
		{
			Levels.Add(GetAttribute(Attribute));
		}
		Mix = StageColorScience::MixAdditive(Fixture->GetEmitterOutputs(), Levels);
	}
	else
	{
		// CMY filters and a gel frame both pass R/G/B of the lamp's white; a plain white fixture passes everything.
		const bool bFiltered = Fixture->ColorSystem == EStageFixtureColorSystem::CMY || Fixture->HasGelFrame();
		Mix = StageColorScience::MixSubtractive(Fixture->ColorTemperatureK, bFiltered ? GetColorRGB() : FLinearColor::White,
			GetAttribute(StageCraftTags::Attribute_CTO), Fixture->FullCTOMiredShift);
	}

	const int32 WheelSlot = FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_ColorWheel));
	if (WheelSlot > 0)
	{
		Mix = StageColorScience::ApplyFilter(Mix, Fixture->GetColorWheelFilter(WheelSlot));
	}
	return Mix;
}

float ALightingFixtureActor::ComputeBeamIntensity(const ULightingFixtureData& Fixture) const
{
	// The light's colour multiplies its intensity, so its luminance is divided out: the beam then carries the luminous flux
	// the mix really has. Full RGB on an RGBW fixture is about half its rated output, as on the real fixture.
	const StageColorScience::FMix Mix = GetBeamMix();
	const double Luminance = FMath::Max(StageColorScience::GetLuminance(Mix.Color), MinLightColorLuminance);
	const double Lumens = Fixture.LuminousFlux * FMath::Clamp(GetAttribute(StageCraftTags::Attribute_Dimmer), 0.f, 1.f) * Mix.LuminousFraction / Luminance;
	return static_cast<float>(Lumens);
}

bool ALightingFixtureActor::IsProjectingShape(const ULightingFixtureData& Fixture) const
{
	return FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Gobo)) != 0
		|| FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Gobo2)) != 0
		|| FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Prism)) != 0
		|| (Fixture.bHasIris && GetAttribute(StageCraftTags::Attribute_Iris) > 0.f)
		|| GetAttribute(StageCraftTags::Attribute_BladeA) > 0.f || GetAttribute(StageCraftTags::Attribute_BladeB) > 0.f
		|| GetAttribute(StageCraftTags::Attribute_BladeC) > 0.f || GetAttribute(StageCraftTags::Attribute_BladeD) > 0.f;
}

ALightingFixtureActor::FConeAngles ALightingFixtureActor::ComputeConeAngles(const ULightingFixtureData& Fixture) const
{
	FConeAngles Angles;
	const double Zoom = Fixture.HasZoom() ? GetAttribute(StageCraftTags::Attribute_Zoom) : Fixture.BeamAngleMax;
	Angles.BeamDegrees = StagePhotometry::ApplyFrost(Zoom, GetAttribute(StageCraftTags::Attribute_Frost), Fixture.FrostFieldMultiplier);
	Angles.FieldDegrees = StagePhotometry::GetFieldAngle(Angles.BeamDegrees, Fixture.FieldAngleRatio);

	const double TanBeam = FMath::Tan(FMath::DegreesToRadians(Angles.BeamDegrees * 0.5));
	double TanOuter = FMath::Tan(FMath::DegreesToRadians(Angles.FieldDegrees * 0.5));
	if (const FStagePrism* Prism = Fixture.GetPrism(FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Prism))))
	{
		// Each copy is a whole beam whose centre sits Spread beam radii off the axis (further out for the ends of a linear prism).
		const double Reach = Prism->bLinear ? Prism->Spread * FMath::Max(1.0, (Prism->Facets - 1) * 0.5) : Prism->Spread;
		TanOuter += TanBeam * Reach;
	}
	// A spot light's cone stops short of 90 degrees.
	TanOuter = FMath::Min(TanOuter, FMath::Tan(FMath::DegreesToRadians(StagePhotometry::MaxBeamAngleDegrees * 0.5)));
	Angles.OuterHalfDegrees = FMath::RadiansToDegrees(FMath::Atan(TanOuter));
	Angles.ConeToBeam = TanBeam > UE_KINDA_SMALL_NUMBER ? TanOuter / TanBeam : 1.0;
	Angles.InnerHalfDegrees = IsProjectingShape(Fixture)
		? Angles.OuterHalfDegrees
		: 0.5 * StagePhotometry::GetInnerConeAngle(Angles.BeamDegrees, Angles.FieldDegrees);
	return Angles;
}

void ALightingFixtureActor::UpdateSpinClock(FSpinClock& Clock, float NewRpm) const
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (Clock.Rpm == NewRpm && Clock.StartSeconds > 0.0)
	{
		return;
	}
	// rpm x 6 = degrees per second. The angle reached so far becomes the new phase.
	Clock.PhaseDegrees = FMath::Fmod(Clock.PhaseDegrees + Clock.Rpm * 6.0 * FMath::Max(0.0, Now - Clock.StartSeconds), 360.0);
	Clock.StartSeconds = Now > 0.0 ? Now : UE_SMALL_NUMBER;
	Clock.Rpm = NewRpm;
}

void ALightingFixtureActor::ApplyBeamShaping(const ULightingFixtureData& Fixture)
{
	if (!IsProjectingShape(Fixture))
	{
		// An open beam projects nothing, and a light function is a full extra lighting pass per light: with 500 fixtures it
		// was two thirds of the GPU frame (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3). The instance is kept.
		if (BeamLight->LightFunctionMaterial)
		{
			BeamLight->SetLightFunctionMaterial(nullptr);
		}
		return;
	}

	if (!BeamShapingInstance)
	{
		if (bBeamShapingUnavailable)
		{
			return;
		}
		UMaterialInterface* Parent = BeamShapingMaterial.LoadSynchronous();
		if (!Parent)
		{
			// Once per fixture: attribute changes would otherwise repeat it every frame of a fade.
			bBeamShapingUnavailable = true;
			UE_LOG(LogStageCraft, Warning, TEXT("%s: BeamShapingMaterial is not set ([/Script/ModularSceneBuilder.LightingFixtureActor] in DefaultGame.ini); gobo, iris, blades and prism are not projected."),
				*GetName());
			return;
		}
		BeamShapingInstance = UMaterialInstanceDynamic::Create(Parent, this);
		AppliedGoboSlot = INDEX_NONE;
		AppliedStaticGoboSlot = INDEX_NONE;
	}
	if (BeamLight->LightFunctionMaterial != BeamShapingInstance)
	{
		BeamLight->SetLightFunctionMaterial(BeamShapingInstance);
	}

	// Textures are swapped only when a slot changes; the scalars are cheap to set on every attribute change.
	const auto ApplyGobo = [this, &Fixture](EStageGoboWheel Wheel, const FGameplayTag& Attribute, int32& AppliedSlot, const FName& TextureParam, const FName& EnabledParam)
	{
		const int32 Slot = FMath::RoundToInt32(GetAttribute(Attribute));
		if (Slot == AppliedSlot)
		{
			return;
		}
		UTexture2D* GoboTexture = Fixture.LoadGoboTexture(Slot, Wheel);
		if (GoboTexture)
		{
			BeamShapingInstance->SetTextureParameterValue(TextureParam, GoboTexture);
		}
		BeamShapingInstance->SetScalarParameterValue(EnabledParam, GoboTexture ? 1.f : 0.f);
		AppliedSlot = Slot;
	};
	ApplyGobo(EStageGoboWheel::Rotating, StageCraftTags::Attribute_Gobo, AppliedGoboSlot, BeamShapingParams::GoboTexture, BeamShapingParams::GoboEnabled);
	ApplyGobo(EStageGoboWheel::Static, StageCraftTags::Attribute_Gobo2, AppliedStaticGoboSlot, BeamShapingParams::Gobo2Texture, BeamShapingParams::Gobo2Enabled);

	UpdateSpinClock(GoboSpinClock, GetAttribute(StageCraftTags::Attribute_GoboSpin));
	UpdateSpinClock(PrismSpinClock, GetAttribute(StageCraftTags::Attribute_PrismRotation));

	const float IrisRadius = Fixture.bHasIris ? FMath::Lerp(1.f, Fixture.IrisMinFraction, GetAttribute(StageCraftTags::Attribute_Iris)) : 1.f;
	const FStagePrism* Prism = Fixture.GetPrism(FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Prism)));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::GoboRotation, GetAttribute(StageCraftTags::Attribute_GoboRotation));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::GoboSpinRpm, GoboSpinClock.Rpm);
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::GoboSpinPhase, static_cast<float>(GoboSpinClock.PhaseDegrees));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::GoboSpinStart, static_cast<float>(GoboSpinClock.StartSeconds));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::IrisRadius, IrisRadius);
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeTop, GetAttribute(StageCraftTags::Attribute_BladeA));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeRight, GetAttribute(StageCraftTags::Attribute_BladeB));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeBottom, GetAttribute(StageCraftTags::Attribute_BladeC));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeLeft, GetAttribute(StageCraftTags::Attribute_BladeD));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeRotation, GetAttribute(StageCraftTags::Attribute_BladeRotation));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::BladeSoftness, Fixture.BeamShaper == EStageBeamShaper::Barndoors ? BarndoorSoftness : 0.f);
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::Frost, GetAttribute(StageCraftTags::Attribute_Frost));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::PrismFacets, Prism ? static_cast<float>(Prism->Facets) : 0.f);
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::PrismLinear, Prism && Prism->bLinear ? 1.f : 0.f);
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::PrismSpread, Prism ? Prism->Spread : 0.f);
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::PrismSpinRpm, PrismSpinClock.Rpm);
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::PrismSpinPhase, static_cast<float>(PrismSpinClock.PhaseDegrees));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::PrismSpinStart, static_cast<float>(PrismSpinClock.StartSeconds));
	BeamShapingInstance->SetScalarParameterValue(BeamShapingParams::ConeToBeam, static_cast<float>(ComputeConeAngles(Fixture).ConeToBeam));
}

void ALightingFixtureActor::ApplyBody(const ULightingFixtureData& Fixture)
{
	// The base is the actor's root, so it cannot be scaled without scaling the whole fixture; library fixtures get a base mesh
	// built at its real size (Scripts/Content/create_fixture_library_content.py). The moving parts are scaled here.
	if (Fixture.bHasPanTilt)
	{
		FitMeshToSize(*YokeMeshComponent, Fixture.Body.YokeSizeCm);
	}
	FitMeshToSize(*HeadMeshComponent, Fixture.Body.HeadSizeCm);

	LensMeshComponent->SetRelativeLocation(Fixture.BeamOriginOffset);
	UStaticMesh* Emitter = nullptr;
	FVector EmitterSize = FVector::ZeroVector;
	// A strip is the emitting face of bars, blinders and strobes (cells or not); a round lens that of the rest.
	if (Fixture.Body.CellStripSizeCm.X > 0.0 && Fixture.Body.CellStripSizeCm.Y > 0.0)
	{
		Emitter = CellStripMesh.LoadSynchronous();
		EmitterSize = FVector(Fixture.Body.CellStripSizeCm.X, Fixture.Body.CellStripSizeCm.Y, 0.0);
	}
	else if (Fixture.Body.LensDiameterCm > 0.f)
	{
		Emitter = LensMesh.LoadSynchronous();
		EmitterSize = FVector(Fixture.Body.LensDiameterCm, Fixture.Body.LensDiameterCm, 1.0);
	}
	LensMeshComponent->SetStaticMesh(Emitter);
	FitMeshToSize(*LensMeshComponent, EmitterSize);
	if (!Emitter)
	{
		return;
	}

	if (!EmitterInstance)
	{
		UMaterialInterface* Parent = EmitterMaterial.LoadSynchronous();
		if (!Parent)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("%s: EmitterMaterial is not set ([/Script/ModularSceneBuilder.LightingFixtureActor] in DefaultGame.ini); the lens does not glow."), *GetName());
			return;
		}
		EmitterInstance = UMaterialInstanceDynamic::Create(Parent, this);
	}
	LensMeshComponent->SetMaterial(0, EmitterInstance);

	// One texel per cell; a fixture swapped to another cell count gets a new texture.
	const int32 Cells = Fixture.HasCells() ? Fixture.CellCount : 0;
	if (Cells > 0 && (!CellTexture || CellTexture->GetSizeX() != Cells))
	{
		CellTexture = UTexture2D::CreateTransient(Cells, 1, PF_B8G8R8A8);
		CellTexture->Filter = TF_Nearest;
		CellTexture->SRGB = true;
		CellTexture->AddressX = TA_Clamp;
		CellTexture->AddressY = TA_Clamp;
		CellTexture->UpdateResource();
	}
	else if (Cells == 0)
	{
		CellTexture = nullptr;
	}
	if (CellTexture)
	{
		EmitterInstance->SetTextureParameterValue(EmitterParams::CellTexture, CellTexture);
	}
	EmitterInstance->SetScalarParameterValue(EmitterParams::CellCount, static_cast<float>(FMath::Max(Cells, 1)));
}

void ALightingFixtureActor::ApplyEmitterLevel()
{
	// The dimmer and the strobe gate are scalars, so a dimmer fade or a strobe flash never re-uploads the cell texture.
	if (EmitterInstance && LensMeshComponent->GetStaticMesh())
	{
		EmitterInstance->SetScalarParameterValue(EmitterParams::Level, FMath::Clamp(GetAttribute(StageCraftTags::Attribute_Dimmer), 0.f, 1.f));
		EmitterInstance->SetScalarParameterValue(EmitterParams::Gate, bStrobeOpen ? 1.f : 0.f);
	}
}

void ALightingFixtureActor::ApplyEmitterColor(const ULightingFixtureData& Fixture)
{
	if (!EmitterInstance || !LensMeshComponent->GetStaticMesh())
	{
		return;
	}

	if (!CellTexture)
	{
		const StageColorScience::FMix Mix = GetBeamMix();
		EmitterInstance->SetVectorParameterValue(EmitterParams::EmitterColor, Mix.Color * static_cast<float>(Mix.LuminousFraction));
		return;
	}

	// Each cell's colour at its own level. The render thread copies the texels later, so the buffer it reads is owned by the
	// clean-up callback, which runs once the copy is done.
	struct FCellUpload
	{
		TArray<FColor> Texels;
		FUpdateTextureRegion2D Region;
	};
	const TSharedRef<FCellUpload> Upload = MakeShared<FCellUpload>();
	const TArray<FLinearColor> Outputs = Fixture.GetEmitterOutputs();
	const int32 Cells = CellTexture->GetSizeX();
	Upload->Texels.Reserve(Cells);
	for (int32 Cell = 0; Cell < Cells; ++Cell)
	{
		const StageColorScience::FMix Mix = ComputeCellMix(Fixture, Outputs, Cell);
		const FLinearColor Lit = Mix.Color * static_cast<float>(Mix.LuminousFraction);
		Upload->Texels.Add(FLinearColor(Lit.R, Lit.G, Lit.B, 1.f).ToFColor(/*bSRGB*/ true));
	}
	Upload->Region = FUpdateTextureRegion2D(0, 0, 0, 0, Cells, 1);
	CellTexture->UpdateTextureRegions(0, 1, &Upload->Region, Cells * sizeof(FColor), sizeof(FColor), reinterpret_cast<uint8*>(Upload->Texels.GetData()),
		[Upload](uint8*, const FUpdateTextureRegion2D*) {});
}

// --- Strobe ---

float ALightingFixtureActor::GetStrobeRateHz() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	const float Shutter = GetAttribute(StageCraftTags::Attribute_Shutter);
	if (!Fixture || !Fixture->bHasShutter || Shutter <= 0.f)
	{
		return 0.f;
	}
	return FMath::Lerp(static_cast<float>(Fixture->StrobeRateHz.X), static_cast<float>(Fixture->StrobeRateHz.Y), FMath::Clamp(Shutter, 0.f, 1.f));
}

void ALightingFixtureActor::SetStrobeOpen(bool bOpen)
{
	if (bStrobeOpen == bOpen)
	{
		return;
	}
	bStrobeOpen = bOpen;
	// Only the output changes; nothing the inspector shows, so no parameter notification.
	const ULightingFixtureData* Fixture = GetFixtureData();
	BeamLight->SetIntensity(bStrobeOpen && Fixture ? ComputeBeamIntensity(*Fixture) : 0.f);
	ApplyEmitterLevel();
}

void ALightingFixtureActor::SetBeamShadowsEnabled(bool bEnabled)
{
	if (BeamLight && BeamLight->CastShadows != bEnabled)
	{
		BeamLight->SetCastShadows(bEnabled);
	}
}

bool ALightingFixtureActor::AreBeamShadowsEnabled() const
{
	return BeamLight && BeamLight->CastShadows;
}

float ALightingFixtureActor::GetShadowRankingFlux() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	return Fixture ? static_cast<float>(Fixture->LuminousFlux * GetAttribute(StageCraftTags::Attribute_Dimmer) * GetBeamMix().LuminousFraction) : 0.f;
}

void ALightingFixtureActor::UpdateStrobeRegistration()
{
	// Before BeginPlay (deferred spawn, editor construction) the subsystem may not exist; BeginPlay's data apply registers then.
	if (UShowControlSubsystem* ShowControl = HasActorBegunPlay() ? GetShowControl() : nullptr)
	{
		ShowControl->SetFixtureStrobing(this, GetStrobeRateHz() > 0.f);
	}
}

// --- Photometrics ---

double ALightingFixtureActor::TraceThrow() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	const UWorld* World = GetWorld();
	if (!Fixture || !World)
	{
		return 0.0;
	}

	const FVector Start = BeamLight->GetComponentLocation();
	const FVector End = Start + BeamLight->GetForwardVector() * Fixture->BeamRange;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(StageFixtureThrow), /*bTraceComplex*/ false, this);
	FHitResult Hit;
	return World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params) ? Hit.Distance : 0.0;
}

StagePhotometry::FResult ALightingFixtureActor::ComputePhotometrics() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture)
	{
		return StagePhotometry::FResult();
	}

	const FConeAngles Angles = ComputeConeAngles(*Fixture);
	const double Iris = Fixture->bHasIris ? GetAttribute(StageCraftTags::Attribute_Iris) : 0.0;

	double Transmission = GetAttribute(StageCraftTags::Attribute_Dimmer)
		* GetBeamMix().LuminousFraction
		* StagePhotometry::GetIrisTransmission(Iris, Fixture->IrisMinFraction)
		* Fixture->GetGoboTransmission(FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Gobo)), EStageGoboWheel::Rotating)
		* Fixture->GetGoboTransmission(FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Gobo2)), EStageGoboWheel::Static);
	if (Fixture->HasBlades())
	{
		Transmission *= StagePhotometry::GetShutterOpenFraction(GetAttribute(StageCraftTags::Attribute_BladeA), GetAttribute(StageCraftTags::Attribute_BladeB),
			GetAttribute(StageCraftTags::Attribute_BladeC), GetAttribute(StageCraftTags::Attribute_BladeD));
	}
	if (const FStagePrism* Prism = Fixture->GetPrism(FMath::RoundToInt32(GetAttribute(StageCraftTags::Attribute_Prism))))
	{
		// The values describe one of the prism's copies: each gets an equal share of what the prism passes.
		Transmission *= FMath::Clamp(Prism->Transmission, 0.f, 1.f) / FMath::Max(Prism->Facets, 1);
	}

	if (!CachedThrowCm.IsSet())
	{
		CachedThrowCm = TraceThrow();
	}

	StagePhotometry::FInputs Inputs;
	Inputs.LuminousFluxLm = Fixture->LuminousFlux;
	Inputs.BeamAngleDegrees = StagePhotometry::ApplyIris(Angles.BeamDegrees, Iris, Fixture->IrisMinFraction);
	Inputs.Transmission = Transmission;
	Inputs.ThrowCm = CachedThrowCm.GetValue();
	return StagePhotometry::Compute(Inputs);
}

bool ALightingFixtureActor::SetFixtureId(int32 NewFixtureId)
{
	if (NewFixtureId <= 0 || NewFixtureId == FixtureId)
	{
		return NewFixtureId == FixtureId;
	}

	const UShowControlSubsystem* ShowControl = GetShowControl();
	if (ShowControl && !ShowControl->IsFixtureIdFree(NewFixtureId, this))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: fixture ID %d is already in use."), *GetName(), NewFixtureId);
		return false;
	}

	FixtureId = NewFixtureId;
	NotifyParameterChanged(StageCraftTags::Param_Patch_FixtureId);
	return true;
}

void ALightingFixtureActor::SetPatch(const FStageDMXPatch& NewPatch)
{
	const FStageDMXPatch Clamped = ClampPatch(NewPatch);
	if (Clamped == Patch)
	{
		return;
	}

	const bool bUniverseChanged = Clamped.Universe != Patch.Universe;
	const bool bAddressChanged = Clamped.Address != Patch.Address;
	Patch = Clamped;
	NotifyPatchSubsystem();
	if (bUniverseChanged)
	{
		NotifyParameterChanged(StageCraftTags::Param_Patch_Universe);
	}
	if (bAddressChanged)
	{
		NotifyParameterChanged(StageCraftTags::Param_Patch_Address);
	}
}

int32 ALightingFixtureActor::GetDMXFootprint() const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	return Fixture ? Fixture->GetDMXFootprint() : 0;
}

void ALightingFixtureActor::WriteDMX(TArrayView<uint8> UniverseData) const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture || !Patch.IsPatched())
	{
		return;
	}

	for (const FStageFixtureDMXChannel& Channel : Fixture->DMXChannels)
	{
		bool bInverted = false;
		const FGameplayTag Attribute = ResolveDMXAttribute(Channel.Attribute, bInverted);
		if (!Fixture->SupportsAttribute(Channel.Attribute) || !Fixture->SupportsAttribute(Attribute))
		{
			continue;
		}

		const int32 Index = Patch.Address - 1 + Channel.Channel - 1;
		if (Index < 0 || Index + Channel.GetWidth() > UniverseData.Num())
		{
			continue;
		}

		float Normalized = Fixture->NormalizeForDMX(Attribute, GetAttribute(Attribute));
		Normalized = FMath::Clamp(bInverted ? 1.f - Normalized : Normalized, 0.f, 1.f);

		if (Channel.b16Bit)
		{
			const uint16 Value = static_cast<uint16>(FMath::RoundToInt(Normalized * 65535.f));
			UniverseData[Index] = static_cast<uint8>(Value >> 8);
			UniverseData[Index + 1] = static_cast<uint8>(Value & 0xFF);
		}
		else
		{
			UniverseData[Index] = static_cast<uint8>(FMath::RoundToInt(Normalized * 255.f));
		}
	}
}

bool ALightingFixtureActor::ReadDMX(TConstArrayView<uint8> UniverseData)
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture || !Patch.IsPatched())
	{
		return false;
	}

	TMap<FGameplayTag, float> Values;
	for (const FStageFixtureDMXChannel& Channel : Fixture->DMXChannels)
	{
		bool bInverted = false;
		const FGameplayTag Attribute = ResolveDMXAttribute(Channel.Attribute, bInverted);
		if (!Fixture->SupportsAttribute(Channel.Attribute) || !Fixture->SupportsAttribute(Attribute))
		{
			continue;
		}

		const int32 Index = Patch.Address - 1 + Channel.Channel - 1;
		if (Index < 0 || Index + Channel.GetWidth() > UniverseData.Num())
		{
			continue;
		}

		float Normalized = Channel.b16Bit
			? static_cast<float>((UniverseData[Index] << 8) | UniverseData[Index + 1]) / 65535.f
			: static_cast<float>(UniverseData[Index]) / 255.f;
		if (bInverted)
		{
			Normalized = 1.f - Normalized;
		}

		Values.Add(Attribute, Fixture->DenormalizeFromDMX(Attribute, Normalized));
	}

	return SetAttributes(Values);
}

FStageItemInteractionDetails ALightingFixtureActor::GetInteractionDetails_Implementation() const
{
	FStageItemInteractionDetails Details = Super::GetInteractionDetails_Implementation();

	const ULightingFixtureData* Fixture = GetFixtureData();
	Details.bSupportsColorEditing = Fixture && (Fixture->AcceptsGelPreset() || Fixture->HasCellColor());
	Details.Color = GetDisplayColor(INDEX_NONE);
	return Details;
}

// --- Gels and cells ---

bool ALightingFixtureActor::ApplyGel(int32 GelIndex)
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	const TArray<FStageGel>& Gels = FStageGelLibrary::Get().GetGels();
	if (!Fixture || !Gels.IsValidIndex(GelIndex) || !(Fixture->AcceptsGelPreset() || Fixture->HasCellColor()))
	{
		return false;
	}
	const FStageGel& Gel = Gels[GelIndex];

	TGuardValue<bool> ApplyingGel(bApplyingGel, true);
	SetAttributes(MatchColor(Gel.Filter));
	AppliedGel = FName(*Gel.GetCode());
	NotifyParameterChanged(StageCraftTags::Param_Fixture_Gel);
	return true;
}

TMap<FGameplayTag, float> ALightingFixtureActor::MatchColor(const FLinearColor& Filter) const
{
	return MatchColorValues(Filter, INDEX_NONE);
}

TMap<FGameplayTag, float> ALightingFixtureActor::MatchColorValues(const FLinearColor& Color, int32 OnlyCell) const
{
	TMap<FGameplayTag, float> Values;
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture || !(Fixture->AcceptsGelPreset() || Fixture->HasCellColor()) || (OnlyCell != INDEX_NONE && !Fixture->HasCellColor()))
	{
		return Values;
	}
	const FLinearColor Clamped = FMath::IsFinite(Color.R) && FMath::IsFinite(Color.G) && FMath::IsFinite(Color.B)
		? FLinearColor(FMath::Clamp(Color.R, 0.f, 1.f), FMath::Clamp(Color.G, 0.f, 1.f), FMath::Clamp(Color.B, 0.f, 1.f))
		: FLinearColor::Black;

	if (!Fixture->IsAdditive())
	{
		// CMY filters and a gel frame are filters already: they take the colour as their transmission.
		Values.Add(StageCraftTags::Attribute_ColorR, Clamped.R);
		Values.Add(StageCraftTags::Attribute_ColorG, Clamped.G);
		Values.Add(StageCraftTags::Attribute_ColorB, Clamped.B);
		return Values;
	}

	// Emitters reproduce the colour as the fixture's own full white through it would look, as brightly as they can, then at the
	// colour's own brightness (a dark red from the picker is a dimmer red, not a full one).
	const TArray<FLinearColor> Outputs = Fixture->GetEmitterOutputs();
	FLinearColor FullWhite = FLinearColor::Black;
	for (const FLinearColor& Output : Outputs)
	{
		FullWhite += Output;
	}
	const float Brightness = FMath::Max3(Clamped.R, Clamped.G, Clamped.B);
	const TArray<float> Levels = StageColorScience::MatchAdditive(Outputs, FullWhite * Clamped);
	const TArray<FGameplayTag> ColorAttributes = Fixture->GetColorAttributes();
	for (int32 Index = 0; Index < ColorAttributes.Num() && Index < Levels.Num(); ++Index)
	{
		const float Level = Levels[Index] * Brightness;
		if (!Fixture->HasCells())
		{
			Values.Add(ColorAttributes[Index], Level);
			continue;
		}
		const EStageCellChannel Channel = ColorAttributes[Index] == StageCraftTags::Attribute_ColorW ? EStageCellChannel::ColorW
			: ColorAttributes[Index] == StageCraftTags::Attribute_ColorA ? EStageCellChannel::ColorA
			: static_cast<EStageCellChannel>(static_cast<int32>(EStageCellChannel::ColorR) + Index);
		for (int32 Cell = 0; Cell < Fixture->CellCount; ++Cell)
		{
			if (OnlyCell == INDEX_NONE || OnlyCell == Cell)
			{
				Values.Add(StageFixtureCells::GetCellAttribute(Cell, Channel), Level);
			}
		}
	}
	return Values;
}

FLinearColor ALightingFixtureActor::GetDisplayColor(int32 Cell) const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture)
	{
		return FLinearColor::Black;
	}
	if (Cell == INDEX_NONE && Fixture->HasCells())
	{
		return GetBeamMix().Color;
	}
	if (!Fixture->IsAdditive())
	{
		return GetColorRGB();
	}
	// The mix's colour at the brightest emitter's level: what MatchColorValues reproduces when this colour is written back.
	float Highest = 0.f;
	TArray<float, TInlineAllocator<5>> Levels;
	for (const FGameplayTag& Attribute : Fixture->GetColorAttributes())
	{
		FGameplayTag Stored = Attribute;
		if (Cell != INDEX_NONE)
		{
			const EStageCellChannel Channel = Attribute == StageCraftTags::Attribute_ColorW ? EStageCellChannel::ColorW
				: Attribute == StageCraftTags::Attribute_ColorA ? EStageCellChannel::ColorA
				: static_cast<EStageCellChannel>(static_cast<int32>(EStageCellChannel::ColorR) + Levels.Num());
			Stored = StageFixtureCells::GetCellAttribute(Cell, Channel);
		}
		Levels.Add(GetAttribute(Stored));
		Highest = FMath::Max(Highest, Levels.Last());
	}
	FLinearColor Display = StageColorScience::MixAdditive(Fixture->GetEmitterOutputs(), Levels).Color * Highest;
	Display.A = 1.f;
	return Display;
}

void ALightingFixtureActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);

	const ULightingFixtureData* Fixture = GetFixtureData();
	if (!Fixture)
	{
		return;
	}

	FStageParameterSection& PatchSection = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Patch, LOCTEXT("PatchSection", "Patch"));
	PatchSection.Add(StageCraftTags::Param_Patch_FixtureId, LOCTEXT("FixtureId", "Fixture ID"), FStageParameterValue::MakeInteger(FixtureId)).Range(1, 99999);
	PatchSection.Add(StageCraftTags::Param_Patch_Universe, LOCTEXT("Universe", "Universe"), FStageParameterValue::MakeInteger(Patch.Universe)).Range(0, 63999);
	PatchSection.Add(StageCraftTags::Param_Patch_Address, LOCTEXT("Address", "Address"), FStageParameterValue::MakeInteger(Patch.Address)).Range(0, 512);
	PatchSection.Add(StageCraftTags::Param_Patch_Mode, LOCTEXT("DMXMode", "DMX Mode"), FStageParameterValue::MakeText(FText::FromName(Fixture->DMXModeName))).ReadOnly();
	PatchSection.Add(StageCraftTags::Param_Patch_Footprint, LOCTEXT("Footprint", "Channels"),
		FStageParameterValue::MakeInteger(Fixture->GetDMXFootprint())).Display(1.0, LOCTEXT("ChannelUnits", "ch")).ReadOnly();

	auto AddAttribute = [this, Fixture](FStageParameterSection& Section, const FGameplayTag& Attribute, const FText& Name, double DisplayScale, const FText& Units, const FText& ShortName = FText::GetEmpty())
	{
		float Min = 0.f;
		float Max = 1.f;
		if (Fixture->GetAttributeRange(Attribute, Min, Max))
		{
			Section.Add(Attribute, Name, FStageParameterValue::MakeFloat(GetAttribute(Attribute))).Range(Min, Max).Display(DisplayScale, Units)
				.WithDefault(FStageParameterValue::MakeFloat(Fixture->GetAttributeDefault(Attribute))).Abbreviated(ShortName);
		}
	};
	// A wheel is picked by slot, so it is a dropdown; cues and DMX see the slot index.
	auto AddSlotAttribute = [this, Fixture](FStageParameterSection& Section, const FGameplayTag& Attribute, const FText& Name, TFunctionRef<FText(int32)> SlotName)
	{
		float Min = 0.f;
		float Max = 0.f;
		if (!Fixture->GetAttributeRange(Attribute, Min, Max))
		{
			return;
		}
		TArray<FText> Slots;
		for (int32 Slot = 0; Slot <= FMath::RoundToInt32(Max); ++Slot)
		{
			Slots.Add(SlotName(Slot));
		}
		Section.Add(Attribute, Name, FStageParameterValue::MakeEnum(FMath::RoundToInt32(GetAttribute(Attribute)))).WithOptions(MoveTemp(Slots));
	};

	const FText Percent = LOCTEXT("Percent", "%");
	const FText Degrees = LOCTEXT("Degrees", "°");
	const FText Rpm = LOCTEXT("Rpm", "rpm");

	FStageParameterSection& Dimmer = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Dimmer, LOCTEXT("DimmerSection", "Dimmer"));
	AddAttribute(Dimmer, StageCraftTags::Attribute_Dimmer, LOCTEXT("Dim", "Dimmer"), 100.0, Percent);
	AddAttribute(Dimmer, StageCraftTags::Attribute_Shutter, LOCTEXT("Shutter", "Strobe"), 100.0, Percent);

	if (Fixture->bHasPanTilt)
	{
		FStageParameterSection& Position = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Position, LOCTEXT("PositionSection", "Position"));
		AddAttribute(Position, StageCraftTags::Attribute_Pan, LOCTEXT("Pan", "Pan"), 1.0, Degrees);
		AddAttribute(Position, StageCraftTags::Attribute_Tilt, LOCTEXT("Tilt", "Tilt"), 1.0, Degrees);
	}

	const bool bHasColorRows = Fixture->AcceptsGelPreset() || Fixture->HasCellColor() || Fixture->bHasCTO || !Fixture->ColorWheel.IsEmpty();
	if (bHasColorRows)
	{
		FStageParameterSection& Color = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Color, LOCTEXT("ColorSection", "Color"));
		if (Fixture->SupportsAttribute(StageCraftTags::Attribute_ColorR))
		{
			Color.Add(StageCraftTags::Attribute_ColorRGB, Fixture->HasGelFrame() ? LOCTEXT("GelFrame", "Gel Frame") : LOCTEXT("ColorRGB", "Color"),
				FStageParameterValue::MakeColor(GetDisplayColor(INDEX_NONE)));
		}
		AddAttribute(Color, StageCraftTags::Attribute_ColorW, LOCTEXT("White", "White"), 100.0, Percent);
		AddAttribute(Color, StageCraftTags::Attribute_ColorA, LOCTEXT("Amber", "Amber"), 100.0, Percent);
		AddAttribute(Color, StageCraftTags::Attribute_ColorTemp, LOCTEXT("ColorTemp", "Color Temp"), 1.0, LOCTEXT("Kelvin", "K"), LOCTEXT("ColorTempShort", "CTC"));
		AddAttribute(Color, StageCraftTags::Attribute_CTO, LOCTEXT("CTO", "CTO"), 100.0, Percent);
		AddSlotAttribute(Color, StageCraftTags::Attribute_ColorWheel, LOCTEXT("ColorWheel", "Color Wheel"), [Fixture](int32 Slot) { return Fixture->GetColorWheelSlotName(Slot); });
		if (Fixture->AcceptsGelPreset() || Fixture->HasCellColor())
		{
			const TArray<FStageGel>& Gels = FStageGelLibrary::Get().GetGels();
			TArray<FText> GelOptions;
			GelOptions.Reserve(Gels.Num() + 1);
			GelOptions.Add(LOCTEXT("NoGel", "None"));
			for (const FStageGel& Gel : Gels)
			{
				GelOptions.Add(Gel.GetLabel());
			}
			FStageParameterValue GelValue;
			ReadParameter(StageCraftTags::Param_Fixture_Gel, GelValue);
			Color.Add(StageCraftTags::Param_Fixture_Gel, LOCTEXT("Gel", "Gel"), GelValue).WithOptions(MoveTemp(GelOptions));
		}
	}

	if (Fixture->HasCells())
	{
		FStageParameterSection& Cells = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Cells, LOCTEXT("CellsSection", "Cells"));
		if (Fixture->HasCellColor())
		{
			Cells.Add(StageCraftTags::Param_Fixture_AllCells, LOCTEXT("AllCells", "All Cells"), FStageParameterValue::MakeColor(GetBeamMix().Color));
		}
		for (int32 Cell = 0; Cell < Fixture->CellCount; ++Cell)
		{
			const FText CellName = FText::Format(LOCTEXT("CellName", "Cell {0}"), Cell + 1);
			if (Fixture->HasCellColor())
			{
				FStageParameterValue CellColor;
				ReadParameter(StageFixtureCells::GetCellAttribute(Cell, EStageCellChannel::ColorRGB), CellColor);
				Cells.Add(StageFixtureCells::GetCellAttribute(Cell, EStageCellChannel::ColorRGB), CellName, CellColor);
				AddAttribute(Cells, StageFixtureCells::GetCellAttribute(Cell, EStageCellChannel::ColorW), FText::Format(LOCTEXT("CellWhite", "Cell {0} White"), Cell + 1), 100.0, Percent);
				AddAttribute(Cells, StageFixtureCells::GetCellAttribute(Cell, EStageCellChannel::ColorA), FText::Format(LOCTEXT("CellAmber", "Cell {0} Amber"), Cell + 1), 100.0, Percent);
			}
			AddAttribute(Cells, StageFixtureCells::GetCellAttribute(Cell, EStageCellChannel::Dimmer),
				Fixture->HasCellColor() ? FText::Format(LOCTEXT("CellDimmer", "Cell {0} Dimmer"), Cell + 1) : CellName, 100.0, Percent);
		}
	}

	const bool bHasBeamRows = Fixture->HasZoom() || Fixture->bHasIris || Fixture->bHasFrost || !Fixture->GoboWheel.IsEmpty()
		|| !Fixture->StaticGoboWheel.IsEmpty() || Fixture->HasPrisms();
	if (bHasBeamRows)
	{
		FStageParameterSection& Beam = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Beam, LOCTEXT("BeamSection", "Beam"));
		AddAttribute(Beam, StageCraftTags::Attribute_Zoom, LOCTEXT("Zoom", "Zoom"), 1.0, Degrees);
		AddAttribute(Beam, StageCraftTags::Attribute_Frost, LOCTEXT("Frost", "Frost"), 100.0, Percent);
		AddAttribute(Beam, StageCraftTags::Attribute_Iris, LOCTEXT("Iris", "Iris"), 100.0, Percent);
		AddSlotAttribute(Beam, StageCraftTags::Attribute_Gobo, LOCTEXT("Gobo", "Gobo"), [Fixture](int32 Slot) { return Fixture->GetGoboSlotName(Slot, EStageGoboWheel::Rotating); });
		AddAttribute(Beam, StageCraftTags::Attribute_GoboRotation, LOCTEXT("GoboRotation", "Gobo Rotate"), 1.0, Degrees, LOCTEXT("GoboRotationShort", "Gobo Rot"));
		AddAttribute(Beam, StageCraftTags::Attribute_GoboSpin, LOCTEXT("GoboSpin", "Gobo Spin"), 1.0, Rpm);
		AddSlotAttribute(Beam, StageCraftTags::Attribute_Gobo2, LOCTEXT("Gobo2", "Gobo 2"), [Fixture](int32 Slot) { return Fixture->GetGoboSlotName(Slot, EStageGoboWheel::Static); });
		AddSlotAttribute(Beam, StageCraftTags::Attribute_Prism, LOCTEXT("Prism", "Prism"), [Fixture](int32 Slot) { return Fixture->GetPrismSlotName(Slot); });
		AddAttribute(Beam, StageCraftTags::Attribute_PrismRotation, LOCTEXT("PrismRotation", "Prism Spin"), 1.0, Rpm);
	}

	if (Fixture->HasBlades())
	{
		const bool bBarndoors = Fixture->BeamShaper == EStageBeamShaper::Barndoors;
		FStageParameterSection& Shapers = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Shapers,
			bBarndoors ? LOCTEXT("BarndoorSection", "Barndoors") : LOCTEXT("ShapersSection", "Shapers"));
		if (bBarndoors)
		{
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeA, LOCTEXT("DoorTop", "Door Top"), 100.0, Percent);
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeB, LOCTEXT("DoorRight", "Door Right"), 100.0, Percent);
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeC, LOCTEXT("DoorBottom", "Door Bottom"), 100.0, Percent);
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeD, LOCTEXT("DoorLeft", "Door Left"), 100.0, Percent);
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeRotation, LOCTEXT("DoorRotation", "Doors Rotate"), 1.0, Degrees);
		}
		else
		{
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeA, LOCTEXT("BladeA", "Blade Top"), 100.0, Percent, LOCTEXT("BladeAShort", "Blade T"));
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeB, LOCTEXT("BladeB", "Blade Right"), 100.0, Percent, LOCTEXT("BladeBShort", "Blade R"));
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeC, LOCTEXT("BladeC", "Blade Bottom"), 100.0, Percent, LOCTEXT("BladeCShort", "Blade B"));
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeD, LOCTEXT("BladeD", "Blade Left"), 100.0, Percent, LOCTEXT("BladeDShort", "Blade L"));
			AddAttribute(Shapers, StageCraftTags::Attribute_BladeRotation, LOCTEXT("BladeRotation", "Shaper Rotate"), 1.0, Degrees, LOCTEXT("BladeRotationShort", "Shp Rot"));
		}
	}

	// The model's catalog specs, read-only: what a rigging plot and a power plan need.
	FStageParameterSection& Specs = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Fixture, LOCTEXT("FixtureSection", "Fixture"));
	FStageParameterValue Text;
	const FGameplayTag SpecRows[] = { StageCraftTags::Param_Fixture_Kind, StageCraftTags::Param_Fixture_Dimensions, StageCraftTags::Param_Fixture_Flux };
	for (const FGameplayTag& Row : SpecRows)
	{
		ReadParameter(Row, Text);
		const FText Name = Row == StageCraftTags::Param_Fixture_Kind ? LOCTEXT("FixtureKind", "Type")
			: Row == StageCraftTags::Param_Fixture_Dimensions ? LOCTEXT("FixtureDimensions", "Size (W x D x H)")
			: LOCTEXT("FixtureFlux", "Output");
		Specs.Add(Row, Name, Text).ReadOnly();
	}

	// Approximate photometrics: read-only, refreshed by the same events as the attributes and position they follow.
	const StagePhotometry::FResult Photo = ComputePhotometrics();
	FStageParameterSection& Photometrics = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Photometrics, LOCTEXT("PhotometricsSection", "Photometrics"));
	Photometrics.Add(StageCraftTags::Param_Photo_BeamAngle, LOCTEXT("PhotoBeamAngle", "Beam Angle"), FStageParameterValue::MakeFloat(Photo.BeamAngleDegrees))
		.Display(1.0, Degrees).ReadOnly();
	Photometrics.Add(StageCraftTags::Param_Photo_FieldAngle, LOCTEXT("PhotoFieldAngle", "Field Angle"), FStageParameterValue::MakeFloat(ComputeConeAngles(*Fixture).FieldDegrees))
		.Display(1.0, Degrees).ReadOnly();
	Photometrics.Add(StageCraftTags::Param_Photo_Intensity, LOCTEXT("PhotoIntensity", "Intensity"), FStageParameterValue::MakeFloat(Photo.IntensityCd))
		.Display(0.001, LOCTEXT("Kilocandela", "kcd")).ReadOnly();
	Photometrics.Add(StageCraftTags::Param_Photo_Throw, LOCTEXT("PhotoThrow", "Throw"),
		FStageParameterValue::MakeText(FormatThrowDependent(Photo, StageCraftTags::Param_Photo_Throw))).ReadOnly();
	Photometrics.Add(StageCraftTags::Param_Photo_Illuminance, LOCTEXT("PhotoIlluminance", "Illuminance"),
		FStageParameterValue::MakeText(FormatThrowDependent(Photo, StageCraftTags::Param_Photo_Illuminance))).ReadOnly();
	Photometrics.Add(StageCraftTags::Param_Photo_BeamDiameter, LOCTEXT("PhotoBeamDiameter", "Beam Size"),
		FStageParameterValue::MakeText(FormatThrowDependent(Photo, StageCraftTags::Param_Photo_BeamDiameter))).ReadOnly();
}

bool ALightingFixtureActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if (ParameterId == StageCraftTags::Attribute_ColorRGB)
	{
		OutValue = FStageParameterValue::MakeColor(GetDisplayColor(INDEX_NONE));
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Fixture_AllCells)
	{
		OutValue = FStageParameterValue::MakeColor(GetBeamMix().Color);
		return Fixture && Fixture->HasCellColor();
	}
	if (ParameterId == StageCraftTags::Param_Fixture_Gel)
	{
		// Option 0 is "None"; the gels follow in library order.
		const int32 GelIndex = AppliedGel.IsNone() ? INDEX_NONE : FStageGelLibrary::Get().FindByCode(AppliedGel.ToString());
		OutValue = FStageParameterValue::MakeEnum(GelIndex + 1);
		return Fixture && (Fixture->AcceptsGelPreset() || Fixture->HasCellColor());
	}
	int32 Cell = INDEX_NONE;
	EStageCellChannel Channel = EStageCellChannel::Count;
	if (StageFixtureCells::ParseCellAttribute(ParameterId, Cell, Channel) && Channel == EStageCellChannel::ColorRGB)
	{
		const bool bValid = Fixture && Fixture->HasCellColor() && Cell < Fixture->CellCount;
		OutValue = FStageParameterValue::MakeColor(bValid ? GetDisplayColor(Cell) : FLinearColor::Black);
		return bValid;
	}
	if (IsSlotAttribute(ParameterId) && Attributes.Contains(ParameterId))
	{
		// Matches the dropdown row; the stored attribute is the same slot index as a float.
		OutValue = FStageParameterValue::MakeEnum(FMath::RoundToInt32(GetAttribute(ParameterId)));
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Fixture_Kind || ParameterId == StageCraftTags::Param_Fixture_Dimensions || ParameterId == StageCraftTags::Param_Fixture_Flux)
	{
		if (!Fixture)
		{
			return false;
		}
		FNumberFormattingOptions Whole;
		Whole.SetMaximumFractionalDigits(0);
		const FVector& Size = Fixture->Specs.DimensionsCm;
		OutValue = FStageParameterValue::MakeText(ParameterId == StageCraftTags::Param_Fixture_Kind ? UEnum::GetDisplayValueAsText(Fixture->FixtureKind)
			: ParameterId == StageCraftTags::Param_Fixture_Flux ? FText::Format(LOCTEXT("FluxValue", "{0} lm"), FText::AsNumber(Fixture->LuminousFlux, &Whole))
			: Fixture->Specs.HasDimensions() ? FText::Format(LOCTEXT("SizeValue", "{0} x {1} x {2} cm"), FText::AsNumber(Size.X, &Whole), FText::AsNumber(Size.Y, &Whole), FText::AsNumber(Size.Z, &Whole))
			: LOCTEXT("SizeUnknown", "Not specified"));
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Photo_FieldAngle)
	{
		OutValue = FStageParameterValue::MakeFloat(Fixture ? ComputeConeAngles(*Fixture).FieldDegrees : 0.0);
		return Fixture != nullptr;
	}
	if (StageCraftTags::IsPhotometricParameter(ParameterId))
	{
		const StagePhotometry::FResult Photo = ComputePhotometrics();
		OutValue = IsThrowDependentParameter(ParameterId)
			? FStageParameterValue::MakeText(FormatThrowDependent(Photo, ParameterId))
			: FStageParameterValue::MakeFloat(ParameterId == StageCraftTags::Param_Photo_BeamAngle ? Photo.BeamAngleDegrees : Photo.IntensityCd);
		return Fixture != nullptr;
	}
	if (ParameterId.MatchesTag(StageCraftTags::Attribute) && Attributes.Contains(ParameterId))
	{
		OutValue = FStageParameterValue::MakeFloat(GetAttribute(ParameterId));
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Patch_FixtureId)
	{
		OutValue = FStageParameterValue::MakeInteger(FixtureId);
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Patch_Universe)
	{
		OutValue = FStageParameterValue::MakeInteger(Patch.Universe);
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Patch_Address)
	{
		OutValue = FStageParameterValue::MakeInteger(Patch.Address);
		return true;
	}
	return Super::ReadParameter(ParameterId, OutValue);
}

bool ALightingFixtureActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	const ULightingFixtureData* Fixture = GetFixtureData();
	if ((ParameterId == StageCraftTags::Attribute_ColorRGB || ParameterId == StageCraftTags::Param_Fixture_AllCells) && Value.Type == EStageParameterType::Color)
	{
		// The colour is reproduced over every emitter (W and A included); on a multi-cell fixture it is every cell's colour.
		if (!Fixture || (ParameterId == StageCraftTags::Param_Fixture_AllCells && !Fixture->HasCellColor()))
		{
			return false;
		}
		const TMap<FGameplayTag, float> Values = MatchColorValues(Value.Color, INDEX_NONE);
		SetAttributes(Values);
		if (Fixture->HasCellColor())
		{
			NotifyParameterChanged(StageCraftTags::Param_Fixture_AllCells);
		}
		return !Values.IsEmpty();
	}
	if (ParameterId == StageCraftTags::Param_Fixture_Gel && Value.Type == EStageParameterType::Enum)
	{
		// "None" only clears the label; the colour stays as it is, as removing a gel from a console's palette would.
		if (Value.Integer <= 0)
		{
			ClearAppliedGel();
			return true;
		}
		return ApplyGel(Value.Integer - 1);
	}
	int32 Cell = INDEX_NONE;
	EStageCellChannel Channel = EStageCellChannel::Count;
	if (StageFixtureCells::ParseCellAttribute(ParameterId, Cell, Channel) && Channel == EStageCellChannel::ColorRGB && Value.Type == EStageParameterType::Color)
	{
		if (!Fixture || !Fixture->HasCellColor() || Cell >= Fixture->CellCount)
		{
			return false;
		}
		SetAttributes(MatchColorValues(Value.Color, Cell));
		return true;
	}
	if (IsSlotAttribute(ParameterId) && Value.Type == EStageParameterType::Enum && Attributes.Contains(ParameterId))
	{
		SetAttribute(ParameterId, static_cast<float>(Value.Integer));
		return true;
	}
	if (StageCraftTags::IsPhotometricParameter(ParameterId) || ParameterId == StageCraftTags::Param_Fixture_Kind
		|| ParameterId == StageCraftTags::Param_Fixture_Dimensions || ParameterId == StageCraftTags::Param_Fixture_Flux)
	{
		// Derived or catalog data, never written.
		return false;
	}
	if (ParameterId.MatchesTag(StageCraftTags::Attribute) && Value.Type == EStageParameterType::Float && Attributes.Contains(ParameterId))
	{
		SetAttribute(ParameterId, static_cast<float>(Value.Float));
		return true;
	}
	if (Value.Type == EStageParameterType::Integer)
	{
		if (ParameterId == StageCraftTags::Param_Patch_FixtureId)
		{
			return SetFixtureId(Value.Integer);
		}
		if (ParameterId == StageCraftTags::Param_Patch_Universe)
		{
			FStageDMXPatch NewPatch = Patch;
			NewPatch.Universe = Value.Integer;
			SetPatch(NewPatch);
			return true;
		}
		if (ParameterId == StageCraftTags::Param_Patch_Address)
		{
			FStageDMXPatch NewPatch = Patch;
			NewPatch.Address = Value.Integer;
			SetPatch(NewPatch);
			return true;
		}
	}
	return Super::WriteParameter(ParameterId, Value);
}

#undef LOCTEXT_NAMESPACE
