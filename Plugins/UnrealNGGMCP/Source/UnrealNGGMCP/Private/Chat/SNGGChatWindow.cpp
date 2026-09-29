// Copyright 2025-2026 NGG. All Rights Reserved.

#include "SNGGChatWindow.h"
#include "FSidecarClient.h"
#include "SidecarLauncher.h"
#include "NodeDepsInstaller.h"

#include "Containers/Ticker.h"
#include "Async/Async.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "BlueprintEditor.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraphNode.h"
#include "GameFramework/Actor.h"
#include "AssetRegistry/AssetData.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateTypes.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Text/RichTextLayoutMarshaller.h"
#include "Framework/Text/ITextDecorator.h"
#include "Fonts/SlateFontInfo.h"
#include "Misc/Base64.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include "Widgets/Layout/SBorder.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Layout/SSpacer.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "NGGChatWindow"

static const TCHAR* HistoryConfigSection = TEXT("NGGChat");
static const TCHAR* HistoryConfigKey     = TEXT("PromptHistory");
static const TCHAR* ModelConfigKey       = TEXT("Model");
static const TCHAR* EffortConfigKey      = TEXT("Effort");
static const TCHAR* AutoApproveConfigKey    = TEXT("AutoApproveTools");
static const TCHAR* BypassPermsConfigKey    = TEXT("BypassPermissions");
static constexpr int32 MaxHistory         = 50;
static constexpr int32 MaxLineChars       = 2000;
// Upper bound on the rich-text transcript buffer. Previously TranscriptBuffer
// only ever shrank on /clear, so a long session grew unbounded and every
// appended line re-SetText the entire buffer (O(n^2) over the session). When
// the buffer exceeds this budget we drop whole lines from the front, keeping
// the newest content. ~200k chars is well beyond a screenful but cheap to
// re-marshal.
static constexpr int32 MaxTranscriptChars = 200000;

// Model display label for the "let the CLI pick" row.
static const TCHAR* ModelDefaultLabel = TEXT("Default");

// ---------------------------------------------------------------------------
// "Neo Graph" brand palette
// ---------------------------------------------------------------------------
// Dark, violet-accented design system. Hex values are the Neo Graph design
// tokens; they are mapped to FLinearColor with the same direct-float
// convention this widget already used (channel/255), so the on-screen result
// matches the spec. One accent (violet) carries the brand; the green/amber/
// coral/cyan/blue hues are reserved strictly for their semantic meaning.
namespace NGGBrand
{
	static FORCEINLINE FLinearColor Hex(uint8 R, uint8 G, uint8 B, float A = 1.f)
	{
		return FLinearColor(R / 255.f, G / 255.f, B / 255.f, A);
	}

	// Surfaces (darkest -> lightest)
	static const FLinearColor AppBg         = Hex(0x0a, 0x0b, 0x10); // app background
	static const FLinearColor Panel         = Hex(0x12, 0x13, 0x1b); // panel / message surface
	static const FLinearColor Raised        = Hex(0x15, 0x16, 0x1f); // raised panel (inputs, cards)
	static const FLinearColor Inset         = Hex(0x18, 0x1a, 0x24); // inset chip / code field
	static const FLinearColor HoverSurface  = Hex(0x1b, 0x1d, 0x27); // hover / active surface

	// Borders & dividers
	static const FLinearColor Border        = Hex(0x20, 0x22, 0x2c); // default border
	static const FLinearColor BorderStrong  = Hex(0x26, 0x28, 0x33); // focused input / active card
	static const FLinearColor Connector     = Hex(0x34, 0x37, 0x4a); // connector / graph lines

	// Text
	static const FLinearColor TextPrimary   = Hex(0xe6, 0xe7, 0xee); // primary text
	static const FLinearColor TextBody      = Hex(0x9a, 0x9c, 0xae); // secondary / body
	static const FLinearColor TextMuted     = Hex(0x6a, 0x6d, 0x80); // muted / labels / timestamps
	static const FLinearColor TextMono      = Hex(0xcf, 0xd3, 0xe0); // monospace code text

	// Brand accent — violet (use sparingly for emphasis)
	static const FLinearColor Violet        = Hex(0xa3, 0x5a, 0xf2); // send, active node, links, focus
	static const FLinearColor VioletLight   = Hex(0xc7, 0x9b, 0xf6); // eyebrow labels, "Claude" tag

	// Semantic / role accents (one hue per purpose)
	static const FLinearColor Success       = Hex(0x56, 0xc0, 0x6a); // approve / compiled
	static const FLinearColor SuccessField  = Hex(0x0f, 0x1a, 0x14); // dark green field
	static const FLinearColor SuccessBorder = Hex(0x23, 0x46, 0x3a); // green border
	static const FLinearColor Warning       = Hex(0xe0, 0xb2, 0x4a); // permission / warning prompts
	static const FLinearColor Info          = Hex(0x5b, 0x8c, 0xf0); // info / model selectors (blue)
	static const FLinearColor Cyan          = Hex(0x43, 0xc4, 0xd6); // tool-call names / secondary tags
	static const FLinearColor Coral         = Hex(0xef, 0x7a, 0x52); // deny / destructive (text only)

	// Near-black icon/text painted on top of a filled accent button.
	static const FLinearColor OnAccent      = Hex(0x0d, 0x0e, 0x15);
	// Default fill for every non-primary button.
	static const FLinearColor ButtonFill    = Hex(0x1d, 0x1f, 0x2a);

	// Rounded-corner radius shared by buttons (px). Pills use a large radius.
	static constexpr float ButtonRadius = 8.f;

	// Primary action: solid violet fill, near-black label.
	static const FButtonStyle& VioletButton()
	{
		static const FButtonStyle Style = []()
		{
			FButtonStyle S;
			S.SetNormal  (FSlateRoundedBoxBrush(Violet,      ButtonRadius));
			S.SetHovered (FSlateRoundedBoxBrush(VioletLight,  ButtonRadius));
			S.SetPressed (FSlateRoundedBoxBrush(Violet,      ButtonRadius));
			S.SetDisabled(FSlateRoundedBoxBrush(ButtonFill,   ButtonRadius));
			S.SetNormalForeground  (FSlateColor(OnAccent));
			S.SetHoveredForeground (FSlateColor(OnAccent));
			S.SetPressedForeground (FSlateColor(OnAccent));
			S.SetDisabledForeground(FSlateColor(TextMuted));
			S.SetNormalPadding (FMargin(10.f, 4.f));
			S.SetPressedPadding(FMargin(10.f, 4.f));
			return S;
		}();
		return Style;
	}

	// Approve / "compiled": solid green fill, near-black label.
	static const FButtonStyle& GreenButton()
	{
		static const FButtonStyle Style = []()
		{
			FButtonStyle S;
			S.SetNormal  (FSlateRoundedBoxBrush(Success,    ButtonRadius));
			S.SetHovered (FSlateRoundedBoxBrush(Hex(0x6c, 0xd0, 0x80), ButtonRadius));
			S.SetPressed (FSlateRoundedBoxBrush(Success,    ButtonRadius));
			S.SetDisabled(FSlateRoundedBoxBrush(ButtonFill, ButtonRadius));
			S.SetNormalForeground  (FSlateColor(OnAccent));
			S.SetHoveredForeground (FSlateColor(OnAccent));
			S.SetPressedForeground (FSlateColor(OnAccent));
			S.SetDisabledForeground(FSlateColor(TextMuted));
			S.SetNormalPadding (FMargin(10.f, 4.f));
			S.SetPressedPadding(FMargin(10.f, 4.f));
			return S;
		}();
		return Style;
	}

	// Every other button: near-black fill, light text, subtle 1.5px border.
	static const FButtonStyle& SecondaryButton()
	{
		static const FButtonStyle Style = []()
		{
			FButtonStyle S;
			S.SetNormal  (FSlateRoundedBoxBrush(ButtonFill,   ButtonRadius, Border,       1.5f));
			S.SetHovered (FSlateRoundedBoxBrush(HoverSurface, ButtonRadius, BorderStrong, 1.5f));
			S.SetPressed (FSlateRoundedBoxBrush(ButtonFill,   ButtonRadius, BorderStrong, 1.5f));
			S.SetDisabled(FSlateRoundedBoxBrush(Panel,        ButtonRadius, Border,       1.5f));
			S.SetNormalForeground  (FSlateColor(TextPrimary));
			S.SetHoveredForeground (FSlateColor(TextPrimary));
			S.SetPressedForeground (FSlateColor(TextPrimary));
			S.SetDisabledForeground(FSlateColor(TextMuted));
			S.SetNormalPadding (FMargin(10.f, 4.f));
			S.SetPressedPadding(FMargin(10.f, 4.f));
			return S;
		}();
		return Style;
	}

	// Deny / destructive: secondary fill, coral label (text only, no fill).
	static const FButtonStyle& CoralButton()
	{
		static const FButtonStyle Style = []()
		{
			FButtonStyle S = SecondaryButton();
			S.SetNormalForeground  (FSlateColor(Coral));
			S.SetHoveredForeground (FSlateColor(Coral));
			S.SetPressedForeground (FSlateColor(Coral));
			return S;
		}();
		return Style;
	}
}

// ---------------------------------------------------------------------------
// Colors / prefixes
// ---------------------------------------------------------------------------
FLinearColor SNGGChatWindow::ColorForKind(ELineKind Kind)
{
	switch (Kind)
	{
		case ELineKind::User:      return NGGBrand::VioletLight; // human prompts — light-violet brand accent
		case ELineKind::Assistant: return NGGBrand::TextPrimary; // Claude's prose — primary readable text
		case ELineKind::Thinking:  return NGGBrand::TextMuted;   // internal reasoning — de-emphasized
		case ELineKind::ToolUse:   return NGGBrand::Cyan;        // tool-call names — cyan
		case ELineKind::ToolOk:    return NGGBrand::Success;     // compiled / ok — green
		case ELineKind::ToolErr:   return NGGBrand::Coral;       // tool error — coral
		case ELineKind::Error:     return NGGBrand::Coral;       // error — coral
		case ELineKind::System:    return NGGBrand::TextMuted;   // system labels / timestamps
		case ELineKind::Perm:      return NGGBrand::Warning;     // permission / warning prompts — amber
	}
	return NGGBrand::TextPrimary;
}

const TCHAR* SNGGChatWindow::PrefixForKind(ELineKind Kind)
{
	switch (Kind)
	{
		case ELineKind::User:      return TEXT("[user]     ");
		case ELineKind::Assistant: return TEXT("[assistant]");
		case ELineKind::Thinking:  return TEXT("[thinking] ");
		case ELineKind::ToolUse:   return TEXT("[tool_use] ");
		case ELineKind::ToolOk:    return TEXT("[tool_ok]  ");
		case ELineKind::ToolErr:   return TEXT("[tool_err] ");
		case ELineKind::Error:     return TEXT("[error]    ");
		case ELineKind::System:    return TEXT("[system]   ");
		case ELineKind::Perm:      return TEXT("[perm]     ");
	}
	return TEXT("[?]        ");
}

const TCHAR* SNGGChatWindow::TagForKind(ELineKind Kind)
{
	// Must match the names registered in TranscriptStyle (see Construct).
	switch (Kind)
	{
		case ELineKind::User:      return TEXT("NGGUser");
		case ELineKind::Assistant: return TEXT("NGGAssistant");
		case ELineKind::Thinking:  return TEXT("NGGThinking");
		case ELineKind::ToolUse:   return TEXT("NGGToolUse");
		case ELineKind::ToolOk:    return TEXT("NGGToolOk");
		case ELineKind::ToolErr:   return TEXT("NGGToolErr");
		case ELineKind::Error:     return TEXT("NGGError");
		case ELineKind::System:    return TEXT("NGGSystem");
		case ELineKind::Perm:      return TEXT("NGGPerm");
	}
	return TEXT("NGGSystem");
}

FString SNGGChatWindow::EscapeRichMarkup(const FString& In)
{
	// Rich-text parser treats <, >, & as markup control chars. Escape them
	// before wrapping a user-supplied string in a style tag, or stray angle
	// brackets in tool output / code will silently eat following text.
	FString Out = In;
	Out.ReplaceInline(TEXT("&"),  TEXT("&amp;"));
	Out.ReplaceInline(TEXT("<"),  TEXT("&lt;"));
	Out.ReplaceInline(TEXT(">"),  TEXT("&gt;"));
	return Out;
}

// ---------------------------------------------------------------------------
// Construct
// ---------------------------------------------------------------------------
void SNGGChatWindow::Construct(const FArguments& InArgs)
{
	LoadHistory();
	InitSlashCommands();

	// Build the model dropdown options once. Current-generation pinned versions
	// sit up top with descriptions; older pinned versions live under a
	// "More models" separator for reproducibility. Every row pins an exact
	// build — we deliberately do NOT list the bare opus/sonnet/haiku aliases,
	// because each alias resolves to whatever the latest pinned build already
	// is, which showed up in the picker as a duplicate of the pinned row.
	ModelOptions.Reset();
	{
		auto Add = [this](const TCHAR* Label, const TCHAR* Desc, const TCHAR* Arg)
		{
			auto Opt = MakeShared<FNGGChatModelOption>();
			Opt->DisplayLabel = Label;
			Opt->Description  = Desc;
			Opt->CliArg       = Arg;
			ModelOptions.Add(Opt);
		};
		auto AddSeparator = [this](const TCHAR* Label)
		{
			auto Opt = MakeShared<FNGGChatModelOption>();
			Opt->DisplayLabel  = Label;
			Opt->bIsSeparator  = true;
			ModelOptions.Add(Opt);
		};

		// Primary: current-generation pinned versions with descriptions.
		// Opus 4.8 is listed first so it is the default / fallback pick — Fable 5
		// is the most capable but premium-priced, so it should be an explicit
		// choice rather than the silent default.
		Add(TEXT("Opus 4.8"),   TEXT("Most capable for ambitious work"),     TEXT("claude-opus-4-8"));
		Add(TEXT("Fable 5"),    TEXT("Most capable for the most demanding work (premium)"), TEXT("claude-fable-5"));
		Add(TEXT("Sonnet 4.6"), TEXT("Most efficient for everyday tasks"),   TEXT("claude-sonnet-4-6"));
		Add(TEXT("Haiku 4.5"),  TEXT("Fastest for quick answers"),           TEXT("claude-haiku-4-5"));

		// More models: older pinned versions, still selectable for reproducibility.
		AddSeparator(TEXT("— More models —"));
		Add(TEXT("Opus 4.7"),   TEXT(""), TEXT("claude-opus-4-7"));
		Add(TEXT("Opus 4.6"),   TEXT(""), TEXT("claude-opus-4-6"));
		Add(TEXT("Sonnet 4.5"), TEXT(""), TEXT("claude-sonnet-4-5"));

		// Let the CLI pick with no --model flag.
		AddSeparator(TEXT("— — —"));
		Add(ModelDefaultLabel, TEXT("Let the Claude CLI pick"), TEXT(""));
	}
	LoadSelectedModel(); // populates SelectedModel from saved config (or first selectable)
	InitEffortOptions();
	LoadSelectedEffort();
	LoadUsageFromConfig(); // restore cumulative token counters from prior editor sessions
	LoadAutoApproveTools();     // restore project-scoped "Always" allow-list
	LoadBypassPermissions();    // restore project-scoped permission-bypass toggle

	// Session id = MD5 of project file path, so the sidecar can isolate
	// state across projects on the same machine.
	SessionId = FMD5::HashAnsiString(*FPaths::GetProjectFilePath());

	ChildSlot
	[
		SNew(SVerticalBox)

		// ---- Status bar ------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(6.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(FSlateColor(NGGBrand::Raised))
			.Padding(6.f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 6.f, 0.f)
				[
					SAssignNew(ThinkingThrobber, SCircularThrobber)
					.Radius(8.f)
					.Period(0.7f)
					.NumPieces(8)
					.ToolTipText(LOCTEXT("ThinkingTooltip", "Claude is processing your request"))
					.Visibility(EVisibility::Collapsed)
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SAssignNew(StatusText, STextBlock)
					.Text(LOCTEXT("StatusConnecting", "Connecting..."))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 4.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ModelLabel", "Model:"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 0.f, 0.f)
				[
					SNew(SBox)
					.MinDesiredWidth(110.f)
					[
						SAssignNew(ModelCombo, SComboBox<TSharedPtr<FNGGChatModelOption>>)
						.OptionsSource(&ModelOptions)
						.InitiallySelectedItem(SelectedModel)
						.OnGenerateWidget(this, &SNGGChatWindow::GenerateModelComboRow)
						.OnSelectionChanged(this, &SNGGChatWindow::OnModelSelectionChanged)
						.ToolTipText(LOCTEXT("ModelTooltip", "Claude model used for new turns. Changing this restarts the current subprocess so the next prompt uses the new model."))
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return SelectedModel.IsValid()
									? FText::FromString(SelectedModel->DisplayLabel)
									: FText::FromString(ModelDefaultLabel);
							})
						]
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.f, 0.f, 6.f, 0.f)
				[
					SAssignNew(ResolvedModelText, STextBlock)
					.Text(LOCTEXT("ModelNotStarted", "(not started)"))
					.ColorAndOpacity(FSlateColor(NGGBrand::TextMuted))
					.ToolTipText(LOCTEXT("ResolvedModelTooltip", "Exact model id reported by the Claude CLI on session init."))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.f, 0.f, 4.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("EffortLabel", "Effort:"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SBox)
					.MinDesiredWidth(100.f)
					[
						SAssignNew(EffortCombo, SComboBox<TSharedPtr<FNGGChatEffortOption>>)
						.OptionsSource(&EffortOptions)
						.InitiallySelectedItem(SelectedEffort)
						.OnGenerateWidget(this, &SNGGChatWindow::GenerateEffortComboRow)
						.OnSelectionChanged(this, &SNGGChatWindow::OnEffortSelectionChanged)
						.ToolTipText(LOCTEXT("EffortTooltip",
							"Extended-thinking budget for new turns. Higher levels tell Claude to "
							"spend more reasoning tokens before answering — better for hard problems, "
							"slower and more expensive. The keyword is prepended to your prompt; "
							"the transcript still shows exactly what you typed."))
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return SelectedEffort.IsValid()
									? FText::FromString(SelectedEffort->DisplayLabel)
									: FText::FromString(FString(TEXT("auto")));
							})
						]
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.f, 0.f, 6.f, 0.f)
				[
					SAssignNew(UsageText, STextBlock)
					.Text(LOCTEXT("UsageIdle", "0 tok"))
					.ColorAndOpacity(FSlateColor(NGGBrand::Cyan))
					.ToolTipText(LOCTEXT("UsageTooltipIdle",
						"Token usage for this conversation. Updates after each assistant turn."))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SAssignNew(ReconnectButton, SButton)
					.ButtonStyle(&NGGBrand::SecondaryButton())
					.Text(LOCTEXT("Reconnect", "Reconnect"))
					.Visibility(EVisibility::Collapsed)
					.OnClicked(this, &SNGGChatWindow::OnReconnectClicked)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.ButtonStyle(&NGGBrand::SecondaryButton())
					.Text(LOCTEXT("RestartSidecar", "↻ Sidecar"))
					.ToolTipText(LOCTEXT("RestartSidecarTip",
						"Kill the ngg-sidecar daemon and respawn it. Use this after you "
						"modify Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/*.js — the daemon caches the old code "
						"across editor restarts, so a manual restart is the only way to "
						"pick up changes."))
					.OnClicked(this, &SNGGChatWindow::OnRestartSidecarClicked)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.ButtonStyle(&NGGBrand::SecondaryButton())
					.Text(LOCTEXT("KillSidecar", "✕ Sidecar"))
					.ToolTipText(LOCTEXT("KillSidecarTip",
						"Shut down the ngg-sidecar daemon without respawning it. "
						"Use this to fully stop the sidecar — press Reconnect (or "
						"↻ Sidecar) later to bring it back up."))
					.OnClicked(this, &SNGGChatWindow::OnKillSidecarClicked)
				]

				// "Bypass perms" toggle. When ON, the sidecar respawns claude
				// with --dangerously-skip-permissions so tool calls don't go
				// through the permission gate. Same speed as running claude in
				// a terminal with the same flag — at the cost of trusting every
				// tool call this session makes. Persisted per project.
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.f, 0.f, 0.f, 0.f)
				[
					SAssignNew(BypassPermsButton, SButton)
					.ButtonStyle(&NGGBrand::SecondaryButton())
					.ToolTipText(LOCTEXT("BypassPermsTip",
						"Toggle: when ON, tool calls bypass the permission gate "
						"(claude is spawned with --dangerously-skip-permissions). "
						"Eliminates per-call HTTP/WS roundtrip + click prompts. "
						"Use only when you trust the prompt. Saved per project."))
					.OnClicked(this, &SNGGChatWindow::OnBypassPermissionsClicked)
					[
						SAssignNew(BypassPermsButtonText, STextBlock)
						.Text(LOCTEXT("BypassPermsOff", "Bypass: OFF"))
					]
				]
			]
		]

		// ---- Blueprint-editor selection banner ------------------------
		// Hidden by default; revealed by RefreshBpSelectionBanner() whenever
		// the developer has BP nodes selected in any open Blueprint editor.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(6.f, 0.f, 6.f, 4.f)
		[
			SAssignNew(BpSelectionBanner, SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(FSlateColor(NGGBrand::Inset))
			.Padding(FMargin(8.f, 6.f))
			.Visibility(EVisibility::Collapsed)
			[
				SAssignNew(BpSelectionBannerText, STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(NGGBrand::Info))
				.ToolTipText(LOCTEXT("BpSelectionBannerTooltip",
					"Claude can read your current Blueprint-editor selection. "
					"Try one of the suggested prompts to act on the selected nodes."))
			]
		]

		// ---- Terminal transcript --------------------------------------
		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		.Padding(6.f, 0.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(FSlateColor(NGGBrand::AppBg))
			.Padding(4.f)
			[
				// Build a private style set + rich-text marshaller so one
				// editable text box can render multi-colored runs (the whole
				// transcript stays selectable / copyable as one widget).
				SAssignNew(TranscriptText, SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AutoWrapText(true)
				.AllowMultiLine(true)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
				.ForegroundColor(FSlateColor(NGGBrand::TextMono))
				.BackgroundColor(FSlateColor(FLinearColor(0, 0, 0, 0)))
				.Margin(FMargin(0.f))
				.Marshaller([this]() -> TSharedPtr<ITextLayoutMarshaller>
				{
					const FSlateFontInfo MonoFont = FCoreStyle::GetDefaultFontStyle("Mono", 10);

					auto MakeStyle = [&MonoFont](const FLinearColor& Color) -> FTextBlockStyle
					{
						return FTextBlockStyle()
							.SetFont(MonoFont)
							.SetColorAndOpacity(FSlateColor(Color));
					};

					TranscriptStyle = MakeShared<FSlateStyleSet>(TEXT("NGGChatStyles"));
					TranscriptStyle->Set(FName(TEXT("NGGUser")),      MakeStyle(ColorForKind(ELineKind::User)));
					TranscriptStyle->Set(FName(TEXT("NGGAssistant")), MakeStyle(ColorForKind(ELineKind::Assistant)));
					TranscriptStyle->Set(FName(TEXT("NGGThinking")),  MakeStyle(ColorForKind(ELineKind::Thinking)));
					TranscriptStyle->Set(FName(TEXT("NGGToolUse")),   MakeStyle(ColorForKind(ELineKind::ToolUse)));
					TranscriptStyle->Set(FName(TEXT("NGGToolOk")),    MakeStyle(ColorForKind(ELineKind::ToolOk)));
					TranscriptStyle->Set(FName(TEXT("NGGToolErr")),   MakeStyle(ColorForKind(ELineKind::ToolErr)));
					TranscriptStyle->Set(FName(TEXT("NGGError")),     MakeStyle(ColorForKind(ELineKind::Error)));
					TranscriptStyle->Set(FName(TEXT("NGGSystem")),    MakeStyle(ColorForKind(ELineKind::System)));
					TranscriptStyle->Set(FName(TEXT("NGGPerm")),      MakeStyle(ColorForKind(ELineKind::Perm)));
					// Fenced code blocks (```...```): monospace code text.
					TranscriptStyle->Set(FName(TEXT("NGGCode")),      MakeStyle(NGGBrand::TextMono));
					// Muted language header shown above a code block.
					TranscriptStyle->Set(FName(TEXT("NGGCodeLabel")), MakeStyle(NGGBrand::TextMuted));

					// Per-token syntax styles used by AppendHighlightedBlock.
					// Names must stay in sync with TokenizeJson / TokenizeCpp.
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxJsonKey")),    MakeStyle(NGGBrand::Cyan));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxJsonStr")),    MakeStyle(NGGBrand::Success));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxJsonNum")),    MakeStyle(NGGBrand::Warning));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxJsonBool")),   MakeStyle(NGGBrand::VioletLight));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxJsonStruct")), MakeStyle(NGGBrand::TextMuted));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxCppKeyword")), MakeStyle(NGGBrand::Violet));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxCppType")),    MakeStyle(NGGBrand::Cyan));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxCppString")),  MakeStyle(NGGBrand::Success));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxCppComment")), MakeStyle(NGGBrand::TextMuted));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxCppPreproc")), MakeStyle(NGGBrand::VioletLight));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxCppNumber")),  MakeStyle(NGGBrand::Warning));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxCppIdent")),   MakeStyle(NGGBrand::TextMono));
					TranscriptStyle->Set(FName(TEXT("NGGSyntaxCppOp")),      MakeStyle(NGGBrand::TextBody));

					return FRichTextLayoutMarshaller::Create(
						TArray<TSharedRef<ITextDecorator>>(),
						TranscriptStyle.Get());
				}())
			]
		]

		// ---- Pending permission prompts (action rows) -----------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(6.f, 4.f, 6.f, 0.f)
		[
			SAssignNew(PendingPermsBorder, SBorder)
			.Visibility(EVisibility::Collapsed)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(FSlateColor(NGGBrand::Warning))
			.Padding(4.f)
			[
				SAssignNew(PendingPermsList, SVerticalBox)
			]
		]

		// ---- Input row -------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(6.f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.Padding(0.f, 0.f, 6.f, 0.f)
			[
				SAssignNew(SlashMenuAnchor, SMenuAnchor)
				.Placement(MenuPlacement_AboveAnchor)
				.OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
				{
					return SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.BorderBackgroundColor(FSlateColor(NGGBrand::Panel))
						.Padding(2.f)
						[
							SNew(SBox)
							.WidthOverride(360.f)
							.MaxDesiredHeight(240.f)
							[
								SAssignNew(SlashListView, SListView<TSharedPtr<FNGGChatSlashCommand>>)
								.ListItemsSource(&FilteredCommands)
								.SelectionMode(ESelectionMode::Single)
								.OnGenerateRow(this, &SNGGChatWindow::GenerateSlashRow)
								.OnSelectionChanged(this, &SNGGChatWindow::OnSlashSelectionChanged)
								.OnMouseButtonClick(this, &SNGGChatWindow::OnSlashRowClicked)
							]
						];
				})
				[
					SAssignNew(AtMenuAnchor, SMenuAnchor)
					.Placement(MenuPlacement_AboveAnchor)
					.OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
					{
						return SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
							.BorderBackgroundColor(FSlateColor(NGGBrand::Panel))
							.Padding(2.f)
							[
								SNew(SBox)
								.WidthOverride(460.f)
								.MaxDesiredHeight(280.f)
								[
									SAssignNew(AtListView, SListView<TSharedPtr<FNGGChatAtMention>>)
									.ListItemsSource(&FilteredAtMentions)
									.SelectionMode(ESelectionMode::Single)
									.OnGenerateRow(this, &SNGGChatWindow::GenerateAtRow)
									.OnSelectionChanged(this, &SNGGChatWindow::OnAtSelectionChanged)
									.OnMouseButtonClick(this, &SNGGChatWindow::OnAtRowClicked)
								]
							];
					})
					[
						SNew(SBox)
						.MinDesiredHeight(60.f)
						.MaxDesiredHeight(220.f)
						[
							SAssignNew(InputBox, SMultiLineEditableTextBox)
							.HintText(LOCTEXT("InputHint", "Ask Claude... (Enter to send, Ctrl+Enter for newline, @ to reference a file)"))
							.AlwaysShowScrollbars(false)
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
							.OnKeyDownHandler(this, &SNGGChatWindow::OnInputKeyDown)
							.OnTextChanged(this, &SNGGChatWindow::OnInputTextChanged)
						]
					]
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 3.f)
				[
					SAssignNew(SendButton, SButton)
					.ButtonStyle(&NGGBrand::VioletButton())
					.Text(LOCTEXT("Send", "Send"))
					.IsEnabled(false)
					.OnClicked(this, &SNGGChatWindow::OnSendClicked)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 3.f)
				[
					SAssignNew(CancelButton, SButton)
					.ButtonStyle(&NGGBrand::SecondaryButton())
					.Text(LOCTEXT("Cancel", "Cancel"))
					.IsEnabled(false)
					.OnClicked(this, &SNGGChatWindow::OnCancelClicked)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 3.f)
				[
					SNew(SButton)
					.ButtonStyle(&NGGBrand::SecondaryButton())
					.Text(LOCTEXT("Clear", "Clear"))
					.OnClicked(this, &SNGGChatWindow::OnClearClicked)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SAssignNew(SelectionButton, SButton)
					.ButtonStyle(&NGGBrand::SecondaryButton())
					.ToolTipText(LOCTEXT("SelectionToggleTip",
						"When enabled, each prompt you send is preceded by a <ue5_selection> "
						"context block describing selected actors and content-browser assets."))
					.OnClicked(this, &SNGGChatWindow::OnSelectionToggleClicked)
					[
						SAssignNew(SelectionButtonText, STextBlock)
						.Text(LOCTEXT("SelectionToggleOff", "+ Selection (0)"))
					]
				]

			]
		]
	];

	Client = MakeShared<FSidecarClient>();
	Client->OnStateChanged.AddSP(this, &SNGGChatWindow::OnClientStateChanged);
	Client->OnEvent.AddSP(this, &SNGGChatWindow::OnClientEvent);
	Client->OnError.AddSP(this, &SNGGChatWindow::OnClientError);
	Client->OnPermissionRequest.AddSP(this, &SNGGChatWindow::OnClientPermissionRequest);
	Client->OnSystem.AddSP(this, &SNGGChatWindow::OnClientSystem);
	Client->OnModels.AddSP(this, &SNGGChatWindow::OnClientModels);

	// Show the saved pick in the resolved-id slot right away so the user
	// sees their model immediately on chat-window open. The exact resolved id
	// will refine this once Claude's `init` event arrives on first prompt.
	if (ResolvedModelText.IsValid() && SelectedModel.IsValid())
	{
		const FString Friendly = FriendlyLabelFromModelId(SelectedModel->CliArg);
		if (Friendly.IsEmpty())
		{
			ResolvedModelText->SetText(LOCTEXT("ModelDefault", "(CLI default)"));
		}
		else
		{
			ResolvedModelText->SetText(FText::FromString(FString::Printf(TEXT("(%s)"), *Friendly)));
			ResolvedModelText->SetToolTipText(FText::FromString(SelectedModel->CliArg));
		}
	}

	AppendLine(ELineKind::System,
		FString::Printf(TEXT("Session started (id: %s)"), *SessionId.Left(12)));

	// UsageText is now assigned — push the loaded counters into the pill.
	UpdateUsageDisplay();

	// Refresh the "+ Selection (N)" label twice per second. Cheap — just
	// queries GEditor and the content browser module, no scene walk.
	// Use an explicit weak-ptr lambda instead of CreateSP so the ticker
	// can self-cancel cleanly if the widget dies before its destructor
	// runs (e.g. during editor shutdown when module teardown order can
	// race our ticker).
	TWeakPtr<SNGGChatWindow> WeakSelfForTicker = SharedThis(this);
	SelectionLabelTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakSelfForTicker](float Dt) -> bool
		{
			if (IsEngineExitRequested()) return false;
			TSharedPtr<SNGGChatWindow> Pinned = WeakSelfForTicker.Pin();
			if (!Pinned.IsValid()) return false; // widget gone — stop ticker
			return Pinned->TickSelectionLabel(Dt);
		}),
		0.5f);

	RefreshBypassButtonAppearance();

	BeginConnect();
}

SNGGChatWindow::~SNGGChatWindow()
{
	if (HealthTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HealthTicker);
		HealthTicker.Reset();
	}
	if (SelectionLabelTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(SelectionLabelTicker);
		SelectionLabelTicker.Reset();
	}
	if (ThinkingAnimTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ThinkingAnimTicker);
		ThinkingAnimTicker.Reset();
	}
	if (RestartTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RestartTicker);
		RestartTicker.Reset();
	}
	if (DepsTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DepsTicker);
		DepsTicker.Reset();
	}
	if (Client.IsValid())
	{
		Client->Disconnect();
		Client.Reset();
	}
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

void SNGGChatWindow::BeginConnect()
{
	if (ReconnectButton.IsValid())
	{
		ReconnectButton->SetVisibility(EVisibility::Collapsed);
	}

	// Re-entrancy guard: a previous BeginConnect may still own a HealthTicker
	// that's polling for an earlier spawn. Tear it down before starting a fresh
	// wait so a stale ticker can't leak and keep probing (and racing this one).
	if (HealthTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HealthTicker);
		HealthTicker.Reset();
	}
	bAwaitingSidecar = false;

	if (DepsTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DepsTicker);
		DepsTicker.Reset();
	}

	// First-run bootstrap: the sidecar and the MCP server are npm packages
	// shipped without node_modules — spawning node before `npm install` has
	// run just produces an instant crash ("Cannot find module 'ws'"). Kick /
	// join the background install (module startup usually started it already)
	// and defer the actual connect until it resolves.
	if (!FNodeDepsInstaller::AreDepsReady())
	{
		FNodeDepsInstaller::StartInstallIfNeeded();

		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusDeps",
				"Installing Node.js packages (first run)..."));
		}
		if (!bDepsNoticeShown)
		{
			bDepsNoticeShown = true;
			AppendLine(ELineKind::System, TEXT(
				"First-run setup: running `npm install` for the chat sidecar and the "
				"UE5 MCP server. This usually takes under a minute — the chat will "
				"connect automatically when it finishes."));
		}

		TWeakPtr<SNGGChatWindow> WeakSelfForDeps = SharedThis(this);
		DepsTicker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([WeakSelfForDeps](float) -> bool
			{
				if (IsEngineExitRequested()) return false;
				TSharedPtr<SNGGChatWindow> Pinned = WeakSelfForDeps.Pin();
				if (!Pinned.IsValid()) return false; // widget gone — unregister
				return Pinned->TickDepsWait();
			}),
			0.5f);
		return;
	}

	if (StatusText.IsValid())
	{
		StatusText->SetText(LOCTEXT("StatusHealth", "Checking sidecar..."));
	}

	UE_LOG(LogNGGChat, Log, TEXT("BeginConnect: ensuring compatible sidecar (async)"));

	// EnsureCompatibleSidecar does an HTTP version probe and, on a version
	// mismatch, a busy-wait shutdown poll that can block for several seconds.
	// Running it on the game thread freezes the editor UI, so push it onto a
	// thread-pool task and marshal the result back to the game thread to drive
	// the rest of the connect flow / UI updates.
	TWeakPtr<SNGGChatWindow> WeakSelf = SharedThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakSelf]()
	{
		const ESidecarEnsureResult Ensure = FSidecarLauncher::EnsureCompatibleSidecar();
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, Ensure]()
		{
			TSharedPtr<SNGGChatWindow> Pinned = WeakSelf.Pin();
			if (!Pinned.IsValid()) return; // widget gone — drop the continuation
			Pinned->ContinueConnectAfterEnsure(Ensure);
		});
	});
}

void SNGGChatWindow::ContinueConnectAfterEnsure(ESidecarEnsureResult Ensure)
{
	if (Ensure == ESidecarEnsureResult::AliveCompatible)
	{
		UE_LOG(LogNGGChat, Log, TEXT("Sidecar already alive at expected version — connecting WS"));
		Client->Connect(FSidecarLauncher::GetWebSocketURL(), SessionId, /*replay=*/true,
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return;
	}

	if (Ensure == ESidecarEnsureResult::ShutdownFailed)
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusShutdownFail",
				"Stale sidecar refused to shut down. Kill the node process manually and reconnect."));
		}
		if (ReconnectButton.IsValid())
		{
			ReconnectButton->SetVisibility(EVisibility::Visible);
		}
		return;
	}

	if (Ensure == ESidecarEnsureResult::SpawnFailed)
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusSpawnFail",
				"Sidecar did not start. Check Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/index.js and NGG_NODE_PATH."));
		}
		if (ReconnectButton.IsValid())
		{
			ReconnectButton->SetVisibility(EVisibility::Visible);
		}
		return;
	}

	// Spawned or RestartedAfterMismatch — wait for the new process to come up.
	// Two timers:
	//   HealthRespawnAt    — if the sidecar isn't alive by this point we retry
	//                        the spawn once. The Windows `cmd /c start /B node`
	//                        chain occasionally fails silently right after the
	//                        previous sidecar's process tree dies (file lock /
	//                        permission-port still held by the dying process),
	//                        leaving no node.exe at all. A single retry after
	//                        ~4s clears every variant we've reproduced.
	//   HealthProbeDeadline — hard give-up; show the failure message.
	bAwaitingSidecar    = true;
	bHealthRespawned    = false;
	HealthRespawnAt     = FPlatformTime::Seconds() + 4.0;
	HealthProbeDeadline = FPlatformTime::Seconds() + 15.0;

	if (StatusText.IsValid())
	{
		StatusText->SetText(
			Ensure == ESidecarEnsureResult::RestartedAfterMismatch
				? LOCTEXT("StatusRestart", "Restarting sidecar (version mismatch)...")
				: LOCTEXT("StatusSpawning", "Starting sidecar..."));
	}

	// Weak-ptr self-guard (mirrors SelectionLabelTicker) so the ticker can
	// self-cancel cleanly if the widget dies before its destructor runs.
	TWeakPtr<SNGGChatWindow> WeakSelfForHealth = SharedThis(this);
	HealthTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakSelfForHealth](float) -> bool
		{
			TSharedPtr<SNGGChatWindow> Pinned = WeakSelfForHealth.Pin();
			if (!Pinned.IsValid()) return false; // widget gone — unregister
			Pinned->TickHealthProbe();
			return Pinned->bAwaitingSidecar;
		}),
		0.2f);
}

bool SNGGChatWindow::TickDepsWait()
{
	const ENGGDepsState DepsState = FNodeDepsInstaller::GetState();

	if (DepsState == ENGGDepsState::Installing || DepsState == ENGGDepsState::Idle)
	{
		// Mirror the installer's progress ("Installing ... (1/2)") in the pill.
		const FString Detail = FNodeDepsInstaller::GetStatusDetail();
		if (StatusText.IsValid() && !Detail.IsEmpty())
		{
			StatusText->SetText(FText::FromString(Detail));
		}
		return true; // keep polling
	}

	DepsTicker.Reset(); // resolved either way — we return false below

	if (DepsState == ENGGDepsState::Failed)
	{
		AppendLine(ELineKind::Error, FNodeDepsInstaller::GetStatusDetail());
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusDepsFail",
				"npm install failed — fix Node.js/npm, then click Reconnect."));
		}
		if (ReconnectButton.IsValid())
		{
			ReconnectButton->SetVisibility(EVisibility::Visible);
		}
		return false;
	}

	// Ready — run the normal connect flow now that node can actually start.
	AppendLine(ELineKind::System, TEXT("Node.js packages installed."));
	UE_LOG(LogNGGChat, Log, TEXT("Node deps ready — resuming connect"));
	BeginConnect();
	return false;
}

void SNGGChatWindow::TickHealthProbe()
{
	if (!bAwaitingSidecar) return;

	if (FSidecarLauncher::IsSidecarAlive(0.3f))
	{
		bAwaitingSidecar = false;
		HealthTicker.Reset();
		UE_LOG(LogNGGChat, Log, TEXT("Sidecar responded — connecting WS"));
		Client->Connect(FSidecarLauncher::GetWebSocketURL(), SessionId, /*replay=*/true,
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return;
	}

	const double Now = FPlatformTime::Seconds();

	// Mid-wait retry. If the original spawn died silently (the convoluted
	// `cmd /c start /B node ...` chain occasionally fails right after the
	// previous process tree exits), one extra LaunchSidecarDetached typically
	// succeeds because the OS has by now released file/port handles.
	if (!bHealthRespawned && Now >= HealthRespawnAt)
	{
		bHealthRespawned = true;
		UE_LOG(LogNGGChat, Warning,
			TEXT("Sidecar still not alive after 4s — retrying spawn once"));
		FSidecarLauncher::LaunchSidecarDetached();
		return;
	}

	if (Now > HealthProbeDeadline)
	{
		bAwaitingSidecar = false;
		HealthTicker.Reset();
		UE_LOG(LogNGGChat, Warning, TEXT("Sidecar did not come online within 15s"));
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusSpawnFail",
				"Sidecar did not start. Check Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/index.js and NGG_NODE_PATH."));
		}
		if (ReconnectButton.IsValid())
		{
			ReconnectButton->SetVisibility(EVisibility::Visible);
		}
	}
}

void SNGGChatWindow::OnClientStateChanged(ENGGChatState NewState)
{
	FText Label;
	bool bEnableSend    = false;
	bool bEnableCancel  = false;
	bool bShowReconnect = false;

	switch (NewState)
	{
		case ENGGChatState::Disconnected:
			Label = LOCTEXT("StatusDisconnected", "Disconnected — sidecar may have exited");
			bShowReconnect = true;
			bModelPushedThisConnection = false;
			bBypassPushedThisConnection = false;
			bModelsRequestedThisConnection = false;
			break;
		case ENGGChatState::Connecting:
			Label = LOCTEXT("StatusConnecting2", "Connecting...");
			break;
		case ENGGChatState::Handshaking:
			Label = LOCTEXT("StatusHandshake", "Handshake...");
			break;
		case ENGGChatState::Ready:
			Label = LOCTEXT("StatusReady", "Ready");
			bEnableSend = true;
			// Sync the saved model to the sidecar once per connection so the
			// first spawn of `claude` uses the user's persisted choice.
			if (!bModelPushedThisConnection && Client.IsValid() && SelectedModel.IsValid())
			{
				Client->SendSetModel(SelectedModel->CliArg);
				bModelPushedThisConnection = true;
			}
			// Same for the bypass-permissions toggle: only push if the user
			// has it enabled — the sidecar default is OFF.
			if (!bBypassPushedThisConnection && bBypassPermissions && Client.IsValid())
			{
				Client->SendSetPermissionBypass(true);
				bBypassPushedThisConnection = true;
			}
			// Pull the live model catalog once per connection so the dropdown
			// reflects the account's currently-available models instead of the
			// hardcoded fallback list.
			if (!bModelsRequestedThisConnection && Client.IsValid())
			{
				Client->SendListModels();
				bModelsRequestedThisConnection = true;
			}
			break;
		case ENGGChatState::Thinking:
			Label = LOCTEXT("StatusThinking", "Thinking");
			bEnableCancel = true;
			break;
	}

	if (StatusText.IsValid())      StatusText->SetText(Label);
	if (SendButton.IsValid())      SendButton->SetEnabled(bEnableSend);
	if (CancelButton.IsValid())    CancelButton->SetEnabled(bEnableCancel);
	if (ReconnectButton.IsValid()) ReconnectButton->SetVisibility(
		bShowReconnect ? EVisibility::Visible : EVisibility::Collapsed);

	// Thinking indicator — spinner + dot-cycle ticker.
	const bool bThinking = (NewState == ENGGChatState::Thinking);
	if (ThinkingThrobber.IsValid())
	{
		ThinkingThrobber->SetVisibility(bThinking ? EVisibility::Visible : EVisibility::Collapsed);
	}
	if (bThinking)
	{
		ThinkingAnimStartTime = FPlatformTime::Seconds();
		if (!ThinkingAnimTicker.IsValid())
		{
			// Weak-ptr self-guard (mirrors SelectionLabelTicker) so the ticker
			// stops cleanly if the widget dies before its destructor runs.
			TWeakPtr<SNGGChatWindow> WeakSelfForThinking = SharedThis(this);
			ThinkingAnimTicker = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([WeakSelfForThinking](float) -> bool
				{
					TSharedPtr<SNGGChatWindow> Pinned = WeakSelfForThinking.Pin();
					if (!Pinned.IsValid()) return false; // widget gone — stop ticking
					if (!Pinned->StatusText.IsValid()) return false;
					const double Elapsed = FPlatformTime::Seconds() - Pinned->ThinkingAnimStartTime;
					const int32 Dots = (int32)(Elapsed / 0.4) % 4;
					switch (Dots)
					{
						case 0:  Pinned->StatusText->SetText(LOCTEXT("StatusThinking0", "Thinking"));    break;
						case 1:  Pinned->StatusText->SetText(LOCTEXT("StatusThinking1", "Thinking."));   break;
						case 2:  Pinned->StatusText->SetText(LOCTEXT("StatusThinking2", "Thinking..")); break;
						default: Pinned->StatusText->SetText(LOCTEXT("StatusThinking3", "Thinking...")); break;
					}
					return true; // keep ticking
				}),
				0.4f);
		}
	}
	else if (ThinkingAnimTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ThinkingAnimTicker);
		ThinkingAnimTicker.Reset();
	}

	UE_LOG(LogNGGChat, Verbose, TEXT("State -> %d"), (int32)NewState);
}

// ---------------------------------------------------------------------------
// Incoming events → terminal lines
// ---------------------------------------------------------------------------

void SNGGChatWindow::OnClientEvent(TSharedPtr<FJsonObject> Event)
{
	if (!Event.IsValid()) return;

	const FString EventType = Event->GetStringField(TEXT("type"));

	if (EventType == TEXT("assistant"))
	{
		const TSharedPtr<FJsonObject>* Msg = nullptr;
		if (Event->TryGetObjectField(TEXT("message"), Msg) && Msg && Msg->IsValid())
		{
			RenderAssistantMessage(*Msg);
		}
	}
	else if (EventType == TEXT("user"))
	{
		const TSharedPtr<FJsonObject>* Msg = nullptr;
		if (Event->TryGetObjectField(TEXT("message"), Msg) && Msg && Msg->IsValid())
		{
			RenderUserMessage(*Msg);
		}
	}
	else if (EventType == TEXT("system"))
	{
		const FString Sub = Event->HasField(TEXT("subtype"))
			? Event->GetStringField(TEXT("subtype"))
			: FString(TEXT("system"));
		FString Body;
		Event->TryGetStringField(TEXT("text"), Body);

		// The Claude CLI emits an `init` system event on every spawn with the
		// resolved model id. Capture it so the status bar can show the exact
		// model (e.g. "claude-sonnet-4-5-20250929") next to the alias dropdown.
		if (Sub == TEXT("init"))
		{
			FString ModelId;
			if (Event->TryGetStringField(TEXT("model"), ModelId) && !ModelId.IsEmpty())
			{
				ResolvedModelId = ModelId;
				if (ResolvedModelText.IsValid())
				{
					const FString Friendly = FriendlyLabelFromModelId(ModelId);
					ResolvedModelText->SetText(FText::FromString(FString::Printf(TEXT("(%s)"), *Friendly)));
					ResolvedModelText->SetToolTipText(FText::FromString(ModelId));
				}
			}
		}

		RenderSystemEvent(Sub, Body);
	}
	else if (EventType == TEXT("result"))
	{
		// Turn finished — pull cumulative token/cost data from this event
		// and refresh the status-bar usage pill.
		AccumulateUsageFromResult(Event);
	}
}

void SNGGChatWindow::AccumulateUsageFromResult(const TSharedPtr<FJsonObject>& ResultEvent)
{
	if (!ResultEvent.IsValid()) return;

	UsageTurns += 1;

	// Per-turn numbers (for the transcript one-liner) — not accumulated yet.
	int64  TurnIn = 0, TurnOut = 0, TurnCR = 0, TurnCW = 0;
	double TurnCost = 0.0;
	double TurnDurationMs = 0.0;

	ResultEvent->TryGetNumberField(TEXT("total_cost_usd"), TurnCost);
	UsageCostUsd += TurnCost;

	// Duration: stream-json emits `duration_ms` (API latency) and
	// `duration_api_ms` / `total_duration_ms` depending on CLI version.
	// Prefer the highest value available so tool-use rounds don't underreport.
	double V = 0.0;
	if (ResultEvent->TryGetNumberField(TEXT("total_duration_ms"), V)) TurnDurationMs = V;
	if (ResultEvent->TryGetNumberField(TEXT("duration_ms"),       V) && V > TurnDurationMs) TurnDurationMs = V;

	const TSharedPtr<FJsonObject>* UsageObj = nullptr;
	if (ResultEvent->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj && UsageObj->IsValid())
	{
		double UV = 0.0;
		if ((*UsageObj)->TryGetNumberField(TEXT("input_tokens"), UV))                TurnIn = (int64)UV;
		if ((*UsageObj)->TryGetNumberField(TEXT("output_tokens"), UV))               TurnOut = (int64)UV;
		if ((*UsageObj)->TryGetNumberField(TEXT("cache_read_input_tokens"), UV))     TurnCR = (int64)UV;
		if ((*UsageObj)->TryGetNumberField(TEXT("cache_creation_input_tokens"), UV)) TurnCW = (int64)UV;
	}

	UsageInputTokens      += TurnIn;
	UsageOutputTokens     += TurnOut;
	UsageCacheReadTokens  += TurnCR;
	UsageCacheWriteTokens += TurnCW;

	// Emit a compact per-turn recap line in the transcript so the developer
	// can see exactly what the just-finished request cost in tokens and time.
	const double Secs = TurnDurationMs / 1000.0;
	FString Line = FString::Printf(
		TEXT("turn %d · %s in · %s out"),
		UsageTurns,
		*FormatTokenCount(TurnIn),
		*FormatTokenCount(TurnOut));
	if (TurnCR > 0 || TurnCW > 0)
	{
		Line += FString::Printf(TEXT(" · cache %s↓/%s↑"),
			*FormatTokenCount(TurnCR), *FormatTokenCount(TurnCW));
	}
	if (TurnDurationMs > 0.0)
	{
		Line += FString::Printf(TEXT(" · %.1fs"), Secs);
	}
	AppendLine(ELineKind::System, Line);

	UpdateUsageDisplay();
	SaveUsageToConfig();
}

void SNGGChatWindow::SaveUsageToConfig()
{
	if (!GConfig) return;
	// FConfigCacheIni has no SetInt64 — round-trip int64s as strings.
	auto WriteI64 = [](const TCHAR* Key, int64 Value)
	{
		GConfig->SetString(HistoryConfigSection, Key, *LexToString(Value), GEditorPerProjectIni);
	};
	WriteI64(TEXT("UsageTurns"),        UsageTurns);
	WriteI64(TEXT("UsageInputTokens"),  UsageInputTokens);
	WriteI64(TEXT("UsageOutputTokens"), UsageOutputTokens);
	WriteI64(TEXT("UsageCacheRead"),    UsageCacheReadTokens);
	WriteI64(TEXT("UsageCacheWrite"),   UsageCacheWriteTokens);
	GConfig->SetDouble(HistoryConfigSection, TEXT("UsageCostUsd"), UsageCostUsd, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void SNGGChatWindow::LoadUsageFromConfig()
{
	if (!GConfig) return;
	auto ReadI64 = [](const TCHAR* Key, int64 Fallback) -> int64
	{
		FString Str;
		if (!GConfig->GetString(HistoryConfigSection, Key, Str, GEditorPerProjectIni) || Str.IsEmpty())
		{
			return Fallback;
		}
		int64 V = Fallback;
		LexFromString(V, *Str);
		return V;
	};
	UsageTurns            = (int32)ReadI64(TEXT("UsageTurns"),        0);
	UsageInputTokens      =        ReadI64(TEXT("UsageInputTokens"),  0);
	UsageOutputTokens     =        ReadI64(TEXT("UsageOutputTokens"), 0);
	UsageCacheReadTokens  =        ReadI64(TEXT("UsageCacheRead"),    0);
	UsageCacheWriteTokens =        ReadI64(TEXT("UsageCacheWrite"),   0);
	double Cost = 0.0;
	GConfig->GetDouble(HistoryConfigSection, TEXT("UsageCostUsd"), Cost, GEditorPerProjectIni);
	UsageCostUsd = Cost;
}

void SNGGChatWindow::ResetUsage()
{
	UsageInputTokens      = 0;
	UsageOutputTokens     = 0;
	UsageCacheReadTokens  = 0;
	UsageCacheWriteTokens = 0;
	UsageCostUsd          = 0.0;
	UsageTurns            = 0;
	UpdateUsageDisplay();
	SaveUsageToConfig();
}

FString SNGGChatWindow::FormatTokenCount(int64 N)
{
	if (N < 1000)
	{
		return FString::Printf(TEXT("%lld"), (long long)N);
	}
	if (N < 1000000)
	{
		return FString::Printf(TEXT("%.1fK"), (double)N / 1000.0);
	}
	return FString::Printf(TEXT("%.2fM"), (double)N / 1000000.0);
}

void SNGGChatWindow::UpdateUsageDisplay()
{
	if (!UsageText.IsValid()) return;

	const int64 Total = UsageInputTokens + UsageOutputTokens
	                  + UsageCacheReadTokens + UsageCacheWriteTokens;

	const FString Compact = FString::Printf(
		TEXT("%s in · %s out"),
		*FormatTokenCount(UsageInputTokens),
		*FormatTokenCount(UsageOutputTokens));
	UsageText->SetText(FText::FromString(Compact));

	const FString Tooltip = FString::Printf(
		TEXT("Token usage (this conversation):\n")
		TEXT("  turns:         %d\n")
		TEXT("  input:         %s\n")
		TEXT("  output:        %s\n")
		TEXT("  cache read:    %s\n")
		TEXT("  cache write:   %s\n")
		TEXT("  total tokens:  %s"),
		UsageTurns,
		*FormatTokenCount(UsageInputTokens),
		*FormatTokenCount(UsageOutputTokens),
		*FormatTokenCount(UsageCacheReadTokens),
		*FormatTokenCount(UsageCacheWriteTokens),
		*FormatTokenCount(Total));
	UsageText->SetToolTipText(FText::FromString(Tooltip));
}

void SNGGChatWindow::OnClientError(FString Message)
{
	AppendLine(ELineKind::Error, Message);
}

void SNGGChatWindow::RenderSystemEvent(const FString& Subtype, const FString& Body)
{
	// Local-command output carries its payload in the `text` field and can
	// span multiple lines — render each line as its own [system] entry so the
	// mono-wrap in the transcript stays readable.
	if (Subtype == TEXT("local_command") && !Body.IsEmpty())
	{
		TArray<FString> Lines;
		Body.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
		for (const FString& L : Lines)
		{
			AppendLine(ELineKind::System, L);
		}
		return;
	}

	// Pretty-print known debug subtypes emitted by the sidecar / MCP server.
	if (Subtype.StartsWith(TEXT("sidecar_ready:")))
	{
		// Payload format: "version|pid|sessionId"
		TArray<FString> Parts;
		Subtype.Mid(14).ParseIntoArray(Parts, TEXT("|"), /*bCullEmpty=*/true);
		const FString Ver  = Parts.IsValidIndex(0) ? Parts[0] : TEXT("?");
		const FString Pid  = Parts.IsValidIndex(1) ? Parts[1] : TEXT("?");
		const FString Sess = Parts.IsValidIndex(2) ? Parts[2] : TEXT("?");
		AppendLine(ELineKind::System,
			FString::Printf(TEXT("Sidecar v%s (pid %s) — session %s"), *Ver, *Pid, *Sess));
		return;
	}
	if (Subtype.StartsWith(TEXT("mcp:")))
	{
		AppendLine(ELineKind::System,
			FString::Printf(TEXT("MCP servers: %s"), *Subtype.Mid(4)));
		return;
	}
	if (Subtype.StartsWith(TEXT("permission_mode:")))
	{
		AppendLine(ELineKind::System,
			FString::Printf(TEXT("Permission mode: %s"), *Subtype.Mid(16)));
		return;
	}
	if (Subtype.StartsWith(TEXT("model:")))
	{
		const FString M = Subtype.Mid(6);
		AppendLine(ELineKind::System,
			M.IsEmpty() ? TEXT("Model: default") : *FString::Printf(TEXT("Model: %s"), *M));
		return;
	}

	// Fallback: if we got a text body, show it; otherwise show the bare subtype.
	AppendLine(ELineKind::System, Body.IsEmpty() ? Subtype : Body);
}

void SNGGChatWindow::RenderAssistantMessage(const TSharedPtr<FJsonObject>& Message)
{
	if (!Message.IsValid()) return;

	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	if (!Message->TryGetArrayField(TEXT("content"), Content) || !Content) return;

	for (const TSharedPtr<FJsonValue>& V : *Content)
	{
		const TSharedPtr<FJsonObject> Part = V.IsValid() ? V->AsObject() : nullptr;
		if (!Part.IsValid()) continue;

		const FString PartType = Part->GetStringField(TEXT("type"));
		if (PartType == TEXT("thinking"))
		{
			FString ThinkingText;
			Part->TryGetStringField(TEXT("thinking"), ThinkingText);
			if (!ThinkingText.IsEmpty())
			{
				RenderTextWithCodeBlocks(ThinkingText, ELineKind::Thinking);
			}
		}
		else if (PartType == TEXT("text"))
		{
			const FString Txt = Part->GetStringField(TEXT("text"));
			RenderTextWithCodeBlocks(Txt);
		}
		else if (PartType == TEXT("tool_use"))
		{
			const FString Name = Part->GetStringField(TEXT("name"));
			const TSharedPtr<FJsonObject>* InputObj = nullptr;
			FString Summary;
			if (Part->TryGetObjectField(TEXT("input"), InputObj) && InputObj)
			{
				Summary = SummariseToolInput(Name, *InputObj);
			}
			const FString Body = Summary.IsEmpty()
				? FString::Printf(TEXT("%s"), *Name)
				: FString::Printf(TEXT("%s %s"), *Name, *Summary);
			AppendLine(ELineKind::ToolUse, Body);
		}
	}
}

TOptional<FNGGPastedImage> SNGGChatWindow::TrySavePastedImage()
{
#if PLATFORM_WINDOWS
	if (!OpenClipboard(NULL)) return {};

	HANDLE hDib = GetClipboardData(CF_DIB);
	if (!hDib)
	{
		CloseClipboard();
		return {};
	}

	void* pDib = GlobalLock(hDib);
	if (!pDib)
	{
		CloseClipboard();
		return {};
	}

	const BITMAPINFOHEADER* Bih       = static_cast<const BITMAPINFOHEADER*>(pDib);
	const int32             Width     = Bih->biWidth;
	const int32             Height    = FMath::Abs((int32)Bih->biHeight);
	const int32             BitCount  = Bih->biBitCount;
	const bool              bTopDown  = Bih->biHeight < 0;

	TArray<uint8> RGBAPixels;
	bool bConverted = false;

	if ((BitCount == 24 || BitCount == 32) && Width > 0 && Height > 0)
	{
		const int32    NumColors   = (Bih->biClrUsed > 0) ? (int32)Bih->biClrUsed : 0;
		const uint8*   pPixels     = static_cast<const uint8*>(pDib) + Bih->biSize + NumColors * sizeof(RGBQUAD);
		const int32    BytesPerPix = BitCount / 8;
		const int32    RowStride   = ((Width * BytesPerPix + 3) / 4) * 4; // DWORD-aligned

		RGBAPixels.SetNumUninitialized(Width * Height * 4);

		for (int32 Y = 0; Y < Height; ++Y)
		{
			const int32  SrcY   = bTopDown ? Y : (Height - 1 - Y);
			const uint8* SrcRow = pPixels + SrcY * RowStride;
			uint8*       DstRow = RGBAPixels.GetData() + Y * Width * 4;

			for (int32 X = 0; X < Width; ++X)
			{
				const uint8* Src = SrcRow + X * BytesPerPix;
				uint8*       Dst = DstRow + X * 4;
				Dst[0] = Src[2];                             // R (DIB stores BGR)
				Dst[1] = Src[1];                             // G
				Dst[2] = Src[0];                             // B
				Dst[3] = (BitCount == 32) ? Src[3] : 255;   // A
			}
		}
		bConverted = true;
	}

	GlobalUnlock(hDib);
	CloseClipboard();

	if (!bConverted) return {};

	IImageWrapperModule& WrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = WrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid()) return {};

	if (!Wrapper->SetRaw(RGBAPixels.GetData(), RGBAPixels.Num(), Width, Height, ERGBFormat::RGBA, 8))
		return {};

	const TArray64<uint8>& PngData = Wrapper->GetCompressed(100);
	if (PngData.IsEmpty()) return {};

	// Encode to base64 for inline API delivery (no temp file needed).
	// FBase64::Encode expects TArray<uint8>; copy from TArray64 (clipboard images are small).
	TArray<uint8> PngBytes;
	PngBytes.Append(PngData.GetData(), (int32)PngData.Num());

	FNGGPastedImage Result;
	Result.MediaType  = TEXT("image/png");
	Result.Base64Data = FBase64::Encode(PngBytes);
	Result.Width      = Width;
	Result.Height     = Height;
	return Result;
#else
	return {};
#endif
}

void SNGGChatWindow::RenderTextWithCodeBlocks(const FString& Text, ELineKind Kind)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

	bool bInCode = false;
	FString CodeLang;
	TArray<FString> CodeLines;
	TArray<FString> ProseLines;

	auto FlushProse = [&]()
	{
		if (ProseLines.Num() == 0) return;
		FString Joined;
		for (int32 i = 0; i < ProseLines.Num(); ++i)
		{
			if (i > 0) Joined += TEXT("\n");
			Joined += ProseLines[i];
		}
		Joined.TrimStartAndEndInline();
		if (!Joined.IsEmpty())
			AppendLine(Kind, Joined);
		ProseLines.Empty();
	};

	auto FlushCode = [&]()
	{
		FString Joined;
		for (int32 i = 0; i < CodeLines.Num(); ++i)
		{
			if (i > 0) Joined += TEXT("\n");
			Joined += CodeLines[i];
		}
		AppendCodeBlock(CodeLang, Joined);
		CodeLines.Empty();
		CodeLang.Empty();
	};

	for (const FString& Line : Lines)
	{
		if (!bInCode && Line.StartsWith(TEXT("```")))
		{
			FlushProse();
			bInCode = true;
			CodeLang = Line.Mid(3).TrimStartAndEnd();
		}
		else if (bInCode && Line.TrimStart() == TEXT("```"))
		{
			FlushCode();
			bInCode = false;
		}
		else if (bInCode)
		{
			CodeLines.Add(Line);
		}
		else
		{
			ProseLines.Add(Line);
		}
	}

	FlushProse();
	if (bInCode) FlushCode(); // handle unclosed fence
}

void SNGGChatWindow::AppendCodeBlock(const FString& Lang, const FString& CodeBody)
{
	// Emit a fenced block: a muted language header followed by the escaped
	// code body wrapped in the code style (green). Everything is still part
	// of the one selectable transcript text.
	if (!TranscriptText.IsValid()) return;

	const FString LangLower = Lang.ToLower();
	const FString HeaderLabel = LangLower.IsEmpty()
		? FString(TEXT("[code]"))
		: FString::Printf(TEXT("[code/%s]"), *LangLower);

	TranscriptBuffer += FString::Printf(TEXT("<NGGCodeLabel>%s</>\n"),
		*EscapeRichMarkup(HeaderLabel));

	// Split code body into individual lines so no span crosses a newline boundary
	// (the marshaller processes line-by-line; mid-span newlines lose styling).
	TArray<FString> CodeBodyLines;
	CodeBody.ParseIntoArrayLines(CodeBodyLines, /*bCullEmpty=*/false);
	for (const FString& Line : CodeBodyLines)
	{
		TranscriptBuffer += FString::Printf(TEXT("<NGGCode>%s</>\n"),
			*EscapeRichMarkup(Line));
	}

	TrimTranscriptBuffer();
	TranscriptText->SetText(FText::FromString(TranscriptBuffer));
	MaybeAutoScroll();
}

// ---------------------------------------------------------------------------
// Syntax-highlighted block renderer
// ---------------------------------------------------------------------------
void SNGGChatWindow::AppendHighlightedBlock(const TArray<FTokenLine>& Lines, const FString& LangLabel)
{
	// Emit one rich-text run per token so the marshaller can color each run
	// independently via the NGGSyntax* styles registered in Construct.
	// Newlines sit outside style tags as literal '\n' separators.
	if (!TranscriptText.IsValid()) return;

	TranscriptBuffer += FString::Printf(TEXT("<NGGCodeLabel>%s</>\n"),
		*EscapeRichMarkup(LangLabel));

	// Fallback style for any token whose Style name wasn't registered.
	static const FName FallbackStyle(TEXT("NGGCode"));

	for (const FTokenLine& Line : Lines)
	{
		for (const FColoredToken& Tok : Line)
		{
			if (Tok.Text.IsEmpty()) continue;
			const FName StyleName = Tok.Style.IsNone() ? FallbackStyle : Tok.Style;
			TranscriptBuffer += FString::Printf(TEXT("<%s>%s</>"),
				*StyleName.ToString(),
				*EscapeRichMarkup(Tok.Text));
		}
		TranscriptBuffer += TEXT("\n");
	}

	TrimTranscriptBuffer();
	TranscriptText->SetText(FText::FromString(TranscriptBuffer));
	MaybeAutoScroll();
}

// ---------------------------------------------------------------------------
// JSON tokenizer
// ---------------------------------------------------------------------------
TArray<SNGGChatWindow::FTokenLine> SNGGChatWindow::TokenizeJson(const FString& Text) const
{
	// Style names — must match TranscriptStyle registrations in Construct.
	static const FName StyleKey    = FName(TEXT("NGGSyntaxJsonKey"));    // sky-blue
	static const FName StyleStr    = FName(TEXT("NGGSyntaxJsonStr"));    // soft-green
	static const FName StyleNum    = FName(TEXT("NGGSyntaxJsonNum"));    // orange
	static const FName StyleBool   = FName(TEXT("NGGSyntaxJsonBool"));   // pink
	static const FName StyleStruct = FName(TEXT("NGGSyntaxJsonStruct")); // mid-gray

	TArray<FTokenLine> Lines;
	FTokenLine CurLine;
	const int32 Len = Text.Len();
	int32 i = 0;

	auto Emit = [&](const FString& Tok, FName Style)
	{
		if (!Tok.IsEmpty()) CurLine.Add({ Tok, Style });
	};
	auto NewLine = [&]()
	{
		Lines.Add(MoveTemp(CurLine));
		CurLine.Empty();
	};

	while (i < Len)
	{
		const TCHAR Ch = Text[i];

		if (Ch == TEXT('\n')) { NewLine(); ++i; continue; }
		if (Ch == TEXT('\r')) { ++i; continue; }

		// Whitespace — preserve as structural
		if (Ch == TEXT(' ') || Ch == TEXT('\t'))
		{
			FString WS;
			while (i < Len && (Text[i] == TEXT(' ') || Text[i] == TEXT('\t')))
				WS += Text[i++];
			Emit(WS, StyleStruct);
			continue;
		}

		// String — peek ahead after closing quote to decide key vs value
		if (Ch == TEXT('"'))
		{
			FString Str = TEXT("\"");
			++i;
			bool bEsc = false;
			while (i < Len)
			{
				const TCHAR C2 = Text[i++];
				Str += C2;
				if (bEsc) { bEsc = false; continue; }
				if (C2 == TEXT('\\')) { bEsc = true; continue; }
				if (C2 == TEXT('"')) break;
			}
			// Peek past whitespace for ':'
			int32 j = i;
			while (j < Len && (Text[j] == TEXT(' ') || Text[j] == TEXT('\t'))) ++j;
			const bool bIsKey = (j < Len && Text[j] == TEXT(':'));
			Emit(Str, bIsKey ? StyleKey : StyleStr);
			continue;
		}

		// Number (or negative)
		if (FChar::IsDigit(Ch) || (Ch == TEXT('-') && i + 1 < Len && FChar::IsDigit(Text[i + 1])))
		{
			FString Num;
			if (Ch == TEXT('-')) { Num += Text[i++]; }
			while (i < Len && (FChar::IsDigit(Text[i]) || Text[i] == TEXT('.') ||
				Text[i] == TEXT('e') || Text[i] == TEXT('E') ||
				Text[i] == TEXT('+') || Text[i] == TEXT('-')))
				Num += Text[i++];
			Emit(Num, StyleNum);
			continue;
		}

		// Alpha — true / false / null
		if (FChar::IsAlpha(Ch))
		{
			FString Lit;
			while (i < Len && FChar::IsAlpha(Text[i])) Lit += Text[i++];
			const bool bBool = (Lit == TEXT("true") || Lit == TEXT("false") || Lit == TEXT("null"));
			Emit(Lit, bBool ? StyleBool : StyleStruct);
			continue;
		}

		// Structural char
		Emit(FString(1, &Ch), StyleStruct);
		++i;
	}

	if (!CurLine.IsEmpty()) Lines.Add(MoveTemp(CurLine));
	return Lines;
}

// ---------------------------------------------------------------------------
// C++ tokenizer
// ---------------------------------------------------------------------------
TArray<SNGGChatWindow::FTokenLine> SNGGChatWindow::TokenizeCpp(const FString& Code) const
{
	// Style names — must match TranscriptStyle registrations in Construct.
	static const FName StyleKeyword = FName(TEXT("NGGSyntaxCppKeyword")); // soft-purple
	static const FName StyleType    = FName(TEXT("NGGSyntaxCppType"));    // teal
	static const FName StyleString  = FName(TEXT("NGGSyntaxCppString"));  // amber
	static const FName StyleComment = FName(TEXT("NGGSyntaxCppComment")); // muted-green
	static const FName StylePreproc = FName(TEXT("NGGSyntaxCppPreproc")); // mauve
	static const FName StyleNumber  = FName(TEXT("NGGSyntaxCppNumber"));  // orange
	static const FName StyleIdent   = FName(TEXT("NGGSyntaxCppIdent"));   // near-white
	static const FName StyleOp      = FName(TEXT("NGGSyntaxCppOp"));      // gray

	// Keyword set (built once)
	static const TSet<FString> KW = []()
	{
		TSet<FString> S;
		static const TCHAR* const List[] = {
			TEXT("if"),TEXT("else"),TEXT("for"),TEXT("while"),TEXT("do"),
			TEXT("switch"),TEXT("case"),TEXT("break"),TEXT("continue"),TEXT("return"),
			TEXT("void"),TEXT("bool"),TEXT("int"),TEXT("float"),TEXT("double"),
			TEXT("char"),TEXT("auto"),TEXT("const"),TEXT("static"),TEXT("virtual"),
			TEXT("override"),TEXT("final"),TEXT("explicit"),TEXT("inline"),
			TEXT("extern"),TEXT("template"),TEXT("typename"),TEXT("class"),
			TEXT("struct"),TEXT("enum"),TEXT("namespace"),TEXT("using"),
			TEXT("public"),TEXT("private"),TEXT("protected"),TEXT("new"),
			TEXT("delete"),TEXT("nullptr"),TEXT("true"),TEXT("false"),TEXT("this"),
			TEXT("operator"),TEXT("friend"),TEXT("sizeof"),TEXT("decltype"),
			TEXT("constexpr"),TEXT("noexcept"),TEXT("throw"),TEXT("try"),TEXT("catch"),
			TEXT("mutable"),TEXT("volatile"),TEXT("typedef"),TEXT("default"),
		};
		for (const TCHAR* K : List) S.Add(K);
		return S;
	}();

	// UE5 macros / reflection specifiers
	static const TSet<FString> UEMacros = []()
	{
		TSet<FString> S;
		static const TCHAR* const List[] = {
			TEXT("TEXT"),TEXT("UFUNCTION"),TEXT("UPROPERTY"),TEXT("UCLASS"),
			TEXT("USTRUCT"),TEXT("UENUM"),TEXT("FORCEINLINE"),TEXT("DOREPLIFETIME"),
			TEXT("DOREPLIFETIME_CONDITION"),TEXT("check"),TEXT("checkf"),
			TEXT("ensure"),TEXT("ensureMsgf"),TEXT("HasAuthority"),
			TEXT("UE_LOG"),TEXT("DECLARE_DYNAMIC_MULTICAST_DELEGATE"),
			TEXT("DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam"),
			TEXT("DECLARE_DELEGATE"),TEXT("GENERATED_BODY"),
		};
		for (const TCHAR* K : List) S.Add(K);
		return S;
	}();

	auto ClassifyWord = [&](const FString& W) -> FName
	{
		if (KW.Contains(W))      return StyleKeyword;
		if (UEMacros.Contains(W)) return StylePreproc;
		if (W.Len() < 2)          return StyleIdent;
		// ALL_CAPS_UNDERSCORE → macro
		bool bAllCaps = true;
		for (TCHAR C : W) { if (!FChar::IsUpper(C) && !FChar::IsDigit(C) && C != TEXT('_')) { bAllCaps = false; break; } }
		if (bAllCaps && W.Contains(TEXT("_"))) return StylePreproc;
		// UE5 prefix pattern (A/U/F/T/I/E + uppercase)
		const TCHAR First = W[0];
		if (FChar::IsUpper(First)) return StyleType;
		return StyleIdent;
	};

	TArray<FTokenLine> Lines;

	TArray<FString> RawLines;
	Code.ParseIntoArrayLines(RawLines, false);

	for (const FString& Line : RawLines)
	{
		FTokenLine TLine;
		const int32 Len = Line.Len();
		int32 i = 0;

		auto EmitT = [&](const FString& Tok, FName Style)
		{
			if (!Tok.IsEmpty()) TLine.Add({ Tok, Style });
		};

		// Preprocessor directive — whole line in mauve
		{
			int32 FirstNonSpace = 0;
			while (FirstNonSpace < Len && (Line[FirstNonSpace] == TEXT(' ') || Line[FirstNonSpace] == TEXT('\t')))
				++FirstNonSpace;
			if (FirstNonSpace < Len && Line[FirstNonSpace] == TEXT('#'))
			{
				TLine.Add({ Line, StylePreproc });
				Lines.Add(MoveTemp(TLine));
				continue;
			}
		}

		// Find position of first '//' not inside a string
		int32 CommentStart = INDEX_NONE;
		{
			bool bInStr = false; bool bEsc = false;
			for (int32 k = 0; k < Len - 1; ++k)
			{
				if (bInStr)
				{
					if (bEsc) { bEsc = false; continue; }
					if (Line[k] == TEXT('\\')) { bEsc = true; continue; }
					if (Line[k] == TEXT('"')) bInStr = false;
					continue;
				}
				if (Line[k] == TEXT('"')) { bInStr = true; continue; }
				if (Line[k] == TEXT('/') && Line[k + 1] == TEXT('/')) { CommentStart = k; break; }
			}
		}

		const int32 CodeEnd = (CommentStart != INDEX_NONE) ? CommentStart : Len;

		while (i < CodeEnd)
		{
			const TCHAR Ch = Line[i];

			// Whitespace
			if (Ch == TEXT(' ') || Ch == TEXT('\t'))
			{
				FString WS;
				while (i < CodeEnd && (Line[i] == TEXT(' ') || Line[i] == TEXT('\t')))
					WS += Line[i++];
				EmitT(WS, StyleOp);
				continue;
			}

			// String literal
			if (Ch == TEXT('"'))
			{
				FString Str = TEXT("\"");
				++i;
				bool bEsc = false;
				while (i < CodeEnd)
				{
					const TCHAR C2 = Line[i++];
					Str += C2;
					if (bEsc) { bEsc = false; continue; }
					if (C2 == TEXT('\\')) { bEsc = true; continue; }
					if (C2 == TEXT('"')) break;
				}
				EmitT(Str, StyleString);
				continue;
			}

			// Char literal
			if (Ch == TEXT('\''))
			{
				FString Str = TEXT("'");
				++i;
				bool bEsc = false;
				while (i < CodeEnd)
				{
					const TCHAR C2 = Line[i++];
					Str += C2;
					if (bEsc) { bEsc = false; continue; }
					if (C2 == TEXT('\\')) { bEsc = true; continue; }
					if (C2 == TEXT('\'')) break;
				}
				EmitT(Str, StyleString);
				continue;
			}

			// Word (keyword / identifier / type)
			if (FChar::IsAlpha(Ch) || Ch == TEXT('_'))
			{
				FString Word;
				while (i < CodeEnd && (FChar::IsAlnum(Line[i]) || Line[i] == TEXT('_')))
					Word += Line[i++];
				EmitT(Word, ClassifyWord(Word));
				continue;
			}

			// Number
			if (FChar::IsDigit(Ch))
			{
				FString Num;
				while (i < CodeEnd && (FChar::IsAlnum(Line[i]) || Line[i] == TEXT('.') ||
					Line[i] == TEXT('x') || Line[i] == TEXT('X') ||
					Line[i] == TEXT('+') || Line[i] == TEXT('-')))
					Num += Line[i++];
				EmitT(Num, StyleNumber);
				continue;
			}

			// Operator / other
			EmitT(FString(1, &Ch), StyleOp);
			++i;
		}

		// Comment suffix
		if (CommentStart != INDEX_NONE)
			TLine.Add({ Line.Mid(CommentStart), StyleComment });

		Lines.Add(MoveTemp(TLine));
	}

	return Lines;
}

void SNGGChatWindow::RenderUserMessage(const TSharedPtr<FJsonObject>& Message)
{
	if (!Message.IsValid()) return;

	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	// user.message.content may be a plain string OR an array of parts.
	FString SimpleText;
	if (Message->TryGetStringField(TEXT("content"), SimpleText))
	{
		AppendLine(ELineKind::User, SimpleText);
		return;
	}
	if (!Message->TryGetArrayField(TEXT("content"), Content) || !Content) return;

	for (const TSharedPtr<FJsonValue>& V : *Content)
	{
		const TSharedPtr<FJsonObject> Part = V.IsValid() ? V->AsObject() : nullptr;
		if (!Part.IsValid()) continue;

		const FString PartType = Part->GetStringField(TEXT("type"));
		if (PartType == TEXT("text"))
		{
			AppendLine(ELineKind::User, Part->GetStringField(TEXT("text")));
		}
		else if (PartType == TEXT("tool_result"))
		{
			FString ResultText;
			FString Raw;
			bool bIsError = false;
			Part->TryGetBoolField(TEXT("is_error"), bIsError);

			if (Part->TryGetStringField(TEXT("content"), Raw))
			{
				ResultText = Raw;
			}
			else
			{
				const TArray<TSharedPtr<FJsonValue>>* InnerArr = nullptr;
				if (Part->TryGetArrayField(TEXT("content"), InnerArr) && InnerArr)
				{
					for (const TSharedPtr<FJsonValue>& IV : *InnerArr)
					{
						const TSharedPtr<FJsonObject> InnerPart = IV.IsValid() ? IV->AsObject() : nullptr;
						if (InnerPart.IsValid())
						{
							FString T;
							if (InnerPart->TryGetStringField(TEXT("text"), T))
							{
								if (!ResultText.IsEmpty()) ResultText += TEXT("\n");
								ResultText += T;
							}
						}
					}
				}
			}

			// Normalize line endings.
			ResultText.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
			ResultText.ReplaceInline(TEXT("\r"), TEXT("\n"));

			if (ResultText.IsEmpty())
			{
				AppendLine(bIsError ? ELineKind::ToolErr : ELineKind::ToolOk, TEXT("(empty result)"));
			}
			else if (bIsError)
			{
				AppendLine(ELineKind::ToolErr, ResultText);
			}
			else if (ResultText.Contains(TEXT("\n")))
			{
				AppendLine(ELineKind::ToolOk, TEXT("↓"));
				const FString Trimmed = ResultText.TrimStart();
				if (Trimmed.StartsWith(TEXT("{")) || Trimmed.StartsWith(TEXT("[")))
				{
					// JSON response from MCP tool
					AppendHighlightedBlock(TokenizeJson(ResultText), TEXT("[json]"));
				}
				else if (ResultText.Contains(TEXT("::")) || ResultText.Contains(TEXT("->"))
					|| ResultText.Contains(TEXT("UPROPERTY")) || ResultText.Contains(TEXT("UFUNCTION"))
					|| (ResultText.Contains(TEXT(";")) && ResultText.Contains(TEXT("{"))))
				{
					// Looks like C++ source (file read, grep result)
					AppendHighlightedBlock(TokenizeCpp(ResultText), TEXT("[cpp]"));
				}
				else
				{
					// Generic text (bash output, plain file, etc.) — flat green
					AppendCodeBlock(TEXT(""), ResultText);
				}
			}
			else
			{
				AppendLine(ELineKind::ToolOk, ResultText);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Tool-input summariser — one-line UX-oriented preview of a tool call
// ---------------------------------------------------------------------------
FString SNGGChatWindow::SummariseToolInput(const FString& ToolName, const TSharedPtr<FJsonObject>& Input) const
{
	if (!Input.IsValid()) return FString();

	auto PickString = [&Input](const TCHAR* Key, FString& Out) -> bool
	{
		return Input->TryGetStringField(Key, Out);
	};

	FString S;
	if (ToolName == TEXT("Write") || ToolName == TEXT("Edit") || ToolName == TEXT("NotebookEdit"))
	{
		if (PickString(TEXT("file_path"), S)) return FString::Printf(TEXT("{file_path:\"%s\"}"), *S);
	}
	else if (ToolName == TEXT("Read"))
	{
		if (PickString(TEXT("file_path"), S)) return FString::Printf(TEXT("{file_path:\"%s\"}"), *S);
	}
	else if (ToolName == TEXT("Bash"))
	{
		if (PickString(TEXT("command"), S))
		{
			FString Shortened = TruncateForLine(S);
			Shortened.ReplaceInline(TEXT("\n"), TEXT(" "));
			return FString::Printf(TEXT("{command:\"%s\"}"), *Shortened);
		}
	}
	else if (ToolName == TEXT("Grep") || ToolName == TEXT("Glob"))
	{
		if (PickString(TEXT("pattern"), S)) return FString::Printf(TEXT("{pattern:\"%s\"}"), *S);
	}

	// NGG MCP tools (mcp__ue5-ngg__*) emit massive JSON payloads — bp_add_logic
	// in particular ships entire node graphs (18 nodes + N connections is common).
	// Render a compact summary keyed on the most useful identifying fields, plus
	// counts of the heavy array fields, so the transcript stays readable.
	if (ToolName.StartsWith(TEXT("mcp__ue5-ngg__")))
	{
		TArray<FString> Parts;

		auto AppendString = [&](const TCHAR* Key)
		{
			FString V;
			if (Input->TryGetStringField(Key, V) && !V.IsEmpty())
			{
				// Trim very long string values so a single huge field doesn't
				// blow up the line either.
				FString Display = V.Len() > 80 ? V.Left(77) + TEXT("...") : V;
				Display.ReplaceInline(TEXT("\n"), TEXT(" "));
				Parts.Add(FString::Printf(TEXT("%s:\"%s\""), Key, *Display));
			}
		};

		auto AppendArrayCount = [&](const TCHAR* Key, const TCHAR* Label)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Input->TryGetArrayField(Key, Arr) && Arr && Arr->Num() > 0)
			{
				Parts.Add(FString::Printf(TEXT("%s:%d"), Label, Arr->Num()));
			}
		};

		// Path-like fields and the graph being edited.
		for (const TCHAR* K : { TEXT("blueprint"), TEXT("path"), TEXT("dest_path"),
		                        TEXT("asset_path"), TEXT("actor"), TEXT("level"),
		                        TEXT("level_path"), TEXT("graph"), TEXT("widget") })
		{
			AppendString(K);
		}
		// Identifiers / type info.
		for (const TCHAR* K : { TEXT("name"), TEXT("class_name"), TEXT("parent"),
		                        TEXT("parent_class"), TEXT("variable"),
		                        TEXT("function"), TEXT("node_type"), TEXT("event"),
		                        TEXT("component"), TEXT("property"), TEXT("type") })
		{
			AppendString(K);
		}
		// Heavy array fields — show counts only.
		AppendArrayCount(TEXT("nodes"),       TEXT("nodes"));
		AppendArrayCount(TEXT("connections"), TEXT("conns"));
		AppendArrayCount(TEXT("pins"),        TEXT("pins"));
		AppendArrayCount(TEXT("variables"),   TEXT("vars"));
		AppendArrayCount(TEXT("components"),  TEXT("comps"));
		AppendArrayCount(TEXT("operations"),  TEXT("ops"));
		AppendArrayCount(TEXT("calls"),       TEXT("calls"));
		AppendArrayCount(TEXT("widgets"),     TEXT("widgets"));
		AppendArrayCount(TEXT("actors"),      TEXT("actors"));
		AppendArrayCount(TEXT("properties"),  TEXT("props"));

		if (Parts.Num() > 0)
		{
			return TEXT("{") + FString::Join(Parts, TEXT(", ")) + TEXT("}");
		}
		// If we couldn't extract anything useful, fall through to the truncated
		// JSON fallback below — but cap it harder than the generic 2000 limit.
		const FString Raw = StringifyJson(Input);
		return Raw.Len() > 200 ? Raw.Left(200) + TEXT("…") : Raw;
	}

	// Fallback: compact JSON, truncated to MaxLineChars.
	const FString Raw = StringifyJson(Input);
	return TruncateForLine(Raw);
}

// ---------------------------------------------------------------------------
// Permission request handling
// ---------------------------------------------------------------------------
void SNGGChatWindow::OnClientPermissionRequest(TSharedPtr<FJsonObject> Request)
{
	if (!Request.IsValid() || !Client.IsValid()) return;

	const FString Id   = Request->GetStringField(TEXT("id"));
	const FString Tool = Request->GetStringField(TEXT("tool"));
	if (Id.IsEmpty()) return;

	// De-dupe across replay.
	if (SeenPermissionIds.Contains(Id)) return;
	SeenPermissionIds.Add(Id);

	// If the sidecar marked this as already settled (replay of a handled prompt),
	// render a passive historical line instead of live approval buttons.
	bool bSettled = false;
	FString SettledDecision;
	Request->TryGetBoolField(TEXT("settled"), bSettled);
	Request->TryGetStringField(TEXT("decision"), SettledDecision);
	if (bSettled)
	{
		const FString Label = (SettledDecision == TEXT("approve"))
			? FString::Printf(TEXT("Permission was granted: %s"), *Tool)
			: FString::Printf(TEXT("Permission was denied: %s"), *Tool);
		AppendLine(ELineKind::System, Label);
		return;
	}

	const TSharedPtr<FJsonObject>* InputObj = nullptr;
	Request->TryGetObjectField(TEXT("input"), InputObj);
	const FString Summary = (InputObj && InputObj->IsValid())
		? SummariseToolInput(Tool, *InputObj) : FString();


	// Auto-approve if user previously clicked "Always" for this tool.
	if (SessionAutoApproveTools.Contains(Tool))
	{
		Client->SendPermissionResponse(Id, TEXT("approve"));
		AppendLine(ELineKind::Perm,
			FString::Printf(TEXT("Auto-approved: %s (session-always allowed)"), *Tool));
		return;
	}

	const FString BodyPreview = Summary.IsEmpty()
		? FString::Printf(TEXT("Claude wants to use %s"), *Tool)
		: FString::Printf(TEXT("Claude wants to use %s  %s"), *Tool, *Summary);

	AppendPermissionPrompt(Id, Tool, BodyPreview);
}

// ---------------------------------------------------------------------------
// Low-level line rendering
// ---------------------------------------------------------------------------

FString SNGGChatWindow::StringifyJson(const TSharedPtr<FJsonObject>& Obj) const
{
	if (!Obj.IsValid()) return FString();
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Out;
}

FString SNGGChatWindow::TruncateForLine(const FString& In) const
{
	if (In.Len() <= MaxLineChars) return In;
	return In.Left(MaxLineChars) + TEXT("…");
}

TSharedRef<SWidget> SNGGChatWindow::AppendLine(ELineKind Kind, const FString& Body)
{
	if (!TranscriptText.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	// Emit each line of Body as its own styled span. The FRichTextLayoutMarshaller
	// processes text line-by-line; a span that contains \n causes all lines after
	// the first to lose their style and renders the closing </> as literal text.
	TArray<FString> BodyLines;
	Body.ParseIntoArrayLines(BodyLines, /*bCullEmpty=*/false);
	if (BodyLines.Num() == 0) BodyLines.Add(FString());

	for (int32 i = 0; i < BodyLines.Num(); ++i)
	{
		const FString Escaped = EscapeRichMarkup(TruncateForLine(BodyLines[i]));
		if (i == 0)
			TranscriptBuffer += FString::Printf(TEXT("<%s>%s %s</>\n"),
				TagForKind(Kind), PrefixForKind(Kind), *Escaped);
		else
			TranscriptBuffer += FString::Printf(TEXT("<%s>%s</>\n"),
				TagForKind(Kind), *Escaped);
	}

	TrimTranscriptBuffer();
	TranscriptText->SetText(FText::FromString(TranscriptBuffer));
	MaybeAutoScroll();
	return TranscriptText.ToSharedRef();
}

void SNGGChatWindow::AppendPermissionPrompt(const FString& RequestId, const FString& Tool, const FString& InputSummary)
{
	if (!PendingPermsList.IsValid() || !Client.IsValid()) return;

	// Transcript records the request as plain text so it's copyable; the
	// action row lives in the pending-perms panel above the input.
	AppendLine(ELineKind::Perm, InputSummary);

	const FSlateFontInfo MonoFont = FCoreStyle::GetDefaultFontStyle("Mono", 10);
	const FLinearColor PermColor = ColorForKind(ELineKind::Perm);

	TSharedPtr<SHorizontalBox> Row;
	SAssignNew(Row, SHorizontalBox);
	TWeakPtr<SHorizontalBox> WeakRow = Row;

	auto RemoveRow = [this, WeakRow]()
	{
		if (TSharedPtr<SHorizontalBox> Pinned = WeakRow.Pin())
		{
			if (PendingPermsList.IsValid())
			{
				PendingPermsList->RemoveSlot(Pinned.ToSharedRef());
			}
		}
		if (PendingPermsList.IsValid() && PendingPermsList->NumSlots() == 0 && PendingPermsBorder.IsValid())
		{
			PendingPermsBorder->SetVisibility(EVisibility::Collapsed);
		}
	};

	Row->AddSlot()
	.FillWidth(1.f)
	.VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Text(FText::FromString(FString::Printf(TEXT("%s %s"),
			PrefixForKind(ELineKind::Perm), *InputSummary)))
		.Font(MonoFont)
		.ColorAndOpacity(FSlateColor(PermColor))
		.AutoWrapText(true)
	];

	Row->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(4.f, 0.f)
	[
		SNew(SButton)
		.ButtonStyle(&NGGBrand::GreenButton())
		.Text(LOCTEXT("PermApprove", "Approve"))
		.OnClicked_Lambda([this, RequestId, Tool, RemoveRow]() -> FReply
		{
			RemoveRow();
			if (Client.IsValid()) Client->SendPermissionResponse(RequestId, TEXT("approve"));
			AppendLine(ELineKind::System, FString::Printf(TEXT("Approved: %s"), *Tool));
			return FReply::Handled();
		})
	];

	Row->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(4.f, 0.f)
	[
		SNew(SButton)
		.ButtonStyle(&NGGBrand::SecondaryButton())
		.Text(LOCTEXT("PermAlways", "Always"))
		.ToolTipText(LOCTEXT("PermAlwaysTT", "Approve and auto-approve this tool for the rest of the session"))
		.OnClicked_Lambda([this, RequestId, Tool, RemoveRow]() -> FReply
		{
			RemoveRow();
			SessionAutoApproveTools.Add(Tool);
			SaveAutoApproveTools();
			if (Client.IsValid()) Client->SendPermissionResponse(RequestId, TEXT("approve_always"));
			AppendLine(ELineKind::System, FString::Printf(TEXT("Always-approved: %s (session)"), *Tool));
			return FReply::Handled();
		})
	];

	Row->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(4.f, 0.f)
	[
		SNew(SButton)
		.ButtonStyle(&NGGBrand::CoralButton())
		.Text(LOCTEXT("PermDeny", "Deny"))
		.OnClicked_Lambda([this, RequestId, Tool, RemoveRow]() -> FReply
		{
			RemoveRow();
			if (Client.IsValid()) Client->SendPermissionResponse(RequestId, TEXT("deny"));
			AppendLine(ELineKind::System, FString::Printf(TEXT("Denied: %s"), *Tool));
			return FReply::Handled();
		})
	];

	PendingPermsList->AddSlot()
	.AutoHeight()
	.Padding(2.f)
	[
		Row.ToSharedRef()
	];

	if (PendingPermsBorder.IsValid())
	{
		PendingPermsBorder->SetVisibility(EVisibility::Visible);
	}
}

void SNGGChatWindow::MaybeAutoScroll()
{
	if (!TranscriptText.IsValid()) return;

	// Terminal view: always scroll to the bottom on append. The user can
	// pause the follow by selecting text — scrolling doesn't clear a
	// selection, it just shifts what's visible (the selection is still in
	// the buffer for Ctrl+C). If the user scrolls up manually they can
	// scroll back down; we don't try to detect manual scrollback here.
	TranscriptText->ScrollTo(ETextLocation::EndOfDocument);

	// Defer a second scroll to next frame so we catch the new line's
	// height after Slate runs its layout pass. Without this the view can
	// land at the old end and you don't see the just-appended message.
	TWeakPtr<SMultiLineEditableTextBox> WeakText = TranscriptText;
	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda(
		[WeakText](double, float) -> EActiveTimerReturnType
		{
			if (TSharedPtr<SMultiLineEditableTextBox> Pinned = WeakText.Pin())
			{
				Pinned->ScrollTo(ETextLocation::EndOfDocument);
			}
			return EActiveTimerReturnType::Stop;
		}));
}

void SNGGChatWindow::TrimTranscriptBuffer()
{
	if (TranscriptBuffer.Len() <= MaxTranscriptChars) return;

	// Drop oldest content: trim everything before the first newline boundary
	// at/after the over-budget offset, so we never leave a partial rich-text
	// span (which would render its closing </> as literal text). Keep the
	// newest MaxTranscriptChars-worth of lines.
	const int32 CutTarget = TranscriptBuffer.Len() - MaxTranscriptChars;
	int32 NewlineIdx = INDEX_NONE;
	if (TranscriptBuffer.FindChar(TEXT('\n'), NewlineIdx) && NewlineIdx >= CutTarget)
	{
		// First newline already past the target — cut there.
		TranscriptBuffer.RightChopInline(NewlineIdx + 1);
		return;
	}

	// Otherwise scan forward from CutTarget to the next newline so we cut on a
	// line boundary rather than mid-span.
	int32 Cut = CutTarget;
	while (Cut < TranscriptBuffer.Len() && TranscriptBuffer[Cut] != TEXT('\n'))
	{
		++Cut;
	}
	if (Cut < TranscriptBuffer.Len())
	{
		++Cut; // step past the newline so the kept buffer starts on a fresh line
	}
	TranscriptBuffer.RightChopInline(Cut);
}

// ---------------------------------------------------------------------------
// Input actions
// ---------------------------------------------------------------------------

FReply SNGGChatWindow::OnSendClicked()
{
	SubmitPrompt();
	return FReply::Handled();
}

FReply SNGGChatWindow::OnCancelClicked()
{
	if (Client.IsValid()) Client->SendCancel();
	return FReply::Handled();
}

FReply SNGGChatWindow::OnClearClicked()
{
	if (Client.IsValid()) Client->SendClear();
	TranscriptBuffer.Reset();
	if (TranscriptText.IsValid())
	{
		TranscriptText->SetText(FText::GetEmpty());
	}
	if (PendingPermsList.IsValid())
	{
		PendingPermsList->ClearChildren();
	}
	if (PendingPermsBorder.IsValid())
	{
		PendingPermsBorder->SetVisibility(EVisibility::Collapsed);
	}
	// SessionAutoApproveTools is project-scoped (persisted) — Clear is a
	// conversation reset, not a permission reset, so leave it intact.
	SeenPermissionIds.Reset();
	PendingImages.Empty();
	ResetUsage();
	AppendLine(ELineKind::System, TEXT("Conversation cleared."));
	return FReply::Handled();
}

FReply SNGGChatWindow::OnReconnectClicked()
{
	BeginConnect();
	return FReply::Handled();
}

FReply SNGGChatWindow::OnRestartSidecarClicked()
{
	AppendLine(ELineKind::System, TEXT("Restarting sidecar..."));
	UE_LOG(LogNGGChat, Log, TEXT("Restart-sidecar requested by user"));

	// Drop the current WS so we don't fight the shutdown — the sidecar's
	// wss.close() will otherwise wait for our connection to drain before
	// httpServer.close() can fire.
	if (Client.IsValid())
	{
		Client->Disconnect();
	}

	// Best-effort graceful shutdown via the daemon's HTTP endpoint. Async so
	// the UI thread doesn't stall; we follow up with TCP polling below.
	FString ShutdownURL = FSidecarLauncher::GetHealthURL();
	ShutdownURL.ReplaceInline(TEXT("/health"), TEXT("/shutdown"));
	// ESPMode::ThreadSafe to match FSidecarLauncher's HTTP requests. The HTTP
	// module hands these refs around across threads; a NotThreadSafe ref can
	// race its ref-controller free and trip the ReleaseSharedReferenceNoInline
	// access violation noted in FSidecarLauncher::IsSidecarAlive.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(ShutdownURL);
	Req->SetVerb(TEXT("POST"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetContentAsString(TEXT("{}"));
	Req->SetTimeout(5.0f);
	Req->OnProcessRequestComplete().BindLambda(
		[](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			UE_LOG(LogNGGChat, Log, TEXT("Sidecar /shutdown complete (ok=%d, code=%d)"),
				bOk ? 1 : 0, Resp.IsValid() ? Resp->GetResponseCode() : 0);
		});
	Req->ProcessRequest();

	if (StatusText.IsValid())
	{
		StatusText->SetText(LOCTEXT("StatusRestarting", "Restarting sidecar..."));
	}

	// Two-phase async wait, both phases share one ticker:
	//   Phase 1: poll the sidecar's TCP port until it stops responding (the
	//            old process is exiting). Sidecar's own setTimeout fallback
	//            in shutdown() force-exits after ~2s, so 10s deadline is
	//            generous enough for a slow editor.
	//   Phase 2: 300ms grace so the OS releases the listening sockets before
	//            BeginConnect spawns a new node. Without this, the new
	//            process loses the bind race and exits silently with
	//            EADDRINUSE — the symptom users see is "restart did nothing,
	//            had to press Reconnect".
	struct FRestartState
	{
		double Deadline       = 0.0;
		bool   bGraceArmed    = false;
		double GraceEndTime   = 0.0;
	};
	TSharedRef<FRestartState> State = MakeShared<FRestartState>();
	State->Deadline = FPlatformTime::Seconds() + 10.0;

	// Cancel any in-flight restart poll so a repeated click doesn't leave two
	// concurrent reconnect loops running against the same widget.
	if (RestartTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RestartTicker);
		RestartTicker.Reset();
	}

	TWeakPtr<SNGGChatWindow> WeakSelfReconnect = SharedThis(this);
	RestartTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakSelfReconnect, State](float) -> bool
		{
			TSharedPtr<SNGGChatWindow> Pinned = WeakSelfReconnect.Pin();
			if (!Pinned.IsValid()) return false;
			const double Now = FPlatformTime::Seconds();

			// Phase 2 — grace period after detected exit.
			if (State->bGraceArmed)
			{
				if (Now < State->GraceEndTime) return true;
				Pinned->RestartTicker.Reset(); // self-terminating; drop the stale handle
				Pinned->BeginConnect();
				return false;
			}

			// Phase 1 — poll for the old sidecar to exit.
			const bool bStillAlive = FSidecarLauncher::IsSidecarAlive(0.3f);
			const bool bExpired    = Now >= State->Deadline;

			if (!bStillAlive)
			{
				UE_LOG(LogNGGChat, Log,
					TEXT("Sidecar exited — 300ms grace then reconnecting"));
				State->bGraceArmed  = true;
				State->GraceEndTime = Now + 0.3;
				return true;
			}

			if (bExpired)
			{
				// Soft POST didn't take. Fall back to the synchronous helper
				// which re-sends /shutdown and confirms the process actually
				// died — guarantees BeginConnect doesn't reconnect to a
				// half-dead old sidecar.
				UE_LOG(LogNGGChat, Warning,
					TEXT("Sidecar still alive after 10s — forcing synchronous shutdown"));
				FSidecarLauncher::RequestSidecarShutdown(3.0f);
				State->bGraceArmed  = true;
				State->GraceEndTime = Now + 0.3;
				return true;
			}

			return true; // keep polling
		}),
		0.25f);

	return FReply::Handled();
}

FReply SNGGChatWindow::OnKillSidecarClicked()
{
	AppendLine(ELineKind::System, TEXT("Killing sidecar..."));
	UE_LOG(LogNGGChat, Log, TEXT("Kill-sidecar requested by user"));

	// Drop the current WS so we don't fight the shutdown — the sidecar's
	// wss.close() will otherwise wait for our connection to drain before
	// httpServer.close() can fire.
	if (Client.IsValid())
	{
		Client->Disconnect();
	}

	// Cancel any in-flight restart poll so a pending restart doesn't respawn
	// the sidecar we're deliberately killing.
	if (RestartTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RestartTicker);
		RestartTicker.Reset();
	}

	// Best-effort graceful shutdown via the daemon's HTTP endpoint. Async so
	// the UI thread doesn't stall. Unlike restart, we do NOT reconnect after.
	FString ShutdownURL = FSidecarLauncher::GetShutdownURL();
	// ESPMode::ThreadSafe to match FSidecarLauncher's HTTP requests (see the
	// note in OnRestartSidecarClicked).
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(ShutdownURL);
	Req->SetVerb(TEXT("POST"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetContentAsString(TEXT("{}"));
	Req->SetTimeout(5.0f);
	Req->OnProcessRequestComplete().BindLambda(
		[](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			UE_LOG(LogNGGChat, Log, TEXT("Sidecar /shutdown complete (ok=%d, code=%d)"),
				bOk ? 1 : 0, Resp.IsValid() ? Resp->GetResponseCode() : 0);
		});
	Req->ProcessRequest();

	if (StatusText.IsValid())
	{
		StatusText->SetText(LOCTEXT("StatusKilled", "Sidecar stopped"));
	}

	return FReply::Handled();
}

FReply SNGGChatWindow::OnInputKeyDown(const FGeometry& /*Geometry*/, const FKeyEvent& KeyEvent)
{
	// ---- Slash-command popup intercepts -------------------------------
	if (IsSlashPopupOpen())
	{
		const FKey Key = KeyEvent.GetKey();
		if (Key == EKeys::Up)    { HighlightDelta(-1); return FReply::Handled(); }
		if (Key == EKeys::Down)  { HighlightDelta(+1); return FReply::Handled(); }
		if (Key == EKeys::Escape){ HideSlashPopup();   return FReply::Handled(); }
		if ((Key == EKeys::Enter && !KeyEvent.IsControlDown() && !KeyEvent.IsShiftDown() && !KeyEvent.IsAltDown())
			|| Key == EKeys::Tab)
		{
			AcceptHighlightedCommand();
			return FReply::Handled();
		}
	}

	// ---- @-mention popup intercepts -----------------------------------
	if (IsAtPopupOpen())
	{
		const FKey Key = KeyEvent.GetKey();
		if (Key == EKeys::Up)    { AtHighlightDelta(-1); return FReply::Handled(); }
		if (Key == EKeys::Down)  { AtHighlightDelta(+1); return FReply::Handled(); }
		if (Key == EKeys::Escape){ HideAtPopup();        return FReply::Handled(); }
		if ((Key == EKeys::Enter && !KeyEvent.IsControlDown() && !KeyEvent.IsShiftDown() && !KeyEvent.IsAltDown())
			|| Key == EKeys::Tab)
		{
			AcceptHighlightedMention();
			return FReply::Handled();
		}
	}

	// ---- Clipboard image paste (Ctrl+V) ----------------------------------
	if (KeyEvent.GetKey() == EKeys::V && KeyEvent.IsControlDown() && !KeyEvent.IsAltDown())
	{
		TOptional<FNGGPastedImage> Captured = TrySavePastedImage();
		if (Captured.IsSet())
		{
			PendingImages.Add(Captured.GetValue());
			AppendLine(ELineKind::System, FString::Printf(
				TEXT("Image captured (%dx%d) — will be sent with next message [total: %d]"),
				Captured.GetValue().Width, Captured.GetValue().Height, PendingImages.Num()));
			return FReply::Handled();
		}
		// No image in clipboard — fall through so the text box handles text paste.
		return FReply::Unhandled();
	}

	if (KeyEvent.GetKey() == EKeys::Enter
		&& !KeyEvent.IsControlDown()
		&& !KeyEvent.IsShiftDown()
		&& !KeyEvent.IsAltDown())
	{
		SubmitPrompt();
		return FReply::Handled();
	}

	if (InputBox.IsValid())
	{
		const FKey Key = KeyEvent.GetKey();
		const bool bIsUp   = (Key == EKeys::Up);
		const bool bIsDown = (Key == EKeys::Down);
		if (bIsUp || bIsDown)
		{
			const FString Current = InputBox->GetText().ToString();

			// If the user is currently typing a slash command, leave Up/Down
			// for the popup's own selection flow (popup reopens on edits).
			const bool bLooksLikeSlashCommand =
				Current.Len() > 0 && Current[0] == TCHAR('/');

			if (!bLooksLikeSlashCommand)
			{
				// Heuristic for "cursor on edge line": use newline presence in
				// the text. `\n` absent → the text is single-line and both
				// edges qualify. With newlines present, we conservatively
				// fall through to default caret movement so multi-line edits
				// don't accidentally eat the user's input.
				const bool bHasNewline = Current.Contains(TEXT("\n"));
				const bool bOnFirstLine = !bHasNewline; // single-line input
				const bool bOnLastLine  = !bHasNewline;

				if (bIsUp && bOnFirstLine)
				{
					NavigateHistory(-1);
					return FReply::Handled();
				}
				if (bIsDown && bOnLastLine)
				{
					NavigateHistory(+1);
					return FReply::Handled();
				}
			}
		}
	}

	return FReply::Unhandled();
}

// ---------------------------------------------------------------------------
// Slash-command autocomplete
// ---------------------------------------------------------------------------

void SNGGChatWindow::InitSlashCommands()
{
	AllSlashCommands.Reset();
	auto Add = [this](const TCHAR* N, const TCHAR* D)
	{
		AllSlashCommands.Add({ FString(N), FString(D) });
	};
	Add(TEXT("/help"),                     TEXT("Show Claude Code help"));
	Add(TEXT("/clear"),                    TEXT("Clear the current conversation"));
	Add(TEXT("/compact"),                  TEXT("Summarize conversation to free context"));
	Add(TEXT("/cost"),                     TEXT("Show token usage for current session"));
	Add(TEXT("/mcp"),                      TEXT("List connected MCP servers and their tools"));
	Add(TEXT("/usage"),                    TEXT("Show API usage stats"));
	Add(TEXT("/resume"),                   TEXT("Resume a previous session"));
	Add(TEXT("/model"),                    TEXT("Switch Claude model for this session"));
	Add(TEXT("/login"),                    TEXT("Open a terminal to sign in to Claude"));
	Add(TEXT("/logout"),                   TEXT("Sign out of Claude (in a new terminal)"));
	Add(TEXT("/status"),                   TEXT("Show Claude CLI auth + account status"));
	Add(TEXT("/export"),                   TEXT("Export conversation to file"));
	Add(TEXT("/init"),                     TEXT("Initialize CLAUDE.md in the project"));
	Add(TEXT("/review"),                   TEXT("Review a pull request"));
	Add(TEXT("/security-review"),          TEXT("Run a security review on the current branch"));
	Add(TEXT("/simplify"),                 TEXT("Review changed code for quality & simplicity"));
	Add(TEXT("/loop"),                     TEXT("Run a command on a recurring schedule"));
	Add(TEXT("/schedule"),                 TEXT("Manage scheduled background agents"));
	Add(TEXT("/claude-api"),               TEXT("Build or migrate Claude API apps"));
	Add(TEXT("/ue5-niagara"),              TEXT("Niagara VFX creation and tuning help"));
	Add(TEXT("/ue5-umg-widgets"),          TEXT("UMG widget layout and design help"));
	Add(TEXT("/update-config"),            TEXT("Configure Claude Code settings/hooks"));
	Add(TEXT("/fewer-permission-prompts"), TEXT("Reduce permission prompts from transcripts"));
	Add(TEXT("/keybindings-help"),         TEXT("Customize keyboard shortcuts"));
}

bool SNGGChatWindow::IsSlashPopupOpen() const
{
	return SlashMenuAnchor.IsValid() && SlashMenuAnchor->IsOpen();
}

void SNGGChatWindow::OnInputTextChanged(const FText& NewText)
{
	if (bSuppressTextChanged) return;

	const FString Raw = NewText.ToString();

	// Slash-command popup requires leading '/' and no whitespace in the token.
	bool bSlashShow = false;
	FString SlashQuery;
	if (Raw.Len() > 0 && Raw[0] == TCHAR('/'))
	{
		bool bHasWhitespace = false;
		for (int32 i = 0; i < Raw.Len(); ++i)
		{
			if (FChar::IsWhitespace(Raw[i])) { bHasWhitespace = true; break; }
		}
		if (!bHasWhitespace)
		{
			bSlashShow = true;
			SlashQuery = Raw;
		}
	}

	// @-mention popup: any unclosed '@token' touching end-of-text. Slash takes
	// precedence if both would match (a line starting with '/' with no spaces
	// can't contain a valid @-mention anyway).
	FString AtQuery;
	int32 AtPos = INDEX_NONE;
	const bool bAtShow = !bSlashShow && ExtractAtQuery(Raw, AtQuery, AtPos);

	if (bSlashShow)
	{
		HideAtPopup();
		RebuildFilteredCommands(SlashQuery);
		ShowSlashPopup();
	}
	else if (bAtShow)
	{
		HideSlashPopup();
		AtActiveTokenStart = AtPos;
		InitAtMentions();
		RebuildFilteredMentions(AtQuery);
		ShowAtPopup();
	}
	else
	{
		HideSlashPopup();
		HideAtPopup();
	}
}

void SNGGChatWindow::RebuildFilteredCommands(const FString& Query)
{
	FilteredCommands.Reset();

	// Strip leading '/' for substring matching against command name's body.
	FString Needle = Query;
	if (Needle.StartsWith(TEXT("/"))) Needle.RemoveAt(0);
	Needle = Needle.ToLower();

	for (const FNGGChatSlashCommand& Cmd : AllSlashCommands)
	{
		if (Needle.IsEmpty())
		{
			FilteredCommands.Add(MakeShared<FNGGChatSlashCommand>(Cmd));
			continue;
		}
		FString NameBody = Cmd.Name;
		if (NameBody.StartsWith(TEXT("/"))) NameBody.RemoveAt(0);
		if (NameBody.ToLower().Contains(Needle))
		{
			FilteredCommands.Add(MakeShared<FNGGChatSlashCommand>(Cmd));
		}
	}

	FilteredCommands.Sort([](const TSharedPtr<FNGGChatSlashCommand>& A,
	                         const TSharedPtr<FNGGChatSlashCommand>& B)
	{
		return A->Name < B->Name;
	});

	HighlightedIndex = 0;
	if (SlashListView.IsValid())
	{
		SlashListView->RequestListRefresh();
		if (FilteredCommands.Num() > 0)
		{
			SlashListView->SetSelection(FilteredCommands[0], ESelectInfo::Direct);
			SlashListView->RequestScrollIntoView(FilteredCommands[0]);
		}
		else
		{
			SlashListView->ClearSelection();
		}
	}
}

void SNGGChatWindow::ShowSlashPopup()
{
	if (!SlashMenuAnchor.IsValid()) return;
	if (FilteredCommands.Num() == 0)
	{
		HideSlashPopup();
		return;
	}
	if (!SlashMenuAnchor->IsOpen())
	{
		UE_LOG(LogNGGChat, Verbose, TEXT("Slash popup: open (%d entries)"), FilteredCommands.Num());
		SlashMenuAnchor->SetIsOpen(true, /*bFocusMenu=*/false);
	}
	// Re-assert selection after the menu lazily realizes its content.
	if (SlashListView.IsValid() && FilteredCommands.Num() > 0)
	{
		SlashListView->SetSelection(FilteredCommands[HighlightedIndex], ESelectInfo::Direct);
		SlashListView->RequestScrollIntoView(FilteredCommands[HighlightedIndex]);
	}
}

void SNGGChatWindow::HideSlashPopup()
{
	if (SlashMenuAnchor.IsValid() && SlashMenuAnchor->IsOpen())
	{
		UE_LOG(LogNGGChat, Verbose, TEXT("Slash popup: close"));
		SlashMenuAnchor->SetIsOpen(false);
	}
}

void SNGGChatWindow::HighlightDelta(int32 Delta)
{
	if (FilteredCommands.Num() == 0) return;
	const int32 N = FilteredCommands.Num();
	HighlightedIndex = ((HighlightedIndex + Delta) % N + N) % N;
	if (SlashListView.IsValid())
	{
		SlashListView->SetSelection(FilteredCommands[HighlightedIndex], ESelectInfo::Direct);
		SlashListView->RequestScrollIntoView(FilteredCommands[HighlightedIndex]);
	}
}

void SNGGChatWindow::AcceptHighlightedCommand()
{
	if (!InputBox.IsValid()) return;
	if (!FilteredCommands.IsValidIndex(HighlightedIndex)) { HideSlashPopup(); return; }

	const FString Replacement = FilteredCommands[HighlightedIndex]->Name + TEXT(" ");
	UE_LOG(LogNGGChat, Verbose, TEXT("Slash popup: accept '%s'"), *Replacement);

	// Suppress recursive OnTextChanged triggering reopen.
	TGuardValue<bool> Guard(bSuppressTextChanged, true);
	InputBox->SetText(FText::FromString(Replacement));
	HideSlashPopup();
	// Keep focus on input so the user can type args immediately.
	FSlateApplication::Get().SetKeyboardFocus(InputBox, EFocusCause::SetDirectly);
}

TSharedRef<ITableRow> SNGGChatWindow::GenerateSlashRow(TSharedPtr<FNGGChatSlashCommand> Item,
                                                      const TSharedRef<STableViewBase>& OwnerTable)
{
	const FSlateFontInfo NameFont = FCoreStyle::GetDefaultFontStyle("Bold", 10);
	const FSlateFontInfo DescFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	const FLinearColor NameColor = NGGBrand::TextPrimary;
	const FLinearColor DescColor = NGGBrand::TextMuted;

	return SNew(STableRow<TSharedPtr<FNGGChatSlashCommand>>, OwnerTable)
		.Padding(FMargin(6.f, 2.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Name))
				.Font(NameFont)
				.ColorAndOpacity(FSlateColor(NameColor))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SNew(SSpacer).Size(FVector2D(8.f, 1.f))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Description))
				.Font(DescFont)
				.ColorAndOpacity(FSlateColor(DescColor))
			]
		];
}

void SNGGChatWindow::OnSlashSelectionChanged(TSharedPtr<FNGGChatSlashCommand> Item, ESelectInfo::Type /*SelectInfo*/)
{
	if (!Item.IsValid()) return;
	const int32 Idx = FilteredCommands.IndexOfByPredicate(
		[&Item](const TSharedPtr<FNGGChatSlashCommand>& E){ return E.Get() == Item.Get(); });
	if (Idx != INDEX_NONE) HighlightedIndex = Idx;
}

void SNGGChatWindow::OnSlashRowClicked(TSharedPtr<FNGGChatSlashCommand> Item)
{
	if (!Item.IsValid()) return;
	const int32 Idx = FilteredCommands.IndexOfByPredicate(
		[&Item](const TSharedPtr<FNGGChatSlashCommand>& E){ return E.Get() == Item.Get(); });
	if (Idx != INDEX_NONE) HighlightedIndex = Idx;
	AcceptHighlightedCommand();
}

// ---------------------------------------------------------------------------
// @-mention file/folder autocomplete
// ---------------------------------------------------------------------------

namespace NGGAtMention
{
	static const int32 MaxEntriesToIndex = 8000;
	static const int32 MaxFilteredResults = 25;

	static bool ShouldSkipDirectory(const FString& DirName)
	{
		static const TCHAR* Skip[] = {
			TEXT("Binaries"),       TEXT("Intermediate"), TEXT("DerivedDataCache"),
			TEXT("Saved"),          TEXT("node_modules"), TEXT(".git"),
			TEXT(".vs"),            TEXT(".claude"),      TEXT(".idea"),
			TEXT("build"),          TEXT("dist"),         TEXT("__pycache__"),
		};
		for (const TCHAR* S : Skip)
		{
			if (DirName.Equals(S, ESearchCase::IgnoreCase)) return true;
		}
		return false;
	}

	static void ScanRecursive(IPlatformFile& PF,
	                          const FString& CurrentAbs,
	                          const FString& BaseAbs,
	                          TArray<FNGGChatAtMention>& Out)
	{
		if (Out.Num() >= MaxEntriesToIndex) return;

		struct FLister : public IPlatformFile::FDirectoryVisitor
		{
			TArray<FString> SubDirs;
			TArray<FString> Files;
			virtual bool Visit(const TCHAR* Path, bool bIsDirectory) override
			{
				if (bIsDirectory) SubDirs.Add(Path);
				else              Files.Add(Path);
				return true;
			}
		};

		FLister Lister;
		PF.IterateDirectory(*CurrentAbs, Lister);

		auto ToRelativePosix = [&BaseAbs](const FString& Abs) -> FString
		{
			FString Path = Abs;
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Path.StartsWith(BaseAbs, ESearchCase::IgnoreCase))
			{
				Path = Path.Mid(BaseAbs.Len());
			}
			while (Path.StartsWith(TEXT("/"))) Path.RemoveAt(0);
			return Path;
		};

		for (const FString& F : Lister.Files)
		{
			if (Out.Num() >= MaxEntriesToIndex) return;
			FNGGChatAtMention M;
			M.RelativePath = ToRelativePosix(F);
			M.DisplayLabel = FPaths::GetCleanFilename(M.RelativePath);
			M.bIsDirectory = false;
			Out.Add(M);
		}

		for (const FString& D : Lister.SubDirs)
		{
			if (Out.Num() >= MaxEntriesToIndex) return;
			const FString DirName = FPaths::GetCleanFilename(D);
			if (ShouldSkipDirectory(DirName)) continue;

			// Record the directory itself so users can reference folders.
			FNGGChatAtMention M;
			M.RelativePath = ToRelativePosix(D);
			M.DisplayLabel = DirName;
			M.bIsDirectory = true;
			Out.Add(M);

			ScanRecursive(PF, D, BaseAbs, Out);
		}
	}
}

void SNGGChatWindow::InitAtMentions()
{
	if (bAtMentionsInitialized) return;
	bAtMentionsInitialized = true;

	const double StartTime = FPlatformTime::Seconds();

	FString BaseAbs = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	BaseAbs.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (!BaseAbs.EndsWith(TEXT("/"))) BaseAbs.Append(TEXT("/"));

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	AllAtMentions.Reset();
	NGGAtMention::ScanRecursive(PF, BaseAbs, BaseAbs, AllAtMentions);

	UE_LOG(LogNGGChat, Log, TEXT("AtMention index built: %d entries in %.1f ms (base=%s)"),
		AllAtMentions.Num(),
		(FPlatformTime::Seconds() - StartTime) * 1000.0,
		*BaseAbs);
}

bool SNGGChatWindow::IsAtPopupOpen() const
{
	return AtMenuAnchor.IsValid() && AtMenuAnchor->IsOpen();
}

bool SNGGChatWindow::ExtractAtQuery(const FString& Text, FString& OutQuery, int32& OutAtPos) const
{
	// Walk back from end-of-text. Return true if we hit an '@' before any
	// whitespace and the '@' itself is at start-of-text or preceded by space.
	for (int32 i = Text.Len() - 1; i >= 0; --i)
	{
		const TCHAR C = Text[i];
		if (FChar::IsWhitespace(C)) return false;
		if (C == TCHAR('@'))
		{
			if (i == 0 || FChar::IsWhitespace(Text[i - 1]))
			{
				OutAtPos = i;
				OutQuery = Text.Mid(i + 1);
				return true;
			}
			return false;
		}
	}
	return false;
}

void SNGGChatWindow::RebuildFilteredMentions(const FString& Query)
{
	FilteredAtMentions.Reset();
	const FString Needle = Query.ToLower();

	// Score lower-is-better: 0 = basename startswith, 1 = basename contains,
	// 2 = path contains. Directories slightly deprioritised so files surface
	// first in ties (common case: user wants a file).
	struct FScored { int32 Score; const FNGGChatAtMention* Entry; };
	TArray<FScored> Scored;
	Scored.Reserve(AllAtMentions.Num());

	for (const FNGGChatAtMention& M : AllAtMentions)
	{
		if (Needle.IsEmpty())
		{
			Scored.Add({ M.bIsDirectory ? 3 : 2, &M });
			continue;
		}
		const FString NameLower = M.DisplayLabel.ToLower();
		const FString PathLower = M.RelativePath.ToLower();
		int32 Base = -1;
		if      (NameLower.StartsWith(Needle))  Base = 0;
		else if (NameLower.Contains(Needle))    Base = 1;
		else if (PathLower.Contains(Needle))    Base = 2;
		if (Base >= 0)
		{
			Scored.Add({ Base + (M.bIsDirectory ? 1 : 0), &M });
		}
	}

	Scored.Sort([](const FScored& A, const FScored& B)
	{
		if (A.Score != B.Score) return A.Score < B.Score;
		// Shorter paths win ties so top-level files beat deeply nested ones.
		if (A.Entry->RelativePath.Len() != B.Entry->RelativePath.Len())
			return A.Entry->RelativePath.Len() < B.Entry->RelativePath.Len();
		return A.Entry->RelativePath < B.Entry->RelativePath;
	});

	for (const FScored& S : Scored)
	{
		FilteredAtMentions.Add(MakeShared<FNGGChatAtMention>(*S.Entry));
		if (FilteredAtMentions.Num() >= NGGAtMention::MaxFilteredResults) break;
	}

	AtHighlightedIndex = 0;
	if (AtListView.IsValid())
	{
		AtListView->RequestListRefresh();
		if (FilteredAtMentions.Num() > 0)
		{
			AtListView->SetSelection(FilteredAtMentions[0], ESelectInfo::Direct);
			AtListView->RequestScrollIntoView(FilteredAtMentions[0]);
		}
		else
		{
			AtListView->ClearSelection();
		}
	}
}

void SNGGChatWindow::ShowAtPopup()
{
	if (!AtMenuAnchor.IsValid()) return;
	if (FilteredAtMentions.Num() == 0)
	{
		HideAtPopup();
		return;
	}
	if (!AtMenuAnchor->IsOpen())
	{
		AtMenuAnchor->SetIsOpen(true, /*bFocusMenu=*/false);
	}
	if (AtListView.IsValid() && FilteredAtMentions.Num() > 0)
	{
		AtListView->SetSelection(FilteredAtMentions[AtHighlightedIndex], ESelectInfo::Direct);
		AtListView->RequestScrollIntoView(FilteredAtMentions[AtHighlightedIndex]);
	}
}

void SNGGChatWindow::HideAtPopup()
{
	if (AtMenuAnchor.IsValid() && AtMenuAnchor->IsOpen())
	{
		AtMenuAnchor->SetIsOpen(false);
	}
}

void SNGGChatWindow::AtHighlightDelta(int32 Delta)
{
	if (FilteredAtMentions.Num() == 0) return;
	const int32 N = FilteredAtMentions.Num();
	AtHighlightedIndex = ((AtHighlightedIndex + Delta) % N + N) % N;
	if (AtListView.IsValid())
	{
		AtListView->SetSelection(FilteredAtMentions[AtHighlightedIndex], ESelectInfo::Direct);
		AtListView->RequestScrollIntoView(FilteredAtMentions[AtHighlightedIndex]);
	}
}

void SNGGChatWindow::AcceptHighlightedMention()
{
	if (!InputBox.IsValid()) return;
	if (!FilteredAtMentions.IsValidIndex(AtHighlightedIndex)) { HideAtPopup(); return; }
	if (AtActiveTokenStart == INDEX_NONE)                     { HideAtPopup(); return; }

	const FString Current = InputBox->GetText().ToString();
	if (AtActiveTokenStart > Current.Len())                   { HideAtPopup(); return; }

	const FNGGChatAtMention& M = *FilteredAtMentions[AtHighlightedIndex];
	// Spec-style: "@path/to/file " with a trailing space so the user can keep
	// typing. Directories get a trailing slash so they read naturally.
	FString Replacement = FString::Printf(TEXT("@%s"), *M.RelativePath);
	if (M.bIsDirectory && !Replacement.EndsWith(TEXT("/"))) Replacement.Append(TEXT("/"));
	Replacement.Append(TEXT(" "));

	const FString NewText = Current.Left(AtActiveTokenStart) + Replacement;

	TGuardValue<bool> Guard(bSuppressTextChanged, true);
	InputBox->SetText(FText::FromString(NewText));
	AtActiveTokenStart = INDEX_NONE;
	HideAtPopup();
	FSlateApplication::Get().SetKeyboardFocus(InputBox, EFocusCause::SetDirectly);
}

TSharedRef<ITableRow> SNGGChatWindow::GenerateAtRow(TSharedPtr<FNGGChatAtMention> Item,
                                                    const TSharedRef<STableViewBase>& OwnerTable)
{
	const FSlateFontInfo NameFont = FCoreStyle::GetDefaultFontStyle("Bold", 10);
	const FSlateFontInfo PathFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	const FLinearColor   FileColor = NGGBrand::TextPrimary;
	const FLinearColor   DirColor  = NGGBrand::Cyan;
	const FLinearColor   PathColor = NGGBrand::TextMuted;

	// Strip the basename out of the path so the row shows "Foo.cpp   Source/.../"
	FString ParentDir = Item->RelativePath;
	const int32 LastSlash = ParentDir.Len() - Item->DisplayLabel.Len() - 1;
	if (LastSlash > 0) ParentDir = ParentDir.Left(LastSlash);
	else               ParentDir.Reset();

	return SNew(STableRow<TSharedPtr<FNGGChatAtMention>>, OwnerTable)
		.Padding(FMargin(6.f, 2.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->bIsDirectory
					? FString::Printf(TEXT("%s/"), *Item->DisplayLabel)
					: Item->DisplayLabel))
				.Font(NameFont)
				.ColorAndOpacity(FSlateColor(Item->bIsDirectory ? DirColor : FileColor))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SNew(SSpacer).Size(FVector2D(8.f, 1.f))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(ParentDir))
				.Font(PathFont)
				.ColorAndOpacity(FSlateColor(PathColor))
			]
		];
}

void SNGGChatWindow::OnAtSelectionChanged(TSharedPtr<FNGGChatAtMention> Item, ESelectInfo::Type SelectInfo)
{
	if (!Item.IsValid()) return;
	const int32 Idx = FilteredAtMentions.IndexOfByPredicate(
		[&Item](const TSharedPtr<FNGGChatAtMention>& E){ return E.Get() == Item.Get(); });
	if (Idx != INDEX_NONE) AtHighlightedIndex = Idx;
}

void SNGGChatWindow::OnAtRowClicked(TSharedPtr<FNGGChatAtMention> Item)
{
	if (!Item.IsValid()) return;
	const int32 Idx = FilteredAtMentions.IndexOfByPredicate(
		[&Item](const TSharedPtr<FNGGChatAtMention>& E){ return E.Get() == Item.Get(); });
	if (Idx != INDEX_NONE) AtHighlightedIndex = Idx;
	AcceptHighlightedMention();
}

void SNGGChatWindow::SubmitPrompt()
{
	if (!Client.IsValid() || !InputBox.IsValid()) return;

	const FString Text = InputBox->GetText().ToString().TrimStartAndEnd();
	if (Text.IsEmpty()) return;
	if (!Client->IsConnected()) return;

	// If the selection toggle is on, prepend a <ue5_selection> block so
	// Claude can refer to "my selected actor/asset" naturally. The user-
	// facing transcript shows the raw text (what the user typed); only the
	// prompt sent to Claude includes the injected context.
	AppendLine(ELineKind::User, Text);
	PushHistory(Text);

	FString Outgoing = Text;

	// Inject extended-thinking trigger if a non-default effort level is picked.
	// Keyword goes on its own leading line so Claude Code reliably picks it up.
	// Skip for slash commands — the sidecar's local-command detector needs the
	// message to start with '/', and reasoning budget is irrelevant for commands
	// like /login or /clear anyway.
	const bool bIsSlashCommand = Text.StartsWith(TEXT("/"));
	if (!bIsSlashCommand && SelectedEffort.IsValid() && !SelectedEffort->TriggerPhrase.IsEmpty())
	{
		Outgoing = FString::Printf(TEXT("%s\n\n%s"), *SelectedEffort->TriggerPhrase, *Outgoing);
	}

	if (bIncludeSelection)
	{
		const FString Summary = GatherSelectionSummary();
		if (!Summary.IsEmpty())
		{
			Outgoing = FString::Printf(
				TEXT("<ue5_selection>\n%s</ue5_selection>\n\n%s"),
				*Summary, *Outgoing);
			AppendLine(ELineKind::System,
				FString::Printf(TEXT("(included %d selection item(s) as context)"), LastSelectionCount));
		}
	}
	if (PendingImages.Num() > 0)
	{
		AppendLine(ELineKind::System,
			FString::Printf(TEXT("Sending %d image(s) with prompt..."), PendingImages.Num()));
		Client->SendPromptWithImages(Outgoing, PendingImages);
		PendingImages.Empty();
	}
	else
	{
		Client->SendPrompt(Outgoing);
	}

	InputBox->SetText(FText::GetEmpty());
	HistoryCursor = INDEX_NONE;
}

// ---------------------------------------------------------------------------
// Input history
// ---------------------------------------------------------------------------

void SNGGChatWindow::LoadHistory()
{
	if (!GConfig) return;
	History.Reset();
	TArray<FString> Raw;
	GConfig->GetArray(HistoryConfigSection, HistoryConfigKey, Raw, GEditorPerProjectIni);
	for (const FString& Line : Raw)
	{
		FString Decoded = Line;
		Decoded.ReplaceInline(TEXT("\\n"), TEXT("\n"));
		History.Add(Decoded);
	}
}

void SNGGChatWindow::SaveHistory()
{
	if (!GConfig) return;
	TArray<FString> Raw;
	for (const FString& H : History)
	{
		FString Encoded = H;
		Encoded.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Raw.Add(Encoded);
	}
	GConfig->SetArray(HistoryConfigSection, HistoryConfigKey, Raw, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void SNGGChatWindow::LoadAutoApproveTools()
{
	if (!GConfig) return;
	SessionAutoApproveTools.Reset();
	TArray<FString> Raw;
	GConfig->GetArray(HistoryConfigSection, AutoApproveConfigKey, Raw, GEditorPerProjectIni);
	for (const FString& Tool : Raw)
	{
		if (!Tool.IsEmpty()) SessionAutoApproveTools.Add(Tool);
	}
}

void SNGGChatWindow::SaveAutoApproveTools()
{
	if (!GConfig) return;
	TArray<FString> Raw;
	Raw.Reserve(SessionAutoApproveTools.Num());
	for (const FString& Tool : SessionAutoApproveTools) Raw.Add(Tool);
	GConfig->SetArray(HistoryConfigSection, AutoApproveConfigKey, Raw, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void SNGGChatWindow::LoadBypassPermissions()
{
	bBypassPermissions = false;
	if (!GConfig) return;
	GConfig->GetBool(HistoryConfigSection, BypassPermsConfigKey, bBypassPermissions, GEditorPerProjectIni);
}

void SNGGChatWindow::SaveBypassPermissions()
{
	if (!GConfig) return;
	GConfig->SetBool(HistoryConfigSection, BypassPermsConfigKey, bBypassPermissions, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void SNGGChatWindow::RefreshBypassButtonAppearance()
{
	if (!BypassPermsButtonText.IsValid()) return;
	BypassPermsButtonText->SetText(bBypassPermissions
		? LOCTEXT("BypassPermsOn",  "Bypass: ON")
		: LOCTEXT("BypassPermsOff", "Bypass: OFF"));
	// Tint the label coral when bypass is active so the user can't miss it.
	BypassPermsButtonText->SetColorAndOpacity(bBypassPermissions
		? FSlateColor(NGGBrand::Coral)
		: FSlateColor::UseForeground());
}

FReply SNGGChatWindow::OnBypassPermissionsClicked()
{
	bBypassPermissions = !bBypassPermissions;
	SaveBypassPermissions();
	RefreshBypassButtonAppearance();

	if (Client.IsValid() && Client->IsConnected())
	{
		Client->SendSetPermissionBypass(bBypassPermissions);
		bBypassPushedThisConnection = true;
	}

	AppendLine(ELineKind::System, bBypassPermissions
		? FString(TEXT("Permission gate BYPASSED. Next prompt will spawn claude with --dangerously-skip-permissions."))
		: FString(TEXT("Permission gate restored. Next prompt will go through the normal approval flow.")));
	return FReply::Handled();
}

void SNGGChatWindow::PushHistory(const FString& Text)
{
	History.Remove(Text);
	History.Add(Text);
	while (History.Num() > MaxHistory)
	{
		History.RemoveAt(0);
	}
	SaveHistory();
}

void SNGGChatWindow::NavigateHistory(int32 Delta)
{
	if (!InputBox.IsValid()) return;

	// Empty history — consume the key so the default Up/Down does not insert
	// a caret movement we don't want while the input has no content.
	if (History.Num() == 0) return;

	// First press: save whatever the user had typed so we can restore it
	// when they scroll back past the newest entry.
	if (HistoryCursor == INDEX_NONE)
	{
		DraftBeforeHistory = InputBox->GetText().ToString();
		HistoryCursor = History.Num();
	}

	HistoryCursor = FMath::Clamp(HistoryCursor + Delta, 0, History.Num());

	// Suppress slash-popup reopening triggered by SetText → OnTextChanged.
	TGuardValue<bool> Guard(bSuppressTextChanged, true);

	if (HistoryCursor >= History.Num())
	{
		// Past the newest entry — restore the in-progress draft (may be empty).
		InputBox->SetText(FText::FromString(DraftBeforeHistory));
	}
	else
	{
		InputBox->SetText(FText::FromString(History[HistoryCursor]));
	}
}

// Unused — kept for future refinement if we want a dedicated scroll poller.
bool SNGGChatWindow::TickScrollState(float /*DeltaTime*/) { return true; }

// ---------------------------------------------------------------------------
// Model selector
// ---------------------------------------------------------------------------

void SNGGChatWindow::LoadSelectedModel()
{
	FString Saved;
	if (GConfig)
	{
		GConfig->GetString(HistoryConfigSection, ModelConfigKey, Saved, GEditorPerProjectIni);
	}

	for (const TSharedPtr<FNGGChatModelOption>& Opt : ModelOptions)
	{
		if (Opt.IsValid() && !Opt->bIsSeparator && Opt->CliArg.Equals(Saved, ESearchCase::IgnoreCase))
		{
			SelectedModel = Opt;
			return;
		}
	}

	// Saved value no longer in the list (or blank) — fall back to the first
	// selectable entry (the primary / "Opus 4.7" row).
	for (const TSharedPtr<FNGGChatModelOption>& Opt : ModelOptions)
	{
		if (Opt.IsValid() && !Opt->bIsSeparator)
		{
			SelectedModel = Opt;
			return;
		}
	}
}

void SNGGChatWindow::SaveSelectedModel(const FString& CliArg)
{
	if (!GConfig) return;
	GConfig->SetString(HistoryConfigSection, ModelConfigKey, *CliArg, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void SNGGChatWindow::OnModelSelectionChanged(TSharedPtr<FNGGChatModelOption> NewValue, ESelectInfo::Type SelectInfo)
{
	if (!NewValue.IsValid()) return;

	// Separators are just visual — reject the selection and snap back to
	// whatever was previously selected without side effects.
	if (NewValue->bIsSeparator)
	{
		if (ModelCombo.IsValid() && SelectedModel.IsValid())
		{
			ModelCombo->SetSelectedItem(SelectedModel);
		}
		return;
	}

	// Ignore direct/programmatic re-selection of the same entry.
	if (SelectedModel == NewValue) return;

	SelectedModel = NewValue;
	SaveSelectedModel(NewValue->CliArg);

	if (Client.IsValid() && Client->IsConnected())
	{
		Client->SendSetModel(NewValue->CliArg);
		AppendLine(ELineKind::System,
			FString::Printf(TEXT("Model set to %s"), *NewValue->DisplayLabel));

		// Claude CLI only emits its `init` event after the first prompt, so
		// until then show the friendly form of the user's pick. When init
		// finally arrives the label will refine to the exact resolved id.
		ResolvedModelId.Reset();
		if (ResolvedModelText.IsValid())
		{
			const FString Friendly = FriendlyLabelFromModelId(NewValue->CliArg);
			if (Friendly.IsEmpty())
			{
				ResolvedModelText->SetText(LOCTEXT("ModelDefault", "(CLI default)"));
			}
			else
			{
				ResolvedModelText->SetText(FText::FromString(FString::Printf(TEXT("(%s)"), *Friendly)));
			}
			ResolvedModelText->SetToolTipText(FText::FromString(NewValue->CliArg));
		}
	}
}

TSharedRef<SWidget> SNGGChatWindow::GenerateModelComboRow(TSharedPtr<FNGGChatModelOption> Item)
{
	if (!Item.IsValid())
	{
		return SNew(STextBlock);
	}

	// Separator row: muted, centred-ish label, fixed height.
	if (Item->bIsSeparator)
	{
		return SNew(SBox)
			.Padding(FMargin(6.f, 4.f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->DisplayLabel))
				.ColorAndOpacity(FSlateColor(NGGBrand::TextMuted))
			];
	}

	// Normal row: bold-ish title + optional muted description below.
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	Box->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(Item->DisplayLabel))
		];

	if (!Item->Description.IsEmpty())
	{
		Box->AddSlot()
			.AutoHeight()
			.Padding(0.f, 1.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Description))
				.ColorAndOpacity(FSlateColor(NGGBrand::TextBody))
			];
	}

	return SNew(SBox)
		.Padding(FMargin(6.f, 4.f))
		.MinDesiredWidth(240.f)
		[
			Box
		];
}

void SNGGChatWindow::OnClientModels(const TArray<FNGGSidecarModel>& Models)
{
	// Empty list = the sidecar couldn't fetch the catalog (no creds / offline /
	// API error). Keep the hardcoded fallback list already populated in Construct.
	if (Models.Num() == 0)
	{
		AppendLine(ELineKind::System,
			TEXT("Model list: live catalog unavailable — using built-in fallback. Sign in with /login if this persists."));
		return;
	}

	// Preserve the user's current pick across the rebuild if it's still offered.
	const FString PrevArg = SelectedModel.IsValid() ? SelectedModel->CliArg : FString();

	ModelOptions.Reset();
	for (const FNGGSidecarModel& M : Models)
	{
		TSharedPtr<FNGGChatModelOption> Opt = MakeShared<FNGGChatModelOption>();
		Opt->DisplayLabel = M.DisplayName;   // e.g. "Claude Opus 4.8"
		Opt->CliArg       = M.Id;            // e.g. "claude-opus-4-8"
		ModelOptions.Add(Opt);
	}

	// Trailing "let the CLI pick" sentinel, mirroring the fallback list so the
	// user can always defer the choice to the Claude CLI.
	{
		TSharedPtr<FNGGChatModelOption> Sep = MakeShared<FNGGChatModelOption>();
		Sep->DisplayLabel = TEXT("— — —");
		Sep->bIsSeparator = true;
		ModelOptions.Add(Sep);

		TSharedPtr<FNGGChatModelOption> Def = MakeShared<FNGGChatModelOption>();
		Def->DisplayLabel = ModelDefaultLabel;
		Def->Description  = TEXT("Let the Claude CLI pick");
		ModelOptions.Add(Def); // empty CliArg
	}

	// Re-select the previous model by CliArg; otherwise fall back to the first
	// selectable entry (the newest model the API returned).
	TSharedPtr<FNGGChatModelOption> NewSelection;
	for (const TSharedPtr<FNGGChatModelOption>& Opt : ModelOptions)
	{
		if (Opt.IsValid() && !Opt->bIsSeparator && Opt->CliArg.Equals(PrevArg, ESearchCase::IgnoreCase))
		{
			NewSelection = Opt;
			break;
		}
	}
	if (!NewSelection.IsValid())
	{
		for (const TSharedPtr<FNGGChatModelOption>& Opt : ModelOptions)
		{
			if (Opt.IsValid() && !Opt->bIsSeparator) { NewSelection = Opt; break; }
		}
	}

	// Assign SelectedModel BEFORE SetSelectedItem so OnModelSelectionChanged's
	// "same item" guard short-circuits — no spurious set_model / config write.
	SelectedModel = NewSelection;
	if (ModelCombo.IsValid())
	{
		ModelCombo->RefreshOptions();
		if (SelectedModel.IsValid())
		{
			ModelCombo->SetSelectedItem(SelectedModel);
		}
	}

	UE_LOG(LogNGGChat, Log, TEXT("Model dropdown populated from live catalog: %d models"), Models.Num());
}

// ---------------------------------------------------------------------------
// Effort (extended-thinking) selector
// ---------------------------------------------------------------------------

void SNGGChatWindow::InitEffortOptions()
{
	EffortOptions.Reset();
	auto Add = [this](const TCHAR* Label, const TCHAR* Desc, const TCHAR* Trigger, const TCHAR* Id)
	{
		auto Opt = MakeShared<FNGGChatEffortOption>();
		Opt->DisplayLabel  = Label;
		Opt->Description   = Desc;
		Opt->TriggerPhrase = Trigger;
		Opt->SaveId        = Id;
		EffortOptions.Add(Opt);
	};

	// Claude Code parses these trigger keywords in the user prompt and scales
	// the extended-thinking budget. We expose a simpler low/medium/high/...
	// level naming and map each to the right keyword.
	Add(TEXT("low"),    TEXT("Light reasoning budget (think)"),            TEXT("think"),         TEXT("low"));
	Add(TEXT("medium"), TEXT("Medium reasoning budget (think hard)"),      TEXT("think hard"),    TEXT("medium"));
	Add(TEXT("high"),   TEXT("Large reasoning budget (think harder)"),     TEXT("think harder"),  TEXT("high"));
	Add(TEXT("xhigh"),  TEXT("Very large reasoning budget (ultrathink)"),  TEXT("ultrathink"),    TEXT("xhigh"));
	Add(TEXT("max"),    TEXT("Maximum reasoning budget (ultrathink)"),     TEXT("ultrathink"),    TEXT("max"));
	Add(TEXT("auto"),   TEXT("Let Claude decide (no trigger injected)"),   TEXT(""),              TEXT("auto"));
}

void SNGGChatWindow::LoadSelectedEffort()
{
	FString Saved;
	if (GConfig)
	{
		GConfig->GetString(HistoryConfigSection, EffortConfigKey, Saved, GEditorPerProjectIni);
	}

	for (const TSharedPtr<FNGGChatEffortOption>& Opt : EffortOptions)
	{
		if (Opt.IsValid() && Opt->SaveId.Equals(Saved, ESearchCase::IgnoreCase))
		{
			SelectedEffort = Opt;
			return;
		}
	}

	// Fallback: "auto" (no keyword injected). If absent for some reason,
	// fall back to the first entry so the combo always has a valid selection.
	for (const TSharedPtr<FNGGChatEffortOption>& Opt : EffortOptions)
	{
		if (Opt.IsValid() && Opt->SaveId.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
		{
			SelectedEffort = Opt;
			return;
		}
	}
	if (EffortOptions.Num() > 0)
	{
		SelectedEffort = EffortOptions[0];
	}
}

void SNGGChatWindow::SaveSelectedEffort(const FString& SaveId)
{
	if (!GConfig) return;
	GConfig->SetString(HistoryConfigSection, EffortConfigKey, *SaveId, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void SNGGChatWindow::OnEffortSelectionChanged(TSharedPtr<FNGGChatEffortOption> NewValue, ESelectInfo::Type /*SelectInfo*/)
{
	if (!NewValue.IsValid() || SelectedEffort == NewValue) return;
	SelectedEffort = NewValue;
	SaveSelectedEffort(NewValue->SaveId);
	AppendLine(ELineKind::System,
		FString::Printf(TEXT("Effort set to %s"), *NewValue->DisplayLabel));
}

TSharedRef<SWidget> SNGGChatWindow::GenerateEffortComboRow(TSharedPtr<FNGGChatEffortOption> Item)
{
	if (!Item.IsValid())
	{
		return SNew(STextBlock);
	}
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(FText::FromString(Item->DisplayLabel))
		];
	if (!Item->Description.IsEmpty())
	{
		Box->AddSlot().AutoHeight().Padding(0.f, 1.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Description))
				.ColorAndOpacity(FSlateColor(NGGBrand::TextBody))
			];
	}
	return SNew(SBox).Padding(FMargin(6.f, 4.f)).MinDesiredWidth(200.f)[ Box ];
}

FString SNGGChatWindow::FriendlyLabelFromModelId(const FString& ModelId)
{
	// Handle both current and legacy id shapes:
	//   "claude-opus-4-7-20251115"     -> "Opus 4.7"
	//   "claude-sonnet-4-6"            -> "Sonnet 4.6"
	//   "claude-3-opus-20240229"       -> "Opus 3"
	//   "claude-3-5-sonnet-20240620"   -> "Sonnet 3.5"
	// Plus the CLI family aliases:
	//   "opus" / "sonnet" / "haiku"    -> "Opus (latest)" / ...
	if (ModelId.IsEmpty()) return FString();

	if (ModelId.Equals(TEXT("opus"), ESearchCase::IgnoreCase) ||
		ModelId.Equals(TEXT("sonnet"), ESearchCase::IgnoreCase) ||
		ModelId.Equals(TEXT("haiku"), ESearchCase::IgnoreCase))
	{
		FString Alias = ModelId;
		Alias[0] = FChar::ToUpper(Alias[0]);
		return FString::Printf(TEXT("%s (latest)"), *Alias);
	}

	TArray<FString> Parts;
	ModelId.ParseIntoArray(Parts, TEXT("-"));
	if (Parts.Num() < 3 || !Parts[0].Equals(TEXT("claude"), ESearchCase::IgnoreCase))
	{
		return ModelId;
	}

	FString Family;
	TArray<FString> Versions;

	// Walk tokens after "claude", picking the first non-numeric word as the
	// family name and numeric tokens under 5 digits as version components.
	// An 8-digit numeric token is a date suffix — stop there.
	for (int32 i = 1; i < Parts.Num(); ++i)
	{
		const FString& P = Parts[i];
		if (P.IsNumeric())
		{
			if (P.Len() >= 6) break; // date suffix
			Versions.Add(P);
		}
		else if (Family.IsEmpty())
		{
			Family = P;
		}
	}

	if (Family.IsEmpty()) return ModelId;
	Family[0] = FChar::ToUpper(Family[0]);

	if (Versions.Num() == 0) return Family;
	if (Versions.Num() == 1) return FString::Printf(TEXT("%s %s"), *Family, *Versions[0]);
	return FString::Printf(TEXT("%s %s.%s"), *Family, *Versions[0], *Versions[1]);
}

// ---------------------------------------------------------------------------
// Editor-selection context
// ---------------------------------------------------------------------------

int32 SNGGChatWindow::GetSelectionCount() const
{
	// Defensive: anything in this function can be called during editor
	// startup or shutdown where GEditor / the ContentBrowser module may
	// be half-initialised or unloading. Fail closed to zero.
	if (IsEngineExitRequested()) return 0;

	int32 Count = 0;

	if (GEditor)
	{
		if (USelection* ActorSel = GEditor->GetSelectedActors())
		{
			Count += ActorSel->Num();
		}
	}

	if (FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		if (FContentBrowserModule* CB =
				FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
		{
			TArray<FAssetData> Assets;
			CB->Get().GetSelectedAssets(Assets);
			Count += Assets.Num();
		}
	}

	return Count;
}

FString SNGGChatWindow::GatherSelectionSummary() const
{
	if (IsEngineExitRequested()) return FString();

	TStringBuilder<1024> Out;

	// Actors.
	TArray<AActor*> Actors;
	if (GEditor)
	{
		if (USelection* ActorSel = GEditor->GetSelectedActors())
		{
			for (FSelectionIterator It(*ActorSel); It; ++It)
			{
				if (AActor* A = Cast<AActor>(*It))
				{
					Actors.Add(A);
				}
			}
		}
	}

	if (Actors.Num() > 0)
	{
		Out.Appendf(TEXT("Selected actors (%d):\n"), Actors.Num());
		for (AActor* A : Actors)
		{
			const FVector Loc = A->GetActorLocation();
			const FString Label = A->GetActorLabel();
			const FString ClassName = A->GetClass()->GetName();
			Out.Appendf(TEXT("  - %s [%s] path=%s loc=(%.0f, %.0f, %.0f)\n"),
				*Label, *ClassName, *A->GetPathName(), Loc.X, Loc.Y, Loc.Z);
		}
	}

	// Content-browser assets. Same defensive guard as GetSelectionCount.
	TArray<FAssetData> Assets;
	if (FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		if (FContentBrowserModule* CB =
				FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
		{
			CB->Get().GetSelectedAssets(Assets);
		}
	}

	if (Assets.Num() > 0)
	{
		Out.Appendf(TEXT("Selected content-browser assets (%d):\n"), Assets.Num());
		for (const FAssetData& Asset : Assets)
		{
			Out.Appendf(TEXT("  - %s [%s]\n"),
				*Asset.GetObjectPathString(),
				*Asset.AssetClassPath.GetAssetName().ToString());
		}
	}

	return Out.ToString();
}

FReply SNGGChatWindow::OnSelectionToggleClicked()
{
	bIncludeSelection = !bIncludeSelection;
	TickSelectionLabel(0.f); // refresh immediately
	return FReply::Handled();
}

bool SNGGChatWindow::TickSelectionLabel(float /*DeltaTime*/)
{
	if (IsEngineExitRequested()) return false;           // self-cancel on exit
	if (!SelectionButtonText.IsValid()) return true;      // widget not yet realised

	const int32 N = GetSelectionCount();
	LastSelectionCount = N;

	const TCHAR* Prefix = bIncludeSelection ? TEXT("✓ Selection") : TEXT("+ Selection");
	SelectionButtonText->SetText(FText::FromString(FString::Printf(TEXT("%s (%d)"), Prefix, N)));

	// Subtle colour cue so the "on" state is obvious at a glance.
	const FLinearColor Colour = bIncludeSelection
		? NGGBrand::Success      // green when active ("included")
		: NGGBrand::TextPrimary; // light text when idle
	SelectionButtonText->SetColorAndOpacity(FSlateColor(Colour));

	// Same cadence: refresh the BP-editor selection banner so a developer who
	// just selected nodes in any open Blueprint editor sees a hint about what
	// Claude can do with that selection.
	RefreshBpSelectionBanner();
	return true;
}

// ---------------------------------------------------------------------------
// Blueprint-editor selection banner
// ---------------------------------------------------------------------------

int32 SNGGChatWindow::GetBpSelectionInfo(FString& OutAssetName) const
{
	OutAssetName.Reset();
	if (IsEngineExitRequested() || !GEditor) return 0;

	UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AES) return 0;

	// Mirror the /bp/get_selection server-side resolution: prefer the open BP
	// editor with a non-empty selection; fall back to "no selection".
	TArray<UObject*> OpenAssets = AES->GetAllEditedAssets();
	for (UObject* Asset : OpenAssets)
	{
		UBlueprint* BP = Cast<UBlueprint>(Asset);
		if (!BP) continue;
		IAssetEditorInstance* Inst = AES->FindEditorForAsset(BP, /*bFocusIfOpen=*/false);
		if (!Inst) continue;
		// "BlueprintEditor" matches the FBlueprintEditor toolkit. Anim-BP and
		// Widget-BP editors report different names and are intentionally
		// excluded for v1.
		if (Inst->GetEditorName() != FName("BlueprintEditor")) continue;

		FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(Inst);
		const FGraphPanelSelectionSet Selected = BPEditor->GetSelectedNodes();

		// Count only real graph nodes (ignore comment-only / non-node objects
		// that may live in the selection set).
		int32 NodeCount = 0;
		for (UObject* Obj : Selected)
		{
			if (Cast<UEdGraphNode>(Obj)) ++NodeCount;
		}
		if (NodeCount > 0)
		{
			OutAssetName = FPaths::GetBaseFilename(BP->GetPathName());
			return NodeCount;
		}
	}
	return 0;
}

void SNGGChatWindow::RefreshBpSelectionBanner()
{
	if (!BpSelectionBanner.IsValid() || !BpSelectionBannerText.IsValid()) return;

	FString AssetName;
	const int32 Count = GetBpSelectionInfo(AssetName);

	// Skip text rebuilds when nothing changed — Slate measures every SetText.
	if (Count == LastBpSelectionCount && AssetName == LastBpSelectionAsset) return;
	LastBpSelectionCount = Count;
	LastBpSelectionAsset = AssetName;

	if (Count <= 0)
	{
		BpSelectionBanner->SetVisibility(EVisibility::Collapsed);
		return;
	}

	const FString NodeWord = (Count == 1) ? TEXT("node") : TEXT("nodes");
	const FString Banner = FString::Printf(
		TEXT("✨ %d %s selected in %s — try \"explain this\", \"convert this to C++\", or \"extract this to a function\"."),
		Count, *NodeWord, *AssetName);
	BpSelectionBannerText->SetText(FText::FromString(Banner));
	BpSelectionBanner->SetVisibility(EVisibility::Visible);
}

void SNGGChatWindow::OnClientSystem(FString Subtype)
{
	RenderSystemEvent(Subtype, FString());
}

#undef LOCTEXT_NAMESPACE
