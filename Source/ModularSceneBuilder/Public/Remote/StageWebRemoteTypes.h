// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageWebRemoteTypes.generated.h"

/** Where the Web Remote listens (Docs/ADR/0036 §6). Off by default; Network must be chosen explicitly. */
UENUM()
enum class EStageWebRemoteScope : uint8
{
	Off,
	/** 127.0.0.1 only: a browser on this PC. */
	ThisComputer,
	/** Every network card: phones and tablets on the LAN. Plain HTTP, guarded by PIN pairing. */
	Network,
};

/**
 * The Web Remote of this PC (Docs/ADR/0036 §6), saved in GameUserSettings. UStageWebRemoteSubsystem is the only writer;
 * StageWebRemote::Sanitize runs on load and on every set, so a hand-edited ini cannot open a privileged port.
 */
USTRUCT()
struct FStageWebRemoteSettings
{
	GENERATED_BODY()

	UPROPERTY(Config)
	EStageWebRemoteScope Scope = EStageWebRemoteScope::Off;

	/** The TCP port the page and its WebSocket are served on, 1024–65535. */
	UPROPERTY(Config)
	int32 Port = 8080;

	bool operator==(const FStageWebRemoteSettings& Other) const
	{
		return Scope == Other.Scope && Port == Other.Port;
	}
};

/** What the Web Remote is doing, for the Preferences panel and the status command. Game thread. */
struct FStageWebRemoteStatus
{
	bool bListening = false;
	/** The pages to open on a phone ("http://192.168.1.20:8080"); empty while off. */
	TArray<FString> Addresses;
	/** The PIN a new device must enter; empty while off. Never logged. */
	FString Pin;
	/** Devices that paired since the server started (at most StageWebRemote::MaxPairedDevices). */
	int32 PairedDevices = 0;
	/** Open WebSockets of paired pages. */
	int32 ConnectedPages = 0;
	/** A device entered too many wrong PINs and waits until this time (FPlatformTime::Seconds); 0 when none is waiting. */
	double PairingLockedUntil = 0.0;

	/** Commands received from pages and HTTP clients. */
	int64 Received = 0;
	/** Commands the show carried out. */
	int64 Applied = 0;
	/** Commands the GameMode or the show refused. */
	int64 Refused = 0;
	/** Paths that matched no action, or a bad value. */
	int64 Ignored = 0;
	/** Commands over the per-frame limit. */
	int64 Dropped = 0;

	/** "/stagecraft/playback/1/level 0.50": the last command, for checking a page. */
	FString LastInput;
	/** Why the server could not open (port in use); empty when it opened as asked. Shown as a warning. */
	FText LastProblem;
	/** Why the last refused command was refused. Information, not a fault. */
	FText LastRefusal;
};

DECLARE_MULTICAST_DELEGATE(FOnStageWebRemoteStatusChanged);
