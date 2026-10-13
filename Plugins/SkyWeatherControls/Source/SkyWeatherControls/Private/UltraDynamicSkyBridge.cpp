// Copyright DamenTools internal. Sky & Weather Controls plugin.

#include "UltraDynamicSkyBridge.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogSkyWeatherControls);

namespace UdsBridge
{
	// Object paths into the Ultra Dynamic Sky content pack. These are deliberately soft
	// (resolved by string at runtime) so the plugin still loads in a project without UDS.
	static const TCHAR* SkyFunctionLibraryPath =
		TEXT("/Game/UltraDynamicSky/Blueprints/Functions/UltraDynamicSky_Functions.UltraDynamicSky_Functions_C");
	static const TCHAR* WeatherFunctionLibraryPath =
		TEXT("/Game/UltraDynamicSky/Blueprints/Functions/UltraDynamicWeather_Functions.UltraDynamicWeather_Functions_C");
	static const TCHAR* SkyActorClassPath =
		TEXT("/Game/UltraDynamicSky/Blueprints/Ultra_Dynamic_Sky.Ultra_Dynamic_Sky_C");
	static const TCHAR* WeatherActorClassPath =
		TEXT("/Game/UltraDynamicSky/Blueprints/Ultra_Dynamic_Weather.Ultra_Dynamic_Weather_C");
	static const TCHAR* WeatherPresetFolder =
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/");

	/**
	 * UDS names its pins for humans ("Transition Duration (Seconds)"), so compare on
	 * alphanumerics only -- that survives spaces, casing and punctuation drifting
	 * between UDS releases.
	 */
	static FString Normalize(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (const TCHAR Char : In)
		{
			if (FChar::IsAlnum(Char))
			{
				Out.AppendChar(FChar::ToLower(Char));
			}
		}
		return Out;
	}

	static UClass* ResolveClass(const TCHAR* ObjectPath)
	{
		// Cached because this runs on every dropdown change; LoadObject is cheap once the
		// package is resident but still walks the object hash each call.
		static TMap<FString, TWeakObjectPtr<UClass>> Cache;

		if (const TWeakObjectPtr<UClass>* Existing = Cache.Find(ObjectPath))
		{
			if (Existing->IsValid())
			{
				return Existing->Get();
			}
		}

		UClass* Loaded = LoadObject<UClass>(nullptr, ObjectPath);
		if (Loaded == nullptr)
		{
			UE_LOG(LogSkyWeatherControls, Warning,
				TEXT("Ultra Dynamic Sky asset not found: %s. Is the UDS content pack present?"), ObjectPath);
			return nullptr;
		}

		Cache.Add(ObjectPath, Loaded);
		return Loaded;
	}

	/**
	 * UDS brands its library nodes by appending a separator plus a stylised suffix to the
	 * function name, and that suffix is not stable across releases. So resolve on the
	 * normalized name: exact match first, then the *shortest* name that starts with what we
	 * asked for -- otherwise "Get Time of Day" could bind to "Get Time of Day as String".
	 */
	static UFunction* ResolveFunction(UClass* LibraryClass, const TCHAR* FunctionName)
	{
		if (UFunction* Exact = LibraryClass->FindFunctionByName(FName(FunctionName)))
		{
			return Exact;
		}

		const FString Wanted = Normalize(FunctionName);
		UFunction* Best = nullptr;
		int32 BestLength = MAX_int32;
		for (TFieldIterator<UFunction> It(LibraryClass); It; ++It)
		{
			const FString Candidate = Normalize(It->GetName());
			if (Candidate == Wanted)
			{
				return *It;
			}
			if (Candidate.StartsWith(Wanted) && Candidate.Len() < BestLength)
			{
				Best = *It;
				BestLength = Candidate.Len();
			}
		}
		return Best;
	}

	/**
	 * One reflected call into a UDS Blueprint function library. Owns the parameter block
	 * for the lifetime of the call and tears it down properly, since parameters are not
	 * guaranteed to be trivially destructible.
	 */
	class FLibraryCall
	{
	public:
		FLibraryCall(const TCHAR* LibraryClassPath, const TCHAR* FunctionName)
		{
			UClass* LibraryClass = ResolveClass(LibraryClassPath);
			if (LibraryClass == nullptr)
			{
				return;
			}

			Function = ResolveFunction(LibraryClass, FunctionName);
			if (Function == nullptr)
			{
				UE_LOG(LogSkyWeatherControls, Warning,
					TEXT("%s has no function matching '%s'. UDS may have renamed it."),
					*LibraryClass->GetName(), FunctionName);
				return;
			}
			UE_LOG(LogSkyWeatherControls, Verbose, TEXT("Resolved '%s' -> %s"), FunctionName, *DescribeSignature());

			// Blueprint function libraries are static: the VM calls them on the class
			// default object, so that is the context we invoke with too.
			Target = LibraryClass->GetDefaultObject();

			Params = static_cast<uint8*>(
				FMemory::Malloc(FMath::Max<int32>(Function->ParmsSize, 1), Function->GetMinAlignment()));
			FMemory::Memzero(Params, Function->ParmsSize);
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->InitializeValue_InContainer(Params);
			}
		}

		~FLibraryCall()
		{
			if (Params != nullptr)
			{
				if (Function != nullptr)
				{
					for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
					{
						It->DestroyValue_InContainer(Params);
					}
				}
				FMemory::Free(Params);
				Params = nullptr;
			}
		}

		FLibraryCall(const FLibraryCall&) = delete;
		FLibraryCall& operator=(const FLibraryCall&) = delete;

		bool IsValid() const { return Function != nullptr && Target != nullptr && Params != nullptr; }

		bool SetObject(std::initializer_list<const TCHAR*> NameCandidates, UObject* Value)
		{
			FObjectPropertyBase* Prop = FindParam<FObjectPropertyBase>(NameCandidates);
			if (Prop == nullptr)
			{
				return false;
			}
			Prop->SetObjectPropertyValue_InContainer(Params, Value);
			Assigned.Add(Prop);
			return true;
		}

		bool SetScalar(std::initializer_list<const TCHAR*> NameCandidates, double Value)
		{
			FNumericProperty* Prop = FindParam<FNumericProperty>(NameCandidates);
			if (Prop == nullptr)
			{
				return false;
			}

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Params);
			if (Prop->IsFloatingPoint())
			{
				Prop->SetFloatingPointPropertyValue(ValuePtr, Value);
			}
			else
			{
				Prop->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value));
			}
			Assigned.Add(Prop);
			return true;
		}

		bool SetBool(std::initializer_list<const TCHAR*> NameCandidates, bool Value)
		{
			FBoolProperty* Prop = FindParam<FBoolProperty>(NameCandidates);
			if (Prop == nullptr)
			{
				return false;
			}
			Prop->SetPropertyValue_InContainer(Params, Value);
			Assigned.Add(Prop);
			return true;
		}

		bool Invoke()
		{
			if (!IsValid())
			{
				return false;
			}
			Target->ProcessEvent(Function, Params);
			return true;
		}

		/**
		 * Reads a numeric result. A Blueprint function's single output is not always flagged
		 * CPF_ReturnParm -- UDS exposes "Get Time of Day" as a plain out-param -- so accept
		 * either, preferring an explicit return, then a name match, then any numeric output.
		 */
		bool GetScalarOut(std::initializer_list<const TCHAR*> NameCandidates, double& OutValue) const
		{
			if (Function == nullptr)
			{
				return false;
			}

			TArray<FString> Normalized;
			for (const TCHAR* Candidate : NameCandidates)
			{
				Normalized.Add(Normalize(Candidate));
			}

			const FNumericProperty* Fallback = nullptr;
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				const FNumericProperty* Numeric = CastField<FNumericProperty>(*It);
				if (Numeric == nullptr || Assigned.Contains(*It))
				{
					continue;
				}

				const bool bIsOutput = It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm);
				if (!bIsOutput)
				{
					continue;
				}

				if (It->HasAnyPropertyFlags(CPF_ReturnParm) || Normalized.Contains(Normalize(It->GetName())))
				{
					OutValue = ReadNumeric(*Numeric);
					return true;
				}
				if (Fallback == nullptr)
				{
					Fallback = Numeric;
				}
			}

			if (Fallback != nullptr)
			{
				OutValue = ReadNumeric(*Fallback);
				return true;
			}
			return false;
		}

		/** Human-readable pin list, logged whenever a parameter cannot be matched. */
		FString DescribeSignature() const
		{
			if (Function == nullptr)
			{
				return TEXT("<unresolved>");
			}

			TArray<FString> Pins;
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				Pins.Add(FString::Printf(TEXT("%s %s"), *It->GetCPPType(), *It->GetName()));
			}
			return FString::Printf(TEXT("%s(%s)"), *Function->GetName(), *FString::Join(Pins, TEXT(", ")));
		}

	private:
		double ReadNumeric(const FNumericProperty& Numeric) const
		{
			const void* ValuePtr = Numeric.ContainerPtrToValuePtr<void>(Params);
			return Numeric.IsFloatingPoint()
				? Numeric.GetFloatingPointPropertyValue(ValuePtr)
				: static_cast<double>(Numeric.GetSignedIntPropertyValue(ValuePtr));
		}

		/**
		 * Match on pin name first; if UDS has renamed the pin, fall back to the first
		 * still-unassigned parameter of the requested type. Callers assign in pin order,
		 * which keeps that fallback predictable.
		 */
		template <typename PropertyType>
		PropertyType* FindParam(std::initializer_list<const TCHAR*> NameCandidates) const
		{
			if (Function == nullptr)
			{
				return nullptr;
			}

			TArray<FString> Normalized;
			for (const TCHAR* Candidate : NameCandidates)
			{
				Normalized.Add(Normalize(Candidate));
			}

			PropertyType* TypeFallback = nullptr;
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_ReturnParm) || Assigned.Contains(*It))
				{
					continue;
				}

				PropertyType* Typed = CastField<PropertyType>(*It);
				if (Typed == nullptr)
				{
					continue;
				}

				if (Normalized.Contains(Normalize(It->GetName())))
				{
					return Typed;
				}
				if (TypeFallback == nullptr)
				{
					TypeFallback = Typed;
				}
			}

			if (TypeFallback == nullptr)
			{
				UE_LOG(LogSkyWeatherControls, Warning, TEXT("No parameter matched [%s] in %s"),
					*FString::Join(Normalized, TEXT(", ")), *DescribeSignature());
			}
			return TypeFallback;
		}

		UObject* Target = nullptr;
		UFunction* Function = nullptr;
		uint8* Params = nullptr;
		TSet<const FProperty*> Assigned;
	};

	static UWorld* ResolveWorld(const UObject* WorldContextObject)
	{
		return GEngine != nullptr
			? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
			: nullptr;
	}

	static bool HasActorOfClass(const UObject* WorldContextObject, const TCHAR* ClassPath)
	{
		const UWorld* World = ResolveWorld(WorldContextObject);
		UClass* ActorClass = ResolveClass(ClassPath);
		if (World == nullptr || ActorClass == nullptr)
		{
			return false;
		}
		return UGameplayStatics::GetActorOfClass(World, ActorClass) != nullptr;
	}
}

bool UUltraDynamicSkyBridge::IsSkyPresent(const UObject* WorldContextObject)
{
	return UdsBridge::HasActorOfClass(WorldContextObject, UdsBridge::SkyActorClassPath);
}

bool UUltraDynamicSkyBridge::IsWeatherPresent(const UObject* WorldContextObject)
{
	return UdsBridge::HasActorOfClass(WorldContextObject, UdsBridge::WeatherActorClassPath);
}

bool UUltraDynamicSkyBridge::TransitionTimeOfDay(const UObject* WorldContextObject, float NewTimeOfDay, float TransitionSeconds)
{
	UWorld* World = UdsBridge::ResolveWorld(WorldContextObject);
	if (World == nullptr)
	{
		return false;
	}

	UdsBridge::FLibraryCall Call(UdsBridge::SkyFunctionLibraryPath, TEXT("Transition Time of Day"));
	if (!Call.IsValid())
	{
		return false;
	}

	Call.SetObject({ TEXT("WorldContextObject"), TEXT("__WorldContext") }, World);
	Call.SetScalar({ TEXT("New Time of Day") }, FMath::Fmod(FMath::Max(NewTimeOfDay, 0.0f), 2400.0f));
	Call.SetScalar({ TEXT("Transition Duration (Seconds)") }, FMath::Max(TransitionSeconds, 0.0f));

	// ProcessEvent starts from a zeroed parameter block, so the defaults UDS authored on the
	// node itself are not applied -- they have to be restated here. Without this, "Allow Time
	// Going Backwards" arrives as false and picking an earlier hour runs forward through the
	// whole day instead of blending straight to it.
	Call.SetBool({ TEXT("Allow Time Going Backwards") }, true);
	Call.SetScalar({ TEXT("Easing Exponent") }, 2.0);

	return Call.Invoke();
}

float UUltraDynamicSkyBridge::GetTimeOfDay(const UObject* WorldContextObject)
{
	UWorld* World = UdsBridge::ResolveWorld(WorldContextObject);
	if (World == nullptr)
	{
		return -1.0f;
	}

	UdsBridge::FLibraryCall Call(UdsBridge::SkyFunctionLibraryPath, TEXT("Get Time of Day"));
	if (!Call.IsValid())
	{
		return -1.0f;
	}

	Call.SetObject({ TEXT("WorldContextObject"), TEXT("__WorldContext") }, World);
	if (!Call.Invoke())
	{
		return -1.0f;
	}

	double Result = -1.0;
	return Call.GetScalarOut({ TEXT("Time of Day") }, Result) ? static_cast<float>(Result) : -1.0f;
}

bool UUltraDynamicSkyBridge::ChangeWeather(const UObject* WorldContextObject, FName PresetName, float TransitionSeconds)
{
	UWorld* World = UdsBridge::ResolveWorld(WorldContextObject);
	if (World == nullptr || PresetName.IsNone())
	{
		return false;
	}

	// Weather presets are data-asset instances of UDS_Weather_Settings_C, so the object
	// path is Folder/Name.Name -- there is no generated "_C" class to load here.
	const FString PresetString = PresetName.ToString();
	const FString PresetPath = FString::Printf(TEXT("%s%s.%s"), UdsBridge::WeatherPresetFolder, *PresetString, *PresetString);

	UObject* Preset = LoadObject<UObject>(nullptr, *PresetPath);
	if (Preset == nullptr)
	{
		UE_LOG(LogSkyWeatherControls, Warning, TEXT("Weather preset not found: %s"), *PresetPath);
		return false;
	}

	UdsBridge::FLibraryCall Call(UdsBridge::WeatherFunctionLibraryPath, TEXT("Change Weather"));
	if (!Call.IsValid())
	{
		return false;
	}

	Call.SetObject({ TEXT("WorldContextObject"), TEXT("__WorldContext") }, World);
	Call.SetObject({ TEXT("New Weather Type") }, Preset);
	Call.SetScalar({ TEXT("Time To Transition To New Weather (Seconds)"), TEXT("Change Duration") },
		FMath::Max(TransitionSeconds, 0.0f));

	return Call.Invoke();
}
