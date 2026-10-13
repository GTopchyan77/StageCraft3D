// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineRouteTimeSubsystem.h"
#include "SplineRouteTool.h"

void USplineRouteTimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	TimeScale = 1.0f;
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] TimeSubsystem initialised. TimeScale=1.0 MaxTimeScale=%.1f"), MaxTimeScale);
}

void USplineRouteTimeSubsystem::SetTimeScale(float NewScale)
{
	const float Clamped = FMath::Max(0.0f, NewScale);
	if (!FMath::IsNearlyEqual(Clamped, TimeScale))
	{
		const float Old = TimeScale;
		TimeScale = Clamped;
		OnTimeScaleChanged.Broadcast(TimeScale);
		UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] TimeScale %.3f -> %.3f"), Old, TimeScale);
	}
}
