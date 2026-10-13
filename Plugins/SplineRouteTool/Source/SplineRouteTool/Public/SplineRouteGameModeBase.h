// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "SplineRouteGameModeBase.generated.h"

class UUserWidget;

/**
 * Minimal game mode for the spline-route demo map.
 * Spawns the HUD widget on BeginPlay so the time-scale slider is always visible.
 */
UCLASS()
class SPLINEROUTETOOL_API ASplineRouteGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	ASplineRouteGameModeBase();

	/** HUD widget class to spawn on BeginPlay (set to WBP_SplineRouteHUD in defaults). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD")
	TSubclassOf<UUserWidget> HUDWidgetClass;

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> HUDWidgetInstance;
};
