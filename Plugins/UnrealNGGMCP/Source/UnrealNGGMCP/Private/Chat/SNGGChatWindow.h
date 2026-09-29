// Copyright 2025-2026 NGG. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Containers/Ticker.h"
#include "FSidecarClient.h"
#include "SidecarLauncher.h" // ESidecarEnsureResult (used by value in ContinueConnectAfterEnsure)

class FSidecarClient;
class FJsonObject;
class FSlateStyleSet;
class SMultiLineEditableTextBox;
class STextBlock;
class SButton;
class SWidget;
class SMenuAnchor;
class SBorder;
class SVerticalBox;
class ITableRow;
class STableViewBase;
template<typename ItemType> class SListView;
template<typename OptionType> class SComboBox;
enum class ENGGChatState : uint8;

/** One row in the slash-command autocomplete popup. */
struct FNGGChatSlashCommand
{
	FString Name;        // including leading '/'
	FString Description; // one-line
};

/** One entry in the model selector combo. */
struct FNGGChatModelOption
{
	FString DisplayLabel;          // user-visible text, e.g. "Opus 4.7"
	FString Description;           // optional subtitle line, e.g. "Most capable for ambitious work"
	FString CliArg;                // passed to `claude --model`. Empty = omit flag.
	bool    bIsSeparator = false;  // non-selectable divider row
};

/** One entry in the effort (extended-thinking) selector combo. */
struct FNGGChatEffortOption
{
	FString DisplayLabel;   // user-visible text, e.g. "Think Hard"
	FString Description;    // one-line subtitle (shown in dropdown rows)
	FString TriggerPhrase;  // prepended to the outgoing prompt; empty = no thinking
	FString SaveId;         // stable id persisted to ini
};

/** One row in the @-mention file/folder picker popup. */
struct FNGGChatAtMention
{
	FString RelativePath; // project-relative POSIX path, e.g. "Source/MyGame/Core/Foo.cpp"
	FString DisplayLabel; // basename shown prominently
	bool    bIsDirectory = false;
};

/**
 * SNGGChatWindow
 *
 * Terminal-style chat UI hosted inside the "NGG Chat" nomad tab. Renders
 * events as colored lines (monospace) in a scrolling box, with inline
 * permission approval buttons when Claude's MCP permission-prompt-tool
 * asks before calling Write/Edit/Bash/etc.
 */
class SNGGChatWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SNGGChatWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SNGGChatWindow();

private:
	// ---- Line kinds (color prefixes) ----------------------------------
	enum class ELineKind : uint8
	{
		User,
		Assistant,
		Thinking,
		ToolUse,
		ToolOk,
		ToolErr,
		Error,
		System,
		Perm,
	};

	// ---- Connection lifecycle -----------------------------------------
	void BeginConnect();
	/** Game-thread continuation of BeginConnect, run after the (blocking)
	 *  EnsureCompatibleSidecar work has completed on a background thread. */
	void ContinueConnectAfterEnsure(ESidecarEnsureResult Ensure);
	void TickHealthProbe();
	/** Poll the first-run npm install; returns true to keep ticking. On
	 *  success re-enters BeginConnect; on failure surfaces the error. */
	bool TickDepsWait();
	void OnClientStateChanged(ENGGChatState NewState);
	void OnClientEvent(TSharedPtr<FJsonObject> Event);
	void OnClientError(FString Message);
	void OnClientPermissionRequest(TSharedPtr<FJsonObject> Request);
	void OnClientSystem(FString Subtype);
	/** Live model catalog arrived from the sidecar — rebuild the model dropdown
	 * from it. Empty list = fetch failed; keep the hardcoded fallback. */
	void OnClientModels(const TArray<struct FNGGSidecarModel>& Models);

	// ---- UI actions ---------------------------------------------------
	FReply OnSendClicked();
	FReply OnCancelClicked();
	FReply OnClearClicked();
	FReply OnReconnectClicked();
	FReply OnRestartSidecarClicked();
	FReply OnKillSidecarClicked();
	FReply OnBypassPermissionsClicked();
	FReply OnInputKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	void   SubmitPrompt();

	// ---- Terminal rendering -------------------------------------------
	/** Append one colored line. Returns the widget (so permission lines can
	 * keep a handle for later greying). */
	TSharedRef<SWidget> AppendLine(ELineKind Kind, const FString& Body);
	/** Append a permission-prompt line (colored prefix + 3 buttons).
	 * Caller owns the returned widget; it is already in the scroll box. */
	void AppendPermissionPrompt(const FString& RequestId, const FString& Tool, const FString& InputSummary);
	/** Auto-scroll to bottom unless the user scrolled up. */
	void MaybeAutoScroll();
	/** Cap TranscriptBuffer growth: if it exceeds the char budget, drop whole
	 *  lines from the front (trimming to the next newline boundary) so the
	 *  newest content is kept and the per-SetText rebuild stays bounded. */
	void TrimTranscriptBuffer();
	/** Poll ticker: track whether the user is near the bottom of the scroll. */
	bool TickScrollState(float DeltaTime);

	// ---- Syntax highlighting ------------------------------------------
	/** One colored text run — Style names a rich-text style registered on
	 * TranscriptStyle (see Construct). The renderer emits one run per token. */
	struct FColoredToken { FString Text; FName Style; };
	/** One line = ordered array of colored tokens. */
	using FTokenLine = TArray<FColoredToken>;

	/** Render an array of tokenized lines as a labeled code block. */
	void AppendHighlightedBlock(const TArray<FTokenLine>& Lines, const FString& LangLabel);
	TArray<FTokenLine> TokenizeJson(const FString& Text) const;
	TArray<FTokenLine> TokenizeCpp(const FString& Code) const;

	// ---- Event rendering helpers --------------------------------------
	void RenderAssistantMessage(const TSharedPtr<FJsonObject>& Message);
	void RenderUserMessage(const TSharedPtr<FJsonObject>& Message);
	void RenderSystemEvent(const FString& Subtype, const FString& Body);
	/** Split text at ``` fences and render prose vs code blocks. Kind controls the prose color. */
	void RenderTextWithCodeBlocks(const FString& Text, ELineKind Kind = ELineKind::Assistant);
	/** Render a fenced code block with language-appropriate syntax coloring. */
	void AppendCodeBlock(const FString& Lang, const FString& CodeBody);
	/** Capture a bitmap image from the Windows clipboard, encode to PNG base64.
	 *  Returns the image descriptor on success; empty optional if clipboard has no image. */
	TOptional<FNGGPastedImage> TrySavePastedImage();
	FString StringifyJson(const TSharedPtr<FJsonObject>& Obj) const;
	FString TruncateForLine(const FString& In) const;
	/** Derive a short, useful one-line summary from a tool's input JSON. */
	FString SummariseToolInput(const FString& ToolName, const TSharedPtr<FJsonObject>& Input) const;

	// ---- Slash-command autocomplete -----------------------------------
	void   InitSlashCommands();
	void   OnInputTextChanged(const FText& NewText);
	void   RebuildFilteredCommands(const FString& Query);
	void   ShowSlashPopup();
	void   HideSlashPopup();
	void   AcceptHighlightedCommand();
	void   HighlightDelta(int32 Delta);
	bool   IsSlashPopupOpen() const;
	TSharedRef<ITableRow> GenerateSlashRow(TSharedPtr<FNGGChatSlashCommand> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void   OnSlashSelectionChanged(TSharedPtr<FNGGChatSlashCommand> Item, ESelectInfo::Type SelectInfo);
	void   OnSlashRowClicked(TSharedPtr<FNGGChatSlashCommand> Item);

	// ---- Editor-selection context ------------------------------------
	/** Returns a human-readable description of the user's current editor
	 * selection (selected actors + content-browser assets). Empty if nothing
	 * is selected. */
	FString GatherSelectionSummary() const;
	/** Returns just a live count (actors + assets) for the toggle label. */
	int32 GetSelectionCount() const;
	FReply OnSelectionToggleClicked();
	bool   TickSelectionLabel(float DeltaTime);

	// ---- @-mention file/folder picker ---------------------------------
	/** Lazy-populate AllAtMentions from the project directory on first open.
	 * Skips heavy folders (Binaries, Intermediate, Saved, node_modules, .git).
	 * Capped at a few thousand entries. */
	void   InitAtMentions();
	bool   IsAtPopupOpen() const;
	void   RebuildFilteredMentions(const FString& Query);
	void   ShowAtPopup();
	void   HideAtPopup();
	void   AcceptHighlightedMention();
	void   AtHighlightDelta(int32 Delta);
	TSharedRef<ITableRow> GenerateAtRow(TSharedPtr<FNGGChatAtMention> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void   OnAtSelectionChanged(TSharedPtr<FNGGChatAtMention> Item, ESelectInfo::Type SelectInfo);
	void   OnAtRowClicked(TSharedPtr<FNGGChatAtMention> Item);
	/** Find the active @-token in input. Returns true if the text ends with
	 * @non-whitespace preceded by start-of-text or a space. */
	bool   ExtractAtQuery(const FString& Text, FString& OutQuery, int32& OutAtPos) const;

	// ---- Input history ------------------------------------------------
	void PushHistory(const FString& Text);
	void LoadHistory();
	void SaveHistory();
	void NavigateHistory(int32 Delta);

	// ---- Auto-approve persistence -------------------------------------
	// SessionAutoApproveTools is project-scoped: persisted to GEditorPerProjectIni
	// so an "Always" decision survives editor restarts.
	void LoadAutoApproveTools();
	void SaveAutoApproveTools();

	// ---- Bypass-permissions toggle persistence ------------------------
	// Project-scoped: a developer who opts in for a project keeps the setting
	// after editor restart, but it does not leak across projects.
	void LoadBypassPermissions();
	void SaveBypassPermissions();
	void RefreshBypassButtonAppearance();

	// ---- Model selector ----------------------------------------------
	void LoadSelectedModel();
	void SaveSelectedModel(const FString& CliArg);
	void OnModelSelectionChanged(TSharedPtr<FNGGChatModelOption> NewValue, ESelectInfo::Type SelectInfo);
	TSharedRef<SWidget> GenerateModelComboRow(TSharedPtr<FNGGChatModelOption> Item);

	// ---- Effort (extended-thinking) selector --------------------------
	void InitEffortOptions();
	void LoadSelectedEffort();
	void SaveSelectedEffort(const FString& SaveId);
	void OnEffortSelectionChanged(TSharedPtr<FNGGChatEffortOption> NewValue, ESelectInfo::Type SelectInfo);
	TSharedRef<SWidget> GenerateEffortComboRow(TSharedPtr<FNGGChatEffortOption> Item);
	/** Turn a CLI-reported model id like "claude-opus-4-7-20251115" into a
	 * friendly label like "Opus 4.7". Returns the raw id if parsing fails. */
	static FString FriendlyLabelFromModelId(const FString& ModelId);

	// ---- Colors -------------------------------------------------------
	static FLinearColor ColorForKind(ELineKind Kind);
	static const TCHAR* PrefixForKind(ELineKind Kind);
	// Rich-text style tag name for a line kind, used by the marshaller.
	static const TCHAR* TagForKind(ELineKind Kind);
	// Escape characters that would be interpreted as rich-text markup.
	static FString      EscapeRichMarkup(const FString& In);

	// ---- Token usage accounting --------------------------------------
	/** Accumulate usage fields from a stream-json `result` event and refresh
	 *  the status-bar display. */
	void AccumulateUsageFromResult(const TSharedPtr<FJsonObject>& ResultEvent);
	void ResetUsage();
	void UpdateUsageDisplay();
	/** Persist / restore cumulative usage counters to GEditorPerProjectIni so
	 *  the numbers survive editor restarts. */
	void SaveUsageToConfig();
	void LoadUsageFromConfig();
	static FString FormatTokenCount(int64 N);

	// ---- State --------------------------------------------------------
	TSharedPtr<FSidecarClient>               Client;
	// Terminal transcript: a single scrollable, selectable read-only text box
	// fed by TranscriptBuffer. Lets the user Ctrl+A / Ctrl+C the whole log.
	// The buffer holds rich-text markup (e.g. "<NGGUser>[user] hi</>\n"), which
	// the marshaller renders as colored runs using styles from TranscriptStyle.
	TSharedPtr<SMultiLineEditableTextBox>    TranscriptText;
	FString                                  TranscriptBuffer;
	TSharedPtr<FSlateStyleSet>               TranscriptStyle;
	// Pending permission prompts are displayed as action rows in this panel,
	// which sits just above the input row (so the transcript stays pure text).
	TSharedPtr<SBorder>                      PendingPermsBorder;
	TSharedPtr<SVerticalBox>                 PendingPermsList;
	TSharedPtr<SMultiLineEditableTextBox>    InputBox;
	TSharedPtr<STextBlock>                   StatusText;
	TSharedPtr<class SCircularThrobber>      ThinkingThrobber;
	TSharedPtr<SButton>                      SendButton;
	TSharedPtr<SButton>                      CancelButton;
	TSharedPtr<SButton>                      ReconnectButton;

	// "Thinking..." animation: ticker cycles dots in StatusText while
	// ENGGChatState::Thinking; throbber spins in parallel.
	FTSTicker::FDelegateHandle ThinkingAnimTicker;
	double                     ThinkingAnimStartTime = 0.0;

	TArray<FString> History;
	int32 HistoryCursor = INDEX_NONE;
	FString DraftBeforeHistory;

	// Images pasted from clipboard, attached to the next outgoing prompt.
	TArray<FNGGPastedImage> PendingImages;

	// Restart/reconnect ticker handle (armed by OnRestartSidecarClicked while it
	// polls for the old sidecar to exit). Stored so the destructor can remove it
	// and so a repeated restart click cancels the prior poll instead of spawning
	// a duplicate, concurrent reconnect loop.
	FTSTicker::FDelegateHandle RestartTicker;

	// Health-probe ticker handle (used during auto-spawn wait).
	FTSTicker::FDelegateHandle HealthTicker;
	double HealthProbeDeadline = 0.0;
	double HealthRespawnAt     = 0.0;   // Time at which to re-issue spawn if still no sidecar.
	bool   bAwaitingSidecar    = false;
	bool   bHealthRespawned    = false; // Have we already retried the spawn for this wait?

	// First-run npm-install wait (armed by BeginConnect when the bundled Node
	// packages aren't installed yet; polls FNodeDepsInstaller until resolved).
	FTSTicker::FDelegateHandle DepsTicker;
	bool   bDepsNoticeShown = false; // "First-run setup..." line appended once per window.

	// Auto-scroll tracking: if user scrolls up, we stop auto-following.
	bool   bAutoScroll = true;

	// Session-wide auto-approve list (set from "Always" button).
	TSet<FString> SessionAutoApproveTools;

	// Track seen permission ids so we can ignore dupes (the sidecar replays
	// pending requests to late-joining clients).
	TSet<FString> SeenPermissionIds;

	FString SessionId;

	// ---- Slash-command state -----------------------------------------
	TArray<FNGGChatSlashCommand>                            AllSlashCommands;
	TArray<TSharedPtr<FNGGChatSlashCommand>>                FilteredCommands;
	TSharedPtr<SMenuAnchor>                                 SlashMenuAnchor;
	TSharedPtr<SListView<TSharedPtr<FNGGChatSlashCommand>>> SlashListView;
	int32                                                   HighlightedIndex = 0;
	bool                                                    bSuppressTextChanged = false;

	// ---- Model selector state ----------------------------------------
	TArray<TSharedPtr<FNGGChatModelOption>>                 ModelOptions;
	TSharedPtr<FNGGChatModelOption>                         SelectedModel;
	TSharedPtr<SComboBox<TSharedPtr<FNGGChatModelOption>>>  ModelCombo;
	// Resolved model id reported by the Claude CLI `init` system event —
	// e.g. "claude-sonnet-4-5-20250929". Shown in the status bar so the user
	// can see which exact build their chosen alias mapped to.
	FString                                                 ResolvedModelId;
	TSharedPtr<STextBlock>                                  ResolvedModelText;

	// ---- Token usage state -------------------------------------------
	// Cumulative counters for the lifetime of the current conversation
	// (reset on /clear or the Clear button). Updated from stream-json
	// `result` events each turn.
	TSharedPtr<STextBlock>                                  UsageText;
	int64                                                   UsageInputTokens       = 0;
	int64                                                   UsageOutputTokens      = 0;
	int64                                                   UsageCacheReadTokens   = 0;
	int64                                                   UsageCacheWriteTokens  = 0;
	double                                                  UsageCostUsd           = 0.0;
	int32                                                   UsageTurns             = 0;
	// Whether we have pushed the saved model to the sidecar since the last
	// Ready transition. Avoids spamming set_model on every state change.
	bool                                                    bModelPushedThisConnection = false;
	// Whether we've requested the live model catalog since the last Ready. The
	// dropdown is populated dynamically from GET /v1/models (relayed by the
	// sidecar); the hardcoded ModelOptions built in Construct is the fallback
	// shown until the catalog arrives (or if the fetch fails).
	bool                                                    bModelsRequestedThisConnection = false;

	// ---- Bypass-permissions state ------------------------------------
	// When on, the sidecar respawns claude with --dangerously-skip-permissions
	// so tool calls don't route through the per-call permission gate. The
	// button is purely a UX shortcut — tools allowed by .claude/settings*.json
	// already short-circuit the gate, but most read tools the AI uses (ue5_*
	// inspectors, context7 lookups) aren't covered by typical allow lists.
	TSharedPtr<SButton>                                     BypassPermsButton;
	TSharedPtr<STextBlock>                                  BypassPermsButtonText;
	bool                                                    bBypassPermissions = false;
	bool                                                    bBypassPushedThisConnection = false;

	// ---- Effort selector state ---------------------------------------
	TArray<TSharedPtr<FNGGChatEffortOption>>                EffortOptions;
	TSharedPtr<FNGGChatEffortOption>                        SelectedEffort;
	TSharedPtr<SComboBox<TSharedPtr<FNGGChatEffortOption>>> EffortCombo;

	// ---- @-mention state ---------------------------------------------
	TArray<FNGGChatAtMention>                               AllAtMentions;
	TArray<TSharedPtr<FNGGChatAtMention>>                   FilteredAtMentions;
	TSharedPtr<SMenuAnchor>                                 AtMenuAnchor;
	TSharedPtr<SListView<TSharedPtr<FNGGChatAtMention>>>    AtListView;
	int32                                                   AtHighlightedIndex = 0;
	bool                                                    bAtMentionsInitialized = false;
	// Text offset of the leading '@' for the currently active token. Used by
	// Accept to splice only that token, preserving any surrounding text.
	int32                                                   AtActiveTokenStart = INDEX_NONE;

	// ---- Selection-context toggle ------------------------------------
	// When on, SubmitPrompt prepends a <ue5_selection> block describing the
	// user's current editor selection (actors + content-browser assets).
	TSharedPtr<SButton>                                     SelectionButton;
	TSharedPtr<STextBlock>                                  SelectionButtonText;
	bool                                                    bIncludeSelection = false;

	int32                                                   LastSelectionCount = 0;
	FTSTicker::FDelegateHandle                              SelectionLabelTicker;

	// ---- Blueprint-editor selection banner ---------------------------
	// Sits between the status bar and the transcript. When the developer has
	// nodes selected in any open Blueprint editor, the banner appears with a
	// hint about what to ask Claude. Hidden when nothing is selected. Refreshed
	// from the same 0.5Hz ticker used for SelectionButtonText.
	TSharedPtr<SBorder>                                     BpSelectionBanner;
	TSharedPtr<STextBlock>                                  BpSelectionBannerText;
	int32                                                   LastBpSelectionCount   = 0;
	FString                                                 LastBpSelectionAsset;
	/** Returns the number of selected BP nodes in the active BP editor, and
	 * fills OutAssetName with the BP's short name (e.g. "BP_ThrowBombGameMode").
	 * Returns 0 if no BP editor is open or nothing is selected. */
	int32 GetBpSelectionInfo(FString& OutAssetName) const;
	/** Update banner text + visibility from the current BP-editor selection. */
	void  RefreshBpSelectionBanner();
};
