#include "BF6Project.h"

#include "BF6Internal.h"        // LogBF6, Notify
#include "BF6Script.h"          // TemplateDir, ScaffoldProject
#include "BF6BuildMode.h"       // BF6Api: the open session, SaveCurrent
#include "BF6PortalWeb.h"       // SetSaveLink, ExperienceForSave, MapIdxForSave, BaseUrl
#include "BF6PortalProfile.h"   // the owned experience list and the file importer
#include "BF6PortalSettings.h"  // the mutators, teams and restrictions snapshot

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Misc/ConfigCacheIni.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_EDITOR
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#endif

DEFINE_LOG_CATEGORY(LogBF6Project);

namespace
{
	// The manifest's own version. Bumped when the shape changes in a way a
	// reader has to know about; a project written by an older tool keeps
	// loading and is upgraded in place.
	const int32 kManifestVersion = 1;

	// ---- small helpers ------------------------------------------------------

	FString SavesRoot()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("saves"));
	}

	FString OldLayoutPath(const FString& Level, const FString& Save)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), Level) / (Save + TEXT(".json"));
	}

	FString PortalCacheRoot()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("portal"), TEXT("experiences"));
	}

	TSharedPtr<FJsonObject> ReadJson(const FString& Path)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) return nullptr;
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(R, Root)) return nullptr;
		return Root;
	}

	bool WriteJson(const FString& Path, const TSharedPtr<FJsonObject>& Root)
	{
		if (!Root.IsValid()) return false;
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), W);
		IFileManager& FM = IFileManager::Get();
		FM.MakeDirectory(*FPaths::GetPath(Path), true);

		// THE PROJECT RECORD IS THE PROJECT.
		//
		// This wrote straight over the destination. A write that fails part way
		// leaves a truncated bf6project.json, and a project whose record will
		// not parse is a project the tool cannot open: the maps, saves, links
		// and artefact history all hang off it. Losing it costs far more than
		// the write that failed.
		//
		// Written beside it and swapped in, with the previous version put back
		// if the swap fails.
		const FString Temp = Path + TEXT(".bf6tmp");
		FM.Delete(*Temp, false, true, true);
		if (!FFileHelper::SaveStringToFile(Out, *Temp))
		{
			UE_LOG(LogBF6Project, Warning,
				TEXT("Could not write %s, so %s was left exactly as it was."), *Temp, *Path);
			return false;
		}

		// READ THE NEW FILE BACK BEFORE THE OLD ONE IS MOVED OUT OF THE WAY.
		//
		// SaveStringToFile returning true says the writes were accepted, not
		// that a whole document is on the disk: a full volume, a quota or a
		// process killed mid write all leave a short file behind and report
		// nothing. The next three lines move the only committed copy of the
		// project record aside on the strength of this one answer, so the
		// answer has to be about the bytes that are actually there.
		{
			FString Back;
			if (!FFileHelper::LoadFileToString(Back, *Temp) || !Back.Equals(Out, ESearchCase::CaseSensitive))
			{
				FM.Delete(*Temp, false, true, true);
				UE_LOG(LogBF6Project, Warning,
					TEXT("The new %s did not read back as a whole document, so it was discarded and the existing file was left alone. This usually means the disk is full."),
					*FPaths::GetCleanFilename(Path));
				return false;
			}
		}

		const FString Backup = Path + TEXT(".bf6prev");
		const bool bHad = FPaths::FileExists(Path);
		if (bHad)
		{
			FM.Delete(*Backup, false, true, true);
			if (!FM.Move(*Backup, *Path, true, true))
			{
				FM.Delete(*Temp, false, true, true);
				return false;   // the existing record is untouched
			}
		}
		if (!FM.Move(*Path, *Temp, true, true))
		{
			// THE COMMIT FAILED WITH THE DESTINATION ALREADY MOVED ASIDE.
			//
			// The put-back used to be attempted and its result thrown away, and
			// the new file deleted whatever happened. So a failed rollback left
			// NO file at Path at all, both remaining copies under names nothing
			// looks at, and this still returned a plain false. "The save failed"
			// and "your project record is gone and is sitting next to it under
			// another name" are not the same thing to tell somebody, and only
			// the second one needs them to do anything about it.
			if (!bHad)
			{
				UE_LOG(LogBF6Project, Warning,
					TEXT("Could not put the new %s in place. Nothing was there before, and the new content is at %s."),
					*FPaths::GetCleanFilename(Path), *Temp);
				return false;
			}
			if (FM.Move(*Path, *Backup, true, true))
			{
				FM.Delete(*Temp, false, true, true);
				UE_LOG(LogBF6Project, Warning,
					TEXT("Could not replace %s, so the previous version was put back. Nothing was lost."),
					*FPaths::GetCleanFilename(Path));
				return false;
			}
			// Neither move worked. Both documents are whole and both are kept:
			// deleting either one here would be deleting the only copy of it.
			UE_LOG(LogBF6Project, Error,
				TEXT("Could not replace %s and could not put the previous version back. Nothing was lost, but the file is not where it belongs: the previous version is at %s and the new one is at %s. Close anything holding that folder open and rename one of them to %s."),
				*FPaths::GetCleanFilename(Path), *Backup, *Temp, *Path);
			return false;
		}
		if (bHad) { FM.Delete(*Backup, false, true, true); }
		return true;
	}

	// A content hash per artefact is what makes two projects comparable without
	// reading either of them twice. MD5 because it is what the engine already
	// has as a one-call file hash, and this is a change detector rather than a
	// security claim.
	FString HashFile(const FString& Path)
	{
		const FMD5Hash H = FMD5Hash::HashFile(*Path);
		return H.IsValid() ? LexToString(H) : FString();
	}

	FString ToolVersion()
	{
		const TSharedPtr<IPlugin> P = IPluginManager::Get().FindPlugin(TEXT("BF6UnrealSDK"));
		return P.IsValid() ? P->GetDescriptor().VersionName : FString(TEXT("unknown"));
	}

	FString NowIso()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	// The uuid out of a Portal experience URL, or empty. The link the panel
	// records is ".../bf6/experience?id=<uuid>".
	FString IdFromUrl(const FString& Url)
	{
		const int32 At = Url.Find(TEXT("id="));
		if (At == INDEX_NONE) return FString();
		FString Id = Url.Mid(At + 3);
		int32 Amp;
		if (Id.FindChar(TEXT('&'), Amp)) Id.LeftInline(Amp);
		return Id.Len() >= 32 ? Id : FString();
	}

	FString UrlForId(const FString& Id)
	{
		return BF6PortalWeb::BaseUrl() + TEXT("/bf6/experience?id=") + Id;
	}

	// ---- the folders the tool owns ------------------------------------------
	//
	// One line each, so the readme and the manifest and the creator all use the
	// same words for the same folder.
	struct FOurFolder { const TCHAR* Rel; const TCHAR* What; };
	const FOurFolder kOurFolders[] =
	{
		{ TEXT("maps"),             TEXT("one folder per map of this experience: its Unreal save, its uploadable spatial and the Godot scene it came from") },
		{ TEXT("spatials"),         TEXT("the uploadable <map>.spatial.json for each map in this project") },
		{ TEXT("unreal"),           TEXT("everything the Unreal tool owns that the scripting template has no place for") },
		{ TEXT("unreal/tscn"),      TEXT("the Godot .tscn of each map, for anyone who wants to open it in the official SDK") },
		{ TEXT("unreal/blockly"),   TEXT("workspace.json, the block rules exactly as the site stores them") },
		{ TEXT("unreal/ui"),        TEXT("UI builder designs, one json per design") },
		{ TEXT("unreal/bindings"),  TEXT("connections between placed objects, block rules, script helpers and UI designs") },
		{ TEXT("unreal/settings"),  TEXT("settings.json: the mutators, the team composition and the asset restrictions") },
		{ TEXT("dist"),             TEXT("build output. npm run build writes bundle.ts and bundle.strings.json here") },
	};

	// ---- artefact discovery -------------------------------------------------

	struct FArtefact
	{
		FString Rel;      // path inside the project, forward slashes
		FString Kind;     // session, spatial, tscn, workspace, ui, settings, rotation, thumbnail, script, strings, bundle
		FString Level;    // when the artefact belongs to one map
	};

	void FindIn(const FString& Dir, const FString& SubRel, const TCHAR* Pattern,
		const TCHAR* Kind, TArray<FArtefact>& Out)
	{
		TArray<FString> Files;
		const FString Full = SubRel.IsEmpty() ? Dir : FPaths::Combine(Dir, SubRel);
		IFileManager::Get().FindFiles(Files, *(Full / Pattern), true, false);
		for (const FString& F : Files)
		{
			FArtefact A;
			A.Rel = SubRel.IsEmpty() ? F : (SubRel + TEXT("/") + F);
			A.Kind = Kind;
			Out.Add(A);
		}
	}

	// Session files are <Level>.json at the project root. Every Battlefield 6
	// level codename starts with MP_, which is what tells them apart from the
	// template's own package.json and tsconfig.json sitting beside them.
	bool IsLevelStem(const FString& Stem)
	{
		return Stem.StartsWith(TEXT("MP_"), ESearchCase::IgnoreCase);
	}

	TArray<FArtefact> GatherArtefacts(const FString& Dir, TArray<FString>& OutLevels)
	{
		TArray<FArtefact> Out;

		TArray<FString> RootJson;
		IFileManager::Get().FindFiles(RootJson, *(Dir / TEXT("*.json")), true, false);
		for (const FString& F : RootJson)
		{
			const FString Stem = FPaths::GetBaseFilename(F);
			if (!IsLevelStem(Stem)) continue;
			FArtefact A; A.Rel = F; A.Kind = TEXT("session"); A.Level = Stem;
			Out.Add(A);
			OutLevels.AddUnique(Stem);
		}

		// ---- an experience's maps ----
		// maps/<Level>/ holds everything that belongs to ONE map: the Unreal
		// save, the uploadable spatial and the Godot scene it was authored
		// from. Everything above it - the script, the settings, the workspace,
		// the thumbnail - is shared by all of them and is found below.
		{
			TArray<FString> MapDirs;
			IFileManager::Get().FindFiles(MapDirs, *(Dir / TEXT("maps") / TEXT("*")), false, true);
			MapDirs.Sort();
			for (const FString& L : MapDirs)
			{
				const FString Sub = FString(TEXT("maps/")) + L;
				auto AddIf = [&](const FString& Leaf, const TCHAR* Kind)
				{
					if (!FPaths::FileExists(FPaths::Combine(Dir, Sub, Leaf))) return;
					FArtefact A; A.Rel = Sub + TEXT("/") + Leaf; A.Kind = Kind; A.Level = L;
					Out.Add(A);
				};
				AddIf(L + TEXT(".json"),         TEXT("session"));
				AddIf(L + TEXT(".spatial.json"), TEXT("spatial"));
				AddIf(L + TEXT(".tscn"),         TEXT("tscn"));
				if (FPaths::FileExists(FPaths::Combine(Dir, Sub, L + TEXT(".json")))) OutLevels.AddUnique(L);
			}
		}

		FindIn(Dir, TEXT("spatials"), TEXT("*.json"), TEXT("spatial"), Out);
		FindIn(Dir, TEXT("unreal/tscn"), TEXT("*.tscn"), TEXT("tscn"), Out);
		FindIn(Dir, TEXT("unreal/ui"), TEXT("*.json"), TEXT("ui"), Out);
		FindIn(Dir, TEXT("unreal/bindings"), TEXT("*.json"), TEXT("binding"), Out);

		auto One = [&](const TCHAR* Rel, const TCHAR* Kind)
		{
			if (FPaths::FileExists(FPaths::Combine(Dir, Rel)))
			{
				FArtefact A; A.Rel = Rel; A.Kind = Kind; Out.Add(A);
			}
		};
		One(TEXT("unreal/blockly/workspace.json"), TEXT("workspace"));
		One(TEXT("unreal/settings/settings.json"), TEXT("settings"));
		One(TEXT("unreal/rotation.json"),          TEXT("rotation"));
		One(TEXT("src/index.ts"),                  TEXT("script"));
		One(TEXT("src/strings.json"),              TEXT("strings"));
		One(TEXT("src/thumbnail.png"),             TEXT("thumbnail"));
		One(TEXT("src/thumbnail.jpg"),             TEXT("thumbnail"));
		One(TEXT("dist/bundle.ts"),                TEXT("bundle"));

		// A spatial is named after its map, so say which map it is rather than
		// leaving the reader to guess from the filename.
		for (FArtefact& A : Out)
		{
			if (A.Kind != TEXT("spatial") && A.Kind != TEXT("tscn")) continue;
			for (const FString& L : OutLevels)
			{
				const FString Leaf = FPaths::GetCleanFilename(A.Rel);
				if (Leaf.StartsWith(L, ESearchCase::IgnoreCase)) { A.Level = L; break; }
			}
		}
		return Out;
	}

	// ---- CANONICAL HASHING --------------------------------------------------
	//
	// RAW BYTES ARE THE WRONG UNIT. A minified spatial and the same spatial
	// unminified mean exactly the same thing and share not one byte; key order
	// out of two different JSON writers differs; a float printed to six places
	// and to four is the same object. So what is hashed is a NORMALISED form:
	//
	//   sorted keys, so writer order cannot matter;
	//   numbers to four decimals, so a re-serialise cannot matter;
	//   ObjIds kept, because they are the identity the site and the tool share;
	//   minified names dropped, because "a" and "DeployCam_01" are the same
	//     object and the site renames every one of them on upload;
	//   the shipped base objects and the Static layer excluded, so a save that
	//     respawns the base setup does not read as different from a file that
	//     carries it.
	//
	// AND NEVER A TIMESTAMP. The site's version fields are strings like "123"
	// and two machines do not agree on the clock, so nothing here compares one.

	void CanonValue(const TSharedPtr<FJsonValue>& V, FString& Out, int32 Depth);

	void CanonObject(const TSharedPtr<FJsonObject>& O, FString& Out, int32 Depth)
	{
		if (!O.IsValid()) { Out += TEXT("{}"); return; }
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Sorted;
		for (const auto& It : O->Values) Sorted.Add(TPair<FString, TSharedPtr<FJsonValue>>(FString(*It.Key), It.Value));
		Sorted.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A, const TPair<FString, TSharedPtr<FJsonValue>>& B) { return A.Key < B.Key; });
		Out += TEXT("{");
		for (int32 i = 0; i < Sorted.Num(); ++i)
		{
			if (i) Out += TEXT(",");
			Out += Sorted[i].Key;
			Out += TEXT(":");
			CanonValue(Sorted[i].Value, Out, Depth + 1);
		}
		Out += TEXT("}");
	}

	void CanonValue(const TSharedPtr<FJsonValue>& V, FString& Out, int32 Depth)
	{
		if (!V.IsValid() || Depth > 40) { Out += TEXT("null"); return; }
		switch (V->Type)
		{
		case EJson::Object: CanonObject(V->AsObject(), Out, Depth); break;
		case EJson::Array:
			Out += TEXT("[");
			{
				const TArray<TSharedPtr<FJsonValue>>& A = V->AsArray();
				for (int32 i = 0; i < A.Num(); ++i) { if (i) Out += TEXT(","); CanonValue(A[i], Out, Depth + 1); }
			}
			Out += TEXT("]");
			break;
		case EJson::Number: Out += FString::Printf(TEXT("%.4f"), V->AsNumber()); break;
		case EJson::Boolean: Out += V->AsBool() ? TEXT("true") : TEXT("false"); break;
		case EJson::String: Out += V->AsString(); break;
		default: Out += TEXT("null"); break;
		}
	}

	// The map's SHIPPED furniture, which both sides have and neither side
	// authored. Excluding it is what stops a save that respawns the base setup
	// from reading as different to a file that carries it.
	bool IsShippedType(const FString& Type)
	{
		return Type == TEXT("DeployCam") || Type == TEXT("CombatArea") || Type.Contains(TEXT("HQ"));
	}

	// One .spatial.json, normalised. Returns the canonical text.
	FString CanonSpatialText(const TSharedPtr<FJsonObject>& Root)
	{
		if (!Root.IsValid()) return FString();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("Portal_Dynamic"), Arr) || !Arr) return FString();

		// Every entry id in the file, so a property whose VALUE is one of them
		// can be recognised as a link and dropped: link targets are minified
		// names and carry no meaning across a round trip.
		TSet<FString> AllIds;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			FString Id;
			if (O.IsValid() && O->TryGetStringField(TEXT("id"), Id)) AllIds.Add(Id);
		}

		// The shipped entries, and everything they own.
		TSet<FString> Drop;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) continue;
			FString Type, Id;
			O->TryGetStringField(TEXT("type"), Type);
			O->TryGetStringField(TEXT("id"), Id);
			if (!IsShippedType(Type)) continue;
			if (!Id.IsEmpty()) Drop.Add(Id);
			for (const auto& It : O->Values)
			{
				if (!It.Value.IsValid() || It.Value->Type != EJson::String) continue;
				const FString S = It.Value->AsString();
				if (AllIds.Contains(S)) Drop.Add(S);
			}
		}

		TArray<FString> Lines;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) continue;
			FString Type, Id;
			O->TryGetStringField(TEXT("type"), Type);
			O->TryGetStringField(TEXT("id"), Id);
			if (IsShippedType(Type)) continue;
			if (!Id.IsEmpty() && Drop.Contains(Id)) continue;

			TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
			for (const auto& It : O->Values)
			{
				if (It.Key == TEXT("name") || It.Key == TEXT("id") || It.Key == TEXT("linked")) continue;
				if (It.Value.IsValid() && It.Value->Type == EJson::String && AllIds.Contains(It.Value->AsString())) continue;
				C->SetField(It.Key, It.Value);
			}
			FString Line;
			CanonObject(C, Line, 0);
			Lines.Add(Line);
		}
		// Order cannot be part of the meaning: the site writes the same objects
		// in whatever order its own export happened to walk them.
		Lines.Sort();
		return FString::Join(Lines, TEXT("\n"));
	}

	// A session save, normalised. Only the PLACED objects: the base array is
	// the shipped setup, which is the same exclusion the spatial gets.
	FString CanonSessionText(const TSharedPtr<FJsonObject>& Root)
	{
		if (!Root.IsValid()) return FString();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("objects"), Arr) || !Arr) return FString();
		TArray<FString> Lines;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FString Line;
			CanonValue(V, Line, 0);
			Lines.Add(Line);
		}
		Lines.Sort();
		return FString::Join(Lines, TEXT("\n"));
	}

	// A Blockly workspace, normalised: the canvas coordinates of a block are
	// where the user dragged it, not what the rule does.
	void StripLayout(const TSharedPtr<FJsonValue>& V, int32 Depth)
	{
		if (!V.IsValid() || Depth > 40) return;
		if (V->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) return;
			O->RemoveField(TEXT("x"));
			O->RemoveField(TEXT("y"));
			for (const auto& It : O->Values) StripLayout(It.Value, Depth + 1);
			return;
		}
		if (V->Type == EJson::Array)
			for (const TSharedPtr<FJsonValue>& E : V->AsArray()) StripLayout(E, Depth + 1);
	}

	FString CanonicalText(const FString& Path, const FString& Kind)
	{
		if (Kind == TEXT("spatial"))  return CanonSpatialText(ReadJson(Path));
		if (Kind == TEXT("session"))  return CanonSessionText(ReadJson(Path));
		if (Kind == TEXT("workspace"))
		{
			TSharedPtr<FJsonObject> R = ReadJson(Path);
			if (!R.IsValid()) return FString();
			const TSharedPtr<FJsonValue> V = MakeShared<FJsonValueObject>(R);
			StripLayout(V, 0);
			FString Out; CanonValue(V, Out, 0); return Out;
		}
		if (Kind == TEXT("settings") || Kind == TEXT("strings") || Kind == TEXT("ui") || Kind == TEXT("rotation"))
		{
			TSharedPtr<FJsonObject> R = ReadJson(Path);
			if (!R.IsValid()) return FString();
			FString Out; CanonObject(R, Out, 0); return Out;
		}
		// Everything else (a .ts source, a thumbnail) is compared as its own
		// text or bytes, with line endings and trailing space taken out so a
		// checkout on another machine is not a change.
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) return FString();
		Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		return Text.TrimEnd();
	}

	FString CanonicalHash(const FString& Path, const FString& Kind)
	{
		const FString T = CanonicalText(Path, Kind);
		return T.IsEmpty() ? FString() : FMD5::HashAnsiString(*T);
	}

	// ---- the diff, keyed on ObjId ------------------------------------------
	//
	// NEVER ON LABELS. The site minifies every name on upload, so "a" here and
	// "a" there are not the same object and "DeployCam_03" does not survive at
	// all. ObjId is the only identity both sides keep.
	struct FSpatialEntry
	{
		FString Type;
		FVector Pos = FVector::ZeroVector;
		FString Props;   // the canonical form of everything else
	};

	void ReadSpatialEntries(const TSharedPtr<FJsonObject>& Root, TMap<int32, FSpatialEntry>& Out, int32& Untracked)
	{
		Untracked = 0;
		if (!Root.IsValid()) return;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("Portal_Dynamic"), Arr) || !Arr) return;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) continue;
			FString Type;
			O->TryGetStringField(TEXT("type"), Type);
			if (IsShippedType(Type)) continue;
			double D = -1.0;
			if (!O->TryGetNumberField(TEXT("ObjId"), D) || (int32)D < 0) { ++Untracked; continue; }
			FSpatialEntry E;
			E.Type = Type;
			const TSharedPtr<FJsonObject>* P = nullptr;
			if (O->TryGetObjectField(TEXT("position"), P) && P)
				E.Pos = FVector((*P)->GetNumberField(TEXT("x")), (*P)->GetNumberField(TEXT("y")), (*P)->GetNumberField(TEXT("z")));
			TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
			for (const auto& It : O->Values)
			{
				if (It.Key == TEXT("name") || It.Key == TEXT("id") || It.Key == TEXT("linked")
					|| It.Key == TEXT("position") || It.Key == TEXT("ObjId")) continue;
				C->SetField(It.Key, It.Value);
			}
			CanonObject(C, E.Props, 0);
			Out.Add((int32)D, MoveTemp(E));
		}
	}

	// ---- project backups ----------------------------------------------------

	FString BackupRoot(const FString& Dir) { return FPaths::Combine(Dir, TEXT(".backups")); }

	// ---- WHAT A SNAPSHOT OWNS -----------------------------------------------
	//
	// This used to be four folders and two files: spatials, unreal/blockly,
	// unreal/settings, src, project.json and unreal/rotation.json. Everything
	// else a person had authored was outside the only protection the project
	// had - the maps themselves, the UI designs, the Godot scenes, and the
	// package.json and lockfile without which none of the restored script
	// sources build. "Restore the snapshot" handed back a project with no maps
	// in it and reported that as an ordinary success.
	//
	// The old exclusion had a reason, and it was wrong. The autosave does keep
	// a rolling copy of the session file, but per LEVEL, outside the project,
	// not keyed to the project and not carried when the project is moved,
	// zipped or synced. A snapshot that cannot put the map back is not a backup
	// of the project. So the map is in here now and the extra disk is the
	// price; the fingerprint below still refuses to spend a snapshot when
	// nothing in the owned set moved, and ProjectBackupMax still bounds how
	// many are kept.
	//
	// NOT in here, deliberately: dist (the bundler rewrites it), node_modules
	// (npm install rebuilds it, and it is enormous), .ai (npm run refresh-ai
	// regenerates it) and .backups itself.
	const TCHAR* kSnapshotDirs[] =
	{
		TEXT("maps"),      // maps/<Level>/: the session save, its spatial and its scene
		TEXT("spatials"),  // the uploadable spatial for each map
		TEXT("unreal"),    // blockly, ui, settings, tscn and rotation.json
		TEXT("src"),       // the script sources, the strings and the thumbnail
		TEXT("scripts"),   // the template's build scripts: without them src does not build
	};
	// The template's own configuration, at the project root. A project whose
	// package.json and lockfile did not come back does not build on the machine
	// it is restored onto, which makes the restored sources useless.
	const TCHAR* kSnapshotFiles[] =
	{
		TEXT("project.json"),
		TEXT("package.json"), TEXT("package-lock.json"),
		TEXT("tsconfig.json"), TEXT("eslint.config.mjs"), TEXT(".prettierrc.json"),
		TEXT("README.md"), TEXT("TEMPLATE.md"),
		// The author's own authoring choices. These are small, they are not
		// recoverable from anything else, and leaving them out meant a restore
		// silently dropped the quick-value list somebody had curated and their
		// publish preference. They belong to the project as much as its code.
		TEXT("bf6-quick-values.json"),
		TEXT("bf6-publish-mode.txt"),
		// Which files this tool generated, and whether the source has been
		// adopted. Without these a restored project cannot tell its own
		// generated files from handwritten ones, and the import safety net
		// starts guessing.
		TEXT(".bf6-generated.json"),
		TEXT(".bf6-source-adopted"),
	};

	// The session saves that sit at the project ROOT rather than under maps/.
	// A standalone save keeps its <Level>.json there, and IsLevelStem is what
	// tells those apart from the package.json and tsconfig.json beside them.
	void FindRootSessions(const FString& Dir, TArray<FString>& Out)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), true, false);
		for (const FString& F : Files)
			if (IsLevelStem(FPaths::GetBaseFilename(F))) Out.AddUnique(F);
	}

	// EVERY OWNED FILE, RELATIVE TO THE PROJECT, forward slashes, sorted.
	//
	// This one list is the ownership boundary and both halves depend on it
	// meaning the same thing: a snapshot copies exactly this set, and a restore
	// may only ever DELETE inside it. Anything not in it - node_modules, dist,
	// .git, .backups, a folder of reference screenshots the user dropped in -
	// is not ours and is left alone by both.
	TArray<FString> OwnedFiles(const FString& Dir)
	{
		TArray<FString> Out;
		IFileManager& FM = IFileManager::Get();
		for (const TCHAR* Sub : kSnapshotDirs)
		{
			const FString Full = FPaths::Combine(Dir, Sub);
			if (!FPaths::DirectoryExists(Full)) continue;
			TArray<FString> Files;
			FM.FindFilesRecursive(Files, *Full, TEXT("*"), true, false);
			for (const FString& F : Files)
			{
				FString Rel = F;
				FPaths::MakePathRelativeTo(Rel, *(Dir / TEXT("")));
				Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
				Out.AddUnique(Rel);
			}
		}
		for (const TCHAR* F : kSnapshotFiles)
			if (FPaths::FileExists(FPaths::Combine(Dir, F))) Out.AddUnique(FString(F));
		TArray<FString> Sessions;
		FindRootSessions(Dir, Sessions);
		for (const FString& F : Sessions) Out.AddUnique(F);
		Out.Sort();
		return Out;
	}

	// The template ships eighteen sample spatials, one per shipped map. They are
	// worth keeping as reference and they are NOT this project's map data, so
	// they go one folder down: spatials/ at the top level means "what this
	// project exports and uploads", and nothing else may sit in it or the
	// manifest counts somebody else's files as ours.
	int32 ParkTemplateSpatials(const FString& Dir, const FString& Template)
	{
		if (Template.IsEmpty()) return 0;
		IFileManager& FM = IFileManager::Get();
		const FString Top = FPaths::Combine(Dir, TEXT("spatials"));
		const FString Down = FPaths::Combine(Top, TEXT("template-samples"));
		TArray<FString> Files;
		FM.FindFiles(Files, *(Top / TEXT("*.json")), true, false);
		int32 N = 0;
		for (const FString& F : Files)
		{
			// Only what the template brought. A spatial this project exported
			// itself has the same extension and must not be swept up with it.
			if (!FPaths::FileExists(FPaths::Combine(Template, TEXT("spatials"), F))) continue;
			FM.MakeDirectory(*Down, true);
			const FString Src = FPaths::Combine(Top, F);
			const FString Dst = FPaths::Combine(Down, F);
			if (FPaths::FileExists(Dst)) FM.Delete(*Dst);
			if (FM.Move(*Dst, *Src)) ++N;
		}
		return N;
	}

	// ONE COPY, ONE ANSWER PER FILE.
	//
	// The old CopyTree returned a count. A count cannot say WHICH files did not
	// arrive, so a snapshot that copied nine of twelve reported nine and a
	// restore from it reported nine, and both read as ordinary success. Every
	// caller here has to be able to tell a complete copy from an incomplete
	// one, because that is the difference between a backup it may delete from
	// and one it may not.
	struct FCopySet
	{
		TArray<FString> Copied;    // relative paths that arrived and hashed back
		TArray<FString> Failed;    // relative paths that did not
		TMap<FString, FString> Md5;
		int64 Bytes = 0;
		bool Complete() const { return Failed.Num() == 0; }
	};

	// Copies a named set of relative paths, verifying each arrival by hash.
	// A copy call that returns COPY_OK and a file that is actually there and
	// whole are different claims, and only the second one is worth anything to
	// a restore.
	void CopyOwned(const FString& From, const FString& To, const TArray<FString>& Rel, FCopySet& Out)
	{
		IFileManager& FM = IFileManager::Get();
		for (const FString& R : Rel)
		{
			const FString Src = FPaths::Combine(From, R);
			const FString Dst = FPaths::Combine(To, R);
			if (!FPaths::FileExists(Src)) { Out.Failed.Add(R); continue; }
			FM.MakeDirectory(*FPaths::GetPath(Dst), true);
			if (FM.Copy(*Dst, *Src, true, true) != COPY_OK) { Out.Failed.Add(R); continue; }
			const FString SrcHash = HashFile(Src);
			const FString DstHash = HashFile(Dst);
			if (SrcHash.IsEmpty() || SrcHash != DstHash) { Out.Failed.Add(R); continue; }
			Out.Copied.Add(R);
			Out.Md5.Add(R, DstHash);
			Out.Bytes += FM.FileSize(*Dst);
		}
	}
}

namespace
{
	// ---- the readme ---------------------------------------------------------

	FString ReadmeText(const FString& Save, const TArray<FString>& Levels)
	{
		FString Maps;
		for (const FString& L : Levels) { if (!Maps.IsEmpty()) Maps += TEXT(", "); Maps += L; }
		if (Maps.IsEmpty()) Maps = TEXT("none yet");

		FString T;
		T += FString::Printf(TEXT("# %s\n\n"), *Save);
		T += TEXT("This folder is one Portal project. It holds the map you built in the BF6 Unreal SDK\n");
		T += TEXT("and a complete Battlefield 6 Portal scripting project around it, so the whole thing\n");
		T += TEXT("can be zipped, shared, put in git, or opened in VS Code and worked on there.\n\n");
		T += FString::Printf(TEXT("Maps in this project: %s\n\n"), *Maps);
		T += TEXT("## What the tool owns\n\n");
		T += TEXT("- `<map>.json` at the root is the session save. It is what the tool reopens. It is\n");
		T += TEXT("  NOT the file you upload to Portal.\n");
		for (const FOurFolder& F : kOurFolders)
			T += FString::Printf(TEXT("- `%s/` %s\n"), F.Rel, F.What);
		T += TEXT("- `project.json` is the manifest: the tool version, the maps, the experience this\n");
		T += TEXT("  project is linked to, and a content hash for every file above. It is what lets\n");
		T += TEXT("  the tool compare this project against the experiences on your Portal account.\n\n");
		T += TEXT("## What comes from the scripting template\n\n");
		T += TEXT("The scripting half is Michael De Luca's bf6-portal-scripting-template (MIT), copied\n");
		T += TEXT("here and set up for you. `package.json`, `tsconfig.json`, `eslint.config.mjs`,\n");
		T += TEXT("`.prettierrc.json`, `src/`, `scripts/` and `.ai/` are all his, unchanged apart from\n");
		T += TEXT("the name and description in package.json. `TEMPLATE.md` is his own readme, which is\n");
		T += TEXT("the reference for everything in `src/`.\n\n");
		T += TEXT("Useful commands, once you have run `npm install` in this folder:\n\n");
		T += TEXT("- `npm run build` bundles `src/index.ts` into `dist/`\n");
		T += TEXT("- `npm run lint` and `npm run prettier` keep the source tidy\n");
		T += TEXT("- `npm run export-thumbnail` makes a Portal-sized thumbnail from `src/thumbnail.png`\n");
		T += TEXT("- `npm run minify-spatials` shrinks everything in `spatials/` into `dist/spatials/`\n");
		T += TEXT("- `npm run refresh-ai` regenerates `.ai/bf6-portal-utils-knowledge.md`\n");
		T += TEXT("- `npm run update` pulls newer template scripts and dependencies\n\n");
		T += TEXT("## What is deliberately not here\n\n");
		T += TEXT("The template's `scripts/deploy.js` and its deploy commands are removed. They work\n");
		T += TEXT("from an EA session id, and this tool never touches a credential. Publishing is you\n");
		T += TEXT("pressing Save on the real Portal site, with your own sign in.\n");
		return T;
	}

	// ---- manifest -----------------------------------------------------------

	FString ManifestPath(const FString& Dir) { return FPaths::Combine(Dir, TEXT("project.json")); }

	// Rebuild project.json from what is actually on disk. Anything the tool
	// cannot know is listed under "missing" rather than filled in with a guess.
	bool WriteManifest(const FString& Dir, const FString& Save, const FString& Level)
	{
		const FString Path = ManifestPath(Dir);
		const TSharedPtr<FJsonObject> Was = ReadJson(Path);

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("_note"), TEXT("BF6 Unreal SDK project manifest. Rewritten on every save; edit the files it describes, not this."));
		Root->SetStringField(TEXT("manifest"), TEXT("bf6-unreal-project"));
		Root->SetNumberField(TEXT("manifestVersion"), kManifestVersion);
		Root->SetStringField(TEXT("save"), Save);

		TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("BF6 Unreal SDK"));
		Tool->SetStringField(TEXT("version"), ToolVersion());
		Root->SetObjectField(TEXT("tool"), Tool);

		TSharedPtr<FJsonObject> Tpl = MakeShared<FJsonObject>();
		Tpl->SetStringField(TEXT("name"), TEXT("bf6-portal-scripting-template"));
		Tpl->SetStringField(TEXT("author"), TEXT("Michael De Luca"));
		Tpl->SetStringField(TEXT("license"), TEXT("MIT"));
		{
			const TSharedPtr<FJsonObject> Pkg = ReadJson(FPaths::Combine(Dir, TEXT("package.json")));
			FString V;
			if (Pkg.IsValid() && Pkg->TryGetStringField(TEXT("templateVersion"), V)) Tpl->SetStringField(TEXT("version"), V);
		}
		Root->SetObjectField(TEXT("template"), Tpl);

		TArray<FString> Levels;
		const TArray<FArtefact> Arts = GatherArtefacts(Dir, Levels);
		if (!Level.IsEmpty()) Levels.AddUnique(Level);

		TArray<TSharedPtr<FJsonValue>> LArr;
		for (const FString& L : Levels) LArr.Add(MakeShared<FJsonValueString>(L));
		Root->SetArrayField(TEXT("levels"), LArr);
		if (!Level.IsEmpty()) Root->SetStringField(TEXT("primaryLevel"), Level);

		// ---- the experience link, if there is one ----
		// Two sources agree or the field is not written: the panel's own record
		// for this save, and whatever a previous manifest carried.
		{
			FString Url, Id;
			for (const FString& L : Levels)
			{
				Url = BF6PortalWeb::ExperienceForSave(L, Save);
				if (!Url.IsEmpty()) break;
			}
			Id = IdFromUrl(Url);
			if (Id.IsEmpty() && Was.IsValid())
			{
				const TSharedPtr<FJsonObject>* Prev = nullptr;
				if (Was->TryGetObjectField(TEXT("experience"), Prev) && Prev)
					(*Prev)->TryGetStringField(TEXT("id"), Id);
			}
			if (!Id.IsEmpty())
			{
				TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
				E->SetStringField(TEXT("id"), Id);
				E->SetStringField(TEXT("url"), Url.IsEmpty() ? UrlForId(Id) : Url);
				for (const FString& L : Levels)
				{
					const int32 Idx = BF6PortalWeb::MapIdxForSave(L, Save);
					if (Idx >= 0) { E->SetNumberField(TEXT("mapIdx"), Idx); break; }
				}
				// The revision id (the play element nested inside a fetched
				// experience) is not something this file can read on its own, so
				// it is carried forward when a previous manifest had it and left
				// out otherwise.
				if (Was.IsValid())
				{
					const TSharedPtr<FJsonObject>* Prev = nullptr;
					FString Rev, LinkedAt, LinkedBy;
					if (Was->TryGetObjectField(TEXT("experience"), Prev) && Prev)
					{
						if ((*Prev)->TryGetStringField(TEXT("revision"), Rev) && !Rev.IsEmpty()) E->SetStringField(TEXT("revision"), Rev);
						if ((*Prev)->TryGetStringField(TEXT("linkedAt"), LinkedAt)) E->SetStringField(TEXT("linkedAt"), LinkedAt);
						if ((*Prev)->TryGetStringField(TEXT("linkedBy"), LinkedBy)) E->SetStringField(TEXT("linkedBy"), LinkedBy);
					}
				}
				const TSharedPtr<FJsonObject> Cache = ReadJson(FPaths::Combine(PortalCacheRoot(), Id, TEXT("experience.json")));
				FString Nm;
				if (Cache.IsValid() && Cache->TryGetStringField(TEXT("name"), Nm)) E->SetStringField(TEXT("name"), Nm);
				Root->SetObjectField(TEXT("experience"), E);
			}
		}

		// ---- what this project came from, and its Godot scene ----
		//
		// Neither is anything disk can answer, so both are carried forward
		// whole. Losing them on a rebuild would ask the user a question they
		// have already answered, which is the one thing the record exists to
		// prevent.
		if (Was.IsValid())
		{
			const TSharedPtr<FJsonObject>* Prev = nullptr;
			if (Was->TryGetObjectField(TEXT("origin"), Prev) && Prev) Root->SetObjectField(TEXT("origin"), *Prev);
			Prev = nullptr;
			if (Was->TryGetObjectField(TEXT("tscn"), Prev) && Prev) Root->SetObjectField(TEXT("tscn"), *Prev);
		}

		// ---- the artefacts, with a hash each, and their own sync state ----
		//
		// The sync block and the origin stamp are CARRIED FORWARD from the
		// previous manifest: this function rebuilds the file list off disk, and
		// what the two sides last agreed on is not something disk can tell it.
		// The canonical hash is recomputed only when the raw bytes moved, so a
		// save does not re-normalise eleven multi megabyte spatials to learn
		// that none of them changed.
		TMap<FString, TSharedPtr<FJsonObject>> Prev;
		if (Was.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PArr = nullptr;
			if (Was->TryGetArrayField(TEXT("artefacts"), PArr) && PArr)
				for (const TSharedPtr<FJsonValue>& V : *PArr)
				{
					const TSharedPtr<FJsonObject> O = V->AsObject();
					FString P;
					if (O.IsValid() && O->TryGetStringField(TEXT("path"), P)) Prev.Add(P, O);
				}
		}

		TArray<TSharedPtr<FJsonValue>> AArr;
		TSet<FString> Kinds;
		for (const FArtefact& A : Arts)
		{
			const FString Full = FPaths::Combine(Dir, A.Rel);
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("path"), A.Rel);
			O->SetStringField(TEXT("kind"), A.Kind);
			if (!A.Level.IsEmpty()) O->SetStringField(TEXT("level"), A.Level);
			O->SetNumberField(TEXT("bytes"), (double)IFileManager::Get().FileSize(*Full));
			const FString H = HashFile(Full);
			if (!H.IsEmpty()) O->SetStringField(TEXT("md5"), H);
			O->SetStringField(TEXT("modified"), IFileManager::Get().GetTimeStamp(*Full).ToIso8601());

			const TSharedPtr<FJsonObject>* Old = Prev.Find(A.Rel);
			FString OldMd5, OldCanon;
			if (Old) { (*Old)->TryGetStringField(TEXT("md5"), OldMd5); (*Old)->TryGetStringField(TEXT("canonical"), OldCanon); }
			const FString Canon = (!OldCanon.IsEmpty() && OldMd5 == H) ? OldCanon : CanonicalHash(Full, A.Kind);
			if (!Canon.IsEmpty()) O->SetStringField(TEXT("canonical"), Canon);

			if (Old)
			{
				const TSharedPtr<FJsonObject>* S = nullptr;
				if ((*Old)->TryGetObjectField(TEXT("sync"), S) && S) O->SetObjectField(TEXT("sync"), *S);
				const TSharedPtr<FJsonObject>* Og = nullptr;
				if ((*Old)->TryGetObjectField(TEXT("origin"), Og) && Og) O->SetObjectField(TEXT("origin"), *Og);
			}
			AArr.Add(MakeShared<FJsonValueObject>(O));
			Kinds.Add(A.Kind);
		}
		Root->SetArrayField(TEXT("artefacts"), AArr);

		// ---- what is honestly not here ----
		TArray<TSharedPtr<FJsonValue>> MArr;
		auto Miss = [&](const TCHAR* Kind, const TCHAR* Why)
		{
			if (Kinds.Contains(Kind)) return;
			MArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %s"), Kind, Why)));
		};
		Miss(TEXT("session"),   TEXT("no map has been saved into this project yet"));
		Miss(TEXT("spatial"),   TEXT("run EXPORT in the tool to write the uploadable file"));
		Miss(TEXT("tscn"),      TEXT("run SAVE AS GODOT SCENE in the tool"));
		Miss(TEXT("workspace"), TEXT("no block workspace has been captured for this project"));
		Miss(TEXT("settings"),  TEXT("no mutators, teams or restrictions have been read from the site"));
		Miss(TEXT("rotation"),  TEXT("this project is not linked to an experience with a map rotation"));
		Miss(TEXT("thumbnail"), TEXT("put a thumbnail at src/thumbnail.png"));
		Miss(TEXT("script"),    TEXT("the scripting template has not been copied in yet"));
		Miss(TEXT("bundle"),    TEXT("run npm run build in this folder"));
		if (!FPaths::FileExists(FPaths::Combine(Dir, TEXT("package.json"))))
			MArr.Add(MakeShared<FJsonValueString>(TEXT("template: the Portal scripting template is not installed where the tool can find it")));
		if (!FPaths::DirectoryExists(FPaths::Combine(Dir, TEXT("node_modules"))))
			MArr.Add(MakeShared<FJsonValueString>(TEXT("node_modules: run npm install in this folder, or press INSTALL in the script editor")));
		Root->SetArrayField(TEXT("missing"), MArr);

		FString Created;
		if (Was.IsValid()) Was->TryGetStringField(TEXT("created"), Created);
		Root->SetStringField(TEXT("created"), Created.IsEmpty() ? NowIso() : Created);
		Root->SetStringField(TEXT("modified"), NowIso());

		return WriteJson(Path, Root);
	}

	// ---- the background scaffold -------------------------------------------
	//
	// WHY OFF THE GAME THREAD. The template is a few hundred small files and a
	// copy of it is tens of milliseconds of file system work at best, seconds on
	// a cold or networked drive. A save must never stall on that, and the
	// scripting half is not needed until somebody opens the script editor. So
	// the manifest is written synchronously (it is small, and it is the part
	// that makes the project comparable straight away) and the copy is queued.
	//
	// npm install is NOT queued: it is a network download of minutes, and the
	// script editor's INSTALL already does it on demand. A project therefore
	// arrives complete on disk and gets its node_modules the first time it is
	// actually used for scripting.
	TSet<FString> GScaffolding;

	void QueueScaffold(const FString& Dir, const FString& Save, const FString& Level)
	{
		if (GScaffolding.Contains(Dir)) return;
		const FString Template = BF6Script::TemplateDir();   // GConfig: game thread
		if (Template.IsEmpty())
		{
			UE_LOG(LogBF6Project, Warning, TEXT("project '%s': the Portal scripting template is not installed, so the scripting half was skipped. Install the Portal SDK, or set [BF6UnrealSDK] ScriptTemplateDir."), *Save);
			return;
		}
		GScaffolding.Add(Dir);

		FString Description;
		{
			const TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(Dir));
			if (M.IsValid()) M->TryGetStringField(TEXT("description"), Description);
		}
		FString ExpId;
		if (!Level.IsEmpty()) ExpId = IdFromUrl(BF6PortalWeb::ExperienceForSave(Level, Save));

		Async(EAsyncExecution::ThreadPool, [Dir, Save, Level, Description, ExpId, Template]()
		{
			const double T0 = FPlatformTime::Seconds();
			FString Why;
			const bool bOk = BF6Script::ScaffoldProject(Dir, Save, Description, TEXT("plain"), ExpId, Why);

			int32 Removed = 0;
			if (bOk)
			{
				IFileManager& FM = IFileManager::Get();
				ParkTemplateSpatials(Dir, Template);

				// His readme is the reference for src/, and ours is the one a
				// creator opening this folder should see first.
				const FString TheirReadme = FPaths::Combine(Dir, TEXT("README.md"));
				const FString Kept = FPaths::Combine(Dir, TEXT("TEMPLATE.md"));
				if (FPaths::FileExists(TheirReadme))
				{
					if (FPaths::FileExists(Kept)) FM.Delete(*Kept);
					FM.Move(*Kept, *TheirReadme);
				}

				// DEPLOY IS NOT OURS. The script works from an EA session id;
				// the tool never touches a credential, so it does not ship in a
				// project the tool made.
				const FString Deploy = FPaths::Combine(Dir, TEXT("scripts"), TEXT("deploy.js"));
				if (FPaths::FileExists(Deploy) && FM.Delete(*Deploy)) ++Removed;

				const FString PkgPath = FPaths::Combine(Dir, TEXT("package.json"));
				TSharedPtr<FJsonObject> Pkg = ReadJson(PkgPath);
				if (Pkg.IsValid())
				{
					const TSharedPtr<FJsonObject>* Scripts = nullptr;
					if (Pkg->TryGetObjectField(TEXT("scripts"), Scripts) && Scripts)
					{
						TArray<FString> Drop;
						for (const auto& It : (*Scripts)->Values)
						{
							const FString K(*It.Key);
							if (K.StartsWith(TEXT("deploy"))) Drop.Add(K);
						}
						for (const FString& K : Drop) { (*Scripts)->RemoveField(K); ++Removed; }
					}
					WriteJson(PkgPath, Pkg);
				}
			}

			AsyncTask(ENamedThreads::GameThread, [Dir, Save, Level, bOk, Why, Removed, T0]()
			{
				GScaffolding.Remove(Dir);
				if (!bOk)
				{
					UE_LOG(LogBF6Project, Warning, TEXT("project '%s': the scripting half could not be written. %s"), *Save, *Why);
					return;
				}
				TArray<FString> Levels;
				FFileHelper::SaveStringToFile(ReadmeText(Save, Levels.Num() ? Levels : TArray<FString>({ Level })),
					*FPaths::Combine(Dir, TEXT("README.md")));
				WriteManifest(Dir, Save, Level);
				UE_LOG(LogBF6Project, Display,
					TEXT("project '%s': scripting template added in %.0f ms (%d deploy entries removed, npm install is left for the script editor)."),
					*Save, (FPlatformTime::Seconds() - T0) * 1000.0, Removed);
			});
		});
	}

	// ---- facts, for the compare --------------------------------------------

	struct FSideFacts
	{
		TArray<FString>            Levels;
		TMap<FString, TSet<int32>> IdsByLevel;
		TMap<FString, int32>       CountByLevel;
		TSet<FString>              WorkspaceNames;
		TSet<FString>              ScriptFiles;
		bool bHaveWorkspace = false;
		bool bHaveScript = false;
	};

	// Every value of a field called NAME, or ending in _NAME, anywhere in a
	// Blockly document. That is where a rule's name and a subroutine's name
	// live, and reading them by shape means a site change to the block types
	// does not silently empty the signal.
	void CollectNames(const TSharedPtr<FJsonValue>& V, TSet<FString>& Out, int32 Depth)
	{
		if (!V.IsValid() || Depth > 40) return;
		if (V->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) return;
			for (const auto& It : O->Values)
			{
				const FString Key(*It.Key);
				if (It.Value.IsValid() && It.Value->Type == EJson::String
					&& (Key == TEXT("NAME") || Key.EndsWith(TEXT("_NAME"))))
				{
					const FString S = It.Value->AsString();
					if (!S.IsEmpty()) Out.Add(S);
				}
				CollectNames(It.Value, Out, Depth + 1);
			}
			return;
		}
		if (V->Type == EJson::Array)
			for (const TSharedPtr<FJsonValue>& E : V->AsArray()) CollectNames(E, Out, Depth + 1);
	}

	void CollectNamesRoot(const TSharedPtr<FJsonObject>& Root, TSet<FString>& Out)
	{
		if (!Root.IsValid()) return;
		CollectNames(MakeShared<FJsonValueObject>(Root), Out, 0);
	}

	// The ObjIds a session save carries. They live as "ObjId=<n>" inside each
	// object's props array, which is where SaveSession writes the actor tags.
	void ReadSessionIds(const TSharedPtr<FJsonObject>& Root, TSet<int32>& Ids, int32& Count)
	{
		if (!Root.IsValid()) return;
		static const TCHAR* Arrays[] = { TEXT("objects"), TEXT("base") };
		for (const TCHAR* Key : Arrays)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (!Root->TryGetArrayField(Key, Arr) || !Arr) continue;
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				if (!O.IsValid()) continue;
				const TArray<TSharedPtr<FJsonValue>>* Props = nullptr;
				if (!O->TryGetArrayField(TEXT("props"), Props) || !Props) continue;
				for (const TSharedPtr<FJsonValue>& P : *Props)
				{
					const FString S = P->AsString();
					if (!S.StartsWith(TEXT("ObjId="))) continue;
					const int32 Id = FCString::Atoi(*S.Mid(6));
					if (Id >= 0) { Ids.Add(Id); ++Count; }
				}
			}
		}
	}

	// The ObjIds a .spatial.json carries. Every Portal_Dynamic entry that has
	// been given one has it as a plain "ObjId" number; -1 means unassigned.
	void ReadSpatialIds(const TSharedPtr<FJsonObject>& Root, TSet<int32>& Ids, int32& Count)
	{
		if (!Root.IsValid()) return;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("Portal_Dynamic"), Arr) || !Arr) return;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) continue;
			double D = -1.0;
			if (!O->TryGetNumberField(TEXT("ObjId"), D)) continue;
			const int32 Id = (int32)D;
			if (Id >= 0) { Ids.Add(Id); ++Count; }
		}
	}

	FSideFacts ProjectFacts(const FString& Dir)
	{
		FSideFacts F;
		TArray<FArtefact> Arts = GatherArtefacts(Dir, F.Levels);
		for (const FArtefact& A : Arts)
		{
			if (A.Kind == TEXT("session") && !A.Level.IsEmpty())
			{
				TSet<int32> Ids; int32 N = 0;
				ReadSessionIds(ReadJson(FPaths::Combine(Dir, A.Rel)), Ids, N);
				F.IdsByLevel.Add(A.Level, MoveTemp(Ids));
				F.CountByLevel.Add(A.Level, N);
			}
			else if (A.Kind == TEXT("script") || A.Kind == TEXT("strings"))
			{
				F.ScriptFiles.Add(FPaths::GetCleanFilename(A.Rel));
				F.bHaveScript = true;
			}
		}
		const TSharedPtr<FJsonObject> W = ReadJson(FPaths::Combine(Dir, TEXT("unreal"), TEXT("blockly"), TEXT("workspace.json")));
		if (W.IsValid()) { CollectNamesRoot(W, F.WorkspaceNames); F.bHaveWorkspace = true; }
		return F;
	}

	// A candidate is a folder in the profile's own experience cache. Reading it
	// straight off disk means COMPARE works with the panel closed, the site
	// down and the account signed out, which is the only way it can be trusted
	// as a check rather than a request.
	struct FCandidate
	{
		FString Id, Name, Dir;
		TArray<FString> SpatialFiles;
		bool bFetched = false;
	};

	TArray<FCandidate> Candidates()
	{
		TArray<FCandidate> Out;
		const FString Root = PortalCacheRoot();
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("*")), false, true);
		for (const FString& D : Dirs)
		{
			FCandidate C;
			C.Id = D;
			C.Dir = FPaths::Combine(Root, D);
			const TSharedPtr<FJsonObject> E = ReadJson(FPaths::Combine(C.Dir, TEXT("experience.json")));
			if (E.IsValid())
			{
				E->TryGetStringField(TEXT("name"), C.Name);
				E->TryGetBoolField(TEXT("fetched"), C.bFetched);
			}
			IFileManager::Get().FindFiles(C.SpatialFiles, *(C.Dir / TEXT("*.spatial.json")), true, false);
			Out.Add(MoveTemp(C));
		}
		return Out;
	}

	float Jaccard(const TSet<int32>& A, const TSet<int32>& B)
	{
		if (A.Num() == 0 || B.Num() == 0) return 0.f;
		int32 Inter = 0;
		for (const int32 X : A) if (B.Contains(X)) ++Inter;
		const int32 Uni = A.Num() + B.Num() - Inter;
		return Uni > 0 ? (float)Inter / (float)Uni : 0.f;
	}

	float JaccardS(const TSet<FString>& A, const TSet<FString>& B)
	{
		if (A.Num() == 0 || B.Num() == 0) return 0.f;
		int32 Inter = 0;
		for (const FString& X : A) if (B.Contains(X)) ++Inter;
		const int32 Uni = A.Num() + B.Num() - Inter;
		return Uni > 0 ? (float)Inter / (float)Uni : 0.f;
	}
}

// ---------------------------------------------------------------------------
// The project folder.
// ---------------------------------------------------------------------------
FString BF6Project::DirFor(const FString& Save)
{
	if (Save.IsEmpty()) return FString();
	// AN EXPERIENCE IS ONE PROJECT WITH MANY MAPS. When a folder of this name
	// is an experience, that folder IS the project: one script project, one
	// settings set, one thumbnail, one workspace, and its maps underneath it.
	// Everything else keeps the per-save folder it has always had.
	const FString E = ExperienceDir(Save);
	if (!E.IsEmpty()) return E;
	return FPaths::Combine(SavesRoot(), Save);
}

// Inside saves/, not beside it. saves/ is the one folder a creator should ever
// have to look in: standalone maps directly in it, experiences under
// saves/experiences/. Sitting outside it meant an empty saves folder next to
// fifteen megabytes of work, which reads as lost work.
FString BF6Project::ExperiencesRoot()
{
	return FPaths::Combine(SavesRoot(), TEXT("experiences"));
}

// The old sibling location, kept only so MoveExperiencesIntoSaves can find
// what an earlier build left there. Nothing else may use it.
FString BF6Project::LegacyExperiencesRoot()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("experiences"));
}

// Move what the previous layout left beside saves/ into it. Runs once at
// startup, moves rather than copies (same volume, so it is a rename), and
// leaves anything it cannot move exactly where it is rather than half-doing it.
int32 BF6Project::MoveExperiencesIntoSaves()
{
	const FString Old = LegacyExperiencesRoot();
	const FString New = ExperiencesRoot();
	if (Old == New || !FPaths::DirectoryExists(Old)) return 0;

	IFileManager& FM = IFileManager::Get();
	TArray<FString> Dirs;
	FM.FindFiles(Dirs, *(Old / TEXT("*")), false, true);
	if (Dirs.Num() == 0)
	{
		FM.DeleteDirectory(*Old, false, false);   // empty leftover, no loss possible
		return 0;
	}

	FM.MakeDirectory(*New, true);
	int32 Moved = 0;
	for (const FString& D : Dirs)
	{
		const FString From = Old / D;
		const FString To   = New / D;
		if (FPaths::DirectoryExists(To))
		{
			UE_LOG(LogBF6Project, Warning,
				TEXT("'%s' is already under saves/experiences, so the older copy at %s was left alone."),
				*D, *From);
			continue;
		}
		if (FM.Move(*To, *From, true, true))
		{
			Moved++;
			UE_LOG(LogBF6Project, Display, TEXT("Moved experience '%s' into saves/experiences."), *D);
		}
		else
		{
			UE_LOG(LogBF6Project, Warning,
				TEXT("Could not move '%s' into saves/experiences. It is still at %s and nothing was lost."),
				*D, *From);
		}
	}
	// Only when it is genuinely empty. A folder that still holds something is
	// left standing, because whatever is in it did not move.
	TArray<FString> Left;
	FM.FindFiles(Left, *(Old / TEXT("*")), true, true);
	if (Left.Num() == 0) FM.DeleteDirectory(*Old, false, false);
	return Moved;
}

FString BF6Project::ExperienceDir(const FString& Folder)
{
	if (Folder.IsEmpty()) return FString();
	const FString D = FPaths::Combine(ExperiencesRoot(), Folder);
	return FPaths::FileExists(FPaths::Combine(D, TEXT("project.json"))) ? D : FString();
}

bool BF6Project::IsExperience(const FString& Save) { return !ExperienceDir(Save).IsEmpty(); }

FString BF6Project::UiDesignDir(const FString& Save)
{
	// unreal/ui is what kOurFolders, the readme, GatherArtefacts and the
	// manifest already call the UI designs folder, so this returns that and
	// nothing else. A second opinion about where a design lives is how the
	// same design ends up saved twice and restored once.
	const FString Dir = DirFor(Save);
	if (Dir.IsEmpty()) return FString();
	return FPaths::Combine(Dir, TEXT("unreal"), TEXT("ui"));
}

FString BF6Project::MapDir(const FString& Save, const FString& Level)
{
	const FString E = ExperienceDir(Save);
	if (E.IsEmpty()) return FString();
	return FPaths::Combine(E, TEXT("maps"), Level);
}

TArray<FString> BF6Project::ExperienceFolders()
{
	TArray<FString> Out, Dirs;
	IFileManager::Get().FindFiles(Dirs, *(ExperiencesRoot() / TEXT("*")), false, true);
	for (const FString& D : Dirs)
		if (FPaths::FileExists(FPaths::Combine(ExperiencesRoot(), D, TEXT("project.json")))) Out.Add(D);
	Out.Sort();
	return Out;
}

TArray<FString> BF6Project::MapsIn(const FString& Save)
{
	TArray<FString> Out;
	const FString E = ExperienceDir(Save);
	if (E.IsEmpty()) return Out;
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(FPaths::Combine(E, TEXT("maps")) / TEXT("*")), false, true);
	for (const FString& D : Dirs)
		if (FPaths::FileExists(FPaths::Combine(E, TEXT("maps"), D, D + TEXT(".json")))) Out.Add(D);
	Out.Sort();
	return Out;
}

// A display name becomes a folder, so the filesystem has a veto - and two
// experiences are allowed the same name, so a collision takes the first eight
// characters of the uuid. The uuid in the manifest is the identity; the folder
// is only how a person finds it in Explorer.
FString BF6Project::FolderNameFor(const FString& DisplayName, const FString& Id)
{
	FString S = DisplayName.TrimStartAndEnd();
	const FString Bad = TEXT("\\/:*?\"<>|");
	FString Clean;
	for (const TCHAR C : S)
	{
		int32 Ignore;
		Clean.AppendChar((Bad.FindChar(C, Ignore) || C < 32) ? TEXT('-') : C);
	}
	Clean.TrimStartAndEndInline();
	while (Clean.EndsWith(TEXT("."))) Clean.LeftChopInline(1);
	if (Clean.IsEmpty()) Clean = TEXT("experience");
	if (Clean.Len() > 80) Clean.LeftInline(80);

	// Taken by a DIFFERENT experience: same name, different uuid.
	const FString Existing = FPaths::Combine(ExperiencesRoot(), Clean, TEXT("project.json"));
	if (FPaths::FileExists(Existing))
	{
		const TSharedPtr<FJsonObject> M = ReadJson(Existing);
		const TSharedPtr<FJsonObject>* E = nullptr;
		FString Was;
		if (M.IsValid() && M->TryGetObjectField(TEXT("experience"), E) && E) (*E)->TryGetStringField(TEXT("id"), Was);
		if (!Id.IsEmpty() && Was != Id) Clean += TEXT(" (") + Id.Left(8) + TEXT(")");
	}
	return Clean;
}

// The folder an experience uuid already has on disk, or empty.
FString BF6Project::FolderForExperience(const FString& Id)
{
	if (Id.IsEmpty()) return FString();
	for (const FString& F : ExperienceFolders())
	{
		const TSharedPtr<FJsonObject> M = ReadJson(FPaths::Combine(ExperiencesRoot(), F, TEXT("project.json")));
		const TSharedPtr<FJsonObject>* E = nullptr;
		FString Was;
		if (M.IsValid() && M->TryGetObjectField(TEXT("experience"), E) && E && (*E)->TryGetStringField(TEXT("id"), Was) && Was == Id)
			return F;
	}
	return FString();
}

FString BF6Project::EnsureExperience(const FString& Id, const FString& DisplayName)
{
	FString Folder = FolderForExperience(Id);
	if (Folder.IsEmpty())
		Folder = FolderNameFor(DisplayName.IsEmpty() ? Id.Left(8) : DisplayName, Id);
	if (Folder.IsEmpty()) return FString();

	const FString Dir = FPaths::Combine(ExperiencesRoot(), Folder);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Dir, TEXT("maps")), true);

	// THE SEED MANIFEST GOES DOWN FIRST, and the order is not cosmetic: DirFor
	// decides which layout a name means by whether this file is there, so
	// everything below would write into the standalone folder without it.
	if (!FPaths::FileExists(ManifestPath(Dir)))
	{
		TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("manifest"), TEXT("bf6-unreal-project"));
		M->SetNumberField(TEXT("manifestVersion"), kManifestVersion);
		M->SetStringField(TEXT("save"), Folder);
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		if (!Id.IsEmpty()) { E->SetStringField(TEXT("id"), Id); E->SetStringField(TEXT("url"), UrlForId(Id)); }
		if (!DisplayName.IsEmpty()) E->SetStringField(TEXT("name"), DisplayName);
		E->SetStringField(TEXT("linkedAt"), NowIso());
		M->SetObjectField(TEXT("experience"), E);
		WriteJson(ManifestPath(Dir), M);
		UE_LOG(LogBF6Project, Display, TEXT("experience project created: %s"), *Dir);
	}
	Ensure(FString(), Folder, false);
	return Folder;
}

namespace
{
	// Copy one file and prove it arrived. Nothing is ever removed on the
	// strength of a return code alone.
	bool CopyVerified(const FString& From, const FString& To)
	{
		if (!FPaths::FileExists(From)) return false;
		IFileManager& FM = IFileManager::Get();
		FM.MakeDirectory(*FPaths::GetPath(To), true);
		if (FM.Copy(*To, *From, true, true) != COPY_OK) return false;
		if (!FPaths::FileExists(To)) return false;
		if (FM.FileSize(*To) != FM.FileSize(*From)) return false;
		return HashFile(To) == HashFile(From);
	}

	// Where a standalone save keeps each of its per-map files today.
	FString StandaloneSession(const FString& Level, const FString& Save)
	{
		const FString New = FPaths::Combine(BF6Project::DirFor(Save), Level + TEXT(".json"));
		if (FPaths::FileExists(New)) return New;
		const FString Old = OldLayoutPath(Level, Save);
		return FPaths::FileExists(Old) ? Old : FString();
	}
	FString StandaloneSpatial(const FString& Level, const FString& Save)
	{
		const FString Dir = FPaths::Combine(BF6Project::DirFor(Save), TEXT("spatials"));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), true, false);
		for (const FString& F : Files)
			if (F.StartsWith(Level, ESearchCase::IgnoreCase)) return FPaths::Combine(Dir, F);
		return FString();
	}
	FString StandaloneTscn(const FString& Level, const FString& Save)
	{
		const FString Dir = FPaths::Combine(BF6Project::DirFor(Save), TEXT("unreal"), TEXT("tscn"));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.tscn")), true, false);
		for (const FString& F : Files)
			if (F.StartsWith(Level, ESearchCase::IgnoreCase)) return FPaths::Combine(Dir, F);
		return FString();
	}
}

bool BF6Project::MoveIntoExperience(const FString& Level, const FString& Save,
	const FString& ExperienceFolder, FString& OutWhat)
{
	if (Level.IsEmpty() || Save.IsEmpty() || ExperienceFolder.IsEmpty())
	{ OutWhat = TEXT("a map, a save and an experience are all needed"); return false; }
	if (IsExperience(Save))
	{ OutWhat = FString::Printf(TEXT("'%s' is already an experience project"), *Save); return false; }
	const FString EDir = ExperienceDir(ExperienceFolder);
	if (EDir.IsEmpty())
	{ OutWhat = FString::Printf(TEXT("there is no experience project called '%s'"), *ExperienceFolder); return false; }

	const FString To = FPaths::Combine(EDir, TEXT("maps"), Level);
	if (FPaths::FileExists(FPaths::Combine(To, Level + TEXT(".json"))))
	{ OutWhat = FString::Printf(TEXT("'%s' already has a %s"), *ExperienceFolder, *Level); return false; }

	// NEVER MOVE WITHOUT A SNAPSHOT. Kept, not rotating, so it cannot age out
	// from under the one person who will ever need it.
	SnapshotProject(Save, FString::Printf(TEXT("before moving %s into %s"), *Level, *ExperienceFolder));

	const FString Sess = StandaloneSession(Level, Save);
	if (Sess.IsEmpty()) { OutWhat = FString::Printf(TEXT("no saved %s in '%s'"), *Level, *Save); return false; }

	// EVERYTHING IS COPIED AND CHECKED BEFORE ANYTHING IS REMOVED. A move that
	// cannot verify leaves the original exactly where it was.
	TArray<TPair<FString, FString>> Done;
	auto Take = [&](const FString& From, const FString& Leaf) -> bool
	{
		if (From.IsEmpty()) return true;                 // nothing of this kind is not a failure
		const FString Dst = FPaths::Combine(To, Leaf);
		if (!CopyVerified(From, Dst)) return false;
		Done.Add(TPair<FString, FString>(From, Dst));
		return true;
	};
	const bool bOk =
		   Take(Sess,                              Level + TEXT(".json"))
		&& Take(StandaloneSpatial(Level, Save),    Level + TEXT(".spatial.json"))
		&& Take(StandaloneTscn(Level, Save),       Level + TEXT(".tscn"));
	if (!bOk)
	{
		for (const TPair<FString, FString>& P : Done) IFileManager::Get().Delete(*P.Value);
		OutWhat = FString::Printf(TEXT("%s could not be copied into '%s', so nothing was moved"), *Level, *ExperienceFolder);
		return false;
	}

	for (const TPair<FString, FString>& P : Done) IFileManager::Get().Delete(*P.Key);

	// The link the panel keeps is per (level, save name), and the save name has
	// just changed, so it is written again under the new one.
	const FString Url = BF6PortalWeb::ExperienceForSave(Level, Save);
	const int32   Idx = BF6PortalWeb::MapIdxForSave(Level, Save);
	if (!Url.IsEmpty()) BF6PortalWeb::SetSaveLink(Level, ExperienceFolder, Url, Idx);

	Ensure(Level, ExperienceFolder, false);
	WriteManifest(EDir, ExperienceFolder, Level);
	OutWhat = FString::Printf(TEXT("%s moved from '%s' into experience '%s' (%d file(s))"),
		*Level, *Save, *ExperienceFolder, Done.Num());
	UE_LOG(LogBF6Project, Display, TEXT("%s"), *OutWhat);
	return true;
}

bool BF6Project::MoveOutOfExperience(const FString& Level, const FString& Save,
	const FString& NewSaveName, FString& OutWhat)
{
	const FString EDir = ExperienceDir(Save);
	if (EDir.IsEmpty()) { OutWhat = FString::Printf(TEXT("'%s' is not an experience project"), *Save); return false; }
	FString Name = NewSaveName.TrimStartAndEnd();
	if (Name.IsEmpty()) Name = FString::Printf(TEXT("%s (%s)"), *Save, *Level);
	if (FPaths::FileExists(ManifestPath(FPaths::Combine(SavesRoot(), Name))))
	{ OutWhat = FString::Printf(TEXT("a save called '%s' is already there"), *Name); return false; }

	const FString From = FPaths::Combine(EDir, TEXT("maps"), Level);
	if (!FPaths::FileExists(FPaths::Combine(From, Level + TEXT(".json"))))
	{ OutWhat = FString::Printf(TEXT("'%s' has no %s"), *Save, *Level); return false; }

	SnapshotProject(Save, FString::Printf(TEXT("before taking %s out of %s"), *Level, *Save));

	const FString To = FPaths::Combine(SavesRoot(), Name);
	TArray<TPair<FString, FString>> Done;
	auto Take = [&](const FString& Leaf, const FString& DstRel) -> bool
	{
		const FString Src = FPaths::Combine(From, Leaf);
		if (!FPaths::FileExists(Src)) return true;
		const FString Dst = FPaths::Combine(To, DstRel);
		if (!CopyVerified(Src, Dst)) return false;
		Done.Add(TPair<FString, FString>(Src, Dst));
		return true;
	};
	const bool bOk =
		   Take(Level + TEXT(".json"),         Level + TEXT(".json"))
		&& Take(Level + TEXT(".spatial.json"), FString(TEXT("spatials/")) + Level + TEXT(".spatial.json"))
		&& Take(Level + TEXT(".tscn"),         FString(TEXT("unreal/tscn/")) + Level + TEXT(".tscn"));
	if (!bOk)
	{
		for (const TPair<FString, FString>& P : Done) IFileManager::Get().Delete(*P.Value);
		OutWhat = FString::Printf(TEXT("%s could not be copied out of '%s', so nothing was moved"), *Level, *Save);
		return false;
	}
	for (const TPair<FString, FString>& P : Done) IFileManager::Get().Delete(*P.Key);
	IFileManager::Get().DeleteDirectory(*From, false, true);

	const FString Url = BF6PortalWeb::ExperienceForSave(Level, Save);
	const int32   Idx = BF6PortalWeb::MapIdxForSave(Level, Save);
	if (!Url.IsEmpty()) BF6PortalWeb::SetSaveLink(Level, Name, Url, Idx);

	Ensure(Level, Name, false);
	WriteManifest(EDir, Save, FString());
	OutWhat = FString::Printf(TEXT("%s taken out of experience '%s' and is now the save '%s'"), *Level, *Save, *Name);
	UE_LOG(LogBF6Project, Display, TEXT("%s"), *OutWhat);
	return true;
}

// ---------------------------------------------------------------------------
// MIGRATION.
//
// An earlier import wrote one flat sibling save per map: eleven folders called
// "Night Ops Breakthrough copy (MP_Isolated)" and so on, each carrying its own
// copy of a script project that is meant to exist once. This turns each of them
// into a map of its experience.
//
// NON-DESTRUCTIVE, AND IT MEANS IT. Every file is copied and hashed at the far
// end before the original is deleted, a kept snapshot is taken first, and a
// save whose experience cannot be identified is left exactly where it is with
// a line saying why.
// ---------------------------------------------------------------------------
int32 BF6Project::MigrateFlatSaves(bool bDryRun)
{
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(SavesRoot() / TEXT("*")), false, true);
	Dirs.Sort();
	int32 Moved = 0, Left = 0;

	for (const FString& Save : Dirs)
	{
		const FString Dir = FPaths::Combine(SavesRoot(), Save);
		TArray<FString> Levels;
		GatherArtefacts(Dir, Levels);
		if (Levels.Num() == 0)
		{
			UE_LOG(LogBF6Project, Display, TEXT("  left alone  '%s': no saved map in it"), *Save);
			Left++;
			continue;
		}

		// WHICH EXPERIENCE. The manifest first, then the panel's own per-save
		// link. Nothing is guessed: a save that answers neither is not moved.
		FString Id, Name;
		const TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(Dir));
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (M.IsValid() && M->TryGetObjectField(TEXT("experience"), E) && E)
		{ (*E)->TryGetStringField(TEXT("id"), Id); (*E)->TryGetStringField(TEXT("name"), Name); }
		if (Id.IsEmpty())
			for (const FString& L : Levels)
			{
				const FString Url = BF6PortalWeb::ExperienceForSave(L, Save);
				Id = IdFromUrl(Url);
				if (!Id.IsEmpty()) break;
			}
		if (Id.IsEmpty())
		{
			UE_LOG(LogBF6Project, Display, TEXT("  left alone  '%s': it belongs to no experience, so it stays a standalone save"), *Save);
			Left++;
			continue;
		}
		if (Name.IsEmpty())
		{
			const TSharedPtr<FJsonObject> Cache = ReadJson(FPaths::Combine(PortalCacheRoot(), Id, TEXT("experience.json")));
			if (Cache.IsValid()) Cache->TryGetStringField(TEXT("name"), Name);
		}
		if (Name.IsEmpty()) Name = Save;

		if (bDryRun)
		{
			UE_LOG(LogBF6Project, Display, TEXT("  would move  '%s' (%s) -> experience '%s'"),
				*Save, *FString::Join(Levels, TEXT(", ")), *FolderNameFor(Name, Id));
			Moved += Levels.Num();
			continue;
		}

		const FString Folder = EnsureExperience(Id, Name);
		if (Folder.IsEmpty()) { Left++; continue; }
		for (const FString& L : Levels)
		{
			FString What;
			if (MoveIntoExperience(L, Save, Folder, What)) { Moved++; UE_LOG(LogBF6Project, Display, TEXT("  moved       %s"), *What); }
			else { Left++; UE_LOG(LogBF6Project, Warning, TEXT("  left alone  %s"), *What); }
		}
		// The emptied standalone folder goes only when nothing of the user's is
		// left in it. Its template copy is now redundant, but a folder with a
		// file we did not put there is never deleted.
		TArray<FString> Still;
		GatherArtefacts(Dir, Still);
		if (Still.Num() == 0)
			UE_LOG(LogBF6Project, Display, TEXT("  '%s' has nothing of yours left in it. Delete it when you are happy: %s"), *Save, *Dir);
	}
	UE_LOG(LogBF6Project, Display, TEXT("migration %s: %d map(s) %s, %d left alone."),
		bDryRun ? TEXT("preview") : TEXT("done"), Moved, bDryRun ? TEXT("would move") : TEXT("moved"), Left);
	return Moved;
}

FString BF6Project::ScriptDirForSave(const FString& Level, const FString& Save)
{
	if (IsExperience(Save)) return DirFor(Save);
	const FString Id = IdFromUrl(BF6PortalWeb::ExperienceForSave(Level, Save));
	const FString Folder = FolderForExperience(Id);
	return Folder.IsEmpty() ? FString() : ExperienceDir(Folder);
}

bool BF6Project::HasManifest(const FString& Save)
{
	const FString D = DirFor(Save);
	return !D.IsEmpty() && FPaths::FileExists(ManifestPath(D));
}

bool BF6Project::HasTemplate(const FString& Save)
{
	const FString D = DirFor(Save);
	return !D.IsEmpty()
		&& FPaths::FileExists(FPaths::Combine(D, TEXT("package.json")))
		&& FPaths::DirectoryExists(FPaths::Combine(D, TEXT("src")));
}

void BF6Project::Ensure(const FString& Level, const FString& Save, bool bWaitForTemplate)
{
	if (Save.IsEmpty()) return;
	const FString Dir = DirFor(Save);
	IFileManager& FM = IFileManager::Get();

	const bool bNew = !FPaths::FileExists(ManifestPath(Dir));
	TArray<FString> Added;

	if (!FPaths::DirectoryExists(Dir)) { FM.MakeDirectory(*Dir, true); Added.Add(TEXT("the project folder")); }

	// MIGRATION. A save from the old flat layout has its file somewhere else
	// entirely. Copy it in rather than move it: nothing an upgrade touches may
	// be the only copy of anything.
	if (!Level.IsEmpty())
	{
		const FString Here = FPaths::Combine(Dir, Level + TEXT(".json"));
		const FString There = OldLayoutPath(Level, Save);
		if (!FPaths::FileExists(Here) && FPaths::FileExists(There) && FM.Copy(*Here, *There) == COPY_OK)
			Added.Add(TEXT("the session file, copied out of the old flat layout"));
	}

	for (const FOurFolder& F : kOurFolders)
	{
		const FString Sub = FPaths::Combine(Dir, F.Rel);
		if (!FPaths::DirectoryExists(Sub)) { FM.MakeDirectory(*Sub, true); Added.Add(F.Rel); }
	}

	TArray<FString> Levels;
	GatherArtefacts(Dir, Levels);
	if (!Level.IsEmpty()) Levels.AddUnique(Level);
	if (!FPaths::FileExists(FPaths::Combine(Dir, TEXT("README.md"))))
	{
		FFileHelper::SaveStringToFile(ReadmeText(Save, Levels), *FPaths::Combine(Dir, TEXT("README.md")));
		Added.Add(TEXT("README.md"));
	}

	const bool bNeedTemplate = !HasTemplate(Save);
	if (bNeedTemplate)
	{
		if (bWaitForTemplate)
		{
			const FString Template = BF6Script::TemplateDir();
			if (Template.IsEmpty())
			{
				UE_LOG(LogBF6Project, Warning, TEXT("project '%s': no scripting template installed, so the scripting half was skipped."), *Save);
			}
			else
			{
				FString Why;
				if (BF6Script::ScaffoldProject(Dir, Save, FString(), TEXT("plain"), FString(), Why))
				{
					ParkTemplateSpatials(Dir, Template);
					const FString TheirReadme = FPaths::Combine(Dir, TEXT("README.md"));
					const FString Kept = FPaths::Combine(Dir, TEXT("TEMPLATE.md"));
					if (FPaths::FileExists(Kept)) FM.Delete(*Kept);
					FM.Move(*Kept, *TheirReadme);
					FFileHelper::SaveStringToFile(ReadmeText(Save, Levels), *TheirReadme);
					const FString Deploy = FPaths::Combine(Dir, TEXT("scripts"), TEXT("deploy.js"));
					if (FPaths::FileExists(Deploy)) FM.Delete(*Deploy);
					TSharedPtr<FJsonObject> Pkg = ReadJson(FPaths::Combine(Dir, TEXT("package.json")));
					const TSharedPtr<FJsonObject>* Scripts = nullptr;
					if (Pkg.IsValid() && Pkg->TryGetObjectField(TEXT("scripts"), Scripts) && Scripts)
					{
						TArray<FString> Drop;
						for (const auto& It : (*Scripts)->Values)
						{
							const FString K(*It.Key);
							if (K.StartsWith(TEXT("deploy"))) Drop.Add(K);
						}
						for (const FString& K : Drop) (*Scripts)->RemoveField(K);
						WriteJson(FPaths::Combine(Dir, TEXT("package.json")), Pkg);
					}
					Added.Add(TEXT("the scripting template"));
				}
				else
				{
					UE_LOG(LogBF6Project, Warning, TEXT("project '%s': %s"), *Save, *Why);
				}
			}
		}
		else
		{
			QueueScaffold(Dir, Save, Level);
		}
	}

	WriteManifest(Dir, Save, Level);

	// ONE LINE PER AUTOMATIC STEP, and only when something actually happened.
	// A save that changes nothing about the project should be silent.
	if (Added.Num() > 0)
	{
		UE_LOG(LogBF6Project, Display, TEXT("project '%s' %s: added %s"),
			*Save, bNew ? TEXT("created") : TEXT("upgraded"), *FString::Join(Added, TEXT(", ")));
	}
}

void BF6Project::NoteSaved(const FString& Level, const FString& Save)
{
	Ensure(Level, Save, false);
	// The rotating project snapshot rides the save, off the game thread and
	// only when something it covers actually moved.
	const FString Dir = DirFor(Save);
	Async(EAsyncExecution::ThreadPool, [Dir, Save]()
	{
		BF6Project::SnapshotProject(Save, FString());
	});
}

// ---------------------------------------------------------------------------
// PROJECT BACKUPS.
//
// Project level, and deliberately NOT a second copy of the temp autosave. The
// autosave keeps the session file per LEVEL, which is the map you were
// building. These keep what a sync can overwrite and the autosave never held:
// the exported spatials, the block workspace, the settings snapshot, the
// script sources and the manifest.
// ---------------------------------------------------------------------------
int32 BF6Project::BackupMax()
{
	int32 N = 5;
	if (GConfig) GConfig->GetInt(TEXT("BF6UnrealSDK"), TEXT("ProjectBackupMax"), N, GEditorPerProjectIni);
	return FMath::Clamp(N, 1, 50);
}

void BF6Project::SetBackupMax(int32 N)
{
	if (GConfig) GConfig->SetInt(TEXT("BF6UnrealSDK"), TEXT("ProjectBackupMax"), FMath::Clamp(N, 1, 50), GEditorPerProjectIni);
}

// ---------------------------------------------------------------------------
// Removing a whole experience.
//
// The snapshot CANNOT be SnapshotProject: that writes into .backups INSIDE the
// project, and the whole point here is that the project folder stops existing.
// The copy is taken to a sibling folder that survives the delete, and the
// delete only happens if the copy verified.
// ---------------------------------------------------------------------------
BF6Project::FRemoval BF6Project::DeleteExperience(const FString& Save, bool bDryRun)
{
	FRemoval R;
	R.Save = Save;

	if (Save.IsEmpty()) { R.Why = TEXT("no experience was named"); return R; }
	if (!IsExperience(Save))
	{
		R.Why = FString::Printf(TEXT("'%s' is not an experience project"), *Save);
		return R;
	}
	R.Dir = DirFor(Save);
	if (R.Dir.IsEmpty() || !FPaths::DirectoryExists(R.Dir))
	{
		R.Why = TEXT("that experience is not on disk");
		return R;
	}

	IFileManager& FM = IFileManager::Get();

	// What is about to go, counted before anything moves, so the dialog can say
	// it and the dry run can report it.
	R.Maps = MapsIn(Save).Num();
	TArray<FString> Files;
	FM.FindFilesRecursive(Files, *R.Dir, TEXT("*"), true, false);
	for (const FString& F : Files) R.Bytes += FM.FileSize(*F);
	const int32 Before = Files.Num();

	if (bDryRun) return R;

	// The kept copy, outside the folder and outside every rotation, so nothing
	// ages it out and a mistake is always recoverable.
	const FString Kept = FPaths::Combine(FPaths::GetPath(ExperiencesRoot()), TEXT("removed"),
		FString::Printf(TEXT("%s-%s"), *Save, *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
	FM.MakeDirectory(*Kept, true);
	if (!FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*Kept, *R.Dir, true))
	{
		R.Why = FString::Printf(TEXT("the copy to %s failed, so nothing was deleted"), *Kept);
		return R;
	}

	// VERIFY BEFORE DELETING. A copy that lost files is not a backup.
	TArray<FString> Copied;
	FM.FindFilesRecursive(Copied, *Kept, TEXT("*"), true, false);
	if (Copied.Num() < Before)
	{
		R.Why = FString::Printf(
			TEXT("the copy holds %d of %d files, so nothing was deleted. The copy is at %s"),
			Copied.Num(), Before, *Kept);
		return R;
	}
	R.Snapshot = Kept;

	if (!FM.DeleteDirectory(*R.Dir, false, true))
	{
		R.Why = FString::Printf(
			TEXT("the folder could not be deleted, which usually means a file in it is open. The copy is at %s"),
			*Kept);
		return R;
	}
	R.bDeleted = true;
	UE_LOG(LogBF6Project, Display,
		TEXT("Removed experience '%s': %d map(s), %d file(s). A copy is kept at %s"),
		*Save, R.Maps, Before, *Kept);
	return R;
}

BF6Project::FSnapshot BF6Project::SnapshotProjectChecked(const FString& Save, const FString& Why)
{
	FSnapshot R;
	const FString Dir = DirFor(Save);
	if (Dir.IsEmpty() || !FPaths::DirectoryExists(Dir))
	{
		R.Why = FString::Printf(TEXT("project '%s' is not on disk"), *Save);
		return R;
	}
	IFileManager& FM = IFileManager::Get();
	const FString Root = BackupRoot(Dir);
	const bool bRotating = Why.IsEmpty();

	const TArray<FString> Owned = OwnedFiles(Dir);
	R.Expected = Owned.Num();
	if (Owned.Num() == 0)
	{
		R.Why = FString::Printf(TEXT("project '%s' holds nothing a snapshot covers yet"), *Save);
		return R;
	}

	// THE FINGERPRINT, so a save that changed nothing does not spend a copy.
	//
	// It used to be the manifest's md5 fields with the session and tscn kinds
	// SKIPPED, because those were not in the snapshot. They are now, and a
	// fingerprint that cannot see the map would let a save that changed only
	// the map decide nothing had moved, leaving the newest snapshot holding an
	// older map and claiming to be current. So every artefact kind counts, and
	// the owned list itself is folded in with each file's size and timestamp so
	// that a file appearing, vanishing or being touched outside the manifest
	// (package.json, the lockfile, a UI design) also shows up.
	FString Fingerprint;
	{
		const TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(Dir));
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (M.IsValid() && M->TryGetArrayField(TEXT("artefacts"), Arr) && Arr)
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				FString P, H;
				if (!O.IsValid() || !O->TryGetStringField(TEXT("path"), P)) continue;
				O->TryGetStringField(TEXT("md5"), H);
				Fingerprint += P + TEXT("=") + H + TEXT("\n");
			}
		for (const FString& Rel : Owned)
		{
			const FString Full = FPaths::Combine(Dir, Rel);
			Fingerprint += FString::Printf(TEXT("%s|%lld|%s\n"),
				*Rel, FM.FileSize(*Full), *FM.GetTimeStamp(*Full).ToIso8601());
		}
		Fingerprint = FMD5::HashAnsiString(*Fingerprint);
	}

	if (bRotating)
	{
		TArray<FString> Dirs;
		FM.FindFiles(Dirs, *(Root / TEXT("*")), false, true);
		FString NewestFp;
		FDateTime Newest = FDateTime::MinValue();
		for (const FString& D : Dirs)
		{
			const TSharedPtr<FJsonObject> S = ReadJson(FPaths::Combine(Root, D, TEXT("snapshot.json")));
			bool bRot = false, bDone = false; FString Fp;
			if (!S.IsValid() || !S->TryGetBoolField(TEXT("rotating"), bRot) || !bRot) continue;
			// An INCOMPLETE snapshot must never satisfy the gate. If it did, the
			// next save would see a matching fingerprint, skip its own snapshot,
			// and the newest thing on disk would stay the one that is missing
			// files - the exact moment when a good snapshot matters most.
			if (!S->TryGetBoolField(TEXT("complete"), bDone) || !bDone) continue;
			S->TryGetStringField(TEXT("fingerprint"), Fp);
			const FDateTime T = FM.GetTimeStamp(*FPaths::Combine(Root, D));
			if (T > Newest) { Newest = T; NewestFp = Fp; }
		}
		if (!Fingerprint.IsEmpty() && Fingerprint == NewestFp)
		{
			R.Why = TEXT("nothing a snapshot covers has changed since the last one");
			return R;
		}
	}

	// A NAME NO OTHER SNAPSHOT CAN HAVE.
	//
	// Seconds are not enough: a restore takes its guard snapshot immediately
	// after whatever prompted it, so two can land in the same second and the
	// second one then writes into the first one's folder - which quietly
	// destroys the very copy the guard exists to keep. If the name is taken,
	// step along until one is free.
	{
		const FString Prefix = (bRotating ? FString() : TEXT("kept-"))
			+ FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
		R.Stamp = Prefix;
		R.Dir = FPaths::Combine(Root, R.Stamp);
		for (int32 Try = 2; Try <= 99 && FPaths::DirectoryExists(R.Dir); Try++)
		{
			R.Stamp = FString::Printf(TEXT("%s-%d"), *Prefix, Try);
			R.Dir = FPaths::Combine(Root, R.Stamp);
		}
	}

	FCopySet Set;
	CopyOwned(Dir, R.Dir, Owned, Set);
	R.Files = Set.Copied.Num();
	R.Bytes = Set.Bytes;
	R.Missing = Set.Failed;
	R.bComplete = Set.Complete() && R.Files == R.Expected;

	if (R.Files == 0)
	{
		FM.DeleteDirectory(*R.Dir, false, true);
		R.Stamp.Reset();
		R.Dir.Reset();
		R.Why = FString::Printf(TEXT("not one of the %d file(s) in project '%s' could be copied"), R.Expected, *Save);
		UE_LOG(LogBF6Project, Warning, TEXT("%s"), *R.Why);
		return R;
	}

	// THE SNAPSHOT DESCRIBES ITSELF, INCLUDING WHEN IT IS INCOMPLETE.
	//
	// A restore has to be able to tell a snapshot it may reconcile against
	// from one it may only copy out of, and it cannot work that out by looking
	// at the folder: a copy that failed halfway leaves a folder that looks
	// perfectly ordinary. So the file count, the completeness and a hash per
	// file are written here, at the one moment the truth is known.
	TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
	S->SetStringField(TEXT("save"), Save);
	S->SetStringField(TEXT("when"), NowIso());
	S->SetStringField(TEXT("why"), bRotating ? TEXT("save") : Why);
	S->SetBoolField(TEXT("rotating"), bRotating);
	S->SetNumberField(TEXT("files"), R.Files);
	S->SetNumberField(TEXT("expected"), R.Expected);
	S->SetNumberField(TEXT("bytes"), (double)R.Bytes);
	S->SetBoolField(TEXT("complete"), R.bComplete);
	S->SetStringField(TEXT("fingerprint"), Fingerprint);
	{
		TArray<TSharedPtr<FJsonValue>> CArr;
		for (const FString& Rel : Set.Copied)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("path"), Rel);
			if (const FString* H = Set.Md5.Find(Rel)) O->SetStringField(TEXT("md5"), *H);
			CArr.Add(MakeShared<FJsonValueObject>(O));
		}
		S->SetArrayField(TEXT("contents"), CArr);
		TArray<TSharedPtr<FJsonValue>> MArr;
		for (const FString& Rel : Set.Failed) MArr.Add(MakeShared<FJsonValueString>(Rel));
		S->SetArrayField(TEXT("missing"), MArr);
	}
	if (!WriteJson(FPaths::Combine(R.Dir, TEXT("snapshot.json")), S))
	{
		// Without its own record a snapshot cannot be verified or reconciled
		// against, so it is not one. The copied files stay where they are and
		// are named, because they may still be the only copy of something.
		R.bComplete = false;
		R.Why = FString::Printf(
			TEXT("the %d copied file(s) are at %s, but the snapshot record beside them could not be written, so this snapshot cannot be restored from"),
			R.Files, *R.Dir);
		UE_LOG(LogBF6Project, Warning, TEXT("project '%s': %s"), *Save, *R.Why);
		return R;
	}

	// Only the rotating ones age out, and only COMPLETE ones count towards the
	// limit: an incomplete snapshot must not push a good one out of the
	// rotation. A pre-import snapshot is the one thing standing between an
	// overwrite and the work it replaced, so it is kept until the user deletes
	// it.
	if (bRotating)
	{
		TArray<FString> Dirs;
		FM.FindFiles(Dirs, *(Root / TEXT("*")), false, true);
		TArray<TPair<FDateTime, FString>> Rot;
		for (const FString& D : Dirs)
		{
			const TSharedPtr<FJsonObject> Meta = ReadJson(FPaths::Combine(Root, D, TEXT("snapshot.json")));
			bool bRot = false, bDone = false;
			if (!Meta.IsValid() || !Meta->TryGetBoolField(TEXT("rotating"), bRot) || !bRot) continue;
			if (!Meta->TryGetBoolField(TEXT("complete"), bDone) || !bDone) continue;
			Rot.Add(TPair<FDateTime, FString>(FM.GetTimeStamp(*FPaths::Combine(Root, D)), D));
		}
		Rot.Sort([](const TPair<FDateTime, FString>& A, const TPair<FDateTime, FString>& B) { return A.Key > B.Key; });
		const int32 Keep = BackupMax();
		for (int32 i = Keep; i < Rot.Num(); ++i) FM.DeleteDirectory(*FPaths::Combine(Root, Rot[i].Value), false, true);
	}

	if (R.bComplete)
	{
		UE_LOG(LogBF6Project, Display, TEXT("project '%s': snapshot %s (%d file(s), %.1f MB, %s)"),
			*Save, *R.Stamp, R.Files, (double)R.Bytes / (1024.0 * 1024.0),
			bRotating ? TEXT("rotating") : *Why);
	}
	else
	{
		UE_LOG(LogBF6Project, Warning,
			TEXT("project '%s': snapshot %s is INCOMPLETE - %d of %d file(s) were copied. It is kept and marked incomplete, and a restore from it will never delete anything. First missing: %s"),
			*Save, *R.Stamp, R.Files, R.Expected,
			R.Missing.Num() ? *R.Missing[0] : TEXT("(none named)"));
	}
	return R;
}

FString BF6Project::SnapshotProject(const FString& Save, const FString& Why)
{
	return SnapshotProjectChecked(Save, Why).Stamp;
}

void BF6Project::LogBackups(const FString& Save)
{
	const FString Root = BackupRoot(DirFor(Save));
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("*")), false, true);
	if (Dirs.Num() == 0) { UE_LOG(LogBF6Project, Display, TEXT("project '%s' has no snapshots yet."), *Save); return; }
	Dirs.Sort();
	UE_LOG(LogBF6Project, Display, TEXT("project '%s': %d snapshot(s), %d rotating slots"), *Save, Dirs.Num(), BackupMax());
	for (const FString& D : Dirs)
	{
		const TSharedPtr<FJsonObject> S = ReadJson(FPaths::Combine(Root, D, TEXT("snapshot.json")));
		FString When, Why; bool bRot = false, bDone = false; double Files = 0, Expected = 0;
		if (S.IsValid())
		{
			S->TryGetStringField(TEXT("when"), When);
			S->TryGetStringField(TEXT("why"), Why);
			S->TryGetBoolField(TEXT("rotating"), bRot);
			S->TryGetBoolField(TEXT("complete"), bDone);
			S->TryGetNumberField(TEXT("files"), Files);
			S->TryGetNumberField(TEXT("expected"), Expected);
		}
		// A snapshot that is missing files says so on its own line. It is the
		// one fact somebody choosing which one to restore actually needs.
		UE_LOG(LogBF6Project, Display, TEXT("  %-24s %-9s %-10s %4d file(s)%s  %s  %s"),
			*D, bRot ? TEXT("rotating") : TEXT("kept"),
			S.IsValid() ? (bDone ? TEXT("complete") : TEXT("INCOMPLETE")) : TEXT("no record"),
			(int32)Files,
			(!bDone && Expected > 0) ? *FString::Printf(TEXT(" of %d"), (int32)Expected) : TEXT(""),
			*When, *Why);
	}
	UE_LOG(LogBF6Project, Display, TEXT("BF6.Project.Backups restore <stamp> puts one back, taking a kept snapshot of what it replaces."));
}

// ---------------------------------------------------------------------------
// RESTORE.
//
// This used to copy the snapshot's files over whatever was in the project and
// report the number that arrived. Two things were wrong with that, and both of
// them are the kind of wrong that reads as success:
//
//   IT OVERLAID. Restoring an older snapshot left every file added since
//   sitting beside the older ones, so the result was not the project as it was
//   at that moment; it was a mixture that had never existed. Half of a
//   two-file change coming back is worse than neither half.
//
//   IT COUNTED. Nine files copied out of twelve returned nine and true, and
//   "restored 9 file(s)" is what the user saw.
//
// So it works the way the update applier does, on three facts kept apart:
//
//   the snapshot        is it COMPLETE by its own recorded file list
//   the guard           is the kept snapshot of what we are about to replace
//                       VERIFIED COMPLETE
//   what we touched     which files were actually written
//
// Only a complete snapshot AND a verified guard earns the right to delete
// anything. Without both, files are copied in and nothing is removed, and the
// result says plainly that the project now holds a mixture. Deletion is bounded
// to OwnedFiles either way: a file outside that boundary is not ours to remove.
// ---------------------------------------------------------------------------
BF6Project::FRestore BF6Project::RestoreBackupChecked(const FString& Save, const FString& Stamp)
{
	FRestore R;
	IFileManager& FM = IFileManager::Get();
	const FString Dir = DirFor(Save);
	const FString From = FPaths::Combine(BackupRoot(Dir), Stamp);
	if (Dir.IsEmpty() || !FPaths::DirectoryExists(Dir))
	{
		R.What = FString::Printf(TEXT("Project '%s' is not on disk, so there is nothing to restore into."), *Save);
		return R;
	}
	if (!FPaths::DirectoryExists(From))
	{
		R.What = FString::Printf(TEXT("There is no snapshot called '%s' in project '%s'. BF6.Project.Backups lists them."), *Stamp, *Save);
		return R;
	}

	// WHAT THE SNAPSHOT SAYS IT HOLDS. Without its own record there is no way
	// to tell a whole snapshot from a truncated one, so a snapshot with no
	// record is restored as a copy and never as a replacement.
	const TSharedPtr<FJsonObject> Meta = ReadJson(FPaths::Combine(From, TEXT("snapshot.json")));
	bool bSnapshotComplete = false;
	TArray<FString> Want;
	TMap<FString, FString> WantMd5;
	if (Meta.IsValid())
	{
		Meta->TryGetBoolField(TEXT("complete"), bSnapshotComplete);
		const TArray<TSharedPtr<FJsonValue>>* CArr = nullptr;
		if (Meta->TryGetArrayField(TEXT("contents"), CArr) && CArr)
			for (const TSharedPtr<FJsonValue>& V : *CArr)
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				FString P, H;
				if (!O.IsValid() || !O->TryGetStringField(TEXT("path"), P)) continue;
				O->TryGetStringField(TEXT("md5"), H);
				Want.AddUnique(P);
				if (!H.IsEmpty()) WantMd5.Add(P, H);
			}
	}
	if (Want.Num() == 0)
	{
		// A snapshot written by an older build of the tool has no contents
		// list. Its files are still real, so they are read off disk and used,
		// but it can never be called complete and so can never delete.
		bSnapshotComplete = false;
		TArray<FString> Files;
		FM.FindFilesRecursive(Files, *From, TEXT("*"), true, false);
		for (const FString& F : Files)
		{
			FString Rel = F;
			FPaths::MakePathRelativeTo(Rel, *(From / TEXT("")));
			Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Rel == TEXT("snapshot.json")) continue;
			Want.AddUnique(Rel);
		}
		Want.Sort();
	}
	R.Expected = Want.Num();
	if (Want.Num() == 0)
	{
		R.What = FString::Printf(TEXT("Snapshot '%s' holds no files, so nothing was changed."), *Stamp);
		return R;
	}

	// The owned set as it is NOW, so the reconcile below knows what would be
	// left behind. Read before anything is written.
	const TArray<FString> Before = OwnedFiles(Dir);

	// THE GUARD. What is about to be replaced becomes a kept snapshot of its
	// own first, and its completeness decides what this call is allowed to do.
	// A project that owns nothing yet - restoring into an empty folder - has
	// nothing to guard, and that counts as verified rather than as a failure.
	bool bGuardComplete = true;
	if (Before.Num() > 0)
	{
		const FSnapshot Guard = SnapshotProjectChecked(Save, TEXT("before restoring ") + Stamp);
		R.Guard = Guard.Stamp;
		bGuardComplete = Guard.bComplete;
		// A PARTIAL GUARD IS NOT A GUARD.
		//
		// This used to abort only when the guard had no stamp at all. A guard
		// where one copy succeeded and another failed still gets a stamp, and
		// the restore then overwrote files whose only copy had just failed to
		// be made. Completeness gated the DELETE and not the OVERWRITE, which
		// is the half that loses the work.
		if (Guard.Stamp.IsEmpty() || !bGuardComplete)
		{
			R.What = FString::Printf(
				TEXT("Nothing was restored: the current state of project '%s' could not be completely backed up first (%s). ")
				TEXT("Free some disk space or close whatever is holding that folder open, then try again."),
				*Save, Guard.Why.IsEmpty()
					? (Guard.Stamp.IsEmpty() ? TEXT("nothing could be copied") : TEXT("only part of it could be copied"))
					: *Guard.Why);
			UE_LOG(LogBF6Project, Warning, TEXT("%s"), *R.What);
			return R;
		}
	}

	// AND THE SNAPSHOT ITSELF IS CHECKED BEFORE ANYTHING IS WRITTEN.
	//
	// The read-back below verified each file AFTER copying it over the
	// project, which is too late: a corrupt snapshot had already replaced good
	// work by the time it was caught. Every file it claims to hold is verified
	// against its recorded hash first, and a snapshot that fails is refused
	// with the project untouched.
	{
		TArray<FString> BadSource;
		for (const FString& Rel : Want)
		{
			const FString* Expect = WantMd5.Find(Rel);
			if (!Expect || Expect->IsEmpty()) continue;      // older snapshot, nothing to check against
			const FString Src = FPaths::Combine(From, Rel);
			if (!FPaths::FileExists(Src) || HashFile(Src) != *Expect) { BadSource.AddUnique(Rel); }
		}
		if (BadSource.Num() > 0)
		{
			R.Failed = BadSource;
			R.What = FString::Printf(
				TEXT("Nothing was restored: %d file(s) in snapshot '%s' are missing or do not match what it recorded (%s%s). ")
				TEXT("The project has not been touched."),
				BadSource.Num(), *Stamp, *BadSource[0],
				BadSource.Num() > 1 ? TEXT(", and others") : TEXT(""));
			UE_LOG(LogBF6Project, Warning, TEXT("%s"), *R.What);
			return R;
		}
	}

	FCopySet Set;
	CopyOwned(From, Dir, Want, Set);
	R.Restored = Set.Copied.Num();
	R.Failed = Set.Failed;

	// Every restored file read back against the hash the snapshot recorded.
	// A copy that reported success and a file that is whole are different
	// claims, and only the second one may be announced to a user.
	for (const FString& Rel : Set.Copied)
	{
		const FString* Expect = WantMd5.Find(Rel);
		if (!Expect || Expect->IsEmpty()) continue;
		if (HashFile(FPaths::Combine(Dir, Rel)) != *Expect)
		{
			R.Failed.AddUnique(Rel);
			--R.Restored;
		}
	}

	const bool bWhole = (R.Failed.Num() == 0) && (R.Restored == R.Expected);

	// RECONCILE, and only when everything above is true. Anything the project
	// owns that this snapshot did not have is work done after it was taken, so
	// removing it is what makes the restore the coherent older version rather
	// than a mixture. Getting here needs a complete snapshot, a verified guard
	// and a clean copy, because every one of those is a way for this deletion
	// to be deleting the only copy of something.
	if (bWhole && bSnapshotComplete && bGuardComplete)
	{
		const TSet<FString> Kept(Want);
		for (const FString& Rel : Before)
		{
			if (Kept.Contains(Rel)) continue;
			if (FM.Delete(*FPaths::Combine(Dir, Rel), false, true, true)) { ++R.Removed; }
			else
			{
				// A file that could not be removed leaves the project a mixture
				// of two versions, which is exactly what this phase exists to
				// prevent. Reported rather than counted as a clean restore.
				R.Failed.AddUnique(Rel);
				UE_LOG(LogBF6Project, Warning,
					TEXT("restore: %s is left over from the newer version and could not be removed."), *Rel);
			}
		}
	}

	// Recomputed AFTER the reconcile: a file that could not be removed is a
	// failure of this restore, and assigning bWhole here would have thrown that
	// away a line later.
	const bool bClean = bWhole && R.Failed.Num() == 0;
	R.bRestored = bClean;
	R.bPartial = !bClean && R.Restored > 0;

	// bClean, NOT bWhole. bWhole was measured before the reconcile, so a file
	// that could not be deleted left the project a mixture of two versions and
	// still printed the sentence that says it was restored. The flags below it
	// already said otherwise; only the words the user reads were wrong, which
	// is the half that matters when somebody is deciding whether to carry on
	// working in that project.
	if (bClean && bSnapshotComplete && bGuardComplete)
	{
		R.What = FString::Printf(
			TEXT("Restored project '%s' from snapshot %s: %d file(s) back, %d removed that the snapshot did not have. What it replaced is kept as %s."),
			*Save, *Stamp, R.Restored, R.Removed, R.Guard.IsEmpty() ? TEXT("the previous snapshot") : *R.Guard);
		UE_LOG(LogBF6Project, Display, TEXT("%s"), *R.What);
	}
	else if (bClean)
	{
		// Every file arrived, but this is not the whole project as it was:
		// either the snapshot never held all of it or its replacement could not
		// be fully backed up, and in both cases nothing may be deleted. Say
		// that, rather than letting a full count imply a full restore.
		R.bRestored = false;
		R.bPartial = true;
		R.What = FString::Printf(
			TEXT("Copied %d file(s) from snapshot %s into project '%s', and deleted nothing. %s, so anything added since is still there beside the restored files. Check the project before working in it."),
			R.Restored, *Stamp, *Save,
			!bSnapshotComplete
				? TEXT("That snapshot is not a complete copy of the project")
				: TEXT("The backup of what this replaced is not a complete copy"));
		UE_LOG(LogBF6Project, Warning, TEXT("%s"), *R.What);
	}
	else
	{
		R.What = FString::Printf(
			TEXT("Snapshot %s was only partly restored into project '%s': %d of %d file(s) came back and nothing was deleted. The project now holds a mixture. What it replaced is kept as %s, and the snapshot is still at %s. First file that failed: %s"),
			*Stamp, *Save, R.Restored, R.Expected,
			R.Guard.IsEmpty() ? TEXT("(no backup was taken)") : *R.Guard, *From,
			R.Failed.Num() ? *R.Failed[0] : TEXT("(none named)"));
		UE_LOG(LogBF6Project, Error, TEXT("%s"), *R.What);
	}
	return R;
}

bool BF6Project::RestoreBackup(const FString& Save, const FString& Stamp)
{
	// TRUE MEANS THE PROJECT IS NOW THAT SNAPSHOT. A partial restore is not a
	// smaller success, so it does not get to return one.
	return RestoreBackupChecked(Save, Stamp).bRestored;
}

// ---------------------------------------------------------------------------
// ORIGIN STAMP.
// ---------------------------------------------------------------------------
void BF6Project::NoteArtefactWritten(const FString& Save, const FString& Rel, const FString& By)
{
	const FString Dir = DirFor(Save);
	const FString Path = ManifestPath(Dir);
	TSharedPtr<FJsonObject> M = ReadJson(Path);
	if (!M.IsValid()) return;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!M->TryGetArrayField(TEXT("artefacts"), Arr) || !Arr) return;
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FString P;
		if (!O.IsValid() || !O->TryGetStringField(TEXT("path"), P) || P != Rel) continue;
		TSharedPtr<FJsonObject> Og = MakeShared<FJsonObject>();
		Og->SetStringField(TEXT("by"), By);
		Og->SetStringField(TEXT("at"), NowIso());
		O->SetObjectField(TEXT("origin"), Og);
		WriteJson(Path, M);
		return;
	}
}

TArray<BF6Project::FArtefactState> BF6Project::ArtefactStates(const FString& Save)
{
	TArray<FArtefactState> Out;
	const TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(DirFor(Save)));
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!M.IsValid() || !M->TryGetArrayField(TEXT("artefacts"), Arr) || !Arr) return Out;
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) continue;
		FArtefactState S;
		O->TryGetStringField(TEXT("path"), S.Rel);
		O->TryGetStringField(TEXT("kind"), S.Kind);
		O->TryGetStringField(TEXT("level"), S.Level);
		O->TryGetStringField(TEXT("canonical"), S.Canonical);
		const TSharedPtr<FJsonObject>* Sy = nullptr;
		if (O->TryGetObjectField(TEXT("sync"), Sy) && Sy)
		{
			(*Sy)->TryGetStringField(TEXT("hash"), S.SyncHash);
			(*Sy)->TryGetStringField(TEXT("side"), S.SyncSide);
			(*Sy)->TryGetStringField(TEXT("at"), S.SyncAt);
		}
		const TSharedPtr<FJsonObject>* Og = nullptr;
		if (O->TryGetObjectField(TEXT("origin"), Og) && Og)
		{
			(*Og)->TryGetStringField(TEXT("by"), S.OriginBy);
			(*Og)->TryGetStringField(TEXT("at"), S.OriginAt);
		}
		Out.Add(MoveTemp(S));
	}
	return Out;
}

// ---------------------------------------------------------------------------
// COMPARE.
//
// Five signals, each with a weight. A signal neither side can answer earns
// nothing AND is taken out of the total, so the confidence is always "of what
// could be checked, this much agreed" rather than a number quietly diluted by
// data nobody has. Every signal that counted is listed with what it found.
// ---------------------------------------------------------------------------
TArray<BF6Project::FMatch> BF6Project::Compare(const FString& Save)
{
	TArray<FMatch> Out;
	const FString Dir = DirFor(Save);
	if (Dir.IsEmpty() || !FPaths::DirectoryExists(Dir)) return Out;

	const FSideFacts Mine = ProjectFacts(Dir);
	FString LinkedId;
	{
		const TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(Dir));
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (M.IsValid() && M->TryGetObjectField(TEXT("experience"), E) && E) (*E)->TryGetStringField(TEXT("id"), LinkedId);
	}

	for (const FCandidate& C : Candidates())
	{
		FMatch M;
		M.Id = C.Id;
		M.Name = C.Name.IsEmpty() ? C.Id : C.Name;

		if (!LinkedId.IsEmpty() && LinkedId == C.Id)
		{
			M.Confidence = 100.f;
			M.Reasons.Add(TEXT("this project is already linked to this experience"));
			Out.Add(M);
			continue;
		}

		float Earned = 0.f, Attainable = 0.f;

		// ---- 1. the maps, weight 40 ----
		TArray<FString> Matched;                 // project levels that a candidate file covers
		TMap<FString, FString> FileForLevel;
		if (Mine.Levels.Num() > 0 && C.SpatialFiles.Num() > 0)
		{
			Attainable += 40.f;
			for (const FString& L : Mine.Levels)
				for (const FString& F : C.SpatialFiles)
					if (F.StartsWith(L + TEXT("_"), ESearchCase::IgnoreCase) || F.StartsWith(L + TEXT("."), ESearchCase::IgnoreCase))
					{
						Matched.AddUnique(L);
						FileForLevel.Add(L, F);
						break;
					}
			const int32 Uni = Mine.Levels.Num() + C.SpatialFiles.Num() - Matched.Num();
			const float J = Uni > 0 ? (float)Matched.Num() / (float)Uni : 0.f;
			Earned += 40.f * J;
			M.Reasons.Add(FString::Printf(TEXT("maps: %d of this project's %d also in the experience's %d (%.0f%% overlap)"),
				Matched.Num(), Mine.Levels.Num(), C.SpatialFiles.Num(), J * 100.f));
		}
		else
		{
			M.NotComparable.Add(Mine.Levels.Num() == 0
				? TEXT("maps: this project has no saved map yet")
				: TEXT("maps: nothing has been fetched for this experience, so it has no map files on disk"));
		}

		if (Matched.Num() == 0)
		{
			// No shared map means the per-map signals cannot run at all. Say so
			// rather than letting them contribute a silent zero.
			M.NotComparable.Add(TEXT("object ids and object counts: no map is shared, so there is nothing to line up"));
		}
		else
		{
			// ---- 2. the ObjIds, weight 30 ----
			// ---- 3. how many objects carry one, weight 15 ----
			TSet<int32> MineAll, TheirsAll;
			int32 MineCount = 0, TheirsCount = 0;
			for (const FString& L : Matched)
			{
				if (const TSet<int32>* S = Mine.IdsByLevel.Find(L)) MineAll.Append(*S);
				if (const int32* N = Mine.CountByLevel.Find(L)) MineCount += *N;
				TSet<int32> Ids; int32 N2 = 0;
				ReadSpatialIds(ReadJson(FPaths::Combine(C.Dir, FileForLevel[L])), Ids, N2);
				TheirsAll.Append(Ids);
				TheirsCount += N2;
			}
			if (MineAll.Num() > 0 && TheirsAll.Num() > 0)
			{
				Attainable += 30.f;
				const float J = Jaccard(MineAll, TheirsAll);
				Earned += 30.f * J;
				int32 Inter = 0;
				for (const int32 X : MineAll) if (TheirsAll.Contains(X)) ++Inter;
				M.Reasons.Add(FString::Printf(TEXT("object ids: %d of this project's %d also in the experience's %d (%.0f%% overlap)"),
					Inter, MineAll.Num(), TheirsAll.Num(), J * 100.f));
			}
			else
			{
				M.NotComparable.Add(TEXT("object ids: one side has no object with an ObjId on the shared maps"));
			}

			if (MineCount > 0 && TheirsCount > 0)
			{
				Attainable += 15.f;
				const float Rel = FMath::Abs((float)(MineCount - TheirsCount)) / (float)FMath::Max(MineCount, TheirsCount);
				const float Score = Rel <= 0.001f ? 1.f : (Rel <= 0.02f ? 0.8f : (Rel <= 0.10f ? 0.4f : 0.f));
				Earned += 15.f * Score;
				M.Reasons.Add(FString::Printf(TEXT("object counts on the shared maps: %d here, %d there"), MineCount, TheirsCount));
			}
			else
			{
				M.NotComparable.Add(TEXT("object counts: one side has nothing counted on the shared maps"));
			}
		}

		// ---- 4. the workspace rule and subroutine names, weight 10 ----
		{
			TSet<FString> Theirs;
			const TSharedPtr<FJsonObject> W = ReadJson(FPaths::Combine(C.Dir, TEXT("rules.blocks.json")));
			if (W.IsValid()) CollectNamesRoot(W, Theirs);
			if (Mine.bHaveWorkspace && Theirs.Num() > 0)
			{
				Attainable += 10.f;
				const float J = JaccardS(Mine.WorkspaceNames, Theirs);
				Earned += 10.f * J;
				M.Reasons.Add(FString::Printf(TEXT("block rule and subroutine names: %.0f%% of the two sets agree (%d here, %d there)"),
					J * 100.f, Mine.WorkspaceNames.Num(), Theirs.Num()));
			}
			else
			{
				M.NotComparable.Add(Mine.bHaveWorkspace
					? TEXT("block names: this experience has no captured workspace on disk")
					: TEXT("block names: this project has no unreal/blockly/workspace.json yet"));
			}
		}

		// ---- 5. the script file names, weight 5 ----
		{
			TSet<FString> Theirs;
			TArray<FString> Ts;
			IFileManager::Get().FindFiles(Ts, *(C.Dir / TEXT("*.ts")), true, false);
			for (const FString& F : Ts) Theirs.Add(F);
			if (FPaths::FileExists(FPaths::Combine(C.Dir, TEXT("strings.json")))) Theirs.Add(TEXT("strings.json"));
			if (Mine.bHaveScript && Theirs.Num() > 0)
			{
				Attainable += 5.f;
				const float J = JaccardS(Mine.ScriptFiles, Theirs);
				Earned += 5.f * J;
				M.Reasons.Add(FString::Printf(TEXT("script file names: %.0f%% of the two sets agree"), J * 100.f));
			}
			else
			{
				M.NotComparable.Add(TEXT("script file names: one side has no script attachment on disk"));
			}
		}

		M.Confidence = Attainable > 0.f ? (Earned / Attainable) * 100.f : 0.f;
		if (Attainable <= 0.f) M.Reasons.Add(TEXT("nothing about these two could be compared"));
		Out.Add(M);
	}

	Out.Sort([](const FMatch& A, const FMatch& B) { return A.Confidence > B.Confidence; });
	return Out;
}

void BF6Project::LogCompare(const FString& Save)
{
	const TArray<FMatch> Ms = Compare(Save);
	if (Ms.Num() == 0)
	{
		UE_LOG(LogBF6Project, Warning, TEXT("compare '%s': nothing to compare against. Link a Portal account and import an experience first."), *Save);
		return;
	}
	UE_LOG(LogBF6Project, Display, TEXT("compare '%s' against %d experience(s):"), *Save, Ms.Num());
	const int32 N = FMath::Min(Ms.Num(), 8);
	for (int32 i = 0; i < N; ++i)
	{
		const FMatch& M = Ms[i];
		UE_LOG(LogBF6Project, Display, TEXT("  %5.1f%%  %s  [%s]"), M.Confidence, *M.Name, *M.Id.Left(8));
		for (const FString& R : M.Reasons)        UE_LOG(LogBF6Project, Display, TEXT("           + %s"), *R);
		for (const FString& R : M.NotComparable)  UE_LOG(LogBF6Project, Display, TEXT("           ? %s"), *R);
	}
	if (Ms.Num() > N) UE_LOG(LogBF6Project, Display, TEXT("  ... and %d more, all below %.1f%%"), Ms.Num() - N, Ms[N - 1].Confidence);

	// The per-artefact table. The whole point of holding sync state per file is
	// that this reads one line per thing that can diverge, so an eleven map
	// project shows which ONE map is out of step instead of a verdict on the
	// experience.
	const TArray<FArtefactState> States = ArtefactStates(Save);
	if (States.Num() == 0) return;
	UE_LOG(LogBF6Project, Display, TEXT("artefacts of '%s':"), *Save);
	UE_LOG(LogBF6Project, Display, TEXT("  %-44s %-10s %-9s %-8s %s"), TEXT("file"), TEXT("kind"), TEXT("content"), TEXT("written"), TEXT("in step with"));
	for (const FArtefactState& S : States)
	{
		FString Step;
		if (S.SyncHash.IsEmpty())      Step = TEXT("never synced");
		else if (S.SyncHash == S.Canonical) Step = FString::Printf(TEXT("yes, %s side, %s"), *S.SyncSide, *S.SyncAt);
		else                            Step = FString::Printf(TEXT("no, changed here since %s"), *S.SyncAt);
		UE_LOG(LogBF6Project, Display, TEXT("  %-44s %-10s %-9s %-8s %s"),
			*S.Rel, *S.Kind, *S.Canonical.Left(8), S.OriginBy.IsEmpty() ? TEXT("unknown") : *S.OriginBy, *Step);
	}
	UE_LOG(LogBF6Project, Display, TEXT("BF6.Project.Sync decides the ones that are out of step, one at a time."));
}

void BF6Project::CompareAll()
{
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(SavesRoot() / TEXT("*")), false, true);
	if (Dirs.Num() == 0) { UE_LOG(LogBF6Project, Warning, TEXT("no projects on disk yet.")); return; }
	UE_LOG(LogBF6Project, Display, TEXT("%d project(s) against the experiences on disk:"), Dirs.Num());
	for (const FString& S : Dirs)
	{
		const TArray<FMatch> Ms = Compare(S);
		if (Ms.Num() == 0) { UE_LOG(LogBF6Project, Display, TEXT("  %-32s no candidates"), *S); continue; }
		UE_LOG(LogBF6Project, Display, TEXT("  %-32s %5.1f%%  %s"), *S, Ms[0].Confidence, *Ms[0].Name);
	}
	UE_LOG(LogBF6Project, Display, TEXT("BF6.Project.Compare <save> prints the reasons for one of them."));
}

// ---------------------------------------------------------------------------
// SYNC: the three way decision run, one artefact at a time.
//
// The site's copy, our copy, and the hash the two last agreed on. Equal to the
// last agreement on both sides means nothing happened and nothing is said.
// Changed on one side offers that side. Changed on BOTH is the only case that
// asks, and it asks with the real counts from an ObjId keyed diff, because
// nobody can answer "the save or the spatial" without them.
//
// ADD-ON CONTENT AND OBJECTS WITH NO PORTAL TYPE ARE NOT IN A SPATIAL AT ALL,
// on either side. They are never reported as missing on the site, because the
// comparison is spatial against spatial and they were never in the question.
// ---------------------------------------------------------------------------
void BF6Project::Sync(const FString& Save)
{
	const FString Dir = DirFor(Save);
	if (Dir.IsEmpty() || !FPaths::DirectoryExists(Dir))
	{
		UE_LOG(LogBF6Project, Warning, TEXT("sync: there is no project called '%s'."), *Save);
		return;
	}
	const FString ManPath = ManifestPath(Dir);
	TSharedPtr<FJsonObject> Man = ReadJson(ManPath);
	FString ExpId;
	{
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (Man.IsValid() && Man->TryGetObjectField(TEXT("experience"), E) && E) (*E)->TryGetStringField(TEXT("id"), ExpId);
	}
	if (ExpId.IsEmpty())
	{
		UE_LOG(LogBF6Project, Warning, TEXT("sync: '%s' is not linked to an experience. BF6.Project.Compare finds a candidate, BF6.Project.Link attaches it."), *Save);
		return;
	}
	const FString Cache = FPaths::Combine(PortalCacheRoot(), ExpId);
	if (!FPaths::DirectoryExists(Cache))
	{
		UE_LOG(LogBF6Project, Warning, TEXT("sync: nothing has been fetched for experience %s, so there is no site copy to compare with."), *ExpId.Left(8));
		return;
	}

	TArray<FString> SiteSpatials;
	IFileManager::Get().FindFiles(SiteSpatials, *(Cache / TEXT("*.spatial.json")), true, false);

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Man.IsValid() || !Man->TryGetArrayField(TEXT("artefacts"), Arr) || !Arr) return;

	int32 Quiet = 0, Asked = 0, Took = 0, Kept = 0;

	// TAKING THE SITE'S COPY WRITES OVER YOURS, and the dialog that asks has
	// already promised the version you lose is kept as a snapshot. That
	// snapshot used to be fired and forgotten: one that failed still let the
	// overwrite go ahead, so the promise was made and then not kept, and the
	// only copy of the user's work was the one just replaced.
	//
	// Now the file being overwritten has to be verifiably IN the snapshot
	// before it is overwritten. Not the whole project - just this file, which
	// is the one thing the promise was about.
	auto TakeTheirs = [&Save, &Dir](const FString& Rel, const FString& SitePath) -> bool
	{
		const FSnapshot Guard = SnapshotProjectChecked(Save,
			FString::Printf(TEXT("before taking the site's %s"), *Rel));
		if (Guard.Stamp.IsEmpty() || Guard.Missing.Contains(Rel))
		{
			UE_LOG(LogBF6Project, Warning,
				TEXT("  %-44s NOT taken: your version could not be put in a snapshot first (%s), and it was not going to be overwritten without one. Your file is untouched."),
				*Rel, Guard.Stamp.IsEmpty()
					? (Guard.Why.IsEmpty() ? TEXT("no reason was given") : *Guard.Why)
					: TEXT("it did not copy into the snapshot"));
			return false;
		}
		if (IFileManager::Get().Copy(*FPaths::Combine(Dir, Rel), *SitePath) != COPY_OK)
		{
			UE_LOG(LogBF6Project, Warning,
				TEXT("  %-44s the site's version could not be written, so yours is still in place. Snapshot %s holds it as well."),
				*Rel, *Guard.Stamp);
			return false;
		}
		NoteArtefactWritten(Save, Rel, TEXT("site"));
		UE_LOG(LogBF6Project, Display, TEXT("  %-44s took the site's version. Yours is in snapshot %s."), *Rel, *Guard.Stamp);
		return true;
	};

	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) continue;
		FString Rel, Kind, Level, Ours;
		O->TryGetStringField(TEXT("path"), Rel);
		O->TryGetStringField(TEXT("kind"), Kind);
		O->TryGetStringField(TEXT("level"), Level);
		O->TryGetStringField(TEXT("canonical"), Ours);
		if (Kind == TEXT("session") || Kind == TEXT("tscn") || Kind == TEXT("bundle") || Kind == TEXT("thumbnail")) continue;

		// Where the SAME artefact lives on the site's side.
		FString SitePath;
		if (Kind == TEXT("spatial"))
		{
			const FString Leaf = FPaths::GetCleanFilename(Rel);
			for (const FString& F : SiteSpatials)
				if (F == Leaf || (!Level.IsEmpty() && F.StartsWith(Level + TEXT("_"), ESearchCase::IgnoreCase))) { SitePath = FPaths::Combine(Cache, F); break; }
		}
		else if (Kind == TEXT("workspace")) SitePath = FPaths::Combine(Cache, TEXT("rules.blocks.json"));
		else if (Kind == TEXT("script") || Kind == TEXT("strings")) SitePath = FPaths::Combine(Cache, FPaths::GetCleanFilename(Rel));

		if (SitePath.IsEmpty() || !FPaths::FileExists(SitePath))
		{
			UE_LOG(LogBF6Project, Display, TEXT("  %-44s no site copy on disk, left alone"), *Rel);
			continue;
		}

		const FString Theirs = CanonicalHash(SitePath, Kind);
		FString Last, DecO, DecT, OriginBy;
		const TSharedPtr<FJsonObject>* Sy = nullptr;
		if (O->TryGetObjectField(TEXT("sync"), Sy) && Sy)
		{
			(*Sy)->TryGetStringField(TEXT("hash"), Last);
			(*Sy)->TryGetStringField(TEXT("declinedOurs"), DecO);
			(*Sy)->TryGetStringField(TEXT("declinedTheirs"), DecT);
		}
		const TSharedPtr<FJsonObject>* Og = nullptr;
		if (O->TryGetObjectField(TEXT("origin"), Og) && Og) (*Og)->TryGetStringField(TEXT("by"), OriginBy);

		auto WriteSync = [&](const FString& Hash, const TCHAR* Side, const FString& InDecO, const FString& InDecT)
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("hash"), Hash);
			S->SetStringField(TEXT("side"), Side);
			S->SetStringField(TEXT("at"), NowIso());
			if (!InDecO.IsEmpty()) S->SetStringField(TEXT("declinedOurs"), InDecO);
			if (!InDecT.IsEmpty()) S->SetStringField(TEXT("declinedTheirs"), InDecT);
			O->SetObjectField(TEXT("sync"), S);
			WriteJson(ManPath, Man);
		};

		// ---- 1. the two sides agree ----
		if (!Ours.IsEmpty() && Ours == Theirs)
		{
			if (Last != Ours) WriteSync(Ours, TEXT("both"), FString(), FString());
			++Quiet;
			continue;
		}

		// The diff, computed once, because both the offer and the conflict need it.
		FSpatialDiff D;
		D.Level = Level;
		if (Kind == TEXT("spatial"))
		{
			TMap<int32, FSpatialEntry> A, B;
			ReadSpatialEntries(ReadJson(FPaths::Combine(Dir, Rel)), A, D.UntrackedHere);
			ReadSpatialEntries(ReadJson(SitePath), B, D.UntrackedThere);
			for (const auto& It : A)
			{
				const FSpatialEntry* T = B.Find(It.Key);
				if (!T) { ++D.Removed; continue; }
				if (!It.Value.Pos.Equals(T->Pos, 0.05)) ++D.Moved;
				else if (It.Value.Props != T->Props) ++D.Changed;
			}
			for (const auto& It : B) if (!A.Contains(It.Key)) ++D.Added;
		}
		const FString DiffLine = Kind == TEXT("spatial")
			? FString::Printf(TEXT("%d only on the site, %d only here, %d moved, %d retuned; %d object(s) here and %d there carry no ObjId and cannot be tracked"),
				D.Added, D.Removed, D.Moved, D.Changed, D.UntrackedHere, D.UntrackedThere)
			: FString(TEXT("the two texts differ"));

		const bool bOursMoved = Ours != Last;
		const bool bTheirsMoved = Theirs != Last;

		// ---- 2. changed only on the site ----
		if (!Last.IsEmpty() && !bOursMoved && bTheirsMoved)
		{
			// The ping pong guard: an artefact the tool itself just wrote is
			// never re-imported by the sync that follows it.
			if (OriginBy == TEXT("tool"))
			{
				UE_LOG(LogBF6Project, Display, TEXT("  %-44s the site moved, but the tool wrote this file last, so it is left alone"), *Rel);
				continue;
			}
			++Asked;
			const FString Q = FString::Printf(TEXT("'%s' changed on Portal and not here.\n\n%s\n\n%s\n\nTake the site's version? Yours is kept as a snapshot either way."),
				*Rel, *DiffLine, *Level);
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Q)) == EAppReturnType::Yes)
			{
				if (TakeTheirs(Rel, SitePath)) { WriteSync(Theirs, TEXT("site"), FString(), FString()); ++Took; }
			}
			else
			{
				WriteSync(Last, TEXT("tool"), Ours, Theirs);
				++Kept;
			}
			continue;
		}

		// ---- 3. changed only here ----
		if (!Last.IsEmpty() && bOursMoved && !bTheirsMoved)
		{
			UE_LOG(LogBF6Project, Display, TEXT("  %-44s changed here and not on Portal. %s"), *Rel, *DiffLine);
			UE_LOG(LogBF6Project, Display, TEXT("       Upload it on the site yourself: the tool never publishes for you. The file to upload is %s"),
				*FPaths::Combine(Dir, Rel));
			continue;
		}

		// ---- 4. changed on both, or no baseline at all ----
		if (Ours == DecO && Theirs == DecT && !DecO.IsEmpty())
		{
			UE_LOG(LogBF6Project, Display, TEXT("  %-44s still diverged, and you have already chosen. Nothing asked."), *Rel);
			continue;
		}
		++Asked;
		const FString Head = Last.IsEmpty()
			? FString::Printf(TEXT("'%s' differs from Portal, and the two have never been in step, so there is no way to tell which side moved."), *Rel)
			: FString::Printf(TEXT("'%s' changed BOTH here and on Portal since they were last in step."), *Rel);
		const FString Q = FString::Printf(TEXT("%s\n\n%s\n\nKeep YOUR version (Yes) or take PORTAL's (No)?\n\nWhichever you do not pick becomes a snapshot in this project, so this is reversible."),
			*Head, *DiffLine);
		if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Q)) == EAppReturnType::Yes)
		{
			// NEVER DELETE THE LOSER: the site's copy is filed as a kept
			// snapshot before we decide to ignore it. Whether that filing
			// worked is the whole value of the sentence, so it is checked
			// rather than assumed - the log used to name a snapshot even when
			// the stamp was empty and the copy into it had failed.
			const FString Stamp = SnapshotProject(Save, FString::Printf(TEXT("keeping ours over the site's %s"), *Rel));
			const bool bFiled = !Stamp.IsEmpty()
				&& IFileManager::Get().Copy(
					*FPaths::Combine(BackupRoot(Dir), Stamp, TEXT("site"), FPaths::GetCleanFilename(Rel)),
					*SitePath) == COPY_OK;
			WriteSync(Last, TEXT("tool"), Ours, Theirs);
			++Kept;
			if (bFiled)
			{
				UE_LOG(LogBF6Project, Display, TEXT("  %-44s kept yours. The site's copy is in snapshot %s under site/."), *Rel, *Stamp);
			}
			else
			{
				UE_LOG(LogBF6Project, Warning,
					TEXT("  %-44s kept yours, but the site's copy could NOT be filed in a snapshot. Nothing of yours was touched; the site's version is still only on the site."),
					*Rel);
			}
		}
		else
		{
			if (TakeTheirs(Rel, SitePath)) { WriteSync(Theirs, TEXT("site"), FString(), FString()); ++Took; }
		}
	}

	WriteManifest(Dir, Save, FString());
	UE_LOG(LogBF6Project, Display, TEXT("sync '%s': %d artefact(s) already in step, %d asked, %d taken from the site, %d kept as yours."),
		*Save, Quiet, Asked, Took, Kept);
}

// ---------------------------------------------------------------------------
// LINK.
// ---------------------------------------------------------------------------
bool BF6Project::Link(const FString& Save, const FString& ExperienceId)
{
	const FString Dir = DirFor(Save);
	if (Dir.IsEmpty() || !FPaths::DirectoryExists(Dir))
	{
		UE_LOG(LogBF6Project, Warning, TEXT("link: there is no project called '%s'."), *Save);
		return false;
	}
	if (ExperienceId.Len() < 32)
	{
		UE_LOG(LogBF6Project, Warning, TEXT("link: '%s' is not an experience id."), *ExperienceId);
		return false;
	}

	TArray<FString> Levels;
	GatherArtefacts(Dir, Levels);
	if (Levels.Num() == 0)
	{
		UE_LOG(LogBF6Project, Warning, TEXT("link: '%s' has no saved map yet, so there is nothing to attach the experience to."), *Save);
		return false;
	}

	// The rotation slot, when the experience's own cache knows it. Never a
	// guess: with no answer the link is recorded without a slot.
	const FString Url = UrlForId(ExperienceId);
	int32 Slot = -1;
	{
		const TArray<BF6PortalProfile::FRotationRow> Rows = BF6PortalProfile::RotationFor(ExperienceId);
		for (const BF6PortalProfile::FRotationRow& R : Rows)
			if (Levels.Contains(R.Map)) { Slot = R.MapIdx; break; }
	}

	for (const FString& L : Levels) BF6PortalWeb::SetSaveLink(L, Save, Url, Slot);

	TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(Dir));
	if (!M.IsValid()) M = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("id"), ExperienceId);
	E->SetStringField(TEXT("url"), Url);
	if (Slot >= 0) E->SetNumberField(TEXT("mapIdx"), Slot);
	E->SetStringField(TEXT("linkedBy"), TEXT("tool"));
	E->SetStringField(TEXT("linkedAt"), NowIso());
	{
		const TSharedPtr<FJsonObject> Cache = ReadJson(FPaths::Combine(PortalCacheRoot(), ExperienceId, TEXT("experience.json")));
		FString Nm;
		if (Cache.IsValid() && Cache->TryGetStringField(TEXT("name"), Nm)) E->SetStringField(TEXT("name"), Nm);
	}
	M->SetObjectField(TEXT("experience"), E);
	WriteJson(ManifestPath(Dir), M);

	// The session file carries the link too, and it only gets it on a write.
	// Saving the open session now means the two agree straight away instead of
	// after whatever the user happens to do next.
	if (BF6Api::CurrentSave() == Save && BF6Api::IsEditing()) BF6Api::SaveCurrent(true);

	WriteManifest(Dir, Save, Levels[0]);
	UE_LOG(LogBF6Project, Display, TEXT("linked project '%s' to experience %s%s."),
		*Save, *ExperienceId.Left(8), Slot >= 0 ? *FString::Printf(TEXT(" (rotation slot %d)"), Slot) : TEXT(""));
	Notify(FString::Printf(TEXT("Linked '%s' to the experience."), *Save));
	return true;
}

bool BF6Project::Unlink(const FString& Save)
{
	const FString Dir = DirFor(Save);
	if (Dir.IsEmpty()) return false;
	TArray<FString> Levels;
	GatherArtefacts(Dir, Levels);
	for (const FString& L : Levels) BF6PortalWeb::SetSaveLink(L, Save, FString(), -1);
	TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(Dir));
	if (M.IsValid()) { M->RemoveField(TEXT("experience")); WriteJson(ManifestPath(Dir), M); }
	UE_LOG(LogBF6Project, Display, TEXT("project '%s' is no longer linked to an experience. Every file it had stays."), *Save);
	return true;
}

void BF6Project::ShowCompareDialog(const FString& Save)
{
	const TArray<FMatch> Ms = Compare(Save);
	LogCompare(Save);
	if (Ms.Num() == 0)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
			TEXT("There are no experiences on this machine to compare against yet.\n\nLink your Portal account on the map screen and import an experience, then try again.")));
		return;
	}
	const FMatch& Best = Ms[0];
	FString Body = FString::Printf(TEXT("Best match for '%s':\n\n%s\n%.1f%% confidence\n\n"), *Save, *Best.Name, Best.Confidence);
	for (const FString& R : Best.Reasons)       Body += FString::Printf(TEXT("  yes   %s\n"), *R);
	for (const FString& R : Best.NotComparable) Body += FString::Printf(TEXT("  n/a   %s\n"), *R);
	if (Ms.Num() > 1)
	{
		Body += TEXT("\nRunners up:\n");
		for (int32 i = 1; i < FMath::Min(Ms.Num(), 4); ++i)
			Body += FString::Printf(TEXT("  %.1f%%  %s\n"), Ms[i].Confidence, *Ms[i].Name);
	}
	Body += TEXT("\nThe full reasoning is in the Output Log under LogBF6Project.\n\nLink this project to the best match?");

	if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Body)) == EAppReturnType::Yes)
		Link(Save, Best.Id);
}

// ---------------------------------------------------------------------------
// ROUND TRIP.
//
// A project written out as the site's own whole-experience JSON, and one of
// those files read in as a project.
//
// NOTE FOR A LATER MERGE. BF6PortalProfile already builds that document from a
// fetched experience and already reads one back in. Its builder is file local
// and cannot be called from here, so the branch below prefers its PUBLIC
// ExperienceJson when the project is linked and only composes its own document
// when there is nothing to ask. The reader half delegates the world side to
// BF6PortalProfile::ImportExperienceFile and only does the project side here,
// so the parse happens twice. When the two files are merged, this is the
// duplication to remove.
// ---------------------------------------------------------------------------
bool BF6Project::ExportExperienceJson(const FString& Save, const FString& Path)
{
	const FString Dir = DirFor(Save);
	if (Dir.IsEmpty() || !FPaths::DirectoryExists(Dir))
	{
		UE_LOG(LogBF6Project, Warning, TEXT("export: there is no project called '%s'."), *Save);
		return false;
	}

	FString Out = Path;
	if (Out.IsEmpty()) Out = FPaths::Combine(Dir, TEXT("unreal"), Save + TEXT(".experience.json"));

	TArray<FString> Levels;
	const TArray<FArtefact> Arts = GatherArtefacts(Dir, Levels);
	const TSharedPtr<FJsonObject> Man = ReadJson(ManifestPath(Dir));
	FString ExpId;
	{
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (Man.IsValid() && Man->TryGetObjectField(TEXT("experience"), E) && E) (*E)->TryGetStringField(TEXT("id"), ExpId);
	}

	// The complete document, when the profile has the experience: it came off
	// the site whole, and rebuilding it here from parts would be a worse copy.
	TSharedPtr<FJsonObject> Root;
	if (!ExpId.IsEmpty() && BF6PortalProfile::ExperienceJson(ExpId, Root) && Root.IsValid())
	{
		if (WriteJson(Out, Root))
		{
			UE_LOG(LogBF6Project, Display, TEXT("export '%s': the experience document as the site holds it, written to %s"), *Save, *Out);
			return true;
		}
		return false;
	}

	// Otherwise compose one from the project, and say what it could not fill in.
	Root = MakeShared<FJsonObject>();
	TArray<FString> Gaps;

	{
		const TSharedPtr<FJsonObject> Pkg = ReadJson(FPaths::Combine(Dir, TEXT("package.json")));
		FString Nm, Desc;
		if (Pkg.IsValid()) { Pkg->TryGetStringField(TEXT("experienceName"), Nm); Pkg->TryGetStringField(TEXT("description"), Desc); }
		Root->SetStringField(TEXT("name"), Nm.IsEmpty() ? Save : Nm);
		Root->SetStringField(TEXT("description"), Desc);
	}
	Root->SetStringField(TEXT("gameMode"), TEXT("ModBuilderCustom"));

	{
		TSharedPtr<FJsonObject> Mut;
		if (BF6PortalSettings::ExperienceMutators(ExpId, Mut) && Mut.IsValid()) Root->SetObjectField(TEXT("mutators"), Mut);
		else Gaps.Add(TEXT("mutators"));
		TArray<TSharedPtr<FJsonValue>> Teams;
		if (BF6PortalSettings::ExperienceTeams(ExpId, Teams)) Root->SetArrayField(TEXT("teamComposition"), Teams);
		else Gaps.Add(TEXT("teamComposition"));
		TSharedPtr<FJsonObject> Res;
		if (BF6PortalSettings::ExperienceRestrictions(ExpId, Res) && Res.IsValid()) Root->SetObjectField(TEXT("assetRestrictions"), Res);
		else Gaps.Add(TEXT("assetRestrictions"));
	}

	{
		const TSharedPtr<FJsonObject> W = ReadJson(FPaths::Combine(Dir, TEXT("unreal"), TEXT("blockly"), TEXT("workspace.json")));
		if (W.IsValid()) Root->SetObjectField(TEXT("workspace"), W);
		else Gaps.Add(TEXT("workspace"));
	}

	// One attachment per exported spatial, in the site's own shape. The site
	// gives every attachment an id and a version; the tool has neither for a
	// file it made itself, so those are left out rather than invented.
	TArray<TSharedPtr<FJsonValue>> Atts, Rot;
	int32 Idx = 0;
	for (const FArtefact& A : Arts)
	{
		if (A.Kind != TEXT("spatial")) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, A.Rel))) continue;
		const FTCHARToUTF8 Utf8(*Text);
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("original"), FBase64::Encode((const uint8*)Utf8.Get(), Utf8.Length()));
		Data->SetStringField(TEXT("compiled"), TEXT(""));
		TSharedPtr<FJsonObject> At = MakeShared<FJsonObject>();
		At->SetStringField(TEXT("filename"), FPaths::GetCleanFilename(A.Rel));
		At->SetNumberField(TEXT("attachmentType"), 1);
		At->SetStringField(TEXT("metadata"), FString::Printf(TEXT("mapIdx=%d"), Idx));
		At->SetObjectField(TEXT("attachmentData"), Data);
		At->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
		Atts.Add(MakeShared<FJsonValueObject>(At));

		TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
		Slot->SetStringField(TEXT("id"), FString::Printf(TEXT("%s-ModBuilderCustom%d"), *A.Level, Idx));
		Rot.Add(MakeShared<FJsonValueObject>(Slot));
		++Idx;
	}
	Root->SetArrayField(TEXT("mapRotation"), Rot);
	Root->SetArrayField(TEXT("attachments"), Atts);
	if (Atts.Num() == 0) Gaps.Add(TEXT("spatial attachments"));

	if (Gaps.Num() > 0)
		Root->SetStringField(TEXT("_incomplete"), FString::Printf(
			TEXT("Composed by the BF6 Unreal SDK from a project that has no fetched experience behind it. Not present: %s."),
			*FString::Join(Gaps, TEXT(", "))));

	if (!WriteJson(Out, Root)) return false;
	UE_LOG(LogBF6Project, Display, TEXT("export '%s': composed an experience document at %s%s"),
		*Save, *Out,
		Gaps.Num() ? *FString::Printf(TEXT(" (not present: %s)"), *FString::Join(Gaps, TEXT(", "))) : TEXT(""));
	return true;
}

bool BF6Project::ImportExperienceJson(const FString& Path, FString& OutSave)
{
	FString File = Path;
#if WITH_EDITOR
	if (File.IsEmpty())
	{
		IDesktopPlatform* DP = FDesktopPlatformModule::Get();
		if (!DP) return false;
		const void* Parent = FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
			? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle() : nullptr;
		TArray<FString> Picked;
		if (!DP->OpenFileDialog(Parent, TEXT("Import a whole experience JSON"), FPaths::ProjectSavedDir(), TEXT(""),
			TEXT("Portal experience (*.json)|*.json"), EFileDialogFlags::None, Picked) || Picked.Num() == 0)
			return false;
		File = Picked[0];
	}
#endif
	if (File.IsEmpty()) return false;

	const TSharedPtr<FJsonObject> Root = ReadJson(File);
	if (!Root.IsValid() || !Root->HasField(TEXT("attachments")))
	{
		UE_LOG(LogBF6Project, Warning, TEXT("import: %s does not look like a whole experience JSON."), *File);
		return false;
	}

	FString Name;
	Root->TryGetStringField(TEXT("name"), Name);
	if (Name.IsEmpty()) Name = FPaths::GetBaseFilename(File);
	for (TCHAR& C : Name) if (FString(TEXT("\\/:*?\"<>|")).Contains(FString(1, &C))) C = TEXT('_');
	OutSave = Name;

	const FString Dir = DirFor(Name);
	const bool bExisted = FPaths::DirectoryExists(Dir);
	IFileManager::Get().MakeDirectory(*Dir, true);
	Ensure(FString(), Name, true);

	// THE PRE-IMPORT SNAPSHOT. Taken before a single byte is overwritten, kept
	// out of the rotation so it can never age out, and taken even when the
	// import turns out to change nothing.
	if (bExisted) SnapshotProject(Name, FString::Printf(TEXT("before importing %s"), *FPaths::GetCleanFilename(File)));

	int32 Wrote = 0;

	{
		const TSharedPtr<FJsonObject>* W = nullptr;
		if (Root->TryGetObjectField(TEXT("workspace"), W) && W)
		{
			WriteJson(FPaths::Combine(Dir, TEXT("unreal"), TEXT("blockly"), TEXT("workspace.json")), *W);
			++Wrote;
		}
	}
	{
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (Root->TryGetObjectField(TEXT("mutators"), O) && O) S->SetObjectField(TEXT("mutators"), *O);
		if (Root->TryGetObjectField(TEXT("assetRestrictions"), O) && O) S->SetObjectField(TEXT("assetRestrictions"), *O);
		const TArray<TSharedPtr<FJsonValue>>* T = nullptr;
		if (Root->TryGetArrayField(TEXT("teamComposition"), T) && T) S->SetArrayField(TEXT("teamComposition"), *T);
		if (S->Values.Num() > 0) { WriteJson(FPaths::Combine(Dir, TEXT("unreal"), TEXT("settings"), TEXT("settings.json")), S); ++Wrote; }
	}
	{
		const TArray<TSharedPtr<FJsonValue>>* R = nullptr;
		if (Root->TryGetArrayField(TEXT("mapRotation"), R) && R)
		{
			TSharedPtr<FJsonObject> Wrap = MakeShared<FJsonObject>();
			Wrap->SetArrayField(TEXT("mapRotation"), *R);
			WriteJson(FPaths::Combine(Dir, TEXT("unreal"), TEXT("rotation.json")), Wrap);
			++Wrote;
		}
	}
	TArray<FString> Refused, Failed;
	{
		const TArray<TSharedPtr<FJsonValue>>* Atts = nullptr;
		if (Root->TryGetArrayField(TEXT("attachments"), Atts) && Atts)
		{
			for (const TSharedPtr<FJsonValue>& V : *Atts)
			{
				const TSharedPtr<FJsonObject> A = V->AsObject();
				if (!A.IsValid()) continue;
				FString FileName;
				A->TryGetStringField(TEXT("filename"), FileName);
				const TSharedPtr<FJsonObject>* D = nullptr;
				if (FileName.IsEmpty() || !A->TryGetObjectField(TEXT("attachmentData"), D) || !D) continue;
				FString B64;
				if (!(*D)->TryGetStringField(TEXT("original"), B64) || B64.IsEmpty()) continue;
				TArray<uint8> Bytes;
				if (!FBase64::Decode(B64, Bytes)) continue;
				Bytes.Add(0);
				const FString Text = FString(UTF8_TO_TCHAR((const ANSICHAR*)Bytes.GetData()));

				// A NAME OUT OF A FILE IS NOT A PATH.
				//
				// This came from an experience export, which is a file the user
				// downloaded from somewhere, and it was joined straight into the
				// project directory. A filename of ..\..\..\something, or a
				// rooted C:\..., wrote wherever it liked. Nothing malicious has
				// to be involved for this to hurt: a filename with a stray path
				// separator lands somewhere surprising and the import still
				// says it succeeded.
				//
				// The leaf is taken, the obvious escapes are refused, and the
				// resolved destination is then CHECKED to be inside the project
				// before anything is written. The last check is the one that
				// counts, because it holds even if the first two miss a form.
				FString Leaf = FileName;
				Leaf.ReplaceInline(TEXT("\\"), TEXT("/"));
				Leaf = FPaths::GetCleanFilename(Leaf);
				if (Leaf.IsEmpty() || Leaf == TEXT(".") || Leaf == TEXT("..") ||
					Leaf.Contains(TEXT(":")))
				{
					Refused.Add(FileName);
					continue;
				}
				const bool bSpatial = Leaf.EndsWith(TEXT(".spatial.json"));
				const FString To = FPaths::ConvertRelativePathToFull(
					FPaths::Combine(Dir, bSpatial ? TEXT("spatials") : TEXT("src"), Leaf));
				if (!FPaths::IsUnderDirectory(To, FPaths::ConvertRelativePathToFull(Dir)))
				{
					UE_LOG(LogBF6Project, Warning,
						TEXT("import: refused an attachment that resolves outside the project: %s"), *FileName);
					Refused.Add(FileName);
					continue;
				}

				// A write that fails is a file the project does not have. It
				// used to be skipped in silence, so a half imported project
				// reported the same success as a whole one.
				if (FFileHelper::SaveStringToFile(Text, *To)) { ++Wrote; }
				else { Failed.Add(Leaf); }
			}
		}
	}

	// The world half is the profile's job, and it already does all of it: the
	// maps, the saves, the settings and the links.
	const bool bWorld = BF6PortalProfile::ImportExperienceFile(File);

	WriteManifest(Dir, Name, FString());
	// EVERY WRITE SAYS WHO CAUSED IT. These came off the site, so the sync that
	// follows knows not to offer them back to the user as our own changes.
	for (const FArtefactState& S : ArtefactStates(Name))
		if (S.Kind == TEXT("spatial") || S.Kind == TEXT("workspace") || S.Kind == TEXT("settings")
			|| S.Kind == TEXT("script") || S.Kind == TEXT("strings") || S.Kind == TEXT("rotation"))
			NoteArtefactWritten(Name, S.Rel, TEXT("site"));

	if (Refused.Num() || Failed.Num())
	{
		// Said plainly rather than folded into the count, because an import that
		// is missing files is not the project the user thinks they imported.
		UE_LOG(LogBF6Project, Warning,
			TEXT("import: %d attachment(s) refused for an unsafe name (%s), %d could not be written (%s). ")
			TEXT("The project is INCOMPLETE."),
			Refused.Num(), Refused.Num() ? *FString::Join(Refused, TEXT(", ")) : TEXT("none"),
			Failed.Num(), Failed.Num() ? *FString::Join(Failed, TEXT(", ")) : TEXT("none"));
	}
	UE_LOG(LogBF6Project, Display, TEXT("import: project '%s' created from %s, %d file(s) written. The map import %s."),
		*Name, *FPaths::GetCleanFilename(File), Wrote, bWorld ? TEXT("ran") : TEXT("was refused, so the maps are the spatials in spatials/ only"));
	return Refused.Num() == 0 && Failed.Num() == 0;
}

// ---------------------------------------------------------------------------
// ===========================================================================
// THE GODOT SCENE A SPATIAL WAS AUTHORED FROM.
//
// A .spatial.json is a flat list of objects. The only trace of the scene tree
// in it is each object's id, which is its full Godot node path, so a group
// pivot that holds no object of its own exists as a path segment and nothing
// more: it has no transform in the file at all, and the importer can only put
// it at the centroid of its children. A MINIFIED spatial, which is what the
// site usually stores, renames every one of those segments to a two or three
// letter token, so a map that came back from Portal has an outliner full of
// "bmw/bmx" where the creator wrote "Arena-Center/Arena-Walls".
//
// The .tscn is the authored source and still has every name, every group and
// every pivot. So when a map opens from an experience the tool asks, once,
// whether the creator still has it - and takes no for an answer permanently.
// ===========================================================================
namespace
{
	// Rewrite one top-level object of project.json in place, leaving the rest
	// of the manifest exactly as it is.
	bool SetManifestField(const FString& Dir, const FString& Field, const TSharedPtr<FJsonObject>& Val)
	{
		const FString Path = ManifestPath(Dir);
		TSharedPtr<FJsonObject> M = ReadJson(Path);
		if (!M.IsValid()) return false;
		M->SetObjectField(Field, Val);
		return WriteJson(Path, M);
	}

	TSharedPtr<FJsonObject> GetManifestField(const FString& Save, const FString& Field)
	{
		const TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(BF6Project::DirFor(Save)));
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (M.IsValid() && M->TryGetObjectField(Field, O) && O) return *O;
		return nullptr;
	}

	// Where a file picker should start looking for a scene file: the SDK's own
	// user levels folder first, because that is where the official editor
	// writes them, then the project itself.
	FString TscnStartDir(const FString& Save)
	{
		const FString Sdk = BF6Api::StoredSdkRoot();
		if (!Sdk.IsEmpty())
		{
			const FString Levels = FPaths::Combine(Sdk, TEXT("GodotProject"), TEXT("User_Created"), TEXT("levels"));
			if (FPaths::DirectoryExists(Levels)) return Levels;
		}
		const FString Mine = FPaths::Combine(BF6Project::DirFor(Save), TEXT("unreal"), TEXT("tscn"));
		if (FPaths::DirectoryExists(Mine)) return Mine;
		const FString Dir = BF6Project::DirFor(Save);
		return FPaths::DirectoryExists(Dir) ? Dir : FPaths::ProjectSavedDir();
	}

	// A save the tool made from a spatial, and not one the user built here.
	// Newer saves say so themselves; older ones are decided by the experience's
	// own rotation, where a slot with a spatial attachment is exactly this
	// population and a slot without one is a base setup with no scene to find.
	bool CameFromSpatial(const FString& Level, const FString& Save)
	{
		const FString Kind = BF6Project::OriginKind(Save);
		if (!Kind.IsEmpty()) return Kind == TEXT("spatial");

		const FString Url = BF6PortalWeb::ExperienceForSave(Level, Save);
		if (Url.IsEmpty()) return false;
		const FString Id = IdFromUrl(Url);
		const int32 Idx = BF6PortalWeb::MapIdxForSave(Level, Save);
		if (Id.IsEmpty() || Idx < 0) return false;
		const TArray<BF6PortalProfile::FRotationRow> Rot = BF6PortalProfile::RotationFor(Id);
		return Rot.IsValidIndex(Idx) && Rot[Idx].bHasSpatial;
	}

#if WITH_EDITOR
	// A modal with as many real buttons as there are real answers. Two answers
	// fit a message box; three do not, and the third answer here ("do not ask
	// again") is the one that must not be hidden behind a Cancel.
	// Returns the index of the button pressed, or INDEX_NONE if the window was
	// closed without one.
	int32 AskWithButtons(const FString& Title, const FString& Body, const TArray<FString>& Buttons)
	{
		if (!FSlateApplication::IsInitialized()) return INDEX_NONE;
		int32 Picked = INDEX_NONE;
		TSharedRef<SWindow> Win = SNew(SWindow)
			.Title(FText::FromString(Title))
			.SizingRule(ESizingRule::Autosized)
			.SupportsMaximize(false).SupportsMinimize(false);

		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (int32 i = 0; i < Buttons.Num(); i++)
		{
			const FString Label = Buttons[i];
			Row->AddSlot().AutoWidth().Padding(6, 0, 0, 0)
			[
				SNew(SButton)
				.ContentPadding(FMargin(14, 6))
				.Text(FText::FromString(Label))
				.OnClicked_Lambda([&Picked, i, WinPtr = &Win.Get()]
				{
					Picked = i;
					WinPtr->RequestDestroyWindow();
					return FReply::Handled();
				})
			];
		}

		Win->SetContent(
			SNew(SBorder).Padding(FMargin(18, 16))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).MaxDesiredWidth(560.f)
					[ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Body)) ]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 18, 0, 0).HAlign(HAlign_Right)
				[ Row ]
			]);

		FSlateApplication::Get().AddModalWindow(Win, FSlateApplication::Get().GetActiveTopLevelWindow());
		return Picked;
	}
#else
	int32 AskWithButtons(const FString&, const FString&, const TArray<FString>&) { return INDEX_NONE; }
#endif

	// The differences, in words rather than a table, because the person reading
	// this is deciding whether to trust the file and not auditing it.
	FString DescribeCompare(const BF6Api::FTscnCompare& C)
	{
		FString S = FString::Printf(TEXT("The scene file has %d objects and this map has %d.\n"),
			C.TscnObjects, C.MapObjects);
		S += FString::Printf(TEXT("%d matched on their script id and %d more on their type and position.\n"),
			C.MatchedById, C.MatchedByPos);
		if (C.OnlyInTscn > 0) S += FString::Printf(TEXT("%d object(s) are in the scene file and not in this map.\n"), C.OnlyInTscn);
		if (C.OnlyInMap  > 0) S += FString::Printf(TEXT("%d object(s) are in this map and not in the scene file.\n"), C.OnlyInMap);
		if (C.Moved      > 0) S += FString::Printf(TEXT("%d object(s) matched but have moved since.\n"), C.Moved);
		return S;
	}
}

FString BF6Project::OriginKind(const FString& Save)
{
	const TSharedPtr<FJsonObject> O = GetManifestField(Save, TEXT("origin"));
	FString Kind;
	if (O.IsValid()) O->TryGetStringField(TEXT("kind"), Kind);
	return Kind;
}

void BF6Project::NoteOrigin(const FString& Level, const FString& Save, const FString& Kind, const FString& Source)
{
	if (Save.IsEmpty()) return;
	const FString Dir = DirFor(Save);
	if (!FPaths::FileExists(ManifestPath(Dir))) Ensure(Level, Save, false);
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("kind"), Kind);
	if (!Source.IsEmpty()) O->SetStringField(TEXT("source"), Source);
	O->SetStringField(TEXT("at"), NowIso());
	SetManifestField(Dir, TEXT("origin"), O);
}

// ONE RECORD PER MAP, not per project. An experience is one save name with
// eleven maps under it, and each of those maps has its own scene file: keyed
// by save alone, answering for one map would have silenced the question for
// the other ten.
BF6Project::FTscnRecord BF6Project::TscnFor(const FString& Save, const FString& Level)
{
	FTscnRecord R;
	const TSharedPtr<FJsonObject> All = GetManifestField(Save, TEXT("tscn"));
	if (!All.IsValid()) return R;
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	TSharedPtr<FJsonObject> O;
	if (!Level.IsEmpty() && All->TryGetObjectField(Level, Sub) && Sub) O = *Sub;
	else if (All->HasField(TEXT("state"))) O = All;   // written before this was per map
	if (!O.IsValid()) return R;
	O->TryGetStringField(TEXT("state"), R.State);
	O->TryGetStringField(TEXT("path"), R.Path);
	O->TryGetStringField(TEXT("md5"), R.Md5);
	O->TryGetStringField(TEXT("adoptedAt"), R.AdoptedAt);
	O->TryGetStringField(TEXT("askedAt"), R.AskedAt);
	if (R.State.IsEmpty()) R.State = TEXT("none");
	if (!R.Path.IsEmpty() && FPaths::FileExists(R.Path))
	{
		R.bOnDisk = true;
		const FString Now = HashFile(R.Path);
		R.bChanged = !R.Md5.IsEmpty() && !Now.IsEmpty() && Now != R.Md5;
	}
	return R;
}

namespace
{
	// Put one map's record into the manifest's tscn block, leaving the other
	// maps of the same experience alone.
	void PutTscnRecord(const FString& Level, const FString& Save, const TSharedPtr<FJsonObject>& Rec)
	{
		if (Save.IsEmpty() || Level.IsEmpty()) return;
		const FString Dir = BF6Project::DirFor(Save);
		if (!FPaths::FileExists(ManifestPath(Dir))) BF6Project::Ensure(Level, Save, false);
		TSharedPtr<FJsonObject> All = GetManifestField(Save, TEXT("tscn"));
		if (!All.IsValid() || All->HasField(TEXT("state"))) All = MakeShared<FJsonObject>();
		All->SetObjectField(Level, Rec);
		SetManifestField(Dir, TEXT("tscn"), All);
	}
}

void BF6Project::NoteTscnAdopted(const FString& Level, const FString& Save, const FString& TscnPath)
{
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("state"), TEXT("adopted"));
	O->SetStringField(TEXT("path"), TscnPath);
	O->SetStringField(TEXT("md5"), HashFile(TscnPath));
	O->SetStringField(TEXT("adoptedAt"), NowIso());
	PutTscnRecord(Level, Save, O);
}

void BF6Project::NoteTscnSkipped(const FString& Level, const FString& Save, bool bForever)
{
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("state"), bForever ? TEXT("never") : TEXT("skipped"));
	O->SetStringField(TEXT("askedAt"), NowIso());
	PutTscnRecord(Level, Save, O);
}

bool BF6Project::ImportTscn(const FString& Save, const FString& InPath)
{
	if (Save.IsEmpty())
	{
		UE_LOG(LogBF6Project, Warning, TEXT("open a map first: a scene file is adopted onto the map it belongs to."));
		return false;
	}
	const FString Level = BF6Api::CurrentSave() == Save ? BF6Api::CurrentLevel() : FString();
	FString File = InPath;
#if WITH_EDITOR
	if (File.IsEmpty())
	{
		IDesktopPlatform* DP = FDesktopPlatformModule::Get();
		if (!DP) return false;
		TArray<FString> Picked;
		const void* Parent = FSlateApplication::IsInitialized()
			? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
		if (!DP->OpenFileDialog(Parent, TEXT("Find the Godot scene this map was made from"),
			TscnStartDir(Save), TEXT(""), TEXT("Godot scene (*.tscn)|*.tscn"),
			EFileDialogFlags::None, Picked) || Picked.Num() == 0) return false;
		File = Picked[0];
	}
#endif
	if (File.IsEmpty() || !FPaths::FileExists(File))
	{
		UE_LOG(LogBF6Project, Warning, TEXT("no such scene file: %s"), *File);
		return false;
	}

	const BF6Api::FTscnCompare C = BF6Api::CompareTscn(File);
	if (!C.Problem.IsEmpty())
	{
		UE_LOG(LogBF6Project, Warning, TEXT("could not read %s: %s"), *File, *C.Problem);
		AskWithButtons(TEXT("Godot scene"),
			FString::Printf(TEXT("%s could not be read as a Godot scene.\n\n%s"), *FPaths::GetCleanFilename(File), *C.Problem),
			{ TEXT("CLOSE") });
		return false;
	}
	UE_LOG(LogBF6Project, Display,
		TEXT("tscn compare '%s' against '%s': %d scene objects, %d map objects, %d matched by ObjId, %d by position, %d only in the scene, %d only in the map, %d moved, %d pivots"),
		*FPaths::GetCleanFilename(File), *Save, C.TscnObjects, C.MapObjects, C.MatchedById, C.MatchedByPos,
		C.OnlyInTscn, C.OnlyInMap, C.Moved, C.TscnPivots);

	bool bMerge = true;
	if (!C.Agrees())
	{
		// THE TWO ARE NOT THE SAME MAP, or not the same version of it. Say how,
		// and offer the honest alternative rather than merging over the top.
		const FString Body = FString::Printf(
			TEXT("%s and this map are not the same set of objects.\n\n%s\nAdopting names and grouping will only touch the %d object(s) that do match, and will leave everything else exactly where it is.\n\nIf this scene file is really a different version of the map, opening it as its own map is the safer answer. Nothing that is saved on disk for '%s' changes either way."),
			*FPaths::GetCleanFilename(File), *DescribeCompare(C), C.MatchedById + C.MatchedByPos, *Save);
		const int32 Pick = AskWithButtons(TEXT("The scene file does not match this map"), Body,
			{ TEXT("ADOPT WHAT MATCHES"), TEXT("OPEN IT AS ITS OWN MAP"), TEXT("CANCEL") });
		if (Pick == 1)
		{
			SnapshotProject(Save, TEXT("before opening ") + FPaths::GetCleanFilename(File) + TEXT(" as its own map"));
			return BF6Api::ImportTscnAsSave(File);
		}
		if (Pick != 0) return false;
	}

	// NEVER WITHOUT A SNAPSHOT. This is the pre-import kind, kept out of the
	// rotation, so it cannot age out from under the person who needs it.
	SnapshotProject(Save, TEXT("before adopting ") + FPaths::GetCleanFilename(File));

	BF6Api::FTscnMerge M;
	if (!BF6Api::MergeTscnTree(File, M)) return false;
	NoteTscnAdopted(Level, Save, File);

	const FString Told = FString::Printf(
		TEXT("Restored %d name(s) and %d group(s) from %s.\n\n%d pivot(s) moved back to where they were authored and %d object(s) went back to the branch they belong to. The map is not saved yet, so SAVE keeps this and closing without saving throws it away."),
		M.Renamed, M.Groups, *FPaths::GetCleanFilename(File), M.Pivots, M.Reparented);
	UE_LOG(LogBF6Project, Display, TEXT("%s"), *Told.Replace(TEXT("\n"), TEXT(" ")));
	Notify(FString::Printf(TEXT("Godot scene adopted: %d names, %d groups restored."), M.Renamed, M.Groups));
	AskWithButtons(TEXT("Godot scene adopted"), Told, { TEXT("GOOD") });
	return true;
}

void BF6Project::ShowOrganiseDialog()
{
	// What is actually there, counted before a word is said about it.
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(SavesRoot() / TEXT("*")), false, true);
	TMap<FString, TArray<FString>> ByExp;   // experience name -> the flat saves that belong to it
	int32 Standalone = 0;
	for (const FString& Save : Dirs)
	{
		const FString Dir = FPaths::Combine(SavesRoot(), Save);
		TArray<FString> Levels;
		GatherArtefacts(Dir, Levels);
		if (Levels.Num() == 0) continue;
		FString Id, Name;
		const TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(Dir));
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (M.IsValid() && M->TryGetObjectField(TEXT("experience"), E) && E)
		{ (*E)->TryGetStringField(TEXT("id"), Id); (*E)->TryGetStringField(TEXT("name"), Name); }
		if (Id.IsEmpty())
			for (const FString& L : Levels) { Id = IdFromUrl(BF6PortalWeb::ExperienceForSave(L, Save)); if (!Id.IsEmpty()) break; }
		if (Id.IsEmpty()) { Standalone++; continue; }
		ByExp.FindOrAdd(Name.IsEmpty() ? Id.Left(8) : Name).Add(Save);
	}

	if (ByExp.Num() == 0)
	{
		AskWithButtons(TEXT("Organise saves into experiences"),
			FString::Printf(TEXT("There is nothing to reorganise: %d save(s) here, and none of them belongs to a Portal experience.\n\nA save that belongs to no experience stays exactly as it is, in its own folder. This only affects maps that are part of a game mode."), Standalone),
			{ TEXT("CLOSE") });
		return;
	}

	FString Body = TEXT("An experience is one game mode: one script project, one settings set, one thumbnail, one block workspace, and many maps. Right now each of its maps is a separate save with its own copy of all of that.\n\nThis puts each experience in one folder with its maps inside it:\n\n");
	for (const TPair<FString, TArray<FString>>& KV : ByExp)
		Body += FString::Printf(TEXT("  %s  -  %d map(s) into one folder\n"), *KV.Key, KV.Value.Num());
	if (Standalone > 0)
		Body += FString::Printf(TEXT("\n%d save(s) belong to no experience and are not touched.\n"), Standalone);
	Body += TEXT("\nEvery file is copied and checked at the far end before the original is removed, and a snapshot is taken first. A map that cannot be verified is left exactly where it is and said so in the Output Log.");

	const int32 Pick = AskWithButtons(TEXT("Organise saves into experiences"), Body,
		{ TEXT("DO IT"), TEXT("SHOW ME FIRST"), TEXT("NOT NOW") });
	if (Pick == 0)
	{
		const int32 N = MigrateFlatSaves(false);
		AskWithButtons(TEXT("Organised"),
			FString::Printf(TEXT("%d map(s) moved into their experience.\n\nThe full account is in the Output Log under LogBF6Project. The old folders are left behind with whatever the tool did not move; delete them yourself once you are happy."), N),
			{ TEXT("GOOD") });
	}
	else if (Pick == 1)
	{
		MigrateFlatSaves(true);
		AskWithButtons(TEXT("This is what it would do"),
			TEXT("The plan is in the Output Log under LogBF6Project. Nothing on disk has been touched."),
			{ TEXT("CLOSE") });
	}
}

void BF6Project::OfferTscn(const FString& Level, const FString& Save)
{
	if (Save.IsEmpty()) return;
	const FTscnRecord R = TscnFor(Save, Level);

	// Already answered with a scene file. The only thing left to say is when
	// that file has moved on since, and that is said once, not silently
	// ignored and not acted on behind the user's back.
	if (R.State == TEXT("adopted"))
	{
		if (!R.bChanged) return;
		const int32 Pick = AskWithButtons(TEXT("The Godot scene has changed"),
			FString::Printf(TEXT("The Godot scene this map's names and grouping came from has changed on disk since it was brought in.\n\n%s\n\nBringing it in again takes whatever was renamed or regrouped in the official editor. Nothing else about this map changes."), *R.Path),
			{ TEXT("BRING IT IN AGAIN"), TEXT("LEAVE IT"), TEXT("STOP TELLING ME") });
		if (Pick == 0) ImportTscn(Save, R.Path);
		else if (Pick == 2) NoteTscnSkipped(Level, Save, true);
		return;
	}
	if (R.State == TEXT("never")) return;          // asked and answered, for good
	if (!CameFromSpatial(Level, Save)) return;     // the user built this one here

	const int32 Pick = AskWithButtons(TEXT("This map came from Portal"),
		FString::Printf(
			TEXT("This map came from Portal as a spatial file, which loses the original names and grouping. If you still have the Godot .tscn it was made from, bringing it in restores them.\n\nNothing is moved, added or deleted: the objects stay exactly where they are, and only their names and the groups they sit in come back.\n\n(Map: %s)"),
			*Save),
		{ TEXT("IMPORT THE .TSCN"), TEXT("USE THE SPATIAL"), TEXT("DO NOT ASK AGAIN FOR THIS MAP") });

	if (Pick == 0)       ImportTscn(Save, FString());
	else if (Pick == 1)  NoteTscnSkipped(Level, Save, false);
	else if (Pick == 2)  NoteTscnSkipped(Level, Save, true);
}

void BF6Project::LogStatus(const FString& Save)
{
	const FString Want = Save.IsEmpty() ? BF6Api::CurrentSave() : Save;
	if (Want.IsEmpty())
	{
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(SavesRoot() / TEXT("*")), false, true);
		UE_LOG(LogBF6Project, Display, TEXT("nothing open. %d project(s) under %s"), Dirs.Num(), *SavesRoot());
		return;
	}
	const FString Dir = DirFor(Want);
	UE_LOG(LogBF6Project, Display, TEXT("project '%s'"), *Want);
	UE_LOG(LogBF6Project, Display, TEXT("  folder    %s"), *Dir);
	UE_LOG(LogBF6Project, Display, TEXT("  manifest  %s"), HasManifest(Want) ? TEXT("yes") : TEXT("no"));
	UE_LOG(LogBF6Project, Display, TEXT("  template  %s"), HasTemplate(Want) ? TEXT("yes") : TEXT("no"));
	UE_LOG(LogBF6Project, Display, TEXT("  modules   %s"), FPaths::DirectoryExists(FPaths::Combine(Dir, TEXT("node_modules"))) ? TEXT("installed") : TEXT("not installed, press INSTALL in the script editor"));
	const TSharedPtr<FJsonObject> M = ReadJson(ManifestPath(Dir));
	if (!M.IsValid()) return;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (M->TryGetArrayField(TEXT("artefacts"), Arr) && Arr)
		UE_LOG(LogBF6Project, Display, TEXT("  artefacts %d"), Arr->Num());
	if (M->TryGetArrayField(TEXT("missing"), Arr) && Arr)
		for (const TSharedPtr<FJsonValue>& V : *Arr) UE_LOG(LogBF6Project, Display, TEXT("  missing   %s"), *V->AsString());
	// Where it came from, and whether its Godot scene has been asked about.
	{
		const FString Kind = OriginKind(Want);
		UE_LOG(LogBF6Project, Display, TEXT("  came from %s"), Kind.IsEmpty() ? TEXT("not recorded (made before the tool kept this)") : *Kind);
		const FString AskLevel = (Want == BF6Api::CurrentSave()) ? BF6Api::CurrentLevel() : FString();
		const FTscnRecord R = TscnFor(Want, AskLevel);
		if (R.State == TEXT("adopted"))
		{
			UE_LOG(LogBF6Project, Display, TEXT("  tscn      adopted %s from %s%s"), *R.AdoptedAt, *R.Path,
				R.bChanged ? TEXT("  (that file has changed since)") : (R.bOnDisk ? TEXT("") : TEXT("  (that file is no longer there)")));
		}
		else if (R.State == TEXT("never"))
		{
			UE_LOG(LogBF6Project, Display, TEXT("  tscn      the user asked not to be asked again (%s)"), *R.AskedAt);
		}
		else if (R.State == TEXT("skipped"))
		{
			UE_LOG(LogBF6Project, Display, TEXT("  tscn      skipped on %s, the offer comes back next time this map opens"), *R.AskedAt);
		}
		else
		{
			UE_LOG(LogBF6Project, Display, TEXT("  tscn      never asked. BF6.Project.ImportTscn brings one in."));
		}
	}

	const TSharedPtr<FJsonObject>* E = nullptr;
	if (M->TryGetObjectField(TEXT("experience"), E) && E)
	{
		FString Id, Nm;
		(*E)->TryGetStringField(TEXT("id"), Id);
		(*E)->TryGetStringField(TEXT("name"), Nm);
		UE_LOG(LogBF6Project, Display, TEXT("  linked to %s %s"), *Id, *Nm);
	}
	else
	{
		UE_LOG(LogBF6Project, Display, TEXT("  linked to nothing. BF6.Project.Compare finds a candidate."));
	}
}

// ---------------------------------------------------------------------------
void BF6Project::Register()
{
	// Before anything reads a save: bring across whatever the previous layout
	// left beside saves/ instead of inside it.
	MoveExperiencesIntoSaves();

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Status"),
		TEXT("What the project folder of a save holds. No argument uses the open save."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			LogStatus(Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : FString());
		}));

	// Deleting a map leaves the game mode standing, on purpose. Getting rid of
	// the game mode itself needs its own command, and it did not have one: an
	// imported experience could not be removed at all, only emptied one map at
	// a time while it went on listing itself as imported.
	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.RemoveExperience"),
		TEXT("Remove a whole experience: its script, settings, workspace and every map. A copy is kept. Add the word go to actually do it; without it this only reports."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			TArray<FString> A = Args;
			bool bGo = false;
			for (int32 i = A.Num() - 1; i >= 0; i--)
				if (A[i].Equals(TEXT("go"), ESearchCase::IgnoreCase)) { bGo = true; A.RemoveAt(i); }

			const FString Save = A.Num() > 0 ? FString::Join(A, TEXT(" ")) : BF6Api::CurrentSave();
			if (Save.IsEmpty())
			{
				UE_LOG(LogBF6Project, Warning,
					TEXT("usage: BF6.Project.RemoveExperience <experience> [go]"));
				return;
			}

			const FRemoval R = DeleteExperience(Save, !bGo);
			if (!R.Why.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("%s"), *R.Why); return; }
			if (!bGo)
			{
				UE_LOG(LogBF6Project, Display,
					TEXT("'%s' holds %d map(s), %.1f MB. Nothing was deleted."),
					*R.Save, R.Maps, (double)R.Bytes / (1024.0 * 1024.0));
				UE_LOG(LogBF6Project, Display,
					TEXT("Add the word go to remove it. A copy is kept either way."));
				return;
			}
			UE_LOG(LogBF6Project, Display,
				TEXT("Removed '%s' and its %d map(s). The copy is at %s"),
				*R.Save, R.Maps, *R.Snapshot);
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Compare"),
		TEXT("Match a project against the Portal experiences on this machine. No argument uses the open save; the word all does every project."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Want = Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : BF6Api::CurrentSave();
			if (Want.Equals(TEXT("all"), ESearchCase::IgnoreCase)) { CompareAll(); return; }
			if (Want.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("usage: BF6.Project.Compare <save name>, or open a save first.")); return; }
			LogCompare(Want);
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Link"),
		TEXT("Attach a project to a Portal experience: BF6.Project.Link <uuid> [save name]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() == 0) { UE_LOG(LogBF6Project, Warning, TEXT("usage: BF6.Project.Link <uuid> [save name]")); return; }
			const FString Save = Args.Num() > 1 ? FString::Join(TArray<FString>(Args.GetData() + 1, Args.Num() - 1), TEXT(" ")) : BF6Api::CurrentSave();
			if (Save.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("no save named, and nothing is open.")); return; }
			Link(Save, Args[0]);
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Unlink"),
		TEXT("Forget which experience a project belongs to. Every file it has stays."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Save = Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : BF6Api::CurrentSave();
			if (Save.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("usage: BF6.Project.Unlink <save name>")); return; }
			Unlink(Save);
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Migrate"),
		TEXT("Bring a save up to a full project now, waiting for the template copy. No argument uses the open save."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Save = Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : BF6Api::CurrentSave();
			if (Save.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("usage: BF6.Project.Migrate <save name>")); return; }
			Ensure(BF6Api::CurrentSave() == Save ? BF6Api::CurrentLevel() : FString(), Save, true);
			LogStatus(Save);
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Sync"),
		TEXT("Decide, one artefact at a time, where a project and its Portal experience disagree. No argument uses the open save."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Save = Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : BF6Api::CurrentSave();
			if (Save.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("usage: BF6.Project.Sync <save name>, or open a save first.")); return; }
			Sync(Save);
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Backups"),
		TEXT("Project snapshots. No argument lists them; 'restore <stamp>' puts one back; 'max <n>' sets how many rotating slots are kept."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Save = BF6Api::CurrentSave();
			if (Args.Num() >= 2 && Args[0].Equals(TEXT("max"), ESearchCase::IgnoreCase))
			{
				SetBackupMax(FCString::Atoi(*Args[1]));
				UE_LOG(LogBF6Project, Display, TEXT("rotating project snapshots kept: %d"), BackupMax());
				return;
			}
			if (Save.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("open a save first.")); return; }
			if (Args.Num() >= 2 && Args[0].Equals(TEXT("restore"), ESearchCase::IgnoreCase))
			{
				// The restore says what it did in one sentence, and that
				// sentence is the answer. Reprinting a file count beside it
				// would be exactly the "restored 9 file(s)" line that made a
				// partial restore look like an ordinary one.
				const FRestore Res = RestoreBackupChecked(Save, Args[1]);
				UE_LOG(LogBF6Project, Display, TEXT("%s"), *Res.What);
				return;
			}
			if (Args.Num() >= 1 && Args[0].Equals(TEXT("now"), ESearchCase::IgnoreCase))
			{
				const FSnapshot S = SnapshotProjectChecked(Save, TEXT("asked for by hand"));
				if (S.Stamp.IsEmpty())
				{
					UE_LOG(LogBF6Project, Display, TEXT("No snapshot was taken: %s."),
						S.Why.IsEmpty() ? TEXT("no reason was given") : *S.Why);
				}
				else if (S.bComplete)
				{
					UE_LOG(LogBF6Project, Display, TEXT("Snapshot %s: all %d file(s)."), *S.Stamp, S.Files);
				}
				else
				{
					UE_LOG(LogBF6Project, Warning,
						TEXT("Snapshot %s is INCOMPLETE: %d of %d file(s). It will never be allowed to delete anything on restore."),
						*S.Stamp, S.Files, S.Expected);
				}
				return;
			}
			LogBackups(Save);
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Experiences"),
		TEXT("The experience projects on this machine and the maps in each. 'migrate' turns the old flat per-map saves into maps of their experience; 'preview' says what that would do and changes nothing."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() > 0 && Args[0].Equals(TEXT("preview"), ESearchCase::IgnoreCase)) { MigrateFlatSaves(true); return; }
			if (Args.Num() > 0 && Args[0].Equals(TEXT("migrate"), ESearchCase::IgnoreCase)) { MigrateFlatSaves(false); return; }
			const TArray<FString> Fs = ExperienceFolders();
			if (Fs.Num() == 0) { UE_LOG(LogBF6Project, Display, TEXT("no experience projects yet. BF6.Project.Experiences migrate makes them out of the flat saves.")); return; }
			for (const FString& F : Fs)
			{
				const TArray<FString> Maps = MapsIn(F);
				UE_LOG(LogBF6Project, Display, TEXT("%-46s %2d map(s)  %s"), *F, Maps.Num(), *FString::Join(Maps, TEXT(", ")));
			}
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.MoveToExperience"),
		TEXT("Move the open map's save into an experience project: BF6.Project.MoveToExperience <experience folder>. Everything is copied and checked before anything is removed."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Save = BF6Api::CurrentSave(), Level = BF6Api::CurrentLevel();
			if (Save.IsEmpty() || Level.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("open a saved map first.")); return; }
			if (Args.Num() == 0) { UE_LOG(LogBF6Project, Warning, TEXT("usage: BF6.Project.MoveToExperience <experience folder>. BF6.Project.Experiences lists them.")); return; }
			FString What;
			MoveIntoExperience(Level, Save, FString::Join(Args, TEXT(" ")), What);
			UE_LOG(LogBF6Project, Display, TEXT("%s"), *What);
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.MoveOutOfExperience"),
		TEXT("Take the open map out of its experience and make it a standalone save again: BF6.Project.MoveOutOfExperience [new save name]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Save = BF6Api::CurrentSave(), Level = BF6Api::CurrentLevel();
			if (Save.IsEmpty() || Level.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("open a saved map first.")); return; }
			FString What;
			MoveOutOfExperience(Level, Save, Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : FString(), What);
			UE_LOG(LogBF6Project, Display, TEXT("%s"), *What);
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.ImportTscn"),
		TEXT("Bring in the Godot .tscn the open map's spatial was authored from, restoring its names, grouping and pivots: BF6.Project.ImportTscn [path]. No path opens a file picker."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Save = BF6Api::CurrentSave();
			if (Save.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("open a map first.")); return; }
			ImportTscn(Save, Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : FString());
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Export"),
		TEXT("Write a project out as the site's whole experience JSON: BF6.Project.Export [path]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Save = BF6Api::CurrentSave();
			if (Save.IsEmpty()) { UE_LOG(LogBF6Project, Warning, TEXT("open a save first.")); return; }
			ExportExperienceJson(Save, Args.Num() > 0 ? Args[0] : FString());
		}));

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("BF6.Project.Import"),
		TEXT("Read a whole experience JSON in as a new project: BF6.Project.Import [path]."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FString Made;
			ImportExperienceJson(Args.Num() > 0 ? Args[0] : FString(), Made);
		}));
}

void BF6Project::Unregister()
{
	static const TCHAR* Names[] =
	{
		TEXT("BF6.Project.Status"), TEXT("BF6.Project.Compare"), TEXT("BF6.Project.Link"),
		TEXT("BF6.Project.Unlink"), TEXT("BF6.Project.Migrate"), TEXT("BF6.Project.Export"),
		TEXT("BF6.Project.Import"), TEXT("BF6.Project.Sync"), TEXT("BF6.Project.Backups"),
		TEXT("BF6.Project.ImportTscn"), TEXT("BF6.Project.Experiences"),
		TEXT("BF6.Project.MoveToExperience"), TEXT("BF6.Project.MoveOutOfExperience")
	};
	for (const TCHAR* N : Names)
		if (IConsoleObject* O = IConsoleManager::Get().FindConsoleObject(N))
			IConsoleManager::Get().UnregisterConsoleObject(O);
}
