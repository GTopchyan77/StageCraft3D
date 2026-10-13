// Copyright DamenTools internal. Sky & Weather Controls plugin.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UltraDynamicSkyBridge.generated.h"

SKYWEATHERCONTROLS_API DECLARE_LOG_CATEGORY_EXTERN(LogSkyWeatherControls, Log, All);

/**
 * Thin bridge to the Ultra Dynamic Sky / Ultra Dynamic Weather Blueprint function
 * libraries that ship under /Game/UltraDynamicSky.
 *
 * UDS is a Blueprint-only product, so there is no header to link against. Instead of
 * generating C++ stubs against its generated classes -- which go stale on every UDS
 * update -- we resolve its two public function libraries by object path at runtime and
 * invoke them through UFunction reflection, matching parameters by name with a
 * type-based fallback. Everything here fails softly: if UDS is missing from the project
 * or renames a pin, the call returns false and logs the signature it actually found.
 */
UCLASS()
class SKYWEATHERCONTROLS_API UUltraDynamicSkyBridge : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** True when an Ultra_Dynamic_Sky actor exists in the world. */
	UFUNCTION(BlueprintPure, Category = "Ultra Dynamic Sky", meta = (WorldContext = "WorldContextObject"))
	static bool IsSkyPresent(const UObject* WorldContextObject);

	/** True when an Ultra_Dynamic_Weather actor exists in the world. */
	UFUNCTION(BlueprintPure, Category = "Ultra Dynamic Sky", meta = (WorldContext = "WorldContextObject"))
	static bool IsWeatherPresent(const UObject* WorldContextObject);

	/**
	 * Blends Ultra Dynamic Sky to a new Time of Day.
	 * @param NewTimeOfDay      Military time x 100, i.e. 0 - 2400 (1330 == 1:30pm).
	 * @param TransitionSeconds Blend duration; 0 snaps instantly.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ultra Dynamic Sky", meta = (WorldContext = "WorldContextObject"))
	static bool TransitionTimeOfDay(const UObject* WorldContextObject, float NewTimeOfDay, float TransitionSeconds = 2.0f);

	/** Current Time of Day (0 - 2400), or -1 when Ultra Dynamic Sky is unavailable. */
	UFUNCTION(BlueprintPure, Category = "Ultra Dynamic Sky", meta = (WorldContext = "WorldContextObject"))
	static float GetTimeOfDay(const UObject* WorldContextObject);

	/**
	 * Blends Ultra Dynamic Weather to one of its shipped weather presets.
	 * @param PresetName        Asset name under Blueprints/Weather_Effects/Weather_Presets,
	 *                          e.g. "Rain_Thunderstorm".
	 * @param TransitionSeconds Blend duration; 0 snaps instantly.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ultra Dynamic Sky", meta = (WorldContext = "WorldContextObject"))
	static bool ChangeWeather(const UObject* WorldContextObject, FName PresetName, float TransitionSeconds = 5.0f);
};
