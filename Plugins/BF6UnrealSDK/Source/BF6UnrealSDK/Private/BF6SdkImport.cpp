// ---------------------------------------------------------------------------
// SDK data import: turn the user's unzipped Portal SDK into the tool's data
// packs.
//
// Catalogue jsons are copied, base setups are parsed natively from
// levels/*.tscn, and the mesh packs are extracted by driving the SDK's own
// bundled Godot headlessly with generated scripts (skip-existing + capped
// batches + relaunch-on-crash, the proven recipe).
//
// Split out of BF6UnrealSDK.cpp because it is a ONE-TIME setup pipeline rather
// than part of the running tool, and because the seam was nearly free: only
// g_pluginDir, Notify, the log category and BF6_SnapshotSdkHistory cross it.
// ---------------------------------------------------------------------------

#include "BF6Internal.h"
#include "BF6BuildMode.h"   // StoredSdkRoot / ValidateSdkRoot

#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Internationalization/Regex.h"

FString BF6_DataDir() { return FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data")); }

FBF6Import g_imp;

int32 BF6_CountFiles(const FString& Dir, const TCHAR* Pattern)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / Pattern), true, false);
	return Files.Num();
}

FString BF6_ReadSdkVersion(const FString& JsonPath)
{
	FString In, Ver;
	if (!FFileHelper::LoadFileToString(In, *JsonPath)) return Ver;
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (FJsonSerializer::Deserialize(R, Root) && Root.IsValid()) Root->TryGetStringField(TEXT("version"), Ver);
	return Ver;
}

// Find the Godot exe the SDK ships at its root (name carries the version).
FString BF6_FindSdkGodot(const FString& SdkRoot)
{
	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *(SdkRoot / TEXT("Godot*win64*.exe")), true, false);
	// prefer the non-console build
	for (const FString& F : Found) if (!F.Contains(TEXT("console"))) return SdkRoot / F;
	return Found.Num() ? SdkRoot / Found[0] : FString();
}

// ---- base setups: parse levels/MP_*.tscn into <map>.base.json (native port of
// the extraction script - plain text parsing, no Godot needed) ----
void BF6_ExtractBaseSetups(const FString& SdkRoot)
{
	const FString LevelsDir = SdkRoot / TEXT("GodotProject/levels");
	const FString OutDir    = BF6_DataDir() / TEXT("basesetup");
	IFileManager::Get().MakeDirectory(*OutDir, true);
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(LevelsDir / TEXT("MP_*.tscn")), true, false);

	const FRegexPattern ExtPat(TEXT("\\[ext_resource[^\\]]*path=\"res://([^\"]+)\"[^\\]]*id=\"([^\"]+)\""));

	auto Nums = [](const FString& S)
	{
		TArray<FString> Parts; S.ParseIntoArray(Parts, TEXT(","));
		TArray<double> Out; for (FString& P : Parts) Out.Add(FCString::Atod(*P.TrimStartAndEnd()));
		return Out;
	};
	auto Between = [](const FString& S, const FString& A, const FString& B, int32 From = 0) -> FString
	{
		const int32 i = S.Find(A, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (i == INDEX_NONE) return FString();
		const int32 j = S.Find(B, ESearchCase::CaseSensitive, ESearchDir::FromStart, i + A.Len());
		if (j == INDEX_NONE) return FString();
		return S.Mid(i + A.Len(), j - i - A.Len());
	};

	for (const FString& File : Files)
	{
		const FString Level = FPaths::GetBaseFilename(File);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *(LevelsDir / File))) continue;

		// ext_resource id -> type leaf (the placeable type the node instances)
		TMap<FString, FString> Ext;
		FRegexMatcher EM(ExtPat, Text);
		while (EM.FindNext()) Ext.Add(EM.GetCaptureGroup(2), FPaths::GetBaseFilename(EM.GetCaptureGroup(1)));

		// split into [node ...] blocks
		TArray<FString> Lines; Text.ParseIntoArrayLines(Lines, false);
		TArray<TSharedPtr<FJsonValue>> Objects;
		for (int32 li = 0; li < Lines.Num(); li++)
		{
			if (!Lines[li].StartsWith(TEXT("[node "))) continue;
			const FString Header = Lines[li];
			FString Body;
			for (int32 bj = li + 1; bj < Lines.Num() && !Lines[bj].StartsWith(TEXT("[node ")); bj++)
				Body += Lines[bj] + TEXT("\n");

			const FString Name   = Between(Header, TEXT("name=\""), TEXT("\""));
			const FString Inst   = Between(Header, TEXT("instance=ExtResource(\""), TEXT("\")"));
			const FString BType  = Between(Header, TEXT("type=\""), TEXT("\""));
			const FString Parent = Between(Header, TEXT("parent=\""), TEXT("\""));
			FString Type = !Inst.IsEmpty() && Ext.Contains(Inst) ? Ext[Inst] : (!BType.IsEmpty() ? BType : TEXT("Node3D"));
			if (Name == TEXT("Static") || Name.EndsWith(TEXT("_Terrain")) || Name.EndsWith(TEXT("_Assets"))) continue;

			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), Name);
			O->SetStringField(TEXT("type"), Type);
			if (!Parent.IsEmpty()) O->SetStringField(TEXT("parent"), Parent);

			const FString Xf = Between(Body, TEXT("transform = Transform3D("), TEXT(")"));
			if (!Xf.IsEmpty())
			{
				const TArray<double> V = Nums(Xf);
				if (V.Num() == 12)
				{
					TArray<TSharedPtr<FJsonValue>> B9, P3;
					for (int32 k = 0; k < 9; k++)  B9.Add(MakeShared<FJsonValueNumber>(V[k]));
					for (int32 k = 9; k < 12; k++) P3.Add(MakeShared<FJsonValueNumber>(V[k]));
					O->SetArrayField(TEXT("basis"), B9);
					O->SetArrayField(TEXT("pos"), P3);
				}
			}
			const FString Pts = Between(Body, TEXT("points = PackedVector2Array("), TEXT(")"));
			if (!Pts.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> PA;
				for (double d : Nums(Pts)) PA.Add(MakeShared<FJsonValueNumber>(d));
				O->SetArrayField(TEXT("points"), PA);
			}
			// scalar + link properties, parsed PER LINE. (The old anchored regex
			// only ever matched the first body line, so HQ spawn lists, teams,
			// and volume links silently went missing - base setups arrived
			// placed but unwired.)
			TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
			TArray<FString> BodyLines;
			Body.ParseIntoArrayLines(BodyLines, false);
			for (const FString& BL : BodyLines)
			{
				const int32 Eq = BL.Find(TEXT(" = "));
				if (Eq == INDEX_NONE) continue;
				const FString K = BL.Left(Eq).TrimStartAndEnd();
				if (K.IsEmpty() || K == TEXT("transform") || K == TEXT("points") || K == TEXT("script") || K.StartsWith(TEXT("metadata"))) continue;
				bool bIdent = true;
				for (const TCHAR C : K) if (!FChar::IsAlnum(C) && C != TEXT('_')) { bIdent = false; break; }
				if (!bIdent) continue;
				Props->SetStringField(K, BL.Mid(Eq + 3).TrimStartAndEnd());
			}
			O->SetObjectField(TEXT("props"), Props);
			Objects.Add(MakeShared<FJsonValueObject>(O));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("level"), Level);
		Root->SetArrayField(TEXT("objects"), Objects);
		FString Out;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), W);
		FFileHelper::SaveStringToFile(Out, *(OutDir / (Level + TEXT(".base.json"))));
	}
	UE_LOG(LogBF6, Warning, TEXT("Base setups extracted for %d levels."), Files.Num());
}

// base.json format 2 = per-line props with the full link/team data (format 1
// only captured each object's first property, so base setups arrived
// unwired). Existing installs regenerate once, silently, from their SDK.
void BF6_EnsureBaseSetupFormat()
{
	const FString Stamp = BF6_DataDir() / TEXT("basesetup") / TEXT(".format2");
	if (FPaths::FileExists(Stamp)) return;
	const FString Root = BF6Api::StoredSdkRoot();
	FString Err;
	if (Root.IsEmpty() || !BF6Api::ValidateSdkRoot(Root, Err)) return;   // the next SDK import regenerates anyway
	BF6_ExtractBaseSetups(Root);
	FFileHelper::SaveStringToFile(TEXT("2"), *Stamp);
}

// ---- the generated Godot extraction scripts (skip-existing, so re-sync after
// an SDK update only converts what's new) ----
FString BF6_ObjectScript(const FString& OutDir, int32 Shard = 0, int32 Shards = 1)
{
	FString S = TEXT(
		"extends SceneTree\n"
		"func _initialize():\n"
		"\tvar OUT := \"__OUT__\"\n"
		"\tDirAccess.make_dir_recursive_absolute(OUT)\n"
		"\tvar probe := FileAccess.open(OUT + \"/write_test.tmp\", FileAccess.WRITE)\n"
		"\tif probe == null:\n"
		"\t\tprint(\"WRITE BLOCKED to \", OUT, \" err=\", FileAccess.get_open_error(), \" - antivirus or Controlled Folder Access is likely blocking Godot\")\n"
		"\t\tquit(); return\n"
		"\tprobe.close()\n"
		"\tDirAccess.remove_absolute(OUT + \"/write_test.tmp\")\n"
		"\tvar d := DirAccess.open(\"res://raw/models\")\n"
		"\tif d == null: print(\"no raw/models\"); quit(); return\n"
		"\tvar names: Array[String] = []\n"
		"\tfor f in d.get_files():\n"
		"\t\tif f.ends_with(\".glb.import\"): names.append(f.substr(0, f.length() - 7))\n"
		"\trandomize(); names.shuffle()\n"
		"\tprint(\"engine \", Engine.get_version_info().string, \"  models found: \", names.size())\n"
		"\tvar did := 0\n"
		"\tvar skipped := 0\n"
		"\tvar no_import := 0\n"
		"\tvar load_fail := 0\n"
		"\tvar write_fail := 0\n"
		"\tfor gname in names:\n"
		"\t\tif (gname.hash() % __SHARDS__) != __SHARD__: continue\n"
		"\t\tvar outp := \"%s/%s.bf6mesh\" % [OUT, gname.get_basename()]\n"
		"\t\tif FileAccess.file_exists(outp): skipped += 1; continue\n"
		"\t\tvar path := \"res://raw/models/\" + gname\n"
		"\t\tif not ResourceLoader.exists(path): no_import += 1; continue\n"
		"\t\tvar ps: PackedScene = ResourceLoader.load(path, \"\", ResourceLoader.CACHE_MODE_IGNORE)\n"
		"\t\tif ps == null: load_fail += 1; continue\n"
		"\t\tvar root := ps.instantiate()\n"
		"\t\tvar ok := _dump(root, outp, false)\n"
		"\t\troot.free()\n"
		"\t\tif not ok: write_fail += 1; continue\n"
		"\t\tdid += 1\n"
		"\t\tif did >= 1200: print(\"BATCH converted=\", did); quit(); return\n"
		"\tprint(\"DONE converted=\", did, \" already_done=\", skipped, \" no_import_cache=\", no_import, \" load_failed=\", load_fail, \" write_failed=\", write_fail)\n"
		"\tquit()\n");
	S += TEXT(
		"func _dump(root: Node, out_path: String, use_global: bool) -> bool:\n"
		"\t# accumulate parent transforms manually: global_transform is wrong for\n"
		"\t# nested nodes when the scene was never added to a tree (headless)\n"
		"\tvar meshes: Array = []\n"
		"\t_collect(root, Transform3D.IDENTITY, meshes)\n"
		"\tvar surfaces: Array = []\n"
		"\tfor pairm in meshes:\n"
		"\t\tvar m: Mesh = pairm[0].mesh\n"
		"\t\tif m == null: continue\n"
		"\t\tvar xf: Transform3D = pairm[1]\n"
		"\t\tfor si in m.get_surface_count(): surfaces.append([m.surface_get_arrays(si), xf])\n"
		"\tif surfaces.is_empty(): return false\n"
		"\tvar sp := StreamPeerBuffer.new()\n"
		"\tsp.big_endian = false\n"
		"\tsp.put_u32(0x42463653); sp.put_u32(1); sp.put_u32(surfaces.size())\n"
		"\tfor pair in surfaces:\n"
		"\t\tvar arr: Array = pair[0]\n"
		"\t\tvar xf: Transform3D = pair[1]\n"
		"\t\tvar verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]\n"
		"\t\tvar norms = arr[Mesh.ARRAY_NORMAL]\n"
		"\t\tvar uvs = arr[Mesh.ARRAY_TEX_UV]\n"
		"\t\tvar idx = arr[Mesh.ARRAY_INDEX]\n"
		"\t\tvar vc := verts.size()\n"
		"\t\tvar indices: PackedInt32Array\n"
		"\t\tif idx == null:\n"
		"\t\t\tindices = PackedInt32Array(); indices.resize(vc)\n"
		"\t\t\tfor k in vc: indices[k] = k\n"
		"\t\telse: indices = idx\n"
		"\t\tsp.put_u32(vc); sp.put_u32(indices.size())\n"
		"\t\tfor k in vc:\n"
		"\t\t\tvar p: Vector3 = xf * verts[k]\n"
		"\t\t\tsp.put_float(p.x); sp.put_float(p.y); sp.put_float(p.z)\n"
		"\t\tvar hasN = norms != null and norms.size() == vc\n"
		"\t\tvar hasU = uvs != null and uvs.size() == vc\n"
		"\t\tsp.put_u8(1 if hasN else 0); sp.put_u8(1 if hasU else 0)\n"
		"\t\tif hasN:\n"
		"\t\t\tvar b: Basis = xf.basis\n"
		"\t\t\tfor k in vc:\n"
		"\t\t\t\tvar n: Vector3 = (b * norms[k]).normalized()\n"
		"\t\t\t\tsp.put_float(n.x); sp.put_float(n.y); sp.put_float(n.z)\n"
		"\t\tif hasU:\n"
		"\t\t\tfor k in vc:\n"
		"\t\t\t\tsp.put_float(uvs[k].x); sp.put_float(uvs[k].y)\n"
		"\t\tfor k in indices.size(): sp.put_32(indices[k])\n"
		"\tvar raw: PackedByteArray = sp.data_array\n"
		"\tvar comp: PackedByteArray = raw.compress(FileAccess.COMPRESSION_GZIP)\n"
		"\tvar fa := FileAccess.open(out_path, FileAccess.WRITE)\n"
		"\tif fa == null: return false\n"
		"\tfa.store_32(0x5A364642)\n"
		"\tfa.store_32(raw.size())\n"
		"\tfa.store_buffer(comp)\n"
		"\tfa.close()\n"
		"\treturn true\n"
		"func _collect(node: Node, xf: Transform3D, out: Array) -> void:\n"
		"\tvar acc := xf\n"
		"\tif node is Node3D: acc = xf * (node as Node3D).transform\n"
		"\tif node is MeshInstance3D and node.mesh != null: out.append([node, acc])\n"
		"\tfor c in node.get_children(): _collect(c, acc, out)\n");
	S = S.Replace(TEXT("__SHARD__"), *FString::FromInt(Shard))
	     .Replace(TEXT("__SHARDS__"), *FString::FromInt(FMath::Max(Shards, 1)));
	return S.Replace(TEXT("__OUT__"), *OutDir.Replace(TEXT("\\"), TEXT("/")));
}

FString BF6_MapScript(const FString& OutDir, int32 Shard = 0, int32 Shards = 1)
{
	FString S = TEXT(
		"extends SceneTree\n"
		"func _initialize():\n"
		"\tvar OUT := \"__OUT__\"\n"
		"\tDirAccess.make_dir_recursive_absolute(OUT)\n"
		"\tvar d := DirAccess.open(\"res://static\")\n"
		"\tif d == null: print(\"no static\"); quit(); return\n"
		"\tvar levels := {}\n"
		"\tfor f in d.get_files():\n"
		"\t\tif f.ends_with(\"_Terrain.tscn\"): levels[f.replace(\"_Terrain.tscn\", \"\")] = true\n"
		"\tfor lvl in levels.keys():\n"
		"\t\tif (String(lvl).hash() % __SHARDS__) != __SHARD__: continue\n"
		"\t\t_do(OUT, lvl, \"Terrain\")\n"
		"\t\t_do(OUT, lvl, \"Assets\")\n"
		"\tprint(\"DONE\"); quit()\n"
		"func _do(OUT: String, level: String, kind: String) -> void:\n"
		"\tvar outp := \"%s/%s_%s.bf6mesh\" % [OUT, level, kind.to_lower()]\n"
		"\tif FileAccess.file_exists(outp): return\n"
		"\tvar path := \"res://static/%s_%s.tscn\" % [level, kind]\n"
		"\tif not ResourceLoader.exists(path): return\n"
		"\tvar ps: PackedScene = load(path)\n"
		"\tif ps == null: return\n"
		"\tvar root := ps.instantiate()\n"
		"\t_dump(root, outp, true)\n"
		"\troot.free()\n"
		"\tprint(level, \" \", kind, \" done\")\n");
	// same _dump/_collect as the object script
	FString Obj = BF6_ObjectScript(OutDir);
	const int32 At = Obj.Find(TEXT("func _dump"));
	S += Obj.Mid(At);
	S = S.Replace(TEXT("__SHARD__"), *FString::FromInt(Shard))
	     .Replace(TEXT("__SHARDS__"), *FString::FromInt(FMath::Max(Shards, 1)));
	return S.Replace(TEXT("__OUT__"), *OutDir.Replace(TEXT("\\"), TEXT("/")));
}

static void BF6_CloseGodotPipe();   // fwd (defined with the launch helpers below)

void BF6_ImportFail(const FString& Why)
{
	BF6_CloseGodotPipe();
	g_imp.Phase = FBF6Import::EPhase::Failed;
	g_imp.Status = FString::Printf(TEXT("Import failed: %s"), *Why);
	if (g_imp.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(g_imp.Tick); g_imp.Tick.Reset(); }
	Notify(FString::Printf(TEXT("SDK import failed: %s"), *Why));
}

// Everything the headless Godot prints lands here, so a machine where the
// conversion produces nothing is diagnosable from a tester's report.
FString BF6_ImportLogPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("import"), TEXT("godot_run.log"));
}
void BF6_ImportLog(const FString& Line)
{
	FFileHelper::SaveStringToFile(Line + LINE_TERMINATOR, *BF6_ImportLogPath(),
		FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}
static void BF6_DrainGodotPipe()
{
	for (void* RP : g_imp.PipeReads)
		if (RP)
		{
			const FString Out = FPlatformProcess::ReadPipe(RP);
			if (!Out.IsEmpty()) BF6_ImportLog(Out.TrimEnd());
		}
}
static void BF6_CloseGodotPipe()
{
	BF6_DrainGodotPipe();
	for (int32 i = 0; i < g_imp.PipeReads.Num(); i++)
		if (g_imp.PipeReads[i] || g_imp.PipeWrites[i])
			FPlatformProcess::ClosePipe(g_imp.PipeReads[i], g_imp.PipeWrites[i]);
	g_imp.PipeReads.Reset();
	g_imp.PipeWrites.Reset();
}

// Launch every worker for the phase: extract_objects_0..N.gd (or maps).
// ABSOLUTE paths only: Godot 4.6 resolves a relative --script against
// res:// (the SDK project), not the working directory. Managed SDK
// downloads live under the project's Saved dir, whose UE paths are
// relative ("../../../Users/...") - every fresh-install user hit
// "Can't load script" on the object conversion because of this.
bool BF6_LaunchGodotWorkers(bool bObjects)
{
	const FString ScriptDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("import"));
	const int32 Shards = bObjects ? kBF6ObjShards : kBF6MapShards;
	for (int32 s = 0; s < Shards; s++)
	{
		const FString Script = ScriptDir / FString::Printf(TEXT("extract_%s_%d.gd"), bObjects ? TEXT("objects") : TEXT("maps"), s);
		const FString Args = FString::Printf(TEXT("--headless --path \"%s\" --script \"%s\""),
			*FPaths::ConvertRelativePathToFull(g_imp.SdkRoot / TEXT("GodotProject")),
			*FPaths::ConvertRelativePathToFull(Script));
		void* RP = nullptr; void* WP = nullptr;
		FPlatformProcess::CreatePipe(RP, WP);
		BF6_ImportLog(FString::Printf(TEXT("--- launching worker %d: \"%s\" %s"), s, *g_imp.GodotExe, *Args));
		FProcHandle H = FPlatformProcess::CreateProc(*FPaths::ConvertRelativePathToFull(g_imp.GodotExe), *Args, false, true, true, nullptr, 0, nullptr, WP);
		if (!H.IsValid())
		{
			BF6_ImportLog(FString::Printf(TEXT("--- worker %d launch FAILED (CreateProc)"), s));
			FPlatformProcess::ClosePipe(RP, WP);
			continue;
		}
		g_imp.Procs.Add(H);
		g_imp.PipeReads.Add(RP);
		g_imp.PipeWrites.Add(WP);
	}
	return g_imp.Procs.Num() > 0;
}

void BF6_ImportTickPhase()
{
	const FString ObjDir = BF6_DataDir() / TEXT("objmodels");
	const FString MapDir = BF6_DataDir() / TEXT("mapmesh");
	const bool bObjects = g_imp.Phase == FBF6Import::EPhase::Objects;
	const FString OutDir = bObjects ? ObjDir : MapDir;
	const int32 Target = bObjects ? g_imp.ObjTotal : g_imp.MapTotal;
	const int32 Count = BF6_CountFiles(OutDir, TEXT("*.bf6mesh"));

	// any worker still running: stream output to the log + report progress
	bool bAnyRunning = false;
	for (FProcHandle& H : g_imp.Procs)
		if (H.IsValid() && FPlatformProcess::IsProcRunning(H)) { bAnyRunning = true; break; }
	if (bAnyRunning)
	{
		BF6_DrainGodotPipe();
		g_imp.Status = FString::Printf(TEXT("%s  %d / %d"), bObjects ? TEXT("Converting object models") : TEXT("Converting map meshes"), Count, Target);
		// objects are ~90%% of the work
		const float PhaseFrac = Target > 0 ? (float)Count / (float)Target : 0.f;
		g_imp.Frac = bObjects ? 0.05f + 0.80f * PhaseFrac : 0.85f + 0.15f * PhaseFrac;
		return;
	}
	if (g_imp.Procs.Num())
	{
		// exit codes separate "antivirus killed it instantly" (non-zero /
		// access violation right away) from "ran fine but converted nothing"
		FString Codes;
		for (FProcHandle& H : g_imp.Procs)
		{
			int32 Code = -1;
			if (H.IsValid()) FPlatformProcess::GetProcReturnCode(H, &Code);
			Codes += FString::Printf(TEXT("%d "), Code);
			if (H.IsValid()) FPlatformProcess::CloseProc(H);
		}
		BF6_ImportLog(FString::Printf(TEXT("--- godot workers exited, codes: %s- %d files so far"), *Codes, Count));
		g_imp.Procs.Reset();
	}
	BF6_CloseGodotPipe();

	// the batch/crash-resume loop: relaunch while progress is being made
	const bool bComplete = Count >= Target || (Count == g_imp.LastCount && ++g_imp.Stagnant >= 2);
	if (Count > g_imp.LastCount) g_imp.Stagnant = 0;
	g_imp.LastCount = Count;
	if (!bComplete)
	{
		if (!BF6_LaunchGodotWorkers(bObjects)) { BF6_ImportFail(TEXT("could not relaunch the SDK's Godot")); }
		return;
	}

	// a phase that converted NOTHING new and is short of its target is a
	// failure, not a completion - say so instead of pretending success
	// (the classic cause: the SDK project was opened in the user's own newer
	// Godot, which rebuilt the model cache in a format the SDK's bundled
	// Godot cannot read)
	if (Count == g_imp.StartCount && Count < Target && Target > 0)
	{
		BF6_ImportFail(FString::Printf(
			TEXT("the SDK's Godot converted nothing new (%d of %d %s). If this SDK was ever opened in your own Godot, its model cache no longer matches the SDK's bundled Godot: open GodotProject once with the Godot exe in the SDK folder, let the import finish, close it, then use Full re-sync here. Please report this with Saved/BF6UnrealSDK/import/godot_run.log attached."),
			Count, Target, bObjects ? TEXT("models") : TEXT("map meshes")));
		return;
	}

	if (bObjects)
	{
		// objects finished (or gave all they can) - move to the map meshes
		UE_LOG(LogBF6, Warning, TEXT("Object models: %d of %d converted."), Count, Target);
		g_imp.ObjDone = Count;
		g_imp.Phase = FBF6Import::EPhase::Maps;
		g_imp.LastCount = BF6_CountFiles(MapDir, TEXT("*.bf6mesh"));
		g_imp.StartCount = g_imp.LastCount;
		g_imp.Stagnant = 0;
		if (!BF6_LaunchGodotWorkers(false)) BF6_ImportFail(TEXT("could not launch the SDK's Godot"));
		return;
	}

	// all done - stamp the SDK version only now, so a failed import is offered
	// a re-sync next launch instead of being recorded as current
	UE_LOG(LogBF6, Warning, TEXT("Map meshes: %d of %d converted."), Count, Target);
	IFileManager::Get().Copy(*(BF6_DataDir() / TEXT("sdk.version.json")), *(g_imp.SdkRoot / TEXT("sdk.version.json")));
	// snapshot for the NEXT update's version-history diff
	BF6Api::BF6_SnapshotSdkHistory(g_imp.SdkRoot);
	g_imp.Phase = FBF6Import::EPhase::Done;
	g_imp.Status = FString::Printf(TEXT("Import complete - %d of %d models, %d of %d map meshes"), g_imp.ObjDone, g_imp.ObjTotal, Count, Target);
	g_imp.Frac = 1.f;
	if (g_imp.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(g_imp.Tick); g_imp.Tick.Reset(); }
	Notify(FString::Printf(TEXT("SDK import complete - %d models and %d map meshes ready."), g_imp.ObjDone, Count));
}
