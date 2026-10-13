// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineRouteGameModeBase.h"
#include "SplineRoutePlayerController.h"
#include "SplineRouteTool.h"

#include "Blueprint/UserWidget.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

ASplineRouteGameModeBase::ASplineRouteGameModeBase()
{
	PlayerControllerClass = ASplineRoutePlayerController::StaticClass();
}

void ASplineRouteGameModeBase::BeginPlay()
{
	Super::BeginPlay();

	if (!HUDWidgetClass)
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("ASplineRouteGameModeBase: no HUDWidgetClass assigned."));
		return;
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC)
	{
		return;
	}

	HUDWidgetInstance = CreateWidget<UUserWidget>(PC, HUDWidgetClass);
	if (HUDWidgetInstance)
	{
		HUDWidgetInstance->AddToViewport(0);
	}
}
