// Copyright Epic Games, Inc. All Rights Reserved.

#include "Startup/StageStartupSubsystem.h"

#include "Engine/GameInstance.h"
#include "Environment/StageAtmosphereSubsystem.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "TimerManager.h"
#include "Workspace/StageWorkspaceSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageStartupSubsystem)

#define LOCTEXT_NAMESPACE "StageStartup"

namespace StageStartup
{
	namespace Source
	{
		const FName Library(TEXT("Library"));
		const FName Panels(TEXT("Panels"));
		const FName Shop(TEXT("Shop"));
		const FName Atmospheres(TEXT("Atmospheres"));
	}

	float CombinedProgress(TConstArrayView<FStageStartupSource> Sources)
	{
		float TotalWeight = 0.f;
		float Done = 0.f;
		for (const FStageStartupSource& Source : Sources)
		{
			const float Weight = FMath::Max(Source.Weight, 0.f);
			TotalWeight += Weight;
			Done += Weight * (Source.State == EStageStartupSourceState::Loading ? FMath::Clamp(Source.Progress, 0.f, 1.f) : 1.f);
		}
		return TotalWeight > 0.f ? FMath::Clamp(Done / TotalWeight, 0.f, 1.f) : 1.f;
	}

	EStageStartupState CombinedState(TConstArrayView<FStageStartupSource> Sources, bool bTimedOut)
	{
		bool bAnyLoading = false;
		bool bAnyFailed = false;
		for (const FStageStartupSource& Source : Sources)
		{
			bAnyLoading |= Source.State == EStageStartupSourceState::Loading;
			bAnyFailed |= Source.State == EStageStartupSourceState::Failed;
		}
		if (bAnyLoading)
		{
			return bTimedOut ? EStageStartupState::TimedOut : EStageStartupState::Loading;
		}
		return bAnyFailed ? EStageStartupState::Failed : EStageStartupState::Ready;
	}

	const FStageStartupSource* FirstLoading(TConstArrayView<FStageStartupSource> Sources)
	{
		return Sources.FindByPredicate([](const FStageStartupSource& Source) { return Source.State == EStageStartupSourceState::Loading; });
	}
}

bool UStageStartupSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

void UStageStartupSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	BindCatalogSources(Collection);

	if (UGameInstance* GameInstance = GetGameInstance(); GameInstance && !IsSettled() && StartupTimeoutSeconds > 0.f)
	{
		GameInstance->GetTimerManager().SetTimer(TimeoutHandle, FTimerDelegate::CreateUObject(this, &ThisClass::HandleTimeout), StartupTimeoutSeconds, false);
	}
}

void UStageStartupSubsystem::Deinitialize()
{
	UnbindCatalogSources();
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(TimeoutHandle);
	}
	OnProgress.Clear();
	OnSettled.Clear();
	Sources.Reset();
	Super::Deinitialize();
}

void UStageStartupSubsystem::BindCatalogSources(FSubsystemCollectionBase& Collection)
{
	using namespace StageStartup;

	// Weights follow what each loads: the library carries every item's icon, the others a handful of small assets.
	if (UStageItemSubsystem* Items = Collection.InitializeDependency<UStageItemSubsystem>())
	{
		ItemSubsystem = Items;
		RegisterSource(Source::Library, LOCTEXT("SourceLibrary", "the library"), 6.f);
		if (Items->IsCatalogLoaded())
		{
			ReportReady(Source::Library);
		}
		else
		{
			ProgressBindings.Emplace(Source::Library, Items->OnCatalogLoadProgress.AddWeakLambda(this, [this](float Fraction) { ReportProgress(StageStartup::Source::Library, Fraction); }));
			Items->OnCatalogLoaded.AddUniqueDynamic(this, &ThisClass::HandleLibraryLoaded);
		}
	}

	if (UStageWorkspaceSubsystem* Workspace = Collection.InitializeDependency<UStageWorkspaceSubsystem>())
	{
		WorkspaceSubsystem = Workspace;
		RegisterSource(Source::Panels, LOCTEXT("SourcePanels", "the panels"), 2.f);
		if (Workspace->IsPanelCatalogLoaded())
		{
			ReportReady(Source::Panels);
		}
		else
		{
			ProgressBindings.Emplace(Source::Panels, Workspace->OnPanelCatalogLoadProgress.AddWeakLambda(this, [this](float Fraction) { ReportProgress(StageStartup::Source::Panels, Fraction); }));
			LoadedBindings.Emplace(Source::Panels, Workspace->OnPanelCatalogLoaded.AddWeakLambda(this, [this]() { ReportReady(StageStartup::Source::Panels); }));
		}
	}

	if (UStageEconomySubsystem* Economy = Collection.InitializeDependency<UStageEconomySubsystem>())
	{
		EconomySubsystem = Economy;
		RegisterSource(Source::Shop, LOCTEXT("SourceShop", "the shop"), 1.f);
		if (Economy->IsProductCatalogLoaded())
		{
			ReportReady(Source::Shop);
		}
		else
		{
			ProgressBindings.Emplace(Source::Shop, Economy->OnProductCatalogLoadProgress.AddWeakLambda(this, [this](float Fraction) { ReportProgress(StageStartup::Source::Shop, Fraction); }));
			Economy->OnProductCatalogLoaded.AddUniqueDynamic(this, &ThisClass::HandleShopLoaded);
		}
	}

	if (UStageAtmosphereSubsystem* Atmosphere = Collection.InitializeDependency<UStageAtmosphereSubsystem>())
	{
		AtmosphereSubsystem = Atmosphere;
		RegisterSource(Source::Atmospheres, LOCTEXT("SourceAtmospheres", "the atmospheres"), 1.f);
		if (Atmosphere->IsLoaded())
		{
			ReportReady(Source::Atmospheres);
		}
		else
		{
			ProgressBindings.Emplace(Source::Atmospheres, Atmosphere->OnLoadProgress.AddWeakLambda(this, [this](float Fraction) { ReportProgress(StageStartup::Source::Atmospheres, Fraction); }));
			LoadedBindings.Emplace(Source::Atmospheres, Atmosphere->OnAtmospheresLoaded.AddWeakLambda(this, [this]() { ReportReady(StageStartup::Source::Atmospheres); }));
		}
	}

	// Settles at once when everything was already loaded, or when there is nothing to wait for.
	Update();
}

void UStageStartupSubsystem::UnbindCatalogSources()
{
	using namespace StageStartup;

	for (const TPair<FName, FDelegateHandle>& Binding : ProgressBindings)
	{
		if (Binding.Key == Source::Library && ItemSubsystem.IsValid())
		{
			ItemSubsystem->OnCatalogLoadProgress.Remove(Binding.Value);
		}
		else if (Binding.Key == Source::Panels && WorkspaceSubsystem.IsValid())
		{
			WorkspaceSubsystem->OnPanelCatalogLoadProgress.Remove(Binding.Value);
		}
		else if (Binding.Key == Source::Shop && EconomySubsystem.IsValid())
		{
			EconomySubsystem->OnProductCatalogLoadProgress.Remove(Binding.Value);
		}
		else if (Binding.Key == Source::Atmospheres && AtmosphereSubsystem.IsValid())
		{
			AtmosphereSubsystem->OnLoadProgress.Remove(Binding.Value);
		}
	}
	for (const TPair<FName, FDelegateHandle>& Binding : LoadedBindings)
	{
		if (Binding.Key == Source::Panels && WorkspaceSubsystem.IsValid())
		{
			WorkspaceSubsystem->OnPanelCatalogLoaded.Remove(Binding.Value);
		}
		else if (Binding.Key == Source::Atmospheres && AtmosphereSubsystem.IsValid())
		{
			AtmosphereSubsystem->OnAtmospheresLoaded.Remove(Binding.Value);
		}
	}
	ProgressBindings.Reset();
	LoadedBindings.Reset();

	if (ItemSubsystem.IsValid())
	{
		ItemSubsystem->OnCatalogLoaded.RemoveDynamic(this, &ThisClass::HandleLibraryLoaded);
	}
	if (EconomySubsystem.IsValid())
	{
		EconomySubsystem->OnProductCatalogLoaded.RemoveDynamic(this, &ThisClass::HandleShopLoaded);
	}
}

void UStageStartupSubsystem::HandleLibraryLoaded()
{
	ReportReady(StageStartup::Source::Library);
}

void UStageStartupSubsystem::HandleShopLoaded()
{
	ReportReady(StageStartup::Source::Shop);
}

bool UStageStartupSubsystem::RegisterSource(FName Id, const FText& Label, float Weight)
{
	if (Id.IsNone() || IsSettled() || FindSource(Id))
	{
		return false;
	}
	FStageStartupSource& Source = Sources.AddDefaulted_GetRef();
	Source.Id = Id;
	Source.Label = Label;
	Source.Weight = FMath::Max(Weight, 0.f);
	return true;
}

FStageStartupSource* UStageStartupSubsystem::FindSource(FName Id)
{
	return Sources.FindByPredicate([Id](const FStageStartupSource& Source) { return Source.Id == Id; });
}

void UStageStartupSubsystem::ReportProgress(FName Id, float Fraction)
{
	FStageStartupSource* Source = FindSource(Id);
	if (!Source || Source->State != EStageStartupSourceState::Loading || IsSettled())
	{
		return;
	}
	const float Clamped = FMath::Clamp(FMath::IsFinite(Fraction) ? Fraction : 0.f, 0.f, 1.f);
	if (Clamped <= Source->Progress)
	{
		return; // never backwards, and no broadcast for nothing
	}
	Source->Progress = Clamped;
	Update();
}

void UStageStartupSubsystem::ReportReady(FName Id)
{
	FStageStartupSource* Source = FindSource(Id);
	if (!Source || Source->State != EStageStartupSourceState::Loading)
	{
		return;
	}
	Source->State = EStageStartupSourceState::Ready;
	Source->Progress = 1.f;
	Update();
}

void UStageStartupSubsystem::ReportFailed(FName Id, const FText& Reason)
{
	FStageStartupSource* Source = FindSource(Id);
	if (!Source || Source->State != EStageStartupSourceState::Loading)
	{
		return;
	}
	Source->State = EStageStartupSourceState::Failed;
	Source->FailureReason = Reason;
	UE_LOG(LogStageCraft, Warning, TEXT("Startup: %s failed to load: %s"), *Source->Label.ToString(), *Reason.ToString());
	Update();
}

void UStageStartupSubsystem::HandleTimeout()
{
	if (IsSettled())
	{
		return;
	}
	bTimedOut = true;
	const FStageStartupSource* Waiting = StageStartup::FirstLoading(Sources);
	UE_LOG(LogStageCraft, Warning, TEXT("Startup: still waiting for %s after %.0f s; the loader offers to continue."),
		Waiting ? *Waiting->Label.ToString() : TEXT("nothing"), StartupTimeoutSeconds);
	Update();
}

void UStageStartupSubsystem::Update()
{
	if (IsSettled())
	{
		return; // The state only moves forward; a late source no longer changes what the loader showed.
	}

	const EStageStartupState NewState = StageStartup::CombinedState(Sources, bTimedOut);
	OnProgress.Broadcast(GetProgress(), GetStatusText());
	if (NewState == EStageStartupState::Loading)
	{
		return;
	}

	State = NewState;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(TimeoutHandle);
	}
	UE_LOG(LogStageCraft, Log, TEXT("Startup settled: %s (%.0f%%)."), *GetStatusText().ToString(), GetProgress() * 100.f);
	OnSettled.Broadcast(State);
}

FText UStageStartupSubsystem::GetStatusText() const
{
	const EStageStartupState Combined = IsSettled() ? State : StageStartup::CombinedState(Sources, bTimedOut);
	switch (Combined)
	{
	case EStageStartupState::Ready:
		return LOCTEXT("StatusReady", "Ready");

	case EStageStartupState::Failed:
	{
		const FStageStartupSource* Failed = Sources.FindByPredicate([](const FStageStartupSource& Source) { return Source.State == EStageStartupSourceState::Failed; });
		return Failed
			? FText::Format(LOCTEXT("StatusFailed", "Couldn't load {0}: {1}"), Failed->Label, Failed->FailureReason)
			: LOCTEXT("StatusFailedUnknown", "Something couldn't load.");
	}

	case EStageStartupState::TimedOut:
	{
		const FStageStartupSource* Waiting = StageStartup::FirstLoading(Sources);
		return FText::Format(LOCTEXT("StatusTimedOut", "Still loading {0}. You can start now; it will finish in the background."),
			Waiting ? Waiting->Label : LOCTEXT("SomethingLabel", "something"));
	}

	case EStageStartupState::Loading:
	default:
	{
		const FStageStartupSource* Waiting = StageStartup::FirstLoading(Sources);
		return Waiting ? FText::Format(LOCTEXT("StatusLoading", "Loading {0}…"), Waiting->Label) : LOCTEXT("StatusStarting", "Starting…");
	}
	}
}

#undef LOCTEXT_NAMESPACE
