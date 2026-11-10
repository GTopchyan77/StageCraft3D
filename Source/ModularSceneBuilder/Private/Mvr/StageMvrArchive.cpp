// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrArchive.h"

#include "Fixtures/StageZipReader.h"
#include "HAL/PlatformTime.h"
#include "Mvr/StageMvrGeometry.h"

namespace StageMvrArchive
{
	FStageMvrArchive Read(TArray<uint8> FileBytes, const StageMvr::FLimits& Limits)
	{
		const double Start = FPlatformTime::Seconds();
		FStageMvrArchive Archive;
		Archive.ArchiveBytes = FileBytes.Num();
		Archive.Parsed = StageMvr::Parse(FileBytes, Limits);
		StageZip::FDirectory Directory;
		FString ZipError;
		if (Archive.Parsed.IsValid() && StageZip::ReadDirectory(FileBytes, Limits.Zip, Directory, ZipError))
		{
			const FStageMvrScene& Scene = Archive.Parsed.Scene;
			const auto Extract = [&](const FString& Name, TArray<uint8>& OutBytes, FString& OutError)
			{
				const StageZip::FEntry* Entry = Directory.Find(Name);
				if (!Entry)
				{
					OutError = TEXT("it is not in the file");
					return false;
				}
				if (!StageZip::Extract(FileBytes, *Entry, Limits.Zip, OutBytes, OutError))
				{
					return false;
				}
				Archive.LargestEntryBytes = FMath::Max(Archive.LargestEntryBytes, int64(OutBytes.Num()));
				return true;
			};

			for (const FString& Spec : Scene.GetGdtfSpecs())
			{
				TArray<uint8> Bytes;
				FString Error;
				if (Extract(Spec, Bytes, Error))
				{
					Archive.GdtfFiles.Add(Spec.ToLower(), UStageFixtureLibrarySubsystem::ParseEmbeddedFile(MoveTemp(Bytes), Spec));
				}
				// A missing file was reported by the parser; its fixtures fall back to the generic fixture.
			}

			// Only geometry a truss uses is read: the bounds size the run. Everything else stays compressed in the archive.
			TSet<FString> Files;
			for (const FStageMvrObject& Object : Scene.Objects)
			{
				if (Object.Kind != EStageMvrObjectKind::Truss)
				{
					continue;
				}
				for (const FString& File : Object.GeometryFiles)
				{
					Files.Add(File.ToLower());
				}
				for (const FGuid& Symbol : Object.Symbols)
				{
					if (const FStageMvrSymdef* Symdef = Scene.Symdefs.FindByPredicate([&Symbol](const FStageMvrSymdef& Entry) { return Entry.Id == Symbol; }))
					{
						for (const FString& File : Symdef->GeometryFiles)
						{
							Files.Add(File.ToLower());
						}
					}
				}
			}
			for (const FString& File : Files)
			{
				TArray<uint8> Bytes;
				FString Error;
				FBox Bounds(ForceInit);
				if (Extract(File, Bytes, Error) && StageMvrGeometry::ReadBounds(File, Bytes, Bounds, Error))
				{
					Archive.GeometryBounds.Add(File, Bounds);
				}
				else
				{
					Archive.GeometryProblems.Add(FString::Printf(TEXT("%s: %s"), *File, *Error));
				}
			}
		}
		else if (Archive.Parsed.IsValid())
		{
			Archive.Parsed.Errors.Add(ZipError);
		}
		FileBytes.Empty();
		Archive.ReadMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		return Archive;
	}
}
