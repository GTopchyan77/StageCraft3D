// Copyright Epic Games, Inc. All Rights Reserved.

#include "Remote/StageRemoteMapping.h"

#include "Algo/Reverse.h"
#include "Show/ShowControlTypes.h"

#define LOCTEXT_NAMESPACE "StageRemote"

namespace StageRemote
{
	namespace
	{
		constexpr float MidiMax = 127.f;

		/** "12" → 12; anything but one or two digits → INDEX_NONE. */
		int32 ParsePlaybackNumber(FStringView Text)
		{
			if (Text.IsEmpty() || Text.Len() > 2)
			{
				return INDEX_NONE;
			}
			int32 Number = 0;
			for (const TCHAR Char : Text)
			{
				if (!FChar::IsDigit(Char))
				{
					return INDEX_NONE;
				}
				Number = Number * 10 + (Char - TEXT('0'));
			}
			return Number;
		}

		/** Splits "/a/b/c" into its parts; false for a missing leading slash or an empty part. */
		bool SplitAddress(FStringView Address, TArray<FStringView, TInlineAllocator<4>>& OutParts)
		{
			if (!Address.StartsWith(TEXT('/')))
			{
				return false;
			}
			int32 Start = 1;
			for (int32 Index = 1; Index <= Address.Len(); ++Index)
			{
				if (Index == Address.Len() || Address[Index] == TEXT('/'))
				{
					if (Index == Start || OutParts.Num() == 4)
					{
						return false;
					}
					OutParts.Add(Address.Mid(Start, Index - Start));
					Start = Index + 1;
				}
			}
			return true;
		}

		bool Is(FStringView Part, const TCHAR* Word)
		{
			return Part.Equals(Word, ESearchCase::IgnoreCase);
		}

		/** A control's key in FStageMidiFeedbackMemory: trigger, channel (0 in HeardChannel) and number. */
		uint32 ControlKey(EStageMidiTrigger Trigger, int32 Channel, int32 Number)
		{
			return (static_cast<uint32>(Trigger) << 16) | (static_cast<uint32>(Channel & 0xFF) << 8) | static_cast<uint32>(Number & 0xFF);
		}

		EStageMidiTrigger KeyTrigger(uint32 Key) { return static_cast<EStageMidiTrigger>((Key >> 16) & 0xFF); }
		int32 KeyChannel(uint32 Key) { return static_cast<int32>((Key >> 8) & 0xFF); }
		int32 KeyNumber(uint32 Key) { return static_cast<int32>(Key & 0xFF); }

		/** The trigger of a raw channel message, false for the kinds a binding cannot answer. */
		bool TriggerOf(int32 Type, EStageMidiTrigger& OutTrigger)
		{
			if (Type == MidiNoteOn || Type == MidiNoteOff)
			{
				OutTrigger = EStageMidiTrigger::Note;
				return true;
			}
			if (Type == MidiControlChange)
			{
				OutTrigger = EStageMidiTrigger::ControlChange;
				return true;
			}
			return false;
		}

		/** The channel a binding's feedback goes to: its own, else where the control was last heard, else 1. */
		int32 AnswerChannel(const FStageMidiBinding& Binding, const FStageMidiFeedbackMemory& Memory)
		{
			const int32* Heard = Memory.HeardChannel.Find(ControlKey(Binding.Trigger, 0, Binding.Number));
			return Binding.Channel != 0 ? Binding.Channel : (Heard ? *Heard : 1);
		}

		void Remember(FStageMidiFeedbackMemory& Memory, uint32 Key, int32 Value, double NowSeconds, TArray<FMidiOutMessage>& OutMessages)
		{
			Memory.Shown.Add(Key, Value);
			if (Memory.RecentSent.Num() >= MaxRecentSent)
			{
				Memory.RecentSent.RemoveAt(0, Memory.RecentSent.Num() - MaxRecentSent + 1, EAllowShrinking::No);
			}
			Memory.RecentSent.Add({ Key, Value, NowSeconds });
			FMidiOutMessage& Message = OutMessages.AddDefaulted_GetRef();
			Message.Type = KeyTrigger(Key) == EStageMidiTrigger::Note ? MidiNoteOn : MidiControlChange;
			Message.Channel = KeyChannel(Key);
			Message.Data1 = KeyNumber(Key);
			Message.Data2 = Value;
		}

		int32 ToMidi(float Value)
		{
			return FMath::IsFinite(Value) ? FMath::Clamp(FMath::RoundToInt(Value * MidiMax), 0, 127) : 0;
		}

		/** The map in force as the user's own, so an edit of the default map keeps the rest of it. */
		void MakeCustom(FStageRemoteSettings& Settings)
		{
			if (!Settings.bCustomMidiMap)
			{
				Settings.MidiBindings = DefaultMidiBindings();
				Settings.bCustomMidiMap = true;
			}
		}

		/** The command a binding gives for a MIDI value: continuous actions get the value, the others pressed (1) or released (0). */
		FStageRemoteCommand MakeCommand(const FStageMidiBinding& Binding, float Value, bool bPressed)
		{
			FStageRemoteCommand Command;
			Command.Action = Binding.Action;
			Command.Playback = Binding.Playback - 1;
			Command.CueNumber = Binding.CueNumber;
			Command.Value = IsContinuous(Binding.Action) ? Value : (bPressed ? 1.f : 0.f);
			return Command;
		}
	}

	bool IsContinuous(EStageRemoteAction Action)
	{
		return Action == EStageRemoteAction::PlaybackLevel || Action == EStageRemoteAction::GrandMaster || Action == EStageRemoteAction::AudioMaster;
	}

	bool IsTrigger(EStageRemoteAction Action)
	{
		switch (Action)
		{
		case EStageRemoteAction::CueGo:
		case EStageRemoteAction::CueBack:
		case EStageRemoteAction::CueGoTo:
		case EStageRemoteAction::PlaybackGo:
		case EStageRemoteAction::PlaybackRelease:
			return true;
		default:
			return false;
		}
	}

	bool UsesPlayback(EStageRemoteAction Action)
	{
		switch (Action)
		{
		case EStageRemoteAction::PlaybackLevel:
		case EStageRemoteAction::PlaybackFlash:
		case EStageRemoteAction::PlaybackGo:
		case EStageRemoteAction::PlaybackRelease:
			return true;
		default:
			return false;
		}
	}

	bool ParseOsc(FStringView Address, TOptional<float> Value, FStageRemoteCommand& OutCommand)
	{
		if (Address.Len() > MaxOscAddressLength || (Value.IsSet() && !FMath::IsFinite(Value.GetValue())))
		{
			return false;
		}
		TArray<FStringView, TInlineAllocator<4>> Parts;
		if (!SplitAddress(Address, Parts) || Parts.Num() < 3 || !Is(Parts[0], TEXT("stagecraft")))
		{
			return false;
		}

		FStageRemoteCommand Command;
		if (Parts.Num() == 3 && Is(Parts[1], TEXT("cue")))
		{
			Command.Action = Is(Parts[2], TEXT("go")) ? EStageRemoteAction::CueGo
				: Is(Parts[2], TEXT("back")) ? EStageRemoteAction::CueBack
				: Is(Parts[2], TEXT("goto")) ? EStageRemoteAction::CueGoTo
				: EStageRemoteAction::None;
		}
		else if (Parts.Num() == 3 && Is(Parts[1], TEXT("master")))
		{
			Command.Action = Is(Parts[2], TEXT("grand")) ? EStageRemoteAction::GrandMaster
				: Is(Parts[2], TEXT("blackout")) ? EStageRemoteAction::Blackout
				: Is(Parts[2], TEXT("audio")) ? EStageRemoteAction::AudioMaster
				: EStageRemoteAction::None;
		}
		else if (Parts.Num() == 4 && Is(Parts[1], TEXT("playback")))
		{
			const int32 Number = ParsePlaybackNumber(Parts[2]);
			if (Number < 1 || Number > StagePlaybackRules::Count)
			{
				return false;
			}
			Command.Playback = Number - 1;
			Command.Action = Is(Parts[3], TEXT("level")) ? EStageRemoteAction::PlaybackLevel
				: Is(Parts[3], TEXT("flash")) ? EStageRemoteAction::PlaybackFlash
				: Is(Parts[3], TEXT("go")) ? EStageRemoteAction::PlaybackGo
				: Is(Parts[3], TEXT("release")) ? EStageRemoteAction::PlaybackRelease
				: EStageRemoteAction::None;
		}
		if (Command.Action == EStageRemoteAction::None)
		{
			return false;
		}

		if (Command.Action == EStageRemoteAction::CueGoTo)
		{
			// The argument is the cue, not a press: there is no release to ignore.
			if (!Value.IsSet())
			{
				return false;
			}
			Command.CueNumber = Value.GetValue();
			Command.Value = 1.f;
		}
		else if (IsContinuous(Command.Action))
		{
			if (!Value.IsSet())
			{
				return false;
			}
			Command.Value = Value.GetValue();
		}
		else
		{
			// A button: no argument fires at once; 1 / 0 are its press and release.
			Command.Value = Value.Get(1.f);
			if (IsTrigger(Command.Action) && !Command.IsDown())
			{
				return false;
			}
		}
		OutCommand = Command;
		return true;
	}

	int32 ResolveMidi(int32 Type, int32 Channel, int32 Data1, int32 Data2, TConstArrayView<FStageMidiBinding> Bindings,
		TArray<FStageRemoteCommand>& OutCommands)
	{
		if (Channel < 1 || Channel > 16 || Data1 < 0 || Data1 > 127 || Data2 < 0 || Data2 > 127)
		{
			return 0;
		}

		EStageMidiTrigger Trigger;
		float Value = 0.f;
		bool bPressed = false;
		if (Type == MidiNoteOn || Type == MidiNoteOff)
		{
			Trigger = EStageMidiTrigger::Note;
			bPressed = Type == MidiNoteOn && Data2 > 0;
			Value = bPressed ? Data2 / MidiMax : 0.f;
		}
		else if (Type == MidiControlChange)
		{
			Trigger = EStageMidiTrigger::ControlChange;
			Value = Data2 / MidiMax;
			// A CC button sends 127 and 0; a fader passing the middle counts as a press too.
			bPressed = Data2 >= 64;
		}
		else
		{
			return 0;
		}

		int32 Added = 0;
		for (const FStageMidiBinding& Binding : Bindings)
		{
			if (Binding.Trigger != Trigger || Binding.Number != Data1 || (Binding.Channel != 0 && Binding.Channel != Channel) || !IsValidBinding(Binding))
			{
				continue;
			}
			const FStageRemoteCommand Command = MakeCommand(Binding, Value, bPressed);
			if (IsTrigger(Command.Action) && !Command.IsDown())
			{
				continue;
			}
			OutCommands.Add(Command);
			++Added;
		}
		return Added;
	}

	bool LearnFromMidi(int32 Type, int32 Channel, int32 Data1, int32 Data2, FStageMidiBinding& InOutBinding)
	{
		if (Channel < 1 || Channel > 16 || Data1 < 0 || Data1 > 127 || Data2 <= 0 || Data2 > 127)
		{
			return false;
		}
		if (Type == MidiNoteOn)
		{
			InOutBinding.Trigger = EStageMidiTrigger::Note;
		}
		else if (Type == MidiControlChange)
		{
			InOutBinding.Trigger = EStageMidiTrigger::ControlChange;
		}
		else
		{
			return false;
		}
		InOutBinding.Channel = Channel;
		InOutBinding.Number = Data1;
		return true;
	}

	FString DescribeMidi(int32 Type, int32 Channel, int32 Data1, int32 Data2)
	{
		switch (Type)
		{
		case MidiNoteOn:
			return Data2 > 0 ? FString::Printf(TEXT("MIDI note %d ch %d on %d"), Data1, Channel, Data2) : FString::Printf(TEXT("MIDI note %d ch %d off"), Data1, Channel);
		case MidiNoteOff:
			return FString::Printf(TEXT("MIDI note %d ch %d off"), Data1, Channel);
		case MidiControlChange:
			return FString::Printf(TEXT("MIDI CC %d ch %d = %d"), Data1, Channel, Data2);
		default:
			return FString::Printf(TEXT("MIDI type %X ch %d"), Type, Channel);
		}
	}

	FString OscAddressFor(EStageRemoteAction Action, int32 PlaybackIndex)
	{
		const int32 Number = PlaybackIndex + 1;
		switch (Action)
		{
		case EStageRemoteAction::CueGo: return TEXT("/stagecraft/cue/go");
		case EStageRemoteAction::CueBack: return TEXT("/stagecraft/cue/back");
		case EStageRemoteAction::CueGoTo: return TEXT("/stagecraft/cue/goto");
		case EStageRemoteAction::PlaybackLevel: return FString::Printf(TEXT("/stagecraft/playback/%d/level"), Number);
		case EStageRemoteAction::PlaybackFlash: return FString::Printf(TEXT("/stagecraft/playback/%d/flash"), Number);
		case EStageRemoteAction::PlaybackGo: return FString::Printf(TEXT("/stagecraft/playback/%d/go"), Number);
		case EStageRemoteAction::PlaybackRelease: return FString::Printf(TEXT("/stagecraft/playback/%d/release"), Number);
		case EStageRemoteAction::GrandMaster: return TEXT("/stagecraft/master/grand");
		case EStageRemoteAction::Blackout: return TEXT("/stagecraft/master/blackout");
		case EStageRemoteAction::AudioMaster: return TEXT("/stagecraft/master/audio");
		default: return FString();
		}
	}

	FText DescribeAction(EStageRemoteAction Action, int32 PlaybackNumber)
	{
		const FText Number = FText::AsNumber(PlaybackNumber);
		switch (Action)
		{
		case EStageRemoteAction::CueGo: return LOCTEXT("CueGo", "Cue GO");
		case EStageRemoteAction::CueBack: return LOCTEXT("CueBack", "Cue BACK");
		case EStageRemoteAction::CueGoTo: return LOCTEXT("CueGoTo", "Go to cue");
		case EStageRemoteAction::PlaybackLevel: return FText::Format(LOCTEXT("PlaybackLevel", "Playback {0} level"), Number);
		case EStageRemoteAction::PlaybackFlash: return FText::Format(LOCTEXT("PlaybackFlash", "Playback {0} flash"), Number);
		case EStageRemoteAction::PlaybackGo: return FText::Format(LOCTEXT("PlaybackGo", "Playback {0} GO"), Number);
		case EStageRemoteAction::PlaybackRelease: return FText::Format(LOCTEXT("PlaybackRelease", "Playback {0} release"), Number);
		case EStageRemoteAction::GrandMaster: return LOCTEXT("GrandMaster", "Grand Master");
		case EStageRemoteAction::Blackout: return LOCTEXT("Blackout", "Blackout");
		case EStageRemoteAction::AudioMaster: return LOCTEXT("AudioMaster", "Audio master");
		default: return FText::GetEmpty();
		}
	}

	const TArray<FStageMidiBinding>& DefaultMidiBindings()
	{
		static const TArray<FStageMidiBinding> Defaults = []
		{
			TArray<FStageMidiBinding> Bindings;
			auto Add = [&Bindings](int32 Number, EStageRemoteAction Action, int32 Playback = 1)
			{
				FStageMidiBinding& Binding = Bindings.AddDefaulted_GetRef();
				Binding.Trigger = EStageMidiTrigger::ControlChange;
				Binding.Number = Number;
				Binding.Action = Action;
				Binding.Playback = Playback;
			};
			// nanoKONTROL2 factory CC mode: faders 0–7, S 32–39, M 48–55, R 64–71, knobs 16–23, transport 41 ▶, 43 ◀◀, 45 ●.
			for (int32 Strip = 0; Strip < 8; ++Strip)
			{
				Add(Strip, EStageRemoteAction::PlaybackLevel, Strip + 1);
				Add(32 + Strip, EStageRemoteAction::PlaybackFlash, Strip + 1);
				Add(48 + Strip, EStageRemoteAction::PlaybackGo, Strip + 1);
				Add(64 + Strip, EStageRemoteAction::PlaybackRelease, Strip + 1);
			}
			Add(16, EStageRemoteAction::GrandMaster);
			Add(17, EStageRemoteAction::AudioMaster);
			Add(41, EStageRemoteAction::CueGo);
			Add(43, EStageRemoteAction::CueBack);
			Add(45, EStageRemoteAction::Blackout);
			return Bindings;
		}();
		return Defaults;
	}

	TConstArrayView<FStageMidiBinding> EffectiveMidiBindings(const FStageRemoteSettings& Settings)
	{
		return Settings.bCustomMidiMap ? TConstArrayView<FStageMidiBinding>(Settings.MidiBindings) : TConstArrayView<FStageMidiBinding>(DefaultMidiBindings());
	}

	bool OverlapsTrigger(const FStageMidiBinding& A, const FStageMidiBinding& B)
	{
		return A.Trigger == B.Trigger && A.Number == B.Number && (A.Channel == B.Channel || A.Channel == 0 || B.Channel == 0);
	}

	bool IsValidBinding(const FStageMidiBinding& Binding)
	{
		const uint8 Action = static_cast<uint8>(Binding.Action);
		const uint8 Trigger = static_cast<uint8>(Binding.Trigger);
		return Action > static_cast<uint8>(EStageRemoteAction::None) && Action <= static_cast<uint8>(EStageRemoteAction::AudioMaster)
			&& Trigger <= static_cast<uint8>(EStageMidiTrigger::ControlChange)
			&& Binding.Channel >= 0 && Binding.Channel <= 16 && Binding.Number >= 0 && Binding.Number <= 127
			&& (!UsesPlayback(Binding.Action) || (Binding.Playback >= 1 && Binding.Playback <= StagePlaybackRules::Count))
			&& FMath::IsFinite(Binding.CueNumber);
	}

	bool Sanitize(FStageRemoteSettings& Settings)
	{
		const FStageRemoteSettings Before = Settings;

		if (static_cast<uint8>(Settings.OscScope) > static_cast<uint8>(EStageOscScope::Network))
		{
			Settings.OscScope = EStageOscScope::Off;
		}
		if (Settings.OscPort < MinPort || Settings.OscPort > MaxPort)
		{
			Settings.OscPort = DefaultOscPort;
		}
		if (Settings.OscFeedbackPort != 0 && (Settings.OscFeedbackPort < MinPort || Settings.OscFeedbackPort > MaxPort))
		{
			Settings.OscFeedbackPort = DefaultFeedbackPort;
		}
		Settings.MidiInputName.TrimStartAndEndInline();
		Settings.MidiInputName.LeftInline(MaxDeviceNameLength);
		Settings.MidiOutputName.TrimStartAndEndInline();
		Settings.MidiOutputName.LeftInline(MaxDeviceNameLength);

		// Keep the last binding of each control (an "any channel" one overlaps every channel): learn appends, so the newest wins.
		TArray<FStageMidiBinding> Kept;
		for (int32 Index = Settings.MidiBindings.Num() - 1; Index >= 0 && Kept.Num() < MaxMidiBindings; --Index)
		{
			const FStageMidiBinding& Binding = Settings.MidiBindings[Index];
			if (IsValidBinding(Binding) && !Kept.ContainsByPredicate([&Binding](const FStageMidiBinding& Other) { return OverlapsTrigger(Other, Binding); }))
			{
				Kept.Add(Binding);
			}
		}
		Algo::Reverse(Kept);
		Settings.MidiBindings = MoveTemp(Kept);

		return !(Settings == Before);
	}

	bool IsSameTarget(const FStageMidiBinding& Binding, EStageRemoteAction Action, int32 PlaybackNumber, float CueNumber)
	{
		return Binding.Action == Action && Action != EStageRemoteAction::None
			&& (!UsesPlayback(Action) || Binding.Playback == PlaybackNumber)
			&& (Action != EStageRemoteAction::CueGoTo || Binding.CueNumber == CueNumber);
	}

	TArray<FStageMidiBinding> BindingsForTarget(TConstArrayView<FStageMidiBinding> Bindings, EStageRemoteAction Action, int32 PlaybackNumber, float CueNumber)
	{
		TArray<FStageMidiBinding> Found;
		for (const FStageMidiBinding& Binding : Bindings)
		{
			if (IsSameTarget(Binding, Action, PlaybackNumber, CueNumber))
			{
				Found.Add(Binding);
			}
		}
		return Found;
	}

	FString DescribeControl(const FStageMidiBinding& Binding)
	{
		const TCHAR* Kind = Binding.Trigger == EStageMidiTrigger::Note ? TEXT("Note") : TEXT("CC");
		return Binding.Channel == 0 ? FString::Printf(TEXT("%s %d"), Kind, Binding.Number) : FString::Printf(TEXT("%s %d ch %d"), Kind, Binding.Number, Binding.Channel);
	}

	bool AssignLearned(FStageRemoteSettings& Settings, const FStageMidiBinding& Learned)
	{
		if (!IsValidBinding(Learned))
		{
			return false;
		}
		MakeCustom(Settings);
		Settings.MidiBindings.RemoveAll([&Learned](const FStageMidiBinding& Binding)
		{
			return OverlapsTrigger(Binding, Learned) || IsSameTarget(Binding, Learned.Action, Learned.Playback, Learned.CueNumber);
		});
		Settings.MidiBindings.Add(Learned);
		return true;
	}

	bool ClearTarget(FStageRemoteSettings& Settings, EStageRemoteAction Action, int32 PlaybackNumber, float CueNumber)
	{
		if (BindingsForTarget(EffectiveMidiBindings(Settings), Action, PlaybackNumber, CueNumber).IsEmpty())
		{
			return false;
		}
		MakeCustom(Settings);
		Settings.MidiBindings.RemoveAll([=](const FStageMidiBinding& Binding) { return IsSameTarget(Binding, Action, PlaybackNumber, CueNumber); });
		return true;
	}

	bool HasInputOn(const FStageRemoteSettings& Settings)
	{
		return Settings.OscScope != EStageOscScope::Off || !Settings.MidiInputName.TrimStartAndEnd().IsEmpty()
			|| !Settings.MidiOutputName.TrimStartAndEnd().IsEmpty();
	}

	bool HasSameInputs(const FStageRemoteSettings& A, const FStageRemoteSettings& B)
	{
		return A.OscScope == B.OscScope && A.OscPort == B.OscPort && A.OscFeedbackPort == B.OscFeedbackPort
			&& A.MidiInputName == B.MidiInputName && A.MidiOutputName == B.MidiOutputName;
	}

	bool NeedsVerdict(const FStageRemoteSettings& Current, const FStageRemoteSettings& Requested)
	{
		const bool bMapEdit = HasSameInputs(Current, Requested) && !(Current == Requested);
		return HasInputOn(Requested) && !bMapEdit;
	}

	TOptional<int32> FeedbackValue(const FStageMidiBinding& Binding, const FMidiShowState& State)
	{
		if (!IsValidBinding(Binding))
		{
			return {};
		}
		const int32 Index = Binding.Playback - 1;
		const auto Lit = [](bool bOn) { return bOn ? 127 : 0; };
		switch (Binding.Action)
		{
		case EStageRemoteAction::PlaybackLevel:
			return State.Levels.IsValidIndex(Index) ? TOptional<int32>(ToMidi(State.Levels[Index])) : TOptional<int32>();
		case EStageRemoteAction::PlaybackFlash:
			return State.Flashing.IsValidIndex(Index) ? TOptional<int32>(Lit(State.Flashing[Index])) : TOptional<int32>();
		case EStageRemoteAction::PlaybackGo:
			return State.Active.IsValidIndex(Index) ? TOptional<int32>(Lit(State.Active[Index])) : TOptional<int32>();
		case EStageRemoteAction::GrandMaster:
			return ToMidi(State.GrandMaster);
		case EStageRemoteAction::AudioMaster:
			return ToMidi(State.AudioMaster);
		case EStageRemoteAction::Blackout:
			return Lit(State.bBlackout);
		default:
			return {};
		}
	}

	bool NoteMidiInput(int32 Type, int32 Channel, int32 Data1, int32 Data2, TConstArrayView<FStageMidiBinding> Bindings, FStageMidiFeedbackMemory& Memory,
		double NowSeconds)
	{
		EStageMidiTrigger Trigger;
		if (Channel < 1 || Channel > 16 || Data1 < 0 || Data1 > 127 || Data2 < 0 || Data2 > 127 || !TriggerOf(Type, Trigger))
		{
			return false;
		}
		Memory.HeardChannel.Add(ControlKey(Trigger, 0, Data1), Channel);

		const bool bMovesItself = Bindings.ContainsByPredicate([=](const FStageMidiBinding& Binding)
		{
			return Binding.Trigger == Trigger && Binding.Number == Data1 && (Binding.Channel == 0 || Binding.Channel == Channel) && IsContinuous(Binding.Action);
		});
		if (!bMovesItself)
		{
			return false;
		}
		const uint32 Key = ControlKey(Trigger, Channel, Data1);
		Memory.Shown.Add(Key, Type == MidiNoteOff ? 0 : Data2);
		Memory.LastMoved.Add(Key, NowSeconds);
		return true;
	}

	bool IsMidiEcho(int32 Type, int32 Channel, int32 Data1, int32 Data2, FStageMidiFeedbackMemory& Memory, double NowSeconds)
	{
		Memory.RecentSent.RemoveAll([NowSeconds](const FStageMidiFeedbackMemory::FSent& Sent) { return NowSeconds - Sent.Time > MidiEchoSeconds; });
		EStageMidiTrigger Trigger;
		if (Channel < 1 || Channel > 16 || !TriggerOf(Type, Trigger))
		{
			return false;
		}
		const uint32 Key = ControlKey(Trigger, Channel, Data1);
		const int32 Value = Type == MidiNoteOff ? 0 : Data2;
		return Memory.RecentSent.ContainsByPredicate([Key, Value](const FStageMidiFeedbackMemory::FSent& Sent) { return Sent.Key == Key && Sent.Value == Value; });
	}

	int32 CollectMidiFeedback(TConstArrayView<FStageMidiBinding> Bindings, const FMidiShowState& State, FStageMidiFeedbackMemory& Memory,
		double NowSeconds, TArray<FMidiOutMessage>& OutMessages, double& OutHoldUntil)
	{
		OutHoldUntil = 0.0;
		TSet<uint32> Answered;
		int32 Added = 0;
		for (const FStageMidiBinding& Binding : Bindings)
		{
			const TOptional<int32> Value = FeedbackValue(Binding, State);
			if (!Value.IsSet())
			{
				continue;
			}
			const uint32 Key = ControlKey(Binding.Trigger, AnswerChannel(Binding, Memory), Binding.Number);
			bool bAlreadyAnswered = false;
			Answered.Add(Key, &bAlreadyAnswered);
			const int32* Shown = Memory.Shown.Find(Key);
			if (bAlreadyAnswered || (Shown && *Shown == Value.GetValue()))
			{
				continue;
			}
			if (const double* Moved = Memory.LastMoved.Find(Key))
			{
				const double HoldUntil = *Moved + MidiHoldSeconds;
				if (NowSeconds < HoldUntil)
				{
					OutHoldUntil = FMath::Max(OutHoldUntil, HoldUntil);
					continue;
				}
				Memory.LastMoved.Remove(Key);
			}
			Remember(Memory, Key, Value.GetValue(), NowSeconds, OutMessages);
			++Added;
		}
		return Added;
	}

	int32 CollectUnboundOff(TConstArrayView<FStageMidiBinding> Bindings, FStageMidiFeedbackMemory& Memory, double NowSeconds,
		TArray<FMidiOutMessage>& OutMessages)
	{
		const FMidiShowState NoState;
		TSet<uint32> Answered;
		for (const FStageMidiBinding& Binding : Bindings)
		{
			// Any action with a state answers; FeedbackValue needs the arrays only for its value, not for whether it has one.
			const bool bHasState = IsValidBinding(Binding) && Binding.Action != EStageRemoteAction::CueGo && Binding.Action != EStageRemoteAction::CueBack
				&& Binding.Action != EStageRemoteAction::CueGoTo && Binding.Action != EStageRemoteAction::PlaybackRelease;
			if (bHasState)
			{
				Answered.Add(ControlKey(Binding.Trigger, AnswerChannel(Binding, Memory), Binding.Number));
			}
		}
		TArray<uint32> Darken;
		for (const TPair<uint32, int32>& Shown : Memory.Shown)
		{
			if (Shown.Value != 0 && !Answered.Contains(Shown.Key))
			{
				Darken.Add(Shown.Key);
			}
		}
		Darken.Sort();
		for (const uint32 Key : Darken)
		{
			Remember(Memory, Key, 0, NowSeconds, OutMessages);
		}
		return Darken.Num();
	}

	bool Enqueue(TArray<FStageRemoteCommand>& Pending, const FStageRemoteCommand& Command)
	{
		if (IsContinuous(Command.Action))
		{
			for (FStageRemoteCommand& Queued : Pending)
			{
				if (Queued.Action == Command.Action && Queued.Source == Command.Source && Queued.Playback == Command.Playback)
				{
					Queued.Value = Command.Value;
					return true;
				}
			}
		}
		if (Pending.Num() >= MaxCommandsPerFrame)
		{
			return false;
		}
		Pending.Add(Command);
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
