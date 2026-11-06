// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageKnowledgeSubsystem.h"

#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageItemSubsystem.h"
#include "UI/StageUIAnchors.h"
#include "Workspace/StageWorkspaceSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageKnowledgeSubsystem)

namespace StageKnowledgeSources
{
	/** One fixture attribute as the assistant explains it. Several tags that are one feature to a user share an entry. */
	struct FAttributeInfo
	{
		FGameplayTag Tag;
		const TCHAR* Name;
		const TCHAR* Meaning;
		/** Unit after a model's range ("°"), or null when the range means nothing to a user (0..1 levels, wheel slots). */
		const TCHAR* RangeUnit;
		TArray<FGameplayTag> AlsoTags;
	};

	TArray<FAttributeInfo> MakeAttributeTable()
	{
		using namespace StageCraftTags;
		return {
			{ Attribute_Dimmer, TEXT("Dimmer (intensity)"), TEXT("How bright the fixture is, 0-100 %. The Grand Master, a playback fader and BLACKOUT scale it last."), nullptr, {} },
			{ Attribute_Pan, TEXT("Pan"), TEXT("Horizontal turn of a moving head's yoke, in degrees either side of home."), TEXT("°"), {} },
			{ Attribute_Tilt, TEXT("Tilt"), TEXT("Vertical turn of a moving head's head, in degrees either side of home."), TEXT("°"), {} },
			{ Attribute_Zoom, TEXT("Zoom (beam angle)"), TEXT("The full beam angle in degrees: small is a narrow beam, large a wide wash."), TEXT("°"), {} },
			{ Attribute_Shutter, TEXT("Shutter and strobe"), TEXT("0 is open; above 0 the shutter strobes, faster as it rises. Strobes that follow the music are limited by the flash limit (Audio panel)."), nullptr, {} },
			{ Attribute_ColorR, TEXT("Colour mixing (RGB, RGBW, RGBA)"), TEXT("The red, green, blue (and white or amber) emitters, each 0-100 %. Mixing them makes the colour."), nullptr, { Attribute_ColorG, Attribute_ColorB, Attribute_ColorW, Attribute_ColorA, Attribute_ColorRGB } },
			{ Attribute_ColorC, TEXT("Colour mixing (CMY)"), TEXT("Cyan, magenta and yellow flags in front of a white lamp, each 0-100 %: more of a flag takes away its opposite colour."), nullptr, { Attribute_ColorM, Attribute_ColorY } },
			{ Attribute_ColorTemp, TEXT("Colour temperature (CTC)"), TEXT("The white's colour in kelvin: low is warm (tungsten), high is cool (daylight)."), TEXT(" K"), {} },
			{ Attribute_CTO, TEXT("CTO filter"), TEXT("A colour temperature orange filter, 0 to full: warms a daylight white towards tungsten."), nullptr, {} },
			{ Attribute_ColorWheel, TEXT("Colour wheel"), TEXT("A wheel of fixed colour filters; 0 is open (white). The slot names are in the Inspector."), nullptr, {} },
			{ Attribute_Gobo, TEXT("Gobo (rotating wheel)"), TEXT("A wheel of patterns projected in the beam; 0 is open. Gobo rotation and spin turn the pattern."), nullptr, { Attribute_GoboRotation, Attribute_GoboSpin } },
			{ Attribute_Gobo2, TEXT("Static gobo wheel"), TEXT("A second wheel of fixed patterns; 0 is open."), nullptr, {} },
			{ Attribute_Prism, TEXT("Prism"), TEXT("Splits the beam into several; 0 is out. Prism rotation spins it."), nullptr, { Attribute_PrismRotation } },
			{ Attribute_Iris, TEXT("Iris"), TEXT("Closes the beam down from the outside: 0 open, 1 as small as it goes."), nullptr, {} },
			{ Attribute_Frost, TEXT("Frost"), TEXT("Softens the beam's edge, 0 clear to 1 full frost (a wider, softer beam)."), nullptr, {} },
			{ Attribute_BladeA, TEXT("Framing blades (shutters)"), TEXT("Four blades that cut the beam's edges (top, right, bottom, left), and a rotation for the set, as on a profile spot."), nullptr, { Attribute_BladeB, Attribute_BladeC, Attribute_BladeD, Attribute_BladeRotation } },
		};
	}

	FString FormatRange(float Min, float Max, const TCHAR* Unit)
	{
		return FString::Printf(TEXT("%s to %s%s"), *FString::SanitizeFloat(FMath::RoundToFloat(Min * 10.f) / 10.f, 0),
			*FString::SanitizeFloat(FMath::RoundToFloat(Max * 10.f) / 10.f, 0), Unit);
	}
}

void UStageKnowledgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UStageItemSubsystem* Items = Collection.InitializeDependency<UStageItemSubsystem>();
	UStageWorkspaceSubsystem* Workspace = Collection.InitializeDependency<UStageWorkspaceSubsystem>();
	if (Items)
	{
		Items->OnCatalogLoaded.AddUniqueDynamic(this, &ThisClass::HandleCatalogChanged);
		Items->OnCatalogChanged.AddUniqueDynamic(this, &ThisClass::HandleCatalogChanged);
	}
	if (Workspace)
	{
		// Panel names come from the panel definitions, which load after start-up.
		PanelCatalogHandle = Workspace->OnPanelCatalogLoaded.AddUObject(this, &ThisClass::MarkStale);
	}
}

void UStageKnowledgeSubsystem::Deinitialize()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UStageItemSubsystem* Items = GameInstance->GetSubsystem<UStageItemSubsystem>())
		{
			Items->OnCatalogLoaded.RemoveDynamic(this, &ThisClass::HandleCatalogChanged);
			Items->OnCatalogChanged.RemoveDynamic(this, &ThisClass::HandleCatalogChanged);
		}
		if (UStageWorkspaceSubsystem* Workspace = GameInstance->GetSubsystem<UStageWorkspaceSubsystem>())
		{
			Workspace->OnPanelCatalogLoaded.Remove(PanelCatalogHandle);
		}
	}
	PanelCatalogHandle.Reset();
	Index = FStageKnowledgeIndex();
	Super::Deinitialize();
}

void UStageKnowledgeSubsystem::HandleCatalogChanged()
{
	MarkStale();
}

const FStageKnowledgeIndex& UStageKnowledgeSubsystem::GetIndex()
{
	if (bStale)
	{
		const double Start = FPlatformTime::Seconds();
		Index = StageKnowledge::Build(GatherSources());
		bStale = false;
		UE_LOG(LogStageCraft, Log, TEXT("Assistant knowledge: %d entries indexed in %.1f ms (%d article problems)."), Index.Num(),
			(FPlatformTime::Seconds() - Start) * 1000.0, ArticleErrors.Num());
		for (const FString& Error : ArticleErrors)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Assistant knowledge: help article %s"), *Error);
		}
	}
	return Index;
}

FString UStageKnowledgeSubsystem::GetHelpDirectory()
{
	return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("StageCraft"), TEXT("Help"));
}

TArray<FStageHelpArticle> UStageKnowledgeSubsystem::ReadArticles(TArray<FString>& OutErrors)
{
	TArray<FStageHelpArticle> Articles;
	const TPair<const TCHAR*, EStageAILanguage> Languages[] = { { TEXT("en"), EStageAILanguage::English }, { TEXT("hy"), EStageAILanguage::Armenian } };
	for (const TPair<const TCHAR*, EStageAILanguage>& Language : Languages)
	{
		const FString Folder = FPaths::Combine(GetHelpDirectory(), Language.Key);
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.md")), /*Files*/ true, /*Directories*/ false);
		Files.Sort();
		for (const FString& File : Files)
		{
			FString Text;
			if (IFileManager::Get().FileSize(*FPaths::Combine(Folder, File)) > StageKnowledge::MaxArticleLength * 4
				|| !FFileHelper::LoadFileToString(Text, *FPaths::Combine(Folder, File)))
			{
				OutErrors.Add(FString::Printf(TEXT("%s/%s: unreadable or too large"), Language.Key, *File));
				continue;
			}
			FStageHelpArticle Article;
			FString Error;
			if (StageKnowledge::ParseArticle(Text, FName(*FPaths::GetBaseFilename(File)), Language.Value, Article, Error))
			{
				Articles.Add(MoveTemp(Article));
			}
			else
			{
				OutErrors.Add(FString::Printf(TEXT("%s/%s"), Language.Key, *Error));
			}
		}
	}
	return Articles;
}

FStageKnowledgeSources UStageKnowledgeSubsystem::GatherStaticSources(TArray<FString>* OutArticleErrors)
{
	using namespace StageKnowledgeSources;
	FStageKnowledgeSources Sources;

	for (const FStageInputCommandInfo& Info : StageKeyBindings::GetCommands())
	{
		FStageKnowledgeSources::FCommand& Command = Sources.Commands.AddDefaulted_GetRef();
		Command.Command = Info.Command;
		Command.Label = Info.Label.ToString();
		Command.Description = Info.Description.ToString();
		Command.DefaultKeys = FString::Printf(TEXT("%s %s"), *Info.DefaultPrimary.ToText().ToString(), *Info.DefaultSecondary.ToText().ToString()).TrimEnd();
	}
	for (const StageUIAnchors::FTable& Table : StageUIAnchors::GetTables())
	{
		FString Name = Table.Panel.ToString();
		int32 Dot = INDEX_NONE;
		if (Name.FindLastChar(TEXT('.'), Dot))
		{
			Name.RightChopInline(Dot + 1);
		}
		for (const FStageUIAnchor& Anchor : Table.Anchors)
		{
			Sources.Controls.Add({ Table.Panel, Name, Anchor.ControlId, Anchor.Label.ToString(), Anchor.Tooltip.ToString() });
		}
	}
	for (const FAttributeInfo& Attribute : MakeAttributeTable())
	{
		Sources.Parameters.Add({ Attribute.Tag, Attribute.Name, Attribute.Meaning, {} });
	}
	TArray<FString> Errors;
	Sources.Articles = ReadArticles(Errors);
	if (OutArticleErrors)
	{
		*OutArticleErrors = MoveTemp(Errors);
	}
	return Sources;
}

FStageKnowledgeSources UStageKnowledgeSubsystem::GatherSources() const
{
	using namespace StageKnowledgeSources;
	FStageKnowledgeSources Sources = GatherStaticSources(&ArticleErrors);

	// Panels as the Window menu names them.
	const UGameInstance* GameInstance = GetGameInstance();
	if (const UStageWorkspaceSubsystem* Workspace = GameInstance ? GameInstance->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr)
	{
		for (const FGameplayTag& Panel : Workspace->GetAvailablePanels())
		{
			Sources.Panels.Add({ Panel, Workspace->GetPanelDisplayName(Panel).ToString() });
		}
		for (FStageKnowledgeSources::FControl& Control : Sources.Controls)
		{
			Control.PanelName = Workspace->GetPanelDisplayName(Control.Panel).ToString();
		}
	}

	// Which catalog models offer each attribute, and their ranges.
	TArray<const ULightingFixtureData*> Fixtures;
	if (const UStageItemSubsystem* Items = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr)
	{
		for (const UBaseItemData* Item : Items->GetCatalog())
		{
			if (const ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(Item))
			{
				Fixtures.Add(Fixture);
			}
		}
	}
	const TArray<FAttributeInfo> Attributes = MakeAttributeTable();
	for (int32 Position = 0; Position < Attributes.Num() && Position < Sources.Parameters.Num(); ++Position)
	{
		const FAttributeInfo& Attribute = Attributes[Position];
		FStageKnowledgeSources::FParameter& Parameter = Sources.Parameters[Position];
		for (const ULightingFixtureData* Fixture : Fixtures)
		{
			bool bSupported = Fixture->SupportsAttribute(Attribute.Tag);
			for (const FGameplayTag& Also : Attribute.AlsoTags)
			{
				bSupported |= Fixture->SupportsAttribute(Also);
			}
			if (!bSupported)
			{
				continue;
			}
			float Min = 0.f;
			float Max = 0.f;
			const FString Name = Fixture->DisplayName.ToString();
			Parameter.Models.Add(Attribute.RangeUnit && Fixture->GetAttributeRange(Attribute.Tag, Min, Max) && Min < Max
				? FString::Printf(TEXT("%s %s"), *Name, *FormatRange(Min, Max, Attribute.RangeUnit))
				: Name);
		}
	}
	return Sources;
}
