// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/StageSpanActor.h"

#include "Components/StageSpanGeometryComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/StageSpanData.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSpanActor)

#define LOCTEXT_NAMESPACE "StageCraftStageSpanActor"

namespace StageSpanActor
{
	constexpr double CmToMetres = 0.01;

	/** Inspector drag step for stretched runs and sag, in metres. Whole-segment runs step by one segment. */
	constexpr double StretchStepMetres = 0.1;
	constexpr double SagStepMetres = 0.05;
}

AStageSpanActor::AStageSpanActor()
{
	Geometry = CreateDefaultSubobject<UStageSpanGeometryComponent>(TEXT("Geometry"));
	Geometry->SetupAttachment(MeshComponent);
	Geometry->SetUsage(EStageSpanGeometryUsage::PlacedItem);
}

UStageSpanData* AStageSpanActor::GetSpanData() const
{
	return Cast<UStageSpanData>(ItemData);
}

FStageSpanPathLayout AStageSpanActor::ComputePathFor(TConstArrayView<FVector> Points, bool bClosed, double Sag) const
{
	const UStageSpanData* Span = GetSpanData();
	return Span ? StageSpanMath::ComputePathLayout(StageSpanMath::MakeRules(*Span), Points, bClosed, Sag) : FStageSpanPathLayout();
}

FStageSpanPathLayout AStageSpanActor::GetPathLayout() const
{
	return BuiltLayout;
}

bool AStageSpanActor::IsSingleLeg() const
{
	return BuiltLayout.Legs.Num() == 1 && !BuiltLayout.bClosed;
}

TArray<FVector> AStageSpanActor::MakeSingleLegPoints(double InLength) const
{
	// Keeps the leg's direction (local +X for a fresh or migrated run).
	const FVector Direction = BuiltLayout.Points.Num() >= 2 ? (BuiltLayout.Points[1] - BuiltLayout.Points[0]).GetSafeNormal() : FVector::ForwardVector;
	const FVector Start = BuiltLayout.Points.IsEmpty() ? FVector::ZeroVector : BuiltLayout.Points[0];
	return { Start, Start + (Direction.IsNearlyZero() ? FVector::ForwardVector : Direction) * InLength };
}

int32 AStageSpanActor::GetQuantity() const
{
	return BuiltLayout.IsValid() ? BuiltLayout.GetSegmentCount() : Super::GetQuantity();
}

int32 AStageSpanActor::PredictQuantity(const FGameplayTag& ParameterId, const FStageParameterValue& Value) const
{
	if (ParameterId == StageCraftTags::Param_Span_Length && Value.Type == EStageParameterType::Float && IsSingleLeg())
	{
		const FStageSpanPathLayout Layout = ComputePathFor(MakeSingleLegPoints(Value.Float), false, SagDepth);
		return Layout.IsValid() ? Layout.GetSegmentCount() : GetQuantity();
	}
	return Super::PredictQuantity(ParameterId, Value);
}

int32 AStageSpanActor::GetGeometryInstanceCount() const
{
	return Geometry->GetInstanceCount();
}

void AStageSpanActor::InitializePath(TConstArrayView<FVector> LocalPoints, bool bClosed)
{
	if (!ensureMsgf(!HasActorBegunPlay(), TEXT("%s: InitializePath is for spans being spawned; edit placed spans through their parameters."), *GetName()))
	{
		return;
	}
	// Stored as drawn; ApplyItemData normalises it once the item is known (spawning sets the item after this).
	PathPoints = TArray<FVector>(LocalPoints);
	bClosedPath = bClosed;
	SpanLength = 0.0;
	if (GetSpanData())
	{
		NormalisePath();
	}
}

void AStageSpanActor::NormalisePath()
{
	if (PathPoints.Num() < 2)
	{
		// A straight run from a scene file written before paths, or a fresh actor: one leg along +X.
		const double Length = FMath::IsFinite(SpanLength) ? FMath::Max(0.0, SpanLength) : 0.0;
		PathPoints = { FVector::ZeroVector, FVector(Length, 0.0, 0.0) };
		bClosedPath = false;
	}
	SpanLength = 0.0;

	FStageSpanPathLayout Layout = ComputePathFor(PathPoints, bClosedPath, SagDepth);
	if (!Layout.IsValid() && GetSpanData())
	{
		// Too short or damaged: the smallest run the item allows, rather than an invisible item that still bills nothing.
		Layout = ComputePathFor({ FVector::ZeroVector, FVector::ForwardVector }, false, 0.0);
	}
	ApplyPath(Layout);
}

void AStageSpanActor::ApplyPath(const FStageSpanPathLayout& Layout)
{
	if (Layout.IsValid())
	{
		PathPoints = Layout.Points;
		bClosedPath = Layout.bClosed;
		SagDepth = Layout.SagDepth;
	}
	BuiltLayout = Layout;
	RebuildGeometry();
}

void AStageSpanActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);

	// A swapped item has its own segment length and limits; a loaded path may be out of range. Both normalise here.
	NormalisePath();
}

void AStageSpanActor::RebuildGeometry()
{
	Geometry->Build(GetSpanData(), BuiltLayout);
	// Parts created by this build start without an overlay.
	Geometry->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageSpanActor::UpdateHighlight()
{
	Super::UpdateHighlight();
	Geometry->SetOverlayMaterial(GetHighlightOverlay());
}

void AStageSpanActor::NotifyShapeChanged()
{
	// Every derived row follows the shape; each refreshes from its own read.
	NotifyParameterChanged(StageCraftTags::Param_Span_Length);
	NotifyParameterChanged(StageCraftTags::Param_Span_Segments);
	NotifyParameterChanged(StageCraftTags::Param_Span_SegmentLength);
	NotifyParameterChanged(StageCraftTags::Param_Span_Legs);
	NotifyParameterChanged(StageCraftTags::Param_Span_Sag);
}

void AStageSpanActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);

	const UStageSpanData* Span = GetSpanData();
	if (!Span || !BuiltLayout.IsValid())
	{
		return;
	}

	using namespace StageSpanActor;
	const StageSpanMath::FRules Rules = StageSpanMath::MakeRules(*Span);
	const bool bWholeSegments = Span->Fit == EStageSpanFit::WholeSegments;
	const FText Metres = LOCTEXT("Metres", "m");

	FStageParameterSection& Section = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Span, LOCTEXT("SpanSection", "Span"));
	FStageParameterDescriptor& Length = Section.Add(StageCraftTags::Param_Span_Length, LOCTEXT("Length", "Length"), FStageParameterValue::MakeFloat(BuiltLayout.GetLength()))
		.Display(CmToMetres, Metres, bWholeSegments ? Span->SegmentLength * CmToMetres : StretchStepMetres);
	if (IsSingleLeg())
	{
		Length.Range(StageSpanMath::GetMinLength(Rules), StageSpanMath::GetMaxLength(Rules));
	}
	else
	{
		// A path's legs keep their own lengths; it is reshaped by drawing it again, so its total is read-only.
		Length.ReadOnly();
	}
	Section.Add(StageCraftTags::Param_Span_Legs, LOCTEXT("Legs", "Legs"),
		FStageParameterValue::MakeInteger(BuiltLayout.Legs.Num())).ReadOnly();
	Section.Add(StageCraftTags::Param_Span_Segments, LOCTEXT("Segments", "Segments"), FStageParameterValue::MakeInteger(BuiltLayout.GetSegmentCount())).ReadOnly();
	Section.Add(StageCraftTags::Param_Span_SegmentLength, LOCTEXT("SegmentLength", "Segment"), FStageParameterValue::MakeFloat(Span->SegmentLength))
		.Display(CmToMetres, Metres).ReadOnly();
	Section.Add(StageCraftTags::Param_Span_Fit, LOCTEXT("Fit", "Fit"),
		FStageParameterValue::MakeText(StaticEnum<EStageSpanFit>()->GetDisplayNameTextByValue(int64(Span->Fit)))).ReadOnly();

	if (BuiltLayout.MaxSagFraction > 0.0)
	{
		// The deepest sag any leg can take; shorter legs are limited further by their own length.
		double LongestLeg = 0.0;
		for (int32 Leg = 0; Leg < BuiltLayout.Legs.Num(); ++Leg)
		{
			LongestLeg = FMath::Max(LongestLeg, FVector::Dist(BuiltLayout.GetLegStart(Leg), BuiltLayout.GetLegEnd(Leg)));
		}
		Section.Add(StageCraftTags::Param_Span_Sag, LOCTEXT("Sag", "Sag"), FStageParameterValue::MakeFloat(SagDepth))
			.Range(0.0, BuiltLayout.MaxSagFraction * LongestLeg)
			.Display(CmToMetres, Metres, SagStepMetres)
			.WithDefault(FStageParameterValue::MakeFloat(0.0));
	}
}

bool AStageSpanActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Param_Span_Length)
	{
		OutValue = FStageParameterValue::MakeFloat(BuiltLayout.GetLength());
		return BuiltLayout.IsValid();
	}
	if (ParameterId == StageCraftTags::Param_Span_Segments)
	{
		OutValue = FStageParameterValue::MakeInteger(BuiltLayout.GetSegmentCount());
		return BuiltLayout.IsValid();
	}
	if (ParameterId == StageCraftTags::Param_Span_Legs)
	{
		OutValue = FStageParameterValue::MakeInteger(BuiltLayout.Legs.Num());
		return BuiltLayout.IsValid();
	}
	if (ParameterId == StageCraftTags::Param_Span_SegmentLength)
	{
		const UStageSpanData* Span = GetSpanData();
		OutValue = FStageParameterValue::MakeFloat(Span ? Span->SegmentLength : 0.0);
		return Span != nullptr;
	}
	if (ParameterId == StageCraftTags::Param_Span_Sag)
	{
		OutValue = FStageParameterValue::MakeFloat(SagDepth);
		return BuiltLayout.MaxSagFraction > 0.0;
	}
	return Super::ReadParameter(ParameterId, OutValue);
}

bool AStageSpanActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	if (ParameterId == StageCraftTags::Param_Span_Length)
	{
		// Only a straight one-leg run is resized by length; a path is reshaped by drawing it.
		if (Value.Type != EStageParameterType::Float || !FMath::IsFinite(Value.Float) || !GetSpanData() || !IsSingleLeg())
		{
			return false;
		}
		ApplyPath(ComputePathFor(MakeSingleLegPoints(Value.Float), false, SagDepth));
		NotifyShapeChanged();
		return true;
	}
	if (ParameterId == StageCraftTags::Param_Span_Sag)
	{
		if (Value.Type != EStageParameterType::Float || !FMath::IsFinite(Value.Float) || !(BuiltLayout.MaxSagFraction > 0.0))
		{
			return false;
		}
		ApplyPath(ComputePathFor(PathPoints, bClosedPath, Value.Float));
		NotifyShapeChanged();
		return true;
	}
	return Super::WriteParameter(ParameterId, Value);
}

#undef LOCTEXT_NAMESPACE
