#include "BF6UnrealSDK.h"

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/MemoryReader.h"
#include "Editor/UnrealEdEngine.h"
#include "LevelEditorViewport.h"
#include "SLevelViewport.h"
#include "Engine/DirectionalLight.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "Components/LightComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "MaterialEditingLibrary.h"
#include "Misc/Compression.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "ScopedTransaction.h"
#include "Misc/ITransaction.h"
#include "EditorActorFolders.h"
#include "ActorEditorUtils.h"
#include "Editor/TransBuffer.h"
#include "LevelEditorViewport.h"
#include "Input/DragAndDrop.h"
#include "ImageUtils.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Camera/CameraComponent.h"
#include "PreviewScene.h"
#include "Styling/SlateBrush.h"
#include "ActorGroupingUtils.h"
#include "Editor/GroupActor.h"
#include "SceneView.h"   // projecting zone points to screen for the dot layer
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"   // GetFileAttributesW / GetDriveTypeW (cloud-drive checks)
#endif
#include "UObject/StrongObjectPtr.h"
#include "Styling/SlateBrush.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "LevelEditor.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Interfaces/IMainFrameModule.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Notifications/SProgressBar.h"

#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/MessageDialog.h"
#include "Async/Async.h"

#include "BF6Internal.h"
#include "BF6MapManifest.h"
#include "BF6BuildMode.h"
#include "BF6ExtensionInternal.h"
#include "BF6Theme.h"
#include "BF6Bridge.h"
#include "SBF6PreviewViewport.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"

THIRD_PARTY_INCLUDES_START
// Kept in ONE block at the top. Three of these used to sit ~4900 lines down,
// which meant everything above them compiled only because a unity blob had
// already pulled them in - and a single-file compile check could never be
// trusted. See docs/BUILDING.md for that check.
#include "Internationalization/Regex.h"
#include "Misc/ConfigCacheIni.h"
#include "Containers/Ticker.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "MaterialDomain.h"
#include "TextureResource.h"
#include "Engine/TextureRenderTarget2D.h"
#include "bf6_core.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY(LogBF6);   // declared in BF6Internal.h

typedef bf6_ctx*  (*bf6_open_fn)(const char*, char*, int);
typedef int       (*bf6_abi_version_fn)(void);
typedef void      (*bf6_close_fn)(bf6_ctx*);
typedef int       (*bf6_catalogue_fn)(bf6_ctx*, const char*, bf6_cat_entry*, int);
typedef bf6_mesh* (*bf6_read_mesh_fn)(bf6_ctx*, const char*, int);
typedef void      (*bf6_free_fn)(bf6_ctx*, void*);
typedef int       (*bf6_load_placeables_fn)(bf6_ctx*, const char*, char*, int);
typedef int       (*bf6_level_count_fn)(bf6_ctx*);
typedef const char* (*bf6_level_name_fn)(bf6_ctx*, int);
typedef int       (*bf6_list_placeables_fn)(bf6_ctx*, const char*, const char*, bf6_placeable*, int);
typedef int       (*bf6_placeable_props_fn)(bf6_ctx*, const char*, bf6_prop*, int);

static bf6_open_fn            g_open   = nullptr;
static bf6_close_fn           g_close  = nullptr;
static bf6_catalogue_fn       g_cat    = nullptr;
static bf6_read_mesh_fn       g_read   = nullptr;
static bf6_free_fn            g_free   = nullptr;
static bf6_load_placeables_fn g_loadp  = nullptr;
static bf6_level_count_fn     g_lvlcnt = nullptr;
static bf6_level_name_fn      g_lvlname= nullptr;
static bf6_list_placeables_fn g_listp  = nullptr;
static bf6_placeable_props_fn g_props  = nullptr;
static bf6_ctx*               g_ctx    = nullptr;
FString                       g_pluginDir;   // shared: BF6Internal.h

static const char* kGameDir =
    "C:/Program Files (x86)/Steam/steamapps/common/Battlefield 6";
// The install the core is actually reading, empty in catalogue-only mode.
// Discovery is still the one Steam path; finding an install properly belongs
// in the core (the Godot plugin's gamedir module), not in two bindings.
static FString                g_gameDir;
static const FName kTabName("BF6Objects");
static const FName kPlacedTag("BF6Placed");
static const FName kContextTag("BF6Context");
static const FName kBaseTag("BF6Base");
// Godot builds its hierarchy out of nodes: an empty Node3D used purely as a
// parent, or an object with children hung off it. Both come back as real
// attachment here rather than as an outliner folder, so a subtree moves and
// deletes as one the way it does in the SDK. This tag marks the stand-in for
// an empty node: no geometry, never exported, purely a parent.
static const FName kGroupTag("BF6Group");

// auto-organized outliner: file each object into its role/category folder
// (defined below; forward-declared so spawn helpers can call it)
static void BF6_FileActor(AActor* A);

// ExploreFolder proved unreliable here: the editor hands it RELATIVE Saved
// paths and ShellExecute silently no-ops on them (or the window opens behind
// the fullscreen editor). explorer.exe with a resolved platform path always
// brings up a window in front.
static void BF6_OpenInExplorer(const FString& InPath, bool bSelectFile)
{
	FString P = FPaths::ConvertRelativePathToFull(InPath);
	FPaths::MakePlatformFilename(P);
	const FString Args = bSelectFile
		? FString::Printf(TEXT("/select,\"%s\""), *P)
		: FString::Printf(TEXT("\"%s\""), *P);
	FPlatformProcess::CreateProc(TEXT("explorer.exe"), *Args, true, false, false, nullptr, 0, nullptr, nullptr);
}

// Godot ENGINE classes that ride along in scenes and base setups as pivots,
// cameras, and editor helpers. They are not Portal placeables - the site
// rejects them with "Provided level contains unknown types" - and Godot's
// own exporter drops them, so we skip them at load, import, and export.
static bool BF6_IsEngineNodeType(const FString& T)
{
	return T == TEXT("Node3D") || T == TEXT("Camera3D") || T == TEXT("Path3D")
		|| T == TEXT("AnimationPlayer") || T == TEXT("Marker3D") || T == TEXT("MeshInstance3D")
		|| T == TEXT("CollisionShape3D") || T == TEXT("CollisionPolygon3D") || T == TEXT("Decal")
		|| T == TEXT("DirectionalLight3D") || T == TEXT("StaticBody3D") || T == TEXT("Area3D")
		|| T == TEXT("AudioStreamPlayer3D") || T == TEXT("Node");
}

// ---- full parent-chain accumulation for base-setup json nodes ----
// Godot semantics: child world = parent_basis * child_local + parent_pos,
// and the basis MULTIPLIES down the chain. The old translation-only sum
// lost every parent rotation - the deploy camera's whole aim lives on a
// Camera3D pivot, and its DeployCam child arrived (and exported) unrotated.
struct FBF6GNode
{
	double B[9] = { 1,0,0, 0,1,0, 0,0,1 };   // row-major Godot basis
	FVector P = FVector::ZeroVector;         // local Godot position
	FString Parent;
};

static void BF6_BuildGNodeMap(const TArray<TSharedPtr<FJsonValue>>& Objs, TMap<FString, FBF6GNode>& Out)
{
	for (const TSharedPtr<FJsonValue>& v : Objs)
	{
		const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
		FBF6GNode N;
		const TArray<TSharedPtr<FJsonValue>>* a = nullptr;
		if (o->TryGetArrayField(TEXT("pos"), a) && a->Num() >= 3)
			N.P = FVector((*a)[0]->AsNumber(), (*a)[1]->AsNumber(), (*a)[2]->AsNumber());
		const TArray<TSharedPtr<FJsonValue>>* b = nullptr;
		if (o->TryGetArrayField(TEXT("basis"), b) && b->Num() >= 9)
			for (int32 i = 0; i < 9; i++) N.B[i] = (*b)[i]->AsNumber();
		o->TryGetStringField(TEXT("parent"), N.Parent);
		Out.Add(o->GetStringField(TEXT("name")), N);
	}
}

static void BF6_GWorldOf(const TMap<FString, FBF6GNode>& M, const FString& Name,
	double OutB[9], FVector& OutP, int32 Depth = 0)
{
	OutB[0] = 1; OutB[1] = 0; OutB[2] = 0;
	OutB[3] = 0; OutB[4] = 1; OutB[5] = 0;
	OutB[6] = 0; OutB[7] = 0; OutB[8] = 1;
	OutP = FVector::ZeroVector;
	if (Name.IsEmpty() || Name == TEXT(".") || Depth > 8) return;
	const FBF6GNode* N = M.Find(Name);
	if (!N) return;
	double PB[9]; FVector PP;
	BF6_GWorldOf(M, N->Parent, PB, PP, Depth + 1);
	for (int32 r = 0; r < 3; r++)
		for (int32 c = 0; c < 3; c++)
			OutB[r * 3 + c] = PB[r * 3 + 0] * N->B[0 * 3 + c]
			                + PB[r * 3 + 1] * N->B[1 * 3 + c]
			                + PB[r * 3 + 2] * N->B[2 * 3 + c];
	OutP.X = PB[0] * N->P.X + PB[1] * N->P.Y + PB[2] * N->P.Z + PP.X;
	OutP.Y = PB[3] * N->P.X + PB[4] * N->P.Y + PB[5] * N->P.Z + PP.Y;
	OutP.Z = PB[6] * N->P.X + PB[7] * N->P.Y + PB[8] * N->P.Z + PP.Z;
}

// accumulated Godot basis -> Unreal rotation / scale (same axis swap the
// per-node converters used: Unreal axes are the Godot COLUMNS, Y/Z swapped)
static FRotator BF6_GRotFromB(const double B[9])
{
	const FVector Ax = FVector(B[0], B[6], B[3]).GetSafeNormal();
	const FVector Ay = FVector(B[2], B[8], B[5]).GetSafeNormal();
	const FVector Az = FVector(B[1], B[7], B[4]).GetSafeNormal();
	return FMatrix(Ax, Ay, Az, FVector::ZeroVector).Rotator();
}
static FVector BF6_GScaleFromB(const double B[9])
{
	const float SX = FVector(B[0], B[3], B[6]).Size();
	const float SY = FVector(B[1], B[4], B[7]).Size();
	const float SZ = FVector(B[2], B[5], B[8]).Size();
	return FVector(FMath::Max(SX, 0.0001f), FMath::Max(SZ, 0.0001f), FMath::Max(SY, 0.0001f));
}

// Pretty outliner names: the raw type/mesh name, no legacy BF6_ prefix, and
// uniquified against the world so name-keyed links stay unambiguous. Link
// identity is still "label minus BF6_", which is a no-op on new names, so
// old saves and blocks (whose stored names were already stripped) keep
// resolving unchanged.
// load-cost counters, reported by the session load line
double GLabelSec = 0.0, GSpawnSec = 0.0, GStatSec = 0.0, GFolderSec = 0.0;

// Naming was 60% of a map load. SetActorLabelUnique walks every actor in the
// world to prove a name is free, so loading N objects costs N*N/2 string
// compares - 3,000 objects meant millions of them, and it climbs steeply with
// map size. The engine's own answer is FCachedActorLabels: build the set once
// and hand it in, so each name is a hash lookup. GBulkLabels points at that
// set for the length of a bulk load and is null the rest of the time, when
// one-off renames should still consult the world.
static FCachedActorLabels* GBulkLabels = nullptr;

static void BF6_SetPrettyLabel(AActor* A, const FString& InName)
{
	if (!A) return;
	FString Nm = InName;
	Nm.RemoveFromStart(TEXT("BF6_"));
	if (Nm.IsEmpty()) Nm = TEXT("Object");
	const double T0 = FPlatformTime::Seconds();
	FActorLabelUtilities::SetActorLabelUnique(A, Nm, GBulkLabels);
	if (GBulkLabels) GBulkLabels->Add(A->GetActorLabel());   // keep the set honest
	GLabelSec += FPlatformTime::Seconds() - T0;
}

// ------------------------------------------------------------------ helpers
static int PlaceableCount(const FString& Level)
{
	if (!g_ctx || !g_listp) return 0;
	return g_listp(g_ctx, Level.IsEmpty() ? "" : TCHAR_TO_UTF8(*Level), "", nullptr, 0);
}

FString BF6_ResolvePlaceableRes(const FString& MeshStem)
{
	if (!g_ctx || !g_cat || MeshStem.IsEmpty()) return FString();
	const FString Q = MeshStem.ToLower();
	const int32 kMax = 96;
	TArray<bf6_cat_entry> Buf; Buf.SetNum(kMax);
	const int total = g_cat(g_ctx, TCHAR_TO_UTF8(*Q), Buf.GetData(), kMax);
	const int n = FMath::Min(total, kMax);
	FString firstMesh;
	for (int i = 0; i < n; i++)
	{
		FString nm = UTF8_TO_TCHAR(Buf[i].res_name);
		if (nm.Contains(TEXT("shadow")) || nm.Contains(TEXT("lod"))) continue;
		if (nm.EndsWith(TEXT("_mesh")))
		{
			if (nm.EndsWith(Q + TEXT("_mesh"))) return nm;
			if (firstMesh.IsEmpty()) firstMesh = nm;
		}
	}
	return firstMesh;
}

bool BF6_ReadMeshInto(UProceduralMeshComponent* Mesh, const FString& ResName, float& OutRadius)
{
	if (!g_ctx || !g_read || !g_free || !Mesh) return false;
	bf6_mesh* m = g_read(g_ctx, TCHAR_TO_UTF8(*ResName), 0);
	if (!m) return false;

	const float cx = (m->aabb_min[0] + m->aabb_max[0]) * 0.5f;
	const float cy = (m->aabb_min[1] + m->aabb_max[1]) * 0.5f;
	const float cz = (m->aabb_min[2] + m->aabb_max[2]) * 0.5f;
	const float M = 100.0f;
	float maxR = 1.0f;
	for (int32 si = 0; si < m->section_count; si++)
	{
		const bf6_section& s = m->sections[si];
		TArray<FVector> V; TArray<FVector> N; TArray<FVector2D> UV; TArray<int32> T;
		V.Reserve(s.vertex_count); T.Reserve(s.index_count);
		const TArray<FLinearColor> NC; const TArray<FProcMeshTangent> NT;
		for (int32 v = 0; v < s.vertex_count; v++)
		{
			const double gx = (s.positions[v * 3 + 0] - cx);
			const double gy = (s.positions[v * 3 + 1] - cy);
			const double gz = (s.positions[v * 3 + 2] - cz);
			FVector P((float)(gx * M), (float)(gz * M), (float)(gy * M));
			V.Add(P);
			maxR = FMath::Max(maxR, (float)P.Size());
			if (s.normals) N.Add(FVector(s.normals[v * 3 + 0], s.normals[v * 3 + 2], s.normals[v * 3 + 1]));
			if (s.uv0) UV.Add(FVector2D(s.uv0[v * 2 + 0], s.uv0[v * 2 + 1]));
		}
		for (int32 i = 0; i < s.index_count; i++) T.Add((int32)s.indices[i]);
		Mesh->CreateMeshSection_LinearColor(si, V, T, N, UV, NC, NT, false);
	}
	OutRadius = maxR;
	g_free(g_ctx, m);
	return true;
}

// Search scoring.
//
// This used to be a plain subsequence match: any name containing the pattern's
// letters IN ORDER counted, so "car" matched ConcreteBarrier and "sedan"
// matched SoukHouse - 741 hits, none of them a car. What people type is a piece
// of the NAME, so a substring is the match and everything else is a fallback:
//
//   - each space-separated word must appear; "car sedan" and "wreck tank" work
//   - a word found as a substring scores by where it sits, best at the start or
//     just after a separator
//   - failing that, scattered letters count only if they stay tight together,
//     which keeps genuine typos working without dragging in the whole library
//
// Measured over the catalogue: "sedan" went from 741 hits led by SoukHouse to
// 113 led by CarSedan_03, and "acacia" from 67 to a single exact hit.
static bool FuzzyScore(const FString& PatternLower, const FString& Str, int32& OutScore)
{
	OutScore = 0;
	if (PatternLower.IsEmpty()) return true;
	const FString S = Str.ToLower();

	TArray<FString> Words;
	PatternLower.ParseIntoArray(Words, TEXT(" "), true);
	if (Words.Num() == 0) return true;

	int32 Total = 0;
	for (const FString& W : Words)
	{
		const int32 At = S.Find(W, ESearchCase::CaseSensitive);
		if (At != INDEX_NONE)
		{
			int32 Sc = 1000 - FMath::Min(At, 200);
			const bool bWordStart = At == 0 || S[At - 1] == '_' || S[At - 1] == ' '
				|| S[At - 1] == '/' || S[At - 1] == '-' || FChar::IsDigit(S[At - 1]);
			if (bWordStart) Sc += 200;
			if (At == 0) Sc += 100;
			Total += Sc;
			continue;
		}
		// no substring: allow a typo only for words long enough to be meant
		if (W.Len() < 4) return false;
		int32 pi = 0, First = -1, Last = -1;
		for (int32 si = 0; si < S.Len() && pi < W.Len(); si++)
			if (S[si] == W[pi]) { if (First < 0) First = si; Last = si; pi++; }
		if (pi != W.Len()) return false;
		if (Last - First + 1 > W.Len() + 4) return false;   // scattered letters are not a match
		Total += 120 - (Last - First);
	}

	OutScore = Total - FMath::Min(S.Len() / 4, 50);   // a tighter name is the better hit
	return true;
}

// Spawn a decoded resource into the level at real BF6 scale, tagged so it can be
// saved. Returns the actor. `Placed` marks it as a user placement (vs context).
static AActor* SpawnResource(const FString& ResName, const FString& Label, const FTransform& Xform, bool Placed = true)
{
	if (!g_ctx || !g_read || !g_free || !GEditor) return nullptr;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) { UE_LOG(LogBF6, Error, TEXT("no editor world - open a level first")); return nullptr; }

	bf6_mesh* m = g_read(g_ctx, TCHAR_TO_UTF8(*ResName), 0);
	if (!m) { UE_LOG(LogBF6, Error, TEXT("read failed for %s (single mesh only; prefabs await the EBX walk)"), *ResName); return nullptr; }

	const float cx = (m->aabb_min[0] + m->aabb_max[0]) * 0.5f;
	const float cy = (m->aabb_min[1] + m->aabb_max[1]) * 0.5f;
	const float cz = (m->aabb_min[2] + m->aabb_max[2]) * 0.5f;
	const float M = 100.0f;

	FActorSpawnParameters SP;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), Xform, SP);
	if (!Actor) { g_free(g_ctx, m); return nullptr; }
	BF6_SetPrettyLabel(Actor, Label.IsEmpty() ? ResName : Label);
	Actor->Tags.Add(kPlacedTag);
	Actor->Tags.Add(FName(*(FString(TEXT("res:")) + ResName)));
	if (!Label.IsEmpty()) Actor->Tags.Add(FName(*(FString(TEXT("label:")) + Label)));

	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Actor, TEXT("ProcMesh"));
	Mesh->SetFlags(RF_Transactional);
	Actor->SetRootComponent(Mesh);
	Mesh->RegisterComponent();
	Actor->AddInstanceComponent(Mesh);
	Actor->SetActorTransform(Xform);   // re-apply: setting the root reset it to origin

	for (int32 si = 0; si < m->section_count; si++)
	{
		const bf6_section& s = m->sections[si];
		TArray<FVector> V; TArray<FVector> N; TArray<FVector2D> UV; TArray<int32> T;
		V.Reserve(s.vertex_count); T.Reserve(s.index_count);
		const TArray<FLinearColor> NC; const TArray<FProcMeshTangent> NT;
		for (int32 v = 0; v < s.vertex_count; v++)
		{
			const double x = (s.positions[v * 3 + 0] - cx) * M;
			const double y = (s.positions[v * 3 + 1] - cy) * M;
			const double z = (s.positions[v * 3 + 2] - cz) * M;
			V.Add(FVector(x, z, y));
			if (s.normals) N.Add(FVector(s.normals[v * 3 + 0], s.normals[v * 3 + 2], s.normals[v * 3 + 1]));
			if (s.uv0) UV.Add(FVector2D(s.uv0[v * 2 + 0], s.uv0[v * 2 + 1]));
		}
		for (int32 i = 0; i < s.index_count; i++) T.Add((int32)s.indices[i]);
		Mesh->CreateMeshSection_LinearColor(si, V, T, N, UV, NC, NT, false);
	}
	Mesh->SetVisibility(true, true);
	if (!Placed)
	{
		Actor->Tags.Remove(kPlacedTag); Actor->Tags.Add(kContextTag);
		// Context is scenery: not clickable in the viewport, foldered out of the
		// way. NOT location-locked - a locked actor never reaches the outliner
		// hierarchy, which is why the map meshes could not be listed at all.
		Mesh->bSelectable = false;
	}
	g_free(g_ctx, m);
	return Actor;
}

static void SpawnResourceAtCursor(const FString& ResName, const FString& Label)
{
	FVector Loc(0, 0, 100);
	if (GCurrentLevelEditingViewportClient)
	{
		const FViewportCursorLocation Cursor = GCurrentLevelEditingViewportClient->GetCursorWorldLocationFromMousePos();
		const FVector O = Cursor.GetOrigin(); const FVector D = Cursor.GetDirection();
		if (!D.IsNearlyZero())
		{
			if (FMath::Abs(D.Z) > 1e-4f) { const float t = -O.Z / D.Z; Loc = (t > 0.f && t < 200000.f) ? (O + D * t) : (O + D * 1000.f); }
			else Loc = O + D * 1000.f;
		}
	}
	AActor* A = SpawnResource(ResName, Label, FTransform(Loc), true);
	if (A && GEditor) { GEditor->SelectNone(false, true, false); GEditor->SelectActor(A, true, true); }
}

// forward decls (definitions live in the base-setup helpers below)
static UProceduralMeshComponent* MakeProcMesh(AActor* A, const FName Name);
static bool FillProcFromBf6Mesh(UProceduralMeshComponent* Mesh, const FString& FilePath, bool bCollision = false, float NormalPush = 0.f);
static class UMaterialInterface* BF6_Material(const TCHAR* Name);   // plugin first, project fallback
extern double GMeshDecodeSec, GMeshBuildSec;   // defined with the mesh cache below
static void ApplyObjectWhite(UProceduralMeshComponent* Mesh);

// ---- low-poly map context: load an extracted .bf6mesh into a proc-mesh actor ----
static void ClearActorsWithTag(FName Tag);   // defined below
static int32 BF6_RebuildTreeFromTags();      // authored tree, hooked up as attachment
static AActor* BF6_SpawnTreeNode(UWorld* W, const FString& Key, const FString& ParentKey,
	const FTransform& Xf, int32 Order = MAX_int32);   // an empty Godot node, as an actor
namespace BF6Api { static bool BF6_IsLinkProp(const FString& TypeName, const FString& PropName); }
namespace BF6Api { static void BF6_MapDecalStash(); }   // fwd: map close stashes the decal shift
static int32 BF6_ApplyTreeMetadata(const TArray<TSharedPtr<FJsonValue>>* StaticArr);   // tree from a spatial file
bool BF6_KeepGodotTree();                    // the creator's outliner preference

// Which level's scenery is currently standing. Reopening or resuming the same
// map is the common loop - build, export, come back - and the terrain and
// assets are identical every time, so they are left in place instead of being
// torn down and rebuilt (which would re-cook collision as well). Switching to
// a different map clears them as before.
static FString GContextLevel;

static bool BF6_ContextAlreadyUp(const FString& Level)
{
	if (Level.IsEmpty() || GContextLevel != Level || !GEditor) return false;
	UWorld* W = GEditor->GetEditorWorldContext().World();
	if (!W) return false;
	for (TActorIterator<AActor> It(W); It; ++It)
		if (It->Tags.Contains(kContextTag)) return true;   // still standing
	return false;
}

// Drop the scenery only when the map actually changes.
// Set by BF6_ClearContextFor: true means the scenery already standing is for
// this map and was kept, so nothing should respawn it. Decided ONCE per map
// open - checking 'is anything standing' per mesh would let the terrain make
// the assets look redundant, and the map would come up without its buildings.
static bool GContextReused = false;

// Drop the scenery only when the map actually changes.
static void BF6_DropRayIndexes();   // defined with the index itself, below

static AActor* BF6_EnsureStaticParent(UWorld* W);   // 'Static', the scenery's parent

// Put the terrain and assets under 'Static'. Called after a fresh spawn AND when
// the scenery is reused: reopening or re-importing the same map deliberately
// leaves it standing, so nothing would ever adopt it otherwise.
static void BF6_GroupContextUnderStatic(const FString& Level)
{
	ON_SCOPE_EXIT{ BF6Api::RefreshSceneTree(); };
	if (!GEditor) return;
	UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
	TArray<AActor*> Loose;
	for (TActorIterator<AActor> It(W); It; ++It)
		if (It->Tags.Contains(kContextTag) && It->GetActorLabel() != TEXT("Static") && !It->GetAttachParentActor())
			Loose.Add(*It);
	if (Loose.Num() == 0) return;
	AActor* Parent = BF6_EnsureStaticParent(W);
	if (!Parent) return;
	for (AActor* A : Loose)
		A->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
	// and Static itself belongs under the map's node, like everything else
	const FString RootKey = FString(TEXT("gpath:")) + Level;
	if (!Parent->GetAttachParentActor() && !Level.IsEmpty())
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(kGroupTag) && It->Tags.Contains(FName(*RootKey)))
			{
				Parent->AttachToActor(*It, FAttachmentTransformRules::KeepWorldTransform);
				break;
			}
}

static void BF6_ClearContextFor(const FString& Level)
{
	GContextReused = BF6_ContextAlreadyUp(Level);
	if (GContextReused) { BF6_GroupContextUnderStatic(Level); return; }
	ClearActorsWithTag(kContextTag);
	BF6_DropRayIndexes();   // the scenery is going, so is the ray index over it
	GContextLevel = Level;
}

// ---- our own ray index for the map's scenery ----
// Cooking physics collision for the terrain and assets meshes was ~97% of the
// cost of putting a map on screen (Battery: 0.12s to build the assets mesh,
// 3.87s to cook it, and the big maps twice that again). Nothing but our own
// rays ever used that collision - placement, the click classifier and
// scatter's follow-terrain - so the cook is gone and those rays are answered
// from a uniform grid over the triangles instead. Building the grid is one
// linear pass over data we have already decoded.
struct FBF6RayIndex
{
	TWeakObjectPtr<AActor> Owner;
	FTransform Xf = FTransform::Identity;
	TArray<FVector3f> V;
	TArray<int32> Tri;              // three entries per triangle
	FBox3f Bounds = FBox3f(ForceInit);
	int32 NX = 0, NY = 0;
	float CX = 1.f, CY = 1.f;
	TArray<int32> CellStart;        // NX*NY + 1, indexes into CellTri
	TArray<int32> CellTri;
	TArray<int32> Stamp;            // a triangle spanning cells is still tested once
	int32 Epoch = 0;

	int32 NumTris() const { return Tri.Num() / 3; }
	int32 CellOfX(float X) const { return FMath::Clamp((int32)((X - Bounds.Min.X) / CX), 0, NX - 1); }
	int32 CellOfY(float Y) const { return FMath::Clamp((int32)((Y - Bounds.Min.Y) / CY), 0, NY - 1); }
	bool Trace(const FVector& WO, const FVector& WEnd, double& OutU, FVector& OutHit, FVector* OutNormal = nullptr);
};
static TArray<TSharedPtr<FBF6RayIndex>> GRayIndexes;
static void BF6_DropRayIndexes() { GRayIndexes.Reset(); }

// Moller-Trumbore, two sided - terrain gets looked at from underneath as often
// as from above.
static FORCEINLINE bool BF6_RayTri(const FVector3f& O, const FVector3f& D,
	const FVector3f& A, const FVector3f& B, const FVector3f& C, float& OutT, FVector3f* OutN = nullptr)
{
	const FVector3f E1 = B - A, E2 = C - A;
	const FVector3f P = FVector3f::CrossProduct(D, E2);
	const float Det = FVector3f::DotProduct(E1, P);
	if (FMath::Abs(Det) < 1e-8f) return false;
	const float Inv = 1.f / Det;
	const FVector3f T = O - A;
	const float U = FVector3f::DotProduct(T, P) * Inv;
	if (U < -1e-5f || U > 1.f + 1e-5f) return false;
	const FVector3f Q = FVector3f::CrossProduct(T, E1);
	const float Vv = FVector3f::DotProduct(D, Q) * Inv;
	if (Vv < -1e-5f || U + Vv > 1.f + 1e-5f) return false;
	const float Dist = FVector3f::DotProduct(E2, Q) * Inv;
	if (Dist < 0.f) return false;
	OutT = Dist;
	if (OutN) *OutN = FVector3f::CrossProduct(E1, E2).GetSafeNormal();
	return true;
}

bool FBF6RayIndex::Trace(const FVector& WO, const FVector& WEnd, double& OutU, FVector& OutHit, FVector* OutNormal)
{
	if (Tri.Num() == 0 || NX <= 0) return false;

	// the grid lives in the mesh's own space, so the segment goes there too;
	// working in segment parameter rather than distance keeps any scale on the
	// component from mattering
	const FVector3f A = (FVector3f)Xf.InverseTransformPosition(WO);
	const FVector3f B = (FVector3f)Xf.InverseTransformPosition(WEnd);
	FVector3f Dir = B - A;
	const float Len = Dir.Size();
	if (Len <= SMALL_NUMBER) return false;
	Dir /= Len;

	// clip against the whole mesh first: most rays miss it outright
	float T0 = 0.f, T1 = Len;
	for (int32 Axis = 0; Axis < 3; Axis++)
	{
		const float d = Dir[Axis], o = A[Axis];
		const float Mn = Bounds.Min[Axis], Mx = Bounds.Max[Axis];
		if (FMath::Abs(d) < 1e-8f) { if (o < Mn || o > Mx) return false; continue; }
		float tA = (Mn - o) / d, tB = (Mx - o) / d;
		if (tA > tB) Swap(tA, tB);
		T0 = FMath::Max(T0, tA);
		T1 = FMath::Min(T1, tB);
		if (T0 > T1) return false;
	}

	const FVector3f Entry = A + Dir * T0;
	int32 X = CellOfX(Entry.X), Y = CellOfY(Entry.Y);
	const int32 StepX = Dir.X > 0.f ? 1 : (Dir.X < 0.f ? -1 : 0);
	const int32 StepY = Dir.Y > 0.f ? 1 : (Dir.Y < 0.f ? -1 : 0);
	float TMaxX = FLT_MAX, TMaxY = FLT_MAX, TDeltaX = FLT_MAX, TDeltaY = FLT_MAX;
	if (StepX != 0)
	{
		const float Edge = Bounds.Min.X + (X + (StepX > 0 ? 1 : 0)) * CX;
		TMaxX = T0 + (Edge - Entry.X) / Dir.X;
		TDeltaX = CX / FMath::Abs(Dir.X);
	}
	if (StepY != 0)
	{
		const float Edge = Bounds.Min.Y + (Y + (StepY > 0 ? 1 : 0)) * CY;
		TMaxY = T0 + (Edge - Entry.Y) / Dir.Y;
		TDeltaY = CY / FMath::Abs(Dir.Y);
	}

	Epoch++;
	float Best = T1;
	bool bAny = false;
	FVector3f BestN(0, 0, 1);

	for (;;)
	{
		const int32 Cell = Y * NX + X;
		for (int32 i = CellStart[Cell], e = CellStart[Cell + 1]; i < e; i++)
		{
			const int32 t = CellTri[i];
			if (Stamp[t] == Epoch) continue;
			Stamp[t] = Epoch;
			float Hit; FVector3f N;
			if (!BF6_RayTri(A, Dir, V[Tri[t * 3]], V[Tri[t * 3 + 1]], V[Tri[t * 3 + 2]], Hit, &N)) continue;
			if (Hit >= T0 - 1.f && Hit <= Best) { Best = Hit; BestN = N; bAny = true; }
		}

		// walking away from a hit we already have: nothing further can beat it
		const float NextT = FMath::Min(TMaxX, TMaxY);
		if (bAny && Best <= NextT) break;
		if (NextT > T1) break;
		if (TMaxX < TMaxY) { X += StepX; if (X < 0 || X >= NX) break; TMaxX += TDeltaX; }
		else               { Y += StepY; if (Y < 0 || Y >= NY) break; TMaxY += TDeltaY; }
	}

	if (!bAny) return false;
	OutU = (double)(Best / Len);
	OutHit = WO + (WEnd - WO) * OutU;
	if (OutNormal)
	{
		*OutNormal = Xf.TransformVectorNoScale(FVector(BestN)).GetSafeNormal();
		if (FVector::DotProduct(*OutNormal, WO - OutHit) < 0.0) *OutNormal = -*OutNormal;   // face the ray
	}
	return true;
}

// Build the grid straight off the component we just filled.
static void BF6_BuildRayIndex(AActor* Owner, UProceduralMeshComponent* M)
{
	if (!Owner || !M) return;
	const double T0 = FPlatformTime::Seconds();

	TSharedPtr<FBF6RayIndex> Ix = MakeShared<FBF6RayIndex>();
	Ix->Owner = Owner;
	Ix->Xf = M->GetComponentTransform();
	for (int32 s = 0; s < M->GetNumSections(); s++)
	{
		FProcMeshSection* Sec = M->GetProcMeshSection(s);
		if (!Sec) continue;
		const int32 Base = Ix->V.Num();
		Ix->V.Reserve(Base + Sec->ProcVertexBuffer.Num());
		for (const FProcMeshVertex& Vx : Sec->ProcVertexBuffer) Ix->V.Add((FVector3f)Vx.Position);
		Ix->Tri.Reserve(Ix->Tri.Num() + Sec->ProcIndexBuffer.Num());
		for (uint32 Idx : Sec->ProcIndexBuffer) Ix->Tri.Add(Base + (int32)Idx);
	}
	const int32 NT = Ix->NumTris();
	if (NT == 0) return;
	for (const FVector3f& P : Ix->V) Ix->Bounds += P;
	Ix->Bounds = Ix->Bounds.ExpandBy(10.f);

	// Cell size follows the geometry rather than the triangle count: terrain
	// triangles are metres across, and a cell smaller than one triangle just
	// lists that triangle over and over, which costs memory and buys nothing.
	// Sample a few thousand, aim a couple of triangles wide, and keep the grid
	// within sane bounds either way.
	const FVector3f Span = Ix->Bounds.Max - Ix->Bounds.Min;
	double SumExtent = 0.0;
	int32 Samples = 0;
	const int32 Stride = FMath::Max(1, NT / 4096);
	for (int32 t = 0; t < NT; t += Stride)
	{
		const FVector3f& A = Ix->V[Ix->Tri[t * 3]];
		const FVector3f& B = Ix->V[Ix->Tri[t * 3 + 1]];
		const FVector3f& C = Ix->V[Ix->Tri[t * 3 + 2]];
		const float dx = FMath::Max3(A.X, B.X, C.X) - FMath::Min3(A.X, B.X, C.X);
		const float dy = FMath::Max3(A.Y, B.Y, C.Y) - FMath::Min3(A.Y, B.Y, C.Y);
		SumExtent += FMath::Max(dx, dy);
		Samples++;
	}
	const float AvgExtent = Samples > 0 ? (float)(SumExtent / Samples) : 100.f;
	const float MaxSpan = FMath::Max(Span.X, Span.Y);
	const float Cell = FMath::Clamp(AvgExtent * 2.f, MaxSpan / 1024.f, MaxSpan / 8.f);
	Ix->NX = FMath::Clamp(FMath::CeilToInt(Span.X / Cell), 1, 1024);
	Ix->NY = FMath::Clamp(FMath::CeilToInt(Span.Y / Cell), 1, 1024);
	Ix->CX = FMath::Max(Span.X / Ix->NX, KINDA_SMALL_NUMBER);
	Ix->CY = FMath::Max(Span.Y / Ix->NY, KINDA_SMALL_NUMBER);

	// counting sort into the cells: count, prefix sum, fill
	const int32 NC = Ix->NX * Ix->NY;
	TArray<int32> Count;
	Count.SetNumZeroed(NC);
	auto CellRange = [&Ix](int32 t, int32& X0, int32& X1, int32& Y0, int32& Y1)
	{
		const FVector3f& A = Ix->V[Ix->Tri[t * 3]];
		const FVector3f& B = Ix->V[Ix->Tri[t * 3 + 1]];
		const FVector3f& C = Ix->V[Ix->Tri[t * 3 + 2]];
		X0 = Ix->CellOfX(FMath::Min3(A.X, B.X, C.X)); X1 = Ix->CellOfX(FMath::Max3(A.X, B.X, C.X));
		Y0 = Ix->CellOfY(FMath::Min3(A.Y, B.Y, C.Y)); Y1 = Ix->CellOfY(FMath::Max3(A.Y, B.Y, C.Y));
	};
	for (int32 t = 0; t < NT; t++)
	{
		int32 X0, X1, Y0, Y1; CellRange(t, X0, X1, Y0, Y1);
		for (int32 y = Y0; y <= Y1; y++) for (int32 x = X0; x <= X1; x++) Count[y * Ix->NX + x]++;
	}
	Ix->CellStart.SetNumUninitialized(NC + 1);
	int32 Run = 0;
	for (int32 c = 0; c < NC; c++) { Ix->CellStart[c] = Run; Run += Count[c]; }
	Ix->CellStart[NC] = Run;
	Ix->CellTri.SetNumUninitialized(Run);
	TArray<int32> Cursor(Ix->CellStart);
	for (int32 t = 0; t < NT; t++)
	{
		int32 X0, X1, Y0, Y1; CellRange(t, X0, X1, Y0, Y1);
		for (int32 y = Y0; y <= Y1; y++) for (int32 x = X0; x <= X1; x++) Ix->CellTri[Cursor[y * Ix->NX + x]++] = t;
	}
	Ix->Stamp.SetNumZeroed(NT);
	GRayIndexes.Add(Ix);

	UE_LOG(LogBF6, Display, TEXT("ray index for %s: %d tris, %dx%d cells (%.0f cm), %.1f MB in %.2fs"),
		*Owner->GetActorLabel(), NT, Ix->NX, Ix->NY, Ix->CX,
		(Ix->V.Num() * 12 + Ix->Tri.Num() * 4 + Ix->CellTri.Num() * 4 + Ix->CellStart.Num() * 4 + Ix->Stamp.Num() * 4) / 1048576.f,
		FPlatformTime::Seconds() - T0);
}

// Nearest hit on the map's scenery. OutU is how far along the segment it
// landed, 0..1, which is what the physics trace's Hit.Time used to give.
static bool BF6_ContextRay(const FVector& O, const FVector& End, FVector& OutHit, double* OutU = nullptr, FVector* OutNormal = nullptr)
{
	bool bAny = false;
	double BestU = 1.0;
	for (int32 i = GRayIndexes.Num() - 1; i >= 0; i--)
	{
		TSharedPtr<FBF6RayIndex>& Ix = GRayIndexes[i];
		if (!Ix.IsValid() || !Ix->Owner.IsValid()) { GRayIndexes.RemoveAt(i); continue; }
		double U;
		FVector P, N;
		if (Ix->Trace(O, End, U, P, &N) && U < BestU)
		{ BestU = U; OutHit = P; if (OutNormal) *OutNormal = N; bAny = true; }
	}
	if (bAny && OutU) *OutU = BestU;
	return bAny;
}


// THE GROUND, AND ONLY THE GROUND.
//
// The ordinary context ray takes the nearest hit across every index, which
// includes the map's low-poly ASSET mesh - so a scatter dropped over a street
// lands bushes on car roofs, awnings and window ledges, all of which are
// perfectly good hits. Scattering vegetation wants the terrain and nothing
// else, and the terrain is identifiable: its index is owned by the context
// actor the map spawns as "<Level>_Terrain".
static bool BF6_TerrainRay(const FVector& O, const FVector& End, FVector& OutHit)
{
	bool bAny = false;
	double BestU = 1.0;
	for (int32 i = GRayIndexes.Num() - 1; i >= 0; i--)
	{
		TSharedPtr<FBF6RayIndex>& Ix = GRayIndexes[i];
		if (!Ix.IsValid() || !Ix->Owner.IsValid()) { GRayIndexes.RemoveAt(i); continue; }
		if (!Ix->Owner->GetActorLabel().EndsWith(TEXT("_Terrain"))) continue;
		double U;
		FVector P, N;
		if (Ix->Trace(O, End, U, P, &N) && U < BestU) { BestU = U; OutHit = P; bAny = true; }
	}
	return bAny;
}


// The map's own terrain and low-poly assets sit under a 'Static' parent in the
// SDK's tree, and creators expect to find them in the same place - not least to
// switch one off with the eye and see inside a building. Tagged as context only,
// so it lives and dies with the scenery it holds rather than with the tree.
static AActor* BF6_EnsureStaticParent(UWorld* W)
{
	if (!W) return nullptr;
	for (TActorIterator<AActor> It(W); It; ++It)
		if (It->Tags.Contains(kContextTag) && It->GetActorLabel() == TEXT("Static")) return *It;
	AActor* A = W->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
	if (!A) return nullptr;
	USceneComponent* Root = NewObject<USceneComponent>(A, TEXT("Node"));
	A->SetRootComponent(Root);
	Root->RegisterComponent();
	A->SetActorLabel(TEXT("Static"));
	A->Tags.Add(kContextTag);
	// The default levels hang Static off the map's own node, last among its
	// children (MP_Badlands.tscn: the HQs, then CombatArea, Camera3D, Static).
	A->Tags.Add(FName(TEXT("gord:9000")));
	A->SetFlags(RF_Transient);
	return A;
}

static AActor* SpawnContextMesh(const FString& FilePath, const FString& Label)
{
	if (!GEditor) return nullptr;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return nullptr;
	if (!FPaths::FileExists(FilePath)) { UE_LOG(LogBF6, Warning, TEXT("context mesh not found: %s"), *FilePath); return nullptr; }
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
	if (!Actor) return nullptr;
	Actor->SetActorLabel(Label);
	Actor->Tags.Add(kContextTag);
	Actor->SetFlags(RF_Transient);          // never saved into the level
	UProceduralMeshComponent* Mesh = MakeProcMesh(Actor, TEXT("ContextMesh"));
	Mesh->SetReceivesDecals(true);   // the map-image decal lands on the context
	// Collision ON: the space-bar placement ray traces this surface so objects
	// land where the crosshair points (not on a flat z=0 plane under the map).
	const double CtxD0 = GMeshDecodeSec, CtxB0 = GMeshBuildSec;
	// Collision is ~97% of the cost of a map's scenery: on Battery the assets
	// mesh takes 0.12s to build and 3.87s to cook, and the big maps are twice
	// that again. So the surface goes up WITHOUT collision - the map is on
	// screen and flyable in a moment - and the cook is queued to happen just
	// after, while the creator is already looking around. Placement needs it,
	// so it is deferred, never skipped.
	if (!FillProcFromBf6Mesh(Mesh, FilePath, false)) { World->EditorDestroyActor(Actor, false); return nullptr; }
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BF6_BuildRayIndex(Actor, Mesh);
	const double CtxDecode = GMeshDecodeSec - CtxD0, CtxBuild = GMeshBuildSec - CtxB0;
	// The map context is scenery: never selectable, never movable.
	Mesh->bSelectable = false;
	// SDK proxy look: flat unlit green terrain / orange assets.
	const bool bAssets = Label.Contains(TEXT("_Assets"));
	if (UMaterialInterface* Mat = BF6_Material(bAssets ? TEXT("M_LevelAssets") : TEXT("M_LevelTerrain")))
		for (int32 s = 0; s < Mesh->GetNumSections(); s++) Mesh->SetMaterial(s, Mat);
	Mesh->SetVisibility(true, true);
	BF6_GroupContextUnderStatic(Label.Left(Label.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd)));
	UE_LOG(LogBF6, Display, TEXT("Loaded context %s: %d section(s) - %.2fs reading, %.2fs building (collision cook included)."),
		*Label, Mesh->GetNumSections(), CtxDecode, CtxBuild);
	return Actor;
}

// Path to a placeable's SDK low-poly model (e.g. "AAGun_01" -> .../objmodels/AAGun_01.bf6mesh).
static FString ObjModelPath(const FString& MeshName)
{
	return FPaths::Combine(BF6_DataDir(), TEXT("objmodels"), MeshName + TEXT(".bf6mesh"));
}

// Place a placeable using the SDK's shipped low-poly model (complete, fast, matches
// the Godot object library). MeshName is the placeable's 'mesh' constant.
static AActor* SpawnSdkModel(const FString& MeshName, const FString& Label, const FTransform& Xform)
{
	if (!GEditor || MeshName.IsEmpty()) return nullptr;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return nullptr;
	const FString Path = ObjModelPath(MeshName);
	const double S0 = FPlatformTime::Seconds();
	const bool bHave = FPaths::FileExists(Path);
	GStatSec += FPlatformTime::Seconds() - S0;
	if (!bHave) { UE_LOG(LogBF6, Warning, TEXT("no SDK model bundled for '%s'"), *MeshName); return nullptr; }
	const double P0 = FPlatformTime::Seconds();
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), Xform);
	GSpawnSec += FPlatformTime::Seconds() - P0;
	if (!Actor) return nullptr;
	BF6_SetPrettyLabel(Actor, Label.IsEmpty() ? MeshName : Label);
	Actor->Tags.Add(kPlacedTag);
	Actor->Tags.Add(FName(*(FString(TEXT("mesh:")) + MeshName)));
	if (!Label.IsEmpty()) Actor->Tags.Add(FName(*(FString(TEXT("label:")) + Label)));
	const double F0 = FPlatformTime::Seconds();
	BF6_FileActor(Actor);   // self-sorting outliner: role/category folder
	GFolderSec += FPlatformTime::Seconds() - F0;
	UProceduralMeshComponent* M = MakeProcMesh(Actor, TEXT("ProcMesh"));
	if (!FillProcFromBf6Mesh(M, Path)) { World->EditorDestroyActor(Actor, false); return nullptr; }
	ApplyObjectWhite(M);               // pure white, like Godot's object library
	Actor->SetActorTransform(Xform);   // setting the root reset it to origin
	M->SetVisibility(true, true);
	return Actor;
}

static void SpawnSdkModelAtCursor(const FString& MeshName, const FString& Label)
{
	FVector Loc(0, 0, 100);
	if (GCurrentLevelEditingViewportClient)
	{
		const FViewportCursorLocation Cursor = GCurrentLevelEditingViewportClient->GetCursorWorldLocationFromMousePos();
		const FVector O = Cursor.GetOrigin(); const FVector D = Cursor.GetDirection();
		if (!D.IsNearlyZero())
		{
			if (FMath::Abs(D.Z) > 1e-4f) { const float t = -O.Z / D.Z; Loc = (t > 0.f && t < 200000.f) ? (O + D * t) : (O + D * 1000.f); }
			else Loc = O + D * 1000.f;
		}
	}
	AActor* A = SpawnSdkModel(MeshName, Label, FTransform(Loc));
	if (A && GEditor) { GEditor->SelectNone(false, true, false); GEditor->SelectActor(A, true, true); }
}

// Bridge for the preview viewport: load an SDK model into a component + report radius.
bool BF6_LoadSdkModelInto(UProceduralMeshComponent* Mesh, const FString& MeshName, float& OutRadius)
{
	if (!FillProcFromBf6Mesh(Mesh, ObjModelPath(MeshName))) return false;
	ApplyObjectWhite(Mesh);   // previews match the placed look: pure white
	const FBoxSphereBounds B = Mesh->CalcBounds(FTransform::Identity);
	OutRadius = FMath::Max(50.f, (float)B.SphereRadius);
	return true;
}

// ============================================================================
// Thumbnail service: renders the same corner-down isometric the hover preview
// uses into 256px PNGs (Saved/BF6UnrealSDK/thumbs), a few per tick, on demand.
// The library's cards poll GetModelThumb; a null return means "queued".
// ============================================================================
namespace
{
	struct FBF6Thumbs
	{
		TUniquePtr<FPreviewScene>     Scene;
		USceneCaptureComponent2D*     Capture = nullptr;
		UTextureRenderTarget2D*       RT = nullptr;
		TMap<FString, TSharedPtr<FSlateBrush>> Brushes;   // mesh -> live brush
		TArray<UTexture2D*>           Held;                // keep brush textures alive
		TArray<FString>               Queue;
		TSet<FString>                 Known;               // queued or done
		FTSTicker::FDelegateHandle    Tick;
	};
	FBF6Thumbs g_thumbs;

	FString BF6_ThumbDir() { return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("thumbs")); }
	// queue keys: plain mesh names, or "block::<name>" for composite block renders
	FString BF6_ThumbPathForKey(const FString& Key)
	{
		return Key.StartsWith(TEXT("block::"))
			? BF6_ThumbDir() / (TEXT("block_") + Key.Mid(7) + TEXT(".png"))
			: BF6_ThumbDir() / (Key + TEXT(".png"));
	}
	FString BF6_ThumbPath(const FString& Mesh) { return BF6_ThumbPathForKey(Mesh); }
	bool BF6_RenderBlockThumb(const FString& Name);   // defined after the Blocks code

	bool BF6_ThumbRigReady()
	{
		if (g_thumbs.Scene.IsValid()) return true;
		g_thumbs.Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues());
		g_thumbs.RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		g_thumbs.RT->AddToRoot();
		g_thumbs.RT->RenderTargetFormat = RTF_RGBA8;
		g_thumbs.RT->InitAutoFormat(256, 256);
		g_thumbs.Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
		g_thumbs.Capture->CaptureSource = SCS_FinalColorLDR;
		g_thumbs.Capture->TextureTarget = g_thumbs.RT;
		g_thumbs.Capture->bCaptureEveryFrame = false;
		g_thumbs.Capture->bCaptureOnMovement = false;
		g_thumbs.Scene->AddComponent(g_thumbs.Capture, FTransform::Identity);
		return true;
	}

	// point the rig at Bounds with the hover preview's corner-down iso framing,
	// capture, and write the PNG. The caller owns adding/removing components.
	bool BF6_CaptureRigTo(const FString& PngPath, const FBoxSphereBounds& B)
	{
		const FRotator Rot(-35.264f, -135.f, 0.f);
		const float Dist = FMath::Max(60.f, (float)B.SphereRadius * 2.4f);
		g_thumbs.Capture->SetWorldLocationAndRotation(B.Origin - Rot.Vector() * Dist, Rot);
		g_thumbs.Capture->FOVAngle = 50.f;
		g_thumbs.Capture->CaptureScene();
		TArray<FColor> Pixels;
		FTextureRenderTargetResource* Res = g_thumbs.RT->GameThread_GetRenderTargetResource();
		if (!Res || !Res->ReadPixels(Pixels)) return false;
		for (FColor& C : Pixels) C.A = 255;
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(256, 256, Pixels, Png);
		IFileManager::Get().MakeDirectory(*BF6_ThumbDir(), true);
		return FFileHelper::SaveArrayToFile(Png, *PngPath);
	}

	// render one model to its PNG; true when a file was produced
	bool BF6_RenderThumb(const FString& Mesh)
	{
		if (!BF6_ThumbRigReady()) return false;
		UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(GetTransientPackage());
		float Radius = 100.f;
		if (!BF6_LoadSdkModelInto(M, Mesh, Radius)) return false;
		g_thumbs.Scene->AddComponent(M, FTransform::Identity);
		const bool bOk = BF6_CaptureRigTo(BF6_ThumbPath(Mesh), M->CalcBounds(FTransform::Identity));
		g_thumbs.Scene->RemoveComponent(M);
		return bOk;
	}

	// PNG on disk -> live texture + brush
	TSharedPtr<FSlateBrush> BF6_BrushFromPng(const FString& Mesh)
	{
		UTexture2D* Tex = FImageUtils::ImportFileAsTexture2D(BF6_ThumbPath(Mesh));
		if (!Tex) return nullptr;
		Tex->AddToRoot();
		g_thumbs.Held.Add(Tex);
		TSharedPtr<FSlateBrush> Brush = MakeShared<FSlateBrush>();
		Brush->SetResourceObject(Tex);
		Brush->ImageSize = FVector2D(256, 256);
		return Brush;
	}

	void BF6_PumpThumbs()
	{
		int32 Done = 0;
		while (g_thumbs.Queue.Num() && Done < 2)   // a couple per tick keeps the editor smooth
		{
			const FString Key = g_thumbs.Queue[0];
			g_thumbs.Queue.RemoveAt(0);
			Done++;
			const bool bOk = Key.StartsWith(TEXT("block::")) ? BF6_RenderBlockThumb(Key.Mid(7)) : BF6_RenderThumb(Key);
			if (bOk)
				if (TSharedPtr<FSlateBrush> Brush = BF6_BrushFromPng(Key))
					g_thumbs.Brushes.Add(Key, Brush);
		}
		if (g_thumbs.Queue.Num() == 0 && g_thumbs.Tick.IsValid())
		{ FTSTicker::GetCoreTicker().RemoveTicker(g_thumbs.Tick); g_thumbs.Tick.Reset(); }
	}
}

namespace BF6Api
{
	const FSlateBrush* GetModelThumb(const FString& Mesh)
	{
		// the Node row has no model on purpose; its card is the Godot icon
		if (Mesh.IsEmpty()) return NodeSymbolBrush(TEXT("Node3D"));
		if (Mesh.IsEmpty()) return nullptr;
		if (const TSharedPtr<FSlateBrush>* B = g_thumbs.Brushes.Find(Mesh)) return B->Get();
		if (g_thumbs.Known.Contains(Mesh)) return nullptr;   // queued or failed
		g_thumbs.Known.Add(Mesh);
		// already rendered in an earlier session?
		if (FPaths::FileExists(BF6_ThumbPath(Mesh)))
		{
			if (TSharedPtr<FSlateBrush> Brush = BF6_BrushFromPng(Mesh))
			{ g_thumbs.Brushes.Add(Mesh, Brush); return Brush.Get(); }
			return nullptr;
		}
		g_thumbs.Queue.Add(Mesh);
		if (!g_thumbs.Tick.IsValid())
			g_thumbs.Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[](float){ BF6_PumpThumbs(); return true; }));
		return nullptr;
	}
}

static void ClearActorsWithTag(FName Tag)
{
	if (!GEditor) return;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return;
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It) if (It->Tags.Contains(Tag)) Doomed.Add(*It);
	for (AActor* A : Doomed) World->EditorDestroyActor(A, false);
}

// ---- base-setup gizmos: markers for point objects, walls for volumes ----
static UProceduralMeshComponent* MakeProcMesh(AActor* A, const FName Name)
{
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(A, Name);
	// The gizmo records moves on the ROOT COMPONENT; NewObject creates with no
	// flags, so without this Modify() silently fails and Ctrl+Z can't undo moves.
	// RF_Transient on BOTH actor and component: our content lives in session
	// json, never in the .umap - letting the editor serialize thousands of
	// proc-mesh actors (one-file-per-actor packages included) crashed the
	// save-on-close prompt. Transient objects still ride the undo buffer.
	M->SetFlags(RF_Transactional | RF_Transient);
	// The map-image decal drapes the low-poly CONTEXT only. Placed objects opt
	// out here so a road stripe never paints itself across the roof of a
	// building the creator just placed; SpawnContextMesh turns receipt back on.
	M->SetReceivesDecals(false);
	A->SetFlags(RF_Transient);
	A->SetRootComponent(M);
	M->RegisterComponent();
	A->AddInstanceComponent(M);
	return M;
}

// Objects render pure white like Godot's object library (not proc-mesh grey).
// The tool's materials ship INSIDE the plugin now. They used to live in the
// project's own Content, which meant the in-editor updater - which sends the
// plugin and nothing else - never delivered them. Anyone who updated in place
// was left without M_Recolor, M_CollisionVis and M_NeonHighlight, so Colorize,
// the collision overlay and the assign-mode highlight all quietly did nothing
// while working perfectly for anyone who had installed a full project zip.
// The plugin copy is tried first; the old project path stays as a fallback so
// existing installs keep working either way.
// THE DEFAULT A VOLUME WEARS when nothing says otherwise.
//
// This is Godot's own collision-shape debug colour, which is what the SDK
// hands a freshly created volume, so a volume made here and one made there
// start life looking the same. It is also markedly more transparent than the
// flat material this replaces - a creator working inside a combat area could
// not see the map through its walls, and the first thing everyone did was go
// looking for an opacity slider.
// Godot's collision-shape colour, alpha included. The stored alpha means what
// it means in Godot; the DISPLAY compensates for our double-sided walls (see
// BF6_ApplyVolumeMaterial), so the same number produces the same look there
// and here - an imported volume's custom colour needs no translation at all.
static const FLinearColor kVolumeDefaultColor(0.0f, 0.6f, 0.7f, 0.42f);

static UMaterialInterface* BF6_Material(const TCHAR* Name)
{
	const FString Plug = FString::Printf(TEXT("/BF6UnrealSDK/Materials/%s.%s"), Name, Name);
	if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *Plug)) return M;
	const FString Game = FString::Printf(TEXT("/Game/Materials/%s.%s"), Name, Name);
	return LoadObject<UMaterialInterface>(nullptr, *Game);
}

// ---------------------------------------------------------------------------
// PER-VOLUME COLOUR.
//
// A polygon volume in the Godot SDK carries an authored `color` - the type
// declares it "(Editor only) Color to differentiate volumes from each other" -
// and creators use it exactly that way: combat area one colour, capture zones
// another, out-of-bounds a third. Every volume here drew in the same flat
// shared material instead, so an imported map arrived as a wall of identical
// boxes and the creator's organisation was gone.
//
// THE SHIPPED M_Volume HAS NO PARAMETERS. It is built from constants, so there
// is nothing on it to drive per volume. Rather than depend on an asset edit,
// the coloured material is built here at runtime and the shipped one stays the
// fallback - the same approach the High Poly add-on takes, and it means an
// older or missing asset degrades to the previous look rather than to nothing.
static UMaterial* GVolumeColorParent = nullptr;

static UMaterial* BF6_VolumeColorParent()
{
	if (GVolumeColorParent) return GVolumeColorParent;

	// A real package, never saved: a material built in the transient package
	// half-registers and then fails to compile, which presents as every volume
	// turning into the engine's default checker.
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BF6VolumeColor"));
	if (!Pkg) return nullptr;
	Pkg->SetFlags(RF_Transient);
	UMaterial* M = NewObject<UMaterial>(Pkg, TEXT("M_BF6VolumeColor"), RF_Transient);
	if (!M) return nullptr;

	M->MaterialDomain = MD_Surface;
	M->BlendMode = BLEND_Translucent;
	// UNLIT, on purpose. A zone is a diagram, not a surface: lit, the far wall
	// of a volume goes dark and reads as a different colour from the near one,
	// which defeats the whole point of colouring them to tell them apart.
	M->SetShadingModel(MSM_Unlit);
	M->TwoSided = true;

	UMaterialExpressionVectorParameter* Col =
		Cast<UMaterialExpressionVectorParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				M, UMaterialExpressionVectorParameter::StaticClass(), -400, 0));
	if (Col)
	{
		Col->ParameterName = TEXT("Color");
		Col->DefaultValue = kVolumeDefaultColor;
		UMaterialEditingLibrary::ConnectMaterialProperty(Col, TEXT(""), MP_EmissiveColor);
		// The ALPHA of the same parameter drives opacity, so one colour value
		// carries both what the creator picked and how see-through they made
		// it - which is how the Godot picker presents it too.
		UMaterialEditingLibrary::ConnectMaterialProperty(Col, TEXT("A"), MP_Opacity);
	}

	M->PreEditChange(nullptr);
	M->PostEditChange();
	GVolumeColorParent = M;
	return M;
}

// The colour a volume actor is wearing, from its own tag. Absent means the
// creator never set one, which is not the same as black.
static bool BF6_VolumeColorOf(AActor* A, FLinearColor& Out)
{
	if (!A) return false;
	for (const FName& T : A->Tags)
	{
		const FString S = T.ToString();
		if (!S.StartsWith(TEXT("vcol:"))) continue;
		// Stored as RRGGBBAA hex: it survives the tag round-trip the save
		// already does, reads the same way a creator types a hex colour, and
		// does not drift through a float printer.
		const FString Hex = S.Mid(5);
		if (Hex.Len() < 8) return false;
		Out = FLinearColor(FColor::FromHex(Hex.Left(8)));
		return true;
	}
	return false;
}

static void BF6_SetVolumeColorTag(AActor* A, const FLinearColor& C)
{
	if (!A) return;
	for (int32 i = A->Tags.Num() - 1; i >= 0; i--)
		if (A->Tags[i].ToString().StartsWith(TEXT("vcol:"))) A->Tags.RemoveAt(i);
	A->Tags.Add(FName(*FString::Printf(TEXT("vcol:%s"), *C.ToFColor(true).ToHex())));
}

// Give one volume's walls their colour. Called wherever walls are built or
// rebuilt, so a reshaped zone keeps the colour it was wearing.
static void BF6_ApplyVolumeMaterial(AActor* Owner, UProceduralMeshComponent* VM);

// Every volume in the world, re-coloured from its own tag.
static void BF6_ReapplyVolumeColors()
{
	if (!GEditor) return;
	UWorld* W = GEditor->GetEditorWorldContext().World();
	if (!W) return;
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		// A volume is exactly an actor carrying a colour tag: the walls may be
		// on the root or on a named component depending on which path built it.
		FLinearColor Ignored;
		if (!BF6_VolumeColorOf(*It, Ignored)) continue;
		TArray<UProceduralMeshComponent*> Meshes;
		It->GetComponents<UProceduralMeshComponent>(Meshes);
		for (UProceduralMeshComponent* M : Meshes)
			if (M && (M->GetFName() == FName(TEXT("Volume")) || M == It->GetRootComponent()))
				BF6_ApplyVolumeMaterial(*It, M);
	}
}

static void BF6_ApplyVolumeMaterial(AActor* Owner, UProceduralMeshComponent* VM)
{
	if (!VM) return;
	FLinearColor C = kVolumeDefaultColor;
	const bool bHas = BF6_VolumeColorOf(Owner, C);

	if (UMaterial* Parent = BF6_VolumeColorParent())
	{
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Parent, VM);
		if (Mid)
		{
			// GODOT DENSITY FROM A GODOT NUMBER. Their gizmo culls back faces,
			// so a volume always blends exactly ONE layer of its alpha; our
			// walls draw both windings and blend TWO, which made every
			// imported colour look nearly twice as dense as it did in the SDK.
			// Solving 1-(1-x)^2 = a gives the per-face alpha that lands on the
			// same final density - and the STORED value stays Godot's, so the
			// round trip carries the creator's number untouched.
			FLinearColor Disp = C;
			Disp.A = 1.f - FMath::Sqrt(FMath::Max(0.f, 1.f - FMath::Clamp(C.A, 0.f, 1.f)));
			Mid->SetVectorParameterValue(TEXT("Color"), Disp);
			VM->SetMaterial(0, Mid);
			// the white edge lines, Godot's own zone furniture (section 1)
			if (VM->GetNumSections() > 1)
				if (UMaterialInstanceDynamic* EdgeMid = UMaterialInstanceDynamic::Create(Parent, VM))
				{
					EdgeMid->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 1.f, 1.f, 0.8f));
					VM->SetMaterial(1, EdgeMid);
				}
			// Recorded even when it came from the default, so that a volume
			// always round-trips the colour it is actually wearing rather than
			// silently re-defaulting if the default ever changes.
			if (!bHas && Owner) BF6_SetVolumeColorTag(Owner, C);
			return;
		}
	}
	// The shipped flat material, if the runtime one could not be built.
	if (UMaterialInterface* Mat = BF6_Material(TEXT("M_Volume"))) VM->SetMaterial(0, Mat);
}

static void ApplyObjectWhite(UProceduralMeshComponent* Mesh)
{
	if (!Mesh) return;
	if (UMaterialInterface* Mat = BF6_Material(TEXT("M_ObjectWhite")))
		for (int32 s = 0; s < Mesh->GetNumSections(); s++) Mesh->SetMaterial(s, Mat);
}

// Load a .bf6mesh file into a proc-mesh component (Godot->Unreal axis swap +
// winding fix). Verts are used as-is: world-space for map context, local for the
// small object/gameplay models. Returns false if missing/unreadable.
// bCollision cooks collision (async) - used by the map context so the space-bar
// placement ray can find the actual surface under the cursor.
// Decoded mesh data, kept so a model is read and unzipped ONCE per session.
// A resumed map is mostly repeats - a few hundred distinct models across
// thousands of objects - and every one of those objects used to re-read its
// file from disk, gunzip it and re-parse it. Big one-off meshes (the map's own
// terrain and assets) are deliberately not cached: they are loaded once and
// keeping them would cost hundreds of megabytes for nothing.
struct FBF6Surface
{
	TArray<FVector> V, N;
	TArray<FVector2D> UV;
	TArray<int32> T;
};
static TMap<FString, TArray<FBF6Surface>> GMeshCache;
static int64 GMeshCacheBytes = 0;
static const int64 kMeshCacheMaxBytes = 512ll * 1024 * 1024;   // generous, still bounded
static const int64 kMeshCacheMaxFile  = 4ll * 1024 * 1024;     // skip the map-sized meshes

// load timing, read back by the session load log
double GMeshDecodeSec = 0.0, GMeshBuildSec = 0.0;
int32  GMeshCalls = 0, GMeshCacheHits = 0;

void BF6_ClearMeshCache()
{
	GMeshCache.Empty();
	GMeshCacheBytes = 0;
}

static bool FillProcFromBf6Mesh(UProceduralMeshComponent* Mesh, const FString& FilePath, bool bCollision, float NormalPush)
{
	if (!Mesh) return false;
	if (bCollision) Mesh->bUseAsyncCooking = true;   // don't hitch the load
	GMeshCalls++;

	const double T0 = FPlatformTime::Seconds();
	TArray<FBF6Surface>* Cached = GMeshCache.Find(FilePath);
	TArray<FBF6Surface> Decoded;
	if (Cached) GMeshCacheHits++;
	else
	{
		if (!FPaths::FileExists(FilePath)) return false;
		TArray<uint8> File;
		if (!FFileHelper::LoadFileToArray(File, *FilePath) || File.Num() < 12) return false;

		// 'BF6Z' = gzip-compressed payload; 'BF6S' = raw payload (legacy, uncompressed).
		uint32 fileMagic = 0; FMemory::Memcpy(&fileMagic, File.GetData(), 4);
		const int64 FileBytes = File.Num();
		TArray<uint8> Raw;
		if (fileMagic == 0x5A364642)   // BF6Z
		{
			uint32 rawSize = 0; FMemory::Memcpy(&rawSize, File.GetData() + 4, 4);
			Raw.SetNumUninitialized((int32)rawSize);
			if (!FCompression::UncompressMemory(NAME_Gzip, Raw.GetData(), (int32)rawSize, File.GetData() + 8, File.Num() - 8))
				return false;
		}
		else if (fileMagic == 0x42463653)   // BF6S
		{
			Raw = MoveTemp(File);
		}
		else return false;

		FMemoryReader Ar(Raw);
		uint32 magic = 0, ver = 0, surf = 0;
		Ar << magic; Ar << ver; Ar << surf;
		if (magic != 0x42463653) return false;
		const float M = 100.0f;
		Decoded.SetNum((int32)surf);
		for (uint32 s = 0; s < surf; s++)
		{
			FBF6Surface& Sf = Decoded[(int32)s];
			uint32 vc = 0, ic = 0; Ar << vc; Ar << ic;
			Sf.V.Reserve(vc);
			for (uint32 v = 0; v < vc; v++) { float x, y, z; Ar << x; Ar << y; Ar << z; Sf.V.Add(FVector(x * M, z * M, y * M)); }
			uint8 hasN = 0, hasU = 0; Ar << hasN; Ar << hasU;
			if (hasN) { Sf.N.Reserve(vc); for (uint32 v = 0; v < vc; v++) { float x, y, z; Ar << x; Ar << y; Ar << z; Sf.N.Add(FVector(x, z, y)); } }
			if (hasU) { Sf.UV.Reserve(vc); for (uint32 v = 0; v < vc; v++) { float u, w; Ar << u; Ar << w; Sf.UV.Add(FVector2D(u, w)); } }
			Sf.T.Reserve(ic);
			for (uint32 i = 0; i < ic; i++) { int32 idx = 0; Ar << idx; Sf.T.Add(idx); }
			for (int32 t = 0; t + 2 < Sf.T.Num(); t += 3) { const int32 tmp = Sf.T[t + 1]; Sf.T[t + 1] = Sf.T[t + 2]; Sf.T[t + 2] = tmp; }
		}

		if (FileBytes <= kMeshCacheMaxFile && GMeshCacheBytes < kMeshCacheMaxBytes)
		{
			int64 Bytes = 0;
			for (const FBF6Surface& Sf : Decoded)
				Bytes += Sf.V.Num() * sizeof(FVector) + Sf.N.Num() * sizeof(FVector)
					+ Sf.UV.Num() * sizeof(FVector2D) + Sf.T.Num() * sizeof(int32);
			GMeshCacheBytes += Bytes;
			Cached = &GMeshCache.Add(FilePath, MoveTemp(Decoded));
		}
	}
	const double T1 = FPlatformTime::Seconds();

	const TArray<FBF6Surface>& Src = Cached ? *Cached : Decoded;
	const TArray<FLinearColor> NC; const TArray<FProcMeshTangent> NT;
	if (NormalPush > 0.f)
	{
		// PUSHED OUT ALONG THE NORMAL, not scaled up.
		//
		// A shell that wraps another mesh z-fights with it, and the usual
		// answer here was a 1.002 uniform scale. That is PROPORTIONAL: on a
		// building it is centimetres and works, on a jerrycan it is under a
		// millimetre and does not, so the overlay flickered on exactly the
		// small props people were trying to inspect. A fixed distance along
		// each vertex's own normal is scale-independent and also correct on
		// vertical faces, which a lift in Z is not.
		//
		// The cache is SHARED and must not be touched: the push goes into a
		// scratch copy, or every later draw of this asset inherits it and the
		// prop itself grows.
		TArray<FVector> Pushed;
		for (int32 s = 0; s < Src.Num(); s++)
		{
			const FBF6Surface& Sf = Src[s];
			Pushed = Sf.V;
			if (Sf.N.Num() == Sf.V.Num())
				for (int32 v = 0; v < Pushed.Num(); v++) Pushed[v] += Sf.N[v] * NormalPush;
			Mesh->CreateMeshSection_LinearColor(s, Pushed, Sf.T, Sf.N, Sf.UV, NC, NT, bCollision);
		}
	}
	else
	for (int32 s = 0; s < Src.Num(); s++)
		Mesh->CreateMeshSection_LinearColor(s, Src[s].V, Src[s].T, Src[s].N, Src[s].UV, NC, NT, bCollision);

	const double T2 = FPlatformTime::Seconds();
	GMeshDecodeSec += T1 - T0;
	GMeshBuildSec  += T2 - T1;
	return Src.Num() > 0;
}

// A vertical pillar marker (local space, centered on the actor).
static void BuildMarker(UProceduralMeshComponent* M)
{
	const float r = 55.f, h = 350.f;   // 0.55m wide, 3.5m tall
	TArray<FVector> V = {
		{-r,-r,0},{r,-r,0},{r,r,0},{-r,r,0}, {-r,-r,h},{r,-r,h},{r,r,h},{-r,r,h} };
	const int32 F[6][4] = {{0,3,2,1},{4,5,6,7},{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7}};
	TArray<int32> T;
	for (auto& f : F) { T.Add(f[0]);T.Add(f[1]);T.Add(f[2]); T.Add(f[0]);T.Add(f[2]);T.Add(f[3]); }
	const TArray<FVector> N; const TArray<FVector2D> UV; const TArray<FLinearColor> NC; const TArray<FProcMeshTangent> NT;
	M->CreateMeshSection_LinearColor(0, V, T, N, UV, NC, NT, false);
}

// Vertical walls (double-sided) along a closed ground loop in world space.
static void BuildWalls(UProceduralMeshComponent* M, const TArray<FVector>& Loop, float Height)
{
	if (Loop.Num() < 2) return;
	TArray<FVector> V; TArray<int32> T;
	for (int32 i = 0; i < Loop.Num(); i++)
	{
		const FVector a = Loop[i], b = Loop[(i + 1) % Loop.Num()];
		const int32 base = V.Num();
		V.Add(a); V.Add(b); V.Add(b + FVector(0,0,Height)); V.Add(a + FVector(0,0,Height));
		T.Add(base);T.Add(base+1);T.Add(base+2); T.Add(base);T.Add(base+2);T.Add(base+3);
		T.Add(base);T.Add(base+2);T.Add(base+1); T.Add(base);T.Add(base+3);T.Add(base+2);  // back
	}
	const TArray<FVector> N; const TArray<FVector2D> UV; const TArray<FLinearColor> NC; const TArray<FProcMeshTangent> NT;
	M->CreateMeshSection_LinearColor(0, V, T, N, UV, NC, NT, false);
	// THE EDGES, white, the way Godot draws a zone: the bottom edge, the
	// vertical crease at each point, and the top edge (their gizmo's exact
	// three, PolygonVolumeGizmo.gd). The fill alone reads as fog; the lines
	// are what make the SHAPE legible, and creators from the SDK look for
	// them. Thin ribbons in the wall plane, nudged off it on both sides so
	// they never z-fight the fill they sit on.
	{
		TArray<FVector> EV; TArray<int32> ET;
		const float W = 5.f;   // ribbon width in cm
		auto Rib = [&EV, &ET](const FVector& p0, const FVector& p1, const FVector& Across, const FVector& Push)
		{
			const int32 b2 = EV.Num();
			EV.Add(p0 + Push); EV.Add(p1 + Push); EV.Add(p1 + Across + Push); EV.Add(p0 + Across + Push);
			ET.Append({ b2, b2 + 1, b2 + 2,  b2, b2 + 2, b2 + 3 });
			ET.Append({ b2, b2 + 2, b2 + 1,  b2, b2 + 3, b2 + 2 });
		};
		for (int32 k = 0; k < Loop.Num(); k++)
		{
			const FVector a = Loop[k], b = Loop[(k + 1) % Loop.Num()];
			const FVector Dir = (b - a).GetSafeNormal2D();
			if (Dir.IsNearlyZero()) continue;
			const FVector Nrm = FVector::CrossProduct(Dir, FVector::UpVector);
			const FVector Up(0, 0, Height);
			for (const float Side : { 1.f, -1.f })
			{
				const FVector Push = Nrm * Side * 1.0f;
				Rib(a, b, FVector(0, 0, W), Push);                                                  // bottom edge
				Rib(a + Up - FVector(0, 0, W), b + Up - FVector(0, 0, W), FVector(0, 0, W), Push);  // top edge
				Rib(a, a + Up, Dir * W, Push);                                                      // vertical crease
			}
		}
		const TArray<FVector> EN; const TArray<FVector2D> EUV; const TArray<FLinearColor> ENC; const TArray<FProcMeshTangent> ENT;
		M->CreateMeshSection_LinearColor(1, EV, ET, EN, EUV, ENC, ENT, false);
	}
}

// ============================================================================
// Zone (polygon volume) point editing + per-actor property store.
// Volumes keep their editable world-space ground loop in a registry; EDIT
// POINTS spawns a small drag handle per vertex that the user moves with the
// normal gizmo while the walls rebuild live (Godot-style).
// ============================================================================
static const FName kHandleTag("BF6Handle");
static const FName kObbTag("BF6Obb");

static TMap<TWeakObjectPtr<AActor>, TArray<FVector>> GVolumeLoops;

// Zone point editing, Godot-style: the points are SCREEN-SPACE orange dots
// (painted by the build overlay, constant pixel size) dragged directly with
// the mouse - no handle actors, no transform gizmo. The loop lives in
// GVolumeLoops (actor space); the actor's "loopN:" tags mirror it inside each
// transaction so undo/redo restores the shape.
struct FBF6VolEdit
{
	TWeakObjectPtr<AActor> Volume;
	int32   Active = 0;              // last-clicked point (the pie's ADD/DELETE anchor)
	int32   Drag = INDEX_NONE;       // point being dragged, else -1
	double  DragZ = 0.0;             // world height the drag slides on
	bool    bTx = false;             // a GEditor transaction is open for the drag
	FVector EdgeWorld = FVector::ZeroVector;   // Ctrl add-point preview
	bool    bEdgeValid = false;
	// screen-space cache, refreshed once per TICK: projecting inside Slate's
	// paint pass proved unreliable, so the paint layer only reads these.
	// Two handles per point like Godot: [0..N) = bottom ring, [N..2N) = top.
	TArray<FVector2D> CachedPx;
	int32 CachedN = 0;               // points per ring
	FVector2D CachedEdgePx = FVector2D::ZeroVector;
	bool bCachedEdge = false;
	int32  EdgeSeg = INDEX_NONE;     // segment the edge preview sits on
	double DragBottomZ = 0.0;        // the dragged point's own height (kept fixed)
};
static FBF6VolEdit GVolEdit;

// ---- shared "focus view" ghosting ----
// Used by assign mode AND group/block focus editing: everything outside the
// working set turns translucent (M_Ghost) and unselectable, restored on exit.
struct FBF6Ghosted { TWeakObjectPtr<UProceduralMeshComponent> Comp; TArray<UMaterialInterface*> Mats; bool bWasSelectable = true; };

// A level viewport only repaints when something invalidates it, so anything we
// change off a button press (materials, colours, ghosting) stays invisible
// until the user happens to fly the camera. Every such action ends with this.
static void BF6_Redraw()
{
	if (GEditor) GEditor->RedrawLevelEditingViewports(true);
}

static void BF6_GhostRestoreSet(TArray<FBF6Ghosted>& Set)
{
	for (FBF6Ghosted& G : Set)
		if (UProceduralMeshComponent* M = G.Comp.Get())
		{
			for (int32 i = 0; i < G.Mats.Num(); i++) M->SetMaterial(i, G.Mats[i]);
			M->bSelectable = G.bWasSelectable;
		}
	Set.Reset();
	BF6_Redraw();
}

struct FBF6LinkPick
{
	TWeakObjectPtr<AActor> Owner;
	FString Prop;
	bool bArray = false;
	bool bActive = false;
	// assign-mode view: everything non-assignable is ghosted (translucent +
	// unselectable); candidates get screen markers, colour-coded, with lines
	// drawn from the owner to assigned/pending targets
	TArray<TWeakObjectPtr<AActor>> Candidates;
	TArray<FVector2D> CandPx;        // cached each tick (viewport pixels)
	TArray<uint8> CandState;         // 0 free, 1 assigned to owner, 2 selected (pending)
	FVector2D OwnerPx = FVector2D::ZeroVector;
	bool bOwnerPx = false;
	TArray<FBF6Ghosted> Ghosted;
	// candidates glow solid neon so they never blend into the map; their real
	// materials are stored here and restored on exit. One MID per state so the
	// mesh colour matches the marker and the line to the owner.
	TArray<FBF6Ghosted> Neon;
	TArray<uint8> NeonApplied;       // last state a candidate was painted with
	TStrongObjectPtr<UMaterialInstanceDynamic> Mid[3];
};
static FBF6LinkPick GLinkPick;

// marker, line, and mesh share these: free cyan / assigned green / pending orange
static const FLinearColor kLinkNeon[3] = {
	FLinearColor(0.f, 0.9f, 1.f),
	FLinearColor(0.22f, 1.f, 0.08f),
	FLinearColor(1.f, 0.63f, 0.f) };

static void BF6_LinkApplyNeon(int32 i)
{
	if (!GLinkPick.Candidates.IsValidIndex(i) || !GLinkPick.CandState.IsValidIndex(i)) return;
	const uint8 St = FMath::Min<uint8>(GLinkPick.CandState[i], 2);
	UMaterialInstanceDynamic* Mid = GLinkPick.Mid[St].Get();
	AActor* C = GLinkPick.Candidates[i].Get();
	if (!C) return;

	// A ZONE STAYS SEE-THROUGH.
	//
	// The neon is a SOLID material, which is right for a prop - it has to stand
	// out against the map and it was never transparent to begin with. On a
	// polygon volume it is wrong: a zone is a translucent shell you look at the
	// map THROUGH, and painting it solid turns every candidate into an opaque
	// wall. That was tolerable while candidates were distant glows and became
	// the main complaint the moment the picker could fly you inside one.
	//
	// So a volume takes the state COLOUR through its own material instead of
	// the neon: same three colours, same meaning, and you can still see what
	// the zone is sitting on.
	if (BF6Api::IsVolumeActor(C))
	{
		if (UMaterial* Parent = BF6_VolumeColorParent())
		{
			TArray<UProceduralMeshComponent*> Meshes;
			C->GetComponents<UProceduralMeshComponent>(Meshes);
			for (UProceduralMeshComponent* M : Meshes)
			{
				if (!M) continue;
				if (UMaterialInstanceDynamic* VM = UMaterialInstanceDynamic::Create(Parent, M))
				{
					FLinearColor Col = kLinkNeon[St];
					Col.A = 0.45f;   // readable as a state, still a window
					VM->SetVectorParameterValue(TEXT("Color"), Col);
					for (int32 s = 0; s < M->GetNumSections(); s++) M->SetMaterial(s, VM);
				}
			}
		}
		if (GLinkPick.NeonApplied.IsValidIndex(i)) GLinkPick.NeonApplied[i] = St;
		return;
	}

	UMaterialInstanceDynamic* Mid2 = GLinkPick.Mid[St].Get();
	if (!Mid2) return;
	if (UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(C->GetRootComponent()))
		for (int32 s = 0; s < M->GetNumSections(); s++) M->SetMaterial(s, Mid2);
	if (GLinkPick.NeonApplied.IsValidIndex(i)) GLinkPick.NeonApplied[i] = St;
}

static void BF6_LinkGhostRestore()
{
	BF6_GhostRestoreSet(GLinkPick.Ghosted);
	BF6_GhostRestoreSet(GLinkPick.Neon);
	// Volumes were never in the stored set - they kept their own material and
	// only had its colour changed - so they are put back from the tag, which is
	// where a zone's colour actually lives.
	for (TWeakObjectPtr<AActor>& Wk : GLinkPick.Candidates)
		if (AActor* C = Wk.Get())
			if (BF6Api::IsVolumeActor(C))
			{
				TArray<UProceduralMeshComponent*> Meshes;
				C->GetComponents<UProceduralMeshComponent>(Meshes);
				for (UProceduralMeshComponent* M : Meshes)
					if (M && (M->GetFName() == FName(TEXT("Volume")) || M == C->GetRootComponent()))
						BF6_ApplyVolumeMaterial(C, M);
			}
	GLinkPick.NeonApplied.Reset();
	for (int32 s = 0; s < 3; s++) GLinkPick.Mid[s].Reset();
	GLinkPick.Candidates.Reset();
	GLinkPick.CandPx.Reset();
	GLinkPick.CandState.Reset();
	GLinkPick.bOwnerPx = false;
}

// Handles draw in the foreground pass (visible through walls/floors) in the
// accent orange, so a vertex buried inside geometry stays visible and grabbable.
static void ApplyHandleStyle(UProceduralMeshComponent* M)
{
	if (!M) return;
	M->SetDepthPriorityGroup(SDPG_Foreground);
	if (UMaterialInterface* Mat = BF6_Material(TEXT("M_LevelAssets")))
		for (int32 s = 0; s < M->GetNumSections(); s++) M->SetMaterial(s, Mat);
}

// A small centered cube the user can grab with the normal move gizmo.
static void BuildHandleCube(UProceduralMeshComponent* M)
{
	const float r = 45.f;
	TArray<FVector> V = {
		{-r,-r,-r},{r,-r,-r},{r,r,-r},{-r,r,-r}, {-r,-r,r},{r,-r,r},{r,r,r},{-r,r,r} };
	const int32 F[6][4] = {{0,3,2,1},{4,5,6,7},{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7}};
	TArray<int32> T;
	for (auto& f : F) { T.Add(f[0]);T.Add(f[1]);T.Add(f[2]); T.Add(f[0]);T.Add(f[2]);T.Add(f[3]); }
	const TArray<FVector> N; const TArray<FVector2D> UV; const TArray<FLinearColor> NC; const TArray<FProcMeshTangent> NT;
	M->CreateMeshSection_LinearColor(0, V, T, N, UV, NC, NT, false);
}

static AActor* SpawnVolumeHandle(UWorld* W, const FVector& Pos, int32 Idx)
{
	AActor* A = W->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
	if (!A) return nullptr;
	UProceduralMeshComponent* M = MakeProcMesh(A, TEXT("Handle"));
	BuildHandleCube(M);
	ApplyHandleStyle(M);
	A->SetActorLocation(Pos);
	A->SetActorLabel(FString::Printf(TEXT("Point_%d"), Idx));
	A->Tags.Add(kHandleTag);
	A->SetFlags(RF_Transient);
	return A;
}

// Zone loops are stored in the volume ACTOR's space (== world at load, since
// volumes spawn at the origin). Consumers that need world coordinates - the
// exporter, the session save, the point-edit handles - transform through the
// actor, so MOVING or ROTATING a whole zone with the gizmo carries its points.
static TArray<FVector> BF6_LoopToWorld(AActor* Vol, const TArray<FVector>& Local)
{
	const FTransform Xf = Vol ? Vol->GetActorTransform() : FTransform::Identity;
	TArray<FVector> Out; Out.Reserve(Local.Num());
	for (const FVector& P : Local) Out.Add(Xf.TransformPosition(P));
	return Out;
}
static TArray<FVector> BF6_LoopToLocal(AActor* Vol, const TArray<FVector>& World)
{
	const FTransform Xf = Vol ? Vol->GetActorTransform() : FTransform::Identity;
	TArray<FVector> Out; Out.Reserve(World.Num());
	for (const FVector& P : World) Out.Add(Xf.InverseTransformPosition(P));
	return Out;
}

static FString TagValue(AActor* A, const FString& Prefix);   // fwd: defined with the session code

// ---- WaypointPath: the third authorable geometry type -----------------------
//
// The SDK's schema has THREE creator-drawn shapes, not two: polygonvolume,
// obbvolume, and waypointpath - a Curve3D the AI walks, linked from
// AI_WaypointPath's `Waypoints` field and exported as world-space points plus
// an isClosed flag (code/gdconverter, test_waypointpath.py pins the contract).
// This tool shipped without it, so imported maps silently lost their patrol
// routes and an AI_WaypointPath placed here was an empty shell.
//
// A path stores its points in GVolumeLoops like a zone does, which buys every
// existing tool - drag dots, Ctrl+LMB add, Ctrl+RMB delete, loop tags, session
// save, undo repair - and only the MESH differs: an open ribbon with direction
// arrows instead of closed walls.

// Does this type CARRY a path? Read from the schema, not a name list.
static bool BF6_TypeHasWaypoints(const FString& Type)
{
	static TMap<FString, bool> Cache;
	if (const bool* C = Cache.Find(Type)) return *C;
	bool bHas = false;
	for (const BF6Api::FPropDef& D : BF6Api::PropsForType(Type))
		if (D.Type == TEXT("WaypointPath")) { bHas = true; break; }
	Cache.Add(Type, bHas);
	return bHas;
}

static bool BF6_IsPathActor(AActor* A)
{
	if (!A) return false;
	FString Ty = TagValue(A, TEXT("label:"));
	if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
	return !Ty.IsEmpty() && BF6_TypeHasWaypoints(Ty);
}

static bool BF6_PathIsClosed(AActor* A)
{
	return BF6Api::GetActorProp(A, TEXT("isClosed")).Equals(TEXT("true"), ESearchCase::IgnoreCase);
}

// An open ribbon: a knee-high wall per segment plus an arrowhead at each
// midpoint, so the DIRECTION reads at a glance - a patrol route runs one way.
static void BuildPathRibbon(UProceduralMeshComponent* M, const TArray<FVector>& Pts, bool bClosed)
{
	if (Pts.Num() < 2) return;
	TArray<FVector> V; TArray<int32> T;
	const float H = 100.f;   // 1 m tall: visible, never a wall
	const int32 Segs = bClosed ? Pts.Num() : Pts.Num() - 1;
	for (int32 i = 0; i < Segs; i++)
	{
		const FVector a = Pts[i], b = Pts[(i + 1) % Pts.Num()];
		const int32 s = V.Num();
		V.Add(a); V.Add(a + FVector(0, 0, H)); V.Add(b); V.Add(b + FVector(0, 0, H));
		// both windings, so it reads from either side
		T.Append({ s, s + 1, s + 2,  s + 2, s + 1, s + 3 });
		T.Append({ s + 2, s + 1, s,  s + 3, s + 1, s + 2 });

		// the arrow: a horizontal chevron at half height, pointing at b
		const FVector Dir = (b - a).GetSafeNormal2D();
		if (!Dir.IsNearlyZero())
		{
			const FVector Mid = (a + b) * 0.5f + FVector(0, 0, H * 0.5f);
			const FVector Side = FVector::CrossProduct(Dir, FVector::UpVector) * 60.f;
			const int32 t = V.Num();
			V.Add(Mid + Dir * 90.f); V.Add(Mid - Dir * 30.f + Side); V.Add(Mid - Dir * 30.f - Side);
			T.Append({ t, t + 1, t + 2,  t + 2, t + 1, t });
		}
	}
	TArray<FVector> N; N.Init(FVector::UpVector, V.Num());
	TArray<FVector2D> UV; UV.Init(FVector2D::ZeroVector, V.Num());
	TArray<FLinearColor> C; C.Init(FLinearColor::White, V.Num());
	TArray<FProcMeshTangent> Tan; Tan.Init(FProcMeshTangent(1, 0, 0), V.Num());
	M->CreateMeshSection_LinearColor(0, V, T, N, UV, C, Tan, false);
	M->SetCastShadow(false);
}

static void RebuildVolumeWalls(AActor* Vol, const TArray<FVector>& Loop)
{
	UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(Vol->GetRootComponent());
	if (!M) return;
	// A PATH rebuilds as a ribbon, not walls - same registry, different mesh.
	// The ribbon gets its OWN component: unlike a zone, a path actor's root is
	// its 3D model, and clearing sections there would eat the model itself.
	if (BF6_IsPathActor(Vol))
	{
		UProceduralMeshComponent* R = nullptr;
		TArray<UProceduralMeshComponent*> Comps;
		Vol->GetComponents<UProceduralMeshComponent>(Comps);
		for (UProceduralMeshComponent* C : Comps)
			if (C->GetFName() == FName(TEXT("PathRibbon"))) { R = C; break; }
		if (!R)
		{
			R = NewObject<UProceduralMeshComponent>(Vol, FName(TEXT("PathRibbon")));
			R->SetFlags(RF_Transactional | RF_Transient);
			R->SetupAttachment(Vol->GetRootComponent());
			R->RegisterComponent();
			Vol->AddInstanceComponent(R);
			// world-space points go in raw, so the component must not inherit
			// the actor's transform on top of them
			R->SetUsingAbsoluteLocation(true);
			R->SetUsingAbsoluteRotation(true);
			R->SetUsingAbsoluteScale(true);
			R->SetWorldTransform(FTransform::Identity);
		}
		R->ClearAllMeshSections();
		BuildPathRibbon(R, Loop, BF6_PathIsClosed(Vol));
		BF6_ApplyVolumeMaterial(Vol, R);
		return;
	}
	// the zone's real height (Godot metres) when it carries one; 0 means
	// INFINITE in the SDK, which Godot's gizmo draws at 5 m - match that
	double H = 5.0;
	const FString HS = BF6Api::GetActorProp(Vol, TEXT("height"));
	if (HS.IsNumeric()) H = FCString::Atod(*HS);
	if (H <= 0.01) H = 5.0;
	M->ClearAllMeshSections();
	BuildWalls(M, Loop, (float)H * 100.f);
	BF6_ApplyVolumeMaterial(Vol, M);
}

// ---- OBBVolume: an oriented box with Godot-style face-handle editing ----
// The box is defined by its actor transform plus a "size" attribute (Godot
// metres, x/y/z with y up - matching the SDK's OBBVolume schema).
static FVector BF6_ObbSizeGodot(AActor* A)
{
	TArray<FString> P;
	BF6Api::GetActorProp(A, TEXT("size")).ParseIntoArray(P, TEXT(","));
	if (P.Num() == 3)
		return FVector(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2]));
	return FVector(10, 10, 10);
}
// Godot (x, y-up, z) metres -> Unreal full size in cm (x, z, y)
static FVector BF6_ObbSizeUE(const FVector& G) { return FVector(G.X, G.Z, G.Y) * 100.f; }

static void BF6_SetObbSizeTag(AActor* A, const FVector& G)
{
	for (int32 i = A->Tags.Num() - 1; i >= 0; i--)
		if (A->Tags[i].ToString().StartsWith(TEXT("p:size="))) A->Tags.RemoveAt(i);
	A->Tags.Add(FName(*FString::Printf(TEXT("p:size=%g,%g,%g"), G.X, G.Y, G.Z)));
}

static void BuildBox(UProceduralMeshComponent* M, const FVector& Full)
{
	const FVector H = Full * 0.5f;
	TArray<FVector> V = {
		{-H.X,-H.Y,-H.Z},{H.X,-H.Y,-H.Z},{H.X,H.Y,-H.Z},{-H.X,H.Y,-H.Z},
		{-H.X,-H.Y, H.Z},{H.X,-H.Y, H.Z},{H.X,H.Y, H.Z},{-H.X,H.Y, H.Z} };
	const int32 F[6][4] = {{0,3,2,1},{4,5,6,7},{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7}};
	TArray<int32> T;
	for (auto& f : F)
	{
		T.Add(f[0]);T.Add(f[1]);T.Add(f[2]); T.Add(f[0]);T.Add(f[2]);T.Add(f[3]);
		T.Add(f[0]);T.Add(f[2]);T.Add(f[1]); T.Add(f[0]);T.Add(f[3]);T.Add(f[2]);   // back faces
	}
	const TArray<FVector> N; const TArray<FVector2D> UV; const TArray<FLinearColor> NC; const TArray<FProcMeshTangent> NT;
	M->CreateMeshSection_LinearColor(0, V, T, N, UV, NC, NT, false);
}

static void RebuildObbBox(AActor* A)
{
	UProceduralMeshComponent* M = A ? Cast<UProceduralMeshComponent>(A->GetRootComponent()) : nullptr;
	if (!M) return;
	M->ClearAllMeshSections();
	BuildBox(M, BF6_ObbSizeUE(BF6_ObbSizeGodot(A)));
	BF6_ApplyVolumeMaterial(A, M);
}

static AActor* SpawnObbActor(UWorld* W, const FTransform& Xf, const FVector& SizeGodot)
{
	AActor* A = W->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
	if (!A) return nullptr;
	BF6_SetPrettyLabel(A, TEXT("OBBVolume"));
	A->Tags.Add(kPlacedTag);
	A->Tags.Add(kObbTag);
	A->Tags.Add(FName(TEXT("label:OBBVolume")));
	BF6_SetObbSizeTag(A, SizeGodot);
	UProceduralMeshComponent* M = MakeProcMesh(A, TEXT("Obb"));
	A->SetActorTransform(Xf);
	RebuildObbBox(A);
	BF6_FileActor(A);
	return A;
}

// ---- session save / load (JSON of placed objects) ----
// Layout: saves/<Custom map name>/<Level>.json - one folder per custom map
// with the level file inside, so the folder IS the project you back up or
// share. Saves from the old flat layout (<Level>/<Name>.json) keep loading
// and listing, and migrate the next time they're saved.
static FString BF6_SavesRoot()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("saves"));
}
static FString BF6_SessionPathNew(const FString& Level, const FString& Name)
{
	return BF6_SavesRoot() / Name / (Level + TEXT(".json"));
}
static FString BF6_SessionPathOld(const FString& Level, const FString& Name)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), Level) / (Name + TEXT(".json"));
}
static FString BF6_SessionPathFor(const FString& Level, const FString& Name)   // load: new layout wins
{
	const FString N = BF6_SessionPathNew(Level, Name);
	return FPaths::FileExists(N) ? N : BF6_SessionPathOld(Level, Name);
}

// A custom map's name becomes a FOLDER on disk, so the filesystem has a veto.
// Without this a name like "M4/Sherman" wrote nothing, the session still
// switched to editing, and the user was told it had been created - a save that
// never existed and only announced itself much later.
//
// Reserved device names are in here because Windows rejects them anywhere in a
// path, extension or not, which is the least guessable failure of the set.
static bool BF6_ValidSaveName(const FString& Name, FString& OutWhy)
{
	if (Name.IsEmpty()) { OutWhy = TEXT("Give the custom map a name first."); return false; }
	if (Name.Len() > 100) { OutWhy = TEXT("That name is too long - keep it under 100 characters."); return false; }

	const FString Bad = TEXT("\\/:*?\"<>|");
	for (const TCHAR C : Name)
	{
		int32 Ignore;
		if (Bad.FindChar(C, Ignore) || C < 32)
		{
			OutWhy = TEXT("A name can't contain  \\ / : * ? \" < > |  because it becomes a folder on disk.");
			return false;
		}
	}
	if (Name.EndsWith(TEXT(".")) || Name.EndsWith(TEXT(" ")))
	{
		OutWhy = TEXT("A name can't end with a dot or a space - Windows drops them and the folder would not match.");
		return false;
	}
	static const TCHAR* Reserved[] = { TEXT("CON"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL"),
		TEXT("COM1"), TEXT("COM2"), TEXT("COM3"), TEXT("COM4"), TEXT("COM5"), TEXT("COM6"), TEXT("COM7"), TEXT("COM8"), TEXT("COM9"),
		TEXT("LPT1"), TEXT("LPT2"), TEXT("LPT3"), TEXT("LPT4"), TEXT("LPT5"), TEXT("LPT6"), TEXT("LPT7"), TEXT("LPT8"), TEXT("LPT9") };
	FString Stem = Name;
	int32 Dot;
	if (Stem.FindChar(TEXT('.'), Dot)) Stem = Stem.Left(Dot);
	for (const TCHAR* R : Reserved)
		if (Stem.Equals(R, ESearchCase::IgnoreCase))
		{
			OutWhy = FString::Printf(TEXT("'%s' is a reserved name on Windows - pick another."), *Stem);
			return false;
		}
	return true;
}

static TArray<FString> ListSaves(const FString& Level)
{
	TArray<FString> Out;
	// new layout: every custom-map folder that holds this level's file
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(BF6_SavesRoot() / TEXT("*")), false, true);
	for (const FString& D : Dirs)
		if (FPaths::FileExists(BF6_SavesRoot() / D / (Level + TEXT(".json"))))
			Out.AddUnique(D);
	// old flat layout keeps listing until re-saved
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), Level) / TEXT("*.json")), true, false);
	for (FString& F : Files) Out.AddUnique(FPaths::GetBaseFilename(F));
	return Out;
}

// A cheap signature of every save on disk.
//
// The map menu snapshots the save lists when it is built, so a creator who
// deletes a save folder in Explorer with the menu open keeps looking at a
// RESUME dropdown for a file that is gone - and clicking it opens nothing.
// Polling this while the menu is up costs two directory reads plus one per
// save folder, which is nothing at a second apart.
//
// Names only, because names are exactly what the menu lists: editing a save's
// contents changes nothing the menu shows. Sorted so the fingerprint depends
// on WHAT is there, not on the order the filesystem happened to hand it back.
static uint32 BF6_SavesFingerprint()
{
	uint32 H = 2166136261u;   // FNV-1a
	auto Fold = [&H](const FString& S)
	{
		for (const TCHAR C : S) { H ^= (uint32)FChar::ToLower(C); H *= 16777619u; }
		H ^= 0xFFu; H *= 16777619u;   // separator, so "ab"+"c" cannot collide with "a"+"bc"
	};
	auto FoldDir = [&Fold](const FString& Dir)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), true, false);
		Files.Sort();
		for (const FString& F : Files) Fold(F);
	};

	// new layout: saves/<custom map>/<Level>.json
	const FString Root = BF6_SavesRoot();
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("*")), false, true);
	Dirs.Sort();
	for (const FString& D : Dirs) { Fold(D); FoldDir(Root / D); }

	// old flat layout: <Level>/<Name>.json, a sibling of saves/. Matched on the
	// level prefix so the tool's own churning folders (thumbs, export, sdk)
	// cannot make this look like a change every second.
	const FString Base = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"));
	TArray<FString> Levels;
	IFileManager::Get().FindFiles(Levels, *(Base / TEXT("MP_*")), false, true);
	Levels.Sort();
	for (const FString& L : Levels) { Fold(L); FoldDir(Base / L); }
	return H;
}

static FString TagValue(AActor* A, const FString& Prefix)
{
	for (const FName& T : A->Tags) { FString S = T.ToString(); if (S.StartsWith(Prefix)) return S.RightChop(Prefix.Len()); }
	return FString();
}

// ---- loop <-> actor-tag mirror, so zone shapes ride the undo system ----
// Tags are transactional with the actor; the live GVolumeLoops map is not.
// Every loop mutation happens inside a transaction that Modify()s the actor
// and rewrites these tags; the post-undo repair reads them back.
static void BF6_WriteLoopTags(AActor* A)
{
	for (int32 i = A->Tags.Num() - 1; i >= 0; i--)
		if (A->Tags[i].ToString().StartsWith(TEXT("loop"))) A->Tags.RemoveAt(i);
	const TArray<FVector>* Loop = GVolumeLoops.Find(A);
	if (!Loop) return;
	FString S;
	for (const FVector& P : *Loop) S += FString::Printf(TEXT("%.1f %.1f %.1f|"), P.X, P.Y, P.Z);
	int32 Chunk = 0;
	while (S.Len())
	{
		const FString Part = S.Left(900);
		S.RightChopInline(Part.Len());
		A->Tags.Add(FName(*FString::Printf(TEXT("loop%d:%s"), Chunk++, *Part)));
	}
}

static bool BF6_ReadLoopTags(AActor* A, TArray<FVector>& Out)
{
	FString S;
	for (int32 Chunk = 0; ; Chunk++)
	{
		const FString V = TagValue(A, FString::Printf(TEXT("loop%d:"), Chunk));
		if (V.IsEmpty()) break;
		S += V;
	}
	if (S.IsEmpty()) return false;
	TArray<FString> Pts; S.ParseIntoArray(Pts, TEXT("|"));
	Out.Reset();
	for (const FString& P : Pts)
	{
		TArray<FString> C; P.ParseIntoArray(C, TEXT(" "));
		if (C.Num() == 3) Out.Add(FVector(FCString::Atod(*C[0]), FCString::Atod(*C[1]), FCString::Atod(*C[2])));
	}
	return Out.Num() >= 3;
}

// one place for every loop mutation: record the actor, store, mirror, rebuild
// (the caller owns the surrounding transaction)
static void BF6_ApplyLoop(AActor* Vol, const TArray<FVector>& LocalLoop)
{
	Vol->Modify();
	GVolumeLoops.Add(Vol, LocalLoop);
	BF6_WriteLoopTags(Vol);
	RebuildVolumeWalls(Vol, LocalLoop);
}

// Bring the tree tags back in line with the live hierarchy before writing them.
// gtree: records where a node was when it arrived; the creator may have dragged
// it somewhere else since, and the save has to keep what they built, not what
// the scene file once said.
static void BF6_SyncTreeTagsFromLive()
{
	if (!GEditor) return;
	UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		AActor* A = *It;
		if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag) && !A->Tags.Contains(kGroupTag)) continue;
		// every node needs a key of its own before anything can point at it
		if (TagValue(A, TEXT("gpath:")).IsEmpty())
		{
			FString Own = A->GetActorLabel(); Own.RemoveFromStart(TEXT("BF6_"));
			A->Tags.Add(FName(*(FString(TEXT("gpath:")) + Own)));
		}
		FString Want;
		if (AActor* P = A->GetAttachParentActor())
		{
			Want = TagValue(P, TEXT("gpath:"));
			if (Want.IsEmpty()) { Want = P->GetActorLabel(); Want.RemoveFromStart(TEXT("BF6_")); }
		}
		const FString Have = TagValue(A, TEXT("gtree:"));
		if (Have == Want) continue;
		if (!Have.IsEmpty()) A->Tags.Remove(FName(*(FString(TEXT("gtree:")) + Have)));
		if (!Want.IsEmpty()) A->Tags.Add(FName(*(FString(TEXT("gtree:")) + Want)));
	}
}

static bool SaveSession(const FString& Level, const FString& Name)
{
	BF6_SyncTreeTagsFromLive();
	if (!GEditor || Name.IsEmpty()) return false;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return false;
	TArray<TSharedPtr<FJsonValue>> Objs;
	TMap<AGroupActor*, int32> GroupIdx;   // group root -> stable save index
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const bool bNode = It->Tags.Contains(kGroupTag);
		if (!It->Tags.Contains(kPlacedTag) && !bNode) continue;
		if (It->Tags.Contains(kHandleTag)) continue;
		const FString MeshName = TagValue(*It, TEXT("mesh:"));
		const FString LabelName = TagValue(*It, TEXT("label:"));
		// volumes carry no mesh - the label identifies them. A node carries
		// neither: it IS the tree, and rebuilding it from a child's tags would
		// lose the pivot it was given and drop empty nodes altogether.
		if (!bNode && MeshName.IsEmpty() && LabelName.IsEmpty()) continue;
		const FTransform Xf = It->GetActorTransform();
		const FVector L = Xf.GetLocation(); const FRotator R = Xf.Rotator(); const FVector S = Xf.GetScale3D();
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("mesh"), MeshName);
		if (bNode) O->SetBoolField(TEXT("node"), true);
		O->SetStringField(TEXT("label"), TagValue(*It, TEXT("label:")));
		// identity that must SURVIVE a reload: the actor's own name (links
		// between placed objects reference it), its block tags, and which
		// group it belongs to - without these, reloading dissolved groups,
		// orphaned blocks, and dangled every placed-to-placed link
		{
			FString Nm = It->GetActorLabel();
			Nm.RemoveFromStart(TEXT("BF6_"));
			O->SetStringField(TEXT("nm"), Nm);
			TArray<TSharedPtr<FJsonValue>> RawTags;
			for (const FName& T : It->Tags)
			{
				const FString TS = T.ToString();
				if (TS.StartsWith(TEXT("blk:")) || TS.StartsWith(TEXT("blkid:")) || TS.StartsWith(TEXT("blkat:"))
					|| TS.StartsWith(TEXT("gtree:")) || TS.StartsWith(TEXT("gpath:"))   // the authored tree
					|| TS.StartsWith(TEXT("gord:"))                              // and its order
					|| TS.StartsWith(TEXT("tint:"))                             // and its colour
					|| TS.StartsWith(TEXT("vcol:")))                            // a volume's own colour
					RawTags.Add(MakeShared<FJsonValueString>(TS));
			}
			if (RawTags.Num()) O->SetArrayField(TEXT("tags"), RawTags);
			if (AGroupActor* Rt = AGroupActor::GetRootForActor(*It))
			{
				int32* Gi = GroupIdx.Find(Rt);
				if (!Gi) Gi = &GroupIdx.Add(Rt, GroupIdx.Num());
				O->SetNumberField(TEXT("grp"), *Gi);
			}
		}
		O->SetNumberField(TEXT("x"), L.X); O->SetNumberField(TEXT("y"), L.Y); O->SetNumberField(TEXT("z"), L.Z);
		O->SetNumberField(TEXT("pitch"), R.Pitch); O->SetNumberField(TEXT("yaw"), R.Yaw); O->SetNumberField(TEXT("roll"), R.Roll);
		O->SetNumberField(TEXT("sx"), S.X); O->SetNumberField(TEXT("sy"), S.Y); O->SetNumberField(TEXT("sz"), S.Z);
		// edited attribute values ("Key=Value" strings from the p: tags)
		TArray<TSharedPtr<FJsonValue>> PTags;
		for (const FName& T : It->Tags)
		{
			const FString TS = T.ToString();
			if (TS.StartsWith(TEXT("p:"))) PTags.Add(MakeShared<FJsonValueString>(TS.Mid(2)));
		}
		if (PTags.Num()) O->SetArrayField(TEXT("props"), PTags);
		// placed zone volumes keep their polygon (world space, like base zones)
		if (const TArray<FVector>* Loop = GVolumeLoops.Find(*It))
		{
			TArray<TSharedPtr<FJsonValue>> LArr;
			for (const FVector& Vv : BF6_LoopToWorld(*It, *Loop))
			{
				LArr.Add(MakeShared<FJsonValueNumber>(Vv.X));
				LArr.Add(MakeShared<FJsonValueNumber>(Vv.Y));
				LArr.Add(MakeShared<FJsonValueNumber>(Vv.Z));
			}
			O->SetArrayField(TEXT("loop"), LArr);
		}
		Objs.Add(MakeShared<FJsonValueObject>(O));
	}
	// base-object edits: attribute values, gizmo moves, and reshaped zones on
	// the map's shipped gameplay objects also belong to the custom map
	TArray<TSharedPtr<FJsonValue>> BaseArr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(kBaseTag)) continue;
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		FString Nm = It->GetActorLabel();
		Nm.RemoveFromStart(TEXT("BF6_"));
		B->SetStringField(TEXT("name"), Nm);
		const FTransform Xf = It->GetActorTransform();
		const FVector L = Xf.GetLocation(); const FRotator R = Xf.Rotator(); const FVector Sc = Xf.GetScale3D();
		B->SetNumberField(TEXT("x"), L.X); B->SetNumberField(TEXT("y"), L.Y); B->SetNumberField(TEXT("z"), L.Z);
		B->SetNumberField(TEXT("pitch"), R.Pitch); B->SetNumberField(TEXT("yaw"), R.Yaw); B->SetNumberField(TEXT("roll"), R.Roll);
		B->SetNumberField(TEXT("sx"), Sc.X); B->SetNumberField(TEXT("sy"), Sc.Y); B->SetNumberField(TEXT("sz"), Sc.Z);
		TArray<TSharedPtr<FJsonValue>> PTags;
		for (const FName& T : It->Tags)
		{
			const FString TS = T.ToString();
			if (TS.StartsWith(TEXT("p:"))) PTags.Add(MakeShared<FJsonValueString>(TS.Mid(2)));
		}
		if (PTags.Num()) B->SetArrayField(TEXT("props"), PTags);
		if (const TArray<FVector>* Loop = GVolumeLoops.Find(*It))
		{
			TArray<TSharedPtr<FJsonValue>> LArr;
			for (const FVector& V : BF6_LoopToWorld(*It, *Loop))
			{
				LArr.Add(MakeShared<FJsonValueNumber>(V.X));
				LArr.Add(MakeShared<FJsonValueNumber>(V.Y));
				LArr.Add(MakeShared<FJsonValueNumber>(V.Z));
			}
			B->SetArrayField(TEXT("loop"), LArr);
		}
		BaseArr.Add(MakeShared<FJsonValueObject>(B));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	// humans open these by hand: make it unmistakable that a session save is
	// NOT an uploadable spatial file (uploading one fails with the site's
	// cryptic "no accepted layers found in provided level")
	Root->SetStringField(TEXT("_note"), TEXT("BF6 Unreal SDK session save - NOT for the Portal site. Use EXPORT in the tool to get the uploadable <map>.spatial.json."));
	Root->SetStringField(TEXT("level"), Level);
	Root->SetArrayField(TEXT("objects"), Objs);
	Root->SetArrayField(TEXT("base"), BaseArr);
	// THIS SAVE'S BASE LIST IS THE WHOLE TRUTH, and saying so is what lets a
	// reload honour a DELETION.
	//
	// The map's shipped gameplay objects are respawned from the base setup file
	// every time a map opens, and the save then edits them. That works for a
	// moved or retuned object and silently fails for a removed one: nothing in
	// the save says "this used to exist and does not any more", so the object
	// came back on every resume. Worse, it came back BEFORE an export, so a
	// creator who deleted the shipped objects still shipped them - which is what
	// "the export seems to include the original map data" was.
	//
	// A flag rather than a list of removals, because the surviving set is
	// already written above and a second list could disagree with it. Older
	// saves have no flag and keep the old behaviour, which is the only safe
	// reading of them: their base list was never guaranteed complete.
	Root->SetBoolField(TEXT("base_complete"), true);
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), W);
	const FString Path = BF6_SessionPathNew(Level, Name);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	// The RESULT matters. A name the filesystem will not take produced a silent
	// no-write, and the caller then reported success for a file that was never
	// created - worse than failing, because nothing looks wrong until the save
	// is gone.
	if (!FFileHelper::SaveStringToFile(Out, *Path))
	{
		UE_LOG(LogBF6, Error, TEXT("Could not write the session to %s"), *Path);
		return false;
	}
	// the same save under the OLD flat layout is superseded - remove it so
	// the resume list never shows doubles
	const FString Old = BF6_SessionPathOld(Level, Name);
	if (FPaths::FileExists(Old)) IFileManager::Get().Delete(*Old);
	UE_LOG(LogBF6, Display, TEXT("Saved %d object(s) to %s"), Objs.Num(), *Path);
	return true;
}

static void LoadSession(const FString& Level, const FString& Name)
{
	const double LoadT0 = FPlatformTime::Seconds();
	GMeshDecodeSec = GMeshBuildSec = 0.0; GMeshCalls = GMeshCacheHits = 0;
	GLabelSec = GSpawnSec = GStatSec = GFolderSec = 0.0;
	// one scan of the world up front instead of one per object
	FCachedActorLabels BulkLabels;
	if (UWorld* LW = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr) BulkLabels.Populate(LW);
	GBulkLabels = &BulkLabels;
	ON_SCOPE_EXIT { GBulkLabels = nullptr; };
	if (Name.IsEmpty()) return;
	const FString Path = BF6_SessionPathFor(Level, Name);
	FString In;
	if (!FFileHelper::LoadFileToString(In, *Path)) { UE_LOG(LogBF6, Warning, TEXT("no save at %s"), *Path); return; }
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
	ClearActorsWithTag(kPlacedTag);
	ClearActorsWithTag(kGroupTag);
	const TArray<TSharedPtr<FJsonValue>>* Objs = nullptr;
	if (!Root->TryGetArrayField(TEXT("objects"), Objs)) return;
	int n = 0;
	TMap<int32, TArray<AActor*>> PendingGroups;   // saved group index -> members
	TMap<FString, FString> Renamed;   // requested label -> what the editor made of it
	TArray<AActor*> LoadedActors;
	for (const auto& V : *Objs)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) continue;
		const FString MeshName = O->GetStringField(TEXT("mesh"));
		const FString Label = O->GetStringField(TEXT("label"));
		FVector L(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")), O->GetNumberField(TEXT("z")));
		FRotator Rot(O->GetNumberField(TEXT("pitch")), O->GetNumberField(TEXT("yaw")), O->GetNumberField(TEXT("roll")));
		FVector Sc(O->GetNumberField(TEXT("sx")), O->GetNumberField(TEXT("sy")), O->GetNumberField(TEXT("sz")));

		AActor* A = nullptr;
		// a tree node: no model, no label, just the pivot it was given
		bool bIsNode = false;
		if (O->TryGetBoolField(TEXT("node"), bIsNode) && bIsNode)
		{
			if (UWorld* Wld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
				A = BF6_SpawnTreeNode(Wld, FString(), FString(), FTransform(Rot, L, Sc));
		}
		const TArray<TSharedPtr<FJsonValue>>* LArr = nullptr;
		if (!A && O->TryGetArrayField(TEXT("loop"), LArr) && LArr->Num() >= 9)
		{
			// a placed zone volume: rebuild it from its polygon
			if (UWorld* Wld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
			{
				TArray<FVector> Loop;
				for (int32 i = 0; i + 2 < LArr->Num(); i += 3)
					Loop.Add(FVector((*LArr)[i]->AsNumber(), (*LArr)[i+1]->AsNumber(), (*LArr)[i+2]->AsNumber()));
				A = Wld->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
				if (A)
				{
					const FString Lb = Label.IsEmpty() ? FString(TEXT("PolygonVolume")) : Label;
					BF6_SetPrettyLabel(A, Lb);
					A->Tags.Add(kPlacedTag);
					A->Tags.Add(FName(*(FString(TEXT("label:")) + Lb)));
					MakeProcMesh(A, TEXT("Volume"));
					GVolumeLoops.Add(A, Loop);
					BF6_WriteLoopTags(A);
					BF6_FileActor(A);
				}
			}
		}
		else if (!A && Label == TEXT("OBBVolume"))
		{
			if (UWorld* Wld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
				A = SpawnObbActor(Wld, FTransform(Rot, L, Sc), FVector(10, 10, 10));
		}
		else if (!A)   // a node is already made above
			A = SpawnSdkModel(MeshName, Label, FTransform(Rot, L, Sc));

		if (A)
		{
			// restore edited attribute values
			const TArray<TSharedPtr<FJsonValue>>* PTags = nullptr;
			if (O->TryGetArrayField(TEXT("props"), PTags))
			{
				for (int32 i = A->Tags.Num() - 1; i >= 0; i--)
					if (A->Tags[i].ToString().StartsWith(TEXT("p:"))) A->Tags.RemoveAt(i);
				for (const auto& PV : *PTags)
				{
					FString KV;
					if (PV->TryGetString(KV) && !KV.IsEmpty()) A->Tags.Add(FName(*(FString(TEXT("p:")) + KV)));
				}
			}
			// restore identity: the saved NAME (links between placed objects
			// point at it), block tags, and pending group membership
			FString Nm;
			if (O->TryGetStringField(TEXT("nm"), Nm) && !Nm.IsEmpty())
			{
				BF6_SetPrettyLabel(A, Nm);
				// The editor may UNIQUIFY this label against actors that already
				// exist - above all the base setup, whose objects often share
				// names with a map imported from Godot (creators copy the base
				// layout's names). Every saved link points at the REQUESTED
				// name, so each rename here silently breaks the links aimed at
				// this object. Record the rename; they are remapped below.
				FString Got = A->GetActorLabel(); Got.RemoveFromStart(TEXT("BF6_"));
				if (Got != Nm) Renamed.Add(Nm, Got);
				LoadedActors.Add(A);
			}
			else LoadedActors.Add(A);
			const TArray<TSharedPtr<FJsonValue>>* RawTags = nullptr;
			if (O->TryGetArrayField(TEXT("tags"), RawTags))
				for (const auto& TV : *RawTags)
				{
					FString TS;
					if (TV->TryGetString(TS) && !TS.IsEmpty()) A->Tags.Add(FName(*TS));
				}
			double G = -1.0;
			if (O->TryGetNumberField(TEXT("grp"), G)) PendingGroups.FindOrAdd((int32)G).Add(A);
			BF6_FileActor(A);   // block tags may change its outliner folder
			BF6Api::ReapplyTint(A);   // a saved colour comes back with the object
			// visuals that depend on the restored values
			if (const TArray<FVector>* Lp = GVolumeLoops.Find(A)) RebuildVolumeWalls(A, *Lp);
			if (BF6Api::IsObbActor(A)) RebuildObbBox(A);
			n++;
		}
	}

	// FOLLOW THE RENAMES THROUGH THE LINKS.
	//
	// The save was internally consistent: its links point at the names its
	// objects were saved under. If the editor renamed any of them on the way
	// in, every link aimed at an old name now points at nothing - or worse, at
	// a base-setup object that happens to wear it. The Conquest HQs of a real
	// creator map lost their areas and spawn lists exactly this way. The links
	// follow the session's OWN objects, never whatever else holds the name.
	if (Renamed.Num())
	{
		int32 Fixed = 0;
		for (AActor* A : LoadedActors)
		{
			if (!A) continue;
			FString Ty = TagValue(A, TEXT("label:"));
			if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
			if (Ty.IsEmpty()) continue;
			for (const BF6Api::FPropDef& D : BF6Api::PropsForType(Ty))
			{
				if (!BF6Api::BF6_IsLinkProp(Ty, D.Name)) continue;
				const FString V = BF6Api::GetActorProp(A, D.Name);
				if (V.IsEmpty() || V.Contains(TEXT("NodePath"))) continue;
				TArray<FString> Parts; V.ParseIntoArray(Parts, TEXT(","), true);
				bool bChanged = false;
				for (FString& Pt : Parts)
				{
					Pt.TrimStartAndEndInline();
					if (const FString* New = Renamed.Find(Pt)) { Pt = *New; bChanged = true; }
				}
				if (bChanged) { BF6Api::SetActorProp(A, D.Name, FString::Join(Parts, TEXT(","))); Fixed++; }
			}
		}
		if (Fixed > 0) UE_LOG(LogBF6, Warning, TEXT("reload renamed %d object(s); %d link field(s) re-pointed"), Renamed.Num(), Fixed);
	}

	// groups re-form exactly as saved (block instances included) and come
	// back locked, so a reloaded block still moves as one thing
	if (PendingGroups.Num())
	{
		if (!UActorGroupingUtils::IsGroupingActive()) UActorGroupingUtils::SetGroupingActive(true);
		for (auto& PG : PendingGroups)
			if (PG.Value.Num() > 1) UActorGroupingUtils::Get()->GroupActors(PG.Value);
	}

	// apply saved base-object edits (attributes, moves, reshaped zones) on top
	// of the freshly loaded base setup
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* BaseArr = nullptr;
	if (World && Root->TryGetArrayField(TEXT("base"), BaseArr))
	{
		TMap<FString, AActor*> ByName;
		for (TActorIterator<AActor> It(World); It; ++It)
			if (It->Tags.Contains(kBaseTag))
			{
				FString Lb = It->GetActorLabel();
				Lb.RemoveFromStart(TEXT("BF6_"));
				ByName.Add(Lb, *It);
			}
		// WHAT THE SAVE DOES NOT LIST, THE CREATOR DELETED - but only for a save
		// that claims a complete list. Reading an older save this way would
		// treat every object it happened not to record as a deletion and strip
		// the map's gameplay setup on open, which is far worse than the bug it
		// fixes.
		bool bComplete = false;
		Root->TryGetBoolField(TEXT("base_complete"), bComplete);
		if (bComplete)
		{
			TSet<FString> Kept;
			for (const auto& BV : *BaseArr)
				if (const TSharedPtr<FJsonObject> B = BV->AsObject())
					Kept.Add(B->GetStringField(TEXT("name")));

			int32 Removed = 0;
			for (const TPair<FString, AActor*>& Pair : ByName)
				if (!Kept.Contains(Pair.Key) && Pair.Value)
				{
					GVolumeLoops.Remove(Pair.Value);
					Pair.Value->Destroy();
					Removed++;
				}
			if (Removed > 0)
			{
				UE_LOG(LogBF6, Warning,
					TEXT("  base setup: %d object(s) stayed deleted"), Removed);
				// The name map is now full of dangling pointers, and the loop
				// below looks names up in it.
				ByName.Reset();
				for (TActorIterator<AActor> It(World); It; ++It)
					if (It->Tags.Contains(kBaseTag))
					{
						FString Lb = It->GetActorLabel();
						Lb.RemoveFromStart(TEXT("BF6_"));
						ByName.Add(Lb, *It);
					}
			}
		}

		for (const auto& BV : *BaseArr)
		{
			const TSharedPtr<FJsonObject> B = BV->AsObject();
			if (!B.IsValid()) continue;
			AActor* A = ByName.FindRef(B->GetStringField(TEXT("name")));
			if (!A) continue;
			// zone loops rebuild the walls; point objects take the transform
			const TArray<TSharedPtr<FJsonValue>>* LArr = nullptr;
			if (B->TryGetArrayField(TEXT("loop"), LArr) && LArr->Num() >= 9)
			{
				TArray<FVector> Loop;
				for (int32 i = 0; i + 2 < LArr->Num(); i += 3)
					Loop.Add(FVector((*LArr)[i]->AsNumber(), (*LArr)[i+1]->AsNumber(), (*LArr)[i+2]->AsNumber()));
				GVolumeLoops.Add(A, Loop);
				BF6_WriteLoopTags(A);
				RebuildVolumeWalls(A, Loop);
			}
			else if (B->HasField(TEXT("x")))
			{
				const FVector L(B->GetNumberField(TEXT("x")), B->GetNumberField(TEXT("y")), B->GetNumberField(TEXT("z")));
				const FRotator Rt(B->GetNumberField(TEXT("pitch")), B->GetNumberField(TEXT("yaw")), B->GetNumberField(TEXT("roll")));
				const FVector Sc(B->GetNumberField(TEXT("sx")), B->GetNumberField(TEXT("sy")), B->GetNumberField(TEXT("sz")));
				A->SetActorTransform(FTransform(Rt, L, Sc));
			}
			const TArray<TSharedPtr<FJsonValue>>* PTags = nullptr;
			if (B->TryGetArrayField(TEXT("props"), PTags))
			{
				// saved values replace the shipped ones
				for (int32 i = A->Tags.Num() - 1; i >= 0; i--)
					if (A->Tags[i].ToString().StartsWith(TEXT("p:"))) A->Tags.RemoveAt(i);
				for (const auto& PV : *PTags)
				{
					FString KV;
					if (PV->TryGetString(KV) && !KV.IsEmpty()) A->Tags.Add(FName(*(FString(TEXT("p:")) + KV)));
				}
				// props land AFTER the loop rebuild above: if this is a zone and
				// the session saved a custom height, stretch the walls to it now
				if (const TArray<FVector>* Lp = GVolumeLoops.Find(A))
					RebuildVolumeWalls(A, *Lp);
			}
		}
	}
	// the tree the creator authored, rebuilt from the keys the save kept
	if (BF6_KeepGodotTree()) BF6_RebuildTreeFromTags();
	UE_LOG(LogBF6, Display, TEXT("Loaded %d object(s) from %s"), n, *Path);
	UE_LOG(LogBF6, Display, TEXT("  load: %.2fs total - %.2fs reading models (%d of %d already cached), %.2fs building meshes"),
		FPlatformTime::Seconds() - LoadT0, GMeshDecodeSec, GMeshCacheHits, GMeshCalls, GMeshBuildSec);
	UE_LOG(LogBF6, Display, TEXT("  load detail: %.2fs naming, %.2fs spawning, %.2fs file checks, %.2fs outliner"),
		GLabelSec, GSpawnSec, GStatSec, GFolderSec);
}

// Carries a placeable while dragging from the list into the level viewport.
class FBF6PlaceableDragDropOp : public FDragDropOperation
{
public:
	DRAG_DROP_OPERATOR_TYPE(FBF6PlaceableDragDropOp, FDragDropOperation)
	FString MeshName, Label;
	static TSharedRef<FBF6PlaceableDragDropOp> New(const FString& InMesh, const FString& InLabel)
	{
		TSharedRef<FBF6PlaceableDragDropOp> Op = MakeShared<FBF6PlaceableDragDropOp>();
		Op->MeshName = InMesh; Op->Label = InLabel; Op->Construct();
		return Op;
	}
	virtual TSharedPtr<SWidget> GetDefaultDecorator() const override
	{
		return SNew(SBorder).BorderBackgroundColor(FLinearColor(0.02f, 0.03f, 0.04f, 0.85f)).Padding(FMargin(8, 4))
			[ SNew(STextBlock).ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.55f, 0.18f))).Text(FText::FromString(FString::Printf(TEXT("+ %s"), *Label))) ];
	}
	virtual void OnDrop(bool bDropWasHandled, const FPointerEvent& MouseEvent) override
	{
		FDragDropOperation::OnDrop(bDropWasHandled, MouseEvent);
		if (!MeshName.IsEmpty()) SpawnSdkModelAtCursor(MeshName, Label);
	}
};

// A brief on-screen toast (top-right of the editor).
void Notify(const FString& Msg)
{
	FNotificationInfo Info(FText::FromString(Msg));
	Info.ExpireDuration = 3.5f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

struct FPlaceableRow { FString Type, Directory, Mesh; int32 PhysicsCost = 0; bool Universal = false; };

// A base-setup object we spawned (HQ, spawn, combat area...) + its current field
// values, so the property panel can show/edit them.
struct FBaseObj { FString Type; TMap<FString, FString> Props; };

// ================================ the editor widget ==========================
class SBF6Browser : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6Browser) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		BuildMapGrid();
		ChildSlot
		[
			SAssignNew(Switcher, SWidgetSwitcher)
			+ SWidgetSwitcher::Slot() [ BuildPicker() ]     // 0 = map picker
			+ SWidgetSwitcher::Slot() [ BuildEditor() ]     // 1 = object editor
		];
		Switcher->SetActiveWidgetIndex(0);
	}

private:
	FString DisplayName(const FString& Code) const
	{
		for (int i = 0; i < GBF6MapCardCount; i++) if (Code == GBF6MapCards[i].Code) return GBF6MapCards[i].Name;
		return Code;
	}

	// ---------- picker ----------
	const FSlateBrush* LoadCardBrush(const FString& PngFile)
	{
		if (PngFile.IsEmpty()) return nullptr;
		const FString Path = FPaths::Combine(BF6_DataDir(), TEXT("maps"), PngFile);
		if (!FPaths::FileExists(Path)) return nullptr;
		UTexture2D* Tex = FImageUtils::ImportFileAsTexture2D(Path);
		if (!Tex) return nullptr;
		CardTextures.Add(TStrongObjectPtr<UTexture2D>(Tex));
		TSharedPtr<FSlateBrush> Brush = MakeShared<FSlateBrush>();
		Brush->SetResourceObject(Tex); Brush->ImageSize = FVector2D(Tex->GetSizeX(), Tex->GetSizeY()); Brush->DrawAs = ESlateBrushDrawType::Image;
		CardBrushes.Add(Brush);
		return Brush.Get();
	}

	void BuildMapGrid()
	{
		TSharedRef<SWrapBox> Grid = SNew(SWrapBox).UseAllottedSize(true);
		for (int i = 0; i < GBF6MapCardCount; i++)
		{
			const FString Code = GBF6MapCards[i].Code;
			const FString Name = GBF6MapCards[i].Name;
			const int Count = PlaceableCount(Code);
			const FSlateBrush* Brush = LoadCardBrush(GBF6MapCards[i].Png);

			// per-card list of saves for the dropdown
			TSharedPtr<TArray<TSharedPtr<FString>>> Saves = MakeShared<TArray<TSharedPtr<FString>>>();
			Saves->Add(MakeShared<FString>(TEXT("(new session)")));
			for (const FString& S : ListSaves(Code)) Saves->Add(MakeShared<FString>(S));
			CardSaveLists.Add(Saves);

			TSharedRef<SOverlay> Face = SNew(SOverlay);
			if (Brush) Face->AddSlot()[ SNew(SImage).Image(Brush) ];
			else       Face->AddSlot()[ SNew(SBorder).BorderBackgroundColor(FLinearColor(0.16f,0.19f,0.21f)) ];
			Face->AddSlot().VAlign(VAlign_Bottom)
			[
				SNew(SBorder).BorderBackgroundColor(FLinearColor(0.02f,0.03f,0.04f,0.72f)).Padding(FMargin(6,3))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(FText::FromString(Name)).ColorAndOpacity(FSlateColor(FLinearColor::White)) ]
					+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%d objects  -  %d save(s)"), Count, Saves->Num()-1))).ColorAndOpacity(FSlateColor(FLinearColor(1.0f,0.55f,0.18f))) ]
				]
			];

			Grid->AddSlot().Padding(6)
			[
				SNew(SBox).WidthOverride(214)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox).HeightOverride(108)
						[
							SNew(SButton).ContentPadding(0)
							.OnClicked_Lambda([this, Code]() { OpenMap(Code, FString()); return FReply::Handled(); })
							[ Face ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,3,0,0)
					[
						SNew(SComboBox<TSharedPtr<FString>>)
						.OptionsSource(Saves.Get())
						.OnGenerateWidget_Lambda([](TSharedPtr<FString> In){ return SNew(STextBlock).Text(FText::FromString(*In)); })
						.OnSelectionChanged_Lambda([this, Code](TSharedPtr<FString> Sel, ESelectInfo::Type)
						{
							if (!Sel.IsValid()) return;
							OpenMap(Code, (*Sel == TEXT("(new session)")) ? FString() : *Sel);
						})
						[ SNew(STextBlock).Text(FText::FromString(TEXT("Load save..."))) ]
					]
				]
			];
		}
		MapGrid = Grid;
	}

	TSharedRef<SWidget> BuildPicker()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(10, 10, 10, 6)
			[ SNew(STextBlock).Text(FText::FromString(TEXT("SELECT A MAP"))).ColorAndOpacity(FSlateColor(FLinearColor(0.9f,0.94f,0.97f))) ]
			+ SVerticalBox::Slot().FillHeight(1).Padding(4)
			[ SNew(SScrollBox) + SScrollBox::Slot()[ MapGrid.ToSharedRef() ] ];
	}

	// ---------- editor ----------
	TSharedRef<SWidget> BuildEditor()
	{
		return SNew(SVerticalBox)
			// header row 1: back + map name + edit status
			+ SVerticalBox::Slot().AutoHeight().Padding(8,8,8,2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,10,0)
				[ SNew(SButton).Text(FText::FromString(TEXT("< Maps"))).OnClicked_Lambda([this]{ Switcher->SetActiveWidgetIndex(0); return FReply::Handled(); }) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,16,0)
				[ SNew(STextBlock).ColorAndOpacity(FSlateColor(FLinearColor(1.0f,0.55f,0.18f))).Text_Lambda([this]{ return FText::FromString(DisplayName(CurrentLevel)); }) ]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[ SNew(STextBlock)
					.Text_Lambda([this]{ return FText::FromString(bEditing ? FString::Printf(TEXT("Editing custom map:  %s"), *CurrentSave) : FString(TEXT("BASE MAP (read-only) - create a custom map to start building"))); })
					.ColorAndOpacity_Lambda([this]{ return bEditing ? FSlateColor(FLinearColor(0.45f,0.82f,0.55f)) : FSlateColor(FLinearColor(0.9f,0.72f,0.32f)); }) ]
			]
			// header row 2: custom-map controls
			+ SVerticalBox::Slot().AutoHeight().Padding(8,0,8,4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1)
				[
					SAssignNew(SaveNameBox, SEditableTextBox).HintText(FText::FromString(TEXT("custom map name")))
						// Enter creates it, same as the button beside it.
						.OnTextCommitted_Lambda([this](const FText&, ETextCommit::Type How)
							{ if (How == ETextCommit::OnEnter) OnCreateCustom(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6,0,0,0)
				[ SNew(SButton).Text(FText::FromString(TEXT("Create Custom Map"))).OnClicked(this, &SBF6Browser::OnCreateCustom) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6,0,0,0)
				[ SNew(SButton).Text(FText::FromString(TEXT("Save"))).OnClicked(this, &SBF6Browser::OnSaveClicked).IsEnabled_Lambda([this]{ return bEditing; }) ]
			]
			// search
			+ SVerticalBox::Slot().AutoHeight().Padding(8,2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,10,0)
				[ SNew(STextBlock).Text_Lambda([this]{ return FText::FromString(FString::Printf(TEXT("%d objects"), Items.Num())); }).ColorAndOpacity(FSlateColor(FLinearColor(0.6f,0.68f,0.74f))) ]
				+ SHorizontalBox::Slot().FillWidth(1)
				[ SNew(SSearchBox).HintText(FText::FromString(TEXT("Fuzzy search placeables..."))).OnTextChanged(this, &SBF6Browser::OnSearch) ]
			]
			// list + preview
			+ SVerticalBox::Slot().FillHeight(1).Padding(4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.5f).Padding(0,0,4,0)
				[
					SAssignNew(ListView, SListView<TSharedPtr<FPlaceableRow>>)
					.ListItemsSource(&Items).SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SBF6Browser::OnGenerateRow)
					.OnSelectionChanged(this, &SBF6Browser::OnSelectionChanged)
					.OnMouseButtonDoubleClick(this, &SBF6Browser::OnActivate)
				]
				+ SHorizontalBox::Slot().FillWidth(0.3f).Padding(0,0,4,0)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().FillHeight(1)[ SAssignNew(Preview, SBF6PreviewViewport) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(2)[ SNew(STextBlock).ColorAndOpacity(FSlateColor(FLinearColor(0.5f,0.55f,0.6f))).Text(FText::FromString(TEXT("click=preview  dbl=spawn  drag=place"))) ]
				]
				// layers panel (rough placeholder for the full-material toggle)
				+ SHorizontalBox::Slot().FillWidth(0.2f)
				[
					SNew(SBorder).BorderBackgroundColor(FLinearColor(0.12f,0.14f,0.16f)).Padding(8)
					[
						SNew(SVerticalBox)
						// live physics-budget readout (SDK "memory" cost; computed
						// before the minifier, which only shrinks JSON strings).
						+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,4)[ SNew(STextBlock).Text(FText::FromString(TEXT("PORTAL BUDGET"))).ColorAndOpacity(FSlateColor(FLinearColor(0.9f,0.94f,0.97f))) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,2)
						[
							SNew(SProgressBar)
							.Percent_Lambda([this]{ return CachedBudgetFrac; })
							.FillColorAndOpacity_Lambda([this]{ return FSlateColor(CachedBudgetColor); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)
						[
							SNew(STextBlock).AutoWrapText(true)
							.Text_Lambda([this]{ return CachedBudgetText; })
							.ColorAndOpacity_Lambda([this]{ return FSlateColor(CachedBudgetColor); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[ SNew(STextBlock).Text(FText::FromString(TEXT("MAP LAYERS"))).ColorAndOpacity(FSlateColor(FLinearColor(0.9f,0.94f,0.97f))) ]
						+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor(FLinearColor(0.55f,0.6f,0.65f))).Text(FText::FromString(TEXT("Low-poly terrain loads with the map. Full-texture/material toggle comes with the materials + walk pass."))) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)[ SNew(SButton).Text(FText::FromString(TEXT("Load low-poly assets (heavy)"))).OnClicked(this, &SBF6Browser::OnLoadAssets) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[ SNew(SButton).Text(FText::FromString(TEXT("Reload terrain"))).OnClicked(this, &SBF6Browser::OnReloadTerrain) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[ SNew(SButton).Text(FText::FromString(TEXT("Clear map context"))).OnClicked_Lambda([this]{ ClearActorsWithTag(kContextTag); return FReply::Handled(); }) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[ SNew(SButton).Text(FText::FromString(TEXT("Export .spatial.json (Portal)"))).OnClicked(this, &SBF6Browser::OnExportSpatial) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[ SNew(SButton).Text(FText::FromString(TEXT("Import .spatial.json..."))).OnClicked(this, &SBF6Browser::OnImportSpatial) ]
					]
				]
			];
	}

	// ---------- flow ----------
	void OpenMap(const FString& Level, const FString& SaveName)
	{
		CurrentLevel = Level;
		// Fresh pick = read-only BASE preview. Opening an existing save = editing
		// that custom map. Either way the base geometry is transient (never saved),
		// so the base is always pristine to return to.
		CurrentSave = SaveName;
		bEditing = !SaveName.IsEmpty();
		Switcher->SetActiveWidgetIndex(1);
		BF6_ClearContextFor(CurrentLevel);
		ClearActorsWithTag(kPlacedTag);
		ClearActorsWithTag(kBaseTag);
		ClearActorsWithTag(kGroupTag);   // the nodes go with them, or they double up
		LoadLevel(); ApplyFilter();
		LoadBudgetMax();
		if (ListView.IsValid()) ListView->RequestListRefresh();
		LoadTerrainContext();
		// Always bring in the low-poly asset mesh too, so the buildings/props make
		// the map recognizable (terrain alone is an anonymous green shape).
		{
			const FString AP = MeshPath(TEXT("_assets.bf6mesh"));
			if (!GContextReused) if (FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *CurrentLevel));
		}
		LoadBaseSetup();   // the map's shipped HQs, spawns, combat area (from the SDK level scene)
		if (!SaveName.IsEmpty()) LoadSession(CurrentLevel, SaveName);
	}

	FString MeshPath(const FString& Suffix) const
	{
		return FPaths::Combine(BF6_DataDir(), TEXT("mapmesh"), CurrentLevel + Suffix);
	}

	void LoadTerrainContext()
	{
		const FString P = MeshPath(TEXT("_terrain.bf6mesh"));
		if (!GContextReused) if (FPaths::FileExists(P)) SpawnContextMesh(P, FString::Printf(TEXT("%s_Terrain"), *CurrentLevel));
		else UE_LOG(LogBF6, Warning, TEXT("no low-poly terrain extracted for %s yet"), *CurrentLevel);
	}

	// Spawn the map's shipped base gameplay setup (HQs, spawn points, combat area)
	// from <level>.base.json: point objects become marker pillars, volumes become
	// wireframe-ish walls. Each is selectable and carries its field values.
	void LoadBaseSetup()
	{
		BaseObjects.Reset();
		if (!GEditor) return;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World) return;
		const FString Path = FPaths::Combine(BF6_DataDir(), TEXT("basesetup"), CurrentLevel + TEXT(".base.json"));
		FString In;
		if (!FFileHelper::LoadFileToString(In, *Path)) { UE_LOG(LogBF6, Warning, TEXT("no base setup for %s"), *CurrentLevel); return; }
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
		if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
		const TArray<TSharedPtr<FJsonValue>>* Objs = nullptr;
		if (!Root->TryGetArrayField(TEXT("objects"), Objs)) return;

		// pass 1: local pos + parent, to compose world positions (bases ~identity).
		TMap<FString, FVector> LocalPos; TMap<FString, FString> ParentOf;
		auto ReadPos = [](const TSharedPtr<FJsonObject>& O)
		{
			FVector p(0,0,0); const TArray<TSharedPtr<FJsonValue>>* a = nullptr;
			if (O->TryGetArrayField(TEXT("pos"), a) && a->Num() >= 3)
			{ p.X = (*a)[0]->AsNumber(); p.Y = (*a)[1]->AsNumber(); p.Z = (*a)[2]->AsNumber(); }
			return p;
		};
		for (const auto& v : *Objs)
		{
			const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
			const FString nm = o->GetStringField(TEXT("name"));
			LocalPos.Add(nm, ReadPos(o));
			FString par; o->TryGetStringField(TEXT("parent"), par); ParentOf.Add(nm, par);
		}
		auto ToUnreal = [](const FVector& G){ return FVector((float)G.X, (float)G.Z, (float)G.Y) * 100.f; };  // Godot -> Unreal

		int32 oid = 0, spawned = 0;
		TMap<FString, FBF6GNode> GMap;
		BF6_BuildGNodeMap(*Objs, GMap);
		for (const auto& v : *Objs)
		{
			const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
			const FString nm = o->GetStringField(TEXT("name"));
			const FString ty = o->GetStringField(TEXT("type"));
			if (BF6_IsEngineNodeType(ty)) continue;   // pivots/cameras: math only, never actors
			// FULL parent-chain accumulation: rotation rides pivots down (the
			// deploy camera's aim lives on its Camera3D parent)
			double WB[9]; FVector gw;
			BF6_GWorldOf(GMap, nm, WB, gw);
			const FRotator Rot = BF6_GRotFromB(WB);

			AActor* A = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* pts = nullptr;
			const bool bVolume = o->TryGetArrayField(TEXT("points"), pts) && pts->Num() >= 6;
			if (bVolume)
			{
				TArray<FVector> Loop;
				for (int32 i = 0; i + 1 < pts->Num(); i += 2)
				{
					const float px = (*pts)[i]->AsNumber(), pz = (*pts)[i + 1]->AsNumber();
					Loop.Add(ToUnreal(FVector(gw.X + px, gw.Y, gw.Z + pz)));   // world-space verts
				}
				A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
				if (A)
				{
					UProceduralMeshComponent* VM = MakeProcMesh(A, TEXT("Volume"));
					BuildWalls(VM, Loop, 500.f);   // 5m, matching the SDK gizmo
					BF6_ApplyVolumeMaterial(A, VM);
				}
			}
			else
			{
				A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
				if (A)
				{
					UProceduralMeshComponent* MM = MakeProcMesh(A, TEXT("Model"));
					// use the SDK's real low-poly model for this gameplay object; fall back to a marker
					if (!FillProcFromBf6Mesh(MM, ObjModelPath(ty))) BuildMarker(MM);
				}
			}
			if (!A) continue;
			// Setting a fresh proc-mesh as root resets the actor to origin, so place
			// point markers explicitly here (volumes carry world-space geometry).
			// Point objects also carry a real orientation (spawn facing) - apply it.
			if (!bVolume) A->SetActorTransform(FTransform(Rot, ToUnreal(gw), FVector::OneVector));
			A->SetActorLabel(nm);   // exact base name: saves match base objects by it
			A->Tags.Add(kBaseTag);
			A->Tags.Add(FName(*(FString(TEXT("type:")) + ty)));
			A->Tags.Add(FName(*(FString(TEXT("oid:")) + FString::FromInt(oid))));
			A->SetFlags(RF_Transient);

			FBaseObj bo; bo.Type = ty;   // field values are captured in the panel build
			BaseObjects.Add(oid, bo);
			oid++; spawned++;
		}
		UE_LOG(LogBF6, Display, TEXT("Base setup loaded for %s: %d objects."), *CurrentLevel, spawned);
	}

	FReply OnLoadAssets()
	{
		const FString P = MeshPath(TEXT("_assets.bf6mesh"));
		if (!GContextReused) if (FPaths::FileExists(P)) SpawnContextMesh(P, FString::Printf(TEXT("%s_Assets"), *CurrentLevel));
		else UE_LOG(LogBF6, Warning, TEXT("no low-poly assets extracted for %s yet"), *CurrentLevel);
		return FReply::Handled();
	}
	FReply OnReloadTerrain() { LoadTerrainContext(); return FReply::Handled(); }

	// Turn the read-only base into an editable custom map. Until this is done, no
	// objects can be placed - so the base is never built on by accident.
	FReply OnCreateCustom()
	{
		FString Name = SaveNameBox.IsValid() ? SaveNameBox->GetText().ToString().TrimStartAndEnd() : FString();
		FString Why;
		if (!BF6_ValidSaveName(Name, Why)) { Notify(Why); return FReply::Handled(); }
		// An existing name would be OVERWRITTEN, and creating happens from the
		// read-only base, so the world being written is empty - the old save
		// would not be merged into, it would be erased. Ask.
		if (FPaths::FileExists(BF6_SessionPathFor(CurrentLevel, Name)))
		{
			const FString Q = FString::Printf(TEXT("A custom map called '%s' already exists for this level.\n\nCreating it again REPLACES it with an empty one. The objects in the old save are lost.\n\nReplace it?"), *Name);
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Q)) != EAppReturnType::Yes)
			{
				Notify(TEXT("Left the existing custom map alone."));
				return FReply::Handled();
			}
		}
		const FString Was = CurrentSave; const bool bWas = bEditing;
		CurrentSave = Name;
		bEditing = true;
		if (!SaveSession(CurrentLevel, CurrentSave))   // create the save file now
		{
			CurrentSave = Was; bEditing = bWas;
			Notify(FString::Printf(TEXT("Could not create '%s' - the file could not be written. Try a simpler name."), *Name));
			return FReply::Handled();
		}
		Notify(FString::Printf(TEXT("Custom map '%s' created - you can now place objects."), *CurrentSave));
		return FReply::Handled();
	}

	FReply OnSaveClicked()
	{
		if (!bEditing || CurrentSave.IsEmpty()) { Notify(TEXT("Create a custom map first.")); return FReply::Handled(); }
		if (!SaveSession(CurrentLevel, CurrentSave))
		{
			Notify(FString::Printf(TEXT("Could not save '%s' - the file could not be written."), *CurrentSave));
			return FReply::Handled();
		}
		Notify(FString::Printf(TEXT("Saved '%s'."), *CurrentSave));
		return FReply::Handled();
	}

	// Export the map to <map>.spatial.json in the SDK's Portal format so the SDK's
	// experience_exporter can package it. Portal_Dynamic = base setup (reformatted
	// from .base.json, already Godot-space) + placed objects (Unreal->Godot);
	// Static = terrain/assets refs. See [[bf6-portal-spatial-export]].
	FReply OnExportSpatial()
	{
		if (!GEditor) return FReply::Handled();
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World) return FReply::Handled();
		auto Vec = [](double x, double y, double z){ TSharedPtr<FJsonObject> v = MakeShared<FJsonObject>(); v->SetNumberField(TEXT("x"),x); v->SetNumberField(TEXT("y"),y); v->SetNumberField(TEXT("z"),z); return v; };

		// Minifier (PortalSpatialMinifier): map each object name/id to a short
		// counter name (a,b,..,z,aa,..) + a name-map so links stay consistent + we
		// write condensed. Portal has an upload size cap; long names dominate size.
		TMap<FString, FString> ShortMap; int32 ShortCtr = 1;
		auto ShortName = [&](const FString& Orig) -> FString
		{
			if (Orig.IsEmpty()) return Orig;
			if (const FString* F = ShortMap.Find(Orig)) return *F;
			FString R; int32 N = ShortCtr++;
			while (N > 0) { N--; R = FString::Chr((TCHAR)('a' + (N % 26))) + R; N /= 26; }
			ShortMap.Add(Orig, R);
			return R;
		};

		TArray<TSharedPtr<FJsonValue>> Dynamic;

		// --- base setup (Godot-native: reformat .base.json, compose world pos) ---
		const FString BasePath = FPaths::Combine(BF6_DataDir(), TEXT("basesetup"), CurrentLevel + TEXT(".base.json"));
		FString In; TSharedPtr<FJsonObject> BaseRoot;
		if (FFileHelper::LoadFileToString(In, *BasePath))
		{
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
			FJsonSerializer::Deserialize(R, BaseRoot);
		}
		const TArray<TSharedPtr<FJsonValue>>* BObjs = nullptr;
		if (BaseRoot.IsValid() && BaseRoot->TryGetArrayField(TEXT("objects"), BObjs))
		{
			TMap<FString, FBF6GNode> GMap;
			BF6_BuildGNodeMap(*BObjs, GMap);
			for (const auto& v : *BObjs)
			{
				const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue;
				const FString nm=o->GetStringField(TEXT("name")), ty=o->GetStringField(TEXT("type"));
				if (ty.StartsWith(TEXT("MP_")) || BF6_IsEngineNodeType(ty)) continue;   // terrain/assets -> Static; pivots never emit
				// ACCUMULATED transform: parent pivots (the deploy camera's
				// aim) carry into the emitted basis, like Godot's exporter
				double WB[9]; FVector gw;
				BF6_GWorldOf(GMap, nm, WB, gw);
				TSharedPtr<FJsonObject> e = MakeShared<FJsonObject>();
				e->SetStringField(TEXT("name"), ShortName(nm));
				e->SetStringField(TEXT("type"), ty);
				// Portal right/up/front are the basis COLUMNS (Godot local
				// axes), not the row-major storage order.
				e->SetObjectField(TEXT("right"), Vec(WB[0], WB[3], WB[6]));
				e->SetObjectField(TEXT("up"),    Vec(WB[1], WB[4], WB[7]));
				e->SetObjectField(TEXT("front"), Vec(WB[2], WB[5], WB[8]));
				e->SetObjectField(TEXT("position"), Vec(gw.X, gw.Y, gw.Z));
				e->SetStringField(TEXT("id"), ShortName(nm));
				const TArray<TSharedPtr<FJsonValue>>* pts=nullptr;
				if (o->TryGetArrayField(TEXT("points"), pts)) e->SetArrayField(TEXT("points"), *pts);   // volume polygon passthrough
				Dynamic.Add(MakeShared<FJsonValueObject>(e));
			}
		}

		// --- placed objects (Unreal -> Godot: pos/100 with Y/Z swap, basis swap) ---
		auto Swap = [](const FVector& v){ return FVector(v.X, v.Z, v.Y); };
		int32 pid = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (!It->Tags.Contains(kPlacedTag)) continue;
			FString Type = TagValue(*It, TEXT("label:"));
			if (Type.IsEmpty()) Type = TagValue(*It, TEXT("mesh:"));
			if (Type.IsEmpty()) continue;
			const FTransform Xf = It->GetActorTransform();
			const FVector L = Xf.GetLocation();
			const FVector R = Swap(Xf.GetUnitAxis(EAxis::X)), U = Swap(Xf.GetUnitAxis(EAxis::Z)), F = Swap(Xf.GetUnitAxis(EAxis::Y));
			TSharedPtr<FJsonObject> e = MakeShared<FJsonObject>();
			const FString nm = ShortName(FString::Printf(TEXT("placed_%d"), pid));
			e->SetStringField(TEXT("name"), nm);
			e->SetStringField(TEXT("type"), Type);
			e->SetObjectField(TEXT("right"), Vec(R.X,R.Y,R.Z));
			e->SetObjectField(TEXT("up"),    Vec(U.X,U.Y,U.Z));
			e->SetObjectField(TEXT("front"), Vec(F.X,F.Y,F.Z));
			e->SetObjectField(TEXT("position"), Vec(L.X/100.0, L.Z/100.0, L.Y/100.0));
			e->SetStringField(TEXT("id"), nm);
			e->SetNumberField(TEXT("ObjId"), -1);
			Dynamic.Add(MakeShared<FJsonValueObject>(e));
			pid++;
		}

		// --- Static: terrain + assets refs ---
		TArray<TSharedPtr<FJsonValue>> Static;
		const TCHAR* Suffixes[2] = { TEXT("_Terrain"), TEXT("_Assets") };
		for (const TCHAR* Suffix : Suffixes)
		{
			const FString snm = CurrentLevel + Suffix;
			TSharedPtr<FJsonObject> e = MakeShared<FJsonObject>();
			e->SetStringField(TEXT("name"), snm);
			e->SetBoolField(TEXT("metadata/_edit_lock_"), true);
			e->SetStringField(TEXT("type"), snm);
			e->SetObjectField(TEXT("right"), Vec(1,0,0));
			e->SetObjectField(TEXT("up"), Vec(0,1,0));
			e->SetObjectField(TEXT("front"), Vec(0,0,1));
			e->SetObjectField(TEXT("position"), Vec(0,0,0));
			e->SetStringField(TEXT("id"), FString::Printf(TEXT("Static/%s"), *snm));
			Static.Add(MakeShared<FJsonValueObject>(e));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("Portal_Dynamic"), Dynamic);
		Root->SetArrayField(TEXT("Static"), Static);
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), W);
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("export"), CurrentLevel + TEXT(".spatial.json"));
		FFileHelper::SaveStringToFile(Out, *Path);
		Notify(FString::Printf(TEXT("Exported %d objects -> %s"), Dynamic.Num(), *Path));
		UE_LOG(LogBF6, Warning, TEXT("Exported spatial.json (%d dynamic, %d static): %s"), Dynamic.Num(), Static.Num(), *Path);
		return FReply::Handled();
	}

	// Import a .spatial.json (ours, an SDK sample like CustomBT, or any Portal export)
	// back into the tool as an editable custom map. The inverse of OnExportSpatial:
	// Godot -> Unreal for every Portal_Dynamic object. The file is authoritative, so
	// we load the map's terrain/asset context but NOT the shipped base setup (the
	// file already carries whatever base objects it wants); every object comes in as
	// an editable placement. See [[bf6-portal-spatial-export]].
	FReply OnImportSpatial()
	{
		if (!GEditor) return FReply::Handled();

		// pick a file
		IDesktopPlatform* DP = FDesktopPlatformModule::Get();
		if (!DP) { Notify(TEXT("File dialog unavailable.")); return FReply::Handled(); }
		TArray<FString> Picked;
		const FString DefaultDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("export"));
		const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(SharedThis(this));
		if (!DP->OpenFileDialog(Parent, TEXT("Import Portal .spatial.json"), DefaultDir, TEXT(""),
			TEXT("Portal spatial (*.spatial.json)|*.spatial.json|JSON (*.json)|*.json"), EFileDialogFlags::None, Picked)
			|| Picked.Num() == 0)
			return FReply::Handled();
		const FString File = Picked[0];

		FString In;
		if (!FFileHelper::LoadFileToString(In, *File)) { Notify(TEXT("Could not read the file.")); return FReply::Handled(); }
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
		if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) { Notify(TEXT("Not valid JSON.")); return FReply::Handled(); }

		// Level from the Static terrain/asset refs (name like "MP_Dumbo_Terrain").
		FString Level;
		const TArray<TSharedPtr<FJsonValue>>* StaticArr = nullptr;
		if (Root->TryGetArrayField(TEXT("Static"), StaticArr))
		{
			for (const auto& v : *StaticArr)
			{
				const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
				FString nm; o->TryGetStringField(TEXT("type"), nm);
				if (nm.IsEmpty()) o->TryGetStringField(TEXT("name"), nm);
				int32 cut;
				if (nm.FindLastChar('_', cut)) { Level = nm.Left(cut); break; }
			}
		}
		if (Level.IsEmpty()) Level = CurrentLevel;   // fall back to whatever is open
		if (Level.IsEmpty()) { Notify(TEXT("Could not tell which map this file is for (no Static terrain ref).")); return FReply::Handled(); }

		const TArray<TSharedPtr<FJsonValue>>* Dyn = nullptr;
		if (!Root->TryGetArrayField(TEXT("Portal_Dynamic"), Dyn)) { Notify(TEXT("No Portal_Dynamic list in file.")); return FReply::Handled(); }

		// set up an editable custom map named after the file
		CurrentLevel = Level;
		CurrentSave  = FPaths::GetBaseFilename(File).Replace(TEXT(".spatial"), TEXT(""));
		bEditing = true;
		if (Switcher.IsValid()) Switcher->SetActiveWidgetIndex(1);
		BF6_ClearContextFor(CurrentLevel);
		ClearActorsWithTag(kPlacedTag);
		ClearActorsWithTag(kBaseTag);
		ClearActorsWithTag(kGroupTag);   // the nodes go with them, or they double up
		LoadLevel(); ApplyFilter(); LoadBudgetMax();
		if (ListView.IsValid()) ListView->RequestListRefresh();
		LoadTerrainContext();
		{
			const FString AP = MeshPath(TEXT("_assets.bf6mesh"));
			if (!GContextReused) if (FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *CurrentLevel));
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World) return FReply::Handled();
		auto ToUnreal = [](double gx, double gy, double gz){ return FVector((float)gx, (float)gz, (float)gy) * 100.f; };  // Godot -> Unreal
		auto Swap = [](const FVector& v){ return FVector(v.X, v.Z, v.Y); };  // its own inverse
		auto ReadVec = [](const TSharedPtr<FJsonObject>& o, const TCHAR* key, const FVector& def)
		{
			const TSharedPtr<FJsonObject>* v = nullptr; if (!o->TryGetObjectField(key, v) || !v->IsValid()) return def;
			return FVector((*v)->GetNumberField(TEXT("x")), (*v)->GetNumberField(TEXT("y")), (*v)->GetNumberField(TEXT("z")));
		};

		int32 spawned = 0, volumes = 0, markers = 0;
		for (const auto& dv : *Dyn)
		{
			const TSharedPtr<FJsonObject> o = dv->AsObject(); if (!o.IsValid()) continue;
			FString Type; o->TryGetStringField(TEXT("type"), Type);
			if (Type.IsEmpty() || Type.StartsWith(TEXT("MP_"))) continue;   // skip terrain/assets
			const FVector gpos = ReadVec(o, TEXT("position"), FVector::ZeroVector);

			// Volume (polygon) objects carry a flat x,z points array.
			const TArray<TSharedPtr<FJsonValue>>* pts = nullptr;
			if (o->TryGetArrayField(TEXT("points"), pts) && pts->Num() >= 6)
			{
				TArray<FVector> Loop;
				for (int32 i = 0; i + 1 < pts->Num(); i += 2)
					Loop.Add(ToUnreal(gpos.X + (*pts)[i]->AsNumber(), gpos.Y, gpos.Z + (*pts)[i + 1]->AsNumber()));
				AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
				if (!A) continue;
				UProceduralMeshComponent* VM = MakeProcMesh(A, TEXT("Volume"));
				BuildWalls(VM, Loop, 500.f);
				BF6_ApplyVolumeMaterial(A, VM);
				BF6_SetPrettyLabel(A, Type);
				A->Tags.Add(kPlacedTag);
				A->Tags.Add(FName(*(FString(TEXT("label:")) + Type)));
				A->SetFlags(RF_Transient);
				spawned++; volumes++;
				continue;
			}

			// Point object: rebuild the actor rotation from the Godot basis.
			const FVector Rg = ReadVec(o, TEXT("right"), FVector(1,0,0));
			const FVector Ug = ReadVec(o, TEXT("up"),    FVector(0,1,0));
			const FVector Fg = ReadVec(o, TEXT("front"), FVector(0,0,1));
			const FVector Ax = Swap(Rg), Ay = Swap(Fg), Az = Swap(Ug);   // inverse of export's basis swap
			const FRotator Rot = FMatrix(Ax, Ay, Az, FVector::ZeroVector).Rotator();
			const FTransform Xf(Rot, ToUnreal(gpos.X, gpos.Y, gpos.Z), FVector::OneVector);

			const FString Mesh = ResolveMesh(Type);
			AActor* A = Mesh.IsEmpty() ? nullptr : SpawnSdkModel(Mesh, Type, Xf);
			if (!A)
			{
				// no bundled model: drop a labelled marker so it still round-trips
				A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
				if (!A) continue;
				UProceduralMeshComponent* MM = MakeProcMesh(A, TEXT("Model"));
				BuildMarker(MM);
				A->SetActorTransform(Xf);
				BF6_SetPrettyLabel(A, Type);
				A->Tags.Add(kPlacedTag);
				A->Tags.Add(FName(*(FString(TEXT("label:")) + Type)));
				A->SetFlags(RF_Transient);
				markers++;
			}
			spawned++;
		}

		const int32 Hooked = BF6_ApplyTreeMetadata(StaticArr);
		if (Hooked > 0) UE_LOG(LogBF6, Display, TEXT("authored tree: %d object(s) attached to their parent"), Hooked);
		Notify(FString::Printf(TEXT("Imported %d objects into %s (%d volumes, %d markers). Editable now."), spawned, *CurrentLevel, volumes, markers));
		UE_LOG(LogBF6, Warning, TEXT("Imported spatial.json: %d objects (%d volumes, %d markers) from %s"), spawned, volumes, markers, *File);
		return FReply::Handled();
	}

	// ---------- list ----------
	void OnSearch(const FText& Text) { Query = Text.ToString(); ApplyFilter(); if (ListView.IsValid()) ListView->RequestListRefresh(); }

	void LoadLevel()
	{
		AllItems.Reset();
		TypeCost.Reset();
		TypeToMesh.Reset();
		if (!g_ctx || !g_listp) return;
		const int32 kMax = 4000;
		TArray<bf6_placeable> Buf; Buf.SetNum(kMax);
		const int total = g_listp(g_ctx, CurrentLevel.IsEmpty() ? "" : TCHAR_TO_UTF8(*CurrentLevel), "", Buf.GetData(), kMax);
		const int n = FMath::Min(total, kMax);
		AllItems.Reserve(n);
		for (int i = 0; i < n; i++)
		{
			TSharedPtr<FPlaceableRow> r = MakeShared<FPlaceableRow>();
			r->Type = UTF8_TO_TCHAR(Buf[i].type); r->Directory = UTF8_TO_TCHAR(Buf[i].directory);
			r->Mesh = UTF8_TO_TCHAR(Buf[i].mesh); r->PhysicsCost = Buf[i].physics_cost; r->Universal = Buf[i].universal != 0;
			AllItems.Add(r);
			// Budget lookup: physicsCost is per placeable TYPE (matches the SDK's
			// memory dock). Key by type, and by mesh too so placed objects that only
			// carry a mesh tag still resolve.
			TypeCost.Add(r->Type, r->PhysicsCost);
			if (!r->Mesh.IsEmpty()) TypeCost.Add(r->Mesh, r->PhysicsCost);
			// Import needs type -> model name to spawn objects a spatial.json only
			// names by type.
			if (!r->Mesh.IsEmpty()) TypeToMesh.Add(r->Type, r->Mesh);
		}
	}

	// Resolve a placeable type to the low-poly model file to spawn. Many types share
	// their model name; otherwise the 'mesh' constant (captured in LoadLevel) names it.
	FString ResolveMesh(const FString& Type) const
	{
		if (FPaths::FileExists(ObjModelPath(Type))) return Type;
		if (const FString* M = TypeToMesh.Find(Type)) return *M;
		return FString();
	}

	// The level's physics-cost ceiling, straight from level_info.json (budget.
	// physicsCostMax; -1 = unlimited). Same source the SDK's memory dock reads.
	void LoadBudgetMax()
	{
		BudgetMax = -1;
		const FString Path = FPaths::Combine(BF6_DataDir(), TEXT("FbExportData/level_info.json"));
		FString In;
		if (!FFileHelper::LoadFileToString(In, *Path)) return;
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
		if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
		const TSharedPtr<FJsonObject>* Lv = nullptr;
		if (!Root->TryGetObjectField(CurrentLevel, Lv) || !Lv->IsValid()) return;
		const TSharedPtr<FJsonObject>* Bud = nullptr;
		if (!(*Lv)->TryGetObjectField(TEXT("budget"), Bud) || !Bud->IsValid()) return;
		double Max = -1;
		if ((*Bud)->TryGetNumberField(TEXT("physicsCostMax"), Max)) BudgetMax = (int32)Max;
	}

	// Sum the physics cost of everything shippable in the scene (base setup + user
	// placements) - exactly what the SDK's memory dock counts, and what the level's
	// physicsCostMax gates. Independent of the minifier (which only shrinks JSON
	// strings, not physics cost).
	void RecomputeBudget()
	{
		TotalCost = 0; TotalObjects = 0;
		if (GEditor)
		{
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					const bool bPlaced = It->Tags.Contains(kPlacedTag);
					const bool bBase   = It->Tags.Contains(kBaseTag);
					if (!bPlaced && !bBase) continue;
					FString Ty = TagValue(*It, TEXT("label:"));
					if (Ty.IsEmpty()) Ty = TagValue(*It, TEXT("type:"));
					if (Ty.IsEmpty()) Ty = TagValue(*It, TEXT("mesh:"));
					TotalObjects++;
					if (const int32* C = TypeCost.Find(Ty)) TotalCost += *C;
				}
			}
		}
		const bool bCapped = BudgetMax > 0;
		CachedBudgetFrac = bCapped ? FMath::Clamp((float)TotalCost / (float)BudgetMax, 0.f, 1.f) : 0.f;
		if (bCapped)
		{
			// green -> amber -> red as it fills; red when over.
			if (TotalCost > BudgetMax)        CachedBudgetColor = FLinearColor(0.95f,0.25f,0.25f);
			else if (CachedBudgetFrac > 0.85f) CachedBudgetColor = FLinearColor(0.95f,0.55f,0.20f);
			else                               CachedBudgetColor = FLinearColor(0.45f,0.80f,0.45f);
			CachedBudgetText = FText::FromString(FString::Printf(
				TEXT("Physics budget  %s / %s   (%d%%)   %d objects"),
				*FText::AsNumber(TotalCost).ToString(), *FText::AsNumber(BudgetMax).ToString(),
				FMath::RoundToInt(CachedBudgetFrac * 100.f), TotalObjects));
		}
		else
		{
			CachedBudgetColor = FLinearColor(0.55f,0.6f,0.65f);
			CachedBudgetText = FText::FromString(FString::Printf(
				TEXT("Physics cost  %s   (no cap on this map)   %d objects"),
				*FText::AsNumber(TotalCost).ToString(), TotalObjects));
		}
	}

	void ApplyFilter()
	{
		Items.Reset();
		const FString P = Query.ToLower();
		if (P.IsEmpty()) { Items = AllItems; return; }
		TArray<TPair<int32, TSharedPtr<FPlaceableRow>>> Scored;
		for (const auto& r : AllItems)
		{
			int32 s = 0;
			if (FuzzyScore(P, r->Type, s)) Scored.Emplace(s + 20, r);
			else { int32 sd = 0; if (FuzzyScore(P, r->Directory, sd)) Scored.Emplace(sd, r); }
		}
		Scored.Sort([](const TPair<int32, TSharedPtr<FPlaceableRow>>& A, const TPair<int32, TSharedPtr<FPlaceableRow>>& B){ return A.Key > B.Key; });
		Items.Reserve(Scored.Num());
		for (const auto& pr : Scored) Items.Add(pr.Value);
	}

	FReply StartRowDrag(TSharedPtr<FPlaceableRow> Item)
	{
		if (!Item.IsValid()) return FReply::Unhandled();
		if (!bEditing) { BF6Api::RefuseReadOnly(FString()); return FReply::Unhandled(); }
		const FString MeshName = Item->Mesh.IsEmpty() ? Item->Type : Item->Mesh;
		if (!FPaths::FileExists(ObjModelPath(MeshName))) return FReply::Unhandled();
		return FReply::Handled().BeginDragDrop(FBF6PlaceableDragDropOp::New(MeshName, Item->Type));
	}

	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FPlaceableRow> Item, const TSharedRef<STableViewBase>& Owner)
	{
		return SNew(STableRow<TSharedPtr<FPlaceableRow>>, Owner)
			.OnDragDetected_Lambda([this, Item](const FGeometry&, const FPointerEvent&){ return StartRowDrag(Item); })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.55f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(Item->Type)) ]
				+ SHorizontalBox::Slot().FillWidth(0.45f).VAlign(VAlign_Center)[ SNew(STextBlock).ColorAndOpacity(FSlateColor(FLinearColor(0.6f,0.68f,0.74f))).Text(FText::FromString(Item->Directory)) ]
			];
	}

	void OnSelectionChanged(TSharedPtr<FPlaceableRow> Item, ESelectInfo::Type)
	{
		if (!Preview.IsValid() || !Item.IsValid()) return;
		Preview->ShowModel(Item->Mesh.IsEmpty() ? Item->Type : Item->Mesh);
	}

	void OnActivate(TSharedPtr<FPlaceableRow> Item)
	{
		if (!Item.IsValid()) return;
		if (!bEditing) { BF6Api::RefuseReadOnly(FString()); return; }
		const FString MeshName = Item->Mesh.IsEmpty() ? Item->Type : Item->Mesh;
		AActor* A = SpawnSdkModel(MeshName, Item->Type, FTransform(FVector(0,0,100)));
		if (!A) { Notify(FString::Printf(TEXT("No SDK model bundled for '%s'."), *Item->Type)); return; }
		if (GEditor) { GEditor->SelectNone(false, true, false); GEditor->SelectActor(A, true, true); }
	}

	// Recompute the running budget a few times a second so it climbs live as the
	// user places (and deletes) objects, including editor-driven deletes we don't
	// hook directly.
	virtual void Tick(const FGeometry& Geo, const double InCurrentTime, const float InDeltaTime) override
	{
		SCompoundWidget::Tick(Geo, InCurrentTime, InDeltaTime);
		if (InCurrentTime - LastBudgetCalc > 0.25)
		{
			LastBudgetCalc = InCurrentTime;
			RecomputeBudget();
		}
	}

	// state
	FString CurrentLevel, Query;
	FString CurrentSave;        // the custom map being edited ("" = read-only base)
	bool    bEditing = false;   // false until a custom map is created/opened
	// budget (physics cost) tracking
	TMap<FString, int32> TypeCost;   // placeable type/mesh -> physicsCost
	TMap<FString, FString> TypeToMesh;  // placeable type -> model name (for import)
	int32   BudgetMax = -1;          // level physicsCostMax (-1 = unlimited)
	int32   TotalCost = 0, TotalObjects = 0;
	FText   CachedBudgetText;
	FLinearColor CachedBudgetColor = FLinearColor(0.55f,0.6f,0.65f);
	float   CachedBudgetFrac = 0.f;
	double  LastBudgetCalc = 0.0;
	TMap<int32, FBaseObj> BaseObjects;   // oid -> base-setup object (type + field values)
	TArray<TSharedPtr<FPlaceableRow>> AllItems, Items;
	TSharedPtr<SWidgetSwitcher> Switcher;
	TSharedPtr<SWrapBox> MapGrid;
	TSharedPtr<SListView<TSharedPtr<FPlaceableRow>>> ListView;
	TSharedPtr<SBF6PreviewViewport> Preview;
	TSharedPtr<SEditableTextBox> SaveNameBox;
	TArray<TStrongObjectPtr<UTexture2D>> CardTextures;
	TArray<TSharedPtr<FSlateBrush>> CardBrushes;
	TArray<TSharedPtr<TArray<TSharedPtr<FString>>>> CardSaveLists;
};

// ============================================================================
// Build Mode session logic. Implements the BF6Api data/actions on a shared
// session state (g_ss). Runs parallel to SBF6Browser, which keeps its own copy
// for the docked tab; cleanup will dedupe the two.
// ============================================================================
struct FBF6SessionState
{
	FString CurrentLevel, CurrentSave;
	bool bEditing = false;
	TArray<TSharedPtr<FPlaceableRow>> AllItems;
	TMap<FString, int32>   TypeCost;
	TMap<FString, FString> TypeToMesh;
	int32 BudgetMax = -1, TotalCost = 0, TotalObjects = 0;
	float BudgetFrac = 0.f;
	FLinearColor BudgetColor = FLinearColor(0.55f, 0.6f, 0.65f);
	FText BudgetText;
	TMap<int32, FBaseObj> BaseObjects;
};
static FBF6SessionState g_ss;

static FString BF6_MapMeshPath(const FString& Level, const FString& Suffix)
{ return FPaths::Combine(BF6_DataDir(), TEXT("mapmesh"), Level + Suffix); }
static FString BF6_BaseJsonPath(const FString& Level)
{ return FPaths::Combine(BF6_DataDir(), TEXT("basesetup"), Level + TEXT(".base.json")); }

static FString BF6_ResolveMeshForType(const FString& Type)
{
	if (FPaths::FileExists(ObjModelPath(Type))) return Type;
	if (const FString* M = g_ss.TypeToMesh.Find(Type)) return *M;
	return FString();
}

// ---- live drag ghost ----
// The real model follows the cursor during a library drag, so you see what
// you're dropping before you let go. Untagged, unselectable, and transient:
// it can never be picked, boxed, saved, exported, or counted by the budget.
static TWeakObjectPtr<AActor> GDragGhost;
static FString GDragGhostType;
static FString GDragGhostFailed;   // types with no model: don't re-try per move

void BF6Api::DestroyDragGhost()
{
	if (AActor* A = GDragGhost.Get())
		if (UWorld* W = A->GetWorld()) W->EditorDestroyActor(A, false);
	GDragGhost.Reset();
	GDragGhostType.Reset();
	GDragGhostFailed.Reset();
}

void BF6Api::UpdateDragGhost(const FString& Type, const FVector& W)
{
	// no ghost on a read-only base: it would promise a placement the drop
	// then refuses (the drop catcher explains instead)
	if (!g_ss.bEditing) return;
	if (!GEditor || Type.IsEmpty() || Type == GDragGhostFailed) return;
	if (Type.StartsWith(TEXT("block::"))) return;   // blocks drop without a ghost
	if (GDragGhost.IsValid() && GDragGhostType != Type) DestroyDragGhost();
	if (!GDragGhost.IsValid())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World) return;
		const FString Mesh = BF6_ResolveMeshForType(Type);
		const FString Path = Mesh.IsEmpty() ? FString() : ObjModelPath(Mesh);
		if (Path.IsEmpty() || !FPaths::FileExists(Path)) { GDragGhostFailed = Type; return; }
		AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(W));
		if (!A) { GDragGhostFailed = Type; return; }
		UProceduralMeshComponent* M = MakeProcMesh(A, TEXT("DragGhost"));
		if (!FillProcFromBf6Mesh(M, Path))
		{
			World->EditorDestroyActor(A, false);
			GDragGhostFailed = Type;
			return;
		}
		ApplyObjectWhite(M);
		M->SetVisibility(true, true);
		M->bSelectable = false;
		A->SetActorLabel(TEXT("DragPreview"));
		A->SetFlags(RF_Transient);
		GDragGhost = A;
		GDragGhostType = Type;
	}
	if (AActor* A = GDragGhost.Get()) A->SetActorLocation(W);
	// a Slate drag starves the viewport of mouse events, and a non-realtime
	// viewport then stops REPAINTING - the ghost moved but the picture froze
	// mid-drag. Invalidate per update so it visibly rides the cursor.
	if (GCurrentLevelEditingViewportClient) GCurrentLevelEditingViewportClient->Invalidate();
}

// Top-level directory segment = the radial category ("Props/Vehicles" -> "Props").
static FString BF6_TopSeg(const FString& Dir)
{
	if (Dir.IsEmpty()) return TEXT("Uncategorized");
	FString L, Rr;
	if (Dir.Split(TEXT("/"), &L, &Rr) || Dir.Split(TEXT("\\"), &L, &Rr)) return L;
	return Dir;
}

// ---- user category overrides (library "move to category") ----
// Type -> category chosen by the user; everything (pie, popups, the library)
// reads categories through BF6_EffectiveCategory so a move shows up everywhere.
static TMap<FString, FString> GCatOverrides;

static FString BF6_CategoriesPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("categories.json"));
}

static void BF6_LoadCatOverrides()
{
	GCatOverrides.Reset();
	FString In; if (!FFileHelper::LoadFileToString(In, *BF6_CategoriesPath())) return;
	TSharedPtr<FJsonObject> Root; TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
	for (const auto& P : Root->Values)
	{
		FString V; if (P.Value->TryGetString(V) && !V.IsEmpty()) GCatOverrides.Add(FString(P.Key), V);
	}
}

static void BF6_SaveCatOverrides()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	for (const auto& P : GCatOverrides) Root->SetStringField(P.Key, P.Value);
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	FFileHelper::SaveStringToFile(Out, *BF6_CategoriesPath());
}

// Which shelf an object belongs on.
//
// Taking the TOP folder segment - what this used to do - buries the library:
// 5,325 of the SDK's 11,142 types sit under "Generic" and another 1,339 under
// "Uncategorized", so trees (Generic/Common/Nature) showed up as "Generic" and
// cars (Generic/Common/Props) as "Props" or nothing at all. Creators went
// looking for a tree by name and never found one.
//
// So: take the most MEANINGFUL folder anywhere in the path, ranked so a weak
// label like Props cannot beat Nature or Military; then read the object's own
// name, which is where DICE actually says what a thing is (Acacia, CarSedan,
// WreckTank); and only then fall back. Measured over the whole catalogue this
// empties Generic entirely and gives Vehicles 615 types and Nature 1,022 where
// both had none and 345.
static FString BF6_EffectiveCategory(const FPlaceableRow& r)
{
	if (const FString* O = GCatOverrides.Find(r.Type)) return *O;

	// folder labels, strongest first
	static const TCHAR* kFolders[][2] = {
		{ TEXT("nature"), TEXT("Nature") },     { TEXT("vehicles"), TEXT("Vehicles") },
		{ TEXT("military"), TEXT("Military") }, { TEXT("industrial"), TEXT("Industrial") },
		{ TEXT("roads"), TEXT("Roads") },       { TEXT("lightfixtures"), TEXT("Lighting") },
		{ TEXT("architecture"), TEXT("Architecture") }, { TEXT("backdrop"), TEXT("Backdrop") },
		{ TEXT("audio"), TEXT("Audio") },       { TEXT("fx"), TEXT("FX") },
		{ TEXT("gameplay"), TEXT("Gameplay") } };

	TArray<FString> Segs;
	r.Directory.ParseIntoArray(Segs, TEXT("/"));
	for (const TCHAR** Pair : kFolders)
		for (const FString& S : Segs)
			if (S.Equals(Pair[0], ESearchCase::IgnoreCase)) return Pair[1];

	// then what the object calls itself
	const FString T = r.Type.ToLower();
	auto Has = [&T](std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* W : Words) if (T.Contains(W)) return true;
		return false;
	};
	if (Has({ TEXT("car"), TEXT("sedan"), TEXT("suv"), TEXT("truck"), TEXT("van"), TEXT("bus"),
		TEXT("taxi"), TEXT("police"), TEXT("ambulance"), TEXT("forklift"), TEXT("tractor"),
		TEXT("trailer"), TEXT("motorcycle"), TEXT("scooter"), TEXT("boat"), TEXT("ship"),
		TEXT("heli"), TEXT("airplane"), TEXT("jet"), TEXT("tank"), TEXT("apc"), TEXT("humvee"),
		TEXT("jeep"), TEXT("pickup") })) return TEXT("Vehicles");
	if (Has({ TEXT("tree"), TEXT("acacia"), TEXT("birch"), TEXT("pine"), TEXT("palm"), TEXT("oak"),
		TEXT("maple"), TEXT("willow"), TEXT("cypress"), TEXT("poplar"), TEXT("eucalyptus"),
		TEXT("juniper"), TEXT("cedar"), TEXT("spruce"), TEXT("bush"), TEXT("shrub"), TEXT("grass"),
		TEXT("fern"), TEXT("rock"), TEXT("boulder"), TEXT("cliff"), TEXT("stump"), TEXT("hedge"),
		TEXT("flower"), TEXT("plant"), TEXT("vine"), TEXT("ivy"), TEXT("cactus"), TEXT("reed"),
		TEXT("branch"), TEXT("moss"), TEXT("mushroom"), TEXT("coral") })) return TEXT("Nature");
	if (Has({ TEXT("sign"), TEXT("billboard"), TEXT("banner"), TEXT("poster"), TEXT("ads") })) return TEXT("Signs");
	if (Has({ TEXT("fence"), TEXT("railing"), TEXT("barrier"), TEXT("barbedwire"), TEXT("bollard"),
		TEXT("gate"), TEXT("wall") })) return TEXT("Barriers");
	if (Has({ TEXT("door"), TEXT("window"), TEXT("shutter"), TEXT("hatch") })) return TEXT("Doors & Windows");
	if (Has({ TEXT("debris"), TEXT("rubble"), TEXT("wreck"), TEXT("broken"), TEXT("destroyed"),
		TEXT("ruin") })) return TEXT("Debris");
	if (Has({ TEXT("pipe"), TEXT("duct"), TEXT("cabletray"), TEXT("vent") })) return TEXT("Pipes & Ducts");
	if (Has({ TEXT("container"), TEXT("crate"), TEXT("barrel"), TEXT("dumpster"), TEXT("pallet"),
		TEXT("sack") })) return TEXT("Containers");
	if (Has({ TEXT("chair"), TEXT("table"), TEXT("desk"), TEXT("sofa"), TEXT("couch"), TEXT("bed"),
		TEXT("shelf"), TEXT("cabinet"), TEXT("stall"), TEXT("market") })) return TEXT("Furniture");

	for (const FString& S : Segs)
		if (S.Equals(TEXT("props"), ESearchCase::IgnoreCase)) return TEXT("Props");
	const FString Top = BF6_TopSeg(r.Directory);
	return (Top.IsEmpty() || Top.Equals(TEXT("Generic"), ESearchCase::IgnoreCase)) ? TEXT("Uncategorized") : Top;
}

// ---- auto-organized outliner ----
// New Unreal users get a self-sorting level tree: every object files itself
// into a readable folder by what it IS, so the outliner never becomes a flat
// dump of BF6_Bucket_01, _2, _3. Folders are cosmetic (Unreal outliner only)
// and never affect links or export - safe to reshuffle any time.
static FString BF6_CategoryForType(const FString& Type)
{
	// AllItems holds this level's placeables, which is everything placeable
	// here - enough to categorize any object actually in the level
	for (const TSharedPtr<FPlaceableRow>& r : g_ss.AllItems)
		if (r.IsValid() && r->Type == Type) return BF6_EffectiveCategory(*r);
	return FString();
}

// Creators coming from Godot build deep, deliberate trees (Sidewalk, Lights,
// Low Detail Buildings, props parented under other props) and want them back
// exactly as authored, so an import records the whole path on each actor and
// this preference decides which view wins. It is asked once at import and
// then sticks; the outliner button flips it at any time.
bool BF6_KeepGodotTree()
{
	bool bKeep = true;   // an authored tree wins by default when one exists
	GConfig->GetBool(TEXT("BF6UnrealSDK"), TEXT("KeepGodotTree"), bKeep, GEditorPerProjectIni);
	return bKeep;
}

void BF6_SetKeepGodotTree(bool bKeep)
{
	GConfig->SetBool(TEXT("BF6UnrealSDK"), TEXT("KeepGodotTree"), bKeep, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

// The outliner folder an object belongs in. Gameplay gets role folders;
// everything else groups under BF6 Build by its library category.
static FString BF6_FolderForActor(AActor* A)
{
	if (!A) return TEXT("BF6 Build");
	// the authored Godot path, verbatim, when the creator asked to keep it
	if (BF6_KeepGodotTree())
	{
		const FString G = TagValue(A, TEXT("gtree:"));
		if (!G.IsEmpty()) return G;
	}
	// a block instance lives with its siblings, one folder per block
	const FString Blk = TagValue(A, TEXT("blk:"));
	if (!Blk.IsEmpty()) return TEXT("BF6 Blocks/") + Blk;

	FString Ty = TagValue(A, TEXT("label:"));
	if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
	const bool bBase = A->Tags.Contains(kBaseTag);

	auto GameplaySub = [](const FString& T) -> FString
	{
		if (T == TEXT("HQ_PlayerSpawner") || T == TEXT("PlayerSpawner")) return TEXT("HQs");
		if (T == TEXT("SpawnPoint")) return TEXT("Spawn Points");
		if (T == TEXT("CapturePoint") || T == TEXT("MCOM") || T == TEXT("Sector")) return TEXT("Flags & Objectives");
		if (T == TEXT("VehicleSpawner") || T.StartsWith(TEXT("VEH_")) || T.StartsWith(TEXT("Stationary"))) return TEXT("Vehicles");
		if (T == TEXT("DeployCam") || T == TEXT("FixedCamera") || T.Contains(TEXT("Camera"))) return TEXT("Cameras");
		if (T == TEXT("CombatArea") || T == TEXT("PolygonVolume") || T == TEXT("OBBVolume") || T == TEXT("AreaTrigger") || T == TEXT("RingOfFire")) return TEXT("Zones");
		if (T.StartsWith(TEXT("AI_")) || T == TEXT("WaypointPath")) return TEXT("AI");
		if (T == TEXT("WorldIcon") || T == TEXT("InteractPoint")) return TEXT("Markers");
		return FString();   // not a recognized gameplay type
	};

	const FString Sub = GameplaySub(Ty);
	if (!Sub.IsEmpty())
		return (bBase ? TEXT("BF6 Base Setup/") : TEXT("BF6 Gameplay/")) + Sub;

	// a plain prop: group by its library category
	const FString Cat = BF6_CategoryForType(Ty);
	if (bBase) return TEXT("BF6 Base Setup/Props");
	return Cat.IsEmpty() ? TEXT("BF6 Build/Props") : (TEXT("BF6 Build/") + Cat);
}

// ---- the authored tree, as real attachment ----
// Every imported node carries its own Godot key (gpath:) and its parent's
// (gtree:). One pass over those tags rebuilds the whole tree, whatever the
// import came from - a .tscn, a base setup, or a session save reopened.
static AActor* BF6_SpawnTreeNode(UWorld* W, const FString& Key, const FString& ParentKey,
	const FTransform& Xf, int32 Order)
{
	if (!W) return nullptr;
	AActor* A = W->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
	if (!A) return nullptr;
	// A node holds no geometry, so without a marker it is invisible in the
	// viewport and can only be found in the tree. Same small cube the volume
	// points use, drawn through walls, in cyan so it reads as structure rather
	// than something placeable.
	UProceduralMeshComponent* Root = NewObject<UProceduralMeshComponent>(A, TEXT("Node"));
	A->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetFlags(RF_Transactional);   // the gizmo needs this to undo a move
	BuildHandleCube(Root);
	Root->SetDepthPriorityGroup(SDPG_Foreground);
	Root->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (UMaterialInterface* Base = BF6_Material(TEXT("M_NeonHighlight")))
		if (UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, Root))
		{
			Mid->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.f, 0.85f, 1.f));
			Mid->SetVectorParameterValue(TEXT("Tint"),  FLinearColor(0.f, 0.85f, 1.f));
			Root->SetMaterial(0, Mid);
		}
	A->SetActorTransform(Xf);
	A->Tags.Add(kGroupTag);
	A->Tags.Add(FName(*(FString(TEXT("gpath:")) + Key)));
	if (!ParentKey.IsEmpty() && ParentKey != TEXT("."))
		A->Tags.Add(FName(*(FString(TEXT("gtree:")) + ParentKey)));
	if (Order != MAX_int32) A->Tags.Add(FName(*FString::Printf(TEXT("gord:%d"), Order)));
	A->SetFlags(RF_Transient);
	return A;
}

// Everything hanging off this actor, the actor itself first.
static void BF6_CollectSubtree(AActor* Root, TArray<AActor*>& Out)
{
	if (!Root || Out.Contains(Root)) return;
	Out.Add(Root);
	TArray<AActor*> Kids;
	Root->GetAttachedActors(Kids);
	for (AActor* K : Kids) BF6_CollectSubtree(K, Out);
}

// Hook every child onto its parent. World transforms are already correct
// (the importers accumulate the Godot parent chain themselves), so the
// attachment keeps them and only takes over from here on.
static int32 BF6_RebuildTreeFromTags()
{
	ON_SCOPE_EXIT{ BF6Api::RefreshSceneTree(); };   // rows for a batch just re-parented
	if (!GEditor) return 0;
	UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
	TMap<FString, AActor*> ByKey;
	TArray<AActor*> Kids;
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		const FString K = TagValue(*It, TEXT("gpath:"));
		if (!K.IsEmpty()) ByKey.Add(K, *It);
		if (!TagValue(*It, TEXT("gtree:")).IsEmpty()) Kids.Add(*It);
	}
	// A parent with no actor of its own is Godot's empty node, and it has to
	// BECOME one - otherwise the child has nothing to hang off and falls back
	// to a folder, which is what filled the outliner with folders where the SDK
	// shows nodes. Path-style keys carry their own ancestry, so the whole chain
	// gets built from the key itself.
	TFunction<AActor*(const FString&, int32)> Ensure = [&](const FString& Key, int32 Depth) -> AActor*
	{
		if (Key.IsEmpty() || Key == TEXT(".")) return nullptr;
		if (AActor** Found = ByKey.Find(Key)) return *Found;
		if (Depth > 32) return nullptr;   // a malformed key cannot spin forever
		FString ParentKey, Leaf = Key;
		int32 Slash;
		if (Key.FindLastChar('/', Slash)) { ParentKey = Key.Left(Slash); Leaf = Key.RightChop(Slash + 1); }
		AActor* Made = BF6_SpawnTreeNode(W, Key, ParentKey, FTransform::Identity);
		if (!Made) return nullptr;
		BF6_SetPrettyLabel(Made, Leaf);
		ByKey.Add(Key, Made);
		if (AActor* Up = Ensure(ParentKey, Depth + 1))
			Made->AttachToActor(Up, FAttachmentTransformRules::KeepWorldTransform);
		return Made;
	};

	// folders left by earlier sessions: the tree replaces them
	if (BF6_KeepGodotTree())
	{
		for (TActorIterator<AActor> It(W); It; ++It)
			if (!It->GetFolderPath().IsNone()) It->SetFolderPath(NAME_None);
		TArray<FFolder> Empties;
		FActorFolders::Get().ForEachFolder(*W, [&Empties](const FFolder& F){ Empties.Add(F); return true; });
		for (const FFolder& F : Empties) FActorFolders::Get().DeleteFolder(*W, F);
	}

	int32 n = 0;
	for (int32 i = 0; i < Kids.Num(); i++)
	{
		AActor* A = Kids[i];
		if (!IsValid(A)) continue;
		// Only adopt orphans. An actor that already has a parent has been placed
		// there - by the import, or by the creator dragging it in the tree - and
		// re-attaching it from the imported tags would drag it back every load.
		if (A->GetAttachParentActor()) continue;
		AActor* P = Ensure(TagValue(A, TEXT("gtree:")), 0);
		if (!P || P == A) continue;
		// a cycle would take the attach call down with it
		bool bLoop = false;
		for (AActor* Up = P; Up; Up = Up->GetAttachParentActor())
			if (Up == A) { bLoop = true; break; }
		if (bLoop) continue;
		if (A->GetAttachParentActor() == P) continue;
		A->AttachToActor(P, FAttachmentTransformRules::KeepWorldTransform);
		n++;
	}
	return n;
}

// Put the tree back on a freshly imported spatial file. The format carries
// no hierarchy of its own, so ours travels as Static metadata (see the
// exporter); a file from anywhere else simply has none and imports flat.
static int32 BF6_ApplyTreeMetadata(const TArray<TSharedPtr<FJsonValue>>* StaticArr)
{
	if (!StaticArr || !GEditor) return 0;
	UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
	FString Raw;
	for (const TSharedPtr<FJsonValue>& v : *StaticArr)
		if (const TSharedPtr<FJsonObject> o = v->AsObject())
			if (o->TryGetStringField(TEXT("metadata/bf6_tree"), Raw) && !Raw.IsEmpty()) break;
	if (Raw.IsEmpty()) return 0;
	TArray<TSharedPtr<FJsonValue>> Arr;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(R, Arr)) return 0;

	struct FTreeRow { FString Parent; bool bGroup = false; FTransform Xf; };
	TMap<FString, FTreeRow> Rows;
	for (const TSharedPtr<FJsonValue>& v : Arr)
	{
		const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
		FString N; if (!o->TryGetStringField(TEXT("n"), N) || N.IsEmpty()) continue;
		FTreeRow Row;
		o->TryGetStringField(TEXT("p"), Row.Parent);
		o->TryGetBoolField(TEXT("g"), Row.bGroup);
		if (Row.bGroup)
			Row.Xf = FTransform(
				FRotator(o->GetNumberField(TEXT("rp")), o->GetNumberField(TEXT("ry")), o->GetNumberField(TEXT("rr"))),
				FVector(o->GetNumberField(TEXT("x")), o->GetNumberField(TEXT("y")), o->GetNumberField(TEXT("z"))));
		Rows.Add(N, Row);
	}

	// the objects that came in, keyed the way the file names them
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		AActor* A = *It;
		if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
		FString Nm = A->GetActorLabel(); Nm.RemoveFromStart(TEXT("BF6_"));
		if (TagValue(A, TEXT("gpath:")).IsEmpty()) A->Tags.Add(FName(*(FString(TEXT("gpath:")) + Nm)));
		if (const FTreeRow* Row = Rows.Find(Nm))
			if (!Row->Parent.IsEmpty() && TagValue(A, TEXT("gtree:")).IsEmpty())
				A->Tags.Add(FName(*(FString(TEXT("gtree:")) + Row->Parent)));
	}
	// and the empty parents, which have no object of their own in the file.
	//
	// ONLY THE ONES THAT ARE NOT ALREADY THERE. LoadBaseSetup spawns a tree
	// node for every Godot pivot/camera in the base scene BEFORE this runs, and
	// a session saved after a .tscn import carries those same paths in its tree
	// metadata. Spawning them again gave two nodes per pivot, and because the
	// second took the same pretty label, Unreal uniquified it - which reads as
	// "it duplicated my nodes and renamed the originals". Keyed on gpath, which
	// is the node's identity, not on the label, which is cosmetic.
	TSet<FString> ExistingNodePaths;
	for (TActorIterator<AActor> It(W); It; ++It)
		if (It->Tags.Contains(kGroupTag))
		{
			const FString P = TagValue(*It, TEXT("gpath:"));
			if (!P.IsEmpty()) ExistingNodePaths.Add(P);
		}
	int32 Reused = 0;
	for (const TPair<FString, FTreeRow>& KV : Rows)
	{
		if (!KV.Value.bGroup) continue;
		if (ExistingNodePaths.Contains(KV.Key)) { ++Reused; continue; }
		if (AActor* GA = BF6_SpawnTreeNode(W, KV.Key, KV.Value.Parent, KV.Value.Xf))
		{
			BF6_SetPrettyLabel(GA, KV.Key);
			ExistingNodePaths.Add(KV.Key);   // the file may name one twice
		}
	}
	if (Reused > 0)
		UE_LOG(LogBF6, Display,
			TEXT("tree: %d node(s) already in the world were reused rather than duplicated"), Reused);
	return BF6_RebuildTreeFromTags();
}

// File one actor into its computed folder (no-op for handles/context).
static void BF6_FileActor(AActor* A)
{
	if (!A || A->Tags.Contains(kHandleTag) || A->Tags.Contains(kContextTag)) return;
	if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag) && !A->Tags.Contains(kGroupTag)) return;
	// an attached actor shows under its parent; a folder on it would be ignored
	if (A->GetAttachParentActor()) return;
	// Keeping the authored tree means the tree IS the organisation, as it is in
	// the SDK. Folders alongside it would be a second, conflicting hierarchy.
	// Whatever we decide below, this actor is new to the tree, so the tree needs
	// rebuilding before the frame ends. Asking here rather than at the twelve
	// call sites is the whole point: a spawner added tomorrow gets it free.
	ON_SCOPE_EXIT{ BF6Api::MarkSceneTreeDirty(); };
	if (BF6_KeepGodotTree()) { A->SetFolderPath(NAME_None); return; }
	A->SetFolderPath(FName(*BF6_FolderForActor(A)));
}

// Every placeable in the SDK regardless of level (the library's "Full" scope).
static TArray<TSharedPtr<FPlaceableRow>> g_allGlobal;

static void BF6_FillRows(const bf6_placeable* Buf, int32 n, TArray<TSharedPtr<FPlaceableRow>>& Out, bool bFeedLookups)
{
	for (int32 i = 0; i < n; i++)
	{
		TSharedPtr<FPlaceableRow> r = MakeShared<FPlaceableRow>();
		r->Type = UTF8_TO_TCHAR(Buf[i].type); r->Directory = UTF8_TO_TCHAR(Buf[i].directory);
		r->Mesh = UTF8_TO_TCHAR(Buf[i].mesh); r->PhysicsCost = Buf[i].physics_cost; r->Universal = Buf[i].universal != 0;
		Out.Add(r);
		if (bFeedLookups)
		{
			g_ss.TypeCost.Add(r->Type, r->PhysicsCost);
			if (!r->Mesh.IsEmpty()) { g_ss.TypeCost.Add(r->Mesh, r->PhysicsCost); g_ss.TypeToMesh.Add(r->Type, r->Mesh); }
		}
	}
}

// schema memo for PropsForType; cleared whenever the catalogue reloads
static TMap<FString, TArray<BF6Api::FPropDef>> g_propCache;

static void BF6_LoadPlaceables(const FString& Level)
{
	g_ss.AllItems.Reset(); g_ss.TypeCost.Reset(); g_ss.TypeToMesh.Reset();
	g_allGlobal.Reset();
	g_propCache.Reset();
	if (!g_ctx || !g_listp) return;
	const int32 kMax = 16000;
	TArray<bf6_placeable> Buf; Buf.SetNum(kMax);
	const int n = FMath::Min(g_listp(g_ctx, Level.IsEmpty() ? "" : TCHAR_TO_UTF8(*Level), "", Buf.GetData(), kMax), kMax);
	BF6_FillRows(Buf.GetData(), n, g_ss.AllItems, true);
	// the level-independent catalogue: the Full Library browses and places from
	// it, so its types must resolve meshes and budget costs too
	const int an = FMath::Min(g_listp(g_ctx, "", "", Buf.GetData(), kMax), kMax);
	BF6_FillRows(Buf.GetData(), an, g_allGlobal, true);

	// THE NODE, first in the shelf. It is the scene tree's building block, and
	// making it placeable is what lets a creator build the whole tree in the
	// editor: place a node where the group belongs, attach things to it by
	// pointing. No mesh - the card wears the Godot node icon instead.
	{
		TSharedPtr<FPlaceableRow> NodeRow = MakeShared<FPlaceableRow>();
		NodeRow->Type = TEXT("Node3D");
		NodeRow->Directory = TEXT("Gameplay");
		NodeRow->Mesh = FString();
		NodeRow->PhysicsCost = 0;
		g_ss.AllItems.Insert(NodeRow, 0);
		g_allGlobal.Insert(NodeRow, 0);
	}
}

static void BF6_LoadBudgetMax(const FString& Level)
{
	g_ss.BudgetMax = -1;
	const FString Path = FPaths::Combine(BF6_DataDir(), TEXT("FbExportData/level_info.json"));
	FString In; if (!FFileHelper::LoadFileToString(In, *Path)) return;
	TSharedPtr<FJsonObject> Root; TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
	const TSharedPtr<FJsonObject>* Lv = nullptr; if (!Root->TryGetObjectField(Level, Lv) || !Lv->IsValid()) return;
	const TSharedPtr<FJsonObject>* Bud = nullptr; if (!(*Lv)->TryGetObjectField(TEXT("budget"), Bud) || !Bud->IsValid()) return;
	double Max = -1; if ((*Bud)->TryGetNumberField(TEXT("physicsCostMax"), Max)) g_ss.BudgetMax = (int32)Max;
}

// ---- upload-size budget ----
// The Portal site rejects a per-map spatial file over 3 MiB client-side,
// BEFORE any upload ("Uploaded file is larger than 3.00 MB" - measured live
// 2026-08-20; the site's own bytesToMbString divides by 1048576, so 3.00 MB
// is exactly 3*1048576). The whole experience is additionally capped by the
// gRPC message size (4,194,304), with ~3.8 MB usable after protocol
// overhead. These MOVE with SDK updates, so the tool refreshes them from a
// remote limits.json we maintain - see BF6Api::FetchUploadLimits.
static FString BF6_BuildSpatialJson(bool bMinify);
static int64  g_upBytes = -1, g_upRawBytes = -1;
static int32  g_upObjects = -1;
static double g_upWhen = 0.0, g_upRatio = 0.0;
static int64  g_limPerMap = 3145728;      // 3 MiB, site-enforced per file (raw)
static int64  g_limExperience = 4194304;  // gRPC cap on the base64 bundle (all maps)

static void BF6_RecomputeBudget()
{
	g_ss.TotalCost = 0; g_ss.TotalObjects = 0;
	if (GEditor) if (UWorld* W = GEditor->GetEditorWorldContext().World())
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			const bool bP = It->Tags.Contains(kPlacedTag), bB = It->Tags.Contains(kBaseTag);
			if (!bP && !bB) continue;
			FString Ty = TagValue(*It, TEXT("label:")); if (Ty.IsEmpty()) Ty = TagValue(*It, TEXT("type:")); if (Ty.IsEmpty()) Ty = TagValue(*It, TEXT("mesh:"));
			g_ss.TotalObjects++;
			if (const int32* C = g_ss.TypeCost.Find(Ty)) g_ss.TotalCost += *C;
		}
	const bool bCap = g_ss.BudgetMax > 0;
	const float PhysFrac = bCap ? FMath::Clamp((float)g_ss.TotalCost / (float)g_ss.BudgetMax, 0.f, 1.f) : 0.f;

	// The site's REAL gate is upload size, not physics cost: a dry-run export
	// gives the exact bytes. DECOUPLED from user actions - re-estimating the
	// instant the count changed put a multi-second hitch on every place and
	// delete on big maps. Now: at most every 15 s, only when the count
	// actually moved, ONE minified pass; raw is derived from a measured ratio
	// (seeded by a single dual pass on the first estimate).
	const double Now = FPlatformTime::Seconds();
	if (g_ss.bEditing && g_upObjects != g_ss.TotalObjects && Now - g_upWhen > 15.0)
	{
		g_upBytes = (int64)FTCHARToUTF8(*BF6_BuildSpatialJson(true)).Length();
		if (g_upRatio <= 0.0 && g_upBytes > 0)
			g_upRatio = (double)FTCHARToUTF8(*BF6_BuildSpatialJson(false)).Length() / (double)g_upBytes;
		g_upRawBytes = (int64)(g_upBytes * (g_upRatio > 0.0 ? g_upRatio : 1.9));
		g_upObjects  = g_ss.TotalObjects;
		g_upWhen     = Now;
		// perf tripwire: a slow estimate here is invisible to users except as
		// a mystery hitch - make it show up in reports
		const double Ms = (FPlatformTime::Seconds() - Now) * 1000.0;
		if (Ms > 250.0)
			UE_LOG(LogBF6, Warning, TEXT("upload estimator took %.0f ms for %d objects - report this if the editor hitches"), Ms, g_ss.TotalObjects);
	}
	const float SizeFrac = (g_upBytes > 0 && g_limPerMap > 0)
		? FMath::Clamp((float)((double)g_upBytes / (double)g_limPerMap), 0.f, 1.f) : 0.f;

	g_ss.BudgetFrac = FMath::Max(PhysFrac, SizeFrac);
	g_ss.BudgetColor = (bCap || g_upBytes > 0) ? BF6Theme::BudgetFill(g_ss.BudgetFrac) : BF6Theme::BudgetLow;

	// the map's minified bytes base64-encode to ~4/3 in the upload bundle,
	// and the whole experience (all maps + script + strings) must fit the
	// gRPC cap. Show this map's share so a multi-map rotation stays legible.
	const int64 Base64 = (g_upBytes * 4 + 2) / 3;
	const FString SizePart = g_upBytes > 0
		? FString::Printf(TEXT("     upload %lld KB min / %lld KB raw   of %lld KB per map   |   uses %lld of %lld KB experience total"),
			g_upBytes / 1024, g_upRawBytes / 1024, g_limPerMap / 1024,
			Base64 / 1024, g_limExperience / 1024)
		: FString();
	if (bCap)
		g_ss.BudgetText = FText::FromString(FString::Printf(TEXT("physics %s / %s   (%d%%)   %d obj%s"),
			*FText::AsNumber(g_ss.TotalCost).ToString(), *FText::AsNumber(g_ss.BudgetMax).ToString(),
			FMath::RoundToInt(PhysFrac * 100.f), g_ss.TotalObjects, *SizePart));
	else
		g_ss.BudgetText = FText::FromString(FString::Printf(TEXT("physics %s (no cap)   %d obj%s"),
			*FText::AsNumber(g_ss.TotalCost).ToString(), g_ss.TotalObjects, *SizePart));

	// The Workspace map is a SCRATCH surface: every real thing lives in the
	// session json (autosaved every minute). Transactions keep dirtying the
	// level package anyway, which made closing prompt "save Workspace?" - and
	// answering yes tried to serialize our transient world. Keep it clean so
	// the confusing prompt (and the crash it led to) never appears.
	if (GEditor)
		if (UWorld* Wd = GEditor->GetEditorWorldContext().World())
			if (Wd->GetMapName().Contains(TEXT("Workspace")))
				if (UPackage* Pk = Wd->GetOutermost())
					if (Pk->IsDirty()) Pk->SetDirtyFlag(false);
}

static void BF6_LoadBaseSetup(const FString& Level)
{
	g_ss.BaseObjects.Reset();
	if (!GEditor) return;
	UWorld* World = GEditor->GetEditorWorldContext().World(); if (!World) return;
	FString In; if (!FFileHelper::LoadFileToString(In, *BF6_BaseJsonPath(Level))) { UE_LOG(LogBF6, Warning, TEXT("no base setup for %s"), *Level); return; }
	TSharedPtr<FJsonObject> Root; TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
	const TArray<TSharedPtr<FJsonValue>>* Objs = nullptr;
	if (!Root->TryGetArrayField(TEXT("objects"), Objs)) return;

	// "." is Godot's scene root - the node named after the map. Treating it as
	// "no parent" left every top-level object loose at the root of the tree
	// rather than under the map, which is not how the SDK shows it.
	FString RootName;
	TMap<FString, int32> OrderOf;
	TMap<FString, FVector> LocalPos; TMap<FString, FString> ParentOf;
	auto ReadPos = [](const TSharedPtr<FJsonObject>& O){ FVector p(0,0,0); const TArray<TSharedPtr<FJsonValue>>* a=nullptr; if(O->TryGetArrayField(TEXT("pos"),a)&&a->Num()>=3){p.X=(*a)[0]->AsNumber();p.Y=(*a)[1]->AsNumber();p.Z=(*a)[2]->AsNumber();} return p; };
	{
		int32 Idx = 0;
		for (const auto& v : *Objs)
		{
			const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
			const FString nm = o->GetStringField(TEXT("name"));
			LocalPos.Add(nm, ReadPos(o));
			FString par; o->TryGetStringField(TEXT("parent"), par);
			ParentOf.Add(nm, par);
			OrderOf.Add(nm, Idx++);   // the order the file lists them in
			if (par.IsEmpty()) RootName = nm;
		}
	}
	auto ResolveParent = [&RootName](const FString& Par) { return Par == TEXT(".") ? RootName : Par; };
	auto ToUnreal = [](const FVector& G){ return FVector((float)G.X,(float)G.Z,(float)G.Y)*100.f; };

	int32 oid = 0, spawned = 0;
	TMap<FString, FBF6GNode> GMap;
	BF6_BuildGNodeMap(*Objs, GMap);
	for (const auto& v : *Objs)
	{
		const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
		const FString nm = o->GetStringField(TEXT("name")); const FString ty = o->GetStringField(TEXT("type"));
		// Godot pivots and cameras never export - the site rejects them - but
		// they ARE the tree the creator built, so each becomes a parent actor
		// holding nothing but its transform, exactly as it does in the SDK.
		if (BF6_IsEngineNodeType(ty))
		{
			double EB[9]; FVector eg;
			BF6_GWorldOf(GMap, nm, EB, eg);
			if (AActor* GA = BF6_SpawnTreeNode(World, nm, ResolveParent(ParentOf.FindRef(nm)),
				FTransform(BF6_GRotFromB(EB), ToUnreal(eg), BF6_GScaleFromB(EB)), OrderOf.FindRef(nm)))
				BF6_SetPrettyLabel(GA, nm);
			continue;
		}
		// FULL parent-chain accumulation: rotation and scale ride pivots down
		// (the deploy camera's aim lives on its Camera3D parent)
		double WB[9]; FVector gw;
		BF6_GWorldOf(GMap, nm, WB, gw);
		const FRotator Rot = BF6_GRotFromB(WB);
		const FVector Scl = BF6_GScaleFromB(WB);
		AActor* A = nullptr; const TArray<TSharedPtr<FJsonValue>>* pts = nullptr;
		const bool bVolume = o->TryGetArrayField(TEXT("points"), pts) && pts->Num() >= 6;
		if (bVolume)
		{
			TArray<FVector> Loop;
			for (int32 i = 0; i + 1 < pts->Num(); i += 2){ const float px=(*pts)[i]->AsNumber(), pz=(*pts)[i+1]->AsNumber(); Loop.Add(ToUnreal(FVector(gw.X+px, gw.Y, gw.Z+pz))); }
			// the shipped zone height when the scene carries one (Godot metres)
			double VolH = 5.0;
			{
				const TSharedPtr<FJsonObject>* PP = nullptr; FString hs;
				if (o->TryGetObjectField(TEXT("props"), PP) && PP->IsValid() && (*PP)->TryGetStringField(TEXT("height"), hs) && hs.IsNumeric())
					VolH = FCString::Atod(*hs);
			}
			A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (VolH <= 0.01) VolH = 5.0;   // SDK: 0 = infinite, drawn at 5 m like Godot
			if (A){ UProceduralMeshComponent* VM=MakeProcMesh(A,TEXT("Volume")); BuildWalls(VM,Loop,(float)VolH*100.f); BF6_ApplyVolumeMaterial(A, VM); GVolumeLoops.Add(A, Loop); BF6_WriteLoopTags(A); }
		}
		else
		{
			A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (A){ UProceduralMeshComponent* MM=MakeProcMesh(A,TEXT("Model")); if(!FillProcFromBf6Mesh(MM,ObjModelPath(ty))) BuildMarker(MM); ApplyObjectWhite(MM); }
		}
		if (!A) continue;
		if (!bVolume) A->SetActorTransform(FTransform(Rot, ToUnreal(gw), Scl));
		A->SetActorLabel(nm);   // exact base name: saves match base objects by it
		A->Tags.Add(kBaseTag);
		// its place in the authored tree, hooked up once the whole file is in
		A->Tags.Add(FName(*(FString(TEXT("gpath:")) + nm)));
		A->Tags.Add(FName(*FString::Printf(TEXT("gord:%d"), OrderOf.FindRef(nm))));   // the order the file declares
		{
			const FString Par = ResolveParent(ParentOf.FindRef(nm));
			if (!Par.IsEmpty() && Par != TEXT(".")) A->Tags.Add(FName(*(FString(TEXT("gtree:")) + Par)));
		}
		BF6_FileActor(A);
		A->Tags.Add(FName(*(FString(TEXT("type:")) + ty)));
		A->Tags.Add(FName(*(FString(TEXT("oid:")) + FString::FromInt(oid))));
		// seed the object's shipped field values (Team, ObjId, timers, links) so
		// the attribute radial edits real data. Raw Godot forms convert to
		// ours - NodePath links become plain object names and enum ints become
		// their selection strings - so the base setup arrives WIRED, exactly
		// like the shipped level, not just placed.
		const TSharedPtr<FJsonObject>* PObj = nullptr;
		if (o->TryGetObjectField(TEXT("props"), PObj) && PObj->IsValid())
			for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : (*PObj)->Values)
			{
				FString Val;
				if (!KV.Value.IsValid() || !KV.Value->TryGetString(Val) || Val.IsEmpty()) continue;
				if (Val.Contains(TEXT("NodePath")))
				{
					// "NodePath(\"a/b\")" or "[NodePath(\"x\"), ...]" -> "b" / "x,y"
					// (base node names are unique per level, so the last path
					// segment IS the object's link name)
					TArray<FString> Names;
					int32 Pos = 0;
					while ((Pos = Val.Find(TEXT("NodePath(\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos)) != INDEX_NONE)
					{
						Pos += 10;
						const int32 End = Val.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
						if (End == INDEX_NONE) break;
						FString Path = Val.Mid(Pos, End - Pos);
						int32 Slash = INDEX_NONE;
						if (Path.FindLastChar(TEXT('/'), Slash)) Path = Path.Mid(Slash + 1);
						if (!Path.IsEmpty()) Names.Add(Path);
						Pos = End;
					}
					if (Names.Num() == 0) continue;
					Val = FString::Join(Names, TEXT(","));
				}
				else if (Val.StartsWith(TEXT("\"")) && Val.EndsWith(TEXT("\"")) && Val.Len() >= 2)
					Val = Val.Mid(1, Val.Len() - 2);
				else if (Val.IsNumeric())
				{
					// Godot stores selections as the enum INDEX
					for (const BF6Api::FPropDef& D : BF6Api::PropsForType(ty))
						if (D.Name == KV.Key && D.Type == TEXT("selection"))
						{
							const int32 Idx = FCString::Atoi(*Val);
							if (D.Options.IsValidIndex(Idx)) Val = D.Options[Idx];
							break;
						}
				}
				else if (Val.Contains(TEXT("(")))
					continue;   // Vector3/Color constructors: not our attributes
				if (!Val.IsEmpty()) A->Tags.Add(FName(*FString::Printf(TEXT("p:%s=%s"), *KV.Key, *Val)));
			}
		A->SetFlags(RF_Transient);
		FBaseObj bo; bo.Type = ty; g_ss.BaseObjects.Add(oid, bo);
		oid++; spawned++;
	}
	if (BF6_KeepGodotTree())
	{
		const int32 Hooked = BF6_RebuildTreeFromTags();
		if (Hooked > 0) UE_LOG(LogBF6, Display, TEXT("authored tree: %d object(s) attached to their parent"), Hooked);
	}
	// a fresh map: look at the play area rather than wherever the editor was
	BF6Api::FrameCombatArea();
		UE_LOG(LogBF6, Display, TEXT("Base setup loaded for %s: %d objects."), *Level, spawned);
}


static void BF6_HookSpawnWatch();   // fwd: defined with the other editor hooks
static void BF6_OpenMapWorldImpl(const FString& Level, const FString& SaveName)
{
	if (!GEditor) return;
	// The save may have been deleted since the menu drew it - the menu watches
	// the folder, but a click can still land in the gap. Opening it anyway
	// would be the worse half of that: the session would take the missing
	// save's NAME onto an empty base map, and the next save would write the
	// deleted file straight back. Open the base map under no name instead.
	FString Save = SaveName;
	if (!Save.IsEmpty() && !FPaths::FileExists(BF6_SessionPathFor(Level, Save)))
	{
		Notify(FString::Printf(TEXT("The save '%s' is no longer on disk - opening the base map instead."), *Save));
		Save.Empty();
	}
	// Tell the add-ons the old map is going before anything is torn down, so
	// an overlay can drop its own actors while the world is still coherent.
	if (!g_ss.CurrentLevel.IsEmpty()) { BF6Api::BF6_MapDecalStash(); BF6ExtInternal::BroadcastMapClosing(g_ss.CurrentLevel); }
	BF6_EnsureBaseSetupFormat();
	g_ss.CurrentLevel = Level; g_ss.CurrentSave = Save; g_ss.bEditing = !Save.IsEmpty();
	BF6_HookSpawnWatch();   // the world may have been replaced since last time
	BF6_ClearContextFor(Level); ClearActorsWithTag(kPlacedTag); ClearActorsWithTag(kBaseTag); ClearActorsWithTag(kGroupTag);
	BF6_LoadPlaceables(Level); BF6_LoadBudgetMax(Level);
	const FString TP = BF6_MapMeshPath(Level, TEXT("_terrain.bf6mesh"));
	if (!GContextReused) if (FPaths::FileExists(TP)) SpawnContextMesh(TP, FString::Printf(TEXT("%s_Terrain"), *Level));
	const FString AP = BF6_MapMeshPath(Level, TEXT("_assets.bf6mesh"));
	if (!GContextReused) if (FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *Level));
	BF6_LoadBaseSetup(Level);
	if (!Save.IsEmpty()) LoadSession(Level, Save);
	// COLOURS LAST, once every path has finished restoring tags.
	//
	// A volume's colour lives in a tag, and the several spawn paths do not
	// agree on when tags land relative to the walls being built: the spatial
	// importer restores properties after the material, the session loader
	// restores raw tags after that again. Re-applying once here is one line
	// against auditing four call orders, and it cannot be wrong - the tag is
	// the only source either way.
	BF6_ReapplyVolumeColors();
	BF6_RecomputeBudget();
	BF6ExtInternal::BroadcastMapOpened(Level, Save);
}

// Export the current session to <map>.spatial.json (Portal format). bMinify
// runs the PortalSpatialMinifier-style renaming for the upload size cap;
// without it names stay readable so the file re-imports and shares cleanly.
static FString BF6_BuildSpatialJson(bool bMinify)
{
	if (!GEditor) return FString();
	UWorld* World = GEditor->GetEditorWorldContext().World(); if (!World) return FString();
	const FString Level = g_ss.CurrentLevel;
	auto Vec = [](double x,double y,double z){ TSharedPtr<FJsonObject> v=MakeShared<FJsonObject>(); v->SetNumberField(TEXT("x"),x); v->SetNumberField(TEXT("y"),y); v->SetNumberField(TEXT("z"),z); return v; };

	TMap<FString,FString> ShortMap; int32 ShortCtr = 1;
	auto ShortName = [&](const FString& Orig)->FString{ if(!bMinify||Orig.IsEmpty())return Orig; if(const FString* F=ShortMap.Find(Orig))return *F; FString Rr; int32 Num=ShortCtr++; while(Num>0){Num--; Rr=FString::Chr((TCHAR)('a'+(Num%26)))+Rr; Num/=26;} ShortMap.Add(Orig,Rr); return Rr; };

	// Emit one property value with the SDK schema's type: bools and numbers as
	// such, link types (volume refs / spawn-point arrays) as minified ids. Raw
	// Godot NodePath/ExtResource values from the shipped scenes are skipped.
	auto EmitTyped = [&](const TSharedPtr<FJsonObject>& e, const BF6Api::FPropDef& D, const FString& Raw)
	{
		FString V = Raw;
		if (V.IsEmpty() || V.Contains(TEXT("NodePath")) || V.Contains(TEXT("ExtResource"))) return;
		// SELECTIONS ARE NAMES, AND GODOT STORES THE INDEX. LoadBaseSetup and the
		// .tscn import both convert index -> option name when they SEED an actor,
		// so a live actor already reads "Team2". The base.json fallback below did
		// not, so any base object with no live actor exported its raw index:
		// TEAM_2_HQ shipped Team "2", which matches no selection name, and Portal
		// fell back to the default Team1 - both HQs ended up on team 1.
		// Guarded on IsNumeric so an already-converted name is never re-indexed
		// ("Team2" would otherwise Atoi to 0 and become TeamNeutral).
		if (D.Type == TEXT("selection") && V.IsNumeric())
		{
			const int32 Idx = FCString::Atoi(*V);
			if (D.Options.IsValidIndex(Idx)) V = D.Options[Idx];
			else UE_LOG(LogBF6, Warning,
				TEXT("export: '%s' selection index %d is outside its %d option(s) - emitting it raw"),
				*D.Name, Idx, D.Options.Num());
		}
		const bool bLink = D.Type.Contains(TEXT("Volume")) || D.Type.Contains(TEXT("Array[")) || D.Type.Contains(TEXT("Path")) || D.Type.Contains(TEXT("SpawnPoint"));
		if (bLink)
		{
			TArray<FString> Parts; V.ParseIntoArray(Parts, TEXT(","));
			if (D.Type.Contains(TEXT("Array[")))
			{
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (FString& P : Parts) Arr.Add(MakeShared<FJsonValueString>(ShortName(P.TrimStartAndEnd())));
				e->SetArrayField(D.Name, Arr);
			}
			else if (Parts.Num()) e->SetStringField(D.Name, ShortName(Parts[0].TrimStartAndEnd()));
			return;
		}
		if (D.Type == TEXT("bool")) { e->SetBoolField(D.Name, V.Equals(TEXT("true"), ESearchCase::IgnoreCase)); return; }
		if ((D.Type == TEXT("int") || D.Type == TEXT("float")) && V.IsNumeric()) { e->SetNumberField(D.Name, FCString::Atod(*V)); return; }
		if (D.Type == TEXT("vector"))
		{
			// stored as "x,y,z"; the spatial format wants a number array
			TArray<FString> Parts; V.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() == 3)
			{
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (FString& P : Parts) Arr.Add(MakeShared<FJsonValueNumber>(FCString::Atod(*P.TrimStartAndEnd())));
				e->SetArrayField(D.Name, Arr);
			}
			return;
		}
		e->SetStringField(D.Name, V);
	};

	// live base actors: edited attribute values and reshaped zones win over the
	// shipped json (matched by name via the actor label)
	TMap<FString, AActor*> LiveByName;
	for (TActorIterator<AActor> It(World); It; ++It)
		if (It->Tags.Contains(kBaseTag))
		{
			FString L = It->GetActorLabel();
			L.RemoveFromStart(TEXT("BF6_"));
			LiveByName.Add(L, *It);
			// Unreal uniquifies labels, so a base setup shipping the same name
			// twice (MP_Aftermath_Portal has CapturePointArea and PlayerSpawner
			// three times each) lands as Name, Name1, Name_2... Register the
			// stripped stem too, or every copy but the first looks deleted.
			FString Stem = L;
			while (Stem.Len() > 1 && FChar::IsDigit(Stem[Stem.Len() - 1])) Stem.LeftChopInline(1);
			Stem.RemoveFromEnd(TEXT("_"));
			if (!Stem.IsEmpty() && Stem != L && !LiveByName.Contains(Stem)) LiveByName.Add(Stem, *It);
		}
	// DELETING A BASE OBJECT MUST REMOVE IT FROM THE EXPORT. The base entries
	// below are read from base.json on disk, and a missing live actor used to
	// mean only "not moved, use the shipped transform" - so an HQ the creator
	// deleted in the editor still shipped in the .spatial.json, and the map got
	// it twice. LoadBaseSetup spawns a kBaseTag actor for every base entry
	// except the engine node types the exporter already skips, so once any base
	// actor exists, a name with no actor is a deletion.
	//
	// Guarded on the map being loaded at all: exporting with nothing loaded
	// must not silently strip the whole base setup.
	const bool bBaseLoaded = LiveByName.Num() > 0;
	int32 DeletedBaseSkipped = 0;

	TArray<TSharedPtr<FJsonValue>> Dynamic;
	FString In; TSharedPtr<FJsonObject> BaseRoot;
	if (FFileHelper::LoadFileToString(In, *BF6_BaseJsonPath(Level))) { TSharedRef<TJsonReader<>> R=TJsonReaderFactory<>::Create(In); FJsonSerializer::Deserialize(R,BaseRoot); }
	// duplicate-id defenses (the Portal site rejects any repeated id)
	TSet<FString> LivePlacedNames;   // placed actors' link names, collected up front
	TMap<FString, AActor*> LivePlacedByName;   // the twin that supersedes a base entry
	TSet<FString> BaseEmitted;       // base entries actually emitted
	const TArray<TSharedPtr<FJsonValue>>* BObjs = nullptr;
	if (BaseRoot.IsValid() && BaseRoot->TryGetArrayField(TEXT("objects"), BObjs))
	{
		TMap<FString, FBF6GNode> GMap;
		BF6_BuildGNodeMap(*BObjs, GMap);
		// A live PLACED copy of a base object (a re-imported export carries
		// the base setup as placed objects) SUPERSEDES the shipped entry.
		// Emitting both gave two objects the same name, the minifier's
		// memoized shortener handed them the same id, and the Portal site
		// rejected the whole experience with "duplicate ids".
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (!It->Tags.Contains(kPlacedTag)) continue;
			FString T2 = TagValue(*It, TEXT("label:")); if (T2.IsEmpty()) T2 = TagValue(*It, TEXT("mesh:"));
			if (T2.IsEmpty() || BF6_IsEngineNodeType(T2)) continue;
			FString PLn = It->GetActorLabel(); PLn.RemoveFromStart(TEXT("BF6_"));
			if (!PLn.IsEmpty()) { LivePlacedNames.Add(PLn); LivePlacedByName.Add(PLn, *It); }
		}
		for (const auto& v:*BObjs)
		{
			const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue;
			const FString nm=o->GetStringField(TEXT("name")), ty=o->GetStringField(TEXT("type"));
			// statics emit separately; Godot engine pivots/cameras never emit
			// (the site errors on them as "unknown types")
			if (ty.StartsWith(TEXT("MP_")) || BF6_IsEngineNodeType(ty)) continue;
			if (LivePlacedNames.Contains(nm))
			{
				// The twin replaces this entry, so it must carry the id scripts use.
				// Losing one here is silent in the file and fatal in the game, so it
				// is called out by name rather than merely logged as superseded.
				double BaseObjId = -1.0;
				o->TryGetNumberField(TEXT("ObjId"), BaseObjId);
				AActor** Twin = LivePlacedByName.Find(nm);
				const FString TwinId = (Twin && *Twin) ? BF6Api::GetActorProp(*Twin, TEXT("ObjId")) : FString();
				if (BaseObjId >= 0.0 && (TwinId.IsEmpty() || FCString::Atoi(*TwinId) < 0))
				{
					UE_LOG(LogBF6, Error, TEXT("export: '%s' had ObjId %d in the base setup, but the placed copy that replaces it has none - scripts addressing that id will not find it."), *nm, (int32)BaseObjId);
					Notify(FString::Printf(TEXT("'%s' lost its ObjId %d - set it on the placed copy before uploading."), *nm, (int32)BaseObjId));
				}
				else UE_LOG(LogBF6, Warning, TEXT("export: base '%s' superseded by a placed object with the same name"), *nm);
				continue;
			}
			if (bBaseLoaded && !LiveByName.Contains(nm))
			{
				// deleted in the editor - honour that rather than re-emitting it
				++DeletedBaseSkipped;
				UE_LOG(LogBF6, Warning,
					TEXT("export: base '%s' (%s) was deleted in the editor - not exported"),
					*nm, *ty);
				continue;
			}
			// (BaseEmitted is added below, under the emitted name, so the
			// duplicate-name check can tell a repeat from the first copy)
			// ACCUMULATED transform (parent pivots carry the deploy camera's
			// aim), unless the actor was MOVED in the editor - then the live
			// transform wins, converted like the placed-object path
			double WB[9]; FVector gw;
			BF6_GWorldOf(GMap, nm, WB, gw);
			TSharedPtr<FJsonObject> e=MakeShared<FJsonObject>();
			// A base setup may ship the SAME name more than once (three
			// CapturePointArea and three PlayerSpawner on MP_Aftermath_Portal).
			// name and id both came from ShortName(nm), which memoizes, so every
			// copy got one id and the Portal site rejected the experience over
			// the collision. Give the repeats a suffix; the first keeps the
			// authored name so existing links and scripts still resolve.
			FString EmitName = nm;
			if (BaseEmitted.Contains(nm))
			{
				int32 Copy = 2;
				while (BaseEmitted.Contains(FString::Printf(TEXT("%s_%d"), *nm, Copy))) ++Copy;
				EmitName = FString::Printf(TEXT("%s_%d"), *nm, Copy);
				UE_LOG(LogBF6, Warning,
					TEXT("export: base setup ships '%s' more than once - the repeat exports as '%s' so the ids stay unique"),
					*nm, *EmitName);
			}
			BaseEmitted.Add(EmitName);
			e->SetStringField(TEXT("name"), ShortName(EmitName)); e->SetStringField(TEXT("type"), ty);
			AActor* Live = LiveByName.FindRef(nm);
			if (Live)
			{
				const FTransform Xf = Live->GetActorTransform();
				const FVector L = Xf.GetLocation();
				const FVector S3 = Xf.GetScale3D();
				auto Sw = [](const FVector& x){ return FVector(x.X, x.Z, x.Y); };
				const FVector Rr = Sw(Xf.GetUnitAxis(EAxis::X) * S3.X);
				const FVector Uu = Sw(Xf.GetUnitAxis(EAxis::Z) * S3.Z);
				const FVector Ff = Sw(Xf.GetUnitAxis(EAxis::Y) * S3.Y);
				e->SetObjectField(TEXT("right"), Vec(Rr.X, Rr.Y, Rr.Z));
				e->SetObjectField(TEXT("up"),    Vec(Uu.X, Uu.Y, Uu.Z));
				e->SetObjectField(TEXT("front"), Vec(Ff.X, Ff.Y, Ff.Z));
				e->SetObjectField(TEXT("position"), Vec(L.X / 100.0, L.Z / 100.0, L.Y / 100.0));
			}
			else
			{
				e->SetObjectField(TEXT("right"), Vec(WB[0], WB[3], WB[6]));
				e->SetObjectField(TEXT("up"),    Vec(WB[1], WB[4], WB[7]));
				e->SetObjectField(TEXT("front"), Vec(WB[2], WB[5], WB[8]));
				e->SetObjectField(TEXT("position"), Vec(gw.X, gw.Y, gw.Z));
			}
			e->SetStringField(TEXT("id"), ShortName(EmitName));
			// zone polygon: the REAL spatial format wants GLOBAL Godot {x,y,z}
			// vectors plus a height field (verified against shipped experiences).
			// A reshaped loop from the point editor wins over the json.
			const TArray<TSharedPtr<FJsonValue>>* pts=nullptr;
			if (o->TryGetArrayField(TEXT("points"),pts))
			{
				TArray<TSharedPtr<FJsonValue>> PtsArr;
				const TArray<FVector>* EditedLoop = Live ? GVolumeLoops.Find(Live) : nullptr;
				if (EditedLoop && EditedLoop->Num() >= 3)
				{
					// world = loop through the actor, so a MOVED zone exports moved
					for (const FVector& Wv : BF6_LoopToWorld(Live, *EditedLoop))
						PtsArr.Add(MakeShared<FJsonValueObject>(Vec(Wv.X / 100.0, Wv.Z / 100.0, Wv.Y / 100.0)));
				}
				else
				{
					for (int32 i = 0; i + 1 < pts->Num(); i += 2)
						PtsArr.Add(MakeShared<FJsonValueObject>(Vec(gw.X + (*pts)[i]->AsNumber(), gw.Y, gw.Z + (*pts)[i + 1]->AsNumber())));
				}
				e->SetArrayField(TEXT("points"), PtsArr);
			}
			// field values: live edits win, else the shipped values from the scene
			{
				const TArray<BF6Api::FPropDef> Defs = BF6Api::PropsForType(ty);
				const TSharedPtr<FJsonObject>* JP = nullptr;
				o->TryGetObjectField(TEXT("props"), JP);
				TArray<TSharedPtr<FJsonValue>> LinkedNames;
				for (const BF6Api::FPropDef& D : Defs)
				{
					FString V = Live ? BF6Api::GetActorProp(Live, D.Name) : FString();
					if (V.IsEmpty() && JP && JP->IsValid()) (*JP)->TryGetStringField(D.Name, V);
					if (V.IsEmpty() || V.Contains(TEXT("NodePath")) || V.Contains(TEXT("ExtResource"))) continue;
					EmitTyped(e, D, V);
					if (D.Type.Contains(TEXT("Volume")) || D.Type.Contains(TEXT("Array[")) || D.Type.Contains(TEXT("Path")) || D.Type.Contains(TEXT("SpawnPoint")))
						LinkedNames.Add(MakeShared<FJsonValueString>(D.Name));
				}
				// the format's "linked" array: which fields are object references
				if (LinkedNames.Num()) e->SetArrayField(TEXT("linked"), LinkedNames);
			}
			Dynamic.Add(MakeShared<FJsonValueObject>(e));
		}
	}

	auto Swap=[](const FVector& v){ return FVector(v.X,v.Z,v.Y); };
	int32 pid = 0;
	TSet<FString> UsedPlacedNames;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(kPlacedTag)) continue;
		FString Type=TagValue(*It,TEXT("label:")); if(Type.IsEmpty())Type=TagValue(*It,TEXT("mesh:")); if(Type.IsEmpty())continue;
		if (BF6_IsEngineNodeType(Type)) continue;   // Godot pivots from old imports
		const FTransform Xf=It->GetActorTransform(); const FVector L=Xf.GetLocation();
		// axis lengths carry the object's scale in the Godot basis
		const FVector S3=Xf.GetScale3D();
		const FVector Rr=Swap(Xf.GetUnitAxis(EAxis::X)*S3.X), U=Swap(Xf.GetUnitAxis(EAxis::Z)*S3.Z), F=Swap(Xf.GetUnitAxis(EAxis::Y)*S3.Y);
		TSharedPtr<FJsonObject> e=MakeShared<FJsonObject>();
		// export under the actor's LINK NAME so links between placed objects
		// (wizard flags -> spawns) survive in the file; a duplicate label
		// falls back to placed_N so ids stay unique
		FString Ln = It->GetActorLabel(); Ln.RemoveFromStart(TEXT("BF6_"));
		// dedupe against OTHER placed names AND the emitted base entries - a
		// name shared with a base entry would minify to the same id
		if (Ln.IsEmpty() || UsedPlacedNames.Contains(Ln) || BaseEmitted.Contains(Ln)) Ln = FString::Printf(TEXT("placed_%d"), pid);
		UsedPlacedNames.Add(Ln);
		const FString nm=ShortName(Ln);
		e->SetStringField(TEXT("name"),nm); e->SetStringField(TEXT("type"),Type);
		e->SetObjectField(TEXT("right"),Vec(Rr.X,Rr.Y,Rr.Z)); e->SetObjectField(TEXT("up"),Vec(U.X,U.Y,U.Z)); e->SetObjectField(TEXT("front"),Vec(F.X,F.Y,F.Z));
		e->SetObjectField(TEXT("position"),Vec(L.X/100.0,L.Z/100.0,L.Y/100.0));
		e->SetStringField(TEXT("id"),nm); e->SetNumberField(TEXT("ObjId"),-1);
		// edited attribute values from the context radial
		{
			const TArray<BF6Api::FPropDef> Defs = BF6Api::PropsForType(Type);
			TArray<TSharedPtr<FJsonValue>> LinkedNames;
			for (const BF6Api::FPropDef& D : Defs)
			{
				const FString V = BF6Api::GetActorProp(*It, D.Name);
				if (V.IsEmpty() || V.Contains(TEXT("NodePath")) || V.Contains(TEXT("ExtResource"))) continue;
				EmitTyped(e, D, V);
				if (D.Type.Contains(TEXT("Volume")) || D.Type.Contains(TEXT("Array[")) || D.Type.Contains(TEXT("Path")) || D.Type.Contains(TEXT("SpawnPoint")))
					LinkedNames.Add(MakeShared<FJsonValueString>(D.Name));
			}
			if (LinkedNames.Num()) e->SetArrayField(TEXT("linked"), LinkedNames);
		}
		// placed zone volumes: the spatial format wants global Godot points
		if (const TArray<FVector>* Loop = GVolumeLoops.Find(*It))
		{
			if (BF6_IsPathActor(*It))
			{
				// A PATH exports as TWO entities, the way gdconverter does it
				// (test_waypointpath.py pins this): the owner keeps its
				// transform and gains `Waypoints: <path id>` in its linked
				// list, and a separate type:"WaypointPath" entity carries the
				// world-space points and the isClosed flag - and no transform.
				const FString PathNm = ShortName(Ln + TEXT("_Path"));
				e->SetStringField(TEXT("Waypoints"), PathNm);
				TArray<TSharedPtr<FJsonValue>> Lk;
				const TArray<TSharedPtr<FJsonValue>>* HaveLk = nullptr;
				if (e->TryGetArrayField(TEXT("linked"), HaveLk)) Lk = *HaveLk;
				Lk.Add(MakeShared<FJsonValueString>(TEXT("Waypoints")));
				e->SetArrayField(TEXT("linked"), Lk);

				TSharedPtr<FJsonObject> pe = MakeShared<FJsonObject>();
				pe->SetStringField(TEXT("name"), PathNm);
				pe->SetStringField(TEXT("type"), TEXT("WaypointPath"));
				pe->SetStringField(TEXT("id"), PathNm);
				pe->SetBoolField(TEXT("isClosed"), BF6_PathIsClosed(*It));
				TArray<TSharedPtr<FJsonValue>> PPts;
				for (const FVector& Wv : BF6_LoopToWorld(*It, *Loop))
					PPts.Add(MakeShared<FJsonValueObject>(Vec(Wv.X / 100.0, Wv.Z / 100.0, Wv.Y / 100.0)));
				pe->SetArrayField(TEXT("points"), PPts);
				Dynamic.Add(MakeShared<FJsonValueObject>(pe));
			}
			else
			{
				TArray<TSharedPtr<FJsonValue>> PtsArr;
				for (const FVector& Wv : BF6_LoopToWorld(*It, *Loop))
					PtsArr.Add(MakeShared<FJsonValueObject>(Vec(Wv.X / 100.0, Wv.Z / 100.0, Wv.Y / 100.0)));
				e->SetArrayField(TEXT("points"), PtsArr);
			}
		}
		// ObjId parity with gdconverter's compatibility shim. For these six
		// types Godot's default is 0 and an unset id exports as 0; our internal
		// "unset" is -1, which the shim would have stripped. So -1 becomes 0
		// here, and everything else keeps whatever it carries.
		{
			static const TCHAR* Six[] = { TEXT("Bomb"), TEXT("CapturePoint"), TEXT("DeployCam"),
				TEXT("RingOfFire"), TEXT("MCOM"), TEXT("Sector") };
			for (const TCHAR* T : Six)
				if (Type == T)
				{
					double Id = -1.0;
					if (!e->TryGetNumberField(TEXT("ObjId"), Id) || FMath::IsNearlyEqual(Id, -1.0))
						e->SetNumberField(TEXT("ObjId"), 0);
					break;
				}
		}
		Dynamic.Add(MakeShared<FJsonValueObject>(e)); pid++;
	}

	TArray<TSharedPtr<FJsonValue>> Static;
	const TCHAR* Suffixes[2]={ TEXT("_Terrain"), TEXT("_Assets") };
	for (const TCHAR* Suffix : Suffixes)
	{
		const FString snm=Level+Suffix; TSharedPtr<FJsonObject> e=MakeShared<FJsonObject>();
		e->SetStringField(TEXT("name"),snm); e->SetBoolField(TEXT("metadata/_edit_lock_"),true); e->SetStringField(TEXT("type"),snm);
		// the spatial format has NO name field, so the custom map name rides as
		// object metadata - unknown per-object keys pass the site untouched
		// (corpus-proven: Godot leaks metadata/_edit_group_ into uploads), and
		// our importer reads it back so round-trips keep the user's name
		if (!g_ss.CurrentSave.IsEmpty()) e->SetStringField(TEXT("metadata/bf6_save"), g_ss.CurrentSave);
		// The tree rides along the same way. The spatial format is a flat list -
		// it has no parent field and never will - so an export that did not carry
		// this would come back as loose objects however carefully it was built.
		// Empty parents have no entry of their own, so their transform travels
		// here too; names go through ShortName so a minified file still matches.
		{
			TArray<TSharedPtr<FJsonValue>> TreeArr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* A = *It;
				const bool bGroup = A->Tags.Contains(kGroupTag);
				if (!bGroup && !A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
				AActor* Par = A->GetAttachParentActor();
				if (!Par && !bGroup) continue;   // a loose object needs no entry
				FString Nm = A->GetActorLabel(); Nm.RemoveFromStart(TEXT("BF6_"));
				TSharedPtr<FJsonObject> t = MakeShared<FJsonObject>();
				t->SetStringField(TEXT("n"), ShortName(Nm));
				if (Par)
				{
					FString PN = Par->GetActorLabel(); PN.RemoveFromStart(TEXT("BF6_"));
					t->SetStringField(TEXT("p"), ShortName(PN));
				}
				if (bGroup)
				{
					t->SetBoolField(TEXT("g"), true);
					const FTransform Xf = A->GetActorTransform();
					const FVector L = Xf.GetLocation();
					const FRotator R = Xf.Rotator();
					t->SetNumberField(TEXT("x"), L.X); t->SetNumberField(TEXT("y"), L.Y); t->SetNumberField(TEXT("z"), L.Z);
					t->SetNumberField(TEXT("rp"), R.Pitch); t->SetNumberField(TEXT("ry"), R.Yaw); t->SetNumberField(TEXT("rr"), R.Roll);
				}
				TreeArr.Add(MakeShared<FJsonValueObject>(t));
			}
			if (TreeArr.Num() > 0)
			{
				FString TreeStr;
				TSharedRef<TJsonWriter<>> TW = TJsonWriterFactory<>::Create(&TreeStr);
				FJsonSerializer::Serialize(TreeArr, TW);
				e->SetStringField(TEXT("metadata/bf6_tree"), TreeStr);
			}
		}
		e->SetObjectField(TEXT("right"),Vec(1,0,0)); e->SetObjectField(TEXT("up"),Vec(0,1,0)); e->SetObjectField(TEXT("front"),Vec(0,0,1)); e->SetObjectField(TEXT("position"),Vec(0,0,0));
		e->SetStringField(TEXT("id"),FString::Printf(TEXT("Static/%s"),*snm));
		Static.Add(MakeShared<FJsonValueObject>(e));
	}

	// final guarantee: no duplicate non-empty ids leave this function - the
	// site rejects the whole experience over a single collision
	{
		TSet<FString> Seen; int32 Dups = 0;
		for (const TSharedPtr<FJsonValue>& v : Dynamic)
		{
			const TSharedPtr<FJsonObject> o = v->AsObject();
			FString Id;
			if (o.IsValid() && o->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
			{
				if (Seen.Contains(Id)) Dups++;
				Seen.Add(Id);
			}
		}
		if (Dups > 0)
			Notify(FString::Printf(TEXT("Export warning: %d duplicate id(s) remain - the Portal site will reject this file. Please report this with the export attached."), Dups));
	}

	// Dropping base objects is a visible decision, so say so rather than
	// letting the creator wonder whether the delete took.
	if (DeletedBaseSkipped > 0)
	{
		UE_LOG(LogBF6, Warning, TEXT("export: %d base object(s) deleted in the editor were left out"), DeletedBaseSkipped);
		Notify(FString::Printf(TEXT("%d base object(s) you deleted were left out of the export."), DeletedBaseSkipped));
	}

	TSharedPtr<FJsonObject> Root=MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("Portal_Dynamic"),Dynamic); Root->SetArrayField(TEXT("Static"),Static);
	FString Out; TSharedRef<TJsonWriter<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>> W=TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(),W);
	return Out;
}

static void BF6_ExportSpatial(bool bMinify)
{
	const FString Out = BF6_BuildSpatialJson(bMinify);
	const FString Level = g_ss.CurrentLevel;
	// community naming convention: <Level>_<SaveName>.spatial.json - the file
	// is recognizable, and two projects on one map never overwrite each other
	FString SafeSave = g_ss.CurrentSave;
	for (TCHAR& C : SafeSave) if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-')) C = TEXT('_');
	const FString File = SafeSave.IsEmpty()
		? Level + TEXT(".spatial.json")
		: Level + TEXT("_") + SafeSave + TEXT(".spatial.json");
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("export"), File);
	FFileHelper::SaveStringToFile(Out, *Path);
	Notify(FString::Printf(TEXT("Exported %d KB -> %s"), FTCHARToUTF8(*Out).Length() / 1024, *File));
	UE_LOG(LogBF6, Warning, TEXT("Exported spatial.json (%d bytes): %s"), FTCHARToUTF8(*Out).Length(), *Path);
	// open Explorer with the file SELECTED so the right file gets uploaded -
	// a session save grabbed by mistake fails on the site with a confusing
	// "no accepted layers" error
	BF6_OpenInExplorer(Path, true);
}

// Import any Portal .spatial.json as an editable custom map (file dialog + data
// load). Free twin of SBF6Browser::OnImportSpatial, minus the tab UI bits.
// ============================================================================
// .tscn import: open a Godot SDK level scene directly. Most creators come
// from the official editor, and because the Portal site sometimes loses
// spatial attachments, the .tscn on disk is often the only surviving copy of
// a map. Parses the text scene format natively: ext_resources give each
// node's type, transforms ACCUMULATE through parent pivots (position-only
// accumulation twists rotated maps - the headless-Godot transform law),
// NodePath links resolve to our name-based link tags, and enum ints become
// their selection strings.
// ============================================================================
// Reads one attribute out of a .tscn line. The match has to START a word:
// searching for id=" would otherwise find it inside uid=", which is how every
// PolygonVolume in a scene came to be dropped on import - the resource
// registered under "uid://jv31so3xcr7p" instead of "5_6gimq", the nodes that
// referenced it resolved to no type at all, and 25 combat volumes, HQ areas and
// capture areas vanished without a word. Godot writes uid= on anything that has
// one, so this is not a one-off.
static FString BF6_TscnAttr(const FString& Line, const TCHAR* Key)
{
	const int32 KeyLen = FCString::Strlen(Key);
	for (int32 i = Line.Find(Key, ESearchCase::CaseSensitive); i != INDEX_NONE;
		i = Line.Find(Key, ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1))
	{
		if (i > 0)
		{
			const TCHAR Before = Line[i - 1];
			if (FChar::IsAlnum(Before) || Before == TEXT('_')) continue;   // uid= is not id=
		}
		const int32 s = i + KeyLen;
		const int32 e = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, s);
		return e == INDEX_NONE ? FString() : Line.Mid(s, e - s);
	}
	return FString();
}

static bool BF6_TscnNums(const FString& S, TArray<double>& Out)
{
	int32 a = INDEX_NONE, b = INDEX_NONE;
	if (!S.FindChar(TEXT('('), a) || !S.FindLastChar(TEXT(')'), b) || b <= a) return false;
	TArray<FString> Parts;
	S.Mid(a + 1, b - a - 1).ParseIntoArray(Parts, TEXT(","));
	for (FString& P : Parts) Out.Add(FCString::Atod(*P.TrimStartAndEnd()));
	return Out.Num() > 0;
}

// resolve a NodePath value ("CombatVolume", "../SpawnPoint_2") against the
// node's own scene path
static FString BF6_TscnResolvePath(const FString& From, const FString& Rel)
{
	TArray<FString> Seg;
	From.ParseIntoArray(Seg, TEXT("/"));
	TArray<FString> R;
	Rel.ParseIntoArray(R, TEXT("/"));
	for (const FString& S : R)
	{
		if (S == TEXT("..")) { if (Seg.Num()) Seg.Pop(); }
		else if (S != TEXT(".")) Seg.Add(S);
	}
	return FString::Join(Seg, TEXT("/"));
}

static bool BF6_ImportTscnFile(const FString& File)
{
	FString In;
	if (!FFileHelper::LoadFileToString(In, *File)) { Notify(TEXT("Could not read the file.")); return false; }

	struct FTNode
	{
		FString Name, Path, ParentPath, Type;
		bool bSkip = false;              // static subtree / hidden convention
		double M[9] = { 1,0,0, 0,1,0, 0,0,1 };   // godot basis, row-major
		double O[3] = { 0,0,0 };
		TArray<double> Points;           // local x,z pairs (PolygonVolume)
		double Height = 0.0;
		// THE VOLUME'S AUTHORED COLOUR, and it is not editor leakage.
		//
		// This importer used to drop `color` alongside `visible` and
		// `process_priority` as noise the Godot editor writes. It is not: both
		// volume types declare it as an exported property whose whole purpose
		// is to tell one volume from another, and creators use it that way. An
		// imported map arrived as a wall of identical translucent boxes with
		// the creator's organisation thrown away.
		bool bHasColor = false;
		FLinearColor Color = FLinearColor::White;
		bool bHasSize = false; double Size[3] = { 10,10,10 };
		// WaypointPath: the Curve3D sub-resource this node references, resolved
		// after the parse into positions (Godot writes 9 numbers per point -
		// in-handle, out-handle, position - and only the position matters here).
		FString CurveId, ShapeId;
		TArray<double> PathPts;          // x,y,z triples, node-local
		bool bPathClosed = false;
		bool bPathWorld = false;         // set when hoisted off a Path3D child
		TArray<TPair<FString, FString>> Props;   // raw key -> raw value
		FString ScriptExt;               // script = ExtResource("id")
		FString InstExt;                 // instance= ext id
		// accumulated world transform (godot space)
		double WM[9] = { 1,0,0, 0,1,0, 0,0,1 };
		double WO[3] = { 0,0,0 };
	};

	TMap<FString, FString> ExtScene;    // ext id -> type name (objects/addons)
	TSet<FString> ExtStatic;            // ext ids that are res://static/ scenes
	TMap<FString, FString> ExtScript;   // ext id -> script class name
	TArray<TSharedPtr<FTNode>> Nodes;
	// sub_resource collectors: Curve3D points for waypoint paths, BoxShape3D
	// sizes for legacy volumes
	FString CurSub, CurSubType;
	TMap<FString, TArray<double>> SubCurvePts;
	TMap<FString, bool> SubCurveClosed;
	TMap<FString, FVector> SubBoxSize;
	TSharedPtr<FTNode> Cur;
	FString RootName;

	TArray<FString> Lines;
	In.ParseIntoArrayLines(Lines);
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		if (Line.StartsWith(TEXT("[ext_resource")))
		{
			const FString Id = BF6_TscnAttr(Line, TEXT("id=\""));
			const FString Path = BF6_TscnAttr(Line, TEXT("path=\""));
			const FString Kind = BF6_TscnAttr(Line, TEXT("type=\""));
			if (Id.IsEmpty() || Path.IsEmpty()) continue;
			const FString Base = FPaths::GetBaseFilename(Path);
			if (Kind == TEXT("PackedScene"))
			{
				if (Path.StartsWith(TEXT("res://static/"))) ExtStatic.Add(Id);
				else ExtScene.Add(Id, Base);
			}
			else if (Kind == TEXT("Script")) ExtScript.Add(Id, Base);
			continue;
		}
		if (Line.StartsWith(TEXT("[node")))
		{
			Cur = MakeShared<FTNode>();
			Cur->Name = BF6_TscnAttr(Line, TEXT("name=\""));
			Cur->ParentPath = BF6_TscnAttr(Line, TEXT("parent=\""));
			Cur->Type = BF6_TscnAttr(Line, TEXT("type=\""));   // engine class; replaced by ext/script below
			const int32 ii = Line.Find(TEXT("instance=ExtResource(\""));
			if (ii != INDEX_NONE)
			{
				const int32 s = ii + FCString::Strlen(TEXT("instance=ExtResource(\""));
				const int32 e2 = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, s);
				if (e2 != INDEX_NONE) Cur->InstExt = Line.Mid(s, e2 - s);
			}
			if (Cur->ParentPath.IsEmpty()) { RootName = Cur->Name; Cur->Path = TEXT("."); }
			else Cur->Path = Cur->ParentPath == TEXT(".") ? Cur->Name : Cur->ParentPath + TEXT("/") + Cur->Name;
			Nodes.Add(Cur);
			continue;
		}
		if (Line.StartsWith(TEXT("[sub_resource")))
		{
			// Curve3D carries a WaypointPath's points; BoxShape3D carries a
			// LEGACY volume's size (old community maps built volumes from
			// CollisionShape3D before the PolygonVolume type existed, and
			// gdconverter still honours them - so do we).
			Cur.Reset();
			CurSub = BF6_TscnAttr(Line, TEXT("id=\""));
			CurSubType = BF6_TscnAttr(Line, TEXT("type=\""));
			continue;
		}
		if (Line.StartsWith(TEXT("[")) ) { Cur.Reset(); CurSub.Empty(); continue; }   // connection blocks etc.
		if (!Cur.IsValid() && !CurSub.IsEmpty())
		{
			// inside a sub_resource block. Curve3D's payload sits in a
			// multi-line _data dictionary, so keys arrive as "points": ... too.
			if (CurSubType == TEXT("Curve3D"))
			{
				if (Line.Contains(TEXT("\"points\"")))
				{
					TArray<double> N;
					if (BF6_TscnNums(Line, N)) SubCurvePts.Add(CurSub, N);
				}
				else if (Line.StartsWith(TEXT("closed")) && Line.Contains(TEXT("true")))
					SubCurveClosed.Add(CurSub, true);
			}
			else if (CurSubType == TEXT("BoxShape3D") && Line.StartsWith(TEXT("size")))
			{
				TArray<double> N;
				if (BF6_TscnNums(Line, N) && N.Num() >= 3) SubBoxSize.Add(CurSub, FVector(N[0], N[1], N[2]));
			}
			continue;
		}
		if (!Cur.IsValid() || Line.IsEmpty()) continue;
		int32 Eq = Line.Find(TEXT(" = "));
		if (Eq == INDEX_NONE) continue;
		const FString Key = Line.Left(Eq).TrimStartAndEnd();
		const FString Val = Line.Mid(Eq + 3).TrimStartAndEnd();
		if (Key == TEXT("transform"))
		{
			TArray<double> N;
			if (BF6_TscnNums(Val, N) && N.Num() >= 12)
			{
				for (int32 k = 0; k < 9; k++) Cur->M[k] = N[k];
				for (int32 k = 0; k < 3; k++) Cur->O[k] = N[9 + k];
			}
		}
		else if (Key == TEXT("points"))  { BF6_TscnNums(Val, Cur->Points); }
		else if (Key == TEXT("height"))  { Cur->Height = FCString::Atod(*Val); }
		else if (Key == TEXT("curve"))   { Cur->CurveId = BF6_TscnAttr(Val, TEXT("SubResource(\"")); }
		else if (Key == TEXT("shape"))   { Cur->ShapeId = BF6_TscnAttr(Val, TEXT("SubResource(\"")); }
		// legacy CollisionPolygon3D volume: polygon + depth play the roles of
		// points + height, with the converter's own half-depth ground offset
		else if (Key == TEXT("polygon")) { BF6_TscnNums(Val, Cur->Points); }
		else if (Key == TEXT("depth"))   { Cur->Height = FCString::Atod(*Val); }
		else if (Key == TEXT("size"))
		{
			TArray<double> N;
			if (BF6_TscnNums(Val, N) && N.Num() >= 3) { Cur->bHasSize = true; Cur->Size[0] = N[0]; Cur->Size[1] = N[1]; Cur->Size[2] = N[2]; }
		}
		else if (Key == TEXT("script"))
		{
			const FString Sid = BF6_TscnAttr(Val, TEXT("ExtResource(\""));
			if (const FString* Sc = ExtScript.Find(Sid)) Cur->ScriptExt = *Sc;
		}
		else if (Key == TEXT("color"))
		{
			// Godot writes it as Color(r, g, b, a) in linear floats.
			TArray<double> C;
			if (BF6_TscnNums(Val, C) && C.Num() >= 3)
			{
				Cur->bHasColor = true;
				Cur->Color = FLinearColor((float)C[0], (float)C[1], (float)C[2],
				                          C.Num() >= 4 ? (float)C[3] : 1.f);
			}
		}
		else if (Key.StartsWith(TEXT("metadata/")) || Key == TEXT("visible")
			|| Key == TEXT("process_priority") || Key == TEXT("top_level") || Key == TEXT("physics_interpolation_mode"))
		{
			// editor-only leakage, not gameplay data
		}
		else Cur->Props.Add(TPair<FString, FString>(Key, Val));
	}
	Cur.Reset();
	if (Nodes.Num() == 0) { Notify(TEXT("No nodes found - is this a Godot scene file?")); return false; }

	// resolve each node's TYPE and static/hidden skips, and accumulate world
	// transforms (parents always precede children in a .tscn)
	TMap<FString, TSharedPtr<FTNode>> ByPath;
	FString Level;
	for (const TSharedPtr<FTNode>& N : Nodes)
	{
		ByPath.Add(N->Path, N);
		if (!N->InstExt.IsEmpty())
		{
			if (ExtStatic.Contains(N->InstExt)) N->bSkip = true;
			else if (const FString* T = ExtScene.Find(N->InstExt)) N->Type = *T;
		}
		else if (!N->ScriptExt.IsEmpty()) N->Type = N->ScriptExt;
		else N->Type.Reset();   // plain pivot (Node3D / Camera3D): transform only

		const TSharedPtr<FTNode>* Par = nullptr;
		if (N->Path != TEXT("."))
		{
			const FString PKey = (N->ParentPath.IsEmpty() || N->ParentPath == TEXT(".")) ? FString(TEXT(".")) : N->ParentPath;
			Par = ByPath.Find(PKey);
		}
		if (Par && Par->IsValid())
		{
			const double (&P)[9] = (*Par)->WM; const double (&Pl)[9] = N->M;
			double R2[9];
			for (int32 r = 0; r < 3; r++)
				for (int32 c = 0; c < 3; c++)
					R2[r * 3 + c] = P[r * 3 + 0] * Pl[0 * 3 + c] + P[r * 3 + 1] * Pl[1 * 3 + c] + P[r * 3 + 2] * Pl[2 * 3 + c];
			double O2[3];
			for (int32 r = 0; r < 3; r++)
				O2[r] = P[r * 3 + 0] * N->O[0] + P[r * 3 + 1] * N->O[1] + P[r * 3 + 2] * N->O[2] + (*Par)->WO[r];
			FMemory::Memcpy(N->WM, R2, sizeof(R2));
			FMemory::Memcpy(N->WO, O2, sizeof(O2));
			if ((*Par)->bSkip) N->bSkip = true;   // whole static subtree stays out
		}
		else
		{
			FMemory::Memcpy(N->WM, N->M, sizeof(N->M));
			FMemory::Memcpy(N->WO, N->O, sizeof(N->O));
		}
		if (N->Name.Contains(TEXT("hidden"), ESearchCase::IgnoreCase)) N->bSkip = true;   // SDK exporter convention
		if (N->Path == TEXT("Static") || N->Path.StartsWith(TEXT("Static/"))) N->bSkip = true;
	}

	// which map? the root node is named after the level; older scenes carry it
	// only in the static terrain reference
	if (RootName.StartsWith(TEXT("MP_"))) Level = RootName;
	if (Level.IsEmpty())
		for (const TSharedPtr<FTNode>& N : Nodes)
			if (N->InstExt.Len() && ExtStatic.Contains(N->InstExt) && N->Name.EndsWith(TEXT("_Terrain")))
			{ Level = N->Name.LeftChop(8); break; }
	if (Level.IsEmpty()) Level = g_ss.CurrentLevel;
	if (Level.IsEmpty()) { Notify(TEXT("Could not tell which map this scene is for.")); return false; }

	if (!GEditor) return false;
	// editable custom map named after the file; the scene is authoritative
	g_ss.CurrentLevel = Level;
	g_ss.CurrentSave = FPaths::GetBaseFilename(File);
	g_ss.bEditing = true;
	BF6_ClearContextFor(Level); ClearActorsWithTag(kPlacedTag); ClearActorsWithTag(kBaseTag); ClearActorsWithTag(kGroupTag);
	BF6_LoadPlaceables(Level); BF6_LoadBudgetMax(Level);
	const FString TP = BF6_MapMeshPath(Level, TEXT("_terrain.bf6mesh")); if (!GContextReused && FPaths::FileExists(TP)) SpawnContextMesh(TP, FString::Printf(TEXT("%s_Terrain"), *Level));
	const FString AP = BF6_MapMeshPath(Level, TEXT("_assets.bf6mesh")); if (!GContextReused && FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *Level));
	UWorld* World = GEditor->GetEditorWorldContext().World(); if (!World) return false;

	auto ToUnreal = [](double gx, double gy, double gz) { return FVector((float)gx, (float)gz, (float)gy) * 100.f; };
	auto GodotXform = [](const double (&B)[9], const double (&O)[3], double x, double y, double z)
	{
		return FVector(
			(float)(B[0] * x + B[1] * y + B[2] * z + O[0]),
			(float)(B[3] * x + B[4] * y + B[5] * z + O[1]),
			(float)(B[6] * x + B[7] * y + B[8] * z + O[2]));
	};

	// Resolve Curve3D references into node-local positions. Godot writes nine
	// numbers per curve point (in-handle, out-handle, position); only the
	// position matters to the exported path, exactly as gdconverter takes
	// elements [6:9] of each nine.
	for (const TSharedPtr<FTNode>& N : Nodes)
	{
		if (N->CurveId.IsEmpty()) continue;
		if (const TArray<double>* Raw = SubCurvePts.Find(N->CurveId))
			for (int32 i = 6; i + 2 < Raw->Num(); i += 9)
			{
				N->PathPts.Add((*Raw)[i]);
				N->PathPts.Add((*Raw)[i + 1]);
				N->PathPts.Add((*Raw)[i + 2]);
			}
		N->bPathClosed = SubCurveClosed.Contains(N->CurveId);
	}

	// A curve can also live on a Path3D CHILD of the owner (Godot lets either
	// carry it). A Path3D never becomes an actor here - it is a tree pivot -
	// so its points are hoisted onto the parent, converted to world space
	// through the CHILD's accumulated transform first, because the parent's
	// transform is not the child's.
	for (const TSharedPtr<FTNode>& N : Nodes)
	{
		if (N->PathPts.Num() < 6 || !(N->Type == TEXT("Node3D") || N->Type.IsEmpty())) continue;
		if (N->ParentPath.IsEmpty() || N->ParentPath == TEXT(".")) continue;
		const TSharedPtr<FTNode>* Par = ByPath.Find(N->ParentPath);
		if (!Par || !Par->IsValid() || (*Par)->PathPts.Num() > 0) continue;
		for (int32 i = 0; i + 2 < N->PathPts.Num(); i += 3)
		{
			const double x = N->PathPts[i], y = N->PathPts[i + 1], z = N->PathPts[i + 2];
			(*Par)->PathPts.Add(N->WM[0] * x + N->WM[1] * y + N->WM[2] * z + N->WO[0]);
			(*Par)->PathPts.Add(N->WM[3] * x + N->WM[4] * y + N->WM[5] * z + N->WO[1]);
			(*Par)->PathPts.Add(N->WM[6] * x + N->WM[7] * y + N->WM[8] * z + N->WO[2]);
		}
		(*Par)->bPathClosed = N->bPathClosed;
		(*Par)->bPathWorld = true;
		N->PathPts.Reset();
	}

	// pass A: spawn, assigning each actor a unique link name from its node name
	TMap<FString, AActor*> ActorByPath;
	TMap<FString, FString> LinkNameByPath;
	TSet<FString> UsedNames;
	int32 spawned = 0, skippedTypes = 0;
	for (const TSharedPtr<FTNode>& N : Nodes)
	{
		if (N->bSkip || N->Type.IsEmpty()) continue;
		// Engine-class nodes carry no object of their own, but they are the
		// tree: this scene is 92 of them holding 1,962 instances. Each becomes
		// a parent node at its own accumulated transform, so moving one moves
		// its children about the pivot the creator built it on, as in the SDK.
		if (N->Type == TEXT("Node3D") || N->Type == TEXT("Camera3D")
			|| N->Type == TEXT("AnimationPlayer") || N->Type == TEXT("Path3D"))
		{
			const FVector Rg((float)N->WM[0], (float)N->WM[3], (float)N->WM[6]);
			const FVector Ug((float)N->WM[1], (float)N->WM[4], (float)N->WM[7]);
			const FVector Fg((float)N->WM[2], (float)N->WM[5], (float)N->WM[8]);
			auto SwapG = [](const FVector& v) { return FVector(v.X, v.Z, v.Y); };
			const FVector NScale(FMath::Max(Rg.Size(), 0.0001f), FMath::Max(Fg.Size(), 0.0001f), FMath::Max(Ug.Size(), 0.0001f));
			const FVector NAx = SwapG(Rg).GetSafeNormal(), NAy = SwapG(Fg).GetSafeNormal(), NAz = SwapG(Ug).GetSafeNormal();
			const FTransform NXf(FMatrix(NAx, NAy, NAz, FVector::ZeroVector).Rotator(),
				ToUnreal(N->WO[0], N->WO[1], N->WO[2]), NScale);
			if (AActor* GA = BF6_SpawnTreeNode(World, N->Path, N->ParentPath, NXf))
				BF6_SetPrettyLabel(GA, N->Name);
			continue;
		}

		FString Nm = N->Name;
		for (int32 sfx = 2; UsedNames.Contains(Nm); sfx++) Nm = FString::Printf(TEXT("%s_%d"), *N->Name, sfx);
		UsedNames.Add(Nm);

		AActor* A = nullptr;
		// Legacy volumes: before PolygonVolume existed, community maps built
		// zones from CollisionPolygon3D (polygon + depth) or CollisionShape3D
		// with a BoxShape3D, each carrying the Volume script. gdconverter still
		// converts both, so an import here must too or those maps lose their
		// zones silently. The polygon flavour becomes a PolygonVolume with the
		// converter's own half-depth ground offset; the box flavour becomes an
		// OBBVolume of the shape's size.
		const bool bLegacyPoly = N->Type == TEXT("Volume") && N->Points.Num() >= 6 && N->ShapeId.IsEmpty();
		if (bLegacyPoly) N->Type = TEXT("PolygonVolume");   // one label downstream, one export path
		if (N->Type == TEXT("Volume") && !N->ShapeId.IsEmpty())
		{
			if (const FVector* BS = SubBoxSize.Find(N->ShapeId))
			{
				N->Type = TEXT("OBBVolume");
				N->bHasSize = true;
				N->Size[0] = BS->X; N->Size[1] = BS->Y; N->Size[2] = BS->Z;
			}
		}
		if ((N->Type == TEXT("PolygonVolume") && N->Points.Num() >= 6) || bLegacyPoly)
		{
			const double LegacyDrop = bLegacyPoly ? (N->Height > 0.01 ? N->Height : 1.0) * -0.5 : 0.0;
			TArray<FVector> Loop;
			for (int32 i = 0; i + 1 < N->Points.Num(); i += 2)
			{
				const FVector G = GodotXform(N->WM, N->WO, N->Points[i], LegacyDrop, N->Points[i + 1]);
				Loop.Add(ToUnreal(G.X, G.Y, G.Z));
			}
			double H = N->Height; if (H <= 0.01) H = bLegacyPoly ? 1.0 : 5.0;   // 0 = infinite, drawn at 5 m
			A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (!A) continue;
			UProceduralMeshComponent* VM = MakeProcMesh(A, TEXT("Volume"));
			// The colour goes on BEFORE the material is built, because the
			// material reads it off the actor.
			if (N->bHasColor) BF6_SetVolumeColorTag(A, N->Color);
			BuildWalls(VM, Loop, (float)H * 100.f);
			BF6_ApplyVolumeMaterial(A, VM);
			A->Tags.Add(kPlacedTag);
			A->Tags.Add(FName(*(FString(TEXT("label:")) + N->Type)));
			if (N->Height > 0.01) A->Tags.Add(FName(*FString::Printf(TEXT("p:height=%g"), N->Height)));
			GVolumeLoops.Add(A, Loop); BF6_WriteLoopTags(A);
		}
		else
		{
			// world basis columns are the godot axes; same conversion as the
			// spatial importer (right=X, up=Y, front=Z; swap to Unreal)
			const FVector Rg((float)N->WM[0], (float)N->WM[3], (float)N->WM[6]);
			const FVector Ug((float)N->WM[1], (float)N->WM[4], (float)N->WM[7]);
			const FVector Fg((float)N->WM[2], (float)N->WM[5], (float)N->WM[8]);
			auto Swap = [](const FVector& v) { return FVector(v.X, v.Z, v.Y); };
			const FVector Scale(FMath::Max(Rg.Size(), 0.0001f), FMath::Max(Fg.Size(), 0.0001f), FMath::Max(Ug.Size(), 0.0001f));
			const FVector Ax = Swap(Rg).GetSafeNormal(), Ay = Swap(Fg).GetSafeNormal(), Az = Swap(Ug).GetSafeNormal();
			const FTransform Xf(FMatrix(Ax, Ay, Az, FVector::ZeroVector).Rotator(), ToUnreal(N->WO[0], N->WO[1], N->WO[2]), Scale);
			if (N->Type == TEXT("OBBVolume"))
			{
				const FVector SzG(N->bHasSize ? N->Size[0] : 10, N->bHasSize ? N->Size[1] : 10, N->bHasSize ? N->Size[2] : 10);
				A = SpawnObbActor(World, Xf, SzG);
				if (A) { BF6_SetObbSizeTag(A, SzG); RebuildObbBox(A); }
			}
			else
			{
				const FString Mesh = BF6_ResolveMeshForType(N->Type);
				A = Mesh.IsEmpty() ? nullptr : SpawnSdkModel(Mesh, N->Type, Xf);
				if (!A)
				{
					A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
					if (!A) { skippedTypes++; continue; }
					UProceduralMeshComponent* MM = MakeProcMesh(A, TEXT("Model"));
					BuildMarker(MM);
					A->SetActorTransform(Xf);
					A->Tags.Add(kPlacedTag);
					A->Tags.Add(FName(*(FString(TEXT("label:")) + N->Type)));
					A->SetFlags(RF_Transient);
				}
			}
		}
		if (!A) continue;
		// A node that referenced a Curve3D carries a WaypointPath. The points
		// are node-local, so they go through the node's accumulated transform
		// like everything else, then into the loop registry - which makes the
		// path editable with the same dots as a zone the moment it lands.
		if (N->PathPts.Num() >= 6)
		{
			TArray<FVector> PPts;
			for (int32 i = 0; i + 2 < N->PathPts.Num(); i += 3)
			{
				const FVector G = N->bPathWorld
					? FVector(N->PathPts[i], N->PathPts[i + 1], N->PathPts[i + 2])
					: GodotXform(N->WM, N->WO, N->PathPts[i], N->PathPts[i + 1], N->PathPts[i + 2]);
				PPts.Add(ToUnreal(G.X, G.Y, G.Z));
			}
			GVolumeLoops.Add(A, PPts);
			BF6_WriteLoopTags(A);
			if (N->bPathClosed) A->Tags.Add(FName(TEXT("p:isClosed=true")));
			RebuildVolumeWalls(A, PPts);
		}
		// Remember the tree the creator actually built. ParentPath is the Godot
		// folder path ("FinalAssault/FinalArea1/.../Sidewalk"); "." means the
		// scene root, which has no folder of its own.
		A->Tags.Add(FName(*(FString(TEXT("gpath:")) + N->Path)));
		// Godot lists children in the order the scene declares them, not by name.
		// Keeping each node's position in the file is the only way to show the
		// same order back, since nothing about the object implies it.
		A->Tags.Add(FName(*FString::Printf(TEXT("gord:%d"), Nodes.IndexOfByKey(N))));
		if (!N->ParentPath.IsEmpty() && N->ParentPath != TEXT("."))
			A->Tags.Add(FName(*(FString(TEXT("gtree:")) + N->ParentPath)));
		BF6_SetPrettyLabel(A, Nm);
		A->SetFlags(RF_Transient);
		ActorByPath.Add(N->Path, A);
		// links point at what the label ACTUALLY became (the editor may uniquify)
		FString FinalNm = A->GetActorLabel(); FinalNm.RemoveFromStart(TEXT("BF6_"));
		LinkNameByPath.Add(N->Path, FinalNm);
		spawned++;
	}

	// pass B: attribute values - NodePath links become our name links, enum
	// ints become their selection strings, everything else carries over
	for (const TSharedPtr<FTNode>& N : Nodes)
	{
		AActor* const* AP2 = ActorByPath.Find(N->Path);
		if (!AP2 || !*AP2) continue;
		AActor* A = *AP2;
		for (const TPair<FString, FString>& P : N->Props)
		{
			FString V = P.Value;
			auto ResolveOne = [&](const FString& Chunk) -> FString
			{
				const FString Rel = BF6_TscnAttr(Chunk, TEXT("NodePath(\""));
				if (Rel.IsEmpty()) return FString();
				const FString* Ln = LinkNameByPath.Find(BF6_TscnResolvePath(N->Path, Rel));
				return Ln ? *Ln : FString();
			};
			if (V.StartsWith(TEXT("[")))
			{
				TArray<FString> Names;
				int32 Pos = 0;
				while ((Pos = V.Find(TEXT("NodePath(\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos)) != INDEX_NONE)
				{
					const FString One = ResolveOne(V.Mid(Pos));
					if (!One.IsEmpty()) Names.Add(One);
					Pos += 10;
				}
				if (Names.Num() == 0) continue;
				V = FString::Join(Names, TEXT(","));
			}
			else if (V.StartsWith(TEXT("NodePath(")))
			{
				V = ResolveOne(V);
				if (V.IsEmpty()) continue;
			}
			else if (V.StartsWith(TEXT("\"")) && V.EndsWith(TEXT("\"")) && V.Len() >= 2)
				V = V.Mid(1, V.Len() - 2);
			else if (V.IsNumeric())
			{
				// Godot stores selections as the enum INDEX; our attributes use
				// the option strings
				for (const BF6Api::FPropDef& D : BF6Api::PropsForType(N->Type))
					if (D.Name == P.Key && D.Type == TEXT("selection"))
					{
						const int32 Idx = FCString::Atoi(*V);
						if (D.Options.IsValidIndex(Idx)) V = D.Options[Idx];
						break;
					}
			}
			else if (V.Contains(TEXT("(")))
				continue;   // Vector3/Color/other constructors: not our attributes
			if (!V.IsEmpty()) A->Tags.Add(FName(*FString::Printf(TEXT("p:%s=%s"), *P.Key, *V)));
		}
	}

	// The tree question, asked once and then remembered. Creators arriving from
	// Godot almost always want the hierarchy they authored; new Unreal users are
	// better served by role folders. Whichever they pick sticks for every map
	// from here on, and the outliner button flips it later.
	int32 WithTree = 0;
	for (const TSharedPtr<FTNode>& N : Nodes)
		if (!N->bSkip && !N->ParentPath.IsEmpty() && N->ParentPath != TEXT(".")) WithTree++;
	bool bAsked = false;
	GConfig->GetBool(TEXT("BF6UnrealSDK"), TEXT("TreeChoiceMade"), bAsked, GEditorPerProjectIni);
	if (!bAsked && WithTree > 0)
	{
		const EAppReturnType::Type Pick = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(TEXT(
			"This scene has folders you built in Godot, and %d objects sit inside them.\n\n"
			"Keep your Godot tree exactly as you made it?\n\n"
			"Yes  -  the outliner mirrors your Godot folders.\n"
			"No   -  objects are filed automatically by what they are.\n\n"
			"Either way you can switch at any time with the outliner button."), WithTree)));
		BF6_SetKeepGodotTree(Pick == EAppReturnType::Yes);
		GConfig->SetBool(TEXT("BF6UnrealSDK"), TEXT("TreeChoiceMade"), true, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		for (TActorIterator<AActor> It(World); It; ++It) BF6_FileActor(*It);   // apply the pick now
	}
	BF6_RecomputeBudget();
	if (BF6_KeepGodotTree())
	{
		const int32 Hooked = BF6_RebuildTreeFromTags();
		if (Hooked > 0) UE_LOG(LogBF6, Display, TEXT("authored tree: %d object(s) attached to their parent"), Hooked);
	}
	Notify(FString::Printf(TEXT("Imported Godot scene '%s' onto %s: %d objects%s. Editable now."),
		*g_ss.CurrentSave, *Level, spawned,
		skippedTypes > 0 ? *FString::Printf(TEXT(" (%d had no model and were skipped)"), skippedTypes) : TEXT("")));
	return true;
}

static bool BF6_ImportSpatialDialog()
{
	if (!GEditor) return false;
	IDesktopPlatform* DP = FDesktopPlatformModule::Get(); if (!DP) { Notify(TEXT("File dialog unavailable.")); return false; }
	TArray<FString> Picked;
	const FString DefaultDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("export"));
	const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	if (!DP->OpenFileDialog(Parent, TEXT("Import a Portal map (.spatial.json or Godot .tscn)"), DefaultDir, TEXT(""),
		TEXT("Portal maps (*.spatial.json;*.tscn)|*.spatial.json;*.tscn"),
		EFileDialogFlags::None, Picked) || Picked.Num() == 0) return false;
	const FString File = Picked[0];
	// Godot scene files take the native .tscn path
	if (File.EndsWith(TEXT(".tscn"))) return BF6_ImportTscnFile(File);

	FString In; if (!FFileHelper::LoadFileToString(In, *File)) { Notify(TEXT("Could not read the file.")); return false; }
	TSharedPtr<FJsonObject> Root; TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) { Notify(TEXT("Not valid JSON.")); return false; }

	FString Level; const TArray<TSharedPtr<FJsonValue>>* StaticArr = nullptr;
	if (Root->TryGetArrayField(TEXT("Static"), StaticArr))
		for (const auto& v : *StaticArr){ const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue; FString nm; o->TryGetStringField(TEXT("type"),nm); if(nm.IsEmpty())o->TryGetStringField(TEXT("name"),nm); int32 cut; if(nm.FindLastChar('_',cut)){ Level=nm.Left(cut); break; } }
	if (Level.IsEmpty()) Level = g_ss.CurrentLevel;
	if (Level.IsEmpty()) { Notify(TEXT("Could not tell which map this file is for.")); return false; }

	const TArray<TSharedPtr<FJsonValue>>* Dyn = nullptr;
	if (!Root->TryGetArrayField(TEXT("Portal_Dynamic"), Dyn))
	{
		// a session save picked by mistake? point at the right door
		if (Root->HasField(TEXT("objects")) && Root->HasField(TEXT("level")))
			Notify(TEXT("That file is a session SAVE, not a spatial export - open it from the map's RESUME list on the map screen instead."));
		else
			Notify(TEXT("No Portal_Dynamic list - this does not look like a Portal spatial file."));
		return false;
	}

	// editable custom map; the custom name rides INSIDE our exports as Static
	// metadata (the format itself has no name field), so a round-trip keeps
	// the user's name even if the file got renamed - filename is the fallback
	FString SaveName;
	if (StaticArr)
		for (const auto& v : *StaticArr)
			if (const TSharedPtr<FJsonObject> o = v->AsObject())
				if (o->TryGetStringField(TEXT("metadata/bf6_save"), SaveName) && !SaveName.IsEmpty()) break;
	if (SaveName.IsEmpty()) SaveName = FPaths::GetBaseFilename(File).Replace(TEXT(".spatial"), TEXT(""));
	g_ss.CurrentLevel = Level; g_ss.CurrentSave = SaveName; g_ss.bEditing = true;
	BF6_ClearContextFor(Level); ClearActorsWithTag(kPlacedTag); ClearActorsWithTag(kBaseTag); ClearActorsWithTag(kGroupTag);
	BF6_LoadPlaceables(Level); BF6_LoadBudgetMax(Level);
	const FString TP = BF6_MapMeshPath(Level, TEXT("_terrain.bf6mesh")); if (!GContextReused && FPaths::FileExists(TP)) SpawnContextMesh(TP, FString::Printf(TEXT("%s_Terrain"), *Level));
	const FString AP = BF6_MapMeshPath(Level, TEXT("_assets.bf6mesh")); if (!GContextReused && FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *Level));

	UWorld* World = GEditor->GetEditorWorldContext().World(); if (!World) return false;
	auto ToUnreal = [](double gx,double gy,double gz){ return FVector((float)gx,(float)gz,(float)gy)*100.f; };
	auto Swap = [](const FVector& v){ return FVector(v.X,v.Z,v.Y); };
	auto ReadVec = [](const TSharedPtr<FJsonObject>& o, const TCHAR* key, const FVector& def){ const TSharedPtr<FJsonObject>* v=nullptr; if(!o->TryGetObjectField(key,v)||!v->IsValid()) return def; return FVector((*v)->GetNumberField(TEXT("x")),(*v)->GetNumberField(TEXT("y")),(*v)->GetNumberField(TEXT("z"))); };

	// restore every non-structural field as an editable attribute (p: tags)
	auto RestoreProps = [](AActor* A, const TSharedPtr<FJsonObject>& o)
	{
		static const TCHAR* Skip[] = { TEXT("name"), TEXT("type"), TEXT("id"), TEXT("right"), TEXT("up"), TEXT("front"), TEXT("position"), TEXT("points"), TEXT("linked") };
		for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : o->Values)
		{
			if (KV.Key.StartsWith(TEXT("metadata"))) continue;
			bool bSkip = false;
			for (const TCHAR* S : Skip) if (KV.Key == S) { bSkip = true; break; }
			if (bSkip || !KV.Value.IsValid()) continue;
			FString V;
			switch (KV.Value->Type)
			{
			case EJson::String:  V = KV.Value->AsString(); break;
			case EJson::Boolean: V = KV.Value->AsBool() ? TEXT("true") : TEXT("false"); break;
			case EJson::Number:
			{
				const double N = KV.Value->AsNumber();
				V = FMath::Frac(N) == 0.0 ? FString::Printf(TEXT("%lld"), (int64)N) : FString::SanitizeFloat(N);
				break;
			}
			case EJson::Array:
			{
				TArray<FString> Parts;
				for (const auto& av : KV.Value->AsArray()) { FString s; if (av->TryGetString(s)) Parts.Add(s); }
				V = FString::Join(Parts, TEXT(","));
				break;
			}
			default: break;
			}
			if (!V.IsEmpty()) A->Tags.Add(FName(*FString::Printf(TEXT("p:%s=%s"), *KV.Key, *V)));
		}
	};

	// Pass 0: WaypointPath entities. The format stores a path as its OWN entity
	// (points + isClosed, no transform) that the owner references by id through
	// its Waypoints field - so paths are collected first and attached when
	// their owner spawns. Left in the main loop they would masquerade as
	// volumes, because a volume is also "an entity with points".
	struct FBF6ImpPath { TArray<FVector> Pts; bool bClosed = false; };
	TMap<FString, FBF6ImpPath> ImpPaths;
	for (const auto& dv : *Dyn)
	{
		const TSharedPtr<FJsonObject> o = dv->AsObject(); if (!o.IsValid()) continue;
		FString Ty; o->TryGetStringField(TEXT("type"), Ty);
		if (Ty != TEXT("WaypointPath")) continue;
		FBF6ImpPath P;
		o->TryGetBoolField(TEXT("isClosed"), P.bClosed);
		const TArray<TSharedPtr<FJsonValue>>* pp = nullptr;
		if (o->TryGetArrayField(TEXT("points"), pp))
			for (const auto& pv : *pp)
				if (const TSharedPtr<FJsonObject> po = pv->AsObject())
					P.Pts.Add(ToUnreal(po->GetNumberField(TEXT("x")), po->GetNumberField(TEXT("y")), po->GetNumberField(TEXT("z"))));
		FString Id; o->TryGetStringField(TEXT("id"), Id);
		if (Id.IsEmpty()) o->TryGetStringField(TEXT("name"), Id);
		if (!Id.IsEmpty() && P.Pts.Num() >= 2) ImpPaths.Add(Id, MoveTemp(P));
	}

	int32 spawned = 0;
	for (const auto& dv : *Dyn)
	{
		const TSharedPtr<FJsonObject> o = dv->AsObject(); if (!o.IsValid()) continue;
		FString Type; o->TryGetStringField(TEXT("type"), Type);
		if (Type.IsEmpty() || Type.StartsWith(TEXT("MP_")) || BF6_IsEngineNodeType(Type)) continue;
		if (Type == TEXT("WaypointPath")) continue;   // collected above, attached below
		const FVector gpos = ReadVec(o, TEXT("position"), FVector::ZeroVector);
		const TArray<TSharedPtr<FJsonValue>>* pts = nullptr;
		if (o->TryGetArrayField(TEXT("points"), pts) && pts->Num() >= 3)
		{
			// the real format: global Godot {x,y,z} vectors; our older files and
			// the base setups use flat local x,z pairs - accept both
			TArray<FVector> Loop;
			if ((*pts)[0]->Type == EJson::Object)
			{
				for (const auto& pv : *pts)
				{
					const TSharedPtr<FJsonObject> po = pv->AsObject(); if (!po.IsValid()) continue;
					Loop.Add(ToUnreal(po->GetNumberField(TEXT("x")), po->GetNumberField(TEXT("y")), po->GetNumberField(TEXT("z"))));
				}
			}
			else if (pts->Num() >= 6)
			{
				for (int32 i = 0; i + 1 < pts->Num(); i += 2)
					Loop.Add(ToUnreal(gpos.X + (*pts)[i]->AsNumber(), gpos.Y, gpos.Z + (*pts)[i + 1]->AsNumber()));
			}
			if (Loop.Num() >= 3)
			{
				double H = 5.0;
				o->TryGetNumberField(TEXT("height"), H);
				AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator); if (!A) continue;
				UProceduralMeshComponent* VM = MakeProcMesh(A, TEXT("Volume"));
				BuildWalls(VM, Loop, (float)FMath::Max(H, 0.5) * 100.f);
				BF6_ApplyVolumeMaterial(A, VM);
				BF6_SetPrettyLabel(A, Type); A->Tags.Add(kPlacedTag); A->Tags.Add(FName(*(FString(TEXT("label:"))+Type))); A->SetFlags(RF_Transient);
				RestoreProps(A, o);
				GVolumeLoops.Add(A, Loop); BF6_WriteLoopTags(A);   // imported zones are point-editable too
				spawned++;
			}
			continue;
		}
		// Rotation AND scale come from the basis: most of a built map is scaled
		// primitives (walls stretched 200x, floors squashed), and the axis
		// lengths carry that scale. Dropping it rendered everything at 1x.
		const FVector Rg=ReadVec(o,TEXT("right"),FVector(1,0,0)), Ug=ReadVec(o,TEXT("up"),FVector(0,1,0)), Fg=ReadVec(o,TEXT("front"),FVector(0,0,1));
		const FVector Scale(FMath::Max(Rg.Size(), 0.0001), FMath::Max(Fg.Size(), 0.0001), FMath::Max(Ug.Size(), 0.0001));
		const FVector Ax=Swap(Rg).GetSafeNormal(), Ay=Swap(Fg).GetSafeNormal(), Az=Swap(Ug).GetSafeNormal();
		const FTransform Xf(FMatrix(Ax,Ay,Az,FVector::ZeroVector).Rotator(), ToUnreal(gpos.X,gpos.Y,gpos.Z), Scale);
		if (Type == TEXT("OBBVolume"))
		{
			FVector SzG(10, 10, 10);
			const TArray<TSharedPtr<FJsonValue>>* SA = nullptr;
			if (o->TryGetArrayField(TEXT("size"), SA) && SA->Num() == 3)
				SzG = FVector((*SA)[0]->AsNumber(), (*SA)[1]->AsNumber(), (*SA)[2]->AsNumber());
			if (AActor* A = SpawnObbActor(World, Xf, SzG)) { RestoreProps(A, o); BF6_SetObbSizeTag(A, SzG); RebuildObbBox(A); spawned++; }
			continue;
		}
		const FString Mesh = BF6_ResolveMeshForType(Type);
		AActor* A = Mesh.IsEmpty() ? nullptr : SpawnSdkModel(Mesh, Type, Xf);
		if (!A){ A=World->SpawnActor<AActor>(AActor::StaticClass(),FVector::ZeroVector,FRotator::ZeroRotator); if(!A)continue; UProceduralMeshComponent* MM=MakeProcMesh(A,TEXT("Model")); BuildMarker(MM); A->SetActorTransform(Xf); BF6_SetPrettyLabel(A, Type); A->Tags.Add(kPlacedTag); A->Tags.Add(FName(*(FString(TEXT("label:"))+Type))); A->SetFlags(RF_Transient); }
		RestoreProps(A, o);   // ObjId, teams, links - everything editable again
		// the path this entity owns, collected in pass 0
		{
			FString WId;
			if (o->TryGetStringField(TEXT("Waypoints"), WId))
				if (const FBF6ImpPath* P = ImpPaths.Find(WId))
				{
					GVolumeLoops.Add(A, P->Pts);
					BF6_WriteLoopTags(A);
					if (P->bClosed) A->Tags.Add(FName(TEXT("p:isClosed=true")));
					RebuildVolumeWalls(A, P->Pts);
				}
		}
		spawned++;
	}
	BF6_RecomputeBudget();
	const int32 Hooked = BF6_ApplyTreeMetadata(StaticArr);
	if (Hooked > 0) UE_LOG(LogBF6, Display, TEXT("authored tree: %d object(s) attached to their parent"), Hooked);
	Notify(FString::Printf(TEXT("Imported '%s' onto %s: %d objects. Editable now."), *g_ss.CurrentSave, *Level, spawned));
	return true;
}

// plugin can't hot-swap its own DLL, so the flow is the Godot staged-lane one:
// check -> download to Saved/ -> restart, and a script applies it while the
// editor is closed, then relaunches the project.
// ============================================================================
static const TCHAR* kUpdateRepoApi = TEXT("https://api.github.com/repos/TabbedScamper/BF6_Unreal_SDK/releases/latest");

static bool BF6_ParseSemver(const FString& In, int32& A, int32& B, int32& C)
{
	FString S = In.TrimStartAndEnd();
	if (S.StartsWith(TEXT("v")) || S.StartsWith(TEXT("V"))) S = S.RightChop(1);
	TArray<FString> Parts;
	S.ParseIntoArray(Parts, TEXT("."));
	if (Parts.Num() < 3) return false;
	A = FCString::Atoi(*Parts[0]); B = FCString::Atoi(*Parts[1]); C = FCString::Atoi(*Parts[2]);
	return true;
}

static bool BF6_IsNewer(const FString& Remote, const FString& Local)
{
	int32 ra, rb, rc, la, lb, lc;
	if (!BF6_ParseSemver(Remote, ra, rb, rc) || !BF6_ParseSemver(Local, la, lb, lc)) return false;
	if (ra != la) return ra > la;
	if (rb != lb) return rb > lb;
	return rc > lc;
}

// Stage the downloaded zip, write the apply script, close the editor. The
// script waits for us to exit, unzips over the plugin, and relaunches.
static void BF6_StageUpdateAndRestart(const TArray<uint8>& ZipBytes, const FString& Tag)
{
	// EVERY path handed to the applier must be absolute and Windows-native.
	// FPaths::ProjectSavedDir() is RELATIVE (resolved against the engine base
	// dir, not the child process's working directory); it only ever worked by
	// the accident of excess ".." clamping at the drive root, and breaks flat
	// out when the project and the engine live on different drives.
	auto Win = [](const FString& P)
	{
		FString S = FPaths::ConvertRelativePathToFull(P);
		S.ReplaceInline(TEXT("/"), TEXT("\\"));
		return S;
	};
	const FString UpdateDir = Win(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("update")));
	const FString ZipPath   = Win(FPaths::Combine(UpdateDir, TEXT("plugin_update.zip")));
	const FString Staging   = Win(FPaths::Combine(UpdateDir, TEXT("staging")));
	const FString Script    = Win(FPaths::Combine(UpdateDir, TEXT("apply_update.ps1")));
	const FString PluginDir = Win(g_pluginDir);
	const FString Project   = Win(FPaths::ProjectDir() / FApp::GetProjectName()) + TEXT(".uproject");
	IFileManager::Get().MakeDirectory(*UpdateDir, true);
	if (!FFileHelper::SaveArrayToFile(ZipBytes, *ZipPath)) { Notify(TEXT("Could not write the update file.")); return; }

	const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
	const FString ApplyLog  = FPaths::Combine(UpdateDir, TEXT("apply_update.log"));
	const FString EditorExe = FPlatformProcess::ExecutablePath();

	// The apply script logs every step to apply_update.log so a failed update
	// on a tester's machine is diagnosable, and it ALWAYS relaunches the editor
	// at the end - the pending.txt verdict on next launch reports the outcome.
	FString Ps;
	Ps += FString::Printf(TEXT("$log = \"%s\"\r\n"), *ApplyLog);
	Ps += TEXT("function Log($m) { Add-Content -Path $log -Value ((Get-Date -Format s) + '  ' + $m) }\r\n");
	// If the breadcrumb cannot be written, STOP. The editor reads a missing
	// breadcrumb as "the applier never started" and stays open, so the applier
	// must agree rather than lurk and apply an update later behind its back.
	Ps += FString::Printf(TEXT("try { Set-Content -Path $log -Value ((Get-Date -Format s) + '  applying update %s') -ErrorAction Stop } catch { exit 1 }\r\n"), *Tag);
	Ps += TEXT("try {\r\n");
	Ps += FString::Printf(TEXT("  try { Wait-Process -Id %u -ErrorAction SilentlyContinue } catch {}\r\n"), Pid);
	Ps += TEXT("  Start-Sleep -Seconds 2\r\n");
	Ps += TEXT("  Log 'editor closed'\r\n");
	Ps += FString::Printf(TEXT("  Remove-Item -Recurse -Force \"%s\" -ErrorAction SilentlyContinue\r\n"), *Staging);
	Ps += TEXT("  try {\r\n");
	Ps += FString::Printf(TEXT("    Expand-Archive -Path \"%s\" -DestinationPath \"%s\" -Force -ErrorAction Stop\r\n"), *ZipPath, *Staging);
	Ps += TEXT("    Log 'unzipped (Expand-Archive)'\r\n");
	Ps += TEXT("  } catch {\r\n");
	Ps += TEXT("    Log ('Expand-Archive failed: ' + $_.Exception.Message)\r\n");
	Ps += TEXT("    Add-Type -AssemblyName System.IO.Compression.FileSystem\r\n");
	Ps += FString::Printf(TEXT("    [System.IO.Compression.ZipFile]::ExtractToDirectory(\"%s\", \"%s\")\r\n"), *ZipPath, *Staging);
	Ps += TEXT("    Log 'unzipped (ZipFile fallback)'\r\n");
	Ps += TEXT("  }\r\n");
	// the zip may carry a BF6UnrealSDK/ root folder or the plugin files directly
	Ps += FString::Printf(TEXT("  $src = Join-Path \"%s\" \"BF6UnrealSDK\"\r\n"), *Staging);
	Ps += TEXT("  if (-not (Test-Path $src)) { $src = \"") + Staging + TEXT("\" }\r\n");
	// bounded retries: robocopy's default is a million 30s retries on a locked
	// file, which reads as "the update silently did nothing" to the user
	Ps += FString::Printf(TEXT("  robocopy $src \"%s\" /E /R:5 /W:2 /NFL /NDL /NJH /NJS | Out-Null\r\n"), *PluginDir);
	Ps += TEXT("  Log ('robocopy exit ' + $LASTEXITCODE)\r\n");
	Ps += FString::Printf(TEXT("  if ((Get-Content \"%s\" -Raw) -match '\"VersionName\"\\s*:\\s*\"([^\"]+)\"') { Log ('plugin is now v' + $Matches[1]) }\r\n"),
		*FPaths::Combine(PluginDir, TEXT("BF6UnrealSDK.uplugin")));
	Ps += TEXT("} catch {\r\n");
	Ps += TEXT("  Log ('APPLY FAILED: ' + $_.Exception.Message)\r\n");
	Ps += TEXT("}\r\n");
	Ps += TEXT("Log 'relaunching the editor'\r\n");
	// launch the editor binary directly: Start-Process on the .uproject relies
	// on a file association that is missing on some machines
	Ps += FString::Printf(TEXT("Start-Process \"%s\" -ArgumentList '\"%s\"'\r\n"), *EditorExe, *Project);
	Ps += TEXT("Log 'done'\r\n");
	IFileManager::Get().Delete(*ApplyLog, false, true, true);   // the breadcrumb waited on below
	if (!FFileHelper::SaveStringToFile(Ps, *Script))
	{
		Notify(TEXT("Could not write the update script - nothing was changed."));
		return;
	}

	// This MUST NOT be launched detached with no handles: powershell is a console
	// program, and DETACHED_PROCESS with no console and no pipes leaves its host
	// without standard handles, so it died before running a single line. The
	// editor then closed for an update that never happened and never came back -
	// the "press yes, nothing changes" loop. (The SDK's tar/curl calls get away
	// with detached only because they are handed a pipe.)
	uint32 ChildPid = 0;
	FProcHandle Proc = FPlatformProcess::CreateProc(TEXT("powershell.exe"),
		*FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\""), *Script),
		false, true, true, &ChildPid, 0, *UpdateDir, nullptr);

	// Never take the editor down on faith. The applier's first act is writing its
	// log, so wait for real bytes to land: no breadcrumb means it never ran, and
	// closing then stranded the user in the loop this guard exists to prevent.
	// (FPaths::FileExists is TRUE for a directory, so size is what gets checked.)
	bool bStarted = false;
	if (Proc.IsValid())
		for (int32 i = 0; i < 60 && !bStarted; i++)   // up to ~6 seconds
		{
			FPlatformProcess::Sleep(0.1f);
			bStarted = IFileManager::Get().FileSize(*ApplyLog) > 0;
		}
	if (!bStarted)
	{
		if (Proc.IsValid()) { FPlatformProcess::TerminateProc(Proc); FPlatformProcess::CloseProc(Proc); }
		UE_LOG(LogBF6, Error, TEXT("Update %s: the applier never started (script %s)."), *Tag, *Script);
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT(
			"The updater could not start, so nothing was changed and the editor is staying open.\n\n"
			"%s is already downloaded. To finish by hand: close the editor, then unzip\n%s\nover\n%s\n\n"
			"(The BF6UnrealSDK folder inside the zip replaces the one in Plugins.)"),
			*Tag, *ZipPath, *FPaths::GetPath(PluginDir))));
		return;   // editor stays up: no silent close, no loop
	}
	FPlatformProcess::CloseProc(Proc);

	// Marker for the next launch: if the plugin version then matches this tag
	// the update applied; if not, the apply failed and the user is told so.
	FFileHelper::SaveStringToFile(Tag, *FPaths::Combine(UpdateDir, TEXT("pending.txt")));
	UE_LOG(LogBF6, Warning, TEXT("Update %s staged (applier pid %u) - closing the editor to apply."), *Tag, ChildPid);
	FPlatformMisc::RequestExit(false);
}

// The persistent "downloading..." toast, kept alive so its text can track
// download progress. Reset when the download ends either way.
static TSharedPtr<SNotificationItem> GUpdateToast;

static void BF6_DownloadUpdate(const FString& Url, const FString& Tag, uint64 ExpectedBytes)
{
	FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("Downloading update %s..."), *Tag)));
	Info.bFireAndForget = false;
	Info.bUseThrobber = true;
	GUpdateToast = FSlateNotificationManager::Get().AddNotification(Info);
	if (GUpdateToast.IsValid()) GUpdateToast->SetCompletionState(SNotificationItem::CS_Pending);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("User-Agent"), TEXT("BF6UnrealSDK"));
	Req->OnRequestProgress64().BindLambda([Tag, ExpectedBytes](FHttpRequestPtr, uint64, uint64 Received)
	{
		if (!GUpdateToast.IsValid()) return;
		const double GotMB = double(Received) / (1024.0 * 1024.0);
		GUpdateToast->SetText(FText::FromString(ExpectedBytes > 0
			? FString::Printf(TEXT("Downloading update %s... %d%% (%.1f / %.1f MB)"),
				*Tag, int32(Received * 100 / ExpectedBytes), GotMB, double(ExpectedBytes) / (1024.0 * 1024.0))
			: FString::Printf(TEXT("Downloading update %s... %.1f MB"), *Tag, GotMB)));
	});
	Req->OnProcessRequestComplete().BindLambda([Tag](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
	{
		const bool bGood = bOk && Resp.IsValid() && Resp->GetResponseCode() == 200;
		if (GUpdateToast.IsValid())
		{
			GUpdateToast->SetText(FText::FromString(bGood
				? FString::Printf(TEXT("Update %s downloaded. The editor will now close and reopen itself."), *Tag)
				: FString(TEXT("Update download failed - try again later."))));
			GUpdateToast->SetCompletionState(bGood ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
			GUpdateToast->ExpireAndFadeout();
			GUpdateToast.Reset();
		}
		if (bGood) BF6_StageUpdateAndRestart(Resp->GetContent(), Tag);
	});
	Req->ProcessRequest();
}

// First launch after an update restart: compare the marker's tag against the
// running plugin version, so the user knows whether the update actually
// applied instead of having to find the version label themselves.
static void BF6_ReportUpdateOutcome()
{
	const FString Marker = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("update"), TEXT("pending.txt"));
	FString Tag;
	if (!FFileHelper::LoadFileToString(Tag, *Marker)) return;
	IFileManager::Get().Delete(*Marker);
	Tag.TrimStartAndEndInline();
	FString Expected = Tag; Expected.RemoveFromStart(TEXT("v"));
	const FString Local = BF6Api::PluginVersion();
	if (Local == Expected)
	{
		FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("Updated to v%s."), *Local)));
		Info.ExpireDuration = 6.0f;
		TSharedPtr<SNotificationItem> N = FSlateNotificationManager::Get().AddNotification(Info);
		if (N.IsValid()) N->SetCompletionState(SNotificationItem::CS_Success);
	}
	else
	{
		FNotificationInfo Info(FText::FromString(FString::Printf(
			TEXT("The update to %s did not apply. You are still on v%s. Close the editor and unzip the plugin package from github.com/TabbedScamper/BF6_Unreal_SDK/releases over Plugins/BF6UnrealSDK. When reporting this, attach Saved/BF6UnrealSDK/update/apply_update.log."),
			*Tag, *Local)));
		Info.ExpireDuration = 20.0f;
		TSharedPtr<SNotificationItem> N = FSlateNotificationManager::Get().AddNotification(Info);
		if (N.IsValid()) N->SetCompletionState(SNotificationItem::CS_Fail);
	}
}

// defined after the namespace (it's the post-undo geometry resync); the focus
// edit's revert path reuses it to rebuild zone walls from restored tags
static void BF6_RepairAfterUndo();
static void BF6_RebuildActorGeometry(AActor* A);   // refill one mesh from disk/cache

// ---- BF6Api: data + action surface consumed by the Build Mode widgets ----
namespace BF6Api
{
	FString PluginVersion()
	{
		if (TSharedPtr<IPlugin> P = IPluginManager::Get().FindPlugin(TEXT("BF6UnrealSDK")))
			return P->GetDescriptor().VersionName;
		return TEXT("0.0.0");
	}

	bool IsDataInstalled()
	{
		return FPaths::FileExists(BF6_DataDir() / TEXT("FbExportData/asset_types.json"))
			&& BF6_CountFiles(BF6_DataDir() / TEXT("objmodels"), TEXT("*.bf6mesh")) >= 1000;
	}

	bool ValidateSdkRoot(const FString& Path, FString& OutError)
	{
		const FString P = Path.TrimStartAndEnd();
		if (P.IsEmpty()) { OutError = TEXT("Pick your unzipped Portal SDK folder."); return false; }
		if (!FPaths::FileExists(P / TEXT("GodotProject/project.godot"))) { OutError = TEXT("That folder has no GodotProject - select the SDK's root folder (the one with GodotProject and FbExportData inside)."); return false; }
		if (!FPaths::FileExists(P / TEXT("FbExportData/asset_types.json"))) { OutError = TEXT("FbExportData/asset_types.json is missing - is this a complete SDK download?"); return false; }
		if (BF6_FindSdkGodot(P).IsEmpty()) { OutError = TEXT("The SDK's Godot exe was not found at the folder root."); return false; }
		if (BF6_CountFiles(P / TEXT("GodotProject/.godot/imported"), TEXT("*.scn")) < 100) { OutError = TEXT("The SDK's imported model cache (.godot/imported) looks empty - unzip the full SDK download."); return false; }

#if PLATFORM_WINDOWS
		// Cloud-synced drives (Google Drive letters, OneDrive/Dropbox "online
		// only") LIST files fine but hand Godot empty placeholders when read.
		// Network drives are slow and flaky for Godot. Catch both up front.
		{
			TArray<FString> Sample;
			IFileManager::Get().FindFiles(Sample, *(P / TEXT("GodotProject/.godot/imported/*.scn")), true, false);
			if (Sample.Num())
			{
				const uint32 Attr = GetFileAttributesW(*(P / TEXT("GodotProject/.godot/imported") / Sample[0]));
				const uint32 kCloud = 0x00001000 /*OFFLINE*/ | 0x00400000 /*RECALL_ON_DATA_ACCESS*/ | 0x00040000 /*RECALL_ON_OPEN*/;
				if (Attr != INDEX_NONE && (Attr & kCloud))
				{
					OutError = TEXT("This SDK folder lives on a cloud-synced drive and its files are online-only placeholders. Right-click the SDK folder and choose 'Always keep on this device' (or move it to a plain local folder like C:\\PortalSDK), then try again.");
					return false;
				}
			}
			if (P.Len() >= 2 && (P.StartsWith(TEXT("\\\\")) || GetDriveTypeW(*(P.Left(2) + TEXT("\\"))) == 4 /*DRIVE_REMOTE*/))
				Notify(TEXT("Heads up: the SDK is on a network drive. The import may be very slow or fail - a local folder is safer."));
		}
#endif
		OutError.Reset();
		return true;
	}

	static void BF6_GenerateSdkChanges(const FString& NewRoot);   // defined with the history section below
	// non-static: BF6SdkImport.cpp calls it at the end of an import
	void BF6_SnapshotSdkHistory(const FString& SdkRoot);

	void StartSdkImport(const FString& SdkRoot, bool bFullResync)
	{
		if (IsImporting()) return;
		FString Err;
		if (!ValidateSdkRoot(SdkRoot, Err)) { Notify(Err); return; }
		// SDK version history: diff the NEW SDK against the previous
		// snapshot BEFORE anything gets overwritten
		BF6_GenerateSdkChanges(SdkRoot);

		if (bFullResync)
		{
			// wipe the converted data so CHANGED SDK content reconverts too
			IFileManager::Get().DeleteDirectory(*(BF6_DataDir() / TEXT("objmodels")), false, true);
			IFileManager::Get().DeleteDirectory(*(BF6_DataDir() / TEXT("mapmesh")),   false, true);
			IFileManager::Get().DeleteDirectory(*(BF6_DataDir() / TEXT("basesetup")), false, true);
		}

		g_imp = FBF6Import();
		g_imp.SdkRoot  = SdkRoot.TrimStartAndEnd();
		g_imp.GodotExe = BF6_FindSdkGodot(g_imp.SdkRoot);

		// remember for re-sync checks
		GConfig->SetString(TEXT("BF6UnrealSDK"), TEXT("SdkRoot"), *g_imp.SdkRoot, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);

		// phase 0 (fast, synchronous): catalogue + version stamp + base setups
		g_imp.Status = TEXT("Copying catalogue...");
		const FString Fb = BF6_DataDir() / TEXT("FbExportData");
		IFileManager::Get().MakeDirectory(*Fb, true);
		IFileManager::Get().Copy(*(Fb / TEXT("asset_types.json")), *(g_imp.SdkRoot / TEXT("FbExportData/asset_types.json")));
		IFileManager::Get().Copy(*(Fb / TEXT("level_info.json")),  *(g_imp.SdkRoot / TEXT("FbExportData/level_info.json")));
		// (sdk.version.json is stamped only when the whole import SUCCEEDS)
		BF6_ExtractBaseSetups(g_imp.SdkRoot);
		FFileHelper::SaveStringToFile(TEXT("2"), *(BF6_DataDir() / TEXT("basesetup") / TEXT(".format2")));
		if (g_ctx && g_loadp)   // refresh the placeable catalogue from the new jsons
		{
			char perr[256] = {0};
			g_loadp(g_ctx, TCHAR_TO_UTF8(*Fb), perr, sizeof(perr));
		}

		// write the extraction scripts + count the work
		const FString ScriptDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("import"));
		IFileManager::Get().MakeDirectory(*ScriptDir, true);
		// FORCE UTF-8: with any non-ASCII character in the embedded paths the
		// default save picks UTF-16, which Godot cannot parse - the whole
		// conversion then dies instantly with a script parse error
		// the scripts' output dir must be ABSOLUTE too - Godot's working
		// directory is not ours, so a relative OUT would land elsewhere.
		// One script per parallel worker, each owning a hash shard.
		for (int32 s = 0; s < kBF6ObjShards; s++)
			FFileHelper::SaveStringToFile(BF6_ObjectScript(FPaths::ConvertRelativePathToFull(BF6_DataDir() / TEXT("objmodels")), s, kBF6ObjShards),
				*(ScriptDir / FString::Printf(TEXT("extract_objects_%d.gd"), s)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		for (int32 s = 0; s < kBF6MapShards; s++)
			FFileHelper::SaveStringToFile(BF6_MapScript(FPaths::ConvertRelativePathToFull(BF6_DataDir() / TEXT("mapmesh")), s, kBF6MapShards),
				*(ScriptDir / FString::Printf(TEXT("extract_maps_%d.gd"), s)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		g_imp.ObjTotal = BF6_CountFiles(g_imp.SdkRoot / TEXT("GodotProject/raw/models"), TEXT("*.glb.import"));
		g_imp.MapTotal = 2 * BF6_CountFiles(g_imp.SdkRoot / TEXT("GodotProject/static"), TEXT("*_Terrain.tscn"));
		IFileManager::Get().MakeDirectory(*(BF6_DataDir() / TEXT("objmodels")), true);
		IFileManager::Get().MakeDirectory(*(BF6_DataDir() / TEXT("mapmesh")), true);

		// start a fresh diagnostics log for this import run
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("SDK import from %s%s"), *g_imp.SdkRoot, LINE_TERMINATOR), *BF6_ImportLogPath());
		// diagnostic context only: verified 2026-08-19 that the SDK's Godot
		// handles 280+ char paths fine even with Windows long paths DISABLED,
		// so path depth is never the cause of a failed conversion
		BF6_ImportLog(FString::Printf(TEXT("path lengths: sdk root=%d chars, output dir=%d chars"), g_imp.SdkRoot.Len(), (BF6_DataDir() / TEXT("objmodels")).Len()));

		// phase 1: object models via the SDK's own Godot, headless
		g_imp.Phase = FBF6Import::EPhase::Objects;
		g_imp.LastCount = BF6_CountFiles(BF6_DataDir() / TEXT("objmodels"), TEXT("*.bf6mesh"));
		g_imp.StartCount = g_imp.LastCount;
		if (!BF6_LaunchGodotWorkers(true)) { BF6_ImportFail(TEXT("could not launch the SDK's Godot")); return; }
		g_imp.Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			BF6_ImportTickPhase();
			return g_imp.Phase == FBF6Import::EPhase::Objects || g_imp.Phase == FBF6Import::EPhase::Maps;
		}), 1.0f);
	}

	bool  IsImporting() { return g_imp.Phase == FBF6Import::EPhase::Objects || g_imp.Phase == FBF6Import::EPhase::Maps; }
	FText ImportStatus() { return FText::FromString(g_imp.Status); }
	float ImportFrac() { return g_imp.Frac; }
	bool  ImportDone() { return g_imp.Phase == FBF6Import::EPhase::Done; }
	bool  ImportFailed() { return g_imp.Phase == FBF6Import::EPhase::Failed; }
	FString StoredSdkRoot()
	{
		FString P;
		GConfig->GetString(TEXT("BF6UnrealSDK"), TEXT("SdkRoot"), P, GEditorPerProjectIni);
		return P;
	}

	// ---- managed SDK lifecycle ----
	// The tool owns the Portal SDK like a launcher owns its patches: first run
	// can download the newest SDK automatically, and on later launches a newer
	// SDK is offered as a one-click update. Nobody hand-manages a 3 GB zip.
	// Saves/blocks/exports live in Saved/BF6UnrealSDK and never move; an
	// update just re-points the SDK and re-syncs the data.
	//
	// Detection and download go to EA's OFFICIAL Portal download service
	// first (the same versions.json + PortalSDK.zip the Portal site serves);
	// the community archive at hoard.bfportal.gg is the fallback and also
	// keeps every historical build. Two CDN quirks handled below: EA answers
	// "Unauthorized" to non-browser user agents, and the official manifest's
	// fileSize string drifts a few bytes from the zip's real Content-Length,
	// so the zip HEAD is the authoritative size.
	static const TCHAR* kOfficialIndex = TEXT("https://download.portal.battlefield.com/versions.json");
	static const TCHAR* kOfficialZip   = TEXT("https://download.portal.battlefield.com/PortalSDK.zip");
	static const TCHAR* kHoardIndex    = TEXT("https://hoard.bfportal.gg/versions.json");
	static const TCHAR* kBF6UA         = TEXT("Mozilla/5.0 (Windows NT 10.0; Win64; x64) BF6UnrealSDK");

	struct FBF6SdkFetch
	{
		enum class EPhase { Idle, Index, Download, Extract, Failed };
		EPhase  Phase = EPhase::Idle;
		FString Version;
		int64   Bytes = 0;
		FString Url, Source;             // where the zip comes from (for status)
		FString ZipPath, DestDir, OldRoot;
		FProcHandle Proc;
		FString Status;
		float   Frac = 0.f;
		double  PhaseStart = 0.0;
		FTSTicker::FDelegateHandle Tick;
		// unpack progress: tar -v lists each entry; total from the zip itself.
		// Extraction runs as FOUR parallel tar workers on disjoint subtrees -
		// zip inflate is single-threaded per process, and one worker took ~4x
		// as long on the 66k-entry SDK.
		TArray<FProcHandle> XProcs;
		TArray<void*> XPipeRead;
		TArray<void*> XPipeWrite;
		int64   EntriesTotal = 0;
		int64   EntriesDone = 0;
	};
	static FBF6SdkFetch g_sdkf;

	// Total entry count from the zip's end-of-central-directory record (the
	// zip64 variant when the archive holds >65535 entries) - the denominator
	// for a real unpack progress bar.
	static int64 BF6_ZipEntryCount(const FString& ZipPath)
	{
		const int64 Size = IFileManager::Get().FileSize(*ZipPath);
		if (Size <= 22) return 0;
		const int64 Want = FMath::Min<int64>(Size, 130 * 1024);
		TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*ZipPath));
		if (!Ar) return 0;
		TArray<uint8> Tail;
		Tail.SetNumUninitialized((int32)Want);
		Ar->Seek(Size - Want);
		Ar->Serialize(Tail.GetData(), Want);
		auto U16 = [&](int64 i){ return (int64)Tail[i] | ((int64)Tail[i + 1] << 8); };
		auto U32 = [&](int64 i){ return (int64)Tail[i] | ((int64)Tail[i + 1] << 8) | ((int64)Tail[i + 2] << 16) | ((int64)Tail[i + 3] << 24); };
		auto U64 = [&](int64 i){ int64 v = 0; for (int32 k = 7; k >= 0; k--) v = (v << 8) | (int64)Tail[i + k]; return v; };
		for (int64 i = Want - 22; i >= 0; i--)
		{
			if (U32(i) != 0x06054b50) continue;   // EOCD signature
			int64 N = U16(i + 10);                 // total entries
			if (N == 0xFFFF)
			{
				// zip64: the EOCD64 record sits just before the locator
				for (int64 j = i - 20; j >= 40; j--)
					if (U32(j) == 0x06064b50) { N = U64(j + 32); break; }
			}
			return N;
		}
		return 0;
	}

	bool  IsSdkFetching() { return g_sdkf.Phase == FBF6SdkFetch::EPhase::Index || g_sdkf.Phase == FBF6SdkFetch::EPhase::Download || g_sdkf.Phase == FBF6SdkFetch::EPhase::Extract; }
	bool  SdkFetchFailed() { return g_sdkf.Phase == FBF6SdkFetch::EPhase::Failed; }
	float SdkFetchFrac() { return g_sdkf.Frac; }
	FText SdkFetchStatus() { return FText::FromString(g_sdkf.Status); }

	static FString BF6_ManagedSdkBase() { return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("sdk")); }
	static FString BF6_SysTool(const TCHAR* Exe)
	{
		const FString Root = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot"));
		return (Root.IsEmpty() ? FString(TEXT("C:\\Windows")) : Root) / TEXT("System32") / Exe;
	}

	// the extracted zip may or may not wrap everything in one folder
	static FString BF6_FindSdkRootIn(const FString& Dir)
	{
		FString Err;
		if (ValidateSdkRoot(Dir, Err)) return Dir;
		TArray<FString> Subs;
		IFileManager::Get().FindFiles(Subs, *(Dir / TEXT("*")), false, true);
		for (const FString& S : Subs)
			if (ValidateSdkRoot(Dir / S, Err)) return Dir / S;
		return FString();
	}

	// where everything lands, shown in the setup UI so nobody installs blind
	FString ManagedSdkDir() { return FPaths::ConvertRelativePathToFull(BF6_ManagedSdkBase()); }
	void OpenManagedSdkDir()
	{
		IFileManager::Get().MakeDirectory(*BF6_ManagedSdkBase(), true);
		BF6_OpenInExplorer(ManagedSdkDir(), false);
	}

	// Manual fallback for a failed download: the user unzips the SDK into the
	// managed folder themselves, clicks the check button, and this verifies
	// it really is a complete SDK (same validator as everything else - Godot
	// exe, catalogue, model cache, cloud-drive traps) before importing.
	bool CheckManualSdkDrop(FString& OutMsg)
	{
		IFileManager::Get().MakeDirectory(*BF6_ManagedSdkBase(), true);
		const FString Root = BF6_FindSdkRootIn(BF6_ManagedSdkBase());
		if (Root.IsEmpty())
		{
			// surface the SPECIFIC problem when a candidate folder exists
			TArray<FString> Subs;
			IFileManager::Get().FindFiles(Subs, *(BF6_ManagedSdkBase() / TEXT("*")), false, true);
			for (const FString& S : Subs)
				if (FPaths::FileExists(BF6_ManagedSdkBase() / S / TEXT("GodotProject/project.godot")))
				{
					FString Err;
					ValidateSdkRoot(BF6_ManagedSdkBase() / S, Err);
					OutMsg = Err;
					return false;
				}
			OutMsg = FString::Printf(TEXT("No SDK found in %s. Unzip the whole PortalSDK download there, so a folder with GodotProject inside it sits in that location."), *ManagedSdkDir());
			return false;
		}
		g_sdkf.Phase = FBF6SdkFetch::EPhase::Idle;   // clear the failed banner
		g_sdkf.Status.Reset();
		GConfig->SetString(TEXT("BF6UnrealSDK"), TEXT("SdkRoot"), *Root, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		StartSdkImport(Root);
		OutMsg = TEXT("SDK found and verified - importing now.");
		return true;
	}

	static void BF6_SdkFetchFail(const FString& Why)
	{
		g_sdkf.Phase = FBF6SdkFetch::EPhase::Failed;
		g_sdkf.Status = Why;
		if (g_sdkf.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(g_sdkf.Tick); g_sdkf.Tick.Reset(); }
	}

	static void BF6_SdkFetchFinish()
	{
		const FString Root = BF6_FindSdkRootIn(g_sdkf.DestDir);
		if (Root.IsEmpty()) { BF6_SdkFetchFail(TEXT("Unpacked, but the folder does not look like a Portal SDK. The archive layout may have changed.")); return; }
		IFileManager::Get().Delete(*g_sdkf.ZipPath, false, false, true);
		GConfig->SetString(TEXT("BF6UnrealSDK"), TEXT("SdkRoot"), *Root, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		g_sdkf.Phase = FBF6SdkFetch::EPhase::Idle;
		g_sdkf.Status.Reset();
		if (g_sdkf.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(g_sdkf.Tick); g_sdkf.Tick.Reset(); }

		// an older MANAGED copy (never the user's own folder) can go now - the
		// new SDK is live and each copy is ~3 GB
		const FString Old = g_sdkf.OldRoot;
		if (!Old.IsEmpty() && Old != Root && FPaths::IsUnderDirectory(Old, BF6_ManagedSdkBase()) && FPaths::DirectoryExists(Old))
		{
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(
				TEXT("Portal SDK %s is set up. Remove the old downloaded copy at\n%s\nto free about 3 GB?"), *g_sdkf.Version, *Old))) == EAppReturnType::Yes)
			{
				Async(EAsyncExecution::Thread, [Old]{ IFileManager::Get().DeleteDirectory(*Old, false, true); });
				Notify(TEXT("Removing the old SDK copy in the background."));
			}
		}
		// straight into the data import - only changed content converts
		StartSdkImport(Root);
	}

	static bool BF6_SdkFetchTickFn(float)
	{
		if (g_sdkf.Phase == FBF6SdkFetch::EPhase::Download)
		{
			int64 Have = IFileManager::Get().FileSize(*g_sdkf.ZipPath);
			if (Have < 0) Have = 0;
			g_sdkf.Frac = g_sdkf.Bytes > 0 ? 0.05f + 0.80f * (float)((double)Have / (double)g_sdkf.Bytes) : 0.05f;
			g_sdkf.Status = FString::Printf(TEXT("Downloading Portal SDK %s from %s - %lld of %lld MB. Safe to leave running; it resumes if interrupted."),
				*g_sdkf.Version, *g_sdkf.Source, Have / (1024 * 1024), g_sdkf.Bytes / (1024 * 1024));
			if (!FPlatformProcess::IsProcRunning(g_sdkf.Proc))
			{
				int32 Rc = -1;
				FPlatformProcess::GetProcReturnCode(g_sdkf.Proc, &Rc);
				FPlatformProcess::CloseProc(g_sdkf.Proc);
				Have = IFileManager::Get().FileSize(*g_sdkf.ZipPath);
				if (Rc != 0 || Have != g_sdkf.Bytes)
				{
					BF6_SdkFetchFail(FString::Printf(TEXT("Download stopped at %lld of %lld MB (curl exit %d). Click the download button again to resume from there."),
						FMath::Max<int64>(Have, 0) / (1024 * 1024), g_sdkf.Bytes / (1024 * 1024), Rc));
					return false;
				}
				// unpack with the tar Windows ships (reads zip, much faster than
				// Expand-Archive on a 3 GB file). -v lists each entry through a
				// pipe, and the zip's own directory gives the total - a real
				// progress bar instead of a guess. FOUR workers on disjoint
				// subtrees run in parallel (inflate is single-threaded per
				// process); the catch-all worker is the only one whose exit
				// code gates success, so a future zip layout just means it
				// does all the work alone and the SDK validator still checks
				// the result.
				IFileManager::Get().MakeDirectory(*g_sdkf.DestDir, true);
				g_sdkf.EntriesTotal = BF6_ZipEntryCount(g_sdkf.ZipPath);
				g_sdkf.EntriesDone = 0;
				const TCHAR* Sets[4] = {
					TEXT("\"GodotProject/.godot\""),
					TEXT("\"GodotProject/raw\" \"FbExportData\""),
					TEXT("\"GodotProject/scripts\" \"GodotProject/objects\""),
					TEXT("--exclude \"GodotProject/.godot/*\" --exclude \"GodotProject/raw/*\" --exclude \"FbExportData/*\" --exclude \"GodotProject/scripts/*\" --exclude \"GodotProject/objects/*\"")
				};
				for (int32 wi = 0; wi < 4; wi++)
				{
					void* RP = nullptr; void* WP = nullptr;
					FPlatformProcess::CreatePipe(RP, WP);
					FProcHandle H = FPlatformProcess::CreateProc(*BF6_SysTool(TEXT("tar.exe")),
						*FString::Printf(TEXT("-xvf \"%s\" -C \"%s\" %s"), *g_sdkf.ZipPath, *g_sdkf.DestDir, Sets[wi]),
						true, true, true, nullptr, 0, nullptr, WP);
					if (!H.IsValid()) { FPlatformProcess::ClosePipe(RP, WP); continue; }
					g_sdkf.XProcs.Add(H);
					g_sdkf.XPipeRead.Add(RP);
					g_sdkf.XPipeWrite.Add(WP);
				}
				if (g_sdkf.XProcs.Num() == 0) { BF6_SdkFetchFail(TEXT("Could not start tar.exe to unpack the SDK.")); return false; }
				g_sdkf.Phase = FBF6SdkFetch::EPhase::Extract;
				g_sdkf.PhaseStart = FPlatformTime::Seconds();
			}
			return true;
		}
		if (g_sdkf.Phase == FBF6SdkFetch::EPhase::Extract)
		{
			// drain every worker's -v listing: each line = one entry on disk
			for (void* RP : g_sdkf.XPipeRead)
				if (RP)
				{
					const FString PipeOut = FPlatformProcess::ReadPipe(RP);
					for (const TCHAR C : PipeOut) if (C == TEXT('\n')) g_sdkf.EntriesDone++;
				}
			if (g_sdkf.EntriesTotal > 0 && g_sdkf.EntriesDone > 0)
			{
				const float F = FMath::Clamp((float)g_sdkf.EntriesDone / (float)g_sdkf.EntriesTotal, 0.f, 1.f);
				g_sdkf.Frac = 0.88f + 0.115f * F;
				g_sdkf.Status = FString::Printf(TEXT("Unpacking Portal SDK %s... %d%%  (%lld of %lld files)"),
					*g_sdkf.Version, (int32)(F * 100.f), g_sdkf.EntriesDone, g_sdkf.EntriesTotal);
			}
			else
			{
				const int32 Mins = (int32)((FPlatformTime::Seconds() - g_sdkf.PhaseStart) / 60.0);
				g_sdkf.Frac = 0.9f;
				g_sdkf.Status = FString::Printf(TEXT("Unpacking Portal SDK %s... this takes a few minutes.%s"),
					*g_sdkf.Version, Mins >= 1 ? *FString::Printf(TEXT("  (%d min)"), Mins) : TEXT(""));
			}
			bool bAnyRunning = false;
			for (FProcHandle& H : g_sdkf.XProcs)
				if (FPlatformProcess::IsProcRunning(H)) { bAnyRunning = true; break; }
			if (!bAnyRunning)
			{
				// last drain, then everything closes with the processes
				for (int32 i = 0; i < g_sdkf.XPipeRead.Num(); i++)
				{
					if (!g_sdkf.XPipeRead[i]) continue;
					const FString PipeOut = FPlatformProcess::ReadPipe(g_sdkf.XPipeRead[i]);
					for (const TCHAR C : PipeOut) if (C == TEXT('\n')) g_sdkf.EntriesDone++;
					FPlatformProcess::ClosePipe(g_sdkf.XPipeRead[i], g_sdkf.XPipeWrite[i]);
				}
				g_sdkf.XPipeRead.Reset();
				g_sdkf.XPipeWrite.Reset();
				// the catch-all worker (last) gates success; the SDK validator
				// after this checks the result is complete either way
				int32 Rc = -1;
				if (g_sdkf.XProcs.Num()) FPlatformProcess::GetProcReturnCode(g_sdkf.XProcs.Last(), &Rc);
				for (FProcHandle& H : g_sdkf.XProcs) FPlatformProcess::CloseProc(H);
				g_sdkf.XProcs.Reset();
				if (Rc != 0) { BF6_SdkFetchFail(FString::Printf(TEXT("Unpacking failed (tar exit %d). The zip is kept - click the download button to retry."), Rc)); return false; }
				BF6_SdkFetchFinish();
				return false;
			}
			return true;
		}
		return g_sdkf.Phase == FBF6SdkFetch::EPhase::Index;
	}

	// parse "a.b.c.d" for newest-version comparison
	static bool BF6_VersionNewer(const FString& A, const FString& B)
	{
		TArray<FString> Pa, Pb;
		A.ParseIntoArray(Pa, TEXT("."));
		B.ParseIntoArray(Pb, TEXT("."));
		for (int32 i = 0; i < 4; i++)
		{
			const int32 Va = Pa.IsValidIndex(i) ? FCString::Atoi(*Pa[i]) : 0;
			const int32 Vb = Pb.IsValidIndex(i) ? FCString::Atoi(*Pb[i]) : 0;
			if (Va != Vb) return Va > Vb;
		}
		return false;
	}

	// ---- SDK version history ----
	// The plugin ships a baked history of every Portal SDK release
	// (Resources/sdkhistory, generated by tools/sdk_history from the
	// community archive). Every SDK UPDATE extends it locally: before the
	// import overwrites anything, the new SDK is diffed against the snapshot
	// of the previous one - placeable types, maps, models, and the scripting
	// API surface - into data/sdkhistory/changes_<ver>.md, shown in the
	// History screen with a summary toast.
	static FString BF6_SdkHistoryDir() { return BF6_DataDir() / TEXT("sdkhistory"); }

	static FString BF6_SdkVersionOf(const FString& Root)
	{
		FString S;
		if (!FFileHelper::LoadFileToString(S, *(Root / TEXT("sdk.version.json")))) return FString();
		TSharedPtr<FJsonObject> J;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(S);
		FString V;
		if (FJsonSerializer::Deserialize(R, J) && J.IsValid()) J->TryGetStringField(TEXT("version"), V);
		return V;
	}

	static void BF6_TypeSetOf(const FString& AssetTypesPath, TSet<FString>& Out)
	{
		FString S;
		if (!FFileHelper::LoadFileToString(S, *AssetTypesPath)) return;
		TSharedPtr<FJsonObject> J;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(S);
		if (!FJsonSerializer::Deserialize(R, J) || !J.IsValid()) return;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!J->TryGetArrayField(TEXT("AssetTypes"), Rows)) return;
		for (const TSharedPtr<FJsonValue>& v : *Rows)
		{
			const TSharedPtr<FJsonObject> o = v->AsObject();
			FString T;
			if (o.IsValid() && o->TryGetStringField(TEXT("type"), T) && !T.IsEmpty()) Out.Add(T);
		}
	}

	static void BF6_ApiSetOf(const FString& DtsPath, TSet<FString>& Out)
	{
		FString S;
		if (!FFileHelper::LoadFileToString(S, *DtsPath)) return;
		TArray<FString> Lines;
		S.ParseIntoArrayLines(Lines, true);
		for (FString L : Lines)
		{
			L.TrimStartInline();
			if (!L.StartsWith(TEXT("export "))) continue;
			L.RightChopInline(7);
			if (L.StartsWith(TEXT("declare "))) L.RightChopInline(8);
			FString Kind;
			for (const TCHAR* K : { TEXT("function "), TEXT("enum "), TEXT("const "), TEXT("class "), TEXT("interface ") })
				if (L.StartsWith(K)) { Kind = FString(K).TrimEnd(); L.RightChopInline(FCString::Strlen(K)); break; }
			if (Kind.IsEmpty()) continue;
			FString Name;
			for (const TCHAR C : L) { if (FChar::IsAlnum(C) || C == TEXT('_')) Name.AppendChar(C); else break; }
			if (!Name.IsEmpty()) Out.Add(Kind + TEXT(" ") + Name);
		}
	}

	static void BF6_FileBasenames(const FString& Dir, const TCHAR* Pattern, TSet<FString>& Out)
	{
		TArray<FString> F;
		IFileManager::Get().FindFiles(F, *(Dir / Pattern), true, false);
		for (const FString& x : F) Out.Add(FPaths::GetBaseFilename(x));
	}

	static void BF6_LoadLineSet(const FString& Path, TSet<FString>& Out)
	{
		FString S;
		if (!FFileHelper::LoadFileToString(S, *Path)) return;
		TArray<FString> Lines;
		S.ParseIntoArrayLines(Lines, true);
		for (const FString& L : Lines) if (!L.IsEmpty()) Out.Add(L);
	}

	void BF6_SnapshotSdkHistory(const FString& SdkRoot)
	{
		const FString Ver = BF6_SdkVersionOf(SdkRoot);
		if (Ver.IsEmpty()) return;
		const FString Dir = BF6_SdkHistoryDir() / Ver;
		if (FPaths::FileExists(Dir / TEXT("asset_types.json"))) return;   // already snapshotted
		IFileManager::Get().MakeDirectory(*Dir, true);
		IFileManager::Get().Copy(*(Dir / TEXT("asset_types.json")), *(SdkRoot / TEXT("FbExportData/asset_types.json")));
		IFileManager::Get().Copy(*(Dir / TEXT("index.d.ts")), *(SdkRoot / TEXT("code/types/mod/index.d.ts")));
		TSet<FString> Lv, Md;
		BF6_FileBasenames(SdkRoot / TEXT("GodotProject/levels"), TEXT("MP_*.tscn"), Lv);
		BF6_FileBasenames(SdkRoot / TEXT("GodotProject/raw/models"), TEXT("*.glb"), Md);
		FFileHelper::SaveStringToFile(FString::Join(Lv.Array(), TEXT("\n")), *(Dir / TEXT("levels.txt")));
		FFileHelper::SaveStringToFile(FString::Join(Md.Array(), TEXT("\n")), *(Dir / TEXT("models.txt")));
	}

	static FString BF6_JoinCapped(const TSet<FString>& In, int32 Cap)
	{
		TArray<FString> A = In.Array();
		A.Sort();
		FString Out;
		for (int32 i = 0; i < A.Num() && i < Cap; i++) { if (i) Out += TEXT(", "); Out += A[i]; }
		if (A.Num() > Cap) Out += FString::Printf(TEXT(", ... and %d more"), A.Num() - Cap);
		return Out;
	}

	static void BF6_GenerateSdkChanges(const FString& NewRoot)
	{
		const FString NewVer = BF6_SdkVersionOf(NewRoot);
		if (NewVer.IsEmpty()) return;
		// newest PREVIOUS snapshot (a same-version re-sync diffs nothing)
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(BF6_SdkHistoryDir() / TEXT("*")), false, true);
		Dirs.Remove(NewVer);
		if (Dirs.Num() == 0) return;
		Dirs.Sort([](const FString& A, const FString& B){ return BF6_VersionNewer(A, B); });
		const FString OldVer = Dirs[0];
		const FString Snap = BF6_SdkHistoryDir() / OldVer;

		TSet<FString> TOld, TNew, AOld, ANew, LOld, LNew, MOld, MNew;
		BF6_TypeSetOf(Snap / TEXT("asset_types.json"), TOld);
		BF6_TypeSetOf(NewRoot / TEXT("FbExportData/asset_types.json"), TNew);
		BF6_ApiSetOf(Snap / TEXT("index.d.ts"), AOld);
		BF6_ApiSetOf(NewRoot / TEXT("code/types/mod/index.d.ts"), ANew);
		BF6_LoadLineSet(Snap / TEXT("levels.txt"), LOld);
		BF6_FileBasenames(NewRoot / TEXT("GodotProject/levels"), TEXT("MP_*.tscn"), LNew);
		BF6_LoadLineSet(Snap / TEXT("models.txt"), MOld);
		BF6_FileBasenames(NewRoot / TEXT("GodotProject/raw/models"), TEXT("*.glb"), MNew);

		const TSet<FString> TAdd = TNew.Difference(TOld), TDel = TOld.Difference(TNew);
		const TSet<FString> AAdd = ANew.Difference(AOld), ADel = AOld.Difference(ANew);
		const TSet<FString> LAdd = LNew.Difference(LOld), LDel = LOld.Difference(LNew);
		const TSet<FString> MAdd = MNew.Difference(MOld), MDel = MOld.Difference(MNew);
		if (TOld.Num() == 0 && AOld.Num() == 0) return;   // no usable snapshot

		FString Md = FString::Printf(TEXT("## %s  (installed %s, replacing %s)\n\n"),
			*NewVer, *FDateTime::Now().ToString(TEXT("%Y-%m-%d")), *OldVer);
		if (LAdd.Num()) Md += FString::Printf(TEXT("- New maps: %s\n"), *BF6_JoinCapped(LAdd, 10));
		if (LDel.Num()) Md += FString::Printf(TEXT("- Maps removed: %s\n"), *BF6_JoinCapped(LDel, 10));
		if (TAdd.Num()) Md += FString::Printf(TEXT("- New placeable types: %d (%s)\n"), TAdd.Num(), *BF6_JoinCapped(TAdd, 30));
		if (TDel.Num()) Md += FString::Printf(TEXT("- Placeable types removed: %s\n"), *BF6_JoinCapped(TDel, 30));
		if (MAdd.Num()) Md += FString::Printf(TEXT("- New models: %d (%s)\n"), MAdd.Num(), *BF6_JoinCapped(MAdd, 15));
		if (MDel.Num()) Md += FString::Printf(TEXT("- Models removed: %s\n"), *BF6_JoinCapped(MDel, 15));
		if (AAdd.Num()) Md += FString::Printf(TEXT("- New scripting API: %s\n"), *BF6_JoinCapped(AAdd, 40));
		if (ADel.Num()) Md += FString::Printf(TEXT("- Scripting API removed: %s\n"), *BF6_JoinCapped(ADel, 40));
		if (TAdd.Num() + TDel.Num() + AAdd.Num() + ADel.Num() + LAdd.Num() + MAdd.Num() == 0)
			Md += TEXT("- No placeable, map, model, or scripting API changes detected.\n");
		Md += TEXT("\n");
		FFileHelper::SaveStringToFile(Md, *(BF6_SdkHistoryDir() / FString::Printf(TEXT("changes_%s.md"), *NewVer)));
		Notify(FString::Printf(TEXT("Portal SDK %s: %d new placeables, %d new models, %d API additions%s. The History screen has the full list."),
			*NewVer, TAdd.Num(), MAdd.Num(), AAdd.Num(),
			LAdd.Num() ? *FString::Printf(TEXT(", new map %s"), *BF6_JoinCapped(LAdd, 3)) : TEXT("")));
	}

	// The first section of a "## version" markdown file - everything from the
	// first heading to the second. This is what NEW FEATURES opens on: the
	// latest, not the archive.
	static FString BF6_FirstSection(const FString& Md)
	{
		int32 First = Md.Find(TEXT("\n## "));
		if (First == INDEX_NONE) return Md;
		First += 1;   // start at the heading itself
		const int32 Second = Md.Find(TEXT("\n## "), ESearchCase::CaseSensitive, ESearchDir::FromStart, First + 4);
		return Second == INDEX_NONE ? Md.Mid(First) : Md.Mid(First, Second - First);
	}

	FString ToolHistoryText()
	{
		FString S;
		FFileHelper::LoadFileToString(S, *(g_pluginDir / TEXT("Resources/CHANGELOG.md")));
		return S;
	}

	FString SdkHistoryText()
	{
		FString Out, S;
		// locally generated updates first (SDKs newer than the baked history)
		TArray<FString> Changes;
		IFileManager::Get().FindFiles(Changes, *(BF6_SdkHistoryDir() / TEXT("changes_*.md")), true, false);
		Changes.Sort([](const FString& A2, const FString& B2){ return BF6_VersionNewer(A2, B2); });
		for (const FString& C : Changes)
			if (FFileHelper::LoadFileToString(S, *(BF6_SdkHistoryDir() / C))) Out += S;
		if (FFileHelper::LoadFileToString(S, *(g_pluginDir / TEXT("Resources/sdkhistory/SDK-HISTORY.md"))))
		{
			S.ReplaceInline(TEXT("# Portal SDK version history\n"), TEXT(""));
			Out += S;
		}
		return Out;
	}

	FString LatestToolNotes() { return BF6_FirstSection(ToolHistoryText()); }
	FString LatestSdkNotes()  { return BF6_FirstSection(SdkHistoryText()); }

	// The unlock dot. Battlefield marks new unlocks with an orange dot on the
	// button's corner; ours means "this version has notes you have not read".
	// Cleared the moment the panel opens, remembered across sessions.
	static int32 GHistNews = -1;   // -1 unknown, 0 seen, 1 news
	bool HistoryHasNews()
	{
		if (GHistNews < 0)
		{
			FString Seen;
			GConfig->GetString(TEXT("BF6UnrealSDK"), TEXT("HistorySeenVersion"), Seen, GEditorPerProjectIni);
			GHistNews = (Seen == PluginVersion()) ? 0 : 1;
		}
		return GHistNews == 1;
	}
	void MarkHistorySeen()
	{
		GHistNews = 0;
		GConfig->SetString(TEXT("BF6UnrealSDK"), TEXT("HistorySeenVersion"), *PluginVersion(), GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	FString VersionHistoryText()
	{
		FString Out, S;
		if (FFileHelper::LoadFileToString(S, *(g_pluginDir / TEXT("Resources/CHANGELOG.md"))))
			Out += S + TEXT("\n");
		Out += TEXT("# Portal SDK version history\n\n");
		// locally generated updates first (SDKs newer than the baked history)
		TArray<FString> Changes;
		IFileManager::Get().FindFiles(Changes, *(BF6_SdkHistoryDir() / TEXT("changes_*.md")), true, false);
		Changes.Sort([](const FString& A, const FString& B){ return BF6_VersionNewer(A, B); });
		for (const FString& C : Changes)
			if (FFileHelper::LoadFileToString(S, *(BF6_SdkHistoryDir() / C))) Out += S;
		if (FFileHelper::LoadFileToString(S, *(g_pluginDir / TEXT("Resources/sdkhistory/SDK-HISTORY.md"))))
		{
			// the baked doc's own title would duplicate the section header
			S.ReplaceInline(TEXT("# Portal SDK version history\n"), TEXT(""));
			Out += S;
		}
		return Out;
	}

	// Ask for the newest SDK version: EA's official manifest first, the
	// community archive as fallback. Done(version, fileSize, bOfficial);
	// empty version = both sources unreachable.
	static void BF6_FetchLatestSdk(TFunction<void(FString, int64, bool)> Done)
	{
		auto Parse = [](const FString& Body, FString& OutVer, int64& OutSize)
		{
			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Body);
			const TArray<TSharedPtr<FJsonValue>>* Vers = nullptr;
			if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid() || !Root->TryGetArrayField(TEXT("versions"), Vers)) return;
			for (const TSharedPtr<FJsonValue>& V : *Vers)
			{
				const TSharedPtr<FJsonObject> O = V->AsObject(); if (!O.IsValid()) continue;
				FString Ver; O->TryGetStringField(TEXT("version"), Ver);
				if (Ver.IsEmpty()) continue;
				// fileSize is a STRING on the official manifest, a number on the mirror
				int64 Sz = 0; FString SzS;
				if (O->TryGetStringField(TEXT("fileSize"), SzS)) Sz = FCString::Atoi64(*SzS);
				else { double D = 0; if (O->TryGetNumberField(TEXT("fileSize"), D)) Sz = (int64)D; }
				if (OutVer.IsEmpty() || BF6_VersionNewer(Ver, OutVer)) { OutVer = Ver; OutSize = Sz; }
			}
		};
		auto Request = [Parse](const FString& Url, TFunction<void(FString, int64)> Next)
		{
			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
			Req->SetURL(Url);
			Req->SetVerb(TEXT("GET"));
			Req->SetHeader(TEXT("User-Agent"), kBF6UA);
			Req->OnProcessRequestComplete().BindLambda([Parse, Next](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
			{
				FString Ver; int64 Sz = 0;
				if (bOk && Resp.IsValid() && Resp->GetResponseCode() == 200) Parse(Resp->GetContentAsString(), Ver, Sz);
				Next(Ver, Sz);
			});
			Req->ProcessRequest();
		};
		Request(kOfficialIndex, [Request, Done](FString Ver, int64 Sz)
		{
			if (!Ver.IsEmpty()) { Done(Ver, Sz, true); return; }
			Request(kHoardIndex, [Done](FString V2, int64 S2) { Done(V2, S2, false); });
		});
	}

	void StartSdkDownload()
	{
		if (IsSdkFetching() || IsImporting()) return;
		// an SDK already sits fully extracted in the managed folder (an
		// interrupted import, or a manual unzip): the zip is long deleted, so
		// without this the button would re-download 3 GB. Use what's there -
		// the validator inside the finder guarantees it's complete, and a
		// NEWER SdK still comes through the normal update offer later.
		if (!IsDataInstalled())
		{
			const FString Existing = BF6_FindSdkRootIn(BF6_ManagedSdkBase());
			if (!Existing.IsEmpty())
			{
				Notify(TEXT("Found the SDK already unpacked - continuing with the import, no download needed."));
				GConfig->SetString(TEXT("BF6UnrealSDK"), TEXT("SdkRoot"), *Existing, GEditorPerProjectIni);
				GConfig->Flush(false, GEditorPerProjectIni);
				StartSdkImport(Existing);
				return;
			}
		}
		g_sdkf = FBF6SdkFetch();
		g_sdkf.Phase = FBF6SdkFetch::EPhase::Index;
		g_sdkf.Status = TEXT("Asking EA's Portal download service for the newest SDK...");
		g_sdkf.Frac = 0.02f;

		BF6_FetchLatestSdk([](FString Ver, int64 IndexSize, bool bOfficial)
		{
			if (g_sdkf.Phase != FBF6SdkFetch::EPhase::Index) return;
			if (Ver.IsEmpty())
			{ BF6_SdkFetchFail(TEXT("Could not reach EA's Portal download service or the community archive. Check your connection and try again, or point the tool at an SDK folder you downloaded yourself.")); return; }

			const FString ZipUrl = bOfficial
				? FString(kOfficialZip)
				: FString::Printf(TEXT("https://hoard.bfportal.gg/SDKs/PortalSDK-v%s.zip"), *Ver);

			// HEAD the actual zip: its Content-Length is the authoritative size
			// (the official manifest's fileSize drifts a few bytes from it)
			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Head = FHttpModule::Get().CreateRequest();
			Head->SetURL(ZipUrl);
			Head->SetVerb(TEXT("HEAD"));
			Head->SetHeader(TEXT("User-Agent"), kBF6UA);
			Head->OnProcessRequestComplete().BindLambda([Ver, IndexSize, ZipUrl, bOfficial](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
			{
				if (g_sdkf.Phase != FBF6SdkFetch::EPhase::Index) return;
				int64 Bytes = IndexSize;
				if (bOk && Resp.IsValid() && Resp->GetResponseCode() == 200)
				{
					const FString CL = Resp->GetHeader(TEXT("Content-Length"));
					if (!CL.IsEmpty()) Bytes = FCString::Atoi64(*CL);
				}
				if (Bytes <= 0) { BF6_SdkFetchFail(TEXT("The SDK download reported no size. Try again later.")); return; }

				// room for the zip AND the unpacked SDK, plus slack
				uint64 Total = 0, Free = 0;
				if (FPlatformMisc::GetDiskTotalAndFreeSpace(FPaths::ProjectSavedDir(), Total, Free) && (int64)Free < Bytes * 2 + (2ll << 30))
				{ BF6_SdkFetchFail(FString::Printf(TEXT("Not enough disk space: the SDK needs about %lld GB free (download plus unpacked copy)."), (Bytes * 2 + (2ll << 30)) >> 30)); return; }

				g_sdkf.Version = Ver;
				g_sdkf.Bytes = Bytes;
				g_sdkf.Url = ZipUrl;
				g_sdkf.Source = bOfficial ? TEXT("EA's Portal service") : TEXT("the community archive");
				g_sdkf.ZipPath = BF6_ManagedSdkBase() / FString::Printf(TEXT("PortalSDK-v%s.zip"), *Ver);
				g_sdkf.DestDir = BF6_ManagedSdkBase() / FString::Printf(TEXT("PortalSDK-%s"), *Ver);
				const FString Cur = StoredSdkRoot();
				if (!Cur.IsEmpty() && FPaths::IsUnderDirectory(Cur, BF6_ManagedSdkBase())) g_sdkf.OldRoot = Cur;

				// this exact version already unpacked? just wire it up
				if (!BF6_FindSdkRootIn(g_sdkf.DestDir).IsEmpty()) { BF6_SdkFetchFinish(); return; }

				IFileManager::Get().MakeDirectory(*BF6_ManagedSdkBase(), true);
				// curl ships with Windows; -C - resumes a partial download, and
				// the browser-style UA matters (EA's CDN rejects curl's default)
				g_sdkf.Proc = FPlatformProcess::CreateProc(*BF6_SysTool(TEXT("curl.exe")),
					*FString::Printf(TEXT("-L -sS -A \"%s\" -o \"%s\" -C - \"%s\""), kBF6UA, *g_sdkf.ZipPath, *ZipUrl),
					true, true, true, nullptr, 0, nullptr, nullptr);
				if (!g_sdkf.Proc.IsValid()) { BF6_SdkFetchFail(TEXT("Could not start curl.exe to download the SDK.")); return; }
				g_sdkf.Phase = FBF6SdkFetch::EPhase::Download;
				g_sdkf.PhaseStart = FPlatformTime::Seconds();
				if (!g_sdkf.Tick.IsValid())
					g_sdkf.Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&BF6_SdkFetchTickFn), 0.25f);
			});
			Head->ProcessRequest();
		});
	}

	// Upload limits move with SDK updates (the community re-measures them by
	// trial and error each time). The tool ships baked baselines and refreshes
	// them from a small limits.json we maintain in the public repo, so a limit
	// change reaches every user without a plugin release. Cached in config for
	// offline sessions.
	void FetchUploadLimits()
	{
		// last known values first, so offline sessions keep the newest numbers
		int32 Cached = 0;
		if (GConfig->GetInt(TEXT("BF6UnrealSDK"), TEXT("LimitPerMapBytes"), Cached, GEditorPerProjectIni) && Cached > 0) g_limPerMap = Cached;
		if (GConfig->GetInt(TEXT("BF6UnrealSDK"), TEXT("LimitExperienceBytes"), Cached, GEditorPerProjectIni) && Cached > 0) g_limExperience = Cached;

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
		Req->SetURL(TEXT("https://raw.githubusercontent.com/TabbedScamper/BF6_Unreal_SDK/main/limits.json"));
		Req->SetVerb(TEXT("GET"));
		Req->SetHeader(TEXT("User-Agent"), kBF6UA);
		Req->OnProcessRequestComplete().BindLambda([](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() != 200) return;   // baked/cached values stand
			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
			if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
			double V = 0;
			if (Root->TryGetNumberField(TEXT("perMapBytes"), V) && V > 0)
			{
				g_limPerMap = (int64)V;
				GConfig->SetInt(TEXT("BF6UnrealSDK"), TEXT("LimitPerMapBytes"), (int32)V, GEditorPerProjectIni);
			}
			if (Root->TryGetNumberField(TEXT("experienceBytes"), V) && V > 0)
			{
				g_limExperience = (int64)V;
				GConfig->SetInt(TEXT("BF6UnrealSDK"), TEXT("LimitExperienceBytes"), (int32)V, GEditorPerProjectIni);
			}
			GConfig->Flush(false, GEditorPerProjectIni);
		});
		Req->ProcessRequest();
	}

	// launch-time: is a newer SDK out than the one our data was built from?
	// Detection asks EA's OFFICIAL manifest (community archive as fallback).
	// One offer per version - declining does not nag every launch.
	void CheckForNewSdk()
	{
		if (!IsDataInstalled() || IsImporting() || IsSdkFetching()) return;
		BF6_FetchLatestSdk([](FString Best, int64, bool bOfficial)
		{
			const FString Have = BF6_ReadSdkVersion(BF6_DataDir() / TEXT("sdk.version.json"));
			if (Best.IsEmpty() || Have.IsEmpty() || !BF6_VersionNewer(Best, Have)) return;   // silent: courtesy check
			FString Offered;
			GConfig->GetString(TEXT("BF6UnrealSDK"), TEXT("LastOfferedSdk"), Offered, GEditorPerProjectIni);
			if (Offered == Best) return;
			GConfig->SetString(TEXT("BF6UnrealSDK"), TEXT("LastOfferedSdk"), *Best, GEditorPerProjectIni);
			GConfig->Flush(false, GEditorPerProjectIni);
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(
				TEXT("EA released Portal SDK %s (your content is built from %s).\n\nDownload it and update now? About 3 GB from %s; your maps, saves, and blocks carry over untouched, and only changed SDK content reconverts."),
				*Best, *Have, bOfficial ? TEXT("EA's Portal service") : TEXT("the community archive")))) == EAppReturnType::Yes)
			{
				ShowSdkSetup();
				StartSdkDownload();
			}
		});
	}

	void CheckForUpdates(bool bManual)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
		Req->SetURL(kUpdateRepoApi);
		Req->SetVerb(TEXT("GET"));
		Req->SetHeader(TEXT("User-Agent"), TEXT("BF6UnrealSDK"));
		Req->SetHeader(TEXT("Accept"), TEXT("application/vnd.github+json"));
		Req->OnProcessRequestComplete().BindLambda([bManual](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() != 200)
			{ if (bManual) Notify(TEXT("Update check failed (no connection, or no releases yet).")); return; }
			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
			if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid())
			{ if (bManual) Notify(TEXT("Update check failed (bad response).")); return; }

			FString Tag; Root->TryGetStringField(TEXT("tag_name"), Tag);
			const FString Local = PluginVersion();
			if (Tag.IsEmpty() || !BF6_IsNewer(Tag, Local))
			{ if (bManual) Notify(FString::Printf(TEXT("You're up to date (v%s)."), *Local)); return; }

			// find the plugin zip asset ("...Plugin....zip" preferred, else first zip)
			FString AssetUrl, AssetName;
			uint64 AssetBytes = 0;
			const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
			if (Root->TryGetArrayField(TEXT("assets"), Assets))
				for (const auto& v : *Assets)
				{
					const TSharedPtr<FJsonObject> a = v->AsObject(); if (!a.IsValid()) continue;
					FString Nm, Url;
					a->TryGetStringField(TEXT("name"), Nm);
					a->TryGetStringField(TEXT("browser_download_url"), Url);
					if (!Nm.EndsWith(TEXT(".zip"))) continue;
					if (AssetUrl.IsEmpty() || Nm.Contains(TEXT("Plugin")))
					{
						AssetUrl = Url; AssetName = Nm;
						double Sz = 0; a->TryGetNumberField(TEXT("size"), Sz);
						AssetBytes = Sz > 0 ? (uint64)Sz : 0;
					}
					if (Nm.Contains(TEXT("Plugin"))) break;
				}
			if (AssetUrl.IsEmpty())
			{ if (bManual) Notify(FString::Printf(TEXT("%s is out, but has no plugin package attached yet."), *Tag)); return; }

			const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(
				TEXT("BF6 Unreal SDK %s is available (you have v%s).\n\nDownload now? The editor will close itself to apply the update and reopen when it's done. The whole thing takes a minute or two, most of it the editor restarting. You'll see \"Updated to %s\" when it's back."),
				*Tag, *Local, *Tag)));
			if (Choice == EAppReturnType::Yes) BF6_DownloadUpdate(AssetUrl, Tag, AssetBytes);
		});
		Req->ProcessRequest();
	}

	FString  GameInstallDir() { return g_gameDir; }
	// The low-poly map, out of the way. Hidden rather than destroyed: it is the
	// tool's own preview of where you are, and an add-on drawing the real thing
	// over it should be able to hand it straight back.
	static bool g_contextHidden = false;

	int32 SetContextHidden(bool bHidden)
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return 0;
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(kContextTag)) continue;
			It->SetIsTemporarilyHiddenInEditor(bHidden);
			n++;
		}
		g_contextHidden = bHidden;
		BF6_Redraw();
		return n;
	}

	bool IsContextHidden() { return g_contextHidden; }

	void     Toast(const FString& Message) { Notify(Message); }
	bool     IsEditing()      { return g_ss.bEditing; }
	FString  CurrentLevel()   { return g_ss.CurrentLevel; }
	FString  CurrentSave()    { return g_ss.CurrentSave; }

	void         RecomputeBudget() { BF6_RecomputeBudget(); }
	FText        BudgetText()  { return g_ss.BudgetText; }
	float        BudgetFrac()  { return g_ss.BudgetFrac; }
	FLinearColor BudgetColor() { return g_ss.BudgetColor; }
	bool         BudgetOver()  { return g_ss.BudgetMax > 0 && g_ss.TotalCost > g_ss.BudgetMax; }

	TArray<FString> Categories()
	{
		TMap<FString,int32> Counts;
		for (const auto& r : g_ss.AllItems) Counts.FindOrAdd(BF6_EffectiveCategory(*r))++;
		Counts.ValueSort([](int32 A, int32 B){ return A > B; });
		TArray<FString> Out; for (const auto& P : Counts) Out.Add(P.Key);
		return Out;
	}
	int32 CategoryCount(const FString& Category)
	{
		int32 n = 0; for (const auto& r : g_ss.AllItems) if (BF6_EffectiveCategory(*r) == Category) n++;
		return n;
	}

	// Shared filter over a row source: category (empty = every category) +
	// fuzzy query, best matches first, capped at Max.
	static TArray<FPlaceableInfo> FilterRows(const TArray<TSharedPtr<FPlaceableRow>>& Src,
		const FString& Category, const FString& Query, int32 Max)
	{
		const FString P = Query.ToLower();
		TArray<TPair<int32, const FPlaceableRow*>> Scored;
		for (const auto& r : Src)
		{
			if (!Category.IsEmpty() && BF6_EffectiveCategory(*r) != Category) continue;
			int32 s = 0;
			if (P.IsEmpty()) Scored.Emplace(0, r.Get());
			else if (FuzzyScore(P, r->Type, s)) Scored.Emplace(s + 20, r.Get());
			else { int32 sd=0; if (FuzzyScore(P, r->Directory, sd)) Scored.Emplace(sd, r.Get()); }
		}
		if (!P.IsEmpty()) Scored.Sort([](const TPair<int32,const FPlaceableRow*>& A, const TPair<int32,const FPlaceableRow*>& B){ return A.Key > B.Key; });
		TArray<FPlaceableInfo> Out;
		for (const auto& pr : Scored)
		{
			if (Out.Num() >= Max) break;
			FPlaceableInfo I; I.Type=pr.Value->Type; I.Directory=pr.Value->Directory; I.Mesh=pr.Value->Mesh;
			I.PhysicsCost=pr.Value->PhysicsCost; I.Category=BF6_EffectiveCategory(*pr.Value);
			Out.Add(I);
		}
		return Out;
	}

	TArray<FPlaceableInfo> PlaceablesIn(const FString& Category, const FString& Query, int32 Max)
	{
		return FilterRows(g_ss.AllItems, Category, Query, Max);
	}

	// ---- the slide-up Object Library ----
	TArray<FString> LibraryCategories(bool bAllLevels)
	{
		TMap<FString,int32> Counts;
		for (const auto& r : (bAllLevels ? g_allGlobal : g_ss.AllItems)) Counts.FindOrAdd(BF6_EffectiveCategory(*r))++;
		Counts.ValueSort([](int32 A, int32 B){ return A > B; });
		TArray<FString> Out; for (const auto& P : Counts) Out.Add(P.Key);
		return Out;
	}
	TArray<FPlaceableInfo> LibraryPlaceables(const FString& Category, const FString& Query, bool bAllLevels, int32 Max)
	{
		return FilterRows(bAllLevels ? g_allGlobal : g_ss.AllItems, Category, Query, Max);
	}
	void SetTypeCategory(const FString& Type, const FString& NewCategory)
	{
		if (NewCategory.IsEmpty()) GCatOverrides.Remove(Type);
		else GCatOverrides.Add(Type, NewCategory);
		BF6_SaveCatOverrides();
	}

	TArray<FString> AllLevels()
	{
		// The LIVE list from the imported catalogue, so a new SDK's new maps show
		// up without a code change. Curated manifest order first, then any levels
		// the manifest doesn't know yet (new maps), sorted.
		TArray<FString> Live;
		if (g_ctx && g_lvlcnt && g_lvlname)
			for (int i = 0, n = g_lvlcnt(g_ctx); i < n; i++) Live.Add(UTF8_TO_TCHAR(g_lvlname(g_ctx, i)));
		TArray<FString> Out;
		if (Live.Num() == 0)
		{
			for (int i = 0; i < GBF6MapCardCount; i++) Out.Add(GBF6MapCards[i].Code);
			return Out;
		}
		for (int i = 0; i < GBF6MapCardCount; i++) if (Live.Contains(GBF6MapCards[i].Code)) Out.Add(GBF6MapCards[i].Code);
		TArray<FString> Extra;
		for (const FString& L : Live) if (!Out.Contains(L)) Extra.Add(L);
		Extra.Sort();
		Out.Append(Extra);
		return Out;
	}
	FString DisplayName(const FString& Level)
	{
		for (int i=0;i<GBF6MapCardCount;i++) if (Level == GBF6MapCards[i].Code) return GBF6MapCards[i].Name;
		// unknown (new) map: prettify the codename until the manifest learns it
		FString S = Level;
		S.RemoveFromStart(TEXT("MP_"));
		S = S.Replace(TEXT("_"), TEXT(" "));
		return S;
	}
	int32 PlaceableTotal(const FString& Level) { return PlaceableCount(Level); }
	TArray<FString> SavesFor(const FString& Level) { return ListSaves(Level); }
	uint32 SavesFingerprint() { return BF6_SavesFingerprint(); }

	// Delete a save from BOTH layouts (the old flat file would otherwise
	// resurface as a zombie in the resume list). Refuses the OPEN session:
	// its autosave would just rewrite the file within the minute.
	bool DeleteSave(const FString& Level, const FString& Name)
	{
		if (Name.IsEmpty()) return false;

		// Deleting the save you have OPEN is allowed. It used to be refused,
		// because an autosave would write the file straight back a minute
		// later - but the answer to that is to close the session first, not to
		// send the creator off to open some other map before they are allowed
		// to tidy up.
		//
		// Closing happens BEFORE the file goes, so there is no window in which
		// a save could land on the path we are about to delete.
		const bool bWasOpen = g_ss.CurrentLevel == Level && g_ss.CurrentSave == Name;
		if (bWasOpen)
		{
			g_ss.CurrentSave.Empty();
			g_ss.bEditing = false;
		}

		bool bAny = false;
		const FString N = BF6_SessionPathNew(Level, Name);
		const FString O = BF6_SessionPathOld(Level, Name);
		if (FPaths::FileExists(N)) bAny |= !!IFileManager::Get().Delete(*N);
		if (FPaths::FileExists(O)) bAny |= !!IFileManager::Get().Delete(*O);
		// an emptied custom-map folder goes with its last level file
		const FString Dir = BF6_SavesRoot() / Name;
		TArray<FString> Left;
		IFileManager::Get().FindFiles(Left, *(Dir / TEXT("*")), true, true);
		if (Left.Num() == 0) IFileManager::Get().DeleteDirectory(*Dir, false, false);

		// The world still holds the objects that save described, and they now
		// belong to nothing. Reopening the base puts the map back in a state
		// that matches the files, which is the honest outcome of deleting the
		// only record of that work.
		if (bWasOpen)
		{
			BF6_OpenMapWorldImpl(Level, FString());
			Notify(FString::Printf(TEXT("Deleted '%s'. This map is back to its read-only base."), *Name));
		}
		return bAny;
	}

	const FSlateBrush* MapThumbnail(const FString& Level)
	{
		static TMap<FString, TSharedPtr<FSlateBrush>> Cache;
		static TArray<TStrongObjectPtr<UTexture2D>> Keep;
		if (TSharedPtr<FSlateBrush>* F = Cache.Find(Level)) return F->Get();
		const TCHAR* Png = nullptr;
		for (int i=0;i<GBF6MapCardCount;i++) if (Level == GBF6MapCards[i].Code) { Png = GBF6MapCards[i].Png; break; }
		TSharedPtr<FSlateBrush> Brush;
		if (Png && *Png)
		{
			const FString Path = FPaths::Combine(BF6_DataDir(), TEXT("maps"), FString(Png));
			if (FPaths::FileExists(Path))
				if (UTexture2D* Tex = FImageUtils::ImportFileAsTexture2D(Path))
				{
					Keep.Add(TStrongObjectPtr<UTexture2D>(Tex));
					Brush = MakeShared<FSlateBrush>();
					Brush->SetResourceObject(Tex);
					Brush->ImageSize = FVector2D(Tex->GetSizeX(), Tex->GetSizeY());
					Brush->DrawAs = ESlateBrushDrawType::Image;
				}
		}
		Cache.Add(Level, Brush);
		return Brush.Get();
	}

	void OpenMapWorld(const FString& Level, const FString& SaveName) { BF6_OpenMapWorldImpl(Level, SaveName); }

	// Close the open session outright. Leaving for the map screen used to keep
	// the whole session running behind the selector, which meant a "Return to
	// build" button to explain, a map that could not be deleted because it was
	// secretly still open, and a creator never quite sure what state they were
	// in. Now the map screen means what it looks like: nothing is open.
	// The terrain is deliberately left standing - reopening the same map is
	// the common loop, and the scenery is identical every time.
	void CloseSession()
	{
		if (g_ss.CurrentLevel.IsEmpty()) return;
		BF6Api::BF6_MapDecalStash();
		BF6ExtInternal::BroadcastMapClosing(g_ss.CurrentLevel);
		ClearActorsWithTag(kPlacedTag);
		ClearActorsWithTag(kBaseTag);
		ClearActorsWithTag(kGroupTag);
		g_ss.CurrentLevel.Empty();
		g_ss.CurrentSave.Empty();
		g_ss.bEditing = false;
		BF6_RecomputeBudget();
		RefreshSceneTree();
	}
	void ExportSpatial()
	{
		// Minified = smallest for the Portal upload cap, but every object gets a
		// short generated name. Readable keeps the real names, which is what you
		// want for re-importing and sharing work in progress.
		const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNoCancel, FText::FromString(
			TEXT("Minify object names for the Portal upload?\n\nYes = minified (smallest file, best for uploading).\nNo = readable names (best for re-importing and sharing).\nCancel = don't export.")));
		if (Choice == EAppReturnType::Cancel) return;
		BF6_ExportSpatial(Choice == EAppReturnType::Yes);
	}
	bool ImportSpatial() { return BF6_ImportSpatialDialog(); }

	// ---- autosave, OFF by default -----------------------------------------
	//
	// A minute of lost work is cheap; an hour of deliberate experiment written
	// over the top of a map somebody wanted to keep is not. The common way this
	// tool gets used on an existing map is heavy, throwaway change - try
	// something, look at it, revert - and an autosave turns "revert" into "the
	// file already has it".
	//
	// So it is a choice, remembered per project, and it starts off. Saving is
	// one button and one keystroke away; nobody loses work they meant to keep
	// without being asked to press it.
	static bool GAutosaveLoaded = false;
	static bool GAutosave = false;

	bool GetAutosave()
	{
		if (!GAutosaveLoaded)
		{
			GAutosaveLoaded = true;
			GConfig->GetBool(TEXT("BF6UnrealSDK"), TEXT("Autosave"), GAutosave, GEditorPerProjectIni);
		}
		return GAutosave;
	}

	void SetAutosave(bool bOn)
	{
		GetAutosave();            // load first, so the write is not against a stale default
		GAutosave = bOn;
		GConfig->SetBool(TEXT("BF6UnrealSDK"), TEXT("Autosave"), GAutosave, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	// ---- Save As -> Godot .tscn -------------------------------------------
	//
	// The way BACK. Import has read Godot scenes since the start; this writes
	// one, so a map built here can be opened in the official SDK - for the
	// creator who wants Godot's exporter, a collaborator on the other tool, or
	// just an exit that is not a one-way door. Format verified against the
	// shipped MP_Battery.tscn and the SDK's own user maps.
	//
	// Layout notes that cost verification time, so they are recorded here:
	// - instances point at res://objects/<directory>/<Type>.tscn, with the two
	//   volume types living under the portal_tools addon instead
	// - Godot serialises Transform3D as basis ROWS; we hold columns, so the
	//   nine numbers go out R.x U.x F.x  R.y U.y F.y  R.z U.z F.z
	// - node transforms are LOCAL to their parent, names unique per parent
	// - selections are written as the enum INDEX (the inverse of our import)
	// - links need BOTH halves: node_paths=PackedStringArray(...) in the node
	//   header and relative NodePath(...) values in the body
	// - a WaypointPath rides its owner as a Curve3D sub_resource, nine numbers
	//   per point (zero in/out handles + position), exactly what our importer
	//   and gdconverter read back
	FString BF6_BuildTscn()
	{
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W || g_ss.CurrentLevel.IsEmpty()) return FString();
		const FString Level = g_ss.CurrentLevel;

		auto GD = [](double v) { return FString::SanitizeFloat(v); };
		auto SanitizeName = [](FString N)
		{
			for (TCHAR& C : N)
				if (C == TEXT('.') || C == TEXT('/') || C == TEXT(':') || C == TEXT('@') || C == TEXT('"') || C == TEXT('%'))
					C = TEXT('_');
			return N;
		};

		// WHERE each object scene really lives, read off the user's own SDK.
		//
		// The schema's `directory` field is NOT the repo layout: a prop whose
		// directory says "Generic/Industrial/LightFixtures" actually lives at
		// res://objects/lightfixtures/<Type>.tscn - flat lowercase category
		// folders that only the SDK's disk knows. The first cut trusted the
		// schema and Godot opened the file to a wall of missing dependencies.
		// So the SDK's objects tree is scanned ONCE and the real relative paths
		// win; the schema guess is only the fallback for a machine with no SDK
		// on it (the file still opens there via Open Anyway).
		static TMap<FString, FString> RealPath;   // Type -> res:// path
		static FString ScannedRoot;
		const FString SdkRoot = StoredSdkRoot();
		if (!SdkRoot.IsEmpty() && SdkRoot != ScannedRoot)
		{
			ScannedRoot = SdkRoot;
			RealPath.Reset();
			const FString ObjRoot = SdkRoot / TEXT("GodotProject/objects");
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *ObjRoot, TEXT("*.tscn"), true, false);
			for (FString& F : Found)
			{
				FString Rel = F;
				FPaths::MakePathRelativeTo(Rel, *(ObjRoot + TEXT("/")));
				RealPath.Add(FPaths::GetBaseFilename(F), TEXT("res://objects/") + Rel);
			}
		}

		TMap<FString, FString> DirOf;
		for (const TSharedPtr<FPlaceableRow>& R : g_allGlobal)
			if (R.IsValid()) DirOf.Add(R->Type, R->Directory);

		auto ScenePathFor = [&DirOf](const FString& Type) -> FString
		{
			if (Type == TEXT("PolygonVolume")) return TEXT("res://addons/bf_portal/portal_tools/types/PolygonVolume/PolygonVolume.tscn");
			if (Type == TEXT("OBBVolume"))     return TEXT("res://addons/bf_portal/portal_tools/types/OBBVolume/OBBVolume.tscn");
			if (const FString* Real = RealPath.Find(Type)) return *Real;
			// a guess now - say so, because Godot will report it as missing
			UE_LOG(LogBF6, Warning, TEXT("tscn save: no scene found in the SDK for '%s', writing a schema guess"), *Type);
			const FString* D = DirOf.Find(Type);
			if (!D || D->IsEmpty()) return FString::Printf(TEXT("res://objects/%s.tscn"), *Type);
			return FString::Printf(TEXT("res://objects/%s/%s.tscn"), **D, *Type);
		};

		// ---- gather ours, tree order: parents before children ----
		TArray<AActor*> All;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (A->Tags.Contains(kHandleTag) || A->Tags.Contains(kContextTag)) continue;
			if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag) && !A->Tags.Contains(kGroupTag)) continue;
			All.Add(A);
		}
		auto Depth = [](AActor* A)
		{
			int32 d = 0;
			for (AActor* P = A->GetAttachParentActor(); P; P = P->GetAttachParentActor()) d++;
			return d;
		};
		All.StableSort([&Depth](AActor& A, AActor& B){ return Depth((AActor*)&A) < Depth((AActor*)&B); });

		// names: unique per PARENT, the Godot rule
		TMap<AActor*, FString> NodeName, NodePathOf, ParentPath;
		TMap<FString, int32> Taken;   // "<parentpath>|<name>" -> count
		TSet<AActor*> Exported(All);
		for (AActor* A : All)
		{
			AActor* P = A->GetAttachParentActor();
			const FString PPath = (P && Exported.Contains(P)) ? NodePathOf[P] : FString(TEXT("."));
			FString Nm = A->GetActorLabel(); Nm.RemoveFromStart(TEXT("BF6_"));
			Nm = SanitizeName(Nm);
			const FString Key0 = PPath + TEXT("|") + Nm;
			if (int32* C = Taken.Find(Key0)) { Nm = FString::Printf(TEXT("%s_%d"), *Nm, ++(*C)); }
			else Taken.Add(Key0, 1);
			NodeName.Add(A, Nm);
			ParentPath.Add(A, PPath);
			NodePathOf.Add(A, PPath == TEXT(".") ? Nm : PPath + TEXT("/") + Nm);
		}

		// relative NodePath from node A to target B (Godot resolves against A)
		auto RelPath = [&NodePathOf](AActor* A, AActor* B) -> FString
		{
			TArray<FString> Pa, Pb;
			NodePathOf[A].ParseIntoArray(Pa, TEXT("/"));
			NodePathOf[B].ParseIntoArray(Pb, TEXT("/"));
			int32 Common = 0;
			while (Common < Pa.Num() && Common < Pb.Num() && Pa[Common] == Pb[Common]) Common++;
			FString R;
			for (int32 i = Common; i < Pa.Num(); i++) R += TEXT("../");
			for (int32 i = Common; i < Pb.Num(); i++) { R += Pb[i]; if (i + 1 < Pb.Num()) R += TEXT("/"); }
			return R.IsEmpty() ? FString(TEXT(".")) : R;
		};
		TMap<FString, AActor*> ByLink;
		for (AActor* A : All) { FString L = A->GetActorLabel(); L.RemoveFromStart(TEXT("BF6_")); ByLink.Add(L, A); }

		auto Swap = [](const FVector& v){ return FVector(v.X, v.Z, v.Y); };
		auto XfLine = [&GD, &Swap](const FTransform& Xf) -> FString
		{
			const FVector S3 = Xf.GetScale3D();
			const FVector R = Swap(Xf.GetUnitAxis(EAxis::X) * S3.X);
			const FVector U = Swap(Xf.GetUnitAxis(EAxis::Z) * S3.Z);
			const FVector F = Swap(Xf.GetUnitAxis(EAxis::Y) * S3.Y);
			const FVector L = Xf.GetLocation();
			return FString::Printf(TEXT("transform = Transform3D(%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)"),
				*GD(R.X), *GD(U.X), *GD(F.X), *GD(R.Y), *GD(U.Y), *GD(F.Y), *GD(R.Z), *GD(U.Z), *GD(F.Z),
				*GD(L.X / 100.0), *GD(L.Z / 100.0), *GD(L.Y / 100.0));
		};

		// ---- ext resources: one per distinct type, plus the two static scenes ----
		TArray<FString> ExtLines; TMap<FString, FString> ExtIdOf;
		auto ExtId = [&ExtLines, &ExtIdOf, &ScenePathFor](const FString& Type) -> FString
		{
			if (const FString* Have = ExtIdOf.Find(Type)) return *Have;
			const FString Id = FString::Printf(TEXT("%d_bf6%c%c"), ExtIdOf.Num() + 1,
				(TCHAR)(TEXT('a') + (ExtIdOf.Num() % 26)), (TCHAR)(TEXT('a') + ((ExtIdOf.Num() / 26) % 26)));
			ExtIdOf.Add(Type, Id);
			const FString Path = Type.StartsWith(TEXT("static://"))
				? Type.RightChop(9) : ScenePathFor(Type);
			ExtLines.Add(FString::Printf(TEXT("[ext_resource type=\"PackedScene\" path=\"%s\" id=\"%s\"]"), *Path, *Id));
			return Id;
		};
		const FString TerrainId = ExtId(TEXT("static://res://static/") + Level + TEXT("_Terrain.tscn"));
		const FString AssetsId  = ExtId(TEXT("static://res://static/") + Level + TEXT("_Assets.tscn"));

		// ---- nodes ----
		TArray<FString> SubLines, NodeLines;
		int32 UniqueId = 1, SubN = 0;
		NodeLines.Add(FString::Printf(TEXT("[node name=\"%s\" type=\"Node3D\" unique_id=%d]"), *Level, UniqueId++));
		NodeLines.Add(FString());
		NodeLines.Add(FString::Printf(TEXT("[node name=\"Static\" type=\"Node3D\" parent=\".\" unique_id=%d]"), UniqueId++));
		NodeLines.Add(FString());
		NodeLines.Add(FString::Printf(TEXT("[node name=\"%s_Terrain\" parent=\"Static\" unique_id=%d instance=ExtResource(\"%s\")]"), *Level, UniqueId++, *TerrainId));
		NodeLines.Add(FString());
		NodeLines.Add(FString::Printf(TEXT("[node name=\"%s_Assets\" parent=\"Static\" unique_id=%d instance=ExtResource(\"%s\")]"), *Level, UniqueId++, *AssetsId));

		for (AActor* A : All)
		{
			FString Type = TagValue(A, TEXT("label:"));
			if (Type.IsEmpty()) Type = TagValue(A, TEXT("type:"));
			const bool bGroup = A->Tags.Contains(kGroupTag);
			const bool bPath = !Type.IsEmpty() && BF6_TypeHasWaypoints(Type);
			AActor* P = A->GetAttachParentActor();
			const FTransform Local = (P && Exported.Contains(P))
				? A->GetActorTransform().GetRelativeTransform(P->GetActorTransform())
				: A->GetActorTransform();

			TArray<FString> Body;
			Body.Add(XfLine(Local));

			// the curve, when this node carries a path
			FString CurveRef;
			if (bPath)
				if (const TArray<FVector>* Loop = GVolumeLoops.Find(A))
				{
					const TArray<FVector> Lp = BF6_LoopToLocal(A, BF6_LoopToWorld(A, *Loop));
					FString Pts;
					for (const FVector& v : Lp)
						Pts += FString::Printf(TEXT("0, 0, 0, 0, 0, 0, %s, %s, %s, "), *GD(v.X / 100.0), *GD(v.Z / 100.0), *GD(v.Y / 100.0));
					Pts.RemoveFromEnd(TEXT(", "));
					const FString Sid = FString::Printf(TEXT("Curve3D_bf6%02d"), ++SubN);
					SubLines.Add(FString::Printf(TEXT("[sub_resource type=\"Curve3D\" id=\"%s\"]"), *Sid));
					SubLines.Add(TEXT("_data = {"));
					SubLines.Add(FString::Printf(TEXT("\"points\": PackedVector3Array(%s),"), *Pts));
					FString Tilts; for (int32 i = 0; i < Lp.Num(); i++) Tilts += (i ? TEXT(", 0") : TEXT("0"));
					SubLines.Add(FString::Printf(TEXT("\"tilts\": PackedFloat32Array(%s)"), *Tilts));
					SubLines.Add(TEXT("}"));
					SubLines.Add(FString::Printf(TEXT("point_count = %d"), Lp.Num()));
					if (BF6_PathIsClosed(A)) SubLines.Add(TEXT("closed = true"));
					SubLines.Add(FString());
					CurveRef = Sid;
				}

			// zone shape (not for paths - their points are the curve)
			//
			// ELEVATION LIVES IN THE POINTS HERE, AND ON THE NODE IN GODOT.
			// Our volume actors mostly sit at the origin with world-space loop
			// points that carry the ground height per vertex; Godot's
			// PolygonVolume is the exact opposite - flat 2D points on the
			// node's XZ plane, elevation solely from the node's translation.
			// The first cut wrote the actor transform and dropped the loop's Z,
			// which landed every zone at sea level. So the node transform is
			// DERIVED from the loop: translation at the loop's centre at its
			// lowest vertex (walls start at the floor), identity rotation, and
			// the points made relative to that - the world shape is identical
			// from either side.
			if (!bPath && Type == TEXT("PolygonVolume"))
				if (const TArray<FVector>* Loop = GVolumeLoops.Find(A))
				{
					const TArray<FVector> Wp = BF6_LoopToWorld(A, *Loop);
					FVector C = FVector::ZeroVector;
					double BaseZ = TNumericLimits<double>::Max();
					for (const FVector& v : Wp) { C += v; BaseZ = FMath::Min(BaseZ, (double)v.Z); }
					if (Wp.Num()) C /= (double)Wp.Num();
					const FTransform TW(FQuat::Identity, FVector(C.X, C.Y, BaseZ));
					Body[0] = XfLine((P && Exported.Contains(P))
						? TW.GetRelativeTransform(P->GetActorTransform()) : TW);
					FString Pts;
					for (const FVector& v : Wp)
						Pts += FString::Printf(TEXT("%s, %s, "), *GD((v.X - C.X) / 100.0), *GD((v.Y - C.Y) / 100.0));
					Pts.RemoveFromEnd(TEXT(", "));
					Body.Add(FString::Printf(TEXT("points = PackedVector2Array(%s)"), *Pts));
					const FString H = GetActorProp(A, TEXT("height"));
					Body.Add(FString::Printf(TEXT("height = %s"), H.IsNumeric() ? *H : TEXT("0.0")));
					FLinearColor VC;
					if (BF6_VolumeColorOf(A, VC))
						Body.Add(FString::Printf(TEXT("color = Color(%s, %s, %s, %s)"), *GD(VC.R), *GD(VC.G), *GD(VC.B), *GD(VC.A)));
				}
			if (Type == TEXT("OBBVolume"))
			{
				const FVector Sz = BF6_ObbSizeGodot(A);
				Body.Add(FString::Printf(TEXT("size = Vector3(%s, %s, %s)"), *GD(Sz.X), *GD(Sz.Y), *GD(Sz.Z)));
				FLinearColor VC;
				if (BF6_VolumeColorOf(A, VC))
					Body.Add(FString::Printf(TEXT("color = Color(%s, %s, %s, %s)"), *GD(VC.R), *GD(VC.G), *GD(VC.B), *GD(VC.A)));
			}
			if (!CurveRef.IsEmpty())
				Body.Add(FString::Printf(TEXT("curve = SubResource(\"%s\")"), *CurveRef));

			// fields: only what the creator set, links split into node_paths
			TArray<FString> LinkNames;
			if (!bGroup && !Type.IsEmpty())
				for (const BF6Api::FPropDef& D : PropsForType(Type))
				{
					if (D.Name == TEXT("Waypoints")) continue;   // the curve IS the waypoints here
					FString V = GetActorProp(A, D.Name);
					if (V.IsEmpty() || V.Contains(TEXT("NodePath")) || V.Contains(TEXT("ExtResource"))) continue;
					const bool bLink = D.Type.Contains(TEXT("Volume")) || D.Type.Contains(TEXT("Array["))
						|| D.Type.Contains(TEXT("Path")) || D.Type.Contains(TEXT("SpawnPoint"));
					if (bLink)
					{
						TArray<FString> Parts; V.ParseIntoArray(Parts, TEXT(","));
						TArray<FString> Rels;
						for (FString& Pt : Parts)
							if (AActor* const* T = ByLink.Find(Pt.TrimStartAndEnd()))
								Rels.Add(FString::Printf(TEXT("NodePath(\"%s\")"), *RelPath(A, *T)));
						if (Rels.Num() == 0) continue;
						LinkNames.Add(D.Name);
						if (D.Type.Contains(TEXT("Array[")))
							Body.Add(FString::Printf(TEXT("%s = [%s]"), *D.Name, *FString::Join(Rels, TEXT(", "))));
						else
							Body.Add(FString::Printf(TEXT("%s = %s"), *D.Name, *Rels[0]));
						continue;
					}
					if (D.Type == TEXT("selection"))
					{
						const int32 Idx = D.Options.IndexOfByKey(V);
						if (Idx >= 0) Body.Add(FString::Printf(TEXT("%s = %d"), *D.Name, Idx));
					}
					else if (D.Type == TEXT("bool"))
						Body.Add(FString::Printf(TEXT("%s = %s"), *D.Name, V.Equals(TEXT("true"), ESearchCase::IgnoreCase) ? TEXT("true") : TEXT("false")));
					else if (D.Type == TEXT("int") || D.Type == TEXT("float"))
					{
						if (V.IsNumeric()) Body.Add(FString::Printf(TEXT("%s = %s"), *D.Name, *V));
					}
					else if (D.Type == TEXT("vector"))
					{
						TArray<FString> Pt; V.ParseIntoArray(Pt, TEXT(","));
						if (Pt.Num() == 3) Body.Add(FString::Printf(TEXT("%s = Vector3(%s, %s, %s)"), *D.Name, *Pt[0].TrimStartAndEnd(), *Pt[1].TrimStartAndEnd(), *Pt[2].TrimStartAndEnd()));
					}
					else
						Body.Add(FString::Printf(TEXT("%s = \"%s\""), *D.Name, *V));
				}

			FString Head;
			if (bGroup || Type.IsEmpty())
				Head = FString::Printf(TEXT("[node name=\"%s\" type=\"Node3D\" parent=\"%s\" unique_id=%d]"),
					*NodeName[A], *ParentPath[A], UniqueId++);
			else
			{
				FString NP;
				if (LinkNames.Num())
				{
					TArray<FString> Q; for (const FString& L : LinkNames) Q.Add(FString::Printf(TEXT("\"%s\""), *L));
					NP = FString::Printf(TEXT(" node_paths=PackedStringArray(%s)"), *FString::Join(Q, TEXT(", ")));
				}
				Head = FString::Printf(TEXT("[node name=\"%s\" parent=\"%s\"%s unique_id=%d instance=ExtResource(\"%s\")]"),
					*NodeName[A], *ParentPath[A], *NP, UniqueId++, *ExtId(Type));
			}
			NodeLines.Add(FString());
			NodeLines.Add(Head);
			NodeLines.Append(Body);
		}

		FString Out = FString::Printf(TEXT("[gd_scene load_steps=%d format=3]\n\n"), ExtLines.Num() + SubN + 1);
		Out += FString::Join(ExtLines, TEXT("\n")) + TEXT("\n\n");
		if (SubLines.Num()) Out += FString::Join(SubLines, TEXT("\n")) + TEXT("\n");
		Out += FString::Join(NodeLines, TEXT("\n")) + TEXT("\n");
		return Out;
	}

	bool SaveAsTscn()
	{
		if (!g_ss.bEditing) { Notify(TEXT("Open a custom map first - the base map already ships as a Godot scene.")); return false; }
		const FString Text = BF6_BuildTscn();
		if (Text.IsEmpty()) { Notify(TEXT("Nothing to write.")); return false; }
		IDesktopPlatform* DP = FDesktopPlatformModule::Get(); if (!DP) return false;
		const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		TArray<FString> Files;
		const FString DefaultName = g_ss.CurrentSave + TEXT(".tscn");
		if (!DP->SaveFileDialog(Parent, TEXT("Save as Godot scene (.tscn)"), FPaths::ProjectSavedDir(), DefaultName,
			TEXT("Godot scene|*.tscn"), EFileDialogFlags::None, Files) || Files.Num() == 0) return false;
		if (!FFileHelper::SaveStringToFile(Text, *Files[0]))
		{
			Notify(FString::Printf(TEXT("Could not write %s."), *Files[0]));
			return false;
		}
		Notify(FString::Printf(TEXT("Saved '%s' as a Godot scene. Drop it in the SDK's User_Created levels folder and it opens in Godot."), *FPaths::GetCleanFilename(Files[0])));
		return true;
	}

	void SaveCurrent(bool bSilent)
	{
		if (g_ss.CurrentSave.IsEmpty()) { if (!bSilent) Notify(TEXT("Name and create your custom map first.")); return; }
		if (!SaveSession(g_ss.CurrentLevel, g_ss.CurrentSave))
		{
			// Always spoken, even on the silent autosave path: a save that is
			// not happening is exactly what a creator must not find out later.
			Notify(FString::Printf(TEXT("Could not save '%s' - the file could not be written."), *g_ss.CurrentSave));
			return;
		}
		if (!bSilent) Notify(FString::Printf(TEXT("Saved '%s'."), *g_ss.CurrentSave));
	}

	void CreateCustom(const FString& Name)
	{
		const FString Clean = Name.TrimStartAndEnd();
		FString Why;
		if (!BF6_ValidSaveName(Clean, Why)) { Notify(Why); return; }
		// An existing name would be OVERWRITTEN, and creating happens from the
		// read-only base, so the world being written is empty - the old save
		// would not be merged into, it would be erased. Ask.
		if (FPaths::FileExists(BF6_SessionPathFor(g_ss.CurrentLevel, Clean)))
		{
			const FString Q = FString::Printf(TEXT("A custom map called '%s' already exists for this level.\n\nCreating it again REPLACES it with an empty one. The objects in the old save are lost.\n\nReplace it?"), *Clean);
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Q)) != EAppReturnType::Yes)
			{
				Notify(TEXT("Left the existing custom map alone."));
				return;
			}
		}
		// Session state changes only once the file is really there. Adopting the
		// name first meant a failed write left the tool "editing" a save that
		// did not exist, and the next save would look like it had worked too.
		const FString Was = g_ss.CurrentSave; const bool bWas = g_ss.bEditing;
		g_ss.CurrentSave = Clean;
		g_ss.bEditing = true;
		if (!SaveSession(g_ss.CurrentLevel, g_ss.CurrentSave))
		{
			g_ss.CurrentSave = Was; g_ss.bEditing = bWas;
			Notify(FString::Printf(TEXT("Could not create '%s' - the file could not be written. Try a simpler name."), *Clean));
			return;
		}
		Notify(FString::Printf(TEXT("Custom map '%s' created - aim and press SPACE to place objects."), *Clean));
	}

	// ---- object attributes ----
	TArray<FPropDef> PropsForType(const FString& Type)
	{
		// memoized: this is called per OBJECT during export/lint/panel ticks,
		// and each miss is an FFI round-trip into bf6_core - uncached, a 4k
		// object map paid seconds of schema lookups on every size estimate
		if (const TArray<FPropDef>* C = g_propCache.Find(Type)) return *C;
		TArray<FPropDef> Out;
		if (!g_ctx || !g_props || Type.IsEmpty()) return Out;
		bf6_prop Buf[64];
		const int n = FMath::Min(g_props(g_ctx, TCHAR_TO_UTF8(*Type), Buf, 64), 64);
		for (int i = 0; i < n; i++)
		{
			FPropDef D;
			D.Name = UTF8_TO_TCHAR(Buf[i].name);
			D.Type = UTF8_TO_TCHAR(Buf[i].type);
			D.Default = UTF8_TO_TCHAR(Buf[i].def);
			const FString Sel = UTF8_TO_TCHAR(Buf[i].selections);
			if (!Sel.IsEmpty()) Sel.ParseIntoArray(D.Options, TEXT("\n"));
			Out.Add(D);
		}
		g_propCache.Add(Type, Out);
		return Out;
	}

	AActor* SelectedGameplayActor(FString& OutType)
	{
		OutType.Reset();
		if (!GEditor) return nullptr;
		USelection* Sel = GEditor->GetSelectedActors();
		if (!Sel) return nullptr;
		for (int32 i = 0; i < Sel->Num(); i++)
		{
			AActor* A = Cast<AActor>(Sel->GetSelectedObject(i));
			if (!A) continue;
			// a node has no type of its own, but the radial still has to open on it:
			// carrying or multiplying one takes its whole subtree
			if (A->Tags.Contains(kGroupTag)) { OutType = TEXT("Node3D"); return A; }
			if (!A->Tags.Contains(kBaseTag) && !A->Tags.Contains(kPlacedTag)) continue;
			FString Ty = TagValue(A, TEXT("label:"));
			if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
			if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("mesh:"));
			if (Ty.IsEmpty()) continue;
			OutType = Ty;
			return A;
		}
		return nullptr;
	}

	FString GetActorProp(AActor* A, const FString& Key, const FString& Fallback)
	{
		if (!A) return Fallback;
		const FString V = TagValue(A, FString::Printf(TEXT("p:%s="), *Key));
		return V.IsEmpty() ? Fallback : V;
	}

	void SetActorProp(AActor* A, const FString& Key, const FString& Value)
	{
		if (!A) return;
		// undoable: record the actor before mutating its tags
		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Edit %s"), *Key)));
		A->Modify();
		const FString Prefix = FString::Printf(TEXT("p:%s="), *Key);
		for (int32 i = A->Tags.Num() - 1; i >= 0; i--)
			if (A->Tags[i].ToString().StartsWith(Prefix)) A->Tags.RemoveAt(i);
		A->Tags.Add(FName(*(Prefix + Value)));
		// a zone's height is part of its look: stretch the walls immediately
		if (Key.Equals(TEXT("height"), ESearchCase::IgnoreCase))
			if (const TArray<FVector>* Lp = GVolumeLoops.Find(A))
				RebuildVolumeWalls(A, *Lp);
		// same for a box volume's size (re-enter the edit to reseat the handles)
		if (Key.Equals(TEXT("size"), ESearchCase::IgnoreCase) && IsObbActor(A))
		{
			RebuildObbBox(A);
			if (IsObbEditing()) { FinishObbEdit(); BeginObbEdit(A); }
		}
	}

	// ---- zone point editing ----
	bool IsVolumeActor(AActor* A) { return A && GVolumeLoops.Contains(A); }
	bool IsVolumeEditing() { return GVolEdit.Volume.IsValid(); }
	static void BF6_ProjectZoneDots();   // defined with the dots section below

	bool IsPathSelectedVolume()
	{
		return GVolEdit.Volume.IsValid() && BF6_IsPathActor(GVolEdit.Volume.Get());
	}

	bool TogglePathClosed()
	{
		AActor* A = GVolEdit.Volume.Get();
		if (!A || !BF6_IsPathActor(A)) return false;
		const bool bNow = !BF6_PathIsClosed(A);
		A->Modify();
		SetActorProp(A, TEXT("isClosed"), bNow ? TEXT("true") : TEXT("false"));
		if (const TArray<FVector>* Loop = GVolumeLoops.Find(A)) RebuildVolumeWalls(A, *Loop);
		BF6_Redraw();
		return bNow;
	}

	void BeginVolumeEdit(AActor* Volume)
	{
		if (!GEditor || !Volume) return;
		FinishVolumeEdit();
		const TArray<FVector>* Loop = GVolumeLoops.Find(Volume);
		if (!Loop || Loop->Num() < 3) { Notify(TEXT("No editable points on this volume.")); return; }
		GVolEdit = FBF6VolEdit();
		GVolEdit.Volume = Volume;
		// (no toast: the top-left CONTROLS panel shows the zone bindings)
	}

	bool VolumeNearestEdgePoint(const FVector& WorldPos, FVector& OutPoint)
	{
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L || L->Num() < 3) return false;
		const TArray<FVector> Loop = BF6_LoopToWorld(Vol, *L);
		float BestD = FLT_MAX;
		for (int32 i = 0; i < Loop.Num(); i++)
		{
			const FVector P = FMath::ClosestPointOnSegment(WorldPos, Loop[i], Loop[(i + 1) % Loop.Num()]);
			const float D = FVector::Dist2D(WorldPos, P);
			if (D < BestD) { BestD = D; OutPoint = P; }
		}
		return BestD != FLT_MAX;
	}

	void TickVolumeEdit()
	{
		if (!IsVolumeEditing()) return;
		AActor* Vol = GVolEdit.Volume.Get();
		if (!Vol) { GVolEdit = FBF6VolEdit(); return; }
		// refresh the screen-space cache the dot layer paints from (this also
		// computes the Ctrl edge preview, in screen space against the cursor)
		BF6_ProjectZoneDots();
		// throttled diagnostics: dot count + a sample position, so "I don't see
		// the dots" is answerable from the log without screenshots
		{
			static double LastLog = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (Now - LastLog > 2.0)
			{
				LastLog = Now;
				TArray<FVector2D> Px; int32 Nn, Ac, Dr; FVector2D Ep; bool bEp;
				if (GetZoneDots(Px, Nn, Ac, Dr, Ep, bEp))
				{
					int32 OnScreen = 0; FVector2D First(-1, -1);
					for (const FVector2D& P : Px)
						if (P.X > -999.f) { if (OnScreen == 0) First = P; OnScreen++; }
					UE_LOG(LogBF6, Log, TEXT("ZoneDots: %d handles (%d pts), %d projectable, first at (%.0f, %.0f), active %d"),
						Px.Num(), Nn, OnScreen, First.X, First.Y, Ac);
				}
				else UE_LOG(LogBF6, Log, TEXT("ZoneDots: GetZoneDots returned FALSE while editing"));
			}
		}
	}

	// fwd: defined with the zone-centre helpers below, used by the point edits
	static bool BF6_VolumeRecenter(AActor* Vol, bool bOwnTransaction);

	void VolumeAddPoint()
	{
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L || L->Num() < 3) return;
		TArray<FVector> World = BF6_LoopToWorld(Vol, *L);
		const int32 Sel = FMath::Clamp(GVolEdit.Active, 0, World.Num() - 1);
		const FVector Mid = (World[Sel] + World[(Sel + 1) % World.Num()]) * 0.5f;
		FScopedTransaction Tx(FText::FromString(TEXT("Add Zone Point")));
		World.Insert(Mid, Sel + 1);
		BF6_ApplyLoop(Vol, BF6_LoopToLocal(Vol, World));
		GVolEdit.Active = Sel + 1;
	
		// The shape changed, so the origin is stale - see BF6_VolumeRecenter.
		BF6_VolumeRecenter(GVolEdit.Volume.Get(), true);
	}

	void VolumeAddPointAt(const FVector& WorldPos)
	{
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L || L->Num() < 3) return;
		TArray<FVector> World = BF6_LoopToWorld(Vol, *L);
		// nearest edge segment to the click
		int32 Best = 0; float BestD = FLT_MAX; FVector BestP = WorldPos;
		for (int32 i = 0; i < World.Num(); i++)
		{
			const FVector A = World[i], B = World[(i + 1) % World.Num()];
			const FVector P = FMath::ClosestPointOnSegment(WorldPos, A, B);
			const float D = FVector::Dist2D(WorldPos, P);
			if (D < BestD) { BestD = D; Best = i; BestP = P; }
		}
		FScopedTransaction Tx(FText::FromString(TEXT("Add Zone Point")));
		World.Insert(BestP, Best + 1);
		BF6_ApplyLoop(Vol, BF6_LoopToLocal(Vol, World));
		GVolEdit.Active = Best + 1;
	}

	void TickZoneAutoEdit()
	{
		if (!GEditor || !g_ss.bEditing) return;
		USelection* S = GEditor->GetSelectedActors();
		if (!S) return;

		if (IsVolumeEditing())
		{
			// Godot-like: deselecting the zone ends the edit (dot clicks are
			// consumed by the input layer, so they never change the selection)
			AActor* Vol = GVolEdit.Volume.Get();
			bool bStillSelected = false;
			for (int32 i = 0; i < S->Num() && !bStillSelected; i++)
				bStillSelected = (S->GetSelectedObject(i) == Vol);
			if (!bStillSelected && GVolEdit.Drag == INDEX_NONE)
				FinishVolumeEdit();
			// fall through: selecting ANOTHER volume starts editing it below
		}

		if (IsObbEditing())
		{
			// the box's face handles are actors: while they're up, only they are
			// selectable so buried faces stay grabbable
			TArray<AActor*> Strip;
			for (int32 i = 0; i < S->Num(); i++)
				if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
					if (!A->Tags.Contains(kHandleTag)) Strip.Add(A);
			for (AActor* A : Strip) GEditor->SelectActor(A, false, true);
			return;
		}

		// selecting a zone or a box volume begins its edit automatically
		AActor* Sel = nullptr;
		for (int32 i = 0; i < S->Num() && !Sel; i++) Sel = Cast<AActor>(S->GetSelectedObject(i));
		if (Sel && GVolumeLoops.Contains(Sel) && GVolEdit.Volume.Get() != Sel)
			BeginVolumeEdit(Sel);
		else if (Sel && IsObbActor(Sel))
			BeginObbEdit(Sel);
	}

	void VolumeDeletePoint()
	{
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L) return;
		if (L->Num() <= 3) { Notify(TEXT("A zone needs at least 3 points.")); return; }
		TArray<FVector> World = BF6_LoopToWorld(Vol, *L);
		const int32 Sel = FMath::Clamp(GVolEdit.Active, 0, World.Num() - 1);
		FScopedTransaction Tx(FText::FromString(TEXT("Delete Zone Point")));
		World.RemoveAt(Sel);
		BF6_ApplyLoop(Vol, BF6_LoopToLocal(Vol, World));
		GVolEdit.Active = FMath::Clamp(Sel, 0, World.Num() - 1);
	
		// The shape changed, so the origin is stale - see BF6_VolumeRecenter.
		BF6_VolumeRecenter(GVolEdit.Volume.Get(), true);
	}

	// ---- OBB face-handle editing (Godot's OBBVolumeGizmo behavior) ----
	// Six handles, one per face. Dragging a handle moves THAT face along its
	// axis (the box re-centres); holding ALT resizes symmetrically about the
	// centre - both straight from the SDK's gizmo source.
	struct FBF6ObbEdit
	{
		TWeakObjectPtr<AActor> Obb;
		TArray<TWeakObjectPtr<AActor>> Handles;   // +X,-X,+Y,-Y,+Z,-Z (Unreal local)
		TArray<FVector> Expected;                 // world positions we last placed them at
	};
	static FBF6ObbEdit GObbEdit;

	bool IsObbActor(AActor* A) { return A && A->Tags.Contains(kObbTag); }
	bool IsObbEditing() { return GObbEdit.Handles.Num() > 0; }

	static FVector BF6_ObbFaceLocal(int32 i, const FVector& FullUE)
	{
		const int32 Axis = i / 2;
		const float Sign = (i % 2 == 0) ? 1.f : -1.f;
		FVector L = FVector::ZeroVector;
		L[Axis] = Sign * FullUE[Axis] * 0.5f;
		return L;
	}

	static void BF6_PlaceObbHandles(AActor* Obb)
	{
		const FVector Full = BF6_ObbSizeUE(BF6_ObbSizeGodot(Obb));
		const FTransform Xf = Obb->GetActorTransform();
		GObbEdit.Expected.SetNum(GObbEdit.Handles.Num());
		for (int32 i = 0; i < GObbEdit.Handles.Num(); i++)
			if (AActor* H = GObbEdit.Handles[i].Get())
			{
				const FVector Wp = Xf.TransformPosition(BF6_ObbFaceLocal(i, Full));
				H->SetActorLocation(Wp);
				GObbEdit.Expected[i] = Wp;
			}
	}

	void BeginObbEdit(AActor* Obb)
	{
		if (!GEditor || !Obb) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		FinishObbEdit();
		GObbEdit.Obb = Obb;
		for (int32 i = 0; i < 6; i++)
			GObbEdit.Handles.Add(SpawnVolumeHandle(W, FVector::ZeroVector, i));
		BF6_PlaceObbHandles(Obb);
		if (GObbEdit.Handles[0].IsValid())
		{
			GEditor->SelectNone(false, true, false);
			GEditor->SelectActor(GObbEdit.Handles[0].Get(), true, true);
		}
		// (no toast: the top-left CONTROLS panel shows the box bindings)
	}

	void FinishObbEdit()
	{
		if (GEditor)
			if (UWorld* W = GEditor->GetEditorWorldContext().World())
				for (const TWeakObjectPtr<AActor>& H : GObbEdit.Handles)
					if (H.IsValid()) W->EditorDestroyActor(H.Get(), false);
		GObbEdit = FBF6ObbEdit();
	}

	void TickObbEdit()
	{
		if (!IsObbEditing()) return;
		AActor* Obb = GObbEdit.Obb.Get();
		if (!Obb) { GObbEdit = FBF6ObbEdit(); return; }
		// a handle deleted (Del / undo) ends the session cleanly
		for (const TWeakObjectPtr<AActor>& H : GObbEdit.Handles)
			if (!H.IsValid()) { FinishObbEdit(); return; }

		// which handle moved?
		int32 Moved = INDEX_NONE;
		for (int32 i = 0; i < 6 && Moved == INDEX_NONE; i++)
			if (!GObbEdit.Handles[i]->GetActorLocation().Equals(GObbEdit.Expected[i], 0.5f)) Moved = i;
		if (Moved == INDEX_NONE) return;

		const FTransform Xf = Obb->GetActorTransform();
		const int32 Axis = Moved / 2;
		const float Sign = (Moved % 2 == 0) ? 1.f : -1.f;
		FVector SizeG = BF6_ObbSizeGodot(Obb);
		FVector FullUE = BF6_ObbSizeUE(SizeG);
		const float Half = FullUE[Axis] * 0.5f;
		// the face's new position along its own local axis (other axes ignored)
		const float f = (float)Xf.InverseTransformPosition(GObbEdit.Handles[Moved]->GetActorLocation())[Axis];

		const bool bSymmetric = FSlateApplication::Get().GetModifierKeys().IsAltDown();
		float NewFull;
		if (bSymmetric)
		{
			NewFull = FMath::Max(50.f, 2.f * FMath::Abs(f));
		}
		else
		{
			NewFull = FMath::Max(50.f, Sign * f + Half);
			// keep the opposite face where it was: shift the centre along the axis
			FVector LocalShift = FVector::ZeroVector;
			LocalShift[Axis] = Sign * (NewFull * 0.5f - Half);
			Obb->SetActorLocation(Obb->GetActorLocation() + Xf.TransformVector(LocalShift));
		}
		FullUE[Axis] = NewFull;
		// back to Godot metres (UE x,y,z -> Godot x,z,y)
		SizeG = FVector(FullUE.X, FullUE.Z, FullUE.Y) / 100.f;
		BF6_SetObbSizeTag(Obb, SizeG);
		RebuildObbBox(Obb);
		BF6_PlaceObbHandles(Obb);
	}

	void VolumeDeletePointAt(const FVector& WorldPos)
	{
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L) return;
		if (L->Num() <= 3) { Notify(TEXT("A zone needs at least 3 points.")); return; }
		TArray<FVector> World = BF6_LoopToWorld(Vol, *L);
		// nearest point to the click, with a grab radius so a miss does nothing
		int32 Best = INDEX_NONE; float BestD = 200.f;
		for (int32 i = 0; i < World.Num(); i++)
		{
			const float D = FVector::Dist2D(WorldPos, World[i]);
			if (D < BestD) { BestD = D; Best = i; }
		}
		if (Best == INDEX_NONE) return;
		FScopedTransaction Tx(FText::FromString(TEXT("Delete Zone Point")));
		World.RemoveAt(Best);
		BF6_ApplyLoop(Vol, BF6_LoopToLocal(Vol, World));
		GVolEdit.Active = FMath::Clamp(Best, 0, World.Num() - 1);
	}

	void FinishVolumeEdit()
	{
		EndZoneDotDrag();   // closes any open drag transaction
		GVolEdit = FBF6VolEdit();
	}

	// ---- the screen-space dots themselves (painted by the build overlay) ----
	// TICK-side projector: fills the cache the paint layer and the mouse
	// hit-test read (projecting inside OnPaint silently failed). Two handles
	// per point (bottom + top ring, like Godot); the Ctrl edge preview is
	// computed in SCREEN space against the bottom ring so it hugs the cursor.
	static void BF6_ProjectZoneDots()
	{
		GVolEdit.CachedPx.Reset();
		GVolEdit.CachedN = 0;
		GVolEdit.bCachedEdge = false;
		GVolEdit.bEdgeValid = false;
		GVolEdit.EdgeSeg = INDEX_NONE;
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L || L->Num() < 3) return;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return;
		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(VC->Viewport, VC->GetScene(), VC->EngineShowFlags));
		FSceneView* View = VC->CalcSceneView(&Family);
		if (!View) return;

		// the volume's drawn height (matches RebuildVolumeWalls)
		double H = 5.0;
		const FString HS = GetActorProp(Vol, TEXT("height"));
		if (HS.IsNumeric()) H = FCString::Atod(*HS);
		if (H <= 0.01) H = 5.0;
		const float TopZ = (float)H * 100.f;

		auto Project = [&](const FVector& W) -> FVector2D
		{
			FVector2D Px(-10000.f, -10000.f);
			const FVector4 Clip = View->WorldToScreen(W);
			if (Clip.W > 0.f) View->ScreenToPixel(Clip, Px);
			return Px;
		};

		const int32 N = L->Num();
		GVolEdit.CachedN = N;
		TArray<FVector> BottomW; BottomW.Reserve(N);
		const FTransform Xf = Vol->GetActorTransform();
		for (int32 i = 0; i < N; i++) BottomW.Add(Xf.TransformPosition((*L)[i]));
		for (int32 i = 0; i < N; i++) GVolEdit.CachedPx.Add(Project(BottomW[i]));
		for (int32 i = 0; i < N; i++) GVolEdit.CachedPx.Add(Project(Xf.TransformPosition((*L)[i] + FVector(0, 0, TopZ))));

		// Ctrl edge preview: nearest bottom-ring segment to the MOUSE, in pixels
		if (GVolEdit.Drag == INDEX_NONE && FSlateApplication::Get().GetModifierKeys().IsControlDown())
		{
			const FVector2D M((float)VC->Viewport->GetMouseX(), (float)VC->Viewport->GetMouseY());
			float BestD = FLT_MAX; int32 BestSeg = INDEX_NONE; float BestT = 0.f;
			for (int32 i = 0; i < N; i++)
			{
				const FVector2D A = GVolEdit.CachedPx[i], B = GVolEdit.CachedPx[(i + 1) % N];
				if (A.X < -999.f || B.X < -999.f) continue;
				const FVector2D AB = B - A;
				const float Len2 = (float)AB.SizeSquared();
				const float T = Len2 > 1.f ? FMath::Clamp((float)FVector2D::DotProduct(M - A, AB) / Len2, 0.f, 1.f) : 0.f;
				const float D = (float)FVector2D::Distance(M, A + AB * T);
				if (D < BestD) { BestD = D; BestSeg = i; BestT = T; }
			}
			if (BestSeg != INDEX_NONE)
			{
				GVolEdit.EdgeSeg = BestSeg;
				GVolEdit.EdgeWorld = FMath::Lerp(BottomW[BestSeg], BottomW[(BestSeg + 1) % N], BestT);
				GVolEdit.bEdgeValid = true;
				GVolEdit.CachedEdgePx = Project(GVolEdit.EdgeWorld);
				GVolEdit.bCachedEdge = GVolEdit.CachedEdgePx.X > -999.f;
			}
		}
	}

	bool GetZoneDots(TArray<FVector2D>& OutPx, int32& OutPointCount, int32& OutActive, int32& OutDrag, FVector2D& OutEdgePx, bool& bOutEdge)
	{
		OutPx.Reset(); bOutEdge = false; OutActive = INDEX_NONE; OutDrag = INDEX_NONE; OutPointCount = 0;
		if (!IsVolumeEditing() || GVolEdit.CachedPx.Num() < 6) return false;
		OutPx = GVolEdit.CachedPx;
		OutPointCount = GVolEdit.CachedN;
		OutActive = GVolEdit.Active;
		OutDrag = GVolEdit.Drag;
		OutEdgePx = GVolEdit.CachedEdgePx;
		bOutEdge = GVolEdit.bCachedEdge;
		return true;
	}

	// Ctrl+LMB: insert exactly where the edge preview shows
	void VolumeAddPointAtPreview()
	{
		if (!GVolEdit.bEdgeValid || GVolEdit.EdgeSeg == INDEX_NONE) return;
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L) return;
		TArray<FVector> World = BF6_LoopToWorld(Vol, *L);
		if (!World.IsValidIndex(GVolEdit.EdgeSeg)) return;
		FScopedTransaction Tx(FText::FromString(TEXT("Add Zone Point")));
		World.Insert(GVolEdit.EdgeWorld, GVolEdit.EdgeSeg + 1);
		BF6_ApplyLoop(Vol, BF6_LoopToLocal(Vol, World));
		GVolEdit.Active = GVolEdit.EdgeSeg + 1;
	}

	// Godot's "Reset Center": move the zone's origin to its points' centroid
	// and offset the points so the shape stays put in the world. Fixes a
	// pivot that sits far from the zone (the gizmo and rotation act around
	// empty space otherwise).
	// PUT THE ORIGIN BACK IN THE MIDDLE OF THE SHAPE.
	//
	// Geometrically neutral: the actor moves and the loop is rewritten to
	// compensate, so the zone does not shift by a millimetre. What changes is
	// where the gizmo sits, and an origin left behind by an edit is a gizmo
	// floating somewhere off the shape it is supposed to move.
	//
	// bOwnTransaction is false while a drag is still open - the recentre then
	// folds into the drag's own transaction, so one Ctrl+Z reverts the whole
	// gesture instead of leaving the shape moved and the origin not.
	// THE GIZMO DOES NOT FOLLOW AN ACTOR THAT MOVES UNDERNEATH IT.
	//
	// The editor caches a pivot for the selection and only recomputes it when
	// the selection changes - which is exactly why the recentred origin looked
	// like it was waiting for a reselect. It was: the actor had already moved,
	// and the thing drawn on screen had not been told.
	static void BF6_NotePivotMoved()
	{
		// Through the editor engine, which is what owns the cached pivot.
		if (UUnrealEdEngine* Ed = Cast<UUnrealEdEngine>(GEditor))
			Ed->UpdatePivotLocationForSelection();
	}

	static bool BF6_VolumeRecenter(AActor* Vol, bool bOwnTransaction)
	{
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L || L->Num() < 2) return false;
		const TArray<FVector> World = BF6_LoopToWorld(Vol, *L);
		FVector C = FVector::ZeroVector;
		for (const FVector& P : World) C += P;
		C /= (double)World.Num();
		if (FVector::Dist(C, Vol->GetActorLocation()) < 1.0) return false;   // already centred
		if (bOwnTransaction)
		{
			FScopedTransaction Tx(FText::FromString(TEXT("Reset Zone Center")));
			Vol->Modify();
			Vol->SetActorLocation(C);
			BF6_ApplyLoop(Vol, BF6_LoopToLocal(Vol, World));
			BF6_NotePivotMoved();
			return true;
		}
		Vol->Modify();
		Vol->SetActorLocation(C);
		BF6_ApplyLoop(Vol, BF6_LoopToLocal(Vol, World));
		BF6_NotePivotMoved();
		return true;
	}

	bool ResetVolumeCenter() { return BF6_VolumeRecenter(GVolEdit.Volume.Get(), true); }

	// Ctrl+RMB on a dot: delete that point (index straight from the hit test)
	void VolumeDeletePointByIndex(int32 RawIndex)
	{
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L || GVolEdit.CachedN <= 0) return;
		if (L->Num() <= 3) { Notify(TEXT("A zone needs at least 3 points.")); return; }
		const int32 Point = RawIndex % GVolEdit.CachedN;
		if (!L->IsValidIndex(Point)) return;
		TArray<FVector> World = BF6_LoopToWorld(Vol, *L);
		FScopedTransaction Tx(FText::FromString(TEXT("Delete Zone Point")));
		World.RemoveAt(Point);
		BF6_ApplyLoop(Vol, BF6_LoopToLocal(Vol, World));
		GVolEdit.Active = FMath::Clamp(Point, 0, World.Num() - 1);
	}

	// IS THE ZONE BEING EDITED UNBOUNDED?
	//
	// Height 0 means infinite, and an infinite zone is DRAWN at 5 m - which
	// makes it pixel-identical to a genuine 5 m zone. The only thing that ever
	// said otherwise was a one-off toast when you dragged through zero, gone a
	// second later and never shown at all for a zone somebody else authored.
	bool IsEditZoneInfinite()
	{
		AActor* Vol = GVolEdit.Volume.Get();
		if (!Vol) return false;
		const FString HS = GetActorProp(Vol, TEXT("height"));
		return !HS.IsNumeric() || FCString::Atod(*HS) <= 0.01;
	}

	int32 ZoneDotUnderMouse()
	{
		TArray<FVector2D> Px; int32 N, A, D; FVector2D E; bool bE;
		if (!GetZoneDots(Px, N, A, D, E, bE)) return INDEX_NONE;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return INDEX_NONE;
		const FVector2D M((float)VC->Viewport->GetMouseX(), (float)VC->Viewport->GetMouseY());
		const float Grab = 14.f * VC->GetDPIScale();
		int32 Best = INDEX_NONE; float BestD = Grab;
		for (int32 i = 0; i < Px.Num(); i++)
		{
			const float Dist = FVector2D::Distance(M, Px[i]);
			if (Dist < BestD) { BestD = Dist; Best = i; }
		}
		return Best;
	}

	bool IsZoneDotDragging() { return GVolEdit.Drag != INDEX_NONE; }

	void BeginZoneDotDrag(int32 Index)
	{
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		const int32 N = GVolEdit.CachedN;
		if (!L || N <= 0 || !GVolEdit.CachedPx.IsValidIndex(Index)) return;
		const int32 Point = Index % N;
		if (!L->IsValidIndex(Point)) return;
		GVolEdit.Active = Point;
		GVolEdit.Drag = Index;
		// the drag slides on the HANDLE's own height plane (top handles drag up
		// high, bottom handles at the base), but only the point's X/Y change
		const FVector BottomW = BF6_LoopToWorld(Vol, *L)[Point];
		GVolEdit.DragBottomZ = BottomW.Z;
		if (Index < N) GVolEdit.DragZ = BottomW.Z;
		else
		{
			double H = 5.0;
			const FString HS = GetActorProp(Vol, TEXT("height"));
			if (HS.IsNumeric()) H = FCString::Atod(*HS);
			if (H <= 0.01) H = 5.0;
			GVolEdit.DragZ = BottomW.Z + H * 100.0;
		}
		if (GEditor)
		{
			GEditor->BeginTransaction(FText::FromString(TEXT("Move Zone Point")));
			GVolEdit.bTx = true;
			Vol->Modify();
		}
	}

	void DragZoneDotToCursor()
	{
		AActor* Vol = GVolEdit.Volume.Get();
		if (!Vol || GVolEdit.Drag == INDEX_NONE) return;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC) return;
		const TArray<FVector>* L = GVolumeLoops.Find(Vol);
		const int32 N = GVolEdit.CachedN;
		if (!L || N <= 0) return;
		const int32 Point = GVolEdit.Drag % N;
		if (!L->IsValidIndex(Point)) return;
		const FViewportCursorLocation Cur = VC->GetCursorWorldLocationFromMousePos();
		const FVector O = Cur.GetOrigin(), Dir = Cur.GetDirection();

		if (GVolEdit.Drag >= N)
		{
			// TOP ring: drag the zone's HEIGHT vertically, like Godot's gizmo.
			// Closest point of the cursor ray to the vertical axis through the
			// grabbed corner gives the new top.
			const FVector Base = BF6_LoopToWorld(Vol, *L)[Point];
			const FVector U(0, 0, 1);
			const double b = FVector::DotProduct(U, Dir);
			const double c = FVector::DotProduct(Dir, Dir);
			const double denom = c - b * b;
			if (FMath::Abs(denom) < 1e-6) return;   // looking straight along the axis
			const FVector w0 = Base - O;
			const double d = FVector::DotProduct(U, w0);
			const double e = FVector::DotProduct(Dir, w0);
			const double t = (b * e - c * d) / denom;
			const double RawH = (Base.Z + t - GVolEdit.DragBottomZ) / 100.0;
			// Season 4: height 0 = INFINITE. Dragging the top through the
			// floor snaps to it; dragging back up restores a real height.
			const double NewH = RawH < 0.5 ? 0.0 : FMath::Clamp(RawH, 0.5, 2000.0);
			if (NewH <= 0.01)
			{
				const FString OldHS = GetActorProp(Vol, TEXT("height"));
				const bool bWasInf = !OldHS.IsNumeric() || FCString::Atod(*OldHS) <= 0.01;
				if (!bWasInf) Notify(TEXT("Height 0 - this zone is now INFINITE height (drawn at 5 m, Season 4 rule)."));
			}
			// SetActorProp stretches the walls immediately; its transaction
			// folds into the drag's own, so one undo reverts the whole drag
			SetActorProp(Vol, TEXT("height"), FString::Printf(TEXT("%.2f"), NewH));
			// infinite draws at 5 m - the handle follows the DRAWN top
			GVolEdit.DragZ = GVolEdit.DragBottomZ + (NewH <= 0.01 ? 5.0 : NewH) * 100.0;
			return;
		}

		// BOTTOM ring: slide on the horizontal plane at the grabbed handle's
		// height - only the point's X/Y change
		if (FMath::Abs(Dir.Z) < 1e-6) return;
		const double t = (GVolEdit.DragZ - O.Z) / Dir.Z;
		if (t <= 0.0) return;
		const FVector Wp = O + Dir * t;
		TArray<FVector> World = BF6_LoopToWorld(Vol, *L);
		// the point itself lives on the bottom ring: keep its height
		World[Point] = FVector(Wp.X, Wp.Y, GVolEdit.DragBottomZ);
		// live update: the actor was already Modify()'d when the drag began
		const TArray<FVector> Local = BF6_LoopToLocal(Vol, World);
		GVolumeLoops.Add(Vol, Local);
		RebuildVolumeWalls(Vol, Local);

		// AND RECENTRED EVERY FRAME, not at the end of the drag.
		//
		// Deferring it to drag end was over-caution on my part: I expected the
		// origin sliding under a moving corner to make the drag chase itself.
		// It cannot - the drag puts the point where the CURSOR is, in world
		// space, and recentring only moves the origin and rewrites the loop to
		// compensate, so not one world position changes. What deferring it
		// actually did was leave the origin visibly stale until the zone was
		// reselected.
		BF6_VolumeRecenter(Vol, false);   // folds into the drag's transaction
	}

	void ClearSelection()
	{
		if (GEditor) { GEditor->SelectNone(false, true, false); GEditor->NoteSelectionChange(); }
	}

	void EndZoneDotDrag()
	{
		if (GVolEdit.Drag != INDEX_NONE)
			if (AActor* Vol = GVolEdit.Volume.Get())
			{
				// RECENTRED ON EVERY SHAPE CHANGE, rather than on a button
				// nobody remembers to press. Dragging a corner moves the shape
				// and leaves the origin where it was, so the gizmo drifts
				// further off the zone with every edit until it is nowhere near
				// the thing it moves.
				//
				// At drag END, not per frame: recentring while the corner is
				// still moving slides the origin under the cursor and the drag
				// chases itself.
				BF6_VolumeRecenter(Vol, false);   // folds into the drag's transaction
				BF6_WriteLoopTags(Vol);           // mirror the final shape for undo
			}
		GVolEdit.Drag = INDEX_NONE;
		if (GVolEdit.bTx && GEditor) { GEditor->EndTransaction(); GVolEdit.bTx = false; }
	}

	// Ghost every placed/base actor NOT in Keep: translucent + unselectable.
	// The originals go into Out so BF6_GhostRestoreSet can undo it exactly.
	static void BF6_GhostAllExcept(const TSet<AActor*>& Keep, TArray<FBF6Ghosted>& Out)
	{
		UMaterialInterface* Ghost = BF6_Material(TEXT("M_Ghost"));
		if (!Ghost || !GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (Keep.Contains(*It)) continue;
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)) continue;
			if (UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(It->GetRootComponent()))
			{
				FBF6Ghosted G;
				G.Comp = M;
				G.bWasSelectable = M->bSelectable;
				for (int32 s = 0; s < M->GetNumSections(); s++) G.Mats.Add(M->GetMaterial(s));
				for (int32 s = 0; s < M->GetNumSections(); s++) M->SetMaterial(s, Ghost);
				M->bSelectable = false;
				Out.Add(MoveTemp(G));
			}
		}
		BF6_Redraw();
	}

	// ---- the lint spotlight -----------------------------------------------
	//
	// Clicking a Validate row used to just select the object, which on a full
	// map means "somewhere in that skyline is your problem". Now the view
	// flies to it and everything else goes translucent, so the one object that
	// is wrong is the one object you can see - until the panel closes and the
	// map comes back.
	static TArray<FBF6Ghosted> GLintGhost;

	void ClearLintSpotlight()
	{
		if (GLintGhost.Num() > 0) BF6_GhostRestoreSet(GLintGhost);
	}

	void LintSpotlight(AActor* A)
	{
		ClearLintSpotlight();   // one spotlight at a time; re-clicks move it
		if (!A) return;
		TSet<AActor*> Keep;
		Keep.Add(A);
		// its branch stays lit too: half the findings are about a RELATIONSHIP
		// (a spawner and its spawns), and ghosting the children would hide the
		// evidence
		TArray<AActor*> Kids;
		A->GetAttachedActors(Kids, true, true);
		for (AActor* K : Kids) Keep.Add(K);
		BF6_GhostAllExcept(Keep, GLintGhost);
		SelectOnly(A);
		FocusSelection();
	}

	// ---- collision overlay (a VIEW aid, never exported) ----
	// Ported from the Godot high-poly tool. This is NOT the game's real
	// collision and must never be described as such - the true shapes live in
	// each object's PhysicsResource as convex-decomposed polytopes, and over
	// half of them are detail/raycast-only volumes that do not block a player
	// at all. What it DOES model correctly is the rule that catches people out:
	// BF6 collision follows the object's geometry but scales UNIFORMLY FROM X,
	// so an object stretched to (10, 20, 20) still collides as if it were
	// (10, 10, 10). Stretch a wall and players walk through the part you added.
	// The overlay rebuilds the object's own mesh in translucent red at that
	// uniform-X scale, so the gap between what you see and what you hit becomes
	// visible. Transient, unselectable, never saved and never exported.
	static const TCHAR* kColVisName = TEXT("BF6CollisionVis");
	// A HAIR off the surface, straight out along each vertex's normal.
	//
	// The job is beating depth precision and nothing else: the shell is meant to
	// read as the object's own skin, so any offset you can SEE is already wrong.
	// 1 mm clears z-fighting at every distance the editor flies at while staying
	// invisible as a gap. The first cut used 10 cm, which floated the shell off
	// the prop and made small objects look wrapped rather than skinned.
	//
	// Still along the NORMAL rather than a scale inflate: a proportional offset
	// is a different physical distance on every prop, so it is invisible on a
	// jerrycan and centimetres on a building.
	static const float  kColVisPush = 0.1f;   // centimetres
	static const float  kColVisEps  = 1.0f;
	static TArray<TWeakObjectPtr<AActor>> GColVis;   // actors carrying an overlay
	static FLinearColor GColVisColor = FLinearColor(1.0f, 0.08f, 0.08f);
	static float GColVisAlpha = 0.45f;

	static bool BF6_ColVisTarget(AActor* A)
	{
		return A && (A->Tags.Contains(kPlacedTag) || A->Tags.Contains(kBaseTag))
			&& !A->Tags.Contains(kHandleTag) && !A->Tags.Contains(kContextTag)
			&& !IsVolumeActor(A) && !IsObbActor(A);   // zones are not collision
	}

	// the child's LOCAL transform that lands it at uniform-X scale in WORLD
	// space, whatever the parent's rotation, mirroring or stretch
	static void BF6_ColVisFit(AActor* A, USceneComponent* Vis)
	{
		if (!A || !Vis) return;
		const FTransform PW = A->GetActorTransform();
		const float Sx = (float)PW.GetScale3D().X;
		if (FMath::Abs(Sx) < 1e-6f) return;
		const FTransform Want(PW.GetRotation(), PW.GetLocation(), FVector(Sx * kColVisEps));
		Vis->SetRelativeTransform(Want.GetRelativeTransform(PW));
	}

	static UProceduralMeshComponent* BF6_ColVisOf(AActor* A)
	{
		if (!A || !A->GetRootComponent()) return nullptr;
		TArray<USceneComponent*> Kids;
		A->GetRootComponent()->GetChildrenComponents(false, Kids);
		for (USceneComponent* C : Kids)
			if (C && C->GetFName() == kColVisName) return Cast<UProceduralMeshComponent>(C);
		return nullptr;
	}

	static bool BF6_ColVisAdd(AActor* A)
	{
		if (!BF6_ColVisTarget(A)) return false;
		if (UProceduralMeshComponent* Have = BF6_ColVisOf(A)) { BF6_ColVisFit(A, Have); return true; }
		FString Mesh = TagValue(A, TEXT("mesh:"));
		if (Mesh.IsEmpty()) Mesh = TagValue(A, TEXT("type:"));
		if (Mesh.IsEmpty()) return false;
		UProceduralMeshComponent* Vis = NewObject<UProceduralMeshComponent>(A, kColVisName);
		if (!Vis) return false;
		Vis->SetFlags(RF_Transient);
		Vis->SetupAttachment(A->GetRootComponent());
		Vis->RegisterComponent();
		if (!FillProcFromBf6Mesh(Vis, ObjModelPath(Mesh), false, kColVisPush))
		{
			Vis->DestroyComponent();
			return false;
		}
		Vis->bSelectable = false;
		Vis->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Vis->SetCastShadow(false);
		if (UMaterialInterface* Base = BF6_Material(TEXT("M_CollisionVis")))
			if (UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, GetTransientPackage()))
			{
				M->SetVectorParameterValue(TEXT("Color"), GColVisColor);
				M->SetScalarParameterValue(TEXT("Alpha"), GColVisAlpha);
				for (int32 s = 0; s < Vis->GetNumSections(); s++) Vis->SetMaterial(s, M);
			}
		BF6_ColVisFit(A, Vis);
		GColVis.Add(A);
		return true;
	}

	static void BF6_ColVisRemove(AActor* A)
	{
		if (UProceduralMeshComponent* V = BF6_ColVisOf(A))
		{
			V->ClearAllMeshSections();   // drop the vertex payload, not just the view
			V->DestroyComponent();
		}
	}

	int32 HideCollisionOverlay();   // defined below; ShowCollisionOverlay clears first

	bool AnyCollisionOverlay() { return GColVis.Num() > 0; }

	// Whether THIS selection is already showing collision. The pill toggles on
	// the selection, not on the map: picking a different object should offer to
	// show that one, not to hide the one you were looking at before.
	// Selecting a NODE means the things under it. A node holds nothing of its own
	// but a marker, so colouring it or asking what it collides with would
	// otherwise act on a cube nobody cares about. Every per-object tool takes its
	// targets from here, so a parent behaves like the thing it stands for.
	void SelectionTargets(TArray<AActor*>& Out)
	{
		Out.Reset();
		if (!GEditor) return;
		USelection* S = GEditor->GetSelectedActors();
		TSet<AActor*> Set;
		for (int32 i = 0; S && i < S->Num(); i++)
		{
			AActor* A = Cast<AActor>(S->GetSelectedObject(i));
			if (!A) continue;
			if (A->Tags.Contains(kGroupTag))
			{
				TArray<AActor*> Sub;
				BF6_CollectSubtree(A, Sub);
				for (AActor* K : Sub)
					if (!K->Tags.Contains(kGroupTag)) Set.Add(K);   // the contents, not the markers
			}
			else Set.Add(A);
		}
		Out = Set.Array();
	}

	bool SelectionHasCollisionOverlay()
	{
		if (!GEditor) return false;
		USelection* S = GEditor->GetSelectedActors();
		for (int32 i = 0; S && i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
				if (BF6_ColVisOf(A)) return true;
		return false;
	}

	int32 HideCollisionForSelection()
	{
		if (!GEditor) return 0;
		USelection* S = GEditor->GetSelectedActors();
		int32 n = 0;
		for (int32 i = 0; S && i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
				if (BF6_ColVisOf(A))
				{
					BF6_ColVisRemove(A);
					GColVis.RemoveAll([A](const TWeakObjectPtr<AActor>& P){ return P.Get() == A; });
					n++;
				}
		BF6_Redraw();
		return n;
	}

	// how many objects are stretched, i.e. where the overlay actually differs
	// from what is drawn - the only places collision can surprise you
	int32 CountStretched()
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!BF6_ColVisTarget(*It)) continue;
			const FVector S = It->GetActorScale3D();
			if (!FMath::IsNearlyEqual(S.X, S.Y, 0.01) || !FMath::IsNearlyEqual(S.X, S.Z, 0.01)) n++;
		}
		return n;
	}

	// Scope: 0 = selection, 1 = every stretched object, 2 = everything.
	int32 ShowCollisionOverlay(int32 Scope)
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		HideCollisionOverlay();
		int32 n = 0;
		if (Scope == 0)
		{
			TArray<AActor*> Targets; SelectionTargets(Targets);
			for (AActor* A : Targets)
				if (BF6_ColVisAdd(A)) n++;
		}
		else
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				if (!BF6_ColVisTarget(*It)) continue;
				if (Scope == 1)
				{
					const FVector S3 = It->GetActorScale3D();
					if (FMath::IsNearlyEqual(S3.X, S3.Y, 0.01) && FMath::IsNearlyEqual(S3.X, S3.Z, 0.01)) continue;
				}
				if (BF6_ColVisAdd(*It)) n++;
			}
		}
		BF6_Redraw();
		return n;
	}

	int32 HideCollisionOverlay()
	{
		int32 n = 0;
		for (const TWeakObjectPtr<AActor>& P : GColVis)
			if (AActor* A = P.Get()) { BF6_ColVisRemove(A); n++; }
		GColVis.Reset();
		BF6_Redraw();
		return n;
	}

	// objects the user moved or rescaled keep their overlay aligned
	void TickCollisionOverlay()
	{
		if (GColVis.Num() == 0) return;
		for (int32 i = GColVis.Num() - 1; i >= 0; i--)
		{
			AActor* A = GColVis[i].Get();
			if (!A) { GColVis.RemoveAt(i); continue; }
			BF6_ColVisFit(A, BF6_ColVisOf(A));
		}
	}

	// ---- recolorizer (a VIEW aid, never exported) ----
	// Ported from the Godot Recolorizer: paint objects so a blockout reads at a
	// glance. Nothing here reaches the .spatial.json - it only swaps the mesh
	// material in the editor, and every original is kept so Clear puts the map
	// back exactly as it was. Recolouring by TYPE is the useful default: each
	// distinct object type gets its own hue, so duplicates and stray props are
	// obvious immediately.
	static TArray<FBF6Ghosted> GRecolored;
	static TSet<TWeakObjectPtr<UProceduralMeshComponent>> GRecolorSeen;

	static UMaterialInstanceDynamic* BF6_RecolorMID(const FLinearColor& C)
	{
		// lit, so shapes still read; the unlit highlight material is the fallback
		UMaterialInterface* Base = BF6_Material(TEXT("M_Recolor"));
		if (!Base) Base = BF6_Material(TEXT("M_NeonHighlight"));
		if (!Base) return nullptr;
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
		if (!M) return nullptr;
		M->SetVectorParameterValue(TEXT("Color"), C);
		return M;
	}

	static void BF6_RecolorOne(AActor* A, const FLinearColor& C)
	{
		if (!A) return;
		UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
		if (!M || M->GetNumSections() == 0) return;
		if (!GRecolorSeen.Contains(M))   // remember the true original exactly once
		{
			FBF6Ghosted G;
			G.Comp = M;
			G.bWasSelectable = M->bSelectable;
			for (int32 s = 0; s < M->GetNumSections(); s++) G.Mats.Add(M->GetMaterial(s));
			GRecolored.Add(MoveTemp(G));
			GRecolorSeen.Add(M);
		}
		// the colour rides the actor as a tag, so it survives a save/reload and
		// a mesh rebuild - the material itself is transient
		A->Tags.RemoveAll([](const FName& T){ return T.ToString().StartsWith(TEXT("tint:")); });
		A->Tags.Add(FName(*FString::Printf(TEXT("tint:%s"), *C.ToFColor(true).ToHex())));
		if (UMaterialInstanceDynamic* Mid = BF6_RecolorMID(C))
			for (int32 s = 0; s < M->GetNumSections(); s++) M->SetMaterial(s, Mid);
	}

	// ---- a TEMPORARY highlight, for the length of one placement run --------
	//
	// NOT BF6_RecolorOne. That writes a "tint:" tag, which is the creator's own
	// deliberate colour: it survives a save, a reload and a mesh rebuild, by
	// design. Lighting up existing spawns for the length of a run and then
	// leaving them green forever is not a highlight, it is repainting somebody
	// else's map.
	//
	// So the original materials are stored and put straight back, and no tag is
	// ever written.
	static TArray<FBF6Ghosted> GHQLit;

	static void BF6_HQUnlight()
	{
		if (GHQLit.Num() > 0) BF6_GhostRestoreSet(GHQLit);
	}

	static void BF6_HQLightOne(AActor* A, const FLinearColor& C)
	{
		if (!A) return;
		UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
		if (!M || M->GetNumSections() == 0) return;
		FBF6Ghosted G;
		G.Comp = M;
		G.bWasSelectable = M->bSelectable;
		for (int32 s = 0; s < M->GetNumSections(); s++) G.Mats.Add(M->GetMaterial(s));
		GHQLit.Add(MoveTemp(G));
		if (UMaterialInstanceDynamic* Mid = BF6_RecolorMID(C))
			for (int32 s = 0; s < M->GetNumSections(); s++) M->SetMaterial(s, Mid);
	}

	static bool BF6_RecolorTarget(AActor* A)
	{
		return A && (A->Tags.Contains(kPlacedTag) || A->Tags.Contains(kBaseTag))
			&& !A->Tags.Contains(kHandleTag) && !A->Tags.Contains(kContextTag)
			&& !IsVolumeActor(A) && !IsObbActor(A);   // zones keep their own look
	}

	// Re-paint an actor that carries a saved tint. Used when a session loads and
	// after undo rebuilds a mesh from disk, both of which reset the material.
	void ReapplyTint(AActor* A)
	{
		if (!A) return;
		const FString Hex = TagValue(A, TEXT("tint:"));
		if (Hex.IsEmpty()) return;
		BF6_RecolorOne(A, FLinearColor(FColor::FromHex(Hex)));
	}

	int32 ReapplyAllTints()
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (!TagValue(*It, TEXT("tint:")).IsEmpty()) { ReapplyTint(*It); n++; }
		BF6_Redraw();
		return n;
	}

	bool AnyRecolored() { return GRecolored.Num() > 0; }

	FLinearColor SelectionColor()
	{
		if (!GEditor) return kVolumeDefaultColor;
		TArray<AActor*> Targets; SelectionTargets(Targets);
		for (AActor* A : Targets)
		{
			FLinearColor C;
			if (BF6_VolumeColorOf(A, C)) return C;
		}
		// A mesh's paint-over tint, if it has one.
		for (AActor* A : Targets)
			for (const FName& T : A->Tags)
			{
				const FString S2 = T.ToString();
				if (S2.StartsWith(TEXT("tint:")) && S2.Len() >= 13)
					return FLinearColor(FColor::FromHex(S2.Mid(5)));
			}
		return kVolumeDefaultColor;
	}

	// fwd: both are declared again further down beside their own sections; an
	// HQ needs the ground under it and the name a link refers to it by.
	bool GroundRay(const FVector& From, const FVector& To, const TArray<AActor*>* Placed,
	               FVector& OutHit, FVector* OutNormal = nullptr);
	static FString BF6_LinkName(AActor* A);

	// ---- HQ: the one object that owns others ------------------------------
	//
	// An HQ_PlayerSpawner is not a prop, it is a small assembly: it needs an
	// area to spawn inside and a set of spawn points to spawn on, and its own
	// schema says so - HQArea is a PolygonVolume, InfantrySpawns and
	// ForwardSpawns are Array[SpawnPoint].
	//
	// Building that by hand means placing a volume somewhere, remembering to
	// come back, opening the HQ's attributes, finding HQArea, entering the
	// picker, and finding the volume again in the world. Every one of those
	// steps is a chance to end up with an HQ that looks finished and spawns
	// nobody, because the link is the only part that matters and it is the
	// part with no visual.
	//
	// So the two things an HQ always needs get made FROM the HQ, and linked on
	// the way out.
	bool IsHQActor(AActor* A)
	{
		if (!A) return false;
		FString Ty = TagValue(A, TEXT("label:"));
		if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
		return Ty == TEXT("HQ_PlayerSpawner");
	}

	// The volume this HQ spawns inside, if it has one.
	// fwd: both are defined further down this file, and the assembly helpers
	// below are the first things that need them.
	static FString BF6_TypeOf(AActor* A);
	static void BF6_ParentUnder(AActor* Child, AActor* Owner);

	AActor* HQVolume(AActor* Owner, const FString& Field)
	{
		if (!Owner || !GEditor || Field.IsEmpty()) return nullptr;
		const FString Want = GetActorProp(Owner, Field).TrimStartAndEnd();
		if (Want.IsEmpty()) return nullptr;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)) continue;
			FString Nm = It->GetActorLabel();
			Nm.RemoveFromStart(TEXT("BF6_"));
			if (Nm == Want) return *It;
		}
		return nullptr;
	}

	// Make the HQ's area, or open the one it already has.
	//
	// The volume lands AROUND the HQ rather than under the cursor: an HQ area
	// that does not contain its own HQ is never what anybody meant, and it is a
	// mistake that only shows up in play.
	AActor* HQCreateOrEditVolume(AActor* Owner, const FString& Field)
	{
		if (!Owner || Field.IsEmpty()) return nullptr;
		if (AActor* Have = HQVolume(Owner, Field))
		{
			if (GEditor)
			{
				GEditor->SelectNone(false, true, false);
				GEditor->SelectActor(Have, true, true);
			}
			BeginVolumeEdit(Have);   // selecting one starts its edit anyway; be explicit
			return Have;
		}
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) return nullptr;

		// Ground level under the owner, so the walls start where the floor is
		// rather than at whatever height the marker happens to sit.
		FVector At = Owner->GetActorLocation();
		FVector Gnd;
		if (GroundRay(At + FVector(0, 0, 2000), At - FVector(0, 0, 50000), nullptr, Gnd)) At.Z = Gnd.Z;

		// The field decides the SHAPE: RingOfFire wants an OBBVolume for one of
		// its two, and placing a polygon there would link something the game
		// cannot read.
		FString Want = TEXT("PolygonVolume");
		for (const FLinkField& F : LinkVolumeFields(Owner))
			if (F.Field == Field) { Want = F.ElemType; break; }

		AActor* Vol = PlaceType(Want, At);
		if (!Vol) return nullptr;
		BF6_SetPrettyLabel(Vol, FString::Printf(TEXT("%s_%s"), *BF6_LinkName(Owner), *Field));

		// THE LINK, which is the whole point. Written straight onto the owner so
		// the creator never has to know the field exists.
		SetActorProp(Owner, Field, BF6_LinkName(Vol));
		// ...and parked under it, the way finished Portal maps are built.
		BF6_ParentUnder(Vol, Owner);
		return Vol;
	}

	// The spawns already wired into ONE of this HQ's spawn arrays, by name.
	static void BF6_HQSpawnNames(AActor* HQ, const FString& Field, TArray<FString>& Out)
	{
		Out.Reset();
		if (!HQ || Field.IsEmpty()) return;
		GetActorProp(HQ, Field).ParseIntoArray(Out, TEXT(","), true);
		for (FString& S : Out) S.TrimStartAndEndInline();
	}

	int32 HQSpawnCount(AActor* HQ, const FString& Field)
	{
		TArray<FString> N; BF6_HQSpawnNames(HQ, Field, N); return N.Num();
	}

	// A field's menu label. "InfantrySpawns" -> "INFANTRY",
	// "RetreatFromArea" -> "RETREAT FROM", "InfantrySpawnPoints_Team1" ->
	// "INFANTRY TEAM1". The trailing noun is the same across a group and says
	// nothing the menu does not already say.
	static FString BF6_FieldLabel(const FString& Field)
	{
		FString L = Field;
		for (const TCHAR* Suf : { TEXT("SpawnPoints"), TEXT("Spawns"), TEXT("Volume"), TEXT("Area") })
			if (L.EndsWith(Suf) && L.Len() > FCString::Strlen(Suf)) { L.RemoveFromEnd(Suf); break; }
		FString Out;
		for (int32 i = 0; i < L.Len(); i++)
		{
			const TCHAR C = L[i];
			if (C == TEXT('_')) { Out.AppendChar(TEXT(' ')); continue; }
			if (i > 0 && FChar::IsUpper(C) && !FChar::IsUpper(L[i - 1])) Out.AppendChar(TEXT(' '));
			Out.AppendChar(C);
		}
		return Out.TrimStartAndEnd().ToUpper();
	}

	// Every link field on this object's type, read off the schema.
	//
	// Never a hardcoded list: eleven shipped types have these, and a field the
	// next SDK adds has to appear on its own or the tool quietly stops being
	// able to build part of a mode.
	static void BF6_LinkFieldsOf(AActor* A, TArray<FLinkField>* OutVolumes, TArray<FLinkField>* OutArrays)
	{
		if (OutVolumes) OutVolumes->Reset();
		if (OutArrays)  OutArrays->Reset();
		if (!A) return;
		const FString Ty = BF6_TypeOf(A);
		if (Ty.IsEmpty()) return;
		for (const FPropDef& D : PropsForType(Ty))
		{
			FLinkField F;
			F.Field = D.Name;
			F.Label = BF6_FieldLabel(D.Name);
			if (D.Type == TEXT("PolygonVolume") || D.Type == TEXT("OBBVolume"))
			{
				if (!OutVolumes) continue;
				F.ElemType = D.Type;
				F.bArray = false;
				F.Count = GetActorProp(A, D.Name).TrimStartAndEnd().IsEmpty() ? 0 : 1;
				OutVolumes->Add(F);
			}
			else if (D.Type.StartsWith(TEXT("Array[")) && D.Type.EndsWith(TEXT("]")))
			{
				if (!OutArrays) continue;
				F.ElemType = D.Type.Mid(6, D.Type.Len() - 7);
				F.bArray = true;
				F.Count = HQSpawnCount(A, D.Name);
				OutArrays->Add(F);
			}
		}
	}

	TArray<FLinkField> LinkVolumeFields(AActor* A)
	{
		TArray<FLinkField> V; BF6_LinkFieldsOf(A, &V, nullptr); return V;
	}
	TArray<FLinkField> LinkArrayFields(AActor* A)
	{
		TArray<FLinkField> R; BF6_LinkFieldsOf(A, nullptr, &R); return R;
	}

	// Make one more spawn point for this HQ and hand it to the cursor.
	//
	// APPENDED to the chosen array rather than replacing it: the array is the
	// point, and a creator laying out a spawn line expects the previous ones to
	// still be there.
	// PARENT A LINKED OBJECT UNDER ITS OWNER.
	//
	// Read off two finished Portal maps by a creator who builds these for real:
	// every PolygonVolume sits under the CapturePoint, HQ or Sector that links
	// it, and every SpawnPoint sits under its HQ. Not one is left at the root.
	// The link and the tree say the same thing, and a reader of either can see
	// the assembly.
	//
	// Only the attachment is set - BF6_SyncTreeTagsFromLive turns real Unreal
	// attachment into the Godot tree on save, so this is all that is needed for
	// it to survive an export.
	static void BF6_ParentUnder(AActor* Child, AActor* Owner)
	{
		if (!Child || !Owner || Child == Owner) return;
		if (Child->GetAttachParentActor()) return;   // never steal one already placed in a tree
		Child->Modify();
		if (USceneComponent* RC = Child->GetRootComponent()) RC->Modify();   // undo needs the component
		Child->AttachToActor(Owner, FAttachmentTransformRules::KeepWorldTransform);
	}

	AActor* HQCreateSpawn(AActor* HQ, const FString& Field)
	{
		if (Field.IsEmpty() || !HQ) return nullptr;
		FVector At = HQ->GetActorLocation();
		FVector Gnd;
		if (GroundRay(At + FVector(0, 0, 2000), At - FVector(0, 0, 50000), nullptr, Gnd)) At.Z = Gnd.Z;

		// What the field ASKS for. Array[SpawnPoint] wants a SpawnPoint,
		// Array[CapturePoint] wants a CapturePoint - hardcoding SpawnPoint here
		// is what kept this working for HQs only.
		FString Elem = TEXT("SpawnPoint");
		for (const FLinkField& F : LinkArrayFields(HQ))
			if (F.Field == Field) { Elem = F.ElemType; break; }

		AActor* Sp = PlaceType(Elem, At);
		if (!Sp) return nullptr;
		BF6_ParentUnder(Sp, HQ);

		TArray<FString> Names;
		BF6_HQSpawnNames(HQ, Field, Names);
		Names.Add(BF6_LinkName(Sp));
		SetActorProp(HQ, Field, FString::Join(Names, TEXT(",")));

		// The team, so a spawn belongs to its owner in the way that matters as
		// well as in the link.
		//
		// The FIELD wins where it names one: CapturePoint holds
		// InfantrySpawnPoints_Team1 and _Team2, so the array a spawn went into
		// is what decides its team, not whatever team the capture point is.
		FString Team;
		int32 Us;
		if (Field.FindLastChar(TEXT('_'), Us) && Field.Mid(Us + 1).StartsWith(TEXT("Team")))
			Team = Field.Mid(Us + 1);
		if (Team.IsEmpty()) Team = GetActorProp(HQ, TEXT("Team"));
		if (!Team.IsEmpty()) SetActorProp(Sp, TEXT("Team"), Team);

		return Sp;
	}

	// ---- laying a SET of spawns, not one -----------------------------------
	//
	// Spawns come in groups: an HQ with one is not a thing anybody builds. The
	// first cut placed a single spawn and stopped, so laying eight meant going
	// back to the radial eight times, and the ring is two clicks away from the
	// place the creator is looking.
	//
	// So it is a RUN: each placement immediately makes the next and hands it to
	// the cursor, until the creator says they are done.
	struct FBF6HQSpawnRun
	{
		TWeakObjectPtr<AActor> HQ;
		FString Field;                    // WHICH of the HQ's spawn arrays this run fills
		TWeakObjectPtr<AActor> Carried;   // made, linked, NOT yet set down
		TArray<TWeakObjectPtr<AActor>> Down;   // set down BY THIS RUN, so Esc can take them back
		bool  bActive = false;
		int32 Placed = 0;
	};
	static FBF6HQSpawnRun GHQRun;

	bool IsHQSpawnRun() { return GHQRun.bActive && GHQRun.HQ.IsValid(); }
	int32 HQSpawnRunPlaced() { return GHQRun.Placed; }

	AActor* HQCreateSpawn(AActor* HQ, const FString& Field);   // fwd: the run makes them one at a time

	void BeginHQSpawnRun(AActor* HQ, const FString& Field)
	{
		GHQRun = FBF6HQSpawnRun();
		if (!IsHQActor(HQ) || Field.IsEmpty()) return;
		GHQRun.HQ = HQ;
		GHQRun.Field = Field;
		GHQRun.bActive = true;
	}

	FString HQSpawnRunField() { return GHQRun.Field; }

	// One placed: make the next one and carry it.
	// Returns false when the run is over or could not continue.
	bool HQSpawnRunNext()
	{
		if (!IsHQSpawnRun()) return false;
		// Whatever was in the hand a moment ago has just been set down.
		if (AActor* JustPlaced = GHQRun.Carried.Get()) GHQRun.Down.Add(JustPlaced);
		AActor* Next = HQCreateSpawn(GHQRun.HQ.Get(), GHQRun.Field);
		if (!Next) { BF6_HQUnlight(); GHQRun = FBF6HQSpawnRun(); return false; }
		GHQRun.Carried = Next;   // so ending the run can take it back out again
		GHQRun.Placed++;
		BeginPickPlace();
		return true;
	}

	// Finish the run. The spawn still riding the cursor was never placed, so it
	// goes - but everything already set down STAYS, which is the convention the
	// rest of this tool uses (the mode wizard says the same: Esc stops, placed
	// steps stay, Ctrl+Z removes them). Destroying a creator's last ten
	// placements because they pressed Escape would be a far worse surprise than
	// leaving them.
	// bKeep: ENTER finishes and keeps what is down. ESCAPE cancels, and cancel
	// means the run leaves no trace - which is what anybody pressing Escape
	// expects, and the reason the old "Escape stops but keeps them" behaviour
	// read as broken.
	int32 EndHQSpawnRun(bool bKeep)
	{
		const int32 n = bKeep ? GHQRun.Down.Num() : 0;
		if (IsPickPlacing()) CancelPickPlace();

		if (!bKeep)
			for (const TWeakObjectPtr<AActor>& Wk : GHQRun.Down)
				if (AActor* A = Wk.Get())
					if (UWorld* DW = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
						DW->EditorDestroyActor(A, true);
		GHQRun.Down.Reset();

		// DESTROY the one still being carried.
		//
		// Cancelling the pick-place only rolls back the MOVE - the spawn was
		// created before that transaction opened, so it survived, and Escape
		// left an orphan sitting at the HQ's feet still listed in the link.
		if (AActor* Carried = GHQRun.Carried.Get())
		{
			if (UWorld* CW = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
				CW->EditorDestroyActor(Carried, true);
		}
		GHQRun.Carried = nullptr;
		BF6_HQUnlight();   // the green was borrowed, not given
		// The carried spawn was linked when it was made, so the link has to come
		// off with it or the HQ points at something that no longer exists.
		if (AActor* HQ = GHQRun.HQ.Get())
		{
			TArray<FString> Names;
			BF6_HQSpawnNames(HQ, GHQRun.Field, Names);
			if (Names.Num() > 0)
			{
				UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				TSet<FString> Alive;
				if (W)
					for (TActorIterator<AActor> It(W); It; ++It)
					{
						FString Nm = It->GetActorLabel();
						Nm.RemoveFromStart(TEXT("BF6_"));
						Alive.Add(Nm);
					}
				Names.RemoveAll([&Alive](const FString& N){ return !Alive.Contains(N); });
				SetActorProp(HQ, GHQRun.Field, FString::Join(Names, TEXT(",")));
			}
		}
		AActor* Owner = GHQRun.HQ.Get();
		GHQRun = FBF6HQSpawnRun();
		// The run was an edit OF the HQ, so the selection goes back to the HQ
		// either way - the same hand-back assign mode does when it ends.
		// Anything else left the last spawn (or nothing at all) selected, which
		// read as the run not being over.
		if (GEditor)
		{
			GEditor->SelectNone(false, true, false);
			if (Owner) GEditor->SelectActor(Owner, true, true);
			else GEditor->NoteSelectionChange();
		}
		return n;
	}

	// Light up what this HQ already spawns on, so a new one is placed in
	// relation to them rather than blind. Uses the link picker's own "assigned"
	// green, because that is what these already are.
	int32 HQHighlightSpawns(AActor* HQ, const FString& Field)
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return 0;
		BF6_HQUnlight();   // a run that never ended cleanly must not stack
		TArray<FString> Names;
		BF6_HQSpawnNames(HQ, Field, Names);
		if (Names.Num() == 0) return 0;
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			FString Nm = It->GetActorLabel();
			Nm.RemoveFromStart(TEXT("BF6_"));
			if (!Names.Contains(Nm)) continue;
			BF6_HQLightOne(*It, FLinearColor(0.13f, 1.f, 0.27f));
			n++;
		}
		BF6_Redraw();
		return n;
	}

	int32 RecolorSelection(FLinearColor C)
	{
		if (!GEditor) return 0;
		TArray<AActor*> Targets; SelectionTargets(Targets);
		int32 n = 0;
		for (AActor* A : Targets)
		{
			// A VOLUME TAKES THE COLOUR ITSELF rather than a paint-over tint.
			//
			// The recolorizer swaps a mesh's material for a flat one and is a
			// pure view aid. A volume has no mesh to swap - its colour IS the
			// authored property the SDK round-trips, and it carries the alpha
			// that decides how see-through the zone is. Painting one through
			// the mesh path did nothing at all, which is why volumes appeared
			// to ignore the colorizer.
			if (IsVolumeActor(A))
			{
				BF6_SetVolumeColorTag(A, C);
				TArray<UProceduralMeshComponent*> Meshes;
				A->GetComponents<UProceduralMeshComponent>(Meshes);
				for (UProceduralMeshComponent* M : Meshes)
					if (M && (M->GetFName() == FName(TEXT("Volume")) || M == A->GetRootComponent()))
						BF6_ApplyVolumeMaterial(A, M);
				n++;
				continue;
			}
			if (BF6_RecolorTarget(A)) { BF6_RecolorOne(A, C); n++; }
		}
		BF6_Redraw();
		return n;
	}

	// one hue per distinct type, spaced by the golden angle so neighbouring
	// types never land on near-identical colours
	int32 RecolorByType()
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		TMap<FString, TArray<AActor*>> ByType;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!BF6_RecolorTarget(*It)) continue;
			FString Ty = TagValue(*It, TEXT("mesh:"));
			if (Ty.IsEmpty()) Ty = TagValue(*It, TEXT("label:"));
			if (Ty.IsEmpty()) Ty = TagValue(*It, TEXT("type:"));
			if (Ty.IsEmpty()) continue;
			ByType.FindOrAdd(Ty).Add(*It);
		}
		TArray<FString> Types; ByType.GetKeys(Types);
		Types.Sort();   // stable: the same map always paints the same way
		int32 n = 0;
		for (int32 i = 0; i < Types.Num(); i++)
		{
			const float Hue = FMath::Fmod(i * 0.61803398875f, 1.0f);
			const FLinearColor C = FLinearColor::MakeFromHSV8((uint8)(Hue * 255.0f), 190, 235);
			for (AActor* A : ByType[Types[i]]) { BF6_RecolorOne(A, C); n++; }
		}
		BF6_Redraw();
		return n;
	}

	// Unpaint just what is selected: its real material goes back, its saved
	// entry is dropped, and the tag goes with it so a reload stays clean.
	int32 ClearRecolorSelection()
	{
		if (!GEditor) return 0;
		USelection* S = GEditor->GetSelectedActors(); if (!S) return 0;
		TSet<UProceduralMeshComponent*> Want;
		for (int32 i = 0; i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
			{
				A->Tags.RemoveAll([](const FName& T){ return T.ToString().StartsWith(TEXT("tint:")); });
				if (UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent())) Want.Add(M);
			}
		if (Want.Num() == 0) return 0;
		int32 n = 0;
		for (int32 i = GRecolored.Num() - 1; i >= 0; i--)
		{
			UProceduralMeshComponent* M = GRecolored[i].Comp.Get();
			if (!M || !Want.Contains(M)) continue;
			for (int32 s = 0; s < GRecolored[i].Mats.Num(); s++) M->SetMaterial(s, GRecolored[i].Mats[s]);
			M->bSelectable = GRecolored[i].bWasSelectable;
			GRecolorSeen.Remove(M);
			GRecolored.RemoveAt(i);
			n++;
		}
		BF6_Redraw();
		return n;
	}

	int32 ClearRecolor()
	{
		const int32 n = GRecolored.Num();
		if (GEditor)
			if (UWorld* W = GEditor->GetEditorWorldContext().World())
				for (TActorIterator<AActor> It(W); It; ++It)
					It->Tags.RemoveAll([](const FName& T){ return T.ToString().StartsWith(TEXT("tint:")); });
		BF6_GhostRestoreSet(GRecolored);   // puts every saved material back
		GRecolorSeen.Reset();
		BF6_Redraw();
		return n;
	}

	// ---- link picking (assign spawn points / volumes) ----
	bool IsLinkPicking() { return GLinkPick.bActive; }

	void BeginLinkPick(AActor* Owner, const FString& PropName, bool bArray)
	{
		BF6_LinkGhostRestore();   // clear any previous assign-mode view
		GLinkPick.Owner = Owner;
		GLinkPick.Prop = PropName;
		GLinkPick.bArray = bArray;
		GLinkPick.bActive = true;
		if (GEditor) GEditor->SelectNone(false, true, false);
		// ---- the assign-mode view ----
		// what TYPE are we assigning? look the property up on the owner's schema
		FString TargetType;
		{
			FString OwnerType = TagValue(Owner, TEXT("label:"));
			if (OwnerType.IsEmpty()) OwnerType = TagValue(Owner, TEXT("type:"));
			for (const FPropDef& D : PropsForType(OwnerType))
				if (D.Name == PropName)
				{
					TargetType = D.Type;
					TargetType.RemoveFromStart(TEXT("Array["));
					TargetType.RemoveFromEnd(TEXT("]"));
					break;
				}
		}
		// candidates: objects of the target type (fallback: everything placed)
		if (UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				if (*It == Owner) continue;
				if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)) continue;
				FString Ty = TagValue(*It, TEXT("label:"));
				if (Ty.IsEmpty()) Ty = TagValue(*It, TEXT("type:"));
				const bool bMatch = TargetType.IsEmpty()
					|| Ty.Equals(TargetType, ESearchCase::IgnoreCase)
					|| (TargetType.Contains(TEXT("Volume")) && (IsVolumeActor(*It) || IsObbActor(*It)));
				if (bMatch) GLinkPick.Candidates.Add(*It);
			}
			if (GLinkPick.Candidates.Num() == 0)
				for (TActorIterator<AActor> It(W); It; ++It)
					if (*It != Owner && (It->Tags.Contains(kPlacedTag) || It->Tags.Contains(kBaseTag)))
						GLinkPick.Candidates.Add(*It);

			// state 1 for candidates already assigned to this owner's property
			TArray<FString> Assigned;
			GetActorProp(Owner, PropName).ParseIntoArray(Assigned, TEXT(","));
			for (FString& S : Assigned) S = S.TrimStartAndEnd();
			GLinkPick.CandState.SetNumZeroed(GLinkPick.Candidates.Num());
			for (int32 i = 0; i < GLinkPick.Candidates.Num(); i++)
				if (AActor* C = GLinkPick.Candidates[i].Get())
				{
					FString Nm = C->GetActorLabel();
					Nm.RemoveFromStart(TEXT("BF6_"));
					if (Assigned.Contains(Nm)) GLinkPick.CandState[i] = 1;
				}

			// ghost everything that can't be assigned: translucent + unselectable
			TSet<AActor*> Keep;
			Keep.Add(Owner);
			for (const TWeakObjectPtr<AActor>& C : GLinkPick.Candidates)
				if (AActor* CA = C.Get()) Keep.Add(CA);
			BF6_GhostAllExcept(Keep, GLinkPick.Ghosted);

			// ...and the candidates themselves glow solid neon (unlit emissive),
			// colour-matched to their marker and link line, restored on exit
			if (UMaterialInterface* Base = BF6_Material(TEXT("M_NeonHighlight")))
			{
				for (int32 s = 0; s < 3; s++)
				{
					UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
					Mid->SetVectorParameterValue(TEXT("Color"), kLinkNeon[s]);
					GLinkPick.Mid[s] = TStrongObjectPtr<UMaterialInstanceDynamic>(Mid);
				}
				GLinkPick.NeonApplied.Init(255, GLinkPick.Candidates.Num());
				for (int32 i = 0; i < GLinkPick.Candidates.Num(); i++)
					if (AActor* C = GLinkPick.Candidates[i].Get())
						if (UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(C->GetRootComponent()))
						{
							FBF6Ghosted G;
							G.Comp = M;
							G.bWasSelectable = M->bSelectable;
							for (int32 s = 0; s < M->GetNumSections(); s++) G.Mats.Add(M->GetMaterial(s));
							GLinkPick.Neon.Add(MoveTemp(G));
							BF6_LinkApplyNeon(i);
						}
			}
		}

		Notify(FString::Printf(TEXT("Assigning %s: click a highlighted marker or object, then SPACE or ENTER confirms (ESC cancels)."),
			*PropName));
	}

	void ConfirmLinkPick()
	{
		if (!GLinkPick.bActive) return;
		GLinkPick.bActive = false;
		BF6_LinkGhostRestore();
		AActor* Owner = GLinkPick.Owner.Get();
		if (!Owner || !GEditor) return;
		TArray<FString> Names;
		USelection* Sel = GEditor->GetSelectedActors();
		for (int32 i = 0; Sel && i < Sel->Num(); i++)
		{
			AActor* A = Cast<AActor>(Sel->GetSelectedObject(i));
			if (!A || A == Owner) continue;
			if (!A->Tags.Contains(kBaseTag) && !A->Tags.Contains(kPlacedTag)) continue;
			FString Nm = A->GetActorLabel();
			Nm.RemoveFromStart(TEXT("BF6_"));
			Names.Add(Nm);
			if (!GLinkPick.bArray) break;
		}
		if (Names.Num() == 0) { Notify(TEXT("Nothing assignable was selected - link unchanged.")); return; }
		SetActorProp(Owner, GLinkPick.Prop, FString::Join(Names, TEXT(",")));
		Notify(FString::Printf(TEXT("%s = %s"), *GLinkPick.Prop, *FString::Join(Names, TEXT(", "))));
		GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(Owner, true, true);
	}

	void CancelLinkPick()
	{
		if (!GLinkPick.bActive) return;
		GLinkPick.bActive = false;
		BF6_LinkGhostRestore();
		// whatever was clicked mid-pick stays selected otherwise - hand the
		// selection back to the owner the attributes menu reopens for
		if (GEditor)
		{
			GEditor->SelectNone(false, true, false);
			if (AActor* Owner = GLinkPick.Owner.Get()) GEditor->SelectActor(Owner, true, true);
		}
		Notify(TEXT("Link assignment cancelled."));
	}

	// banner text for the assign-mode screen chrome
	FString LinkPickLabel()
	{
		if (!GLinkPick.bActive) return FString();
		FString Own;
		if (AActor* O = GLinkPick.Owner.Get())
		{
			Own = O->GetActorLabel();
			Own.RemoveFromStart(TEXT("BF6_"));
		}
		return Own.IsEmpty()
			? FString::Printf(TEXT("ASSIGNING %s"), *GLinkPick.Prop.ToUpper())
			: FString::Printf(TEXT("ASSIGNING %s FOR %s"), *GLinkPick.Prop.ToUpper(), *Own.ToUpper());
	}

	bool HasSelection()
	{
		if (!GEditor) return false;
		USelection* Sel = GEditor->GetSelectedActors();
		return Sel && Sel->Num() > 0;
	}

	// the viewport is piloting an actor (right-click > Pilot): Esc belongs to
	// the pilot exit, never to our deselect fallback
	bool IsViewportPiloting()
	{
		return GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsAnyActorLocked();
	}

	// ---- assign-mode overlay data (projected on the tick, like zone dots) ----
	void TickLinkPick()
	{
		if (!GLinkPick.bActive) return;
		GLinkPick.bOwnerPx = false;
		GLinkPick.CandPx.Reset();
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return;
		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(VC->Viewport, VC->GetScene(), VC->EngineShowFlags));
		FSceneView* View = VC->CalcSceneView(&Family);
		if (!View) return;
		auto Project = [&](const FVector& W, FVector2D& Out) -> bool
		{
			const FVector4 Clip = View->WorldToScreen(W);
			return Clip.W > 0.f && View->ScreenToPixel(Clip, Out);
		};
		if (AActor* Own = GLinkPick.Owner.Get())
			GLinkPick.bOwnerPx = Project(Own->GetActorLocation(), GLinkPick.OwnerPx);
		for (int32 i = 0; i < GLinkPick.Candidates.Num(); i++)
		{
			FVector2D Px(-10000.f, -10000.f);
			if (AActor* C = GLinkPick.Candidates[i].Get()) Project(C->GetActorLocation(), Px);
			GLinkPick.CandPx.Add(Px);
			// pending (selected) beats assigned in the colour coding
			if (GLinkPick.CandState.IsValidIndex(i))
			{
				AActor* C = GLinkPick.Candidates[i].Get();
				const bool bSel = C && C->IsSelected();
				if (bSel) GLinkPick.CandState[i] = 2;
				else if (GLinkPick.CandState[i] == 2) GLinkPick.CandState[i] = 0;
				// keep the mesh glow in step with the marker colour
				if (GLinkPick.NeonApplied.IsValidIndex(i) && GLinkPick.NeonApplied[i] != GLinkPick.CandState[i])
					BF6_LinkApplyNeon(i);
			}
		}
	}

	bool GetLinkOverlay(TArray<FVector2D>& OutPx, TArray<uint8>& OutState, FVector2D& OutOwnerPx, bool& bOutOwner)
	{
		if (!GLinkPick.bActive || GLinkPick.CandPx.Num() == 0) return false;
		OutPx = GLinkPick.CandPx;
		OutState = GLinkPick.CandState;
		OutOwnerPx = GLinkPick.OwnerPx;
		bOutOwner = GLinkPick.bOwnerPx;
		return true;
	}

	int32 LinkDotUnderMouse()
	{
		if (!GLinkPick.bActive) return INDEX_NONE;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return INDEX_NONE;
		const FVector2D M((float)VC->Viewport->GetMouseX(), (float)VC->Viewport->GetMouseY());
		const float Grab = 16.f * VC->GetDPIScale();
		int32 Best = INDEX_NONE; float BestD = Grab;
		for (int32 i = 0; i < GLinkPick.CandPx.Num(); i++)
		{
			const float D = FVector2D::Distance(M, GLinkPick.CandPx[i]);
			if (D < BestD) { BestD = D; Best = i; }
		}
		return Best;
	}

	// ---- the candidate LIST -----------------------------------------------
	//
	// Hunting glowing markers around a map is fine when there are three of them
	// and hopeless when there are thirty, half of them behind a building. The
	// same candidates the overlay is already tracking, named and listed, so a
	// creator can see what is assignable at all - and what they have actually
	// assigned - without flying the map to find out.
	int32 LinkCandidateCount() { return GLinkPick.bActive ? GLinkPick.Candidates.Num() : 0; }

	FString LinkCandidateName(int32 i)
	{
		if (!GLinkPick.bActive || !GLinkPick.Candidates.IsValidIndex(i)) return FString();
		AActor* A = GLinkPick.Candidates[i].Get();
		if (!A) return FString();
		FString Nm = A->GetActorLabel();
		Nm.RemoveFromStart(TEXT("BF6_"));
		return Nm;
	}

	// 0 free, 1 already assigned to this owner, 2 picked in this session.
	int32 LinkCandidateState(int32 i)
	{
		if (!GLinkPick.bActive || !GLinkPick.CandState.IsValidIndex(i)) return 0;
		return GLinkPick.CandState[i];
	}

	// SNAP THE VIEW TO ONE. Framed from the map's own overview angle and backed
	// off by the object's size, so a lamp post and a capture zone both end up
	// filling a sensible part of the screen rather than one being a dot and the
	// other overshooting past it.
	// SNAP THE VIEW ONTO WHAT IS SELECTED.
	//
	// Framed from the selection's own bounds so a bollard and a whole building
	// both end up filling a sensible part of the screen - a fixed distance
	// makes one a dot and overshoots past the other. The view ANGLE is kept:
	// the creator chose where they are looking from, and a jump that also
	// re-aims is disorienting when all they asked for was to be taken there.
	// A VIEWPORT TO FLY, even when a menu has the focus.
	//
	// GCurrentLevelEditingViewportClient is whichever viewport the editor
	// considers current, and a Slate popup taking focus can leave it null - so
	// a "snap to this" clicked from inside a menu found no viewport and
	// returned quietly, which is exactly the shape of a button that does
	// nothing. Falling back to the first active level viewport gives it one.
	static FLevelEditorViewportClient* BF6_ViewportToFly()
	{
		if (GCurrentLevelEditingViewportClient) return GCurrentLevelEditingViewportClient;
		if (FLevelEditorModule* LE = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
			if (TSharedPtr<SLevelViewport> VP = LE->GetFirstActiveLevelViewport())
				return &VP->GetLevelViewportClient();
		return nullptr;
	}

	// Frame the view on a linked object WITHOUT selecting it.
	//
	// Selecting would be the obvious way and is wrong here: the attribute list
	// is open on the object that OWNS the link, and changing the selection
	// closes the list out from under the creator - they would lose the very
	// panel they clicked from, to look at the thing it named.
	bool FocusByLinkName(const FString& Name)
	{
		if (!GEditor || Name.IsEmpty()) return false;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return false;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			// kGroupTag too: the attach search flies to nodes with this
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag) && !It->Tags.Contains(kGroupTag)) continue;
			FString Nm = It->GetActorLabel();
			Nm.RemoveFromStart(TEXT("BF6_"));
			if (Nm != Name) continue;
			FLevelEditorViewportClient* VC = BF6_ViewportToFly();
			if (!VC) return false;
			FVector Org, Ext;
			It->GetActorBounds(false, Org, Ext);
			const double Span = FMath::Max3((double)Ext.X * 2.0, (double)Ext.Y * 2.0, 800.0);
			FRotator Look = VC->GetViewRotation();
			if (Look.Pitch > -10.f) Look.Pitch = -30.f;
			VC->SetViewLocation(Org - Look.Vector() * (Span * 2.2));
			VC->SetViewRotation(Look);
			VC->Invalidate();
			return true;
		}
		return false;
	}

	bool FocusSelection()
	{
		if (!GEditor) return false;
		TArray<AActor*> Targets; SelectionTargets(Targets);
		if (Targets.Num() == 0) return false;
		FBox B(ForceInit);
		for (AActor* A : Targets)
		{
			FVector Org, Ext;
			A->GetActorBounds(false, Org, Ext);
			B += FBox(Org - Ext, Org + Ext);
		}
		if (!B.IsValid) return false;
		FLevelEditorViewportClient* VC = BF6_ViewportToFly();
		if (!VC) return false;
		const FVector C = B.GetCenter();
		const double Span = FMath::Max3((double)B.GetSize().X, (double)B.GetSize().Y, 600.0);
		FRotator Look = VC->GetViewRotation();
		if (Look.Pitch > -10.f) Look.Pitch = -30.f;   // looking flat sees nothing of it
		VC->SetViewLocation(C - Look.Vector() * (Span * 2.0));
		VC->SetViewRotation(Look);
		VC->Invalidate();
		return true;
	}

	void FocusLinkCandidate(int32 i)
	{
		if (!GLinkPick.bActive || !GLinkPick.Candidates.IsValidIndex(i)) return;
		AActor* A = GLinkPick.Candidates[i].Get();
		if (!A) return;
		FLevelEditorViewportClient* VC = BF6_ViewportToFly();
		if (!VC) return;
		FVector Org, Ext;
		A->GetActorBounds(false, Org, Ext);
		const double Span = FMath::Max3((double)Ext.X * 2.0, (double)Ext.Y * 2.0, 800.0);
		const FRotator Look(-35.f, VC->GetViewRotation().Yaw, 0.f);
		VC->SetViewLocation(Org - Look.Vector() * (Span * 2.2));
		VC->SetViewRotation(Look);
		VC->Invalidate();
	}

	void ToggleLinkCandidate(int32 Index)
	{
		if (!GEditor || !GLinkPick.Candidates.IsValidIndex(Index)) return;
		AActor* C = GLinkPick.Candidates[Index].Get();
		if (!C) return;
		const bool bWas = C->IsSelected();
		if (!GLinkPick.bArray) GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(C, !bWas, true);
	}

	AActor* PlaceType(const FString& Type, const FVector& WorldPos)
	{
		if (!g_ss.bEditing) { BF6Api::RefuseReadOnly(TEXT("Objects can only be placed on a custom map. Name one and press Create, bottom right.")); return nullptr; }
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

		// A NODE is placeable like any object, so the whole tree can be built
		// from the library without ever opening the Scene panel: place a node,
		// then ATTACH things under it by pointing at them.
		if (Type == TEXT("Node3D") && World)
		{
			FScopedTransaction Tx(FText::FromString(TEXT("Place Node")));
			World->GetCurrentLevel()->Modify();   // spawns are only undoable with the level marked
			AActor* A = BF6_SpawnTreeNode(World, FString(), FString(), FTransform(WorldPos));
			if (!A) return nullptr;
			BF6_SetPrettyLabel(A, TEXT("Node3D"));
			FString Key = A->GetActorLabel(); Key.RemoveFromStart(TEXT("BF6_"));
			A->Tags.Add(FName(*(FString(TEXT("gpath:")) + Key)));
			if (GEditor) { GEditor->SelectNone(false, true, false); GEditor->SelectActor(A, true, true); }
			return A;
		}

		// volumes have no mesh - they're built, not loaded
		if (Type == TEXT("OBBVolume") && World)
		{
			FScopedTransaction Tx(FText::FromString(TEXT("Place OBBVolume")));
			// default 10 m box, resting on the clicked surface
			AActor* A = SpawnObbActor(World, FTransform(WorldPos + FVector(0, 0, 500.f)), FVector(10, 10, 10));
			if (A && GEditor) { GEditor->SelectNone(false, true, false); GEditor->SelectActor(A, true, true); }
			return A;
		}
		if (Type == TEXT("PolygonVolume") && World)
		{
			FScopedTransaction Tx(FText::FromString(TEXT("Place PolygonVolume")));
			// default 10 m square at the clicked spot, like the SDK's default
			TArray<FVector> Loop = {
				WorldPos + FVector(-500, -500, 0), WorldPos + FVector(500, -500, 0),
				WorldPos + FVector(500, 500, 0),   WorldPos + FVector(-500, 500, 0) };
			AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (!A) return nullptr;
			BF6_SetPrettyLabel(A, TEXT("PolygonVolume"));
			A->Tags.Add(kPlacedTag);
			A->Tags.Add(FName(TEXT("label:PolygonVolume")));
			A->Tags.Add(FName(TEXT("p:height=5")));
			MakeProcMesh(A, TEXT("Volume"));
			GVolumeLoops.Add(A, Loop);
			BF6_WriteLoopTags(A);
			RebuildVolumeWalls(A, Loop);
			BF6_FileActor(A);
			if (GEditor) { GEditor->SelectNone(false, true, false); GEditor->SelectActor(A, true, true); }
			return A;
		}

		const FString Mesh = BF6_ResolveMeshForType(Type);
		if (Mesh.IsEmpty()) { Notify(FString::Printf(TEXT("No SDK model for '%s'."), *Type)); return nullptr; }
		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Place %s"), *Type)));
		AActor* A = SpawnSdkModel(Mesh, Type, FTransform(WorldPos));
		// A type that carries a WaypointPath is born WITH one. An AI_WaypointPath
		// without points is an empty shell that exports a patrol route of
		// nothing, and the SDK's own scene ships a default curve for the same
		// reason. Three points ahead of the marker, open, ready to drag.
		if (A && BF6_TypeHasWaypoints(Type))
		{
			TArray<FVector> Pts = {
				WorldPos + FVector(0,     0, 0),
				WorldPos + FVector(800,   0, 0),
				WorldPos + FVector(1600, 500, 0) };
			for (FVector& P : Pts)
			{
				FVector G;
				if (GroundRay(P + FVector(0, 0, 2000), P - FVector(0, 0, 50000), nullptr, G)) P.Z = G.Z;
			}
			GVolumeLoops.Add(A, Pts);
			BF6_WriteLoopTags(A);
			RebuildVolumeWalls(A, Pts);
		}
		if (A && GEditor) { GEditor->SelectNone(false, true, false); GEditor->SelectActor(A, true, true); }
		BF6_RecomputeBudget();
		return A;
	}

	// Library double-click / "Place in scene": drop the object in front of the
	// camera. Picking another object before touching the first one SWAPS it
	// (auditioning objects in place), so the map never collects stray props.
	static TWeakObjectPtr<AActor> GQuickPlaced;
	static FVector GQuickPlacedAt = FVector::ZeroVector;

	AActor* QuickPlace(const FString& Type)
	{
		FVector W;
		if (!WorldFromViewportCenter(W)) return nullptr;
		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Place %s"), *Type)));
		if (AActor* Prev = GQuickPlaced.Get())
			if (Prev->GetActorLocation().Equals(GQuickPlacedAt, 1.f))
			{
				W = GQuickPlacedAt;   // swap in exactly the same spot
				if (GEditor)
					if (UWorld* World = GEditor->GetEditorWorldContext().World())
					{ Prev->Modify(); World->EditorDestroyActor(Prev, true); }
			}
		GQuickPlaced = nullptr;
		AActor* A = PlaceType(Type, W);
		if (A) { GQuickPlaced = A; GQuickPlacedAt = A->GetActorLocation(); }
		return A;
	}

	// ---- selection tools ----
	void SelectSimilar()
	{
		FString Type; AActor* Seed = SelectedGameplayActor(Type);
		if (!Seed || !GEditor) return;
		// similar = SAME MESH (what the object actually is), not the same label;
		// gameplay objects without a mesh (spawn points, HQs) match by type
		const FString Ms = TagValue(Seed, TEXT("mesh:"));
		const FString Ty = TagValue(Seed, TEXT("type:"));
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)) continue;
			bool bSame = false;
			if (!Ms.IsEmpty())      bSame = TagValue(*It, TEXT("mesh:")) == Ms;
			else if (!Ty.IsEmpty()) bSame = TagValue(*It, TEXT("type:")) == Ty;
			if (!bSame) continue;
			GEditor->SelectActor(*It, true, false);
			n++;
		}
		GEditor->NoteSelectionChange();
		Notify(FString::Printf(TEXT("Selected %d x %s"), n, *(Ms.IsEmpty() ? Type : Ms)));
	}

	// ---- ObjId registry ----
	// Scripts address gameplay objects by ObjId, and duplicate or unset ids
	// quietly break modes (the community's Portal Jam was won by a plain
	// ObjId manager). The registry lists every id; auto-assign numbers a
	// selection in a spatial sweep so the numbering is predictable.
	static bool BF6_TypeHasObjId(const FString& Ty)
	{
		static TMap<FString, bool> Cache;
		if (const bool* B = Cache.Find(Ty)) return *B;
		bool bHas = false;
		for (const FPropDef& D : PropsForType(Ty))
			if (D.Name == TEXT("ObjId")) { bHas = true; break; }
		Cache.Add(Ty, bHas);
		return bHas;
	}

	static FString BF6_ObjIdType(AActor* A)
	{
		FString Ty = TagValue(A, TEXT("label:"));
		if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
		return Ty;
	}

	TArray<FObjIdRow> GatherObjIds()
	{
		TArray<FObjIdRow> Out;
		if (!GEditor) return Out;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return Out;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)) continue;
			const FString Ty = BF6_ObjIdType(*It);
			if (Ty.IsEmpty() || !BF6_TypeHasObjId(Ty)) continue;
			FObjIdRow R;
			R.Actor = *It;
			R.Name = It->GetActorLabel();
			R.Type = Ty;
			const FString V = GetActorProp(*It, TEXT("ObjId"));
			R.Id = V.IsEmpty() ? -1 : FCString::Atoi(*V);
			Out.Add(R);
		}
		return Out;
	}

	// Scripts address objects by ObjId, so an id a creator already set is data,
	// not scratch space. Assigning only FILLS BLANKS by default, never renumbers
	// what is already set, and never hands out a number in use elsewhere in the
	// level (a duplicate id breaks scripts just as badly as a changed one).
	// bOverwriteExisting is the deliberate "renumber these anyway" path.
	FObjIdAssign AutoAssignObjIds(int32 StartId, bool bOverwriteExisting)
	{
		FObjIdAssign R;
		if (!GEditor) return R;
		USelection* S = GEditor->GetSelectedActors(); if (!S) return R;
		TArray<AActor*> Targets;
		for (int32 i = 0; i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
			{
				if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
				const FString Ty = BF6_ObjIdType(A);
				if (!Ty.IsEmpty() && BF6_TypeHasObjId(Ty)) Targets.Add(A);
			}
		R.Considered = Targets.Num();
		if (Targets.Num() == 0) return R;

		// every id already spoken for, so a fresh number never collides. Ids on
		// the objects we are about to renumber are released back to the pool.
		TSet<AActor*> TargetSet(Targets);
		TSet<int32> Taken;
		for (const FObjIdRow& Row : GatherObjIds())
		{
			if (Row.Id < 0) continue;
			AActor* RA = Row.Actor.Get();
			if (bOverwriteExisting && RA && TargetSet.Contains(RA)) continue;
			Taken.Add(Row.Id);
		}

		// left-to-right, then front-to-back sweep: predictable numbering for
		// rows of flags/spawns, like the community's auto-id tools
		Targets.Sort([](const AActor& A, const AActor& B)
		{
			const FVector PA = A.GetActorLocation(), PB = B.GetActorLocation();
			if (PA.X != PB.X) return PA.X < PB.X;
			return PA.Y < PB.Y;
		});

		FScopedTransaction Tx(FText::FromString(TEXT("Assign ObjIds")));
		int32 Id = FMath::Max(0, StartId);
		for (AActor* A : Targets)
		{
			const FString Cur = GetActorProp(A, TEXT("ObjId"));
			const bool bHas = !Cur.IsEmpty() && FCString::Atoi(*Cur) >= 0;
			if (bHas && !bOverwriteExisting) { R.Kept++; continue; }   // hands off
			while (Taken.Contains(Id)) Id++;
			SetActorProp(A, TEXT("ObjId"), FString::FromInt(Id));
			Taken.Add(Id);
			R.Assigned++;
		}
		return R;
	}

	int32 SelectDuplicateObjIds()
	{
		if (!GEditor) return 0;
		TArray<FObjIdRow> All = GatherObjIds();
		TMap<int32, int32> Count;
		for (const FObjIdRow& R : All) if (R.Id >= 0) Count.FindOrAdd(R.Id)++;
		GEditor->SelectNone(false, true, false);
		int32 n = 0;
		for (const FObjIdRow& R : All)
			if (R.Id >= 0 && Count[R.Id] > 1)
				if (AActor* A = R.Actor.Get()) { GEditor->SelectActor(A, true, false); n++; }
		GEditor->NoteSelectionChange();
		return n;
	}

	void SelectOnly(AActor* A)
	{
		if (!GEditor || !A) return;
		GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(A, true, true);
	}

	// ---- snap-build + multiply ----
	// Duplication that TILES: copies land flush against the source using its
	// own footprint, so walls and floors line up without hand-nudging.
	static AActor* BF6_SingleSelectedMeshActor()
	{
		if (!GEditor) return nullptr;
		USelection* S = GEditor->GetSelectedActors(); if (!S) return nullptr;
		AActor* Found = nullptr;
		for (int32 i = 0; i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
			{
				if (!A->Tags.Contains(kPlacedTag) || TagValue(A, TEXT("mesh:")).IsEmpty()) continue;
				if (Found) return nullptr;   // exactly one
				Found = A;
			}
		return Found;
	}

	static FVector BF6_PlacedFootprint(AActor* A)
	{
		UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
		if (!M) return FVector(100, 100, 100);
		const FBoxSphereBounds B = M->CalcBounds(FTransform::Identity);
		return B.BoxExtent * 2.0 * A->GetActorScale3D();
	}

	static AActor* BF6_DuplicatePlacedMesh(AActor* Seed, const FTransform& Xf)
	{
		FString Mesh = TagValue(Seed, TEXT("mesh:"));
		FString Ty = TagValue(Seed, TEXT("label:"));
		if (Ty.IsEmpty()) Ty = TagValue(Seed, TEXT("type:"));
		// BASE objects (the map's own trees and props) carry a type, not a
		// mesh tag - resolve it so they scatter/duplicate too; the copy is a
		// normal PLACED object
		if (Mesh.IsEmpty()) Mesh = BF6_ResolveMeshForType(Ty);
		if (Mesh.IsEmpty()) return nullptr;
		AActor* A = SpawnSdkModel(Mesh, Ty, Xf);
		if (!A) return nullptr;
		for (const FName& T : Seed->Tags)
		{
			const FString TS = T.ToString();
			if (!TS.StartsWith(TEXT("p:"))) continue;
			if (TS.StartsWith(TEXT("p:ObjId="))) continue;   // ids never duplicate
			A->Tags.Add(T);
		}
		return A;
	}

	// ---- multiply unit ----
	// The thing being multiplied: one object, a whole group, or a block -
	// whatever the selection holds. Copies reproduce the entire arrangement.
	// (block/link helpers live in the blocks section further down)
	static FString BF6_LinkName(AActor* A);
	static bool BF6_IsLinkProp(const FString& TypeName, const FString& PropName);
	static int32 BF6_PruneDeadLinks();   // fwd: the fast delete prunes inside its own transaction
	static void BF6_TagBlockInstance(const TArray<AActor*>& Actors, const FString& Name, const FVector& Anchor);

	struct FBF6MultiUnit
	{
		TArray<AActor*> Members;
		FVector Center = FVector::ZeroVector;   // bounds centre, on the ground
		FVector Size = FVector(100, 100, 100);  // combined world bounds
		bool bBlock = false;
		FString BlockName;
	};

	static bool BF6_GatherMultiUnit(FBF6MultiUnit& U)
	{
		if (!GEditor) return false;
		// a node stands for what hangs off it, so multiplying one multiplies the lot
		TArray<AActor*> Targets; SelectionTargets(Targets);
		for (AActor* A : Targets)
			{
				if (Cast<AGroupActor>(A) || A->Tags.Contains(kHandleTag)) continue;
				if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
				FString Mesh = TagValue(A, TEXT("mesh:"));
				if (Mesh.IsEmpty())
				{
					FString Ty = TagValue(A, TEXT("label:"));
					if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
					Mesh = BF6_ResolveMeshForType(Ty);
				}
				if (Mesh.IsEmpty()) continue;   // volumes can't multiply
				U.Members.Add(A);
				const FString Blk = TagValue(A, TEXT("blk:"));
				if (!Blk.IsEmpty()) { U.bBlock = true; U.BlockName = Blk; }
			}
		if (U.Members.Num() == 0) return false;
		FBox Box(ForceInit);
		for (AActor* A : U.Members) Box += A->GetComponentsBoundingBox(true);
		U.Center = Box.GetCenter();
		U.Center.Z = Box.Min.Z;
		U.Size = Box.GetSize().ComponentMax(FVector(50, 50, 50));
		return true;
	}

	// Spawn one copy of the whole unit at TargetGround, rotated by YawDeg and
	// scaled uniformly. Intra-unit links remap to the fresh copies, a block
	// unit becomes its own new instance, and multi-member copies group.
	static int32 BF6_SpawnUnitCopy(const FBF6MultiUnit& U, const FVector& TargetGround, double YawDeg, double Scale,
		FCollisionQueryParams& QP, TArray<AActor*>& OutNew, double TiltXDeg = 0.0, double TiltYDeg = 0.0)
	{
		// yaw first, then the scatter wobble tilts (X roll, Y pitch)
		const FQuat YawQ = FQuat(FVector::UpVector, FMath::DegreesToRadians(YawDeg))
			* FQuat(FVector::ForwardVector, FMath::DegreesToRadians(TiltXDeg))
			* FQuat(FVector::RightVector, FMath::DegreesToRadians(TiltYDeg));
		TArray<AActor*> Copies;
		TMap<FString, FString> NameMap;   // original link name -> copy link name
		for (AActor* M : U.Members)
		{
			const FVector Off = (M->GetActorLocation() - U.Center) * Scale;
			const FVector P = TargetGround + YawQ.RotateVector(Off);
			const FQuat R = YawQ * M->GetActorQuat();
			if (AActor* A = BF6_DuplicatePlacedMesh(M, FTransform(R, P, M->GetActorScale3D() * Scale)))
			{
				Copies.Add(A);
				QP.AddIgnoredActor(A);
				NameMap.Add(BF6_LinkName(M), BF6_LinkName(A));
			}
		}
		if (Copies.Num() == 0) return 0;
		// links between members follow the copies, not the originals
		for (AActor* A : Copies)
			for (int32 t = A->Tags.Num() - 1; t >= 0; t--)
			{
				const FString TS = A->Tags[t].ToString();
				if (!TS.StartsWith(TEXT("p:"))) continue;
				int32 Eq = INDEX_NONE;
				if (!TS.FindChar(TEXT('='), Eq)) continue;
				const FString Key = TS.Mid(2, Eq - 2);
				FString Ty = TagValue(A, TEXT("label:"));
				if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
				if (!BF6_IsLinkProp(Ty, Key)) continue;
				TArray<FString> Parts;
				TS.Mid(Eq + 1).ParseIntoArray(Parts, TEXT(","));
				bool bChanged = false;
				for (FString& Pt : Parts)
				{
					Pt = Pt.TrimStartAndEnd();
					if (const FString* NewNm = NameMap.Find(Pt)) { Pt = *NewNm; bChanged = true; }
				}
				if (!bChanged) continue;
				A->Tags.RemoveAt(t);
				A->Tags.Add(FName(*(TS.Left(Eq + 1) + FString::Join(Parts, TEXT(",")))));
			}
		if (U.bBlock && !U.BlockName.IsEmpty())
			BF6_TagBlockInstance(Copies, U.BlockName, TargetGround);
		if (Copies.Num() > 1)
		{
			if (!UActorGroupingUtils::IsGroupingActive()) UActorGroupingUtils::SetGroupingActive(true);
			UActorGroupingUtils::Get()->GroupActors(Copies);
		}
		OutNew.Append(Copies);
		return Copies.Num();
	}

	bool SnapBuildDuplicate(int32 Dir)
	{
		AActor* Seed = BF6_SingleSelectedMeshActor();
		if (!Seed) return false;
		FVector Want;
		if (Dir == 4) Want = FVector::UpVector;
		else if (Dir == 5) Want = -FVector::UpVector;
		else
		{
			FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
			if (!VC) return false;
			const FRotationMatrix Cam(VC->GetViewRotation());
			FVector R = Cam.GetScaledAxis(EAxis::Y); R.Z = 0;
			FVector F = Cam.GetScaledAxis(EAxis::X); F.Z = 0;
			if (!R.Normalize()) R = FVector::RightVector;
			if (!F.Normalize()) F = FVector::ForwardVector;
			Want = Dir == 0 ? R : Dir == 1 ? -R : Dir == 2 ? F : -F;
		}
		// step along the SEED's closest local axis so copies tile with the
		// object's own orientation, not the world grid
		const FVector Size = BF6_PlacedFootprint(Seed);
		const FQuat Q = Seed->GetActorQuat();
		const FVector Axes[3] = { Q.GetAxisX(), Q.GetAxisY(), Q.GetAxisZ() };
		int32 Best = 0; double BestD = 0.0;
		for (int32 i = 0; i < 3; i++)
		{
			const double D = FVector::DotProduct(Axes[i], Want);
			if (FMath::Abs(D) > FMath::Abs(BestD)) { Best = i; BestD = D; }
		}
		const double Sign = BestD >= 0.0 ? 1.0 : -1.0;
		const double Step = Best == 0 ? Size.X : Best == 1 ? Size.Y : Size.Z;
		if (Step <= 1.0) return false;
		FScopedTransaction Tx(FText::FromString(TEXT("Snap build")));
		FTransform Xf = Seed->GetActorTransform();
		Xf.AddToTranslation(Axes[Best] * Sign * Step);
		AActor* Copy = BF6_DuplicatePlacedMesh(Seed, Xf);
		if (!Copy) return false;
		SelectOnly(Copy);   // chain: the next press builds from the new copy
		BF6_RecomputeBudget();
		return true;
	}

	int32 MultiplyGrid(int32 Count, int32 Rows, double GapMetres)
	{
		FBF6MultiUnit U;
		if (!BF6_GatherMultiUnit(U)) { Notify(TEXT("Select an object, a group, or a block first.")); return 0; }
		Count = FMath::Clamp(Count, 1, 100);
		Rows = FMath::Clamp(Rows, 1, 100);
		if (Count * Rows * U.Members.Num() > 800) { Notify(TEXT("That would be over 800 objects - use smaller numbers.")); return 0; }
		const double Gap = GapMetres * 100.0;
		// tile along the (first member's) facing so rotated rows stay straight
		const FQuat Q = U.Members[0]->GetActorQuat();
		const FVector Right = Q.GetAxisY(), Fwd = Q.GetAxisX();
		const double YawDeg = 0.0;   // grid copies keep the unit's orientation
		FScopedTransaction Tx(FText::FromString(TEXT("Multiply grid")));
		FCollisionQueryParams QP(FName(TEXT("BF6Multiply")), true);
		TArray<AActor*> NewOnes;
		for (int32 r = 0; r < Rows; r++)
			for (int32 c = 0; c < Count; c++)
			{
				if (r == 0 && c == 0) continue;   // the seed IS cell one
				const FVector P = U.Center + Right * (double)c * (U.Size.Y + Gap) + Fwd * (double)r * (U.Size.X + Gap);
				BF6_SpawnUnitCopy(U, P, YawDeg, 1.0, QP, NewOnes);
			}
		GEditor->SelectNone(false, true, false);
		for (AActor* A : NewOnes) GEditor->SelectActor(A, true, false);
		GEditor->NoteSelectionChange();
		BF6_RecomputeBudget();
		return NewOnes.Num();
	}

	int32 MultiplyCircle(int32 Count, double RadiusMetres)
	{
		FBF6MultiUnit U;
		if (!BF6_GatherMultiUnit(U)) { Notify(TEXT("Select an object, a group, or a block first.")); return 0; }
		Count = FMath::Clamp(Count, 1, 200);
		const double R = FMath::Max(RadiusMetres, 0.5) * 100.0;
		FScopedTransaction Tx(FText::FromString(TEXT("Multiply circle")));
		FCollisionQueryParams QP(FName(TEXT("BF6Multiply")), true);
		TArray<AActor*> NewOnes;
		for (int32 i = 0; i < Count; i++)
		{
			const double Ang = 2.0 * PI * (double)i / (double)Count;
			const FVector Off(FMath::Cos(Ang) * R, FMath::Sin(Ang) * R, 0.0);
			// every copy faces the centre, like posts around a ring
			const double YawDeg = FMath::RadiansToDegrees(FMath::Atan2(-Off.Y, -Off.X));
			BF6_SpawnUnitCopy(U, U.Center + Off, YawDeg, 1.0, QP, NewOnes);
		}
		GEditor->SelectNone(false, true, false);
		for (AActor* A : NewOnes) GEditor->SelectActor(A, true, false);
		GEditor->NoteSelectionChange();
		BF6_RecomputeBudget();
		return NewOnes.Num();
	}

	// Scatter, the Proton-Scatter idea kept simple: N copies at random spots
	// inside a circle, each with its own rotation (and a little size variation
	// if asked), every one dropped onto the ground by a vertical trace. Made
	// for trees, rocks, and clutter. A spacing floor derived from the object's
	// own footprint stops ugly clumping without adding a knob.
	void CollectPlacedIn(const FBox2D& Area, TArray<AActor*>& Out);            // fwd (ray section)
	bool GroundRay(const FVector& From, const FVector& To, const TArray<AActor*>* Placed, FVector& OutHit, FVector* OutNormal);   // default is on the earlier declaration

	int32 MultiplyScatter(int32 Count, double RadiusMetres, bool bVarySize)
	{
		FBF6MultiUnit U;
		if (!BF6_GatherMultiUnit(U)) { Notify(TEXT("Select an object, a group, or a block first.")); return 0; }
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		Count = FMath::Clamp(Count, 1, 500);
		if (Count * U.Members.Num() > 800) { Notify(TEXT("That would be over 800 objects - use a smaller count.")); return 0; }
		const double R = FMath::Max(RadiusMetres, 1.0) * 100.0;
		// The COUNT wins: spacing starts from the unit's footprint but shrinks
		// until the requested number fits the circle (real forests overlap
		// canopies) - a fixed footprint spacing silently capped tree scatters
		// around 25 at the default radius.
		const double FitDist = FMath::Sqrt((PI * R * R) / ((double)Count * 2.6));
		const double MinDist = FMath::Clamp(FMath::Min(FMath::Max(U.Size.X, U.Size.Y) * 0.7, FitDist), 50.0, R);

		FScopedTransaction Tx(FText::FromString(TEXT("Scatter")));
		FCollisionQueryParams QP(FName(TEXT("BF6Scatter")), true);
		for (AActor* M : U.Members) QP.AddIgnoredActor(M);
		// what is already standing in the circle, so copies can sit on it
		TArray<AActor*> Nearby;
		CollectPlacedIn(FBox2D(FVector2D(U.Center.X - R, U.Center.Y - R),
			FVector2D(U.Center.X + R, U.Center.Y + R)), Nearby);
		Nearby.RemoveAll([&U](AActor* A){ return U.Members.Contains(A); });
		TArray<FVector> Placed;
		Placed.Add(U.Center);
		TArray<AActor*> NewOnes;
		int32 nUnits = 0, Attempts = 0;
		while (nUnits < Count && Attempts++ < Count * 40)
		{
			// uniform in the disc
			const double Rr = R * FMath::Sqrt(FMath::FRand());
			const double Th = FMath::FRand() * 2.0 * PI;
			FVector P = U.Center + FVector(FMath::Cos(Th) * Rr, FMath::Sin(Th) * Rr, 0.0);
			bool bTooClose = false;
			for (const FVector& Q : Placed)
				if (FVector::DistSquaredXY(P, Q) < MinDist * MinDist) { bTooClose = true; break; }
			if (bTooClose) continue;
			// land the WHOLE unit on the ground: near-height first (so a
			// scatter under a bridge stays under it), high-altitude second
			// (hillsides), and with no hit at all the seed's height stands -
			// nothing ever falls into the void on terrain-less spots
			FVector Gnd;
			if (GroundRay(P + FVector(0, 0, 500), P - FVector(0, 0, 50000), &Nearby, Gnd))
				P.Z = Gnd.Z;
			else if (GroundRay(P + FVector(0, 0, 50000), P - FVector(0, 0, 50000), &Nearby, Gnd))
				P.Z = Gnd.Z;
			const double Yaw = FMath::FRandRange(0.0, 360.0);
			const double Scale = bVarySize ? FMath::FRandRange(0.85, 1.15) : 1.0;
			if (BF6_SpawnUnitCopy(U, P, Yaw, Scale, QP, NewOnes) > 0)
			{
				Placed.Add(P);
				nUnits++;
			}
		}
		GEditor->SelectNone(false, true, false);
		for (AActor* A : NewOnes) GEditor->SelectActor(A, true, false);
		GEditor->NoteSelectionChange();
		BF6_RecomputeBudget();
		if (nUnits < Count)
			Notify(FString::Printf(TEXT("Scattered %d of %d - the circle filled up. A bigger radius fits more."), nUnits, Count));
		return nUnits;
	}

	// Re-file every object into its role/category folder. Fixes a level built
	// before auto-organizing, or one whose objects were hand-moved around.
	// Which view the outliner is in, and the flip. Switching re-files every
	// object, so it is instant and reversible - the authored path rides along
	// on each actor, so going back to the Godot tree never loses anything.
	bool KeepingGodotTree() { return BF6_KeepGodotTree(); }

	bool AnyGodotTree()
	{
		if (!GEditor) return false;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return false;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (!TagValue(*It, TEXT("gtree:")).IsEmpty()) return true;
		return false;
	}

	// Godot's other half of tree editing: making a node to hang things off. A new
	// node lands under the selection if there is one, at the selection's centre so
	// the pivot is where the creator is looking, and empty otherwise.
	AActor* AddTreeNode()
	{
		// the base map is read only - nothing is added to it, from here or anywhere
		if (!GEditor || !IsEditing()) return nullptr;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return nullptr;
		TArray<AActor*> Sel;
		USelection* S = GEditor->GetSelectedActors();
		for (int32 i = 0; S && i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i))) Sel.Add(A);

		FVector Where = FVector::ZeroVector;
		if (Sel.Num() > 0)
		{
			for (AActor* A : Sel) Where += A->GetActorLocation();
			Where /= Sel.Num();
		}
		else if (FEditorViewportClient* VC = (FEditorViewportClient*)GEditor->GetActiveViewport()->GetClient())
			Where = VC->GetViewLocation() + VC->GetViewRotation().Vector() * 1000.f;

		FScopedTransaction Tx(FText::FromString(TEXT("Add Node")));
		AActor* Node = BF6_SpawnTreeNode(W, FString(), FString(), FTransform(Where));
		if (!Node) return nullptr;
		BF6_SetPrettyLabel(Node, TEXT("Node3D"));
		FString Key = Node->GetActorLabel(); Key.RemoveFromStart(TEXT("BF6_"));
		Node->Tags.Add(FName(*(FString(TEXT("gpath:")) + Key)));
		// under whatever the selection hangs off, so it lands beside its siblings
		if (Sel.Num() > 0)
			if (AActor* Up = Sel[0]->GetAttachParentActor())
				Node->AttachToActor(Up, FAttachmentTransformRules::KeepWorldTransform);
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Node, true, true);
		RefreshSceneTree();
		return Node;
	}

	// Godot's 'reparent to new node': the selection keeps its world transforms and
	// moves as one from here on.
	// ---- moving things into an EXISTING node ------------------------------
	//
	// Unreal's actor menu offers "Move To" a FOLDER, which is the wrong idea in
	// this tool: folders are an editor convenience that Portal knows nothing
	// about, and the thing that actually parents objects here - and that does
	// reach the export - is a node. Offering both would be offering a creator a
	// choice between the real one and a decoy.
	static void BF6_CollectNodes(TArray<AActor*>& Out)
	{
		Out.Reset();
		if (!GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(kGroupTag)) Out.Add(*It);
		Out.Sort([](const AActor& A, const AActor& B)
			{ return A.GetActorLabel() < B.GetActorLabel(); });
	}

	int32 TreeNodeCount()
	{
		TArray<AActor*> N; BF6_CollectNodes(N); return N.Num();
	}

	FString TreeNodeName(int32 i)
	{
		TArray<AActor*> N; BF6_CollectNodes(N);
		if (!N.IsValidIndex(i)) return FString();
		FString Nm = N[i]->GetActorLabel();
		Nm.RemoveFromStart(TEXT("BF6_"));
		return Nm;
	}

	// ---- attach pick: parenting by pointing --------------------------------
	//
	// The tree can drag-parent and the wheel can list nodes, but the fastest
	// statement of "this goes under that" is looking at THAT and clicking it.
	// The selection is captured when the mode begins, so clicking the target
	// cannot deselect the things being attached.
	struct FBF6AttachPick
	{
		bool bActive = false;
		TArray<TWeakObjectPtr<AActor>> Movers;
	};
	static FBF6AttachPick GAttachPick;

	bool IsAttachPicking() { return GAttachPick.bActive; }

	void BeginAttachPick()
	{
		GAttachPick = FBF6AttachPick();
		TArray<AActor*> Sel; SelectionTargets(Sel);
		if (Sel.Num() == 0) { Notify(TEXT("Select what you want to attach first.")); return; }
		for (AActor* A : Sel) GAttachPick.Movers.Add(A);
		GAttachPick.bActive = true;
	}

	void CancelAttachPick() { GAttachPick = FBF6AttachPick(); }

	int32 ConfirmAttachPick(AActor* Target)
	{
		if (!GAttachPick.bActive) return 0;
		if (!Target) return 0;

		// only OUR objects can parent, and never something riding a group or
		// block: the group's click-selects-all lock would fight every later
		// attempt to select the parent on its own
		const bool bOurs = Target->Tags.Contains(kPlacedTag) || Target->Tags.Contains(kBaseTag) || Target->Tags.Contains(kGroupTag);
		if (!bOurs || Target->Tags.Contains(kHandleTag) || Target->Tags.Contains(kContextTag)) return 0;
		if (Target->GroupActor != nullptr)
		{
			Notify(TEXT("That object is inside a group or block - it cannot take children. Pick something loose, or a node."));
			return 0;
		}

		FScopedTransaction Tx(FText::FromString(TEXT("Attach to object")));
		int32 n = 0;
		for (const TWeakObjectPtr<AActor>& Wk : GAttachPick.Movers)
		{
			AActor* A = Wk.Get();
			if (!A || A == Target) continue;
			// no cycles: the target must not hang under anything being moved
			bool bCycle = false;
			for (AActor* Up = Target; Up; Up = Up->GetAttachParentActor())
				if (Up == A) { bCycle = true; break; }
			if (bCycle) continue;
			A->Modify();
			if (USceneComponent* RC = A->GetRootComponent()) RC->Modify();   // undo needs the component
			A->AttachToActor(Target, FAttachmentTransformRules::KeepWorldTransform);
			n++;
		}
		GAttachPick = FBF6AttachPick();
		if (n > 0)
		{
			RefreshSceneTree();
			// The attach is done. Leaving the movers selected read as the mode
			// still waiting for another click - an empty selection is the
			// visible "finished", and the toast already says what happened.
			ClearSelection();
		}
		return n;
	}

	// Fly the view to node i without selecting it - the same look-before-you-
	// commit courtesy the link picker gives its candidates.
	void FocusTreeNode(int32 i)
	{
		TArray<AActor*> Nodes; BF6_CollectNodes(Nodes);
		if (!Nodes.IsValidIndex(i) || !Nodes[i]) return;
		FLevelEditorViewportClient* VC = BF6_ViewportToFly();
		if (!VC) return;
		FVector Org, Ext;
		Nodes[i]->GetActorBounds(false, Org, Ext);
		const double Span = FMath::Max3((double)Ext.X * 2.0, (double)Ext.Y * 2.0, 800.0);
		const FRotator Look(-35.f, VC->GetViewRotation().Yaw, 0.f);
		VC->SetViewLocation(Org - Look.Vector() * (Span * 1.6));
		VC->SetViewRotation(Look);
		VC->Invalidate();
	}

	// Detach the selection back to the map root, world positions kept - the
	// inverse of Move to node, so a wrong drop is one click to undo by hand.
	int32 DetachSelectionToRoot()
	{
		if (!GEditor) return 0;
		TArray<AActor*> Sel; SelectionTargets(Sel);
		FScopedTransaction Tx(FText::FromString(TEXT("Detach from node")));
		int32 n = 0;
		for (AActor* A : Sel)
		{
			if (!A || !A->GetAttachParentActor()) continue;
			A->Modify();
			if (USceneComponent* RC = A->GetRootComponent()) RC->Modify();   // undo needs the component
			A->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			n++;
		}
		if (n > 0)
		{
			RefreshSceneTree();
			ClearSelection();   // same "finished" statement as an attach
		}
		return n;
	}

	int32 AttachSelectionToNode(int32 i)
	{
		if (!GEditor) return 0;
		TArray<AActor*> Nodes; BF6_CollectNodes(Nodes);
		if (!Nodes.IsValidIndex(i)) return 0;
		AActor* Node = Nodes[i];
		TArray<AActor*> Sel; SelectionTargets(Sel);
		FScopedTransaction Tx(FText::FromString(TEXT("Move to node")));
		int32 n = 0;
		for (AActor* A : Sel)
		{
			if (!A || A == Node) continue;
			// A node cannot be moved under something hanging off itself - that
			// makes a cycle, and the tree walk that builds the export would not
			// come back from it.
			bool bCycle = false;
			for (AActor* Up = Node; Up; Up = Up->GetAttachParentActor())
				if (Up == A) { bCycle = true; break; }
			if (bCycle) continue;
			A->Modify();
			// Ctrl+Z must undo the attach, and the attachment is a property of
			// the ROOT COMPONENT - Modify() on the actor alone records nothing
			// the undo system can put back.
			if (USceneComponent* RC = A->GetRootComponent()) RC->Modify();
			A->AttachToActor(Node, FAttachmentTransformRules::KeepWorldTransform);
			n++;
		}
		if (n > 0)
		{
			RefreshSceneTree();
			ClearSelection();   // same "finished" statement as an attach
		}
		return n;
	}

	// ---- attach search: find the parent by name ----------------------------
	//
	// The panel's list only holds nodes, but attach takes ANY loose object -
	// and on a real map "the thing called Team1_HQ" is faster to type than to
	// fly to. Same target rules as clicking it in the world.
	static bool BF6_AttachableTarget(AActor* A)
	{
		if (!A) return false;
		const bool bOurs = A->Tags.Contains(kPlacedTag) || A->Tags.Contains(kBaseTag) || A->Tags.Contains(kGroupTag);
		if (!bOurs || A->Tags.Contains(kHandleTag) || A->Tags.Contains(kContextTag)) return false;
		if (A->GroupActor != nullptr) return false;   // group members cannot parent
		return true;
	}

	static AActor* BF6_AttachSearchFind(const FString& Name)
	{
		if (Name.IsEmpty() || !GEditor) return nullptr;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			FString Nm = It->GetActorLabel();
			Nm.RemoveFromStart(TEXT("BF6_"));
			if (Nm == Name) return *It;
		}
		return nullptr;
	}

	int32 AttachCandidates(const FString& Query, TArray<FString>& OutNames, int32 Max)
	{
		OutNames.Reset();
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return 0;
		TArray<FString> Toks;
		Query.ParseIntoArrayWS(Toks);
		TArray<AActor*> Sel; SelectionTargets(Sel);
		// Nodes first: they exist to be parents, so with equal claim on the
		// words they outrank a prop that merely shares them.
		TArray<FString> Nodes, Rest;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (!BF6_AttachableTarget(A) || Sel.Contains(A)) continue;
			FString Nm = A->GetActorLabel();
			Nm.RemoveFromStart(TEXT("BF6_"));
			bool bAll = true;
			for (const FString& T : Toks)
				if (!Nm.Contains(T)) { bAll = false; break; }
			if (!bAll) continue;
			(A->Tags.Contains(kGroupTag) ? Nodes : Rest).Add(Nm);
		}
		Nodes.Sort(); Rest.Sort();
		const int32 Total = Nodes.Num() + Rest.Num();
		for (const FString& N : Nodes) { if (OutNames.Num() >= Max) break; OutNames.Add(N); }
		for (const FString& N : Rest)  { if (OutNames.Num() >= Max) break; OutNames.Add(N); }
		return Total;
	}

	int32 AttachSelectionToName(const FString& Name)
	{
		if (!GEditor) return 0;
		AActor* Target = BF6_AttachSearchFind(Name);
		if (!Target || !BF6_AttachableTarget(Target)) return 0;
		TArray<AActor*> Sel; SelectionTargets(Sel);
		FScopedTransaction Tx(FText::FromString(TEXT("Attach to object")));
		int32 n = 0;
		for (AActor* A : Sel)
		{
			if (!A || A == Target) continue;
			bool bCycle = false;
			for (AActor* Up = Target; Up; Up = Up->GetAttachParentActor())
				if (Up == A) { bCycle = true; break; }
			if (bCycle) continue;
			A->Modify();
			if (USceneComponent* RC = A->GetRootComponent()) RC->Modify();   // undo needs the component
			A->AttachToActor(Target, FAttachmentTransformRules::KeepWorldTransform);
			n++;
		}
		if (n > 0)
		{
			RefreshSceneTree();
			ClearSelection();   // same "finished" statement as an attach
		}
		return n;
	}

	int32 GroupSelectionUnderNode()
	{
		if (!GEditor || !IsEditing()) return 0;
		TArray<AActor*> Sel;
		USelection* S = GEditor->GetSelectedActors();
		for (int32 i = 0; S && i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i))) Sel.Add(A);
		if (Sel.Num() == 0) return 0;
		AActor* Node = AddTreeNode();
		if (!Node) return 0;
		for (AActor* A : Sel)
			if (A != Node)
			{
				A->Modify();
				if (USceneComponent* RC = A->GetRootComponent()) RC->Modify();   // undo needs the component
				A->AttachToActor(Node, FAttachmentTransformRules::KeepWorldTransform);
			}
		RefreshSceneTree();
		// The node is what this action made, so the node is what stays
		// selected - same as placing one from the library.
		if (GEditor) { GEditor->SelectNone(false, true, false); GEditor->SelectActor(Node, true, true); }
		return Sel.Num();
	}

	// Quick hides for the two things that clutter a viewport while building:
	// zone walls and the node markers. Both stay ON, and this only hides them in
	// the viewport - nothing leaves the tree, and nothing changes on export.
	static bool GVolumesShown = true, GNodesShown = true;

	bool VolumesShown() { return GVolumesShown; }
	bool NodesShown()   { return GNodesShown; }

	int32 SetVolumesShown(bool bShow)
	{
		GVolumesShown = bShow;
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (IsVolumeActor(*It) || IsObbActor(*It))
			{ It->SetIsTemporarilyHiddenInEditor(!bShow); n++; }
		BF6_Redraw();
		return n;
	}

	int32 SetNodesShown(bool bShow)
	{
		GNodesShown = bShow;
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(kGroupTag))
			{
				// hide the marker, not the subtree: children stay visible
				if (UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(It->GetRootComponent()))
					M->SetVisibility(bShow, false);
				n++;
			}
		BF6_Redraw();
		return n;
	}

	// ---- walking the map ----
	// A creator cannot judge scale from a flying camera: a doorway, a wall, a
	// crate all look right from above and wrong at eye level. This walks the
	// editor camera at head height with gravity, standing on the same surfaces
	// the placement ray uses - the map's terrain and assets, and anything placed.
	// No PIE, no pawn: the whole tool stays live, so objects can still be placed
	// and edited while walking around them.
	struct FBF6Walk
	{
		bool bActive = false;
		FVector SavedLoc = FVector::ZeroVector;
		FRotator SavedRot = FRotator::ZeroRotator;
		double VelZ = 0.0;
		FVector Vel = FVector::ZeroVector;   // horizontal momentum
		bool bGrounded = false;
		bool bCrouch = false;
		float SavedFov = 90.f;
		double Eye = 172.0;          // measured off a spawn point when the map has one
		TArray<AActor*> Nearby;      // placed objects worth testing, refreshed as we go
		FVector NearbyAt = FVector::ZeroVector;
	};
	static FBF6Walk GWalk;

	// A six foot soldier is 183 cm tall and sees from about 172, which is the
	// fallback. Better is to MEASURE the map: a SpawnPoint marker is a soldier's
	// height, so standing eye-to-eye with one is right whatever the scale turns
	// out to be - and it self-corrects if the import scale is ever not 1:100.
	static const double kEyeHeight = 172.0;

	static double BF6_MeasureSoldierEye()
	{
		if (!GEditor) return kEyeHeight;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return kEyeHeight;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)) continue;
			FString Ty = TagValue(*It, TEXT("label:"));
			if (Ty.IsEmpty()) Ty = TagValue(*It, TEXT("type:"));
			if (!Ty.Contains(TEXT("SpawnPoint"))) continue;
			FVector Org, Ext;
			It->GetActorBounds(false, Org, Ext);
			const double H = Ext.Z * 2.0;
			if (H > 50.0 && H < 600.0)
			{
				UE_LOG(LogBF6, Log, TEXT("walk: spawn point stands %.0f cm, eye at %.0f"), H, H - 12.0);
				return H - 12.0;   // the eyes sit just below the top of the head
			}
		}
		return kEyeHeight;
	}
	static const double kStepUp    = 45.0;    // curbs and low crates, not walls
	static const double kRadius    = 35.0;    // shoulder room for the wall probes

	bool IsWalking() { return GWalk.bActive; }

	// the surface under a point, terrain/assets plus whatever is placed nearby
	static bool BF6_WalkGround(const FVector& From, double Down, double& OutZ)
	{
		FVector Hit;
		if (!GroundRay(From, From - FVector(0, 0, Down), &GWalk.Nearby, Hit)) return false;
		OutZ = Hit.Z;
		return true;
	}

	void ToggleWalk()
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC) return;
		if (GWalk.bActive)
		{
			// Stand up where you walked to, not where you dropped in. Creators walk
			// somewhere to look at it and then want to keep building THERE - being
			// thrown back across the map on the way out undoes the whole point.
			// Only the lens goes back to the editor's.
			VC->ViewFOV = GWalk.SavedFov;
			GWalk.bActive = false;
			GWalk.Nearby.Reset();
			GWalk.Vel = FVector::ZeroVector;
			Notify(TEXT("Flying again, from where you were standing."));
			BF6_Redraw();
			return;
		}
		GWalk.SavedLoc = VC->GetViewLocation();
		GWalk.SavedRot = VC->GetViewRotation();
		GWalk.VelZ = 0.0;
		GWalk.NearbyAt = FVector(BIG_NUMBER);
		GWalk.bActive = true;
		GWalk.Vel = FVector::ZeroVector;
		GWalk.bGrounded = false;
		GWalk.Eye = BF6_MeasureSoldierEye();
		GWalk.SavedFov = VC->ViewFOV;
		VC->ViewFOV = 90.f;   // an FPS lens, not the editor's 60
		// No snapping to the ground: you drop in from wherever the camera was and
		// FALL. Teleporting someone onto the terrain loses the very thing they
		// came for - a sense of the height they were looking from - and falling
		// off a roof to see how far it is down is worth having.
		FRotator R = GWalk.SavedRot; R.Roll = 0.f;   // level the horizon, keep the aim
		VC->SetViewRotation(R);
		Notify(TEXT("On foot. Mouse looks, WASD walks, Shift runs, Ctrl crouches, Space jumps. F for the menu, Esc to fly again."));
		BF6_Redraw();
	}

	// Move-and-slide, the shape every character controller uses (Rapier, Unity's
	// CharacterController, Godot's move_and_slide): step the movement, and when
	// it hits something, take the part of it that runs ALONG the surface and try
	// again. Sliding on the surface normal is what stops a shoulder against a
	// wall from halting you dead, and what lets a corner feel like a corner.
	// Constants follow Unreal's own character defaults - 45 cm step, ~45 degree
	// slope limit, 34 cm radius - so a map that walks right here walks right in
	// game.
	void WalkJump()
	{
		if (GWalk.bActive && GWalk.bGrounded) { GWalk.VelZ = 420.0; GWalk.bGrounded = false; }
	}

	void WalkCrouch(bool bDown) { GWalk.bCrouch = bDown; }

	void TickWalk(float Dt, float Fwd, float Strafe, bool bRun)
	{
		if (!GWalk.bActive) return;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC) { GWalk.bActive = false; return; }
		Dt = FMath::Clamp(Dt, 0.001f, 0.1f);   // a stall must not fling the walker

		FVector Pos = VC->GetViewLocation();
		if (FVector::DistSquared(Pos, GWalk.NearbyAt) > FMath::Square(1500.0))
		{
			const FVector2D C(Pos.X, Pos.Y);
			CollectPlacedIn(FBox2D(C - FVector2D(4000, 4000), C + FVector2D(4000, 4000)), GWalk.Nearby);
			GWalk.NearbyAt = Pos;
		}

		const FRotator Yaw(0.f, VC->GetViewRotation().Yaw, 0.f);
		FVector Want = Yaw.Vector() * Fwd + FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y) * Strafe;
		if (!Want.IsNearlyZero()) Want.Normalize();
		// Accelerate toward the wanted direction and coast when input stops, so
		// starting, stopping and turning have weight. Instant velocity is what
		// makes a walker feel like a spreadsheet rather than a game.
		const double Top = (GWalk.bCrouch ? 140.0 : (bRun ? 600.0 : 260.0));
		const FVector Target = Want * Top;
		const double Accel = GWalk.bGrounded ? 2400.0 : 700.0;   // less control in the air
		GWalk.Vel = FMath::VInterpConstantTo(GWalk.Vel, Target, Dt, Accel);
		FVector Move = GWalk.Vel * Dt;

		// ---- horizontal: slide along whatever we run into ----
		const double Skin = 8.0;      // a small gap, or probes start inside the wall
		for (int32 Pass = 0; Pass < 4 && !Move.IsNearlyZero(); Pass++)
		{
			const double Len = Move.Size();
			const FVector Dir = Move / Len;
			bool bHit = false;
			FVector Normal(0, 0, 1);
			// knees, chest and head: a doorway must pass, a railing must not
			for (double H : { -GWalk.Eye + kStepUp + 10.0, -GWalk.Eye * 0.45, -10.0 })
			{
				const FVector From = Pos + FVector(0, 0, H);
				FVector P, N;
				if (!GroundRay(From, From + Dir * (Len + kRadius), &GWalk.Nearby, P, &N)) continue;
				bHit = true; Normal = N; break;
			}
			if (!bHit) { Pos += Move; break; }
			// keep the part of the move that runs along the surface
			Normal.Z = 0.0;
			if (Normal.IsNearlyZero()) break;
			Normal.Normalize();
			Move -= Normal * FVector::DotProduct(Move, Normal);
			Move -= Dir * FMath::Min(Len, Skin);   // never end the frame inside it
		}

		// ---- vertical: gravity, ground, and the slope you are allowed to stand on ----
		const bool bWasOnFloor = GWalk.bGrounded;   // snapping is only for walkers
		GWalk.VelZ = FMath::Max(GWalk.VelZ - 980.0 * Dt, -3000.0);
		Pos.Z += GWalk.VelZ * Dt;

		const double Eye = GWalk.bCrouch ? GWalk.Eye * 0.58 : GWalk.Eye;
		const FVector Probe(Pos.X, Pos.Y, Pos.Z - Eye + kStepUp);
		FVector GroundPt, GroundN(0, 0, 1);
		if (GroundRay(Probe, Probe - FVector(0, 0, 100000.0), &GWalk.Nearby, GroundPt, &GroundN))
		{
			const double Stand = GroundPt.Z + Eye;
			const bool bWalkable = GroundN.Z >= 0.71;   // about 45 degrees, Unreal's limit
			// Snap down only when we were ALREADY on the floor and the ground has
			// dropped by less than a step - that is stairs and ramps. Applying it
			// while airborne is what cut jumps short: the arc got yanked back to
			// the ground the moment it started coming down.
			const bool bSnap = bWasOnFloor && GWalk.VelZ <= 0.0 && (Pos.Z - Stand) <= kStepUp;
			if (bWalkable && (Pos.Z <= Stand || bSnap))
			{
				Pos.Z = Stand;
				GWalk.VelZ = 0.0;
				GWalk.bGrounded = true;
			}
			else if (Pos.Z > Stand + 2.0) GWalk.bGrounded = false;
			else if (!bWalkable && Pos.Z <= Stand)
			{
				// too steep to stand on: sit on it but slide down the face
				Pos.Z = Stand;
				GWalk.bGrounded = false;
				const FVector Down = FVector(GroundN.X, GroundN.Y, 0).GetSafeNormal();
				Pos += Down * (200.0 * Dt);
				GWalk.VelZ = 0.0;
			}
		}

		VC->SetViewLocation(Pos);
		VC->Invalidate(false, false);
	}

	int32 SetOutlinerMode(bool bKeepTree)
	{
		BF6_SetKeepGodotTree(bKeepTree);
		GConfig->SetBool(TEXT("BF6UnrealSDK"), TEXT("TreeChoiceMade"), true, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
		return OrganizeOutliner();
	}

	int32 OrganizeOutliner()
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		// Keeping the tree means real attachment; filing by category means the
		// tree comes apart first, because an attached actor takes its place in
		// the outliner from its parent and would ignore any folder we set.
		if (BF6_KeepGodotTree()) return BF6_RebuildTreeFromTags();
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->GetAttachParentActor() && !TagValue(*It, TEXT("gtree:")).IsEmpty())
				It->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(kPlacedTag) || It->Tags.Contains(kBaseTag) || It->Tags.Contains(kGroupTag))
			{ BF6_FileActor(*It); n++; }
		return n;
	}

	// ---- fixed-camera tools: live preview + set-from-view ----
	// Selecting a camera object (DeployCam and friends) docks a live picture-
	// in-picture of what that camera sees; moving it with the gizmo or drag
	// updates the view in real time. Godot cameras look down their LOCAL -Z,
	// which our basis conversion maps to the actor's -Y, so the capture yaws
	// -90 about the actor's local up.
	static TWeakObjectPtr<AActor> GCamRig;
	static TWeakObjectPtr<USceneCaptureComponent2D> GCamCap;
	static TWeakObjectPtr<AActor> GCamTarget;
	static UTextureRenderTarget2D* GCamRT = nullptr;   // rooted, tiny, reused

	// ---- the camera an HQ or a flag carries ----
	// These have no camera NODE: HQ_PlayerSpawner and CapturePoint each declare a
	// dozen Camera* attributes (offset, look angle, FOV, look-at offset) and the
	// game builds the deploy/objective camera from them. So a creator sets numbers
	// and finds out what they framed only in game. Here those numbers are turned
	// into a real transform, which the existing preview rig can look through.
	// The type's own schema, not the actor's tags: an object only carries a p:
	// tag for a value the scene OVERRODE, so an HQ that kept its default camera
	// has none at all. GetActorProp does not fall back to the schema (whatever
	// its comment says), so the default is fetched here.
	static FString BF6_TypeOf(AActor* A)
	{
		if (!A) return FString();
		FString Ty = TagValue(A, TEXT("label:"));
		if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
		if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("mesh:"));
		return Ty;
	}

	static FString BF6_PropOrDefault(AActor* A, const TCHAR* Key)
	{
		const FString Live = GetActorProp(A, Key);
		if (!Live.IsEmpty()) return Live;
		for (const FPropDef& D : PropsForType(BF6_TypeOf(A)))
			if (D.Name == Key)
			{
				FString V = D.Default;
				V.ReplaceInline(TEXT("["), TEXT("")); V.ReplaceInline(TEXT("]"), TEXT(""));
				V.ReplaceInline(TEXT(" "), TEXT(""));
				return V;
			}
		return FString();
	}

	static bool BF6_HasAttributeCamera(AActor* A)
	{
		if (!A) return false;
		if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) return false;
		for (const FPropDef& D : PropsForType(BF6_TypeOf(A)))
			if (D.Name == TEXT("CameraOffsetPosition")) return true;
		return false;
	}

	// "x,y,z" in Godot metres -> Unreal centimetres, with the axis swap
	static FVector BF6_PropVec(AActor* A, const TCHAR* Key, const FVector& Fallback)
	{
		TArray<FString> P;
		BF6_PropOrDefault(A, Key).ParseIntoArray(P, TEXT(","));
		if (P.Num() != 3) return Fallback;
		const double gx = FCString::Atod(*P[0]), gy = FCString::Atod(*P[1]), gz = FCString::Atod(*P[2]);
		return FVector(gx, gz, gy) * 100.0;   // godot y is up
	}

	static double BF6_PropNum(AActor* A, const TCHAR* Key, double Fallback)
	{
		const FString V = BF6_PropOrDefault(A, Key);
		return V.IsNumeric() ? FCString::Atod(*V) : Fallback;
	}

	// Where that camera sits and what it looks at. CameraLookAtPosition means it
	// aims at the object (plus its look-at offset); otherwise CameraLookAngle is
	// the pitch and CameraRotationDirection the yaw it holds.
	bool AttributeCameraView(AActor* A, FVector& OutLoc, FRotator& OutRot, float& OutFov)
	{
		if (!BF6_HasAttributeCamera(A)) return false;
		const FVector Base = A->GetActorLocation();
		OutLoc = Base + BF6_PropVec(A, TEXT("CameraOffsetPosition"), FVector(1500, 1500, 4500));
		const FVector LookAt = Base + BF6_PropVec(A, TEXT("CameraLookAtPositionOffset"), FVector::ZeroVector);
		const FString Aim = BF6_PropOrDefault(A, TEXT("CameraLookAtPosition"));
		const bool bAimAtObject = Aim.IsEmpty() || Aim.Equals(TEXT("true"), ESearchCase::IgnoreCase);
		if (bAimAtObject && !(LookAt - OutLoc).IsNearlyZero())
			OutRot = (LookAt - OutLoc).Rotation();
		else
			OutRot = FRotator(BF6_PropNum(A, TEXT("CameraLookAngle"), -30.0),
				BF6_PropNum(A, TEXT("CameraRotationDirection"), 0.0), 0.0);
		OutFov = (float)BF6_PropNum(A, TEXT("CameraFOV"), 70.0);
		return true;
	}

	static bool BF6_IsCameraActor(AActor* A)
	{
		if (!A) return false;
		if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) return false;
		FString Ty = TagValue(A, TEXT("label:"));
		if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
		if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("mesh:"));
		return Ty.Contains(TEXT("Cam"));
	}

	// Which way a camera TYPE faces - PROVEN from the SDK's own exporter
	// (gdconverter _property_utils.py): a gameplay node's facing is its +Z
	// "front", the OPPOSITE of Godot's Camera3D -Z. FixedCamera is a plain
	// Node3D, so the game views along its +Z (= our actor's +Y); set-from-
	// view with the camera convention aimed it exactly backwards. DeployCam
	// is authored UNDER a real Camera3D pivot in every base level (aim = -Z,
	// straight down at the map), so it keeps the camera convention.
	static bool BF6_CamFacesPlusY(AActor* A)
	{
		FString Ty = TagValue(A, TEXT("label:"));
		if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
		if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("mesh:"));
		return !Ty.Contains(TEXT("DeployCam"));
	}

	static FQuat BF6_CamViewQuat(AActor* A)
	{
		return A->GetActorQuat() * FQuat(FVector::UpVector, BF6_CamFacesPlusY(A) ? HALF_PI : -HALF_PI);
	}

	// A REAL Unreal camera rides every camera object: selecting one gets the
	// editor's native frustum lines and camera preview, and right-click ->
	// "Pilot" flies the camera directly - the most natural way to aim it.
	// The component is transient + editor-only, aligned to the TYPE's true
	// view direction, so what Unreal shows is exactly what the game renders.
	// PUT UNREAL'S CAMERA WHERE OUR PREVIEW SAYS THE CAMERA IS.
	//
	// These were two different answers to the same question. The preview asks
	// AttributeCameraView, which offsets by CameraOffsetPosition and aims at
	// the object - the values the object actually ships with. The component was
	// attached at the actor ROOT with a relative rotation and nothing else, so
	// on an HQ it sat on the floor looking sideways while the preview beside it
	// showed the real shot. Same object, two cameras, and only one of them was
	// reading the properties.
	//
	// An object with no camera attributes (a plain Godot Camera3D) keeps the
	// old treatment: there is nothing to read, and its own transform IS the
	// camera.
	static void BF6_SyncCameraComponent(AActor* Cam)
	{
		if (!Cam) return;
		UCameraComponent* C = Cam->FindComponentByClass<UCameraComponent>();
		if (!C) return;
		FVector Loc; FRotator Rot; float Fov = 75.f;
		if (AttributeCameraView(Cam, Loc, Rot, Fov))
		{
			// World, not relative: the offset is authored in world terms and
			// the actor may be rotated.
			C->SetWorldLocationAndRotation(Loc, Rot);
			C->SetFieldOfView(FMath::Clamp(Fov, 5.f, 170.f));
			return;
		}
		C->SetRelativeLocation(FVector::ZeroVector);
		C->SetRelativeRotation(FRotator(0.f, BF6_CamFacesPlusY(Cam) ? 90.f : -90.f, 0.f));
		C->SetFieldOfView(75.f);   // Godot Camera3D default, same as the PIP
	}

	static void BF6_EnsureCameraComponent(AActor* Cam)
	{
		if (!Cam) return;
		if (!Cam->FindComponentByClass<UCameraComponent>())
		{
			UCameraComponent* C = NewObject<UCameraComponent>(Cam, TEXT("BF6CamView"));
			C->SetFlags(RF_Transient);
			C->bIsEditorOnly = true;
			C->SetupAttachment(Cam->GetRootComponent());
			C->RegisterComponent();
			C->SetConstraintAspectRatio(false);
		}
		// Every time, not just on creation: the offset and look angle are
		// attributes a creator edits, and a camera that only reads them once
		// drifts away from its own preview the moment they do.
		BF6_SyncCameraComponent(Cam);
	}

	AActor* CameraPreviewTarget() { return GCamTarget.Get(); }
	UTexture* CameraPreviewTexture() { return GCamRT; }

	void TickCameraPreview()
	{
		AActor* Cam = nullptr;
		if (GEditor)
			if (USelection* Sel = GEditor->GetSelectedActors())
				for (int32 i = 0; i < Sel->Num(); i++)
					if (AActor* A = Cast<AActor>(Sel->GetSelectedObject(i)))
						if (BF6_IsCameraActor(A) || BF6_HasAttributeCamera(A)) { Cam = A; break; }
		GCamTarget = Cam;
		if (Cam) BF6_EnsureCameraComponent(Cam);   // also re-syncs it from the attributes
		if (!Cam)
		{
			// nothing selected: the capture rig goes away (it costs GPU)
			if (AActor* R = GCamRig.Get())
				if (UWorld* W = R->GetWorld()) W->EditorDestroyActor(R, false);
			GCamRig.Reset();
			GCamCap.Reset();
			return;
		}
		if (!GCamRig.IsValid())
		{
			UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
			AActor* R = W->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (!R) return;
			R->SetFlags(RF_Transient);
			R->SetActorLabel(TEXT("CameraPreviewRig"));
			USceneCaptureComponent2D* C = NewObject<USceneCaptureComponent2D>(R, TEXT("Capture"));
			C->SetFlags(RF_Transient);
			R->SetRootComponent(C);
			C->RegisterComponent();
			if (!GCamRT)
			{
				GCamRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), TEXT("BF6CamPreviewRT"));
				GCamRT->AddToRoot();
				GCamRT->InitAutoFormat(480, 270);
			}
			C->TextureTarget = GCamRT;
			C->bCaptureEveryFrame = true;
			C->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
			C->FOVAngle = 75.f;   // Godot Camera3D default
			GCamRig = R;
			GCamCap = C;
		}
		// The preview must show the MAP, not our editing aids. A node marker sits
		// exactly where its node is - and a deploy camera hangs off a node - so the
		// cyan cube was parked in front of the lens, drawn in the foreground pass,
		// filling the shot. Markers and drag handles are hidden from the capture.
		if (USceneCaptureComponent2D* Cap = GCamCap.Get())
		{
			Cap->HiddenActors.Reset();
			if (UWorld* CW = GEditor->GetEditorWorldContext().World())
				for (TActorIterator<AActor> It(CW); It; ++It)
					if (It->Tags.Contains(kGroupTag) || It->Tags.Contains(kHandleTag))
						Cap->HiddenActors.Add(*It);
		}
		if (AActor* R = GCamRig.Get())
		{
			FVector L; FRotator Rt; float Fov = 75.f;
			if (AttributeCameraView(Cam, L, Rt, Fov))
			{
				R->SetActorLocationAndRotation(L, Rt.Quaternion());
				if (USceneCaptureComponent2D* C = GCamCap.Get()) C->FOVAngle = Fov;
			}
			else
			{
				R->SetActorLocationAndRotation(Cam->GetActorLocation(), BF6_CamViewQuat(Cam));
				if (USceneCaptureComponent2D* C = GCamCap.Get()) C->FOVAngle = 75.f;
			}
		}
	}

	void SetCameraFromView()
	{
		AActor* Cam = GCamTarget.Get();
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!Cam || !VC) return;
		FScopedTransaction Tx(FText::FromString(TEXT("Set Camera From View")));
		Cam->Modify();
		// An HQ or a flag has no camera to move: its camera IS the numbers. So the
		// editor's viewpoint is written back as the offset from the object, and the
		// aim is expressed the way the object already asks for it - a look-at
		// offset if it aims at itself, otherwise a pitch and a yaw.
		if (BF6_HasAttributeCamera(Cam))
		{
			const FVector Base = Cam->GetActorLocation();
			const FVector Off = VC->GetViewLocation() - Base;
			auto ToGodot = [](const FVector& V){ return FString::Printf(TEXT("%.3f,%.3f,%.3f"), V.X / 100.0, V.Z / 100.0, V.Y / 100.0); };
			SetActorProp(Cam, TEXT("CameraOffsetPosition"), ToGodot(Off));
			const FString Aim = BF6_PropOrDefault(Cam, TEXT("CameraLookAtPosition"));
			const bool bAimAtObject = Aim.IsEmpty() || Aim.Equals(TEXT("true"), ESearchCase::IgnoreCase);
			if (bAimAtObject)
			{
				// where the view is actually pointed, at the object's own distance
				const double Dist = FMath::Max(100.0, Off.Size());
				const FVector Aimed = VC->GetViewLocation() + VC->GetViewRotation().Vector() * Dist;
				SetActorProp(Cam, TEXT("CameraLookAtPositionOffset"), ToGodot(Aimed - Base));
			}
			else
			{
				const FRotator R = VC->GetViewRotation();
				SetActorProp(Cam, TEXT("CameraLookAngle"), FString::SanitizeFloat(R.Pitch));
				SetActorProp(Cam, TEXT("CameraRotationDirection"), FString::SanitizeFloat(R.Yaw));
			}
			Notify(TEXT("Camera offset and aim written from the view."));
			return;
		}
		// inverse of BF6_CamViewQuat: the actor takes the editor's view,
		// respecting the TYPE's facing convention (FixedCamera +Z, DeployCam -Z)
		const FQuat ViewQ = VC->GetViewRotation().Quaternion();
		Cam->SetActorLocationAndRotation(VC->GetViewLocation(),
			ViewQ * FQuat(FVector::UpVector, BF6_CamFacesPlusY(Cam) ? -HALF_PI : HALF_PI));
	}

	void LookThroughCamera()
	{
		AActor* Cam = GCamTarget.Get();
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!Cam || !VC) return;
		FVector L; FRotator Rt; float Fov;
		if (AttributeCameraView(Cam, L, Rt, Fov))
		{
			VC->SetViewLocation(L);
			VC->SetViewRotation(Rt);
			VC->ViewFOV = Fov;
			VC->Invalidate();
			return;
		}
		VC->SetViewLocation(Cam->GetActorLocation());
		VC->SetViewRotation(BF6_CamViewQuat(Cam).Rotator());
	}

	// ---- live scatter editor (the Proton Scatter feel) ----
	// Opened from the SCATTER pill: the scatter exists immediately and
	// REGENERATES as the sliders move. A fixed seed keeps the pattern stable
	// while dragging; New pattern re-rolls it. Apply rebuilds the exact
	// preview inside one transaction (single Ctrl+Z), Esc removes it all.
	// ONE ENTRY IN THE SCATTER POOL.
	//
	// A library source needs no template actor in the world: duplicating a
	// placed object already resolves down to a mesh name and a type and calls
	// SpawnSdkModel, so the pool can carry those two directly. Spawning hidden
	// templates to clone from would put objects in the level that the creator
	// never placed and that a mid-session save would capture.
	//
	// Size is the object's world bounds, learned from the first copy actually
	// spawned. It only feeds the minimum-spacing rule, so being unknown for one
	// regeneration costs a slightly tight first layout and nothing else.
	struct FBF6ScatterSource
	{
		FString Mesh;
		FString Type;
		FString Label;                        // what the panel lists it as
		FVector Size = FVector(100, 100, 100);
		bool    bSized = false;
	};

	struct FBF6ScatterLive
	{
		bool bActive = false;
		FBF6MultiUnit U;
		// THE POOL, when the scatter came from the object library rather than
		// from a selection. Empty means the old behaviour: scatter copies of
		// whatever was selected. Non-empty means every copy draws a source
		// from here, so one region can mix rocks, bushes and debris.
		TArray<FBF6ScatterSource> Pool;
		TArray<TWeakObjectPtr<AActor>> Preview;
		struct FTarget { FVector P; double Yaw = 0; double Tx = 0; double Ty = 0; double Scl = 1;
		                 int32 Pool = INDEX_NONE; };
		TArray<FTarget> Targets;              // the preview's exact recipe
		// per-copy variation limits: every scattered copy draws its OWN
		// random value inside these ranges
		int32 Count = 24; float RadiusM = 20.f;
		float RotDeg = 360.f;                 // random yaw within this arc
		float WobX = 0.f;                     // random lean on X, +/- half this arc
		float WobY = 0.f;                     // random lean on Y, +/- half this arc
		float ElevM = 0.f;                    // random up/down offset, +/- this many meters
		float Vary = 0.15f;                   // random size, +/- this fraction
		int32 Seed = 1;
		// region and grounding
		int32 Shape = 0;                      // 0 circle, 1 square, 2 ring, 3 drawn outline
		bool bFollowTerrain = true;           // off = copies keep the original's height
		// ONLY LAND ON THE GROUND. Without it a scatter dropped over a street
		// puts bushes on car roofs and window ledges, because the grounding
		// trace takes the first thing it hits. On means a copy whose trace
		// lands on anything but the map's terrain is skipped rather than
		// placed, so the count is a budget and not a promise.
		bool bTerrainOnly = false;
		// WHERE THIS SESSION CAME FROM. A library scatter shows the pool
		// section and asks for objects; a selection scatter never should, and
		// "the pool is empty" cannot tell them apart at the moment one starts.
		bool bFromLibrary = false;
		// Whether anybody has said WHERE yet. A selection scatter is centred on
		// its selection from the first frame; a library one is centred nowhere
		// until the creator clicks the ground, and laying it out at the world
		// origin in the meantime drops a ring of objects under the map.
		bool bHasCenter = false;
		bool bDrawing = false;                // clicking out a custom outline right now
		TArray<FVector> Poly;                 // the drawn outline (world, >= 3 points)
		// PAINT (shape 4). The brush accumulates rather than describing a
		// region, so the STROKES are the record: everything visible - the area
		// on the ground and the copies standing in it - is derived from them
		// and can be rebuilt from them. That is what makes a stroke undoable
		// and the area editable right up until Enter or Escape.
		struct FStroke
		{
			TArray<FVector> Stamps;
			float RadiusCm = 0.f;             // the brush width AT THE TIME
		};
		bool bPainting = false;               // left button down and travelling
		TArray<FStroke> Strokes;
		// WHAT HAS ALREADY BEEN FILLED, so a stroke in progress only has to
		// cover the ground it just gained. Rebuilding every copy ten times a
		// second is what made painting crawl: the cost is not the sampling, it
		// is destroying and respawning hundreds of procedural-mesh actors.
		// WHAT EACH PREVIEW ACTOR IS, parallel to Preview. Reuse has to hand a
		// target an actor of the SAME source: a bush standing in for a rock
		// would be quietly wrong in a way that survives all the way to Apply.
		TArray<int32> PreviewKind;
		TSet<int64> FilledCells;
		double      FillDensity = 0.0;        // copies per square cm, held during a stroke
	};
	static FBF6ScatterLive GScat;
	static int32 GScatSession = 0;   // bumps per session so the panel knows to reset
	static double GScatPaintDrawn = 0.0;   // last time the painted area was rebuilt
	// Ctrl edge preview on the drawn outline (mirrors the zone-point flow)
	static int32 GScatEdgeSeg = INDEX_NONE;
	static FVector GScatEdgeWorld = FVector::ZeroVector;
	static FVector2D GScatEdgePx = FVector2D(-10000.f, -10000.f);
	static bool GScatEdge = false;

	// The outline's own undo: its corners live outside the editor transaction
	// system, so Ctrl+Z is claimed per-session and replays these snapshots.
	struct FBF6PolySnap { TArray<FVector> Poly; int32 Shape = 0; };
	static TArray<FBF6PolySnap> GScatPolyUndo;
	static TArray<FBF6PolySnap> GScatPolyRedo;

	static void BF6_ScatterPolySnapshot()
	{
		FBF6PolySnap S; S.Poly = GScat.Poly; S.Shape = GScat.Shape;
		GScatPolyUndo.Add(MoveTemp(S));
		if (GScatPolyUndo.Num() > 64) GScatPolyUndo.RemoveAt(0);
		GScatPolyRedo.Reset();
	}

	bool IsScatterLive() { return GScat.bActive; }
	int32 ScatterSession() { return GScatSession; }

	static void BF6_ScatterDestroyPreview()
	{
		if (!GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		TSet<AGroupActor*> Roots;
		for (TWeakObjectPtr<AActor>& Wk : GScat.Preview)
			if (AActor* A = Wk.Get())
			{
				if (AGroupActor* G = AGroupActor::GetRootForActor(A)) Roots.Add(G);
				W->EditorDestroyActor(A, false);
			}
		for (AGroupActor* G : Roots) if (IsValid(G)) W->EditorDestroyActor(G, false);
		GScat.Preview.Reset();
		GScat.PreviewKind.Reset();   // parallel to Preview; they must never drift
	}

	// ---- the region preview: a translucent draped shape + corner dots ----
	// The chosen shape renders as a transient blue translucent mesh draped a
	// hair above the terrain, so the extents are visible while the sliders
	// move. Drawn outlines fill with hatch strips (which also handles concave
	// shapes exactly); their corners paint as draggable dots like zone points.
	static TWeakObjectPtr<AActor> GScatRegion;
	static TArray<FVector2D> GScatDotPx;      // projected corner dots (viewport px)
	static int32 GScatDotDrag = INDEX_NONE;

	static void BF6_ScatterRegionDestroy()
	{
		if (AActor* A = GScatRegion.Get())
			if (UWorld* W = A->GetWorld()) W->EditorDestroyActor(A, false);
		GScatRegion.Reset();
	}

	// RefZ is the height to search DOWN from, and it matters: sampling every
	// vertex of a drawn region from the seed unit's height starts the ray
	// underneath the terrain wherever the region sits higher than the seed,
	// and the first surface below it is then whatever lies under the map.
	// Every caller passes the height it already knows for that spot.
	static double BF6_ScatterGroundZ(double X, double Y, double RefZ, const TArray<AActor*>* Placed)
	{
		const FVector P(X, Y, RefZ);
		if (GScat.bFollowTerrain)
		{
			FVector Gnd;
			if (GroundRay(P + FVector(0, 0, 500), P - FVector(0, 0, 50000), Placed, Gnd)) return Gnd.Z;
			if (GroundRay(P + FVector(0, 0, 50000), P - FVector(0, 0, 50000), Placed, Gnd)) return Gnd.Z;
		}
		return RefZ;
	}

	// THE PAINTED AREA, as cells. One definition shared by the thing that DRAWS
	// the area and the thing that FILLS it, because those two disagreeing is
	// the bug that would be hardest to see: copies landing slightly outside the
	// region they are supposed to be inside, or a strip of region with nothing
	// in it.
	//
	// A grid rather than the raw discs. A stroke lays hundreds of overlapping
	// discs, and both consumers want the UNION: drawn as discs the overlaps
	// darken, and sampled as discs the density follows how slowly the creator
	// moved rather than the area they painted.
	static double BF6_PaintCoverage(TArray<FIntPoint>& OutCells)
	{
		OutCells.Reset();
		float MaxR = 0.f;
		for (const FBF6ScatterLive::FStroke& St : GScat.Strokes) MaxR = FMath::Max(MaxR, St.RadiusCm);
		if (MaxR <= 0.f) return 0.0;
		const double Cell = FMath::Clamp((double)MaxR / 5.0, 120.0, 900.0);

		TSet<int64> Seen;
		for (const FBF6ScatterLive::FStroke& St : GScat.Strokes)
		{
			const double Rr = FMath::Max((double)St.RadiusCm, 50.0);
			const int32 Span = FMath::Clamp((int32)FMath::CeilToInt(Rr / Cell), 1, 64);
			for (const FVector& Pt : St.Stamps)
			{
				const int32 cx = (int32)FMath::FloorToDouble(Pt.X / Cell);
				const int32 cy = (int32)FMath::FloorToDouble(Pt.Y / Cell);
				for (int32 dy = -Span; dy <= Span; dy++)
					for (int32 dx = -Span; dx <= Span; dx++)
					{
						// The cell's CENTRE against the brush, so the edge of
						// the area follows the circle rather than the grid it
						// was rasterised on.
						const double px = (cx + dx + 0.5) * Cell;
						const double py = (cy + dy + 0.5) * Cell;
						if (FMath::Square(px - Pt.X) + FMath::Square(py - Pt.Y) > Rr * Rr) continue;
						const int64 Key = ((int64)(cx + dx) << 32) ^ (int64)(uint32)(cy + dy);
						if (Seen.Contains(Key)) continue;
						Seen.Add(Key);
						OutCells.Add(FIntPoint(cx + dx, cy + dy));
					}
			}
		}
		return Cell;
	}

	static void BF6_ScatterRegionRebuild()
	{
		if (!GScat.bActive || !GEditor) { BF6_ScatterRegionDestroy(); return; }
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		FCollisionQueryParams QP(FName(TEXT("BF6ScatterRegion")), true);
		for (AActor* M : GScat.U.Members) if (IsValid(M)) QP.AddIgnoredActor(M);
		for (TWeakObjectPtr<AActor>& Wk : GScat.Preview) if (AActor* A = Wk.Get()) QP.AddIgnoredActor(A);

		const double Lift = 20.0;
		const double R = FMath::Max(GScat.RadiusM, 1.f) * 100.0;
		const FVector C = GScat.U.Center;
		// what the creator has already built inside this area, gathered once
		FBox2D Area(FVector2D(C.X - R, C.Y - R), FVector2D(C.X + R, C.Y + R));
		if (GScat.Shape == 3) { Area = FBox2D(ForceInit); for (const FVector& Q : GScat.Poly) Area += FVector2D(Q.X, Q.Y); }
		TArray<AActor*> Nearby;
		CollectPlacedIn(Area, Nearby);
		Nearby.RemoveAll([](AActor* A){ return GScat.U.Members.Contains(A); });
		TArray<FVector> V; TArray<int32> T; TArray<FLinearColor> VC;
		auto Vert = [&](double X, double Y, double RefZ)
		{
			V.Add(FVector(X, Y, BF6_ScatterGroundZ(X, Y, RefZ, &Nearby) + Lift));
			VC.Add(FLinearColor(0.15f, 0.45f, 1.f, 0.35f));
		};
		auto Quad = [&](int32 a, int32 b, int32 c2, int32 d)
		{
			T.Append({ a, b, c2, a, c2, d, a, c2, b, a, d, c2 });   // both windings
		};
		const bool bDrawn = GScat.Shape == 3 && GScat.Poly.Num() >= 3;

		// The painted area, from the one definition of it that the filler also
		// uses - see BF6_PaintCoverage.
		if (GScat.Shape == 4)
		{
			TArray<FIntPoint> Cells;
			const double Cell = BF6_PaintCoverage(Cells);
			for (const FIntPoint& Cp : Cells)
			{
				const double X0 = Cp.X * Cell, Y0 = Cp.Y * Cell;
				// A hair inside the cell, so neighbouring quads do not co-plane
				// along a shared edge and shimmer against each other.
				const double X1 = X0 + Cell * 0.96, Y1 = Y0 + Cell * 0.96;
				const int32 Base = V.Num();
				Vert(X0, Y0, C.Z); Vert(X1, Y0, C.Z); Vert(X1, Y1, C.Z); Vert(X0, Y1, C.Z);
				Quad(Base, Base + 1, Base + 2, Base + 3);
			}
		}
		else if (bDrawn)
		{
			// hatch strips: scanlines clipped to the outline, so concave
			// outlines fill correctly and the area reads as a hatch
			FBox2D BB(ForceInit);
			for (const FVector& P : GScat.Poly) BB += FVector2D(P.X, P.Y);
			const int32 RowsN = 24;
			const double Step = (BB.Max.Y - BB.Min.Y) / RowsN;
			if (Step > 1.0)
				for (int32 r = 0; r < RowsN; r++)
				{
					const double Y0 = BB.Min.Y + r * Step;
					const double Ym = Y0 + Step * 0.5;
					const double Y1 = Y0 + Step * 0.8;   // 20% gap = the hatch look
					// the outline was drawn ON the surface, so its own height is what
					// each vertex should search down from - carried across with X
					struct FCross { double X, Z; };
					TArray<FCross> Xs;
					for (int32 i = 0, j = GScat.Poly.Num() - 1; i < GScat.Poly.Num(); j = i++)
					{
						const FVector& A = GScat.Poly[i];
						const FVector& B = GScat.Poly[j];
						if ((A.Y > Ym) != (B.Y > Ym))
						{
							const double f = (Ym - A.Y) / (B.Y - A.Y);
							Xs.Add({ (B.X - A.X) * f + A.X, FMath::Lerp(A.Z, B.Z, f) });
						}
					}
					Xs.Sort([](const FCross& L, const FCross& R){ return L.X < R.X; });
					for (int32 s = 0; s + 1 < Xs.Num(); s += 2)
					{
						// subdivide long strips so they follow the terrain
						const double X0 = Xs[s].X, X1 = Xs[s + 1].X;
						const double Z0 = Xs[s].Z, Z1 = Xs[s + 1].Z;
						const int32 SubN = FMath::Clamp((int32)((X1 - X0) / 400.0), 1, 24);
						for (int32 k = 0; k < SubN; k++)
						{
							const double Xa = FMath::Lerp(X0, X1, (double)k / SubN);
							const double Xb = FMath::Lerp(X0, X1, (double)(k + 1) / SubN);
							const int32 Base = V.Num();
							const double Za = FMath::Lerp(Z0, Z1, (double)k / SubN);
							const double Zb = FMath::Lerp(Z0, Z1, (double)(k + 1) / SubN);
							Vert(Xa, Y0, Za); Vert(Xb, Y0, Zb); Vert(Xb, Y1, Zb); Vert(Xa, Y1, Za);
							Quad(Base, Base + 1, Base + 2, Base + 3);
						}
					}
				}
		}
		else if (GScat.Shape == 1)   // square: a terrain-following grid
		{
			const int32 N = 12;
			const int32 Base = V.Num();
			for (int32 y = 0; y <= N; y++)
				for (int32 x = 0; x <= N; x++)
					Vert(C.X + ((double)x / N * 2.0 - 1.0) * R, C.Y + ((double)y / N * 2.0 - 1.0) * R, C.Z);
			for (int32 y = 0; y < N; y++)
				for (int32 x = 0; x < N; x++)
				{
					const int32 a = Base + y * (N + 1) + x;
					Quad(a, a + 1, a + N + 2, a + N + 1);
				}
		}
		else   // circle / ring: a radial terrain-following grid
		{
			const int32 Seg = 48, Rad = 5;
			const double R0 = GScat.Shape == 2 ? 0.6 * R : 0.0;
			const int32 Base = V.Num();
			for (int32 ri = 0; ri <= Rad; ri++)
				for (int32 s = 0; s <= Seg; s++)
				{
					const double Rr = FMath::Lerp(R0, R, (double)ri / Rad);
					const double Th = (double)s / Seg * 2.0 * PI;
					Vert(C.X + FMath::Cos(Th) * Rr, C.Y + FMath::Sin(Th) * Rr, C.Z);
				}
			for (int32 ri = 0; ri < Rad; ri++)
				for (int32 s = 0; s < Seg; s++)
				{
					const int32 a = Base + ri * (Seg + 1) + s;
					Quad(a, a + 1, a + Seg + 2, a + Seg + 1);
				}
		}
		if (V.Num() == 0) return;
		// THE REGION ACTOR IS REUSED, not respawned.
		//
		// While a brush stroke is running this rebuilds ten times a second, and
		// spawning an actor plus registering a component that often is real
		// cost for no gain - the geometry changes, the actor never does. Only
		// the mesh section is replaced.
		AActor* A = GScatRegion.Get();
		UProceduralMeshComponent* M = nullptr;
		if (A)
		{
			M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
		}
		else
		{
			A = W->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (!A) return;
			M = MakeProcMesh(A, TEXT("ScatterRegion"));
			if (UMaterialInterface* Mat = BF6_Material(TEXT("M_Volume")))
				M->SetMaterial(0, Mat);
			M->bSelectable = false;   // clicks pass straight through to the map
			A->SetActorLabel(TEXT("ScatterRegion"));
			A->SetFlags(RF_Transient);
			GScatRegion = A;
		}
		if (!M) return;
		const TArray<FVector> NN; const TArray<FVector2D> UV; const TArray<FProcMeshTangent> NT;
		M->ClearAllMeshSections();
		M->CreateMeshSection_LinearColor(0, V, T, NN, UV, VC, NT, false);   // no collision: never blocks traces
	}

	static void BF6_ScatterRegenerate()
	{
		if (!GScat.bActive || !GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		// A SELECTION-BASED SCATTER DIES WITH ITS SOURCE; a pool one has no
		// source in the world to lose, so the same check would end the session
		// the moment it started.
		if (GScat.Pool.Num() == 0)
			for (AActor* M : GScat.U.Members)
				if (!IsValid(M)) { GScat.bActive = false; BF6_ScatterDestroyPreview(); BF6_ScatterRegionDestroy(); return; }
		// PAINTING IS A REGION, NOT A DEPOSIT. An earlier cut of this had the
		// brush place copies as it travelled and switched regeneration off, so
		// the sliders only reached the NEXT stamp. That is the wrong contract:
		// the whole reason the painted area is drawn on the ground is so the
		// sliders can re-lay what is inside it, live, the way they do for every
		// other shape. Different settings for a different patch means confirming
		// this one and starting another.
		//
		// It also means the terrain filter and New pattern work here for free,
		// because they are simply inputs to the same fill.
		if (GScat.Shape == 4 && GScat.Strokes.Num() == 0)
		{ BF6_ScatterDestroyPreview(); BF6_ScatterRegionDestroy(); GScat.Targets.Reset(); return; }

		// NOTHING TO LAY OUT YET is a normal state for a library scatter, not a
		// failure: the pool may still be empty, or the creator may not have
		// clicked a spot. Clearing and returning leaves the panel up and asking.
		if (GScat.bFromLibrary && GScat.Pool.Num() == 0)
		{ BF6_ScatterDestroyPreview(); BF6_ScatterRegionDestroy(); GScat.Targets.Reset(); return; }
		if (GScat.bFromLibrary && !GScat.bHasCenter && GScat.Shape != 4
			&& !(GScat.Shape == 3 && GScat.Poly.Num() >= 3))
		{ BF6_ScatterDestroyPreview(); BF6_ScatterRegionDestroy(); GScat.Targets.Reset(); return; }

		// THE PREVIEW SURVIVES SAMPLING in pool mode - the reconcile pass below
		// is what decides which of these actors are still wanted. Destroying
		// them here would throw away the very thing being reused.
		if (GScat.Pool.Num() == 0) BF6_ScatterDestroyPreview();
		GScat.Targets.Reset();
		// A FULL REFILL OWNS THE PAINTING AGAIN. Whatever the incremental pass
		// did during the stroke is gone, so the record of what it filled has to
		// go with it or the next stroke would skip ground that is now empty.
		GScat.FilledCells.Reset();
		if (GScat.Shape == 4)
		{
			TArray<FIntPoint> Cs;
			const double Cl = BF6_PaintCoverage(Cs);
			if (Cl > 0.0)
				for (const FIntPoint& Cp : Cs)
					GScat.FilledCells.Add(((int64)Cp.X << 32) ^ (int64)(uint32)Cp.Y);
		}
		// 1000 cap: the typed-value readout can stretch COUNT past the
		// slider's default 200 top, but preview churn respawns everything per
		// slider move, so a hard sanity ceiling stays
		const int32 Count = FMath::Clamp(GScat.Count, 1, 1000);
		const double R = FMath::Max(GScat.RadiusM, 1.f) * 100.0;
		const bool bDrawn = GScat.Shape == 3 && GScat.Poly.Num() >= 3;
		// the drawn outline's bounds and area (shoelace) drive its sampling
		FBox2D BB(ForceInit);
		double Area = PI * R * R;
		if (GScat.Shape == 1) Area = 4.0 * R * R;
		else if (GScat.Shape == 2) Area = PI * R * R * (1.0 - 0.36);
		else if (bDrawn)
		{
			double A2 = 0;
			for (int32 i = 0; i < GScat.Poly.Num(); i++)
			{
				const FVector& a = GScat.Poly[i];
				const FVector& b = GScat.Poly[(i + 1) % GScat.Poly.Num()];
				A2 += a.X * b.Y - b.X * a.Y;
				BB += FVector2D(a.X, a.Y);
			}
			Area = FMath::Max(FMath::Abs(A2) * 0.5, 10000.0);
		}
		// The painted area is however many cells it covers. Measuring the union
		// this way rather than summing the discs matters: a stroke that doubles
		// back over itself covers no more ground the second time.
		TArray<FIntPoint> PaintCells;
		double PaintCell = 0.0;
		if (GScat.Shape == 4)
		{
			PaintCell = BF6_PaintCoverage(PaintCells);
			Area = FMath::Max((double)PaintCells.Num() * PaintCell * PaintCell, 10000.0);
			BB = FBox2D(ForceInit);
			for (const FIntPoint& Cp : PaintCells)
			{
				BB += FVector2D(Cp.X * PaintCell, Cp.Y * PaintCell);
				BB += FVector2D((Cp.X + 1) * PaintCell, (Cp.Y + 1) * PaintCell);
			}
		}
		const double FitDist = FMath::Sqrt(Area / ((double)Count * 2.6));
		// HOW CLOSE TWO COPIES MAY LAND, from the widest thing being scattered.
		// With a pool that is the widest SOURCE, not the selection - which for
		// a library scatter does not exist at all, and read as zero would let
		// everything pile into the same spot.
		double UnitW = FMath::Max(GScat.U.Size.X, GScat.U.Size.Y);
		if (GScat.Pool.Num() > 0)
		{
			UnitW = 0.0;
			for (const FBF6ScatterSource& Src : GScat.Pool)
				UnitW = FMath::Max(UnitW, FMath::Max(Src.Size.X, Src.Size.Y));
		}
		const double MinDist = FMath::Clamp(FMath::Min(UnitW * 0.7, FitDist), 50.0, FMath::Max(R, 100.0));
		// nearest point on the drawn outline, which carries the height it was
		// drawn at. Strided: the outline is recorded while dragging so it is
		// dense, and 64 samples of it place a point as well as all of them.
		auto PolyRefZ = [&](double X, double Y)
		{
			double Best = TNumericLimits<double>::Max(), Z = GScat.U.Center.Z;
			const int32 Step = FMath::Max(1, GScat.Poly.Num() / 64);
			for (int32 i = 0; i < GScat.Poly.Num(); i += Step)
			{
				const FVector& A = GScat.Poly[i];
				const double dd = FMath::Square(A.X - X) + FMath::Square(A.Y - Y);
				if (dd < Best) { Best = dd; Z = A.Z; }
			}
			return Z;
		};
		FRandomStream Rng(GScat.Seed);
		FCollisionQueryParams QP(FName(TEXT("BF6ScatterLive")), true);
		for (AActor* M : GScat.U.Members) QP.AddIgnoredActor(M);
		TArray<FVector> Placed;
		// The centre is only occupied when something is already standing there:
		// a selection scatter keeps its original, a library one has none and
		// reserving the spot would leave a bare patch in the middle.
		if (GScat.Pool.Num() == 0) Placed.Add(GScat.U.Center);
		TArray<AActor*> NewOnes;
		// The map's scenery is always in play; this is what the creator has
		// built inside the area, so a scatter can land on a roof or a walkway
		// as readily as on the ground. Gathered once - sweeping the whole map
		// per sample would not be usable.
		FBox2D Area2 = bDrawn ? BB : FBox2D(
			FVector2D(GScat.U.Center.X - R, GScat.U.Center.Y - R),
			FVector2D(GScat.U.Center.X + R, GScat.U.Center.Y + R));
		TArray<AActor*> Nearby;
		CollectPlacedIn(Area2, Nearby);
		Nearby.RemoveAll([](AActor* A){ return GScat.U.Members.Contains(A); });
		for (TWeakObjectPtr<AActor>& Wk : GScat.Preview) Nearby.Remove(Wk.Get());
		int32 n = 0;
		for (int32 s = 0; s < Count * 40 && n < Count; s++)
		{
			// the same draws happen for every sample so the pattern stays
			// stable while count/radius/shape move
			const double u = Rng.FRand();
			const double v = Rng.FRand();
			const double Yaw = (Rng.FRand() - 0.5) * GScat.RotDeg;
			const double Tx = (Rng.FRand() - 0.5) * GScat.WobX;   // wobble lean
			const double Ty = (Rng.FRand() - 0.5) * GScat.WobY;
			const double Scl = 1.0 + (Rng.FRand() * 2.0 - 1.0) * GScat.Vary;
			const double Elev = (Rng.FRand() * 2.0 - 1.0) * GScat.ElevM * 100.0;
			FVector P = GScat.U.Center;
			if (GScat.Shape == 4)
			{
				// UNIFORM OVER THE PAINTED AREA. Sampling a random STAMP and
				// then a point in its disc would be easier and wrong: a stroke
				// lays stamps wherever the cursor slowed down, so density would
				// follow how fast the creator moved rather than what they
				// painted, and doubling back would double the objects.
				if (PaintCells.Num() == 0) break;
				const FIntPoint& Cp = PaintCells[Rng.RandRange(0, PaintCells.Num() - 1)];
				P.X = (Cp.X + Rng.FRand()) * PaintCell;
				P.Y = (Cp.Y + Rng.FRand()) * PaintCell;
				// The stroke was painted ON the surface, so its own height is
				// the sensible place to start the ground ray from - the same
				// reason the drawn outline carries its height across.
				double Best = TNumericLimits<double>::Max();
				for (const FBF6ScatterLive::FStroke& St : GScat.Strokes)
				{
					const int32 Step = FMath::Max(1, St.Stamps.Num() / 32);
					for (int32 si = 0; si < St.Stamps.Num(); si += Step)
					{
						const double dd = FMath::Square(St.Stamps[si].X - P.X)
						                + FMath::Square(St.Stamps[si].Y - P.Y);
						if (dd < Best) { Best = dd; P.Z = St.Stamps[si].Z; }
					}
				}
			}
			else if (bDrawn)
			{
				P.X = FMath::Lerp((double)BB.Min.X, (double)BB.Max.X, u);
				P.Y = FMath::Lerp((double)BB.Min.Y, (double)BB.Max.Y, v);
				// point-in-polygon (XY crossing test) against the drawn outline
				bool bIn = false;
				for (int32 i = 0, j = GScat.Poly.Num() - 1; i < GScat.Poly.Num(); j = i++)
				{
					const FVector& A = GScat.Poly[i];
					const FVector& B = GScat.Poly[j];
					if ((A.Y > P.Y) != (B.Y > P.Y)
						&& P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X)
						bIn = !bIn;
				}
				if (!bIn) continue;
				// Only X and Y come from the outline; without this the height is
				// still the seed unit's, so a region drawn on higher ground starts
				// its ground ray underneath the terrain and lands on whatever is
				// below the map. The outline knows the height it was drawn at.
				P.Z = PolyRefZ(P.X, P.Y);
			}
			else if (GScat.Shape == 1)
			{
				P += FVector((u * 2.0 - 1.0) * R, (v * 2.0 - 1.0) * R, 0.0);
			}
			else
			{
				const double Rr = (GScat.Shape == 2)
					? R * FMath::Sqrt(FMath::Lerp(0.36, 1.0, u))   // ring: hollow centre
					: R * FMath::Sqrt(u);
				const double Th = v * 2.0 * PI;
				P += FVector(FMath::Cos(Th) * Rr, FMath::Sin(Th) * Rr, 0.0);
			}
			bool bClose = false;
			for (const FVector& Q : Placed)
				if (FVector::DistSquaredXY(P, Q) < MinDist * MinDist) { bClose = true; break; }
			if (bClose) continue;
			if (GScat.bTerrainOnly)
			{
				// A MISS IS A SKIP, not a fallback. The point of the filter is
				// that this copy would have landed on something that is not the
				// ground, and dropping it back to the ordinary trace would put
				// it on exactly the roof the creator was trying to avoid.
				FVector Gnd;
				if (!BF6_TerrainRay(P + FVector(0, 0, 5000), P - FVector(0, 0, 50000), Gnd)) continue;
				P.Z = Gnd.Z;
			}
			else if (GScat.bFollowTerrain)
			{
				FVector Gnd;
				if (GroundRay(P + FVector(0, 0, 500), P - FVector(0, 0, 50000), &Nearby, Gnd))
					P.Z = Gnd.Z;
				else if (GroundRay(P + FVector(0, 0, 50000), P - FVector(0, 0, 50000), &Nearby, Gnd))
					P.Z = Gnd.Z;
			}
			else P.Z = GScat.U.Center.Z;
			P.Z += Elev;

			int32 Made = 0;
			int32 PoolPick = INDEX_NONE;
			if (GScat.Pool.Num() > 0)
			{
				// WHICH SOURCE THIS COPY IS, drawn from the same stream as its
				// rotation and size so a given seed always lays out the same
				// mix - re-rolling the pattern re-rolls which object goes
				// where, and nudging a slider does not.
				// NOTHING IS SPAWNED HERE. The layout is decided first and
				// built afterwards, so the pass below can hand a target an
				// actor that already exists instead of making a new one. Moving
				// a static mesh is nearly free; creating one is not, and a
				// slider drag was paying for a few hundred of them eight times
				// a second when the only thing that changed was where they sit.
				PoolPick = Rng.RandRange(0, GScat.Pool.Num() - 1);
				Made = 1;
			}
			else Made = BF6_SpawnUnitCopy(GScat.U, P, Yaw, Scl, QP, NewOnes, Tx, Ty);

			if (Made > 0)
			{
				Placed.Add(P);
				FBF6ScatterLive::FTarget T; T.P = P; T.Yaw = Yaw; T.Tx = Tx; T.Ty = Ty; T.Scl = Scl;
				T.Pool = PoolPick;
				GScat.Targets.Add(T);
				n++;
			}
		}
		if (GScat.Pool.Num() > 0)
		{
			// ---- RECONCILE: move what exists, make only the difference ----
			//
			// The old pass destroyed every copy and spawned the whole set again
			// on any change at all. For ROTATION, SIZE, WOBBLE, ELEVATION and
			// New pattern nothing about WHICH objects exist changes - only
			// where they sit - so all of that work bought nothing. This is the
			// same insight as the fast delete: do not pay to recreate something
			// you already have.
			//
			// Buckets by source, because a target must be handed an actor of
			// its own kind. Anything left over at the end is genuinely surplus.
			TMap<int32, TArray<AActor*>> Spare;
			for (int32 i = 0; i < GScat.Preview.Num(); i++)
				if (AActor* Old = GScat.Preview[i].Get())
					Spare.FindOrAdd(GScat.PreviewKind.IsValidIndex(i) ? GScat.PreviewKind[i] : INDEX_NONE).Add(Old);

			TArray<TWeakObjectPtr<AActor>> Keep;
			TArray<int32> KeepKind;
			Keep.Reserve(GScat.Targets.Num());
			KeepKind.Reserve(GScat.Targets.Num());
			int32 Reused = 0, Spawned = 0;

			for (int32 ti = 0; ti < GScat.Targets.Num(); ti++)
			{
				const FBF6ScatterLive::FTarget& T = GScat.Targets[ti];
				if (!GScat.Pool.IsValidIndex(T.Pool)) continue;
				FBF6ScatterSource& Src = GScat.Pool[T.Pool];
				const FQuat RotQ = FQuat(FVector::UpVector, FMath::DegreesToRadians(T.Yaw))
					* FQuat(FVector::ForwardVector, FMath::DegreesToRadians(T.Tx))
					* FQuat(FVector::RightVector, FMath::DegreesToRadians(T.Ty));
				const FTransform Xf(RotQ, T.P, FVector(T.Scl));

				AActor* A = nullptr;
				if (TArray<AActor*>* Bucket = Spare.Find(T.Pool))
					if (Bucket->Num() > 0) { A = Bucket->Pop(EAllowShrinking::No); A->SetActorTransform(Xf); Reused++; }
				if (!A)
				{
					A = SpawnSdkModel(Src.Mesh, Src.Type, Xf);
					if (!A) continue;
					Spawned++;
				}
				// The bounds are only knowable once something exists, and they
				// feed the spacing rule for the NEXT regeneration.
				if (!Src.bSized)
				{
					FVector Org, Ext;
					A->GetActorBounds(false, Org, Ext);
					if (!Ext.IsNearlyZero()) { Src.Size = Ext * 2.0; Src.bSized = true; }
				}
				Keep.Add(A);
				KeepKind.Add(T.Pool);
			}

			// Whatever no target claimed. Sections emptied first, for the same
			// reason the fast delete does it: a procedural mesh serialises its
			// whole vertex payload if it goes into a transaction.
			for (const TPair<int32, TArray<AActor*>>& Kv : Spare)
				for (AActor* Old : Kv.Value)
					if (IsValid(Old))
					{
						if (UProceduralMeshComponent* Pm = Cast<UProceduralMeshComponent>(Old->GetRootComponent()))
							Pm->ClearAllMeshSections();
						W->EditorDestroyActor(Old, false);
					}

			GScat.Preview = MoveTemp(Keep);
			GScat.PreviewKind = MoveTemp(KeepKind);
		}
		else
		{
			for (AActor* A : NewOnes) GScat.Preview.Add(A);
			GScat.PreviewKind.Init(INDEX_NONE, GScat.Preview.Num());
		}
		BF6_ScatterRegionRebuild();
		BF6_RecomputeBudget();
	}

	// START A SCATTER WITH NOTHING SELECTED.
	//
	// The old flow was select-then-scatter, which reads backwards for the thing
	// people actually want: a bed of mixed vegetation. That starts from "what
	// shall I strew", not from an object already standing in the level, and it
	// meant placing a bush by hand first purely to have something to scatter.
	//
	// A session opened this way starts with an empty pool. It is live
	// immediately so the panel can dock and ask for objects.
	bool BeginScatterFromLibrary()
	{
		if (GScat.bActive) return true;
		GScat = FBF6ScatterLive();
		GScat.bActive = true;
		GScat.bTerrainOnly = true;   // the common case for a library scatter
		GScat.bFromLibrary = true;
		GScatSession++;
		GScatPolyUndo.Reset();
		GScatPolyRedo.Reset();
		// No regenerate yet: with no sources there is nothing to lay out, and
		// the panel says so rather than showing an empty region.
		return true;
	}

	int32 ScatterPoolCount() { return GScat.bActive ? GScat.Pool.Num() : 0; }

	bool IsScatterFromLibrary() { return GScat.bActive && GScat.bFromLibrary; }

	FString ScatterPoolName(int32 i)
	{
		return (GScat.bActive && GScat.Pool.IsValidIndex(i)) ? GScat.Pool[i].Label : FString();
	}

	void AddScatterObject(const FString& Mesh, const FString& Type, const FString& Label)
	{
		if (!GScat.bActive || Mesh.IsEmpty()) return;
		for (const FBF6ScatterSource& S : GScat.Pool)
			if (S.Mesh == Mesh && S.Type == Type) return;   // already in the mix
		FBF6ScatterSource Src;
		Src.Mesh = Mesh;
		Src.Type = Type;
		Src.Label = Label.IsEmpty() ? (Type.IsEmpty() ? Mesh : Type) : Label;
		GScat.Pool.Add(Src);
		BF6_ScatterRegenerate();
	}

	void RemoveScatterObject(int32 i)
	{
		if (!GScat.bActive || !GScat.Pool.IsValidIndex(i)) return;
		GScat.Pool.RemoveAt(i);
		BF6_ScatterRegenerate();
	}

	bool GetScatterTerrainOnly() { return GScat.bTerrainOnly; }

	void SetScatterTerrainOnly(bool bOn)
	{
		if (!GScat.bActive || GScat.bTerrainOnly == bOn) return;
		GScat.bTerrainOnly = bOn;
		BF6_ScatterRegenerate();
	}

	// Where a click while a library scatter is live should drop the region.
	// Selection-based scatter centres on the selection; this one has none, so
	// the creator says where by clicking.
	void SetScatterCenter(const FVector& W)
	{
		if (!GScat.bActive) return;
		GScat.U.Center = W;
		GScat.U.Size = FVector(100, 100, 100);
		GScat.bHasCenter = true;
		BF6_ScatterRegenerate();
	}

	bool BeginScatterLive()
	{
		if (GScat.bActive) return true;
		FBF6MultiUnit U;
		if (!BF6_GatherMultiUnit(U)) { Notify(TEXT("Select an object, a group, or a block first.")); return false; }
		GScat = FBF6ScatterLive();
		GScat.U = U;
		GScat.bActive = true;
		GScatSession++;
		GScatPolyUndo.Reset();
		GScatPolyRedo.Reset();
		BF6_ScatterRegenerate();
		return true;
	}

	void UpdateScatterLive(int32 Count, float RadiusM, float RotDeg, float WobX, float WobY, float ElevM, float Vary, int32 Seed)
	{
		if (!GScat.bActive) return;
		if (GScat.Count == Count && GScat.RadiusM == RadiusM && GScat.RotDeg == RotDeg
			&& GScat.WobX == WobX && GScat.WobY == WobY
			&& GScat.ElevM == ElevM && GScat.Vary == Vary && GScat.Seed == Seed) return;
		GScat.Count = Count; GScat.RadiusM = RadiusM; GScat.RotDeg = RotDeg;
		GScat.WobX = WobX; GScat.WobY = WobY;
		GScat.ElevM = ElevM; GScat.Vary = Vary; GScat.Seed = Seed;
		BF6_ScatterRegenerate();
	}

	void SetScatterShape(int32 Shape)
	{
		if (!GScat.bActive || GScat.Shape == Shape) return;
		const int32 Was = GScat.Shape;
		GScat.Shape = Shape;
		if (Shape != 3) GScat.bDrawing = false;
		if (Shape == 4)
		{
			GScat.Strokes.Reset();
			GScat.FilledCells.Reset();
			// A BLANK CANVAS. Carrying the circle's layout into the brush would
			// mean the first stroke starts on top of a scatter nobody asked
			// for, and there is no way to tell the two apart afterwards.
			BF6_ScatterDestroyPreview();
			BF6_ScatterRegionDestroy();
			GScat.Targets.Reset();
			GScat.bPainting = false;
			BF6_RecomputeBudget();
			return;
		}
		if (Was == 4) { GScat.Strokes.Reset(); GScat.FilledCells.Reset(); GScat.bPainting = false; }
		BF6_ScatterRegenerate();
	}
	int32 GetScatterShape() { return GScat.Shape; }

	void SetScatterFollowTerrain(bool b)
	{
		if (!GScat.bActive || GScat.bFollowTerrain == b) return;
		GScat.bFollowTerrain = b;
		BF6_ScatterRegenerate();
	}
	bool GetScatterFollowTerrain() { return GScat.bFollowTerrain; }

	// custom outline: the user clicks corners on the map; from the third
	// corner on, the fill regenerates live after every click
	void BeginScatterDraw()
	{
		if (!GScat.bActive) return;
		BF6_ScatterPolySnapshot();
		GScat.bDrawing = true;
		GScat.Poly.Reset();
		GScat.Shape = 3;
		// clean slate while outlining: the old shape's copies and region go
		// away, the fill comes back live from the third corner
		BF6_ScatterDestroyPreview();
		BF6_ScatterRegionDestroy();
		GScat.Targets.Reset();
		BF6_RecomputeBudget();
	}
	bool IsScatterDrawing() { return GScat.bActive && GScat.bDrawing; }
	int32 ScatterDrawPointCount() { return GScat.Poly.Num(); }
	void ScatterDrawAddPoint(const FVector& W)
	{
		if (!IsScatterDrawing()) return;
		BF6_ScatterPolySnapshot();
		GScat.Poly.Add(W);
		if (GScat.Poly.Num() >= 3) BF6_ScatterRegenerate();
	}
	void FinishScatterDraw()
	{
		if (!IsScatterDrawing()) return;
		GScat.bDrawing = false;
		if (GScat.Poly.Num() < 3)
		{
			// not enough corners for an outline - fall back to the circle
			GScat.Shape = 0;
			Notify(TEXT("An outline needs at least 3 corners - back to the circle."));
		}
		BF6_ScatterRegenerate();
	}
	void CancelScatterDraw()
	{
		if (!IsScatterDrawing()) return;
		BF6_ScatterPolySnapshot();
		GScat.bDrawing = false;
		GScat.Poly.Reset();
		GScat.Shape = 0;
		BF6_ScatterRegenerate();
	}

	// the outline's own undo / redo (Ctrl+Z / Ctrl+Y while the scatter is
	// live): replays the corner snapshots and refills the shape
	bool ScatterOutlineUndo()
	{
		if (!GScat.bActive || GScatPolyUndo.Num() == 0) return false;
		FBF6PolySnap Now; Now.Poly = GScat.Poly; Now.Shape = GScat.Shape;
		GScatPolyRedo.Add(MoveTemp(Now));
		const FBF6PolySnap S = GScatPolyUndo.Pop();
		GScat.Poly = S.Poly;
		GScat.Shape = S.Shape;
		GScatDotDrag = INDEX_NONE;
		BF6_ScatterRegenerate();
		return true;
	}

	bool ScatterOutlineRedo()
	{
		if (!GScat.bActive || GScatPolyRedo.Num() == 0) return false;
		FBF6PolySnap Now; Now.Poly = GScat.Poly; Now.Shape = GScat.Shape;
		GScatPolyUndo.Add(MoveTemp(Now));
		const FBF6PolySnap S = GScatPolyRedo.Pop();
		GScat.Poly = S.Poly;
		GScat.Shape = S.Shape;
		GScatDotDrag = INDEX_NONE;
		BF6_ScatterRegenerate();
		return true;
	}

	// ---- corner dots: projected each tick, painted by the dot layer, and
	// draggable like zone points (the region follows live; the copies refill
	// on release) ----
	void TickScatter()
	{
		GScatDotPx.Reset();
		GScatEdge = false;
		GScatEdgeSeg = INDEX_NONE;
		if (!GScat.bActive || GScat.Poly.Num() == 0) return;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return;
		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(VC->Viewport, VC->GetScene(), VC->EngineShowFlags));
		FSceneView* View = VC->CalcSceneView(&Family);
		if (!View) return;
		auto Project = [&](const FVector& W) -> FVector2D
		{
			FVector2D Px(-10000.f, -10000.f);
			const FVector4 Clip = View->WorldToScreen(W + FVector(0, 0, 30));
			if (Clip.W > 0.f) View->ScreenToPixel(Clip, Px);
			return Px;
		};
		for (const FVector& Pw : GScat.Poly) GScatDotPx.Add(Project(Pw));

		// Ctrl edge preview, like zone points: the nearest outline segment to
		// the mouse gets an insert dot (only when reasonably close, so
		// Ctrl+click elsewhere keeps its native meaning)
		if (GScat.Poly.Num() >= 3 && GScatDotDrag == INDEX_NONE
			&& FSlateApplication::Get().GetModifierKeys().IsControlDown())
		{
			const FVector2D M((float)VC->Viewport->GetMouseX(), (float)VC->Viewport->GetMouseY());
			const int32 N = GScat.Poly.Num();
			float BestD = 40.f * VC->GetDPIScale();
			for (int32 i = 0; i < N; i++)
			{
				const FVector2D A = GScatDotPx[i], B = GScatDotPx[(i + 1) % N];
				if (A.X < -999.f || B.X < -999.f) continue;
				const FVector2D AB = B - A;
				const float Len2 = (float)AB.SizeSquared();
				const float T = Len2 > 1.f ? FMath::Clamp((float)FVector2D::DotProduct(M - A, AB) / Len2, 0.f, 1.f) : 0.f;
				const float D = (float)FVector2D::Distance(M, A + AB * T);
				if (D < BestD)
				{
					BestD = D;
					GScatEdgeSeg = i;
					GScatEdgeWorld = FMath::Lerp(GScat.Poly[i], GScat.Poly[(i + 1) % N], (double)T);
				}
			}
			if (GScatEdgeSeg != INDEX_NONE)
			{
				GScatEdgePx = Project(GScatEdgeWorld);
				GScatEdge = GScatEdgePx.X > -999.f;
			}
		}
	}

	bool GetScatterEdgePreview(FVector2D& OutPx)
	{
		OutPx = GScatEdgePx;
		return GScat.bActive && GScatEdge;
	}

	// Ctrl+LMB: insert a corner exactly where the edge preview shows
	void ScatterAddPointAtPreview()
	{
		if (!GScat.bActive || !GScatEdge || GScatEdgeSeg == INDEX_NONE) return;
		BF6_ScatterPolySnapshot();
		GScat.Poly.Insert(GScatEdgeWorld, GScatEdgeSeg + 1);
		GScatEdge = false;
		GScatEdgeSeg = INDEX_NONE;
		BF6_ScatterRegenerate();
	}

	// Del / Ctrl+RMB on a corner dot: remove that corner (min 3 stay)
	void ScatterDeletePointByIndex(int32 Index)
	{
		if (!GScat.bActive || !GScat.Poly.IsValidIndex(Index)) return;
		if (GScat.Poly.Num() <= 3) { Notify(TEXT("An outline needs at least 3 corners.")); return; }
		BF6_ScatterPolySnapshot();
		GScat.Poly.RemoveAt(Index);
		if (GScatDotDrag == Index) GScatDotDrag = INDEX_NONE;
		BF6_ScatterRegenerate();
	}

	// ---- PAINT (shape 4) --------------------------------------------------
	//
	// The brush paints an AREA, and the area is filled exactly like any other
	// shape. That is the whole design, and an earlier cut of it got this wrong:
	// it had the brush place copies as it travelled and switched regeneration
	// off, which meant the sliders only ever reached the next stamp. The reason
	// to draw the painted area on the ground is so the sliders can re-lay what
	// is inside it, live. Wanting different settings for a different patch
	// means confirming this one and starting another.
	//
	// So the STROKES are the only state, and everything else - the region mesh,
	// the copies, the area used to space them - is derived from them. The
	// terrain filter and New pattern need no special handling here at all: they
	// are inputs to the same fill.
	// Fill only the cells the stroke just gained, at the density the whole
	// painting is currently carrying.
	//
	// Density rather than a total, because a total is not knowable mid-stroke:
	// the area is still growing. It is recomputed from Count over the area so
	// far on every call, so it tracks the slider and self-corrects as the
	// stroke runs; the authoritative refill on mouse-up settles it exactly.
	static void BF6_ScatterPaintFillNew()
	{
		if (!GScat.bActive || GScat.Shape != 4 || !GEditor) return;
		if (GScat.Pool.Num() == 0 && GScat.U.Members.Num() == 0) return;

		TArray<FIntPoint> Cells;
		const double Cell = BF6_PaintCoverage(Cells);
		if (Cell <= 0.0 || Cells.Num() == 0) return;

		TArray<FIntPoint> Fresh;
		for (const FIntPoint& Cp : Cells)
		{
			const int64 Key = ((int64)Cp.X << 32) ^ (int64)(uint32)Cp.Y;
			if (GScat.FilledCells.Contains(Key)) continue;
			GScat.FilledCells.Add(Key);
			Fresh.Add(Cp);
		}
		if (Fresh.Num() == 0) return;

		const double CellArea = Cell * Cell;
		const double TotalArea = FMath::Max((double)Cells.Num() * CellArea, 1.0);
		const int32  Count = FMath::Clamp(GScat.Count, 1, 1000);
		const double Density = (double)Count / TotalArea;
		int32 Want = FMath::RoundToInt(Density * (double)Fresh.Num() * CellArea);
		if (Want <= 0) return;
		Want = FMath::Min(Want, 400);   // one stamp can never be a whole map

		double UnitW = FMath::Max(GScat.U.Size.X, GScat.U.Size.Y);
		if (GScat.Pool.Num() > 0)
		{
			UnitW = 0.0;
			for (const FBF6ScatterSource& Src : GScat.Pool)
				UnitW = FMath::Max(UnitW, FMath::Max(Src.Size.X, Src.Size.Y));
		}
		const double FitDist = FMath::Sqrt(1.0 / FMath::Max(Density, 1e-12) / 2.6);
		const double MinDist = FMath::Clamp(FMath::Min(UnitW * 0.7, FitDist), 40.0, 4000.0);

		// Only what is near the fresh ground can conflict with it. Bounded by
		// the fresh cells rather than the whole painting, which is the other
		// half of why this stays cheap as a stroke gets long.
		FBox2D Fresh2(ForceInit);
		for (const FIntPoint& Cp : Fresh)
		{
			Fresh2 += FVector2D(Cp.X * Cell, Cp.Y * Cell);
			Fresh2 += FVector2D((Cp.X + 1) * Cell, (Cp.Y + 1) * Cell);
		}
		Fresh2 = Fresh2.ExpandBy(MinDist * 2.0);
		TArray<FVector> Near;
		for (const FBF6ScatterLive::FTarget& T : GScat.Targets)
			if (Fresh2.IsInside(FVector2D(T.P.X, T.P.Y))) Near.Add(T.P);

		FCollisionQueryParams QP(FName(TEXT("BF6ScatterPaint")), true);
		for (AActor* M : GScat.U.Members) QP.AddIgnoredActor(M);

		// Seeded from how much is already down, so the stroke keeps producing
		// fresh arrangements as it travels instead of repeating one.
		FRandomStream Rng(GScat.Seed * 7919 + GScat.Targets.Num() * 104729 + Fresh.Num());

		TArray<AActor*> NewOnes;
		int32 n = 0;
		for (int32 s = 0; s < Want * 20 && n < Want; s++)
		{
			const FIntPoint& Cp = Fresh[Rng.RandRange(0, Fresh.Num() - 1)];
			FVector P((Cp.X + Rng.FRand()) * Cell, (Cp.Y + Rng.FRand()) * Cell, 0.0);

			bool bClose = false;
			for (const FVector& Q : Near)
				if (FVector::DistSquaredXY(P, Q) < MinDist * MinDist) { bClose = true; break; }
			if (bClose) continue;

			if (GScat.bTerrainOnly)
			{
				FVector Gnd;
				if (!BF6_TerrainRay(P + FVector(0, 0, 50000), P - FVector(0, 0, 50000), Gnd)) continue;
				P.Z = Gnd.Z;
			}
			else
			{
				FVector Gnd;
				if (!GroundRay(P + FVector(0, 0, 50000), P - FVector(0, 0, 50000), nullptr, Gnd)) continue;
				P.Z = Gnd.Z;
			}

			const double Yaw = (Rng.FRand() - 0.5) * GScat.RotDeg;
			const double Tx  = (Rng.FRand() - 0.5) * GScat.WobX;
			const double Ty  = (Rng.FRand() - 0.5) * GScat.WobY;
			const double Scl = 1.0 + (Rng.FRand() * 2.0 - 1.0) * GScat.Vary;
			P.Z += (Rng.FRand() * 2.0 - 1.0) * GScat.ElevM * 100.0;

			int32 Made = 0;
			int32 PoolPick = INDEX_NONE;
			if (GScat.Pool.Num() > 0)
			{
				PoolPick = Rng.RandRange(0, GScat.Pool.Num() - 1);
				FBF6ScatterSource& Src = GScat.Pool[PoolPick];
				const FQuat RotQ = FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw))
					* FQuat(FVector::ForwardVector, FMath::DegreesToRadians(Tx))
					* FQuat(FVector::RightVector, FMath::DegreesToRadians(Ty));
				if (AActor* A = SpawnSdkModel(Src.Mesh, Src.Type, FTransform(RotQ, P, FVector(Scl))))
				{
					NewOnes.Add(A);
					QP.AddIgnoredActor(A);
					Made = 1;
					if (!Src.bSized)
					{
						FVector Org, Ext;
						A->GetActorBounds(false, Org, Ext);
						if (!Ext.IsNearlyZero()) { Src.Size = Ext * 2.0; Src.bSized = true; }
					}
				}
			}
			else Made = BF6_SpawnUnitCopy(GScat.U, P, Yaw, Scl, QP, NewOnes, Tx, Ty);

			if (Made > 0)
			{
				Near.Add(P);
				FBF6ScatterLive::FTarget T;
				T.P = P; T.Yaw = Yaw; T.Tx = Tx; T.Ty = Ty; T.Scl = Scl; T.Pool = PoolPick;
				GScat.Targets.Add(T);
				n++;
			}
		}
		// Kinds alongside, or the reconcile pass would mistake these for
		// unknown-source actors and destroy every one of them on the next fill.
		for (int32 k = 0; k < NewOnes.Num(); k++)
		{
			GScat.Preview.Add(NewOnes[k]);
			const int32 Ti = GScat.Targets.Num() - NewOnes.Num() + k;
			GScat.PreviewKind.Add(GScat.Targets.IsValidIndex(Ti) ? GScat.Targets[Ti].Pool : INDEX_NONE);
		}
		if (n > 0) BF6_RecomputeBudget();
	}

	void BeginScatterPaint()
	{
		if (!GScat.bActive) return;
		GScat.bPainting = true;
		// A NEW STROKE PER PRESS, which is the unit Ctrl+Z takes back. It also
		// restarts the spacing rule, so a fresh press right where the last one
		// ended still records a stamp instead of being suppressed as too close.
		FBF6ScatterLive::FStroke S;
		S.RadiusCm = FMath::Max(GScat.RadiusM, 0.5f) * 100.f;
		GScat.Strokes.Add(MoveTemp(S));
		// The cells already carrying copies stay filled across strokes: a new
		// stroke over old ground should not double up on it.
	}

	void ScatterPaintTo(const FVector& W)
	{
		if (!GScat.bActive || !GScat.bPainting || GScat.Strokes.Num() == 0) return;
		FBF6ScatterLive::FStroke& Stroke = GScat.Strokes.Last();
		const double R = FMath::Max((double)Stroke.RadiusCm, 50.0);

		// SPACING ALONG THE STROKE. Not about density - the fill decides that -
		// but about how many stamps the area is described by. A stamp every
		// third of a brush width traces the path closely and keeps the coverage
		// pass cheap.
		if (Stroke.Stamps.Num() > 0
			&& FVector::DistSquaredXY(Stroke.Stamps.Last(), W) < FMath::Square(R * 0.35)) return;
		Stroke.Stamps.Add(W);

		// ONLY THE GROUND JUST GAINED.
		//
		// A full refill destroys and respawns every copy in the whole painting,
		// and during a stroke that happens ten times a second over an area that
		// only ever grows - which is what made this crawl. The stroke can only
		// ADD coverage, so the copies already standing are still correct and
		// the only work due is the cells that just became covered.
		//
		// The authoritative refill still happens, on mouse-up, where a pause is
		// invisible. Everything a slider does goes through that path too, so
		// the exact semantics are never decided by this shortcut.
		BF6_ScatterPaintFillNew();

		const double Now = FPlatformTime::Seconds();
		if (Now - GScatPaintDrawn > 0.1)
		{
			GScatPaintDrawn = Now;
			BF6_ScatterRegionRebuild();   // the area, not the fill
		}
	}

	void EndScatterPaint()
	{
		if (!GScat.bActive) return;
		GScat.bPainting = false;
		// An empty stroke is one the creator clicked without dragging; keeping
		// it would make the next Ctrl+Z appear to do nothing.
		if (GScat.Strokes.Num() > 0 && GScat.Strokes.Last().Stamps.Num() == 0)
			GScat.Strokes.Pop();
		BF6_ScatterRegenerate();   // the final, unthrottled fill
	}

	bool IsScatterPainting() { return GScat.bActive && GScat.bPainting; }
	int32 ScatterStrokeCount() { return GScat.bActive ? GScat.Strokes.Num() : 0; }

	// One stroke back off the area. The fill follows, because the fill is only
	// ever a function of the strokes.
	bool ScatterPaintUndo()
	{
		if (!GScat.bActive || GScat.Shape != 4 || GScat.Strokes.Num() == 0) return false;
		GScat.Strokes.Pop();
		BF6_ScatterRegenerate();
		return true;
	}

	// The brush ring, in screen pixels, for the viewport overlay.
	bool GetScatterBrush(FVector2D& OutCenterPx, float& OutRadiusPx)
	{
		if (!GScat.bActive || GScat.Shape != 4) return false;
		FVector W;
		if (!WorldFromViewportCursor(W)) return false;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return false;
		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(VC->Viewport, VC->GetScene(), VC->EngineShowFlags));
		FSceneView* View = VC->CalcSceneView(&Family);
		if (!View) return false;
		auto Project = [&](const FVector& P, FVector2D& Px) -> bool
		{
			const FVector4 Clip = View->WorldToScreen(P);
			if (Clip.W <= 0.f) return false;
			View->ScreenToPixel(Clip, Px);
			return true;
		};
		// Projected rather than a fixed pixel size: a ring that stays the same
		// on screen tells you nothing about how much ground it covers.
		const double R = FMath::Max(GScat.RadiusM, 0.5f) * 100.0;
		FVector2D C, E;
		if (!Project(W, C) || !Project(W + FVector(R, 0, 0), E)) return false;
		OutCenterPx = C;
		OutRadiusPx = (float)FVector2D::Distance(C, E);
		return true;
	}

	bool GetScatterDots(TArray<FVector2D>& OutPx, int32& OutDrag)
	{
		OutPx.Reset(); OutDrag = INDEX_NONE;
		if (!GScat.bActive || GScatDotPx.Num() == 0) return false;
		OutPx = GScatDotPx;
		OutDrag = GScatDotDrag;
		return true;
	}

	int32 ScatterDotUnderMouse()
	{
		if (!GScat.bActive || GScatDotPx.Num() == 0) return INDEX_NONE;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return INDEX_NONE;
		const FVector2D M((float)VC->Viewport->GetMouseX(), (float)VC->Viewport->GetMouseY());
		const float Grab = 14.f * VC->GetDPIScale();
		int32 Best = INDEX_NONE; float BestD = Grab;
		for (int32 i = 0; i < GScatDotPx.Num(); i++)
		{
			const float D = FVector2D::Distance(M, GScatDotPx[i]);
			if (D < BestD) { BestD = D; Best = i; }
		}
		return Best;
	}

	bool IsScatterDotDragging() { return GScat.bActive && GScatDotDrag != INDEX_NONE; }

	void BeginScatterDotDrag(int32 Index)
	{
		if (!GScat.bActive || !GScat.Poly.IsValidIndex(Index)) return;
		BF6_ScatterPolySnapshot();   // one undo step reverts the whole drag
		GScatDotDrag = Index;
	}

	void DragScatterDotToCursor()
	{
		if (!GScat.bActive || !GScat.Poly.IsValidIndex(GScatDotDrag)) return;
		FVector Wp;
		if (!WorldFromViewportCursor(Wp)) return;
		GScat.Poly[GScatDotDrag] = Wp;
		BF6_ScatterRegionRebuild();   // the shape follows the drag live
	}

	void EndScatterDotDrag()
	{
		if (GScatDotDrag == INDEX_NONE) return;
		GScatDotDrag = INDEX_NONE;
		if (GScat.Poly.Num() >= 3) BF6_ScatterRegenerate();   // refill the new shape
	}

	void CancelScatterLive()
	{
		if (!GScat.bActive) return;
		GScat.bActive = false;
		GScatDotDrag = INDEX_NONE;
		GScatPolyUndo.Reset();
		GScatPolyRedo.Reset();
		BF6_ScatterDestroyPreview();
		BF6_ScatterRegionDestroy();
		BF6_RecomputeBudget();
	}

	void ApplyScatterLive()
	{
		if (!GScat.bActive) return;
		GScat.bActive = false;
		GScat.Strokes.Reset();
		GScat.bPainting = false;
		GScatDotDrag = INDEX_NONE;
		GScatPolyUndo.Reset();
		GScatPolyRedo.Reset();
		BF6_ScatterRegionDestroy();
		// The source check applies only to a selection scatter; a pool one has
		// no members and would fail it every time.
		if (GScat.Pool.Num() == 0)
			for (AActor* M : GScat.U.Members)
				if (!IsValid(M)) { BF6_ScatterDestroyPreview(); return; }
		// swap the preview for an identical, UNDOABLE rebuild
		BF6_ScatterDestroyPreview();
		FScopedTransaction Tx(FText::FromString(TEXT("Scatter")));
		FCollisionQueryParams QP(FName(TEXT("BF6ScatterLive")), true);
		TArray<AActor*> NewOnes;
		for (const FBF6ScatterLive::FTarget& T : GScat.Targets)
		{
			// EACH TARGET REMEMBERS WHICH SOURCE IT WAS, and it has to: replaying
			// a pool scatter through the selection unit spawns from an empty
			// member list, so the preview would vanish on Apply and leave
			// nothing behind. The preview IS the promise, and this is what
			// keeps it.
			if (GScat.Pool.IsValidIndex(T.Pool))
			{
				const FBF6ScatterSource& Src = GScat.Pool[T.Pool];
				const FQuat RotQ = FQuat(FVector::UpVector, FMath::DegreesToRadians(T.Yaw))
					* FQuat(FVector::ForwardVector, FMath::DegreesToRadians(T.Tx))
					* FQuat(FVector::RightVector, FMath::DegreesToRadians(T.Ty));
				if (AActor* A = SpawnSdkModel(Src.Mesh, Src.Type, FTransform(RotQ, T.P, FVector(T.Scl))))
				{ NewOnes.Add(A); QP.AddIgnoredActor(A); }
				continue;
			}
			BF6_SpawnUnitCopy(GScat.U, T.P, T.Yaw, T.Scl, QP, NewOnes, T.Tx, T.Ty);
		}
		if (GEditor)
		{
			GEditor->SelectNone(false, true, false);
			for (AActor* A : NewOnes) GEditor->SelectActor(A, true, false);
			GEditor->NoteSelectionChange();
		}
		BF6_RecomputeBudget();
		Notify(FString::Printf(TEXT("Scattered %d - one undo removes them all."), GScat.Targets.Num()));
	}

	void GroupSelection()
	{
		// grouping a node without its children would group an empty marker
		if (GEditor)
		{
			TArray<AActor*> Targets; SelectionTargets(Targets);
			for (AActor* A : Targets) GEditor->SelectActor(A, true, false);
			GEditor->NoteSelectionChange();
		}
		if (!UActorGroupingUtils::IsGroupingActive()) UActorGroupingUtils::SetGroupingActive(true);
		UActorGroupingUtils::Get()->GroupSelected();
	}
	void UngroupSelection()
	{
		if (!UActorGroupingUtils::IsGroupingActive()) UActorGroupingUtils::SetGroupingActive(true);
		UActorGroupingUtils::Get()->UngroupSelected();
		// splitting a block instance DETACHES it: the loose pieces stop being a
		// copy of the block (leaving the identity on ungrouped actors would let
		// a later block save or refresh mangle them)
		if (!GEditor) return;
		USelection* S = GEditor->GetSelectedActors(); if (!S) return;
		bool bDetached = false;
		for (int32 i = 0; i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
			{
				bool bTouched = false;
				for (int32 t = A->Tags.Num() - 1; t >= 0; t--)
				{
					const FString TS = A->Tags[t].ToString();
					if (TS.StartsWith(TEXT("blk:")) || TS.StartsWith(TEXT("blkid:")) || TS.StartsWith(TEXT("blkat:")))
					{
						if (!bTouched) { A->Modify(); bTouched = true; }
						A->Tags.RemoveAt(t);
					}
				}
				bDetached |= bTouched;
			}
		if (bDetached) Notify(TEXT("Ungrouped - these objects are detached from their block now."));
	}

	// ---- focus edit mode (Revit-style "tab into a group or block") ----
	// Double-click (or GROUPING > Edit) tabs into a group or placed block: only
	// its members stay solid and selectable, the rest of the world ghosts like
	// assign mode. ESC reverts every member to how it was on entry; ENTER keeps
	// the edits - and for a block, re-saves the definition so every other
	// placed copy refreshes to match.
	struct FBF6FocusEdit
	{
		bool bActive = false;
		FString BlockName;                        // empty = plain group
		TWeakObjectPtr<AGroupActor> Group;
		TArray<TWeakObjectPtr<AActor>> Members;
		struct FSnap { TWeakObjectPtr<AActor> Actor; FTransform Xf; TArray<FName> Tags; };
		TArray<FSnap> Snaps;                      // how everything looked on entry
		TArray<FBF6Ghosted> Ghosted;
	};
	static FBF6FocusEdit GFocus;
	static int32 BF6_SaveBlockFromActors(const FString& InName, const TArray<AActor*>& Picked);
	static void BF6_HealDuplicateBlockIds();

	bool SelectionGrouped()
	{
		FString T; AActor* A = SelectedGameplayActor(T);
		return A && (AGroupActor::GetRootForActor(A) != nullptr || !TagValue(A, TEXT("blkid:")).IsEmpty());
	}

	// What the selection looks like, for the context-sensitive controls panel:
	// count of our actors, whether they are one group/block acting as a unit,
	// and (single object) whether it has a model and editable fields.
	FSelInfo SelectionInfo()
	{
		FSelInfo I;
		if (!GEditor) return I;
		USelection* S = GEditor->GetSelectedActors(); if (!S) return I;
		AActor* Single = nullptr;
		AGroupActor* FirstRoot = nullptr;
		bool bAllRooted = true;
		for (int32 i = 0; i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
			{
				if (Cast<AGroupActor>(A)) continue;   // the wrapper, not a thing
				if (A->Tags.Contains(kHandleTag)) continue;
				// nodes count too: carrying or moving one takes its whole subtree,
				// which is half of what a tree is for
				if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)
					&& !A->Tags.Contains(kGroupTag)) continue;
				if (A->Tags.Contains(kGroupTag)) I.bNode = true;
				I.Count++;
				Single = A;
				if (!TagValue(A, TEXT("blkid:")).IsEmpty()) I.bBlock = true;
				AGroupActor* Rt = AGroupActor::GetRootForActor(A);
				if (!Rt) bAllRooted = false;
				else if (!FirstRoot) FirstRoot = Rt;
				else if (Rt != FirstRoot) bAllRooted = false;
			}
		I.bOneGroup = I.Count >= 1 && bAllRooted && FirstRoot != nullptr;
		if (I.Count == 1 && Single)
		{
			I.bMesh = !TagValue(Single, TEXT("mesh:")).IsEmpty();
			FString Ty = TagValue(Single, TEXT("label:"));
			if (Ty.IsEmpty()) Ty = TagValue(Single, TEXT("type:"));
			I.Fields = Ty.IsEmpty() ? 0 : PropsForType(Ty).Num();
		}
		return I;
	}
	bool IsGroupEditing() { return GFocus.bActive; }
	bool GroupEditIsBlock() { return GFocus.bActive && !GFocus.BlockName.IsEmpty(); }

	bool BeginGroupEditFromActor(AActor* Seed)
	{
		if (!Seed || GFocus.bActive || !GEditor) return false;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return false;

		// Ctrl+D / paste copies the source's instance id along with the tags:
		// split shared ids apart FIRST, or this edit would grab every copy
		BF6_HealDuplicateBlockIds();

		// a block instance beats the group wrapped around it: editing the BLOCK
		// is what lets the save fan out to its other copies
		TArray<AActor*> Members;
		const FString BlkId = TagValue(Seed, TEXT("blkid:"));
		AGroupActor* G = AGroupActor::GetRootForActor(Seed);
		if (!BlkId.IsEmpty())
		{
			for (TActorIterator<AActor> It(W); It; ++It)
				if (TagValue(*It, TEXT("blkid:")) == BlkId) Members.Add(*It);
		}
		else if (G) G->GetGroupActors(Members, true);
		if (Members.Num() == 0) return false;

		GFocus.bActive = true;
		GFocus.BlockName = BlkId.IsEmpty() ? FString() : TagValue(Seed, TEXT("blk:"));
		GFocus.Group = G;
		GFocus.Members.Reset();
		GFocus.Snaps.Reset();
		for (AActor* M : Members)
		{
			GFocus.Members.Add(M);
			FBF6FocusEdit::FSnap S; S.Actor = M; S.Xf = M->GetActorTransform(); S.Tags = M->Tags;
			GFocus.Snaps.Add(MoveTemp(S));
		}
		if (G) G->Unlock();

		// the assign-mode view: everything that isn't a member goes ghost
		TSet<AActor*> Keep;
		for (AActor* M : Members) Keep.Add(M);
		BF6_GhostAllExcept(Keep, GFocus.Ghosted);

		GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(Seed, true, true);
		Notify(GFocus.BlockName.IsEmpty()
			? FString(TEXT("Editing group: ENTER keeps the changes, ESC reverts them."))
			: FString::Printf(TEXT("Editing block '%s': ENTER saves it and updates every copy, ESC reverts."), *GFocus.BlockName));
		return true;
	}

	void BeginGroupEditFromSelection()
	{
		FString T; AActor* A = SelectedGameplayActor(T);
		if (!A || !BeginGroupEditFromActor(A))
			Notify(TEXT("Select a grouped object or a placed block first."));
	}

	void FinishGroupEdit(bool bKeepEdits)
	{
		if (!GFocus.bActive) return;
		GFocus.bActive = false;
		BF6_GhostRestoreSet(GFocus.Ghosted);

		TArray<AActor*> Alive;
		for (const TWeakObjectPtr<AActor>& Wk : GFocus.Members)
			if (AActor* M = Wk.Get()) Alive.Add(M);

		if (!bKeepEdits)
		{
			// put every surviving member back exactly how it was on entry
			FScopedTransaction Tx(FText::FromString(TEXT("Revert group edit")));
			for (FBF6FocusEdit::FSnap& S : GFocus.Snaps)
				if (AActor* M = S.Actor.Get())
				{
					M->Modify();
					M->SetActorTransform(S.Xf);
					M->Tags = S.Tags;
				}
			// resync built geometry from the restored tags: zone loops first,
			// then anything prop-driven (wall heights, box sizes)
			::BF6_RepairAfterUndo();
			for (FBF6FocusEdit::FSnap& S : GFocus.Snaps)
				if (AActor* M = S.Actor.Get())
				{
					const FString H = TagValue(M, TEXT("p:height="));
					if (!H.IsEmpty()) SetActorProp(M, TEXT("height"), H);
					const FString Sz = TagValue(M, TEXT("p:size="));
					if (!Sz.IsEmpty()) SetActorProp(M, TEXT("size"), Sz);
				}
		}
		else if (!GFocus.BlockName.IsEmpty() && Alive.Num() > 0)
		{
			// re-save the definition from the edited members; every other placed
			// copy of the block refreshes inside BF6_SaveBlockFromActors
			BF6_SaveBlockFromActors(GFocus.BlockName, Alive);
		}

		// lock the group back up (Lock() isn't exported: go through the utility)
		if (GEditor)
			if (AGroupActor* G = GFocus.Group.Get())
			{
				GEditor->SelectNone(false, true, false);
				GEditor->SelectActor(G, true, true);
				UActorGroupingUtils::Get()->LockSelectedGroups();
			}
		GFocus.Group = nullptr;
		GFocus.Members.Reset();
		GFocus.Snaps.Reset();
		GFocus.BlockName.Reset();
	}

	void TickGroupEdit()
	{
		if (!GFocus.bActive || !GEditor) return;
		USelection* S = GEditor->GetSelectedActors(); if (!S) return;
		TArray<AActor*> Strip;
		for (int32 i = 0; i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
			{
				if (A == GFocus.Group.Get()) continue;
				bool bMember = false;
				for (const TWeakObjectPtr<AActor>& M : GFocus.Members)
					if (M.Get() == A) { bMember = true; break; }
				if (!bMember) Strip.Add(A);
			}
		for (AActor* A : Strip) GEditor->SelectActor(A, false, true);
	}

	// what the editor cursor is over, via the viewport's hit proxies (the
	// double-click "tab into it" entry point)
	AActor* ActorUnderCursor()
	{
		// GetHitProxy forces a hit-proxy RENDER; doing that while a package is
		// saving (or GC runs) is the "Illegal call to StaticFindObjectFast()
		// while serializing" FATAL from the crash dumps - a click during the
		// save dialog fired this through the input processor
		if (UE::IsSavingPackage() || IsGarbageCollecting()) return nullptr;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return nullptr;
		FIntPoint MP;
		VC->Viewport->GetMousePos(MP);
		if (MP.X < 0 || MP.Y < 0) return nullptr;
		if (HHitProxy* HP = VC->Viewport->GetHitProxy(MP.X, MP.Y))
			if (HP->IsA(HActor::StaticGetType()))
				return static_cast<HActor*>(HP)->Actor;
		return nullptr;
	}

	// ---- Godot-style box select ----
	// In Godot, dragging LMB on empty ground rubber-bands a selection - no
	// modifier keys. The input processor claims empty-space LMB (actors and
	// the transform gizmo are hit proxies, so clicking THEM stays native),
	// these track the marquee, and release selects everything inside.
	struct FBF6BoxSel { bool bActive = false; FIntPoint Start; FIntPoint Cur; };
	static FBF6BoxSel GBoxSel;

	bool ViewportHitProxyEmpty()
	{
		// same serialization guard as ActorUnderCursor: never render hit
		// proxies while saving or collecting garbage
		if (UE::IsSavingPackage() || IsGarbageCollecting()) return false;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return false;
		// the transform gizmo: its hover state is refreshed by the editor on
		// every mouse-move and is more reliable than the raw hit-proxy read
		// (a stale proxy buffer read null over the axes, and box select stole
		// the axis drag)
		if (VC->GetCurrentWidgetAxis() != EAxisList::None) return false;
		FIntPoint MP;
		VC->Viewport->GetMousePos(MP);
		if (MP.X < 0 || MP.Y < 0) return false;
		return VC->Viewport->GetHitProxy(MP.X, MP.Y) == nullptr;
	}

	void BeginBoxSelect()
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return;
		VC->Viewport->GetMousePos(GBoxSel.Start);
		GBoxSel.Cur = GBoxSel.Start;
		GBoxSel.bActive = true;
	}

	void UpdateBoxSelect()
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (GBoxSel.bActive && VC && VC->Viewport) VC->Viewport->GetMousePos(GBoxSel.Cur);
	}

	void CancelBoxSelect() { GBoxSel.bActive = false; }

	bool GetBoxSelectRect(FVector2D& OutA, FVector2D& OutB)
	{
		if (!GBoxSel.bActive) return false;
		OutA = FVector2D(FMath::Min(GBoxSel.Start.X, GBoxSel.Cur.X), FMath::Min(GBoxSel.Start.Y, GBoxSel.Cur.Y));
		OutB = FVector2D(FMath::Max(GBoxSel.Start.X, GBoxSel.Cur.X), FMath::Max(GBoxSel.Start.Y, GBoxSel.Cur.Y));
		return true;
	}

	int32 EndBoxSelect(bool bAdd)
	{
		if (!GBoxSel.bActive) return 0;
		FVector2D A, B;
		GetBoxSelectRect(A, B);
		GBoxSel.bActive = false;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport || !GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(VC->Viewport, VC->GetScene(), VC->EngineShowFlags));
		FSceneView* View = VC->CalcSceneView(&Family);
		if (!View) return 0;
		if (!bAdd) GEditor->SelectNone(false, true, false);
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)
				&& !It->Tags.Contains(kGroupTag)) continue;   // nodes drag-select too
			if (It->Tags.Contains(kHandleTag)) continue;
			// ghosted (unselectable) objects stay out, like native selection
			if (UPrimitiveComponent* RC = Cast<UPrimitiveComponent>(It->GetRootComponent()))
				if (!RC->bSelectable) continue;
			FVector2D Px;
			if (!View->WorldToPixel(It->GetActorLocation(), Px)) continue;
			if (Px.X < A.X || Px.X > B.X || Px.Y < A.Y || Px.Y > B.Y) continue;
			GEditor->SelectActor(*It, true, false);
			n++;
		}
		GEditor->NoteSelectionChange();
		return n;
	}

	// ---- Godot-style drag-move ----
	// In Godot you move a node by just dragging it. The processor claims LMB
	// that lands on an ALREADY SELECTED placed/base actor (unselected actors
	// keep the native click-to-select, groups included), and these slide the
	// whole selection on the horizontal plane of the grab point. Ctrl snaps
	// to the metre grid. One transaction per drag = one undo; Esc reverts.
	struct FBF6DragMove
	{
		bool bPending = false;   // LMB went down on a selected movable actor
		bool bMoving = false;    // passed the threshold, transaction open
		FVector GrabW = FVector::ZeroVector;
		double PlaneZ = 0;
		TArray<TWeakObjectPtr<AActor>> Movers;
	};
	static FBF6DragMove GDragMove;

	// Hit-proxy-free classification for the Godot LMB gestures. The editor's
	// hit-proxy read proved unreliable at mouse-down (stale buffer: null over
	// axes AND objects), and our proc meshes carry no physics, so this uses
	// cursor-ray vs actor BOUNDS for our objects, a real trace for the map
	// surface, and a screen-radius zone around the selection pivot for the
	// gizmo. Returns 0 = empty (box select), 1 = one of our actors (OutActor),
	// 2 = the gizmo zone (always Unreal's). OutActor is filled when an actor
	// is under the cursor even in the gizmo zone (the double-click needs it).
	// A polygon volume is picked by its WALLS, never its bounds: a big combat
	// zone's box covers the whole map, and clicking open ground inside it
	// kept selecting the zone. Ray vs each wall quad (two triangles).
	static bool BF6_RayHitsVolumeWalls(AActor* Vol, const TArray<FVector>& LocalLoop,
		const FVector& O, const FVector& End, double& OutT)
	{
		const TArray<FVector> Loop = BF6_LoopToWorld(Vol, LocalLoop);
		if (Loop.Num() < 3) return false;
		double H = 5.0;
		const FString HS = GetActorProp(Vol, TEXT("height"));
		if (HS.IsNumeric()) H = FCString::Atod(*HS);
		if (H <= 0.01) H = 5.0;   // 0 = infinite, drawn at 5 m like Godot
		const FVector Up(0, 0, H * 100.0);
		const double Len = (End - O).Size();
		bool bHit = false;
		for (int32 i = 0; i < Loop.Num(); i++)
		{
			const FVector A = Loop[i], B = Loop[(i + 1) % Loop.Num()];
			FVector Pt, N;
			if (FMath::SegmentTriangleIntersection(O, End, A, B, B + Up, Pt, N)
				|| FMath::SegmentTriangleIntersection(O, End, A, B + Up, A + Up, Pt, N))
			{
				const double T = Len > 1.0 ? (Pt - O).Size() / Len : 0.0;
				if (!bHit || T < OutT) { OutT = T; bHit = true; }
			}
		}
		return bHit;
	}

	int32 ClassifyCursorForGodotClick(AActor*& OutActor)
	{
		OutActor = nullptr;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport || !GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;

		// our actors, by nearest ray-vs-bounds entry (volumes by their walls)
		const FViewportCursorLocation Cur = VC->GetCursorWorldLocationFromMousePos();
		const FVector O = Cur.GetOrigin(), Dir = Cur.GetDirection();
		const FVector End = O + Dir * 500000.0;
		double BestT = 1e18;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			// nodes are pickable too - the marker is the only way to grab one in the
			// viewport instead of hunting for it in the tree
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)
				&& !It->Tags.Contains(kGroupTag)) continue;
			if (It->Tags.Contains(kHandleTag)) continue;
			if (UPrimitiveComponent* RC = Cast<UPrimitiveComponent>(It->GetRootComponent()))
				if (!RC->bSelectable) continue;   // ghosted stays unpickable
			if (const TArray<FVector>* Loop = GVolumeLoops.Find(*It))
			{
				double T = 0.0;
				if (Loop->Num() >= 3 && BF6_RayHitsVolumeWalls(*It, *Loop, O, End, T) && T < BestT)
				{
					BestT = T;
					OutActor = *It;
				}
				continue;   // never by AABB - the interior stays click-through
			}
			const FBox B = It->GetComponentsBoundingBox(true);
			if (!B.IsValid) continue;
			FVector HitL, HitN; float T = 0.f;
			if (FMath::LineExtentBoxIntersection(B, O, End, FVector::ZeroVector, HitL, HitN, T) && (double)T < BestT)
			{
				BestT = T;
				OutActor = *It;
			}
		}
		// the map surface wins when it is CLOSER than the object's bounds -
		// that click was on the ground in front of the object
		if (OutActor)
		{
			FVector CtxHit;
			double CtxU = 1.0;
			if (BF6_ContextRay(O, End, CtxHit, &CtxU) && CtxU < BestT - 1e-4)
				OutActor = nullptr;
		}

		// gizmo zone: a screen radius around the selection pivot
		USelection* Sel = GEditor->GetSelectedActors();
		if (Sel && Sel->Num() > 0)
		{
			FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(VC->Viewport, VC->GetScene(), VC->EngineShowFlags));
			if (FSceneView* View = VC->CalcSceneView(&Family))
			{
				FVector2D Px;
				const FVector2D M((float)VC->Viewport->GetMouseX(), (float)VC->Viewport->GetMouseY());
				if (View->WorldToPixel(VC->GetWidgetLocation(), Px)
					&& FVector2D::Distance(Px, M) < 130.f * VC->GetDPIScale())
					return 2;
			}
		}
		return OutActor ? 1 : 0;
	}

	// ---- fast delete ----
	// A proc-mesh component is TRANSACTIONAL (the gizmo records moves on the
	// root component, so it must ride the undo buffer), which made stock
	// delete serialize every section's VERTEX DATA into the transaction -
	// hundreds of scattered meshes meant multi-second deletes. This empties
	// the sections FIRST, outside the transaction, then deletes: the buffer
	// records featherweight components, and BF6_RepairAfterUndo refills
	// undeleted meshes from the bf6mesh files on disk.
	bool DeleteSelectionFast()
	{
		if (!GEditor) return false;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return false;
		TArray<AActor*> Doomed;
		USelection* Sel = GEditor->GetSelectedActors();
		for (int32 i = 0; Sel && i < Sel->Num(); i++)
			if (AActor* A = Cast<AActor>(Sel->GetSelectedObject(i)))
				// nodes too, or deleting one falls through to the editor's own delete -
				// which walks every object in the world and took seconds on a big map
				if (A->Tags.Contains(kPlacedTag) || A->Tags.Contains(kBaseTag)
					|| A->Tags.Contains(kGroupTag) || Cast<AGroupActor>(A))
					Doomed.Add(A);
		if (Doomed.Num() == 0) return false;   // nothing of ours: stock delete
		// group members ride along, like the editor's own delete
		TSet<AActor*> All;
		for (AActor* A : Doomed)
		{
			if (AGroupActor* G = Cast<AGroupActor>(A))
			{
				TArray<AActor*> Members;
				G->GetGroupActors(Members, true);
				for (AActor* M : Members) if (M) All.Add(M);
			}
			// children go with the parent, as they do in Godot
			TArray<AActor*> Sub;
			BF6_CollectSubtree(A, Sub);
			for (AActor* S : Sub) All.Add(S);
		}
		const double T0 = FPlatformTime::Seconds();
		for (AActor* A : All)
			if (UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent()))
				M->ClearAllMeshSections();
		FScopedTransaction Tx(FText::FromString(TEXT("Delete")));
		GEditor->SelectNone(false, true, false);
		int32 n = 0;
		for (AActor* A : All)
			if (W->EditorDestroyActor(A, true)) n++;
		// This path destroys actors itself and never raises the editor's own
		// delete delegate, so the prune has to happen here too. Inside the same
		// transaction, so one Ctrl+Z brings back the objects AND their links.
		BF6_PruneDeadLinks();
		GEditor->NoteSelectionChange();
		BF6_RecomputeBudget();
		UE_LOG(LogBF6, Log, TEXT("fast delete: %d actor(s) in %.0f ms"), n, (FPlatformTime::Seconds() - T0) * 1000.0);
		return true;
	}

	// native-like click selection: a grouped member selects its whole group
	void SelectClicked(AActor* A)
	{
		if (!A || !GEditor) return;
		AGroupActor* G = AGroupActor::GetRootForActor(A);
		GEditor->SelectNone(false, true, false);
		if (G)
		{
			TArray<AActor*> Members;
			G->GetGroupActors(Members, true);
			for (AActor* M : Members) if (M) GEditor->SelectActor(M, true, false);
			GEditor->SelectActor(G, true, false);
		}
		else GEditor->SelectActor(A, true, false);
		GEditor->NoteSelectionChange();
	}

	// ---- moving with UNREAL'S gizmo ----
	// The gizmo opens its own transaction and speculatively Modify()s the
	// selection, which snapshots our whole vertex payload into the undo buffer -
	// seconds of stall on a big object, the same bill deletes used to pay. There
	// is no engine hook before that Modify, but our input handler sees the mouse
	// press first, so the payload is emptied there and put back the moment the
	// transaction is open (GUndo set) and the snapshot is already taken. The
	// object is only bare for that instant. Undo restores an empty mesh and the
	// repair pass refills it, exactly as it does for a delete.
	static TArray<TWeakObjectPtr<AActor>> GStripped;

	// Emptying the payload the public way (ClearAllMeshSections) also shrinks the
	// bounds, rebuilds collision and marks the render state dirty, and that last
	// one is what made the object blink out for a frame at each end of a drag: the
	// scene proxy is thrown away and rebuilt from an array we just emptied. The
	// proxy keeps its own copy of the vertices, so dropping the data without
	// telling the renderer leaves it drawing while the snapshot is taken. The array
	// is private, hence reflection; it is a UPROPERTY, which is exactly why the
	// transaction was serialising it in the first place.
	bool EmptySectionsQuietly(UProceduralMeshComponent* M)
	{
		static FArrayProperty* Prop = FindFProperty<FArrayProperty>(
			UProceduralMeshComponent::StaticClass(), TEXT("ProcMeshSections"));
		if (!Prop) return false;   // renamed by an engine update: caller falls back
		FScriptArrayHelper Sections(Prop, Prop->ContainerPtrToValuePtr<void>(M));
		Sections.EmptyValues();
		return true;
	}

	int32 StripSelectionForTransaction()
	{
		if (!GEditor) return 0;   // additive: anything already emptied is skipped below
		USelection* Sel = GEditor->GetSelectedActors();
		TSet<AActor*> Set;
		for (int32 i = 0; Sel && i < Sel->Num(); i++)
		{
			AActor* S = Cast<AActor>(Sel->GetSelectedObject(i));
			if (!S) continue;
			if (AGroupActor* G = Cast<AGroupActor>(S))
			{
				TArray<AActor*> Members;
				G->GetGroupActors(Members, true);
				for (AActor* M : Members) if (M) Set.Add(M);
			}
			else Set.Add(S);
		}
		// moving a parent moves - and transacts - everything under it
		for (AActor* S : TSet<AActor*>(Set))
		{
			TArray<AActor*> Sub;
			BF6_CollectSubtree(S, Sub);
			for (AActor* K : Sub) Set.Add(K);
		}
		int32 n = 0;
		for (AActor* A : Set)
		{
			if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
			UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
			if (!M || M->GetNumSections() == 0) continue;
			if (!EmptySectionsQuietly(M)) M->ClearAllMeshSections();
			GStripped.Add(A);
			n++;
		}
		if (n > 0) UE_LOG(LogBF6, Log, TEXT("gizmo drag: stripped %d object(s) before the snapshot"), n);
		return n;
	}

	bool HasStrippedGeometry() { return GStripped.Num() > 0; }

	// Put the geometry back only for objects the transaction has already
	// recorded. Waiting on 'a transaction exists' was not enough: the gizmo
	// takes its snapshot at some point AFTER opening one, so refilling too
	// early simply handed it the full mesh again - which is why the stall
	// survived the first attempt.
	void RestoreStrippedGeometry(bool bForce)
	{
		if (GStripped.Num() == 0) return;
		bool bAny = false;
		for (int32 i = GStripped.Num() - 1; i >= 0; i--)
		{
			AActor* A = GStripped[i].Get();
			if (!A) { GStripped.RemoveAt(i); continue; }
			const bool bRecorded = GUndo && GUndo->ContainsObject(A);
			if (!bForce && !bRecorded) continue;   // snapshot not taken yet
			BF6_RebuildActorGeometry(A);
			GStripped.RemoveAt(i);
			bAny = true;
		}
		if (bAny) BF6_Redraw();
	}

	bool BeginDragMoveOn(AActor* A)
	{
		if (!A || !GEditor) return false;
		// Godot in one motion: pressing an unselected object selects it first
		if (!A->IsSelected()) SelectClicked(A);
		FVector W;
		if (!WorldFromViewportCursor(W)) return false;
		GDragMove = FBF6DragMove();
		GDragMove.bPending = true;
		GDragMove.GrabW = W;
		GDragMove.PlaneZ = W.Z;
		// the move set: every selected actor, groups expanded to their members
		TSet<AActor*> Set;
		USelection* Sel = GEditor->GetSelectedActors();
		for (int32 i = 0; Sel && i < Sel->Num(); i++)
		{
			AActor* S = Cast<AActor>(Sel->GetSelectedObject(i));
			if (!S) continue;
			if (AGroupActor* G = Cast<AGroupActor>(S))
			{
				TArray<AActor*> Members;
				G->GetGroupActors(Members, true);
				for (AActor* M : Members) if (M) Set.Add(M);
			}
			else Set.Add(S);
		}
		for (AActor* S : Set) GDragMove.Movers.Add(S);
		if (GDragMove.Movers.Num() == 0) { GDragMove = FBF6DragMove(); return false; }
		// same rule while dragging: the moving objects are not the surface
		{
			TArray<AActor*> Moving;
			for (const TWeakObjectPtr<AActor>& Wk : GDragMove.Movers) if (AActor* M = Wk.Get()) Moving.Add(M);
			SetPlacementIgnore(Moving);
		}
		return true;
	}

	void UpdateDragMove(bool bSnap)
	{
		if (!GDragMove.bPending) return;
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC) return;
		const FViewportCursorLocation Cur = VC->GetCursorWorldLocationFromMousePos();
		const FVector O = Cur.GetOrigin(), D = Cur.GetDirection();
		if (FMath::Abs(D.Z) < 1e-6) return;
		const double t = (GDragMove.PlaneZ - O.Z) / D.Z;
		if (t <= 0.0) return;
		FVector W = O + D * t;
		if (bSnap)
		{
			W.X = FMath::GridSnap(W.X, 100.0);
			W.Y = FMath::GridSnap(W.Y, 100.0);
		}
		FVector Delta = W - GDragMove.GrabW;
		Delta.Z = 0;
		if (Delta.IsNearlyZero()) return;
		if (!GDragMove.bMoving)
		{
			GDragMove.bMoving = true;
			// A proc-mesh component is TRANSACTIONAL so the gizmo can undo a move,
			// which means Modify() snapshots its whole vertex payload into the undo
			// buffer - megabytes per object, and the reason a drag on a dense map
			// took seconds to start and to let go. Same cure as delete: empty the
			// mesh first so the buffer records a featherweight component, then put
			// the geometry straight back. Undo restores the empty mesh and the
			// repair pass refills it, exactly as it already does for deletes.
			const double MoveT0 = FPlatformTime::Seconds();
			for (TWeakObjectPtr<AActor>& Wk : GDragMove.Movers)
				if (AActor* M = Wk.Get())
					if (UProceduralMeshComponent* PM = Cast<UProceduralMeshComponent>(M->GetRootComponent()))
						PM->ClearAllMeshSections();
			GEditor->BeginTransaction(FText::FromString(TEXT("Move")));
			for (TWeakObjectPtr<AActor>& Wk : GDragMove.Movers)
				if (AActor* M = Wk.Get()) M->Modify();
			for (TWeakObjectPtr<AActor>& Wk : GDragMove.Movers)
				if (AActor* M = Wk.Get()) BF6_RebuildActorGeometry(M);
			UE_LOG(LogBF6, Log, TEXT("drag start: %d object(s) in %.0f ms"),
				GDragMove.Movers.Num(), (FPlatformTime::Seconds() - MoveT0) * 1000.0);
		}
		for (TWeakObjectPtr<AActor>& Wk : GDragMove.Movers)
			if (AActor* M = Wk.Get())
				M->SetActorLocation(M->GetActorLocation() + Delta);
		GDragMove.GrabW = W;
	}

	void EndDragMove()
	{
		ClearPlacementIgnore();
		if (GDragMove.bMoving && GEditor)
		{
			// closing the transaction is the other half of the old stall
			const double EndT0 = FPlatformTime::Seconds();
			GEditor->EndTransaction();
			GEditor->NoteSelectionChange();
			const double EndMs = (FPlatformTime::Seconds() - EndT0) * 1000.0;
			if (EndMs > 100.0) UE_LOG(LogBF6, Warning, TEXT("drag end took %.0f ms"), EndMs);
		}
		GDragMove = FBF6DragMove();
	}

	void CancelDragMove()
	{
		ClearPlacementIgnore();
		if (GDragMove.bMoving && GEditor) GEditor->CancelTransaction(0);
		GDragMove = FBF6DragMove();
	}

	// ---- PICK PLACE: carry the selection with the cursor, click to set it
	// down ---- Like a drag-and-drop, but for something already placed: the
	// whole selection (groups expanded) rides the cursor along the terrain,
	// keeping its internal offsets and heights. Click drops it (one undo
	// reverts), Esc puts everything back where it was.
	struct FBF6PickPlace
	{
		bool bActive = false;
		TArray<TWeakObjectPtr<AActor>> Movers;
		TArray<FVector> Offsets;    // per mover, from the reference ground point
		TArray<FRotator> StartRot;  // per mover, so Yaw is applied ONCE not compounded
		FVector Ref = FVector::ZeroVector;
		// One yaw for the whole carried set, turned about the reference point.
		// Stored rather than applied straight to the actors, because the tick
		// rebuilds every transform from Ref + Offsets each frame and anything
		// written directly would be overwritten by the next mouse move.
		float Yaw = 0.f;
	};
	static FBF6PickPlace GPickPlace;

	bool IsPickPlacing() { return GPickPlace.bActive; }

	// Turn what is being carried, before setting it down.
	//
	// Rotating after placing means finding the gizmo, switching to rotate, and
	// aiming it - all of it undone the moment the object turns out to be in the
	// wrong spot. Turning it in the hand is the same decision made once.
	bool RotatePickPlace(float DeltaDeg, bool bSnap)
	{
		if (!GPickPlace.bActive) return false;
		GPickPlace.Yaw += DeltaDeg;
		// Held Ctrl means the same thing here as everywhere else in the tool:
		// land on the round number rather than near it.
		if (bSnap) GPickPlace.Yaw = FMath::GridSnap(GPickPlace.Yaw, 15.f);
		GPickPlace.Yaw = FMath::Fmod(GPickPlace.Yaw, 360.f);
		TickPickPlace(false);   // show it immediately, without waiting for a mouse move
		return true;
	}

	bool BeginPickPlace()
	{
		if (GPickPlace.bActive || !GEditor) return false;
		GPickPlace = FBF6PickPlace();
		TSet<AActor*> Set;
		USelection* Sel = GEditor->GetSelectedActors();
		for (int32 i = 0; Sel && i < Sel->Num(); i++)
		{
			AActor* S = Cast<AActor>(Sel->GetSelectedObject(i));
			if (!S) continue;
			if (AGroupActor* G = Cast<AGroupActor>(S))
			{
				TArray<AActor*> Members;
				G->GetGroupActors(Members, true);
				for (AActor* M : Members) if (M) Set.Add(M);
			}
			else Set.Add(S);
		}
		if (Set.Num() == 0) { Notify(TEXT("Select something to pick up first.")); return false; }
		// reference = the selection's bounds centre at its lowest point, so
		// the whole thing sits ON the cursor's surface point
		FBox B(ForceInit);
		for (AActor* S : Set) B += S->GetActorLocation();
		GPickPlace.Ref = FVector(B.GetCenter().X, B.GetCenter().Y, B.Min.Z);
		for (AActor* S : Set)
		{
			GPickPlace.Movers.Add(S);
			GPickPlace.Offsets.Add(S->GetActorLocation() - GPickPlace.Ref);
			GPickPlace.StartRot.Add(S->GetActorRotation());
		}
		GEditor->BeginTransaction(FText::FromString(TEXT("Pick Place")));
		for (TWeakObjectPtr<AActor>& Wk : GPickPlace.Movers)
			if (AActor* M = Wk.Get()) M->Modify();
		GPickPlace.bActive = true;
		// the carried objects must not act as the surface under themselves
		{
			TArray<AActor*> Carried;
			for (const TWeakObjectPtr<AActor>& Wk : GPickPlace.Movers) if (AActor* M = Wk.Get()) Carried.Add(M);
			SetPlacementIgnore(Carried);
		}
		return true;
	}

	void TickPickPlace(bool bSnap)
	{
		if (!GPickPlace.bActive) return;
		FVector W;
		if (!WorldFromViewportCursor(W)) return;
		if (bSnap)
		{
			W.X = FMath::GridSnap(W.X, 100.0);
			W.Y = FMath::GridSnap(W.Y, 100.0);
		}
		// The carried set turns about its OWN reference point, so a row of
		// objects pivots as one piece instead of each spinning in place and the
		// arrangement coming apart.
		const FRotator Turn(0.f, GPickPlace.Yaw, 0.f);
		for (int32 i = 0; i < GPickPlace.Movers.Num(); i++)
			if (AActor* M = GPickPlace.Movers[i].Get())
			{
				M->SetActorLocation(W + Turn.RotateVector(GPickPlace.Offsets[i]));
				if (GPickPlace.StartRot.IsValidIndex(i))
					M->SetActorRotation(FRotator(GPickPlace.StartRot[i].Pitch,
						GPickPlace.StartRot[i].Yaw + GPickPlace.Yaw, GPickPlace.StartRot[i].Roll));
			}
		GPickPlace.Ref = W;
	}

	void FinishPickPlace()
	{
		ClearPlacementIgnore();
		if (!GPickPlace.bActive) return;
		GPickPlace.bActive = false;
		if (GEditor)
		{
			GEditor->EndTransaction();
			GEditor->NoteSelectionChange();
		}
		BF6_RecomputeBudget();
		GPickPlace = FBF6PickPlace();
	}

	void CancelPickPlace()
	{
		ClearPlacementIgnore();
		if (!GPickPlace.bActive) return;
		GPickPlace.bActive = false;
		if (GEditor) GEditor->CancelTransaction(0);   // everything returns home
		GPickPlace = FBF6PickPlace();
	}

	// ---- Blocks: named user prefabs, one portable JSON each ----
	// Share a block by sending its file; received files just go in the folder.
	static FString BF6_BlocksDir() { return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("blocks")); }
	static FString BF6_BlockPath(const FString& Name) { return BF6_BlocksDir() / (Name + TEXT(".json")); }
	static FString BF6_SafeName(const FString& In)
	{
		FString S = In.TrimStartAndEnd();
		for (TCHAR& C : S) if (!FChar::IsAlnum(C) && C != '_' && C != '-' && C != ' ') C = '_';
		return S;
	}

	static void  BF6_TagBlockInstance(const TArray<AActor*>& Actors, const FString& Name, const FVector& Anchor);
	static int32 BF6_RefreshBlockInstances(const FString& Name, const TArray<AActor*>& Keep);

	int32 SaveBlockFromSelection(const FString& InName)
	{
		if (!GEditor) return 0;
		// selecting the node that holds a build is the natural way to save it
		TArray<AActor*> Targets; SelectionTargets(Targets);
		TArray<AActor*> Picked;
		for (AActor* A : Targets)
			if (A->Tags.Contains(kPlacedTag)) Picked.Add(A);
		if (Picked.Num() == 0) { Notify(TEXT("Select placed objects first (base objects can't go in a block).")); return 0; }
		return BF6_SaveBlockFromActors(InName, Picked);
	}

	// The actual save: build the definition JSON from these actors, stamp them
	// as an instance, and refresh every OTHER placed copy to match. Used by the
	// save-block popup (via selection) and by finishing a block focus edit.
	// Links between block MEMBERS are stored by member index ("@3") instead of
	// by actor name: every placed copy gets fresh actor labels, so name-based
	// links broke the moment a block was placed (the same failure the Godot
	// community hits hand-copying prefabs - "the Sector will lose its data
	// for the capture points"). Save encodes, spawn decodes.
	static bool BF6_IsLinkProp(const FString& TypeName, const FString& PropName)
	{
		for (const FPropDef& D : PropsForType(TypeName))
			if (D.Name == PropName)
				return D.Type.Contains(TEXT("Volume")) || D.Type.Contains(TEXT("Array["))
				    || D.Type.Contains(TEXT("Path"))   || D.Type.Contains(TEXT("SpawnPoint"));
		return false;
	}
	// Drop links that point at objects which no longer exist.
	//
	// A link is stored as a NAME, so deleting the thing it names leaves the
	// name behind: an HQ still listing eight spawns after they were deleted,
	// with the attribute panel reporting them as assigned. The link is the only
	// part that matters and it is the part with no visual, so nothing about the
	// map looks wrong until it is played.
	//
	// Swept over every link field of every object rather than special-cased for
	// spawns, because every link type has this same hole.
	//
	// Runs INSIDE the delete transaction and calls Modify() first, so undoing
	// the delete brings the links back with the objects.
	static int32 BF6_PruneDeadLinks()
	{
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) return 0;

		TSet<FString> Alive;
		TArray<AActor*> Ours;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag) && !A->Tags.Contains(kGroupTag)) continue;
			if (!IsValid(A)) continue;
			Ours.Add(A);
			FString Nm = A->GetActorLabel();
			Nm.RemoveFromStart(TEXT("BF6_"));
			Alive.Add(Nm);
		}

		int32 Fixed = 0;
		for (AActor* A : Ours)
		{
			FString Ty = TagValue(A, TEXT("label:"));
			if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("type:"));
			if (Ty.IsEmpty()) continue;
			for (const FPropDef& D : PropsForType(Ty))
			{
				if (!BF6_IsLinkProp(Ty, D.Name)) continue;
				const FString Val = GetActorProp(A, D.Name);
				if (Val.IsEmpty()) continue;
				// A raw Godot value from a shipped scene is not ours to rewrite.
				if (Val.Contains(TEXT("NodePath")) || Val.Contains(TEXT("ExtResource"))) continue;
				TArray<FString> Parts;
				Val.ParseIntoArray(Parts, TEXT(","), true);
				const int32 Before = Parts.Num();
				Parts.RemoveAll([&Alive](const FString& N){ return !Alive.Contains(N.TrimStartAndEnd()); });
				if (Parts.Num() == Before) continue;
				A->Modify();
				SetActorProp(A, D.Name, FString::Join(Parts, TEXT(",")));
				Fixed++;
			}
		}
		if (Fixed > 0) UE_LOG(LogBF6, Warning, TEXT("pruned dead links on %d object(s)"), Fixed);
		return Fixed;
	}

	int32 PruneDeadLinks() { return BF6_PruneDeadLinks(); }

	// ---- the transform block in the attributes panel ----------------------
	//
	// Typing "10" into Z should mean ten METRES above the parent, the way it
	// does in Godot's inspector - not a thousand centimetres of world
	// coordinate in a Details panel two menus away. Metres because that is the
	// hand our creators arrive with; Unreal's axes because this is Unreal and
	// the gizmo, the maps and every other number here already speak them.
	bool GetXformM(AActor* A, FXformM& Out)
	{
		if (!A) return false;
		const bool bRel = A->GetAttachParentActor() != nullptr;
		const FVector L = bRel ? A->GetRootComponent()->GetRelativeLocation() : A->GetActorLocation();
		const FRotator R = bRel ? A->GetRootComponent()->GetRelativeRotation() : A->GetActorRotation();
		Out.Pos = L / 100.0;
		Out.Rot = FVector(R.Roll, R.Pitch, R.Yaw);
		Out.Scale = A->GetActorRelativeScale3D();
		Out.bRelative = bRel;
		return true;
	}

	// One axis at a time, because that is how typing works: each commit reads
	// the live transform and replaces only the number that changed, so two
	// fields edited in a row cannot clobber each other.
	void SetXformPosM(AActor* A, int32 Axis, double Metres)
	{
		if (!A || Axis < 0 || Axis > 2) return;
		const bool bRel = A->GetAttachParentActor() != nullptr;
		FScopedTransaction Tx(FText::FromString(TEXT("Set position")));
		A->Modify();
		FVector L = bRel ? A->GetRootComponent()->GetRelativeLocation() : A->GetActorLocation();
		L[Axis] = Metres * 100.0;
		if (bRel) A->GetRootComponent()->SetRelativeLocation(L); else A->SetActorLocation(L);
		BF6_NotePivotMoved();
		BF6_Redraw();
	}

	void SetXformRotDeg(AActor* A, int32 Axis, double Degrees)
	{
		if (!A || Axis < 0 || Axis > 2) return;
		const bool bRel = A->GetAttachParentActor() != nullptr;
		FScopedTransaction Tx(FText::FromString(TEXT("Set rotation")));
		A->Modify();
		FRotator R = bRel ? A->GetRootComponent()->GetRelativeRotation() : A->GetActorRotation();
		if (Axis == 0) R.Roll = Degrees; else if (Axis == 1) R.Pitch = Degrees; else R.Yaw = Degrees;
		if (bRel) A->GetRootComponent()->SetRelativeRotation(R); else A->SetActorRotation(R);
		BF6_NotePivotMoved();
		BF6_Redraw();
	}

	void SetXformScale(AActor* A, int32 Axis, double InScale)
	{
		if (!A || Axis < 0 || Axis > 2) return;
		FScopedTransaction Tx(FText::FromString(TEXT("Set scale")));
		A->Modify();
		FVector Sc = A->GetActorRelativeScale3D();
		Sc[Axis] = FMath::Max(0.0001, InScale);
		A->SetActorRelativeScale3D(Sc);
		BF6_NotePivotMoved();
		BF6_Redraw();
	}

	static FString BF6_LinkName(AActor* A)
	{
		FString Nm = A->GetActorLabel();
		Nm.RemoveFromStart(TEXT("BF6_"));
		return Nm;
	}

	static int32 BF6_SaveBlockFromActors(const FString& InName, const TArray<AActor*>& Picked)
	{
		const FString Name = BF6_SafeName(InName);
		if (Name.IsEmpty() || Picked.Num() == 0) return 0;

		// member name -> index, for encoding intra-block links
		TMap<FString, int32> MemberIdx;
		for (int32 mi = 0; mi < Picked.Num(); mi++) MemberIdx.Add(BF6_LinkName(Picked[mi]), mi);

		// anchor: centroid in the plane, lowest Z - so placement lands on surfaces
		FVector Anchor = FVector::ZeroVector; double MinZ = DBL_MAX;
		for (AActor* A : Picked) { Anchor += A->GetActorLocation(); MinZ = FMath::Min(MinZ, (double)A->GetActorLocation().Z); }
		Anchor /= (double)Picked.Num(); Anchor.Z = MinZ;

		TArray<TSharedPtr<FJsonValue>> Objs;
		for (AActor* A : Picked)
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("type"), TagValue(A, TEXT("label:")));
			O->SetStringField(TEXT("mesh"), TagValue(A, TEXT("mesh:")));
			const FVector P = A->GetActorLocation() - Anchor;
			const FRotator R = A->GetActorRotation();
			const FVector S = A->GetActorScale3D();
			auto Arr = [](std::initializer_list<double> V){ TArray<TSharedPtr<FJsonValue>> A2; for (double d : V) A2.Add(MakeShared<FJsonValueNumber>(d)); return A2; };
			O->SetArrayField(TEXT("pos"),   Arr({ P.X, P.Y, P.Z }));
			O->SetArrayField(TEXT("rot"),   Arr({ R.Pitch, R.Yaw, R.Roll }));
			O->SetArrayField(TEXT("scale"), Arr({ S.X, S.Y, S.Z }));
			TArray<TSharedPtr<FJsonValue>> Props;
			for (const FName& T : A->Tags)
			{
				FString TS = T.ToString();
				if (!TS.StartsWith(TEXT("p:"))) continue;
				TS = TS.Mid(2);
				// link props: swap member names for "@index" so the link
				// survives placement
				int32 Eq = INDEX_NONE;
				if (TS.FindChar(TEXT('='), Eq))
				{
					const FString Key = TS.Left(Eq);
					if (BF6_IsLinkProp(TagValue(A, TEXT("label:")), Key))
					{
						TArray<FString> Parts;
						TS.Mid(Eq + 1).ParseIntoArray(Parts, TEXT(","));
						for (FString& Pt : Parts)
						{
							Pt = Pt.TrimStartAndEnd();
							if (const int32* Mi = MemberIdx.Find(Pt)) Pt = FString::Printf(TEXT("@%d"), *Mi);
						}
						TS = Key + TEXT("=") + FString::Join(Parts, TEXT(","));
					}
				}
				Props.Add(MakeShared<FJsonValueString>(TS));
			}
			if (Props.Num()) O->SetArrayField(TEXT("props"), Props);
			Objs.Add(MakeShared<FJsonValueObject>(O));
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("name"), Name);
		Root->SetStringField(TEXT("level"), g_ss.CurrentLevel);
		Root->SetArrayField(TEXT("objects"), Objs);
		FString Out;
		TSharedRef<TJsonWriter<>> Wr = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Wr);
		IFileManager::Get().MakeDirectory(*BF6_BlocksDir(), true);
		if (!FFileHelper::SaveStringToFile(Out, *BF6_BlockPath(Name))) return 0;
		// force the block's composite thumbnail to regenerate
		IFileManager::Get().Delete(*(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("thumbs")) / (TEXT("block_") + Name + TEXT(".png"))), false, false, true);
		// the source selection becomes an instance of the (possibly updated) block
		BF6_TagBlockInstance(Picked, Name, Anchor);
		// an instance behaves as ONE thing from the moment it's saved: group the
		// source objects unless they already share a group (a block focus edit
		// re-saving, for example, is already grouped)
		if (Picked.Num() > 1)
		{
			AGroupActor* GrpRoot = AGroupActor::GetRootForActor(Picked[0]);
			bool bGrouped = GrpRoot != nullptr;
			for (int32 i = 1; bGrouped && i < Picked.Num(); i++)
				bGrouped = AGroupActor::GetRootForActor(Picked[i]) == GrpRoot;
			if (!bGrouped)
			{
				if (!UActorGroupingUtils::IsGroupingActive()) UActorGroupingUtils::SetGroupingActive(true);
				UActorGroupingUtils::Get()->GroupActors(Picked);
			}
		}
		// Revit-style: every OTHER placed copy of this block refreshes to match
		const int32 Refreshed = BF6_RefreshBlockInstances(Name, Picked);
		if (Refreshed > 0)
			Notify(FString::Printf(TEXT("Updated %d other placed cop%s of block '%s'."),
				Refreshed, Refreshed == 1 ? TEXT("y") : TEXT("ies"), *Name));
		return Picked.Num();
	}

	static TSharedPtr<FJsonObject> BF6_LoadBlock(const FString& Name)
	{
		FString In;
		if (!FFileHelper::LoadFileToString(In, *BF6_BlockPath(Name))) return nullptr;
		TSharedPtr<FJsonObject> Root; TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
		return (FJsonSerializer::Deserialize(R, Root) && Root.IsValid()) ? Root : nullptr;
	}

	TArray<FBlockInfo> ListBlocks()
	{
		TArray<FBlockInfo> Out;
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(BF6_BlocksDir() / TEXT("*.json")), true, false);
		Files.Sort();
		for (const FString& F : Files)
		{
			const FString Name = FPaths::GetBaseFilename(F);
			if (TSharedPtr<FJsonObject> B = BF6_LoadBlock(Name))
			{
				FBlockInfo I; I.Name = Name;
				B->TryGetStringField(TEXT("level"), I.Level);
				const TArray<TSharedPtr<FJsonValue>>* Objs = nullptr;
				if (B->TryGetArrayField(TEXT("objects"), Objs)) I.Count = Objs->Num();
				Out.Add(I);
			}
		}
		return Out;
	}

	// Stamp a set of actors as one placed INSTANCE of a block, so a later
	// re-save of that block can find and refresh every copy on the map.
	static void BF6_TagBlockInstance(const TArray<AActor*>& Actors, const FString& Name, const FVector& Anchor)
	{
		const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Short);
		for (AActor* A : Actors)
		{
			// strip any previous block identity first
			for (int32 i = A->Tags.Num() - 1; i >= 0; i--)
			{
				const FString TS = A->Tags[i].ToString();
				if (TS.StartsWith(TEXT("blk:")) || TS.StartsWith(TEXT("blkid:")) || TS.StartsWith(TEXT("blkat:")))
					A->Tags.RemoveAt(i);
			}
			A->Tags.Add(FName(*(TEXT("blk:") + Name)));
			A->Tags.Add(FName(*(TEXT("blkid:") + Id)));
			A->Tags.Add(FName(*FString::Printf(TEXT("blkat:%f,%f,%f"), Anchor.X, Anchor.Y, Anchor.Z)));
		}
	}

	// Ctrl+D / paste copies a block instance's id along with its tags, so two
	// copies end up sharing one identity - editing or refreshing would then
	// treat all of them as a single mangled instance. Split them: within each
	// shared id, the actors under each group root are one copy (loose actors
	// are single-object copies); the first set keeps the id, every other set
	// is re-stamped with a fresh id and its own recomputed anchor.
	static void BF6_HealDuplicateBlockIds()
	{
		if (!GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		TMap<FString, TArray<AActor*>> ById;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			const FString Id = TagValue(*It, TEXT("blkid:"));
			if (!Id.IsEmpty()) ById.FindOrAdd(Id).Add(*It);
		}
		for (auto& P : ById)
		{
			TMap<AGroupActor*, TArray<AActor*>> ByRoot;
			TArray<TArray<AActor*>> Sets;
			for (AActor* A : P.Value)
			{
				if (AGroupActor* R = AGroupActor::GetRootForActor(A)) ByRoot.FindOrAdd(R).Add(A);
				else { TArray<AActor*> One; One.Add(A); Sets.Add(MoveTemp(One)); }
			}
			for (auto& G : ByRoot) Sets.Add(MoveTemp(G.Value));
			if (Sets.Num() <= 1) continue;
			for (int32 s = 1; s < Sets.Num(); s++)
			{
				const FString Name = TagValue(Sets[s][0], TEXT("blk:"));
				FVector Anchor = FVector::ZeroVector; double MinZ = DBL_MAX;
				for (AActor* A : Sets[s])
				{
					Anchor += A->GetActorLocation();
					MinZ = FMath::Min(MinZ, (double)A->GetActorLocation().Z);
				}
				Anchor /= (double)Sets[s].Num(); Anchor.Z = MinZ;
				BF6_TagBlockInstance(Sets[s], Name, Anchor);
			}
		}
	}

	// spawn a block's actors at WorldPos; no selection/group/notify side effects
	static bool BF6_SpawnBlockActors(const FString& Name, const FVector& WorldPos, TArray<AActor*>& OutSpawned)
	{
		TSharedPtr<FJsonObject> B = BF6_LoadBlock(Name);
		if (!B.IsValid()) return false;
		const TArray<TSharedPtr<FJsonValue>>* Objs = nullptr;
		if (!B->TryGetArrayField(TEXT("objects"), Objs) || Objs->Num() == 0) return false;
		auto Vec = [](const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const FVector& Def)
		{
			const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
			if (!O->TryGetArrayField(Key, A) || A->Num() < 3) return Def;
			return FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
		};
		// ByIndex mirrors the JSON object order (nullptr where a spawn was
		// skipped) so "@index" member links resolve even with gaps
		TArray<AActor*> ByIndex;
		for (const TSharedPtr<FJsonValue>& V : *Objs)
		{
			ByIndex.Add(nullptr);
			const TSharedPtr<FJsonObject> O = V->AsObject(); if (!O.IsValid()) continue;
			FString Ty, Ms;
			O->TryGetStringField(TEXT("type"), Ty);
			O->TryGetStringField(TEXT("mesh"), Ms);
			if (Ms.IsEmpty()) Ms = BF6_ResolveMeshForType(Ty);
			if (Ms.IsEmpty()) continue;
			const FVector P = Vec(O, TEXT("pos"), FVector::ZeroVector);
			const FVector R = Vec(O, TEXT("rot"), FVector::ZeroVector);
			const FVector S = Vec(O, TEXT("scale"), FVector::OneVector);
			AActor* A = SpawnSdkModel(Ms, Ty, FTransform(FRotator(R.X, R.Y, R.Z), WorldPos + P, S));
			if (!A) continue;
			const TArray<TSharedPtr<FJsonValue>>* Props = nullptr;
			if (O->TryGetArrayField(TEXT("props"), Props))
				for (const TSharedPtr<FJsonValue>& PV : *Props)
				{ FString PS; if (PV->TryGetString(PS)) A->Tags.Add(FName(*(TEXT("p:") + PS))); }
			ByIndex.Last() = A;
			OutSpawned.Add(A);
		}
		// decode "@index" member links to the fresh copies' names (a member
		// that failed to spawn drops out of the link instead of dangling)
		for (AActor* A : OutSpawned)
			for (int32 t = A->Tags.Num() - 1; t >= 0; t--)
			{
				const FString TS = A->Tags[t].ToString();
				if (!TS.StartsWith(TEXT("p:")) || !TS.Contains(TEXT("@"))) continue;
				int32 Eq = INDEX_NONE;
				if (!TS.FindChar(TEXT('='), Eq)) continue;
				const FString Key = TS.Mid(2, Eq - 2);
				if (!BF6_IsLinkProp(TagValue(A, TEXT("label:")), Key)) continue;
				TArray<FString> Parts;
				TS.Mid(Eq + 1).ParseIntoArray(Parts, TEXT(","));
				bool bChanged = false;
				for (int32 pi = Parts.Num() - 1; pi >= 0; pi--)
				{
					const FString Pt = Parts[pi].TrimStartAndEnd();
					if (!Pt.StartsWith(TEXT("@"))) { Parts[pi] = Pt; continue; }
					const int32 Mi = FCString::Atoi(*Pt.Mid(1));
					if (AActor* Target = ByIndex.IsValidIndex(Mi) ? ByIndex[Mi] : nullptr)
						Parts[pi] = BF6_LinkName(Target);
					else
						Parts.RemoveAt(pi);
					bChanged = true;
				}
				if (!bChanged) continue;
				A->Tags.RemoveAt(t);
				A->Tags.Add(FName(*(TS.Left(Eq + 1) + FString::Join(Parts, TEXT(",")))));
			}
		BF6_TagBlockInstance(OutSpawned, Name, WorldPos);
		for (AActor* A : OutSpawned) BF6_FileActor(A);   // into BF6 Blocks/<name>
		return OutSpawned.Num() > 0;
	}

	bool PlaceBlock(const FString& Name, const FVector& WorldPos)
	{
		if (!g_ss.bEditing) { BF6Api::RefuseReadOnly(TEXT("Objects can only be placed on a custom map. Name one and press Create, bottom right.")); return false; }
		TSharedPtr<FJsonObject> B = BF6_LoadBlock(Name);
		if (!B.IsValid()) { Notify(FString::Printf(TEXT("Block '%s' could not be read."), *Name)); return false; }
		FString Level; B->TryGetStringField(TEXT("level"), Level);
		if (!Level.IsEmpty() && Level != g_ss.CurrentLevel)
			Notify(FString::Printf(TEXT("Heads up: block '%s' was built for %s."), *Name, *DisplayName(Level)));

		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Place block %s"), *Name)));
		TArray<AActor*> Spawned;
		if (!BF6_SpawnBlockActors(Name, WorldPos, Spawned))
		{ Notify(FString::Printf(TEXT("Block '%s': none of its objects could be spawned."), *Name)); return false; }
		if (GEditor)
		{
			GEditor->SelectNone(false, true, false);
			for (AActor* A : Spawned) GEditor->SelectActor(A, true, false);
			GEditor->NoteSelectionChange();
			if (Spawned.Num() > 1) GroupSelection();   // arrives as one movable group
		}
		BF6_RecomputeBudget();
		Notify(FString::Printf(TEXT("Placed block '%s' (%d objects) - it's grouped; UNGROUP any time."), *Name, Spawned.Num()));
		return true;
	}

	// Refresh every placed instance of a block (except the ones in Keep, which
	// ARE the new definition): tear each copy down and respawn it at its stamped
	// anchor from the just-saved JSON. One undoable transaction.
	static int32 BF6_RefreshBlockInstances(const FString& Name, const TArray<AActor*>& Keep)
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		// duplicated copies sharing one instance id would be torn down together
		// and respawned as ONE copy - split them before collecting
		BF6_HealDuplicateBlockIds();
		TSet<FString> KeepIds;
		for (AActor* A : Keep) { const FString Id = TagValue(A, TEXT("blkid:")); if (!Id.IsEmpty()) KeepIds.Add(Id); }
		TMap<FString, TArray<AActor*>> Instances;
		const FName Want(*(TEXT("blk:") + Name));
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(Want)) continue;
			const FString Id = TagValue(*It, TEXT("blkid:"));
			if (Id.IsEmpty() || KeepIds.Contains(Id)) continue;
			Instances.FindOrAdd(Id).Add(*It);
		}
		if (Instances.Num() == 0) return 0;

		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Update block %s"), *Name)));
		int32 n = 0;
		for (auto& P : Instances)
		{
			FVector At = P.Value[0]->GetActorLocation();
			const FString AtS = TagValue(P.Value[0], TEXT("blkat:"));
			TArray<FString> Parts; AtS.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() == 3) At = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
			if (AGroupActor* G = AGroupActor::GetRootForActor(P.Value[0])) { G->Modify(); W->EditorDestroyActor(G, true); }
			for (AActor* A : P.Value) { A->Modify(); W->EditorDestroyActor(A, true); }
			TArray<AActor*> Fresh;
			if (BF6_SpawnBlockActors(Name, At, Fresh) && Fresh.Num() > 1)
			{
				if (!UActorGroupingUtils::IsGroupingActive()) UActorGroupingUtils::SetGroupingActive(true);
				UActorGroupingUtils::Get()->GroupActors(Fresh);
			}
			n++;
		}
		// hand the selection back to the definition the user was working on
		GEditor->SelectNone(false, true, false);
		for (AActor* A : Keep) if (IsValid(A)) GEditor->SelectActor(A, true, false);
		GEditor->NoteSelectionChange();
		BF6_RecomputeBudget();
		return n;
	}

	void DeleteBlock(const FString& Name)
	{
		IFileManager::Get().Delete(*BF6_BlockPath(Name), false, false, true);
		IFileManager::Get().Delete(*(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("thumbs")) / (TEXT("block_") + Name + TEXT(".png"))), false, false, true);
		// placed copies lose their block identity but KEEP their grouping: they
		// become plain groups instead of orphans pointing at a dead definition
		if (!GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		const FName Want(*(TEXT("blk:") + Name));
		TSet<FString> Ids;
		TArray<AActor*> Tagged;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(Want)) Tagged.Add(*It);
		if (Tagged.Num() == 0) return;
		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Delete block %s"), *Name)));
		for (AActor* A : Tagged)
		{
			const FString Id = TagValue(A, TEXT("blkid:"));
			if (!Id.IsEmpty()) Ids.Add(Id);
			A->Modify();
			for (int32 i = A->Tags.Num() - 1; i >= 0; i--)
			{
				const FString TS = A->Tags[i].ToString();
				if (TS.StartsWith(TEXT("blk:")) || TS.StartsWith(TEXT("blkid:")) || TS.StartsWith(TEXT("blkat:")))
					A->Tags.RemoveAt(i);
			}
		}
		Notify(FString::Printf(TEXT("Block '%s' deleted - %d placed cop%s on the map converted to plain group%s."),
			*Name, Ids.Num(), Ids.Num() == 1 ? TEXT("y") : TEXT("ies"), Ids.Num() == 1 ? TEXT("") : TEXT("s")));
	}

	void OpenBlocksFolder()
	{
		IFileManager::Get().MakeDirectory(*BF6_BlocksDir(), true);
		BF6_OpenInExplorer(BF6_BlocksDir(), false);
	}

	// one folder per custom map, the level file inside - the folder is what
	// you back up or share
	// ---- the map-image decal ----------------------------------------------
	//
	// The Godot SDK's terrain_decal plugin, matched: the same top-down map
	// image from the same CDN ("<downloadUrl>maptiles/<level>.jpg", which
	// answers only to a browser User-Agent), placed with the same per-map box
	// from the SDK's own bounds.json, draped over the low-poly terrain and
	// assets. It is a real actor: hide it, or grab it with the gizmo and shift
	// it to realign, exactly like moving the Decal node in Godot. The adjusted
	// transform is remembered per map.
	static const FName kMapDecalTag("BF6MapDecal");
	static bool GMapDecalFetching = false;

	static AActor* BF6_FindMapDecal()
	{
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) return nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(kMapDecalTag)) return *It;
		return nullptr;
	}

	static FString BF6_MapDecalCachePath(const FString& Level)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("decals"), Level + TEXT(".jpg"));
	}

	// the image, wherever it already is: the SDK's own download cache first,
	// then ours. Empty means it needs fetching.
	static FString BF6_MapDecalImageOnDisk(const FString& Level)
	{
		const FString Sdk = StoredSdkRoot();
		if (!Sdk.IsEmpty())
		{
			const FString Theirs = Sdk / TEXT("GodotProject/addons/bf_portal/terrain_decal/textures") / (Level + TEXT(".jpg"));
			if (FPaths::FileExists(Theirs)) return Theirs;
		}
		const FString Ours = BF6_MapDecalCachePath(Level);
		return FPaths::FileExists(Ours) ? Ours : FString();
	}

	// the per-map box, from the SDK's own bounds.json (Godot metres)
	static bool BF6_MapDecalBounds(const FString& Level, FVector& OutSizeM, FVector& OutPosM)
	{
		OutSizeM = FVector(1000, 500, 1000); OutPosM = FVector::ZeroVector;
		const FString Sdk = StoredSdkRoot();
		if (Sdk.IsEmpty()) return false;
		FString In;
		if (!FFileHelper::LoadFileToString(In, *(Sdk / TEXT("GodotProject/addons/bf_portal/terrain_decal/bounds.json")))) return false;
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
		if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return false;
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (!Root->TryGetObjectField(Level, E) && !Root->TryGetObjectField(TEXT("_default"), E)) return false;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if ((*E)->TryGetArrayField(TEXT("size"), Arr) && Arr->Num() == 3)
			OutSizeM = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		if ((*E)->TryGetArrayField(TEXT("position"), Arr) && Arr->Num() == 3)
			OutPosM = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}

	// runtime decal material, built the way the volume material is built - the
	// proven pattern: a real /Temp package (transient-package materials
	// half-register and fail to compile) and UMaterialEditingLibrary wiring.
	static UMaterialInstanceDynamic* BF6_MapDecalMID(UTexture2D* Tex, UObject* Outer)
	{
		static TWeakObjectPtr<UMaterial> GParent;
		UMaterial* Parent = GParent.Get();
		if (!Parent)
		{
			UPackage* Pkg = CreatePackage(TEXT("/Temp/BF6MapDecal"));
			if (!Pkg) return nullptr;
			Pkg->SetFlags(RF_Transient);
			Parent = NewObject<UMaterial>(Pkg, TEXT("M_BF6MapDecal"), RF_Transient);
			if (!Parent) return nullptr;
			Parent->MaterialDomain = MD_DeferredDecal;
			Parent->BlendMode = BLEND_Translucent;
			UMaterialExpressionTextureSampleParameter2D* T =
				Cast<UMaterialExpressionTextureSampleParameter2D>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						Parent, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, 0));
			if (T)
			{
				T->ParameterName = TEXT("Image");
				T->SamplerType = SAMPLERTYPE_Color;
				UMaterialEditingLibrary::ConnectMaterialProperty(T, TEXT("RGB"), MP_BaseColor);
			}
			UMaterialExpressionScalarParameter* O =
				Cast<UMaterialExpressionScalarParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(
						Parent, UMaterialExpressionScalarParameter::StaticClass(), -400, 200));
			if (O)
			{
				O->ParameterName = TEXT("Opacity");
				O->DefaultValue = 0.9f;
				UMaterialEditingLibrary::ConnectMaterialProperty(O, TEXT(""), MP_Opacity);
			}
			Parent->PreEditChange(nullptr);
			Parent->PostEditChange();
			Parent->AddToRoot();
			GParent = Parent;
		}
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Parent, Outer);
		if (Mid && Tex) Mid->SetTextureParameterValue(TEXT("Image"), Tex);
		return Mid;
	}

	static FString BF6_MapDecalConfigKey(const FString& Level) { return TEXT("Decal_") + Level; }

	static void BF6_MapDecalSpawn(const FString& Level, const FString& ImagePath)
	{
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!W) return;
		UTexture2D* Tex = FImageUtils::ImportFileAsTexture2D(ImagePath);
		if (!Tex) { Notify(TEXT("Could not read the map image.")); return; }
		Tex->AddToRoot();

		FVector SizeM, PosM;
		BF6_MapDecalBounds(Level, SizeM, PosM);

		ADecalActor* A = W->SpawnActor<ADecalActor>(ADecalActor::StaticClass());
		if (!A) return;
		A->SetFlags(RF_Transient);
		A->Tags.Add(kContextTag);   // never saved, never exported, cleared with the map
		A->Tags.Add(kMapDecalTag);
		BF6_SetPrettyLabel(A, FString::Printf(TEXT("%s_MapImage"), *Level));

		UDecalComponent* D = A->GetDecal();
		D->SetDecalMaterial(BF6_MapDecalMID(Tex, D));
		// godot metres -> our cm, godot (x, y-up, z) -> ours (x, z, y). The
		// decal projects along its local X; pitched down, local Z spans world
		// X and local Y spans world Y. Half-extents.
		D->DecalSize = FVector(SizeM.Y * 50.f, SizeM.Z * 50.f, SizeM.X * 50.f);
		FTransform Xf(FRotator(-90, 0, 0), FVector(PosM.X, PosM.Z, PosM.Y) * 100.f);
		// a saved realignment wins over the book position
		FString Saved;
		if (GConfig->GetString(TEXT("BF6UnrealSDK"), *BF6_MapDecalConfigKey(Level), Saved, GEditorPerProjectIni) && !Saved.IsEmpty())
			Xf.InitFromString(Saved);
		A->SetActorTransform(Xf);
		BF6_Redraw();
	}

	// remember where the creator shifted it, per map
	static void BF6_MapDecalStash()
	{
		AActor* A = BF6_FindMapDecal();
		if (!A || g_ss.CurrentLevel.IsEmpty()) return;
		GConfig->SetString(TEXT("BF6UnrealSDK"), *BF6_MapDecalConfigKey(g_ss.CurrentLevel),
			*A->GetActorTransform().ToString(), GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	int32 MapDecalState()
	{
		if (GMapDecalFetching) return 2;
		AActor* A = BF6_FindMapDecal();
		if (!A) return 0;
		return A->IsTemporarilyHiddenInEditor() ? 1 : 3;
	}

	void ToggleMapDecal()
	{
		const FString Level = g_ss.CurrentLevel;
		if (Level.IsEmpty() || GMapDecalFetching) return;

		if (AActor* A = BF6_FindMapDecal())
		{
			const bool bHide = !A->IsTemporarilyHiddenInEditor();
			A->SetIsTemporarilyHiddenInEditor(bHide);
			BF6_MapDecalStash();
			BF6_Redraw();
			return;
		}

		const FString Have = BF6_MapDecalImageOnDisk(Level);
		if (!Have.IsEmpty()) { BF6_MapDecalSpawn(Level, Have); return; }

		// fetch it the way the SDK does, from the same place - the CDN answers
		// only to a browser User-Agent, which the SDK-download code already
		// carries for exactly this host
		const FString Url = FString::Printf(TEXT("https://download.portal.battlefield.com/maptiles/%s.jpg"), *Level);
		GMapDecalFetching = true;
		Notify(FString::Printf(TEXT("Downloading the %s map image..."), *BF6Api::DisplayName(Level)));
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
		Req->SetURL(Url);
		Req->SetVerb(TEXT("GET"));
		Req->SetHeader(TEXT("User-Agent"), TEXT("Mozilla/5.0 (Windows NT 10.0; Win64; x64)"));
		Req->OnProcessRequestComplete().BindLambda([Level](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			GMapDecalFetching = false;
			// the CDN's "not found" is a tiny 200 text page, so the content
			// type is the real answer, exactly as the SDK's own plugin checks
			if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() != 200
				|| !Resp->GetContentType().StartsWith(TEXT("image")))
			{
				Notify(TEXT("No map image is published for this level."));
				return;
			}
			const FString Out = BF6_MapDecalCachePath(Level);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Out), true);
			if (!FFileHelper::SaveArrayToFile(Resp->GetContent(), *Out))
			{
				Notify(TEXT("Could not save the map image."));
				return;
			}
			if (g_ss.CurrentLevel == Level) BF6_MapDecalSpawn(Level, Out);
		});
		Req->ProcessRequest();
	}

	void OpenSavesFolder()
	{
		IFileManager::Get().MakeDirectory(*BF6_SavesRoot(), true);
		BF6_OpenInExplorer(BF6_SavesRoot(), false);
	}

	// the answer to "where did my export go" - one click, Explorer opens on
	// the folder every .spatial.json lands in
	void OpenExportsFolder()
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("export"));
		IFileManager::Get().MakeDirectory(*Dir, true);
		BF6_OpenInExplorer(Dir, false);
	}

	const FSlateBrush* GetBlockThumb(const FString& Name)
	{
		// same cache/queue as model thumbs, namespaced by the block:: key
		return GetModelThumb(TEXT("block::") + Name);
	}

	// ---- lint: offline validation ----
	// Every rule here is a community-verified law whose violation otherwise
	// costs a full export-upload-host-test cycle to discover. Severity:
	// 0 = problem (will not work / will not upload), 1 = warning, 2 = advice.
	bool ReverseVolumeWinding(AActor* Vol)
	{
		TArray<FVector>* Lp = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!Lp || Lp->Num() < 3) return false;
		FScopedTransaction Tx(FText::FromString(TEXT("Reverse volume winding")));
		Vol->Modify();
		for (int32 i = 0, j = Lp->Num() - 1; i < j; i++, j--) Lp->Swap(i, j);
		BF6_WriteLoopTags(Vol);
		RebuildVolumeWalls(Vol, *Lp);
		return true;
	}

	// What the scene tree hangs its warning badges on. Re-running the whole lint
	// per row per frame is out of the question on a two thousand object map, so
	// the result is cached by actor and refreshed on a timer (or whenever the
	// Validate panel runs, which fills this on its way past).
	struct FLintMark { uint8 Severity = 2; FString Message; };
	static TMap<TWeakObjectPtr<AActor>, FLintMark> GLintMarks;
	static double GLintStamp = 0.0;

	static void BF6_CacheLint(const TArray<FLintItem>& Items)
	{
		GLintMarks.Reset();
		for (const FLintItem& I : Items)
		{
			AActor* A = I.Actor.Get();
			if (!A || I.Severity > 1) continue;   // advisories stay out of the tree
			FLintMark& M = GLintMarks.FindOrAdd(A);
			if (I.Severity <= M.Severity) { M.Severity = I.Severity; M.Message = I.Message; }
		}
		GLintStamp = FPlatformTime::Seconds();

		// Say what was found, once per change. A creator seeing badges appear on a
		// fresh import needs to know WHICH check fired without hovering rows one by
		// one, and it is the same line we need to read a bug report from.
		TMap<FString, int32> ByMessage;
		for (const FLintItem& I : Items)
			if (I.Severity <= 1) ByMessage.FindOrAdd(I.Message)++;
		FString Sig = FString::FromInt(Items.Num());
		for (const TPair<FString, int32>& KV : ByMessage) Sig += FString::Printf(TEXT("|%s=%d"), *KV.Key, KV.Value);
		static FString LastSig;
		if (Sig == LastSig) return;
		LastSig = Sig;
		if (ByMessage.Num() == 0) { UE_LOG(LogBF6, Display, TEXT("validate: all clear")); return; }
		ByMessage.ValueSort([](int32 A, int32 B){ return A > B; });
		UE_LOG(LogBF6, Warning, TEXT("validate: %d flagged object(s), %d distinct issue(s):"), GLintMarks.Num(), ByMessage.Num());
		int32 Shown = 0;
		for (const TPair<FString, int32>& KV : ByMessage)
		{
			UE_LOG(LogBF6, Warning, TEXT("   x%-4d %s"), KV.Value, *KV.Key);
			if (++Shown >= 12) break;
		}
	}

	bool LintMarkFor(AActor* A, uint8& OutSeverity, FString& OutMessage)
	{
		if (const FLintMark* M = GLintMarks.Find(A))
		{
			OutSeverity = M->Severity;
			OutMessage = M->Message;
			return true;
		}
		return false;
	}

	void RefreshLintIfStale(double MaxAgeSeconds)
	{
		if (FPlatformTime::Seconds() - GLintStamp < MaxAgeSeconds) return;
		GLintStamp = FPlatformTime::Seconds();   // stamp first: RunLint is not free
		const double T0 = FPlatformTime::Seconds();
		RunLint();
		const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
		if (Ms > 100.0) UE_LOG(LogBF6, Warning, TEXT("validate sweep took %.0f ms"), Ms);
	}

	TArray<FLintItem> RunLint()
	{
		TArray<FLintItem> Out;
		ON_SCOPE_EXIT{ BF6_CacheLint(Out); };   // the scene tree reads its badges off this
		if (!GEditor) return Out;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return Out;
		auto Add = [&Out](uint8 Sev, AActor* A, const FString& Msg, bool bWindingFix = false)
		{
			FLintItem I; I.Severity = Sev; I.Actor = A; I.Message = Msg; I.bWindingFix = bWindingFix;
			Out.Add(MoveTemp(I));
		};

		TArray<AActor*> Ours;
		TMap<FString, AActor*> ByName;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)) continue;
			Ours.Add(*It);
			ByName.Add(BF6_LinkName(*It), *It);
		}

		// resolve a link prop's comma name-list to actors; legacy NodePath
		// values from base imports can't be resolved and are skipped quietly
		auto LinkTargets = [&ByName](AActor* A, const TCHAR* Prop, TArray<AActor*>& OutT, bool& bLegacy)
		{
			OutT.Reset(); bLegacy = false;
			const FString V = GetActorProp(A, Prop);
			if (V.IsEmpty()) return;
			if (V.Contains(TEXT("NodePath")) || V.Contains(TEXT("ExtResource"))) { bLegacy = true; return; }
			TArray<FString> Parts;
			V.ParseIntoArray(Parts, TEXT(","));
			for (FString& P : Parts)
				if (AActor* const* T = ByName.Find(P.TrimStartAndEnd())) OutT.Add(*T);
		};
		// ---- will the game read this, and is it listed for this map? ----
		// Keyed on TYPE and nothing else. DICE reshuffles which folder an object
		// lives in between SDK releases, and an object that moved folder - or that
		// dropped off a map's list - usually still loads on that map. So a type the
		// catalogue knows but this level does not list is a WARNING to go and check
		// in game, never a failure. Only a type the catalogue has never heard of is
		// treated as broken, and then only for objects placed from the library,
		// where the catalogue is the authority on what exists.
		{
			TSet<FString> LevelTypes, AllTypes;
			for (const TSharedPtr<FPlaceableRow>& r : g_ss.AllItems) if (r.IsValid()) LevelTypes.Add(r->Type);
			for (const TSharedPtr<FPlaceableRow>& r : g_allGlobal)  if (r.IsValid()) AllTypes.Add(r->Type);
			if (AllTypes.Num() > 0)
				for (AActor* A : Ours)
				{
					FString Ty = TagValue(A, TEXT("type:"));
					if (Ty.IsEmpty()) Ty = TagValue(A, TEXT("label:"));
					if (Ty.IsEmpty()) continue;
					const bool bFromLibrary = !TagValue(A, TEXT("mesh:")).IsEmpty();
					if (!AllTypes.Contains(Ty))
					{
						if (bFromLibrary)
							Add(0, A, FString::Printf(TEXT("'%s' is not in this SDK's catalogue at all - the game has nothing to load for it. It may have been renamed or removed in an SDK update."), *Ty));
					}
					else if (!LevelTypes.Contains(Ty))
						Add(1, A, FString::Printf(TEXT("'%s' is not listed for %s in this SDK release. It usually still loads - objects move between folders and map lists between releases - but check it in game before shipping."), *Ty, *g_ss.CurrentLevel));
				}
		}

		auto VolHeight = [](AActor* V) { const FString H = TagValue(V, TEXT("p:height=")); return H.IsEmpty() ? 0.0 : FCString::Atod(*H); };
		auto LoopOf = [](AActor* Vol, TArray<FVector>& OutLoop)
		{
			const TArray<FVector>* Lp = GVolumeLoops.Find(Vol);
			if (!Lp || Lp->Num() < 3) return false;
			OutLoop = BF6_LoopToWorld(Vol, *Lp);
			return true;
		};
		auto PointInPoly = [](const FVector& P, const TArray<FVector>& Loop)
		{
			bool bIn = false;
			for (int32 i = 0, j = Loop.Num() - 1; i < Loop.Num(); j = i++)
			{
				if (((Loop[i].Y > P.Y) != (Loop[j].Y > P.Y)) &&
					(P.X < (Loop[j].X - Loop[i].X) * (P.Y - Loop[i].Y) / (Loop[j].Y - Loop[i].Y) + Loop[i].X))
					bIn = !bIn;
			}
			return bIn;
		};

		// the map's combat polygon, reused by the HQ-inside rule below
		TArray<FVector> CombatLoop;

		for (AActor* A : Ours)
		{
			const FString Ty = BF6_ObjIdType(A);
			TArray<AActor*> T; bool bLegacy = false;

			// gdconverter's REQUIRED_PROPS: a RingOfFire with either shape unset
			// is refused at conversion, so it is a fault here, not advice.
			if (Ty == TEXT("RingOfFire"))
			{
				if (GetActorProp(A, TEXT("HardRestrictOBB")).TrimStartAndEnd().IsEmpty())
					Add(0, A, TEXT("Ring of fire has no HardRestrictOBB volume - the SDK refuses to convert the map without one."));
				if (GetActorProp(A, TEXT("RestrictShapeData")).TrimStartAndEnd().IsEmpty())
					Add(0, A, TEXT("Ring of fire has no RestrictShapeData volume - the SDK refuses to convert the map without one."));
			}

			if (Ty == TEXT("CombatArea"))
			{
				LinkTargets(A, TEXT("CombatVolume"), T, bLegacy);
				// Advisory, not a warning. The SDK's own CombatArea.gd exports
				// CombatVolume as optional and its _get_configuration_warnings()
				// says nothing when it is empty - it only checks the area limit.
				// Shipped maps do link one 91 times out of 95, so it is worth
				// mentioning, but it is not a fault and earns no badge.
				if (T.Num() == 0 && !bLegacy)
					Add(2, A, TEXT("Combat area has no combat volume linked. Most maps link one; check this is deliberate."));
				for (AActor* Vol : T)
				{
					TArray<FVector> Loop;
					if (!LoopOf(Vol, Loop)) continue;
					CombatLoop = Loop;
					// The SDK's own hard limit: gdconverter refuses a combat
					// volume whose area exceeds 16,640,000 (Godot metres
					// squared). Ours is in cm, so the shoelace result divides
					// by 100^2. Finding this at upload after shaping a huge
					// zone is the worst possible time.
					{
						double A2 = 0.0;
						for (int32 i = 0; i < Loop.Num(); i++)
						{
							const FVector& a = Loop[i]; const FVector& b = Loop[(i + 1) % Loop.Num()];
							A2 += a.X * b.Y - b.X * a.Y;
						}
						const double AreaM2 = FMath::Abs(A2) * 0.5 / 10000.0;
						if (AreaM2 > 16640000.0)
							Add(0, Vol, FString::Printf(TEXT("Combat volume covers %.0f km2 - over the SDK's %.1f km2 limit, and the converter rejects the map."), AreaM2 / 1e6, 16.64));
					}
					// winding law: the game wants CLOCKWISE in Godot's XZ, which
					// is our X (east) / Y (as Godot Z). CCW turns the zone
					// inside out - everything OUTSIDE becomes playable.
					double S = 0.0, Area2 = 0.0;
					for (int32 i = 0; i < Loop.Num(); i++)
					{
						const FVector& P1 = Loop[i];
						const FVector& P2 = Loop[(i + 1) % Loop.Num()];
						const double x1 = P1.X / 100.0, z1 = P1.Y / 100.0;
						const double x2 = P2.X / 100.0, z2 = P2.Y / 100.0;
						S += (x2 - x1) * (z2 + z1);
						Area2 += x1 * z2 - x2 * z1;
					}
					if (S <= 0.0)
						Add(1, Vol, TEXT("Combat volume looks counter-clockwise - in game that makes everything OUTSIDE it playable. Fix reverses the point order."), true);
					const double Area = FMath::Abs(Area2) * 0.5;
					if (Area > 16640000.0)
						Add(0, Vol, FString::Printf(TEXT("Combat volume area is %.0f - over the 16,640,000 limit, the SDK exporter refuses files like this."), Area));
				}
			}
			else if (Ty == TEXT("HQ_PlayerSpawner") || Ty == TEXT("PlayerSpawner"))
			{
				const TCHAR* SpawnProp = Ty == TEXT("HQ_PlayerSpawner") ? TEXT("InfantrySpawns") : TEXT("SpawnPoints");
				LinkTargets(A, SpawnProp, T, bLegacy);
				if (T.Num() == 0 && !bLegacy)
					Add(0, A, TEXT("Spawner has no spawn points linked - players get 'deployment unavailable'."));
				else if (T.Num() > 0 && T.Num() < 4)
					Add(2, A, FString::Printf(TEXT("Only %d spawn point%s linked - 4 or more, spread out, avoids the deploy-availability bug."), T.Num(), T.Num() == 1 ? TEXT("") : TEXT("s")));
				if (Ty == TEXT("HQ_PlayerSpawner"))
				{
					TArray<AActor*> Hq; bool bHqLegacy = false;
					LinkTargets(A, TEXT("HQArea"), Hq, bHqLegacy);
					if (Hq.Num() == 0 && !bHqLegacy)
						Add(1, A, TEXT("HQ has no HQArea volume - each HQ needs its own protected area."));
				}
			}
			else if (Ty == TEXT("CapturePoint"))
			{
				LinkTargets(A, TEXT("CaptureArea"), T, bLegacy);
				if (T.Num() == 0 && !bLegacy)
					Add(0, A, TEXT("Capture point has no capture area - the flag can never be taken."));
				for (AActor* Vol : T)
				{
					// Season 4: height 0 = INFINITE, which is valid (and
					// common). Only a tiny NONZERO height is suspicious - it
					// registers almost nobody.
					const double H = VolHeight(Vol);
					if (H > 0.01 && H < 0.5)
						Add(2, Vol, TEXT("Capture area height is nearly zero - players will barely register. Set a real height, or 0 for infinite (Season 4)."));
				}
				for (const TCHAR* Team : { TEXT("InfantrySpawnPoints_Team1"), TEXT("InfantrySpawnPoints_Team2") })
				{
					TArray<AActor*> Sp; bool bSpLegacy = false;
					LinkTargets(A, Team, Sp, bSpLegacy);
					if (Sp.Num() > 0 && Sp.Num() < 4)
						Add(2, A, FString::Printf(TEXT("%s has only %d spawn point%s - 4 or more, spread out, is the safe pattern."), Team, Sp.Num(), Sp.Num() == 1 ? TEXT("") : TEXT("s")));
				}
			}
			else if (Ty == TEXT("AreaTrigger"))
			{
				LinkTargets(A, TEXT("Area"), T, bLegacy);
				for (AActor* Vol : T)
				{
					const double H = VolHeight(Vol);
					if (H > 0.01 && H < 0.5)
						Add(2, Vol, TEXT("Area trigger height is nearly zero - it will barely fire. Set a real height, or 0 for infinite (Season 4)."));
				}
			}
			else if (Ty == TEXT("Sector"))
			{
				TArray<AActor*> Cp, Mc; bool bL1 = false, bL2 = false;
				LinkTargets(A, TEXT("CapturePoints"), Cp, bL1);
				LinkTargets(A, TEXT("MCOMs"), Mc, bL2);
				if (Cp.Num() == 0 && Mc.Num() == 0 && !bL1 && !bL2)
					Add(1, A, TEXT("Sector owns no capture points or MCOMs - without a sector, every flag shows as 'A'."));
			}

			// non-uniform scale desyncs collision from the visual mesh
			if (!TagValue(A, TEXT("mesh:")).IsEmpty())
			{
				const FVector Sc = A->GetActorScale3D();
				const double Mx = FMath::Max3(Sc.X, Sc.Y, Sc.Z), Mn = FMath::Min3(Sc.X, Sc.Y, Sc.Z);
				if (Mn > 0.0 && Mx / Mn > 1.01)
					Add(1, A, FString::Printf(TEXT("Non-uniform scale (%.2f, %.2f, %.2f). The shipped maps almost never do this (2 objects in 380) and collision can disagree with the visual - check it in game."), Sc.X, Sc.Y, Sc.Z));
			}
		}

		// No check on where an HQ sits relative to the combat volume. The inside
		// of that volume IS the play area, and DICE's own shipped setups put HQs
		// on both sides of it - 5 of 16 are inside (Abbasid and Aftermath put both
		// there; Badlands and Battery keep them out). It is a design choice, not a
		// defect, and flagging it told creators their working map was broken.

		// duplicate ObjIds break scripts silently. EA's own base setups reuse
		// ids ACROSS types (a DeployCam 1 next to an HQ 1), so cross-type reuse
		// is only advice; the same id on two objects of the SAME type is the
		// real problem.
		{
			TArray<FObjIdRow> Ids = GatherObjIds();
			TMap<int32, TArray<FObjIdRow*>> ByIdMap;
			for (FObjIdRow& R : Ids) if (R.Id >= 0) ByIdMap.FindOrAdd(R.Id).Add(&R);
			for (auto& P : ByIdMap)
			{
				if (P.Value.Num() < 2) continue;
				TSet<FString> Types;
				for (FObjIdRow* R : P.Value) Types.Add(R->Type);
				const bool bSameType = Types.Num() < P.Value.Num();
				Add(bSameType ? (uint8)0 : (uint8)2, P.Value[0]->Actor.Get(), bSameType
					? FString::Printf(TEXT("ObjId %d is used by %d objects of the same type - scripts can't tell them apart. Fix it in OBJECT IDS."), P.Key, P.Value.Num())
					: FString::Printf(TEXT("ObjId %d is shared by %d objects of different types. The base maps do this too, but unique ids everywhere are safer for scripts."), P.Key, P.Value.Num()));
			}
		}

		// upload size: the site rejects a per-map file over the limit outright,
		// so a map that would bounce is a problem, and one near the line is a warning
		if (g_upBytes > 0 && g_limPerMap > 0)
		{
			if (g_upBytes > g_limPerMap)
				Add(0, nullptr, FString::Printf(TEXT("This map exports to %lld KB minified - over the %lld KB per-map upload limit, the Portal site rejects it. Remove detail or split the map."), g_upBytes / 1024, g_limPerMap / 1024));
			else if (g_upBytes > (int64)(g_limPerMap * 0.9))
				Add(1, nullptr, FString::Printf(TEXT("This map is %lld KB minified, close to the %lld KB per-map limit. Not much room left."), g_upBytes / 1024, g_limPerMap / 1024));
		}

		// problems first, then warnings, then advice
		Out.StableSort([](const FLintItem& A, const FLintItem& B){ return A.Severity < B.Severity; });
		return Out;
	}

	// ---- mode setup wizard ----
	// Guided point-and-place scaffolding for Conquest and Breakthrough: the
	// panel explains every step with "Step N of M", each click builds a fully
	// linked bundle (HQ + area + spawns, flag + capture area + spawns), and
	// the finish wires the Sector and runs the checks. ObjIds follow the
	// community's established ranges (flags 200+ with A=200, HQs 300s,
	// sectors 100s) so existing script templates line up.
	struct FBF6Wiz
	{
		bool bActive = false;
		bool bConquest = true;
		int32 Count = 3;                 // flags (conquest) or sectors (breakthrough)
		int32 Step = 0, Total = 0;
		TArray<FString> FlagNames;       // conquest: link names of placed flags
		TArray<FVector> FlagPos;
		TArray<FString> SectorCp;        // breakthrough: current sector's objectives
		TArray<FVector> SectorCpPos;
	};
	static FBF6Wiz GWiz;

	bool IsModeWizardActive() { return GWiz.bActive; }
	int32 ModeWizardStep()    { return GWiz.Step + 1; }
	int32 ModeWizardTotal()   { return GWiz.Total; }

	static FString BF6_WizFlagLetter(int32 i) { return FString::Chr((TCHAR)('A' + i)); }

	FString ModeWizardTitle()
	{
		if (!GWiz.bActive) return FString();
		if (GWiz.bConquest)
		{
			if (GWiz.Step == 0) return TEXT("Place the Team 1 HQ");
			if (GWiz.Step == 1) return TEXT("Place the Team 2 HQ");
			return FString::Printf(TEXT("Place flag %s"), *BF6_WizFlagLetter(GWiz.Step - 2));
		}
		const int32 s = GWiz.Step / 4 + 1, sub = GWiz.Step % 4;
		switch (sub)
		{
		case 0: return FString::Printf(TEXT("Sector %d: place the attacker HQ (Team 1)"), s);
		case 1: return FString::Printf(TEXT("Sector %d: place the defender HQ (Team 2)"), s);
		case 2: return FString::Printf(TEXT("Sector %d: place objective A"), s);
		default: return FString::Printf(TEXT("Sector %d: place objective B"), s);
		}
	}

	// What this step is FOR, what a good placement looks like, and what happens
	// when you click.
	//
	// The first cut said "Aim at the ground and click", which the title already
	// said. Someone who has never built a Portal mode does not need the verb
	// repeated - they need to know that an HQ is where a team comes back in
	// after dying, that putting the two of them within sight of each other is
	// the classic spawn-camping map, and that the thing they are about to click
	// is a dozen linked objects they can pull apart afterwards.
	FString ModeWizardBody()
	{
		if (!GWiz.bActive) return FString();

		const bool bHQ = GWiz.bConquest ? (GWiz.Step <= 1) : (GWiz.Step % 4 <= 1);
		if (bHQ)
		{
			const bool bFirst = GWiz.bConquest ? (GWiz.Step == 0) : (GWiz.Step % 4 == 0);
			return FString::Printf(TEXT(
				"An HQ is where this team comes back in after dying, and the one place they cannot be shot while doing it.%s\n\n"
				"Put it behind the team's side of the map, with cover between it and the fighting. The two HQs facing each other across open ground is how a map ends up as a spawn-camp.\n\n"
				"One click builds the HQ, a protected area around it, and four spawn points inside that area, all linked. They are ordinary objects afterwards - move them, reshape the area, add more spawns."),
				bFirst ? TEXT("") : TEXT(" This is the second one, so keep it well away from the first."));
		}

		if (GWiz.bConquest)
			return TEXT(
				"A flag is a place worth fighting over, so it wants cover, more than one way in, and no single window that owns it.\n\n"
				"Spread flags out: players walk between them, and two flags close together turn the map into one fight instead of several.\n\n"
				"One click builds the capture point, its capture area, and eight spawn points split between the two teams and facing the flag. When every flag is down, the sector is wired up for you and the checks run.");

		return TEXT(
			"An objective is what the attackers have to take before the sector moves on. Two per sector, far enough apart that one defensive position cannot hold both.\n\n"
			"One click builds the capture point, its area, and spawns for both teams. When both are down this sector is wired and the next one begins.");
	}

	// The actor a link name refers to, or null.
	static AActor* BF6_FindByLinkName(const FString& Name)
	{
		if (Name.IsEmpty() || !GEditor) return nullptr;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return nullptr;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			FString Nm = It->GetActorLabel();
			Nm.RemoveFromStart(TEXT("BF6_"));
			if (Nm == Name) return *It;
		}
		return nullptr;
	}

	// The next free ObjId in a band.
	//
	// The community's script templates address objects by id and expect the
	// established bands - flags from 200, HQs in the 300s, sectors in the 100s -
	// so a bundle dropped on its own has to land in the band the wizard would
	// have used, and must not collide with one already there.
	static int32 BF6_NextObjId(int32 Base, int32 Span)
	{
		int32 Best = Base - 1;
		if (GEditor)
			if (UWorld* W = GEditor->GetEditorWorldContext().World())
				for (TActorIterator<AActor> It(W); It; ++It)
				{
					const FString V = TagValue(*It, TEXT("p:ObjId="));
					if (V.IsEmpty()) continue;
					const int32 Id = FCString::Atoi(*V);
					if (Id >= Base && Id < Base + Span) Best = FMath::Max(Best, Id);
				}
		return Best + 1;
	}

	static FString BF6_ActorLinkName(AActor* A)
	{
		FString Nm = A->GetActorLabel();
		Nm.RemoveFromStart(TEXT("BF6_"));
		return Nm;
	}

	static AActor* BF6_WizSpawn(const FString& Type, const FVector& Pos, double YawDeg, const FString& Label)
	{
		const FTransform Xf(FRotator(0, YawDeg, 0), Pos, FVector::OneVector);
		const FString Mesh = BF6_ResolveMeshForType(Type);
		AActor* A = Mesh.IsEmpty() ? nullptr : SpawnSdkModel(Mesh, Type, Xf);
		if (!A)
		{
			UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr; if (!W) return nullptr;
			A = W->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (!A) return nullptr;
			UProceduralMeshComponent* MM = MakeProcMesh(A, TEXT("Model"));
			BuildMarker(MM);
			A->SetActorTransform(Xf);
			A->Tags.Add(kPlacedTag);
			A->Tags.Add(FName(*(FString(TEXT("label:")) + Type)));
			A->SetFlags(RF_Transient);
			// A type with no bundled model still belongs in the tree. Missing
			// this is why a placed flag appeared in the world and nowhere in
			// the scene tree.
			BF6_FileActor(A);
		}
		BF6_SetPrettyLabel(A, Label);
		return A;
	}

	static AActor* BF6_WizSquareVolume(const FVector& Center, double HalfCm, double HeightM, const FString& Label)
	{
		UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr; if (!W) return nullptr;
		// clockwise in Godot's ground plane, so the containment test works
		TArray<FVector> Loop = {
			Center + FVector(-HalfCm, -HalfCm, 0), Center + FVector(-HalfCm, HalfCm, 0),
			Center + FVector(HalfCm, HalfCm, 0),   Center + FVector(HalfCm, -HalfCm, 0) };
		AActor* A = W->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
		if (!A) return nullptr;
		A->Tags.Add(kPlacedTag);
		A->Tags.Add(FName(TEXT("label:PolygonVolume")));
		A->Tags.Add(FName(*FString::Printf(TEXT("p:height=%g"), HeightM)));
		MakeProcMesh(A, TEXT("Volume"));
		GVolumeLoops.Add(A, Loop);
		BF6_WriteLoopTags(A);
		RebuildVolumeWalls(A, Loop);
		BF6_SetPrettyLabel(A, Label);
		BF6_FileActor(A);
		A->SetFlags(RF_Transient);
		return A;
	}

	// HQ + protected area + four linked spawn points facing outward
	static void BF6_WizHqBundle(int32 Team, const FVector& Pos, int32 ObjIdVal, const FString& Prefix)
	{
		AActor* Hq = BF6_WizSpawn(TEXT("HQ_PlayerSpawner"), Pos, 0, Prefix);
		if (!Hq) return;
		AActor* Area = BF6_WizSquareVolume(Pos, 800.0, 10.0, Prefix + TEXT("_Area"));
		TArray<FString> Sp;
		for (int32 i = 0; i < 4; i++)
		{
			const double Ang = PI * 0.25 + PI * 0.5 * (double)i;
			const FVector P = Pos + FVector(FMath::Cos(Ang), FMath::Sin(Ang), 0.0) * 500.0;
			if (AActor* S = BF6_WizSpawn(TEXT("SpawnPoint"), P, FMath::RadiansToDegrees(Ang), FString::Printf(TEXT("%s_Spawn%d"), *Prefix, i + 1)))
				Sp.Add(BF6_ActorLinkName(S));
		}
		Hq->Tags.Add(FName(*FString::Printf(TEXT("p:Team=Team%d"), Team)));
		Hq->Tags.Add(FName(*FString::Printf(TEXT("p:AltTeam=Team%d"), Team == 1 ? 2 : 1)));
		Hq->Tags.Add(FName(*FString::Printf(TEXT("p:ObjId=%d"), ObjIdVal)));
		if (Sp.Num()) Hq->Tags.Add(FName(*(TEXT("p:InfantrySpawns=") + FString::Join(Sp, TEXT(",")))));
		if (Area) Hq->Tags.Add(FName(*(TEXT("p:HQArea=") + BF6_ActorLinkName(Area))));
		// The bundle is an assembly, so it is built like one: everything it made
		// hangs under the HQ, the way real Portal maps are built.
		BF6_ParentUnder(Area, Hq);
		for (const FString& N : Sp) BF6_ParentUnder(BF6_FindByLinkName(N), Hq);
	}

	// capture point + capture area + four spawns per team facing the flag
	static FString BF6_WizFlagBundle(const FVector& Pos, int32 ObjIdVal, const FString& Prefix)
	{
		AActor* Cp = BF6_WizSpawn(TEXT("CapturePoint"), Pos, 0, Prefix);
		if (!Cp) return FString();
		AActor* Area = BF6_WizSquareVolume(Pos, 600.0, 10.0, Prefix + TEXT("_Area"));
		TArray<FString> T1, T2;
		for (int32 i = 0; i < 8; i++)
		{
			const double Ang = PI * 0.25 * (double)i;
			const FVector P = Pos + FVector(FMath::Cos(Ang), FMath::Sin(Ang), 0.0) * 800.0;
			const double Yaw = FMath::RadiansToDegrees(Ang) + 180.0;   // face the flag
			if (AActor* S = BF6_WizSpawn(TEXT("SpawnPoint"), P, Yaw, FString::Printf(TEXT("%s_Spawn%d"), *Prefix, i + 1)))
				((i % 2 == 0) ? T1 : T2).Add(BF6_ActorLinkName(S));
		}
		Cp->Tags.Add(FName(*FString::Printf(TEXT("p:ObjId=%d"), ObjIdVal)));
		if (Area) Cp->Tags.Add(FName(*(TEXT("p:CaptureArea=") + BF6_ActorLinkName(Area))));
		if (T1.Num()) Cp->Tags.Add(FName(*(TEXT("p:InfantrySpawnPoints_Team1=") + FString::Join(T1, TEXT(",")))));
		if (T2.Num()) Cp->Tags.Add(FName(*(TEXT("p:InfantrySpawnPoints_Team2=") + FString::Join(T2, TEXT(",")))));
		BF6_ParentUnder(Area, Cp);
		for (const FString& N : T1) BF6_ParentUnder(BF6_FindByLinkName(N), Cp);
		for (const FString& N : T2) BF6_ParentUnder(BF6_FindByLinkName(N), Cp);
		return BF6_ActorLinkName(Cp);
	}

	static void BF6_WizSector(const FVector& Pos, int32 ObjIdVal, const FString& Prefix, const TArray<FString>& CpNames)
	{
		AActor* Sec = BF6_WizSpawn(TEXT("Sector"), Pos, 0, Prefix);
		if (!Sec) return;
		Sec->Tags.Add(FName(*FString::Printf(TEXT("p:ObjId=%d"), ObjIdVal)));
		if (CpNames.Num()) Sec->Tags.Add(FName(*(TEXT("p:CapturePoints=") + FString::Join(CpNames, TEXT(",")))));
	}

	// ---- BUNDLES: a finished piece, dropped in one click --------------------
	//
	// The wizard already knew how to build these; they were just locked inside a
	// script you had to run start to finish. Most of the time a creator wants
	// ONE more flag, not a fresh Conquest layout, and building it by hand is a
	// capture point, an area, sixteen spawns and twenty links.
	//
	// Same builders as the wizard, so a bundle placed here and one placed by the
	// wizard cannot come out different.
	TArray<FBundleDef> Bundles()
	{
		TArray<FBundleDef> Out;
		Out.Add({ TEXT("HQ1"),    TEXT("HQ TEAM 1"), TEXT("hq, area, 4 spawns") });
		Out.Add({ TEXT("HQ2"),    TEXT("HQ TEAM 2"), TEXT("hq, area, 4 spawns") });
		Out.Add({ TEXT("FLAG"),   TEXT("FLAG"),      TEXT("capture point, area, 8 spawns") });
		Out.Add({ TEXT("SECTOR"), TEXT("SECTOR"),    TEXT("sector and its area") });
		Out.Add({ TEXT("MCOM"),   TEXT("MCOM"),      TEXT("one objective, on its own") });
		return Out;
	}

	bool PlaceBundle(const FString& Key, const FVector& World)
	{
		if (!g_ss.bEditing) { RefuseReadOnly(FString()); return false; }
		UWorld* BW = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!BW) return false;
		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Place %s"), *Key)));
		// A SPAWN is only undoable if the LEVEL was marked before it happened.
		// Without this the transaction records the property edits and none of
		// the actors, so Ctrl+Z left the whole bundle standing.
		BW->GetCurrentLevel()->Modify();

		FString RootNm;   // the piece's top actor, selected once it stands
		if (Key == TEXT("HQ1") || Key == TEXT("HQ2"))
		{
			const int32 Team = (Key == TEXT("HQ2")) ? 2 : 1;
			const int32 Id = BF6_NextObjId(301, 99);
			RootNm = FString::Printf(TEXT("Team%d_HQ_%d"), Team, Id);
			BF6_WizHqBundle(Team, World, Id, RootNm);
		}
		else if (Key == TEXT("FLAG"))
		{
			const int32 Id = BF6_NextObjId(200, 99);
			// Flags are lettered in every Portal script anyone will paste in, and
			// A is 200 - so the letter follows from the id rather than a counter
			// that would restart at A after a reload.
			const FString Letter = FString::Chr((TCHAR)(TEXT('A') + FMath::Clamp(Id - 200, 0, 25)));
			RootNm = BF6_WizFlagBundle(World, Id, TEXT("CapturePoint_") + Letter);
		}
		else if (Key == TEXT("SECTOR"))
		{
			const int32 Id = BF6_NextObjId(100, 99);
			const FString Nm = FString::Printf(TEXT("Sector_%d"), Id - 99);
			RootNm = Nm;
			BF6_WizSector(World, Id, Nm, TArray<FString>());
			// A sector with no area covers nothing, so it gets the one field it
			// cannot work without and the creator shapes it.
			if (AActor* Sec = BF6_FindByLinkName(Nm))
				if (AActor* Ar = BF6_WizSquareVolume(World, 2000.0, 10.0, Nm + TEXT("_SectorArea")))
				{
					Sec->Tags.Add(FName(*(TEXT("p:SectorArea=") + BF6_ActorLinkName(Ar))));
					BF6_ParentUnder(Ar, Sec);
				}
		}
		else if (Key == TEXT("MCOM"))
		{
			// MCOM owns nothing - it has no link fields at all - so the bundle is
			// the object, and pretending otherwise would be theatre.
			RootNm = FString::Printf(TEXT("MCOM_%d"), BF6_NextObjId(400, 99) - 399);
			BF6_WizSpawn(TEXT("MCOM"), World, 0, RootNm);
		}
		else return false;

		BF6_RecomputeBudget();
		// The new piece is the result, so the new piece is what ends up
		// selected - the same statement placing a single object makes. Whatever
		// was selected before the wheel opened has nothing to do with what just
		// landed, and leaving it lit made the placement look like a no-op.
		if (GEditor)
			if (AActor* Root = BF6_FindByLinkName(RootNm))
			{
				GEditor->SelectNone(false, true, false);
				GEditor->SelectActor(Root, true, true);
			}
		return true;
	}

	static void BF6_WizFinish()
	{
		GWiz.bActive = false;
		const TArray<FLintItem> L = RunLint();
		int32 nProb = 0;
		for (const FLintItem& I : L) if (I.Severity == 0) nProb++;
		Notify(nProb == 0
			? FString::Printf(TEXT("%s setup complete - checks came back clean. Everything is a normal object now, move and edit freely."), GWiz.bConquest ? TEXT("Conquest") : TEXT("Breakthrough"))
			: FString::Printf(TEXT("%s setup complete - CHECKS found %d problem%s worth a look."), GWiz.bConquest ? TEXT("Conquest") : TEXT("Breakthrough"), nProb, nProb == 1 ? TEXT("") : TEXT("s")));
	}

	void StartModeWizard(const FString& Mode, int32 Count)
	{
		if (!g_ss.bEditing) { BF6Api::RefuseReadOnly(FString()); return; }
		GWiz = FBF6Wiz();
		GWiz.bActive = true;
		GWiz.bConquest = Mode == TEXT("Conquest");
		GWiz.Count = FMath::Clamp(Count, 1, GWiz.bConquest ? 7 : 6);
		GWiz.Total = GWiz.bConquest ? GWiz.Count + 2 : GWiz.Count * 4;
		if (GEditor) GEditor->SelectNone(false, true, false);
	}

	void CancelModeWizard() { GWiz.bActive = false; }

	void ModeWizardPlaceAt(const FVector& World)
	{
		if (!GWiz.bActive) return;
		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Mode setup step %d"), GWiz.Step + 1)));
		// same reason as the bundles: without the level, the spawns are not
		// in the transaction and Ctrl+Z undoes nothing visible
		if (UWorld* WW = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
			WW->GetCurrentLevel()->Modify();
		if (GWiz.bConquest)
		{
			if (GWiz.Step == 0)      BF6_WizHqBundle(1, World, 301, TEXT("Team1_HQ"));
			else if (GWiz.Step == 1) BF6_WizHqBundle(2, World, 302, TEXT("Team2_HQ"));
			else
			{
				const int32 f = GWiz.Step - 2;
				const FString Nm = BF6_WizFlagBundle(World, 200 + f, TEXT("CapturePoint_") + BF6_WizFlagLetter(f));
				if (!Nm.IsEmpty()) { GWiz.FlagNames.Add(Nm); GWiz.FlagPos.Add(World); }
			}
			GWiz.Step++;
			if (GWiz.Step >= GWiz.Total)
			{
				FVector C = FVector::ZeroVector;
				for (const FVector& P : GWiz.FlagPos) C += P;
				if (GWiz.FlagPos.Num()) C /= (double)GWiz.FlagPos.Num();
				BF6_WizSector(C, 100, TEXT("Sector_1"), GWiz.FlagNames);
				BF6_WizFinish();
			}
		}
		else
		{
			const int32 s = GWiz.Step / 4, sub = GWiz.Step % 4;
			if (sub == 0)      BF6_WizHqBundle(1, World, 301 + s, FString::Printf(TEXT("S%d_Team1_HQ"), s + 1));
			else if (sub == 1) BF6_WizHqBundle(2, World, 401 + s, FString::Printf(TEXT("S%d_Team2_HQ"), s + 1));
			else
			{
				const FString Nm = BF6_WizFlagBundle(World, 1100 + s * 100 + (sub - 2), FString::Printf(TEXT("S%d_Objective%s"), s + 1, sub == 2 ? TEXT("A") : TEXT("B")));
				if (!Nm.IsEmpty()) { GWiz.SectorCp.Add(Nm); GWiz.SectorCpPos.Add(World); }
			}
			GWiz.Step++;
			if (GWiz.Step % 4 == 0)
			{
				FVector C = FVector::ZeroVector;
				for (const FVector& P : GWiz.SectorCpPos) C += P;
				if (GWiz.SectorCpPos.Num()) C /= (double)GWiz.SectorCpPos.Num();
				BF6_WizSector(C, 100 + s + 1, FString::Printf(TEXT("Sector_%d"), s + 1), GWiz.SectorCp);
				GWiz.SectorCp.Reset(); GWiz.SectorCpPos.Reset();
			}
			if (GWiz.Step >= GWiz.Total) BF6_WizFinish();
		}
		BF6_RecomputeBudget();
	}

	// ---- Godot-style camera navigation (driven by the input processor) ----
	// Orbit pivot: the selection's centre when something is selected (you orbit
	// what you're working on), else the surface under the cursor, else a point
	// ahead of the camera.
	bool ComputeOrbitPivot(FVector& Out)
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC) return false;
		if (GEditor)
			if (USelection* S = GEditor->GetSelectedActors())
			{
				FBox Box(ForceInit); int32 n = 0;
				for (int32 i = 0; i < S->Num(); i++)
					if (AActor* A = Cast<AActor>(S->GetSelectedObject(i))) { Box += A->GetActorLocation(); n++; }
				if (n > 0) { Out = Box.GetCenter(); return true; }
			}
		if (WorldFromViewportCursor(Out)) return true;
		const FViewportCameraTransform& Cam = VC->GetViewTransform();
		Out = Cam.GetLocation() + Cam.GetRotation().Vector() * 1000.f;
		return true;
	}

	// Opening a map used to leave the camera wherever the editor happened to be -
	// often the origin, which on most maps is under the terrain or off in a
	// corner. A new map now opens looking down at the middle of the play area
	// from a height that fits it on screen, the way the SDK presents a level.
	// Resuming a save is left alone: the creator's own view is restored there.
	void FrameCombatArea()
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;

		// the combat volume if there is one, else everything the map ships with
		FBox Play(ForceInit);
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (!It->Tags.Contains(kBaseTag) && !It->Tags.Contains(kPlacedTag)) continue;
			const TArray<FVector>* Loop = GVolumeLoops.Find(*It);
			if (!Loop || Loop->Num() < 3) continue;
			FString Ty = TagValue(*It, TEXT("label:"));
			if (Ty.IsEmpty()) Ty = TagValue(*It, TEXT("type:"));
			const FString Nm = It->GetActorLabel();
			if (!Nm.Contains(TEXT("Combat")) && !Ty.Contains(TEXT("Combat"))) continue;
			for (const FVector& Pt : BF6_LoopToWorld(*It, *Loop)) Play += Pt;
		}
		if (!Play.IsValid)
			for (TActorIterator<AActor> It(W); It; ++It)
				if (It->Tags.Contains(kBaseTag)) Play += It->GetActorLocation();
		if (!Play.IsValid) return;   // nothing to look at: leave the view alone

		const FVector Centre = Play.GetCenter();
		const double Span = FMath::Max3(Play.GetSize().X, Play.GetSize().Y, 5000.0);
		// far enough back that the whole play area fits, at the SDK's overview angle
		const FRotator Look(-50.f, -45.f, 0.f);
		VC->SetViewLocation(Centre - Look.Vector() * (Span * 0.9));
		VC->SetViewRotation(Look);
		VC->Invalidate();
	}

	void CameraOrbit(const FVector2D& DeltaPx, const FVector& Pivot)
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC) return;
		FRotator R = VC->GetViewRotation();
		R.Yaw += DeltaPx.X * 0.4f;
		R.Pitch = FMath::Clamp(R.Pitch - DeltaPx.Y * 0.4f, -85.f, 85.f);
		R.Roll = 0.f;
		const float Dist = FMath::Max(50.f, (float)FVector::Dist(VC->GetViewLocation(), Pivot));
		VC->SetViewLocation(Pivot - R.Vector() * Dist);
		VC->SetViewRotation(R);
		VC->Invalidate();
	}

	void CameraPan(const FVector2D& DeltaPx, const FVector& DepthRef)
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC) return;
		const FRotationMatrix M(VC->GetViewRotation());
		const FVector Right = M.GetScaledAxis(EAxis::Y), Up = M.GetScaledAxis(EAxis::Z);
		// pan speed scales with how far the reference surface is, like Godot
		const float Depth = FMath::Max(200.f, (float)FVector::Dist(VC->GetViewLocation(), DepthRef));
		const FVector Move = (-Right * DeltaPx.X + Up * DeltaPx.Y) * Depth * 0.0018f;
		VC->SetViewLocation(VC->GetViewLocation() + Move);
		VC->Invalidate();
	}

	// Shared ray-to-surface drop for both entry points below.
	static bool TraceToSurface(const FVector& O, const FVector& D, FVector& OutWorld);

	bool WorldFromViewportCenter(FVector& OutWorld)
	{
		// the spot in front of the camera: a ray through the viewport centre
		if (!GCurrentLevelEditingViewportClient) return false;
		const FViewportCameraTransform& Cam = GCurrentLevelEditingViewportClient->GetViewTransform();
		return TraceToSurface(Cam.GetLocation(), Cam.GetRotation().Vector(), OutWorld);
	}

	// ---- DISPLAY / SUN ----------------------------------------------------
	//
	// A VIEW AID, AND NOTHING MORE. Portal has no lighting in its authorable
	// surface at all - the only types it exports are the two volumes, its
	// exporter has no sun, sky or light handling, and this tool already treats
	// DirectionalLight3D as an engine node that Portal's own exporter drops.
	//
	// That has to be said on the panel, because the failure mode is expensive
	// and silent: a creator spends an hour finding a golden-hour angle, ships,
	// and the map arrives lit however Portal lights it. Nothing here reaches an
	// export, and nothing here is saved.
	//
	// It is still worth having. Turning the sun is how you read a silhouette,
	// check that a lane stays legible when the light is low, and see where
	// shadows actually fall; unlit is how you judge massing without shading
	// arguing with you about it.
	struct FBF6DisplaySun
	{
		TWeakObjectPtr<ADirectionalLight> Light;
		bool     bCaptured = false;         // the map's own lighting, before we touched it
		FRotator OrigRot = FRotator::ZeroRotator;
		float    OrigIntensity = 0.f;
		bool     bOrigShadows = true;
		bool     bSpawned = false;          // we made the light; the map had none

		float TimeH = 13.5f;                // a high, slightly-past-noon default
		float DirDeg = 135.f;
		float Bright = 1.f;                 // multiplier on the map's own intensity
		bool  bShadows = true;
		int32 ViewMode = 0;                 // 0 lit, 1 unlit, 2 wireframe
		float EV = 0.f;                     // exposure compensation, 0 = the map's own
		bool  bTouched = false;
	};
	static FBF6DisplaySun GSun;

	// The sun this map is lit by. Preference is the light ALREADY IN THE LEVEL:
	// spawning a second one next to it does not replace the first, it adds to
	// it, and the map ends up brighter and lit from two directions at once.
	static ADirectionalLight* BF6_SunLight(bool bCreate)
	{
		if (GSun.Light.IsValid()) return GSun.Light.Get();
		if (!GEditor) return nullptr;
		UWorld* W = GEditor->GetEditorWorldContext().World();
		if (!W) return nullptr;

		for (TActorIterator<ADirectionalLight> It(W); It; ++It)
		{
			GSun.Light = *It;
			GSun.bSpawned = false;
			return *It;
		}
		if (!bCreate) return nullptr;

		// No sun in the map at all. Ours is transient and context-tagged so it
		// lives and dies with the scenery rather than with the creator's level.
		ADirectionalLight* L = W->SpawnActor<ADirectionalLight>();
		if (!L) return nullptr;
		L->SetActorLabel(TEXT("BF6_ViewSun"));
		L->Tags.Add(kContextTag);
		L->SetFlags(RF_Transient);
		if (ULightComponent* LC = L->GetLightComponent()) LC->SetIntensity(3.f);
		GSun.Light = L;
		GSun.bSpawned = true;
		return L;
	}

	static void BF6_SunCapture(ADirectionalLight* L)
	{
		if (GSun.bCaptured || !L) return;
		GSun.OrigRot = L->GetActorRotation();
		if (ULightComponent* LC = L->GetLightComponent())
		{
			GSun.OrigIntensity = LC->Intensity;
			GSun.bOrigShadows = LC->CastShadows != 0;
		}
		GSun.bCaptured = true;
	}

	static void BF6_SunApply()
	{
		ADirectionalLight* L = BF6_SunLight(true);
		if (!L) return;
		BF6_SunCapture(L);

		// TIME TO AN ANGLE. Six is dawn on the horizon, noon is high, eighteen
		// is dusk; outside that the sun is below the horizon and the map goes
		// to whatever ambient it has, which is a legitimate thing to want to
		// look at. Elevation tops out at 80 rather than 90 so there is always
		// SOME shadow direction - a light straight overhead flattens every
		// vertical face equally and tells you nothing about form.
		const float Elev = FMath::Sin((GSun.TimeH - 6.f) / 12.f * PI) * 80.f;
		L->SetActorRotation(FRotator(-Elev, GSun.DirDeg, 0.f));

		if (ULightComponent* LC = L->GetLightComponent())
		{
			const float Base = GSun.OrigIntensity > 0.f ? GSun.OrigIntensity : 3.f;
			LC->SetIntensity(Base * FMath::Max(GSun.Bright, 0.f));
			LC->SetCastShadows(GSun.bShadows);
		}
		GSun.bTouched = true;
		BF6_Redraw();
	}

	static void BF6_DisplayApply()
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC) return;
		VC->SetViewMode(GSun.ViewMode == 1 ? VMI_Unlit
			: GSun.ViewMode == 2 ? VMI_Wireframe : VMI_Lit);
		// Exposure: zero means hand it back to the map's own auto exposure,
		// rather than pinning it at EV 0 which is a real and quite dark value.
		if (FMath::IsNearlyZero(GSun.EV))
		{
			VC->ExposureSettings.bFixed = false;
		}
		else
		{
			VC->ExposureSettings.bFixed = true;
			VC->ExposureSettings.FixedEV100 = GSun.EV;
		}
		GSun.bTouched = true;
		BF6_Redraw();
	}

	float GetSunTime()        { return GSun.TimeH; }
	float GetSunDirection()   { return GSun.DirDeg; }
	float GetSunBrightness()  { return GSun.Bright; }
	bool  GetSunShadows()     { return GSun.bShadows; }
	int32 GetDisplayViewMode(){ return GSun.ViewMode; }
	float GetDisplayExposure(){ return GSun.EV; }
	bool  DisplaySunTouched() { return GSun.bTouched; }

	void SetSunTime(float H)       { GSun.TimeH = FMath::Clamp(H, 0.f, 24.f); BF6_SunApply(); }
	void SetSunDirection(float D)  { GSun.DirDeg = D; BF6_SunApply(); }
	void SetSunBrightness(float B) { GSun.Bright = FMath::Clamp(B, 0.f, 4.f); BF6_SunApply(); }
	void SetSunShadows(bool b)     { GSun.bShadows = b; BF6_SunApply(); }
	void SetDisplayViewMode(int32 M) { GSun.ViewMode = FMath::Clamp(M, 0, 2); BF6_DisplayApply(); }
	void SetDisplayExposure(float E) { GSun.EV = FMath::Clamp(E, -6.f, 6.f); BF6_DisplayApply(); }

	// Back to the map's own lighting, exactly as it was.
	void ResetDisplaySun()
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (VC)
		{
			VC->SetViewMode(VMI_Lit);
			VC->ExposureSettings.bFixed = false;
		}
		ADirectionalLight* L = GSun.Light.Get();
		if (L && GSun.bCaptured)
		{
			if (GSun.bSpawned)
			{
				// Ours to begin with, so the honest reset is to take it away
				// again rather than leave a sun the map never had.
				L->Destroy();
				GSun.Light.Reset();
			}
			else
			{
				L->SetActorRotation(GSun.OrigRot);
				if (ULightComponent* LC = L->GetLightComponent())
				{
					LC->SetIntensity(GSun.OrigIntensity);
					LC->SetCastShadows(GSun.bOrigShadows);
				}
			}
		}
		GSun.bCaptured = false;
		GSun.bSpawned = false;
		GSun.TimeH = 13.5f; GSun.DirDeg = 135.f; GSun.Bright = 1.f;
		GSun.bShadows = true; GSun.ViewMode = 0; GSun.EV = 0.f;
		GSun.bTouched = false;
		BF6_Redraw();
	}

	bool WorldFromViewportCursor(FVector& OutWorld)
	{
		if (!GCurrentLevelEditingViewportClient) return false;
		// On foot the pointer is captured for looking, so the aim comes from the
		// centre of the screen - place what you are looking at, like a crosshair.
		if (GWalk.bActive)
		{
			FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
			return TraceToSurface(VC->GetViewLocation(), VC->GetViewRotation().Vector(), OutWorld);
		}
		const FViewportCursorLocation Cursor = GCurrentLevelEditingViewportClient->GetCursorWorldLocationFromMousePos();
		const FVector O = Cursor.GetOrigin(), D = Cursor.GetDirection();
		if (D.IsNearlyZero()) return false;
		return TraceToSurface(O, D, OutWorld);
	}

	// Deproject an explicit viewport pixel instead of the CACHED mouse pos.
	// During a Slate drag the drop catcher covers the viewport, the cached pos
	// freezes, and library drops stopped landing under the cursor - the drag
	// event's own position is the truth there.
	bool WorldFromViewportPoint(const FVector2D& ViewportPx, FVector& OutWorld)
	{
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!VC || !VC->Viewport) return false;
		FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(VC->Viewport, VC->GetScene(), VC->EngineShowFlags));
		FSceneView* View = VC->CalcSceneView(&Family);
		if (!View) return false;
		FVector O = FVector::ZeroVector, D = FVector::ZeroVector;
		View->DeprojectFVector2D(ViewportPx, O, D);
		if (D.IsNearlyZero()) return false;
		return TraceToSurface(O, D, OutWorld);
	}

	// What the ray must NOT hit: whatever is being placed right now. A carried
	// object is a placed object like any other, so without this it lands on
	// itself - the surface moves with the cursor, the object climbs its own
	// face, and placement jitters. Set while carrying, cleared when it lands.
	static TSet<TWeakObjectPtr<AActor>> GRayIgnore;

	void SetPlacementIgnore(const TArray<AActor*>& Actors)
	{
		GRayIgnore.Reset();
		for (AActor* A : Actors) if (A) GRayIgnore.Add(A);
	}

	void ClearPlacementIgnore() { GRayIgnore.Reset(); }

	// Objects you placed yourself are hit too, so you can build upward - a crate
	// on a rooftop you dropped in, a light on your own gantry. They carry no
	// cooked collision (cooking 3,000 props would cost minutes at load, which is
	// exactly the bill we just removed from map opens), so the ray is tested
	// against their geometry directly: a cheap bounds check first, then real
	// triangles from the mesh cache for the handful that survive it.
	// Only: a shortlist gathered once by the caller, for rays fired in bulk.
	// Scatter samples thousands of points, and walking every actor in the map
	// per sample is the difference between instant and unusable.
	static bool BF6_RayHitsPlaced(const FVector& O, const FVector& D, double MaxDist, FVector& OutHit,
		const TArray<AActor*>* Only)
	{
		if (!GEditor) return false;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return false;
		const FVector End = O + D * MaxDist;

		// Pass one is bounds only, which is cheap enough to run over the whole map.
		// Testing triangles for every actor the ray brushes was what made placement
		// stutter: a single prop can carry thousands, and a ray down a street can
		// clip dozens of props. So candidates are collected with their entry
		// distance, sorted, and only opened up nearest-first.
		struct FCand { AActor* A; double Dist; FVector BoxHit; };
		TArray<FCand> Cands;
		TArray<AActor*> Everything;
		if (!Only)
		{
			for (TActorIterator<AActor> It(W); It; ++It) Everything.Add(*It);
			Only = &Everything;
		}
		for (AActor* A : *Only)
		{
			if (!IsValid(A)) continue;
			if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
			if (A->Tags.Contains(kHandleTag) || A->Tags.Contains(kContextTag)) continue;
			if (A->Tags.Contains(kGroupTag)) continue;          // nor is a node marker
			if (IsVolumeActor(A) || IsObbActor(A)) continue;   // zones are not surfaces
			if (GRayIgnore.Contains(A)) continue;   // the thing being placed
			FVector Org, Ext;
			A->GetActorBounds(false, Org, Ext);
			if (Ext.IsNearlyZero()) continue;
			FVector BoxHit, BoxNorm;
			float BoxDist = (float)MaxDist;
			if (!FMath::LineExtentBoxIntersection(FBox(Org - Ext, Org + Ext), O, End, FVector::ZeroVector, BoxHit, BoxNorm, BoxDist))
				continue;
			Cands.Add({ A, (BoxHit - O).Size(), BoxHit });
		}
		if (Cands.Num() == 0) return false;
		Cands.Sort([](const FCand& X, const FCand& Y){ return X.Dist < Y.Dist; });

		double Best = MaxDist;
		bool bAny = false;
		for (const FCand& C : Cands)
		{
			// everything from here on starts further away than the hit we already
			// have, so nothing left can win
			if (C.Dist >= Best) break;

			FString MeshName = TagValue(C.A, TEXT("mesh:"));
			if (MeshName.IsEmpty()) MeshName = TagValue(C.A, TEXT("type:"));
			const TArray<FBF6Surface>* Surfs = MeshName.IsEmpty() ? nullptr : GMeshCache.Find(ObjModelPath(MeshName));
			if (!Surfs)
			{
				// not decoded yet: its box is a fair stand-in until it is
				if (C.Dist < Best) { Best = C.Dist; OutHit = C.BoxHit; bAny = true; }
				continue;
			}

			const FTransform Xf = C.A->GetActorTransform();
			const FVector LO = Xf.InverseTransformPosition(O);
			const FVector LE = Xf.InverseTransformPosition(End);
			for (const FBF6Surface& S : *Surfs)
			{
				for (int32 t = 0; t + 2 < S.T.Num(); t += 3)
				{
					FVector Hit, Norm;
					if (!FMath::SegmentTriangleIntersection(LO, LE, S.V[S.T[t]], S.V[S.T[t + 1]], S.V[S.T[t + 2]], Hit, Norm))
						continue;
					const FVector WHit = Xf.TransformPosition(Hit);
					const double Dist = (WHit - O).Size();
					if (Dist < Best) { Best = Dist; OutHit = WHit; bAny = true; }
				}
			}
		}
		return bAny;
	}

	// Everything the creator has placed whose footprint overlaps an area, so a
	// scatter can land on a roof or a gantry the same as on the terrain.
	void CollectPlacedIn(const FBox2D& Area, TArray<AActor*>& Out)
	{
		Out.Reset();
		if (!GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
			if (A->Tags.Contains(kHandleTag) || A->Tags.Contains(kContextTag)) continue;
			if (IsVolumeActor(A) || IsObbActor(A)) continue;
			FVector Org, Ext;
			A->GetActorBounds(false, Org, Ext);
			if (Ext.IsNearlyZero()) continue;
			const FBox2D Foot(FVector2D(Org.X - Ext.X, Org.Y - Ext.Y), FVector2D(Org.X + Ext.X, Org.Y + Ext.Y));
			if (Foot.Intersect(Area)) Out.Add(A);
		}
	}

	// The surface under a point: the map's own scenery, or anything the
	// creator has built on it, whichever comes first.
	bool GroundRay(const FVector& From, const FVector& To, const TArray<AActor*>* Placed, FVector& OutHit, FVector* OutNormal)
	{
		bool bAny = false;
		double Best = (To - From).Size();
		FVector P, N(0, 0, 1);
		if (BF6_ContextRay(From, To, P, nullptr, &N))
		{ Best = (P - From).Size(); OutHit = P; if (OutNormal) *OutNormal = N; bAny = true; }
		if (Placed && Placed->Num() > 0)
		{
			const FVector D = (To - From).GetSafeNormal();
			// a placed object has no normal from this test; flat-up is close enough
			// for standing on a crate and the wall probe only needs the hit
			if (BF6_RayHitsPlaced(From, D, Best, P, Placed))
			{ OutHit = P; if (OutNormal) *OutNormal = FVector::UpVector; bAny = true; }
		}
		return bAny;
	}

	static bool TraceToSurface(const FVector& O, const FVector& D, FVector& OutWorld)
	{
		const double kReach = 500000.f;
		bool bHitWorld = false;
		double WorldDist = kReach;
		// The map's own surface, answered by our ray index rather than by physics
		// (see FBF6RayIndex): placements land on the bridge deck or the hillside
		// under the crosshair instead of a flat z=0 plane below the map.
		FVector CtxHit;
		if (BF6_ContextRay(O, O + D * kReach, CtxHit))
		{ OutWorld = CtxHit; WorldDist = (CtxHit - O).Size(); bHitWorld = true; }

		// whichever comes first: the map, or something the creator placed on it
		FVector PlacedHit;
		if (BF6_RayHitsPlaced(O, D, WorldDist, PlacedHit, nullptr)) { OutWorld = PlacedHit; return true; }
		if (bHitWorld) return true;

		// fallback: ground plane
		if (FMath::Abs(D.Z) > 1e-4f) { const float t = -O.Z / D.Z; OutWorld = (t > 0.f && t < 200000.f) ? (O + D * t) : (O + D * 1000.f); }
		else OutWorld = O + D * 1000.f;
		return true;
	}
}

// (The old docked tab is gone: the tool UI now attaches straight onto the level
// viewport via BF6Api::ShowStartupUI, so it always fills the editor's centre.)

// Undo/redo resurrects our actors WITHOUT their geometry (procedural-mesh
// sections are not transactional), so they'd reappear invisible. After every
// undo/redo, rebuild any of our actors that came back empty.
static FDelegateHandle g_postUndoHandle;
// Put one actor's geometry back. Undo needs it because a transaction that
// recorded an emptied mesh restores an emptied mesh; the drag path needs it
// because emptying is exactly how we keep vertex data out of the undo buffer.
static void BF6_RebuildActorGeometry(AActor* A)
{
	if (!A) return;
	UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
	if (!M || M->GetNumSections() > 0) return;
	if (A->Tags.Contains(kHandleTag)) { BuildHandleCube(M); ApplyHandleStyle(M); return; }
	if (const TArray<FVector>* Loop = GVolumeLoops.Find(A)) { RebuildVolumeWalls(A, *Loop); return; }
	FString Mesh = TagValue(A, TEXT("mesh:"));
	if (Mesh.IsEmpty()) Mesh = TagValue(A, TEXT("type:"));
	if (!Mesh.IsEmpty() && FillProcFromBf6Mesh(M, ObjModelPath(Mesh))) { ApplyObjectWhite(M); BF6Api::ReapplyTint(A); return; }
	BuildMarker(M); ApplyObjectWhite(M);
}

// Undo pays the same bill a third time. Before it applies the recorded state it
// serialises the CURRENT state so redo can come back to it, and by then the drag
// has already put the geometry back - so ctrl+Z on a big object froze for the
// same couple of seconds. This fires before that, and empties the payload of
// exactly the objects the transaction touches. The direction is not in the
// context, so both candidates are covered: the one about to be undone and the
// one about to be redone. Over-stripping is harmless, the repair pass below
// refills anything left empty.
static FDelegateHandle g_preUndoHandle;
static void BF6_StripBeforeUndoRedo(const FTransactionContext&)
{
	UTransBuffer* TB = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	if (!TB) return;
	const int32 Len = TB->GetQueueLength();
	const int32 Und = TB->GetUndoCount();
	const int32 Candidates[2] = { Len - Und - 1, Len - Und };
	TSet<AActor*> Seen;
	int32 n = 0;
	for (int32 Idx : Candidates)
	{
		if (Idx < 0 || Idx >= Len) continue;
		const FTransaction* T = TB->GetTransaction(Idx);
		if (!T) continue;
		TArray<UObject*> Objs;
		T->GetTransactionObjects(Objs);
		for (UObject* O : Objs)
		{
			AActor* A = Cast<AActor>(O);
			if (!A) if (UActorComponent* C = Cast<UActorComponent>(O)) A = C->GetOwner();
			if (!A || Seen.Contains(A)) continue;
			Seen.Add(A);
			if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
			UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
			if (!M || M->GetNumSections() == 0) continue;
			if (BF6Api::EmptySectionsQuietly(M)) n++;
		}
	}
	if (n > 0) UE_LOG(LogTemp, Log, TEXT("BF6: undo/redo stripped %d object(s) before the flip"), n);
}

// The transaction buffer does not exist yet when this module starts up, so the
// hook above has to be attached later - and quietly went unattached until the
// log showed it never firing. Attempted at startup and again once the engine is
// up, whichever gets there first.
static void BF6_HookTransBuffer()
{
	if (g_preUndoHandle.IsValid()) return;
	UTransBuffer* TB = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	if (!TB) return;
	g_preUndoHandle = TB->OnBeforeRedoUndo().AddStatic(&BF6_StripBeforeUndoRedo);
}

static void BF6_RepairAfterUndo()
{
	if (!GEditor) return;
	// an undone attach or detach changed the tree's shape; refile it
	BF6Api::MarkSceneTreeDirty();
	UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
	const double RepairT0 = FPlatformTime::Seconds();
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		AActor* A = *It;
		if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag) && !A->Tags.Contains(kHandleTag)) continue;
		// zone shapes ride the undo system through the actor's loop tags: undo
		// restored the tags, now resync the live loop + rebuild the walls
		TArray<FVector> TagLoop;
		if (BF6_ReadLoopTags(A, TagLoop))
		{
			const TArray<FVector>* Live = GVolumeLoops.Find(A);
			bool bDiffers = !Live || Live->Num() != TagLoop.Num();
			if (!bDiffers)
				for (int32 i = 0; i < TagLoop.Num(); i++)
					if (!(*Live)[i].Equals(TagLoop[i], 0.5f)) { bDiffers = true; break; }
			if (bDiffers)
			{
				GVolumeLoops.Add(A, TagLoop);
				RebuildVolumeWalls(A, TagLoop);
				GVolEdit.Active = 0;
			}
		}
		BF6_RebuildActorGeometry(A);
	}
	BF6_RecomputeBudget();
	const double RepairMs = (FPlatformTime::Seconds() - RepairT0) * 1000.0;
	if (RepairMs > 100.0) UE_LOG(LogTemp, Log, TEXT("BF6: undo repair took %.0f ms"), RepairMs);
}

// The viewport Del key goes through DeleteSelectionFast, but the outliner's
// right-click Delete and the Edit menu run the STOCK delete, which snapshots
// every transactional proc-mesh component into the undo buffer - vertex data
// and all, the same multi-second stall. This delegate fires as the stock
// delete begins, before any component's Modify() snapshot, so emptying the
// sections here keeps those snapshots featherweight too. BF6_RepairAfterUndo
// refills the meshes if the delete is undone.
static FDelegateHandle g_preDeleteHandle, g_postDeleteHandle;
static FDelegateHandle g_selChangedHandle;
static FDelegateHandle g_spawnHandle;
static TWeakObjectPtr<UWorld> g_spawnHookWorld;

// THE TREE-REFRESH INVARIANT, closed for good.
//
// Filing an actor marks the tree, and every well-behaved spawn files. The
// trouble is the ones that are not: twenty-five places in this file spawn an
// actor, several build one by hand without filing it, and each time one is
// added the tree silently stops listing what it made. The flag bundle was the
// latest - its fallback marker path never filed, so a flag appeared in the
// world and nowhere in the tree.
//
// Auditing twenty-five call sites is a job that has to be redone the moment
// somebody writes a twenty-sixth. Watching the WORLD instead is one hook that
// cannot be forgotten. The mark coalesces and is spaced a quarter second, so
// being told about spawns we did not care about costs almost nothing.
static void BF6_OnAnyActorSpawned(AActor*)
{
	if (g_ss.CurrentLevel.IsEmpty()) return;   // no map open: nothing to list
	BF6Api::MarkSceneTreeDirty();
}

static void BF6_HookSpawnWatch()
{
	UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (g_spawnHookWorld.Get() == W && g_spawnHandle.IsValid()) return;
	if (UWorld* Old = g_spawnHookWorld.Get())
		if (g_spawnHandle.IsValid()) Old->RemoveOnActorSpawnedHandler(g_spawnHandle);
	g_spawnHandle.Reset();
	g_spawnHookWorld = W;
	if (!W) return;
	g_spawnHandle = W->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateStatic(&BF6_OnAnyActorSpawned));
}

// Cancel a selection made on a read-only base map.
//
// Selecting an object there leads nowhere: every action the selection unlocks
// is refused, so the selection itself is the first thing the tool gets wrong -
// it looks like a handle on something the creator cannot actually touch.
// Refusing it the moment it happens is the honest answer, and it is where the
// pulse belongs.
//
// DEFERRED, because clearing the selection from inside the selection-changed
// event would re-enter it. One shot, and a guard so the clear we cause does
// not look like a new selection to cancel.
static bool GCancellingSelection = false;
static void BF6_CancelSelectionOnBase(UObject*)
{
	if (GCancellingSelection) return;
	if (!GEditor || g_ss.bEditing || g_ss.CurrentLevel.IsEmpty()) return;
	if (!BF6Api::IsBuildOverlayActive()) return;

	// Only OUR objects. An engine actor, a light, whatever else the creator
	// clicks in their own level is none of this tool's business.
	bool bOurs = false;
	if (USelection* Sel = GEditor->GetSelectedActors())
		for (FSelectionIterator It(*Sel); It; ++It)
			if (AActor* A = Cast<AActor>(*It))
				if (A->Tags.Contains(kPlacedTag) || A->Tags.Contains(kBaseTag) || A->Tags.Contains(kGroupTag))
					{ bOurs = true; break; }
	if (!bOurs) return;

	GCancellingSelection = true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
	{
		if (GEditor) { GEditor->SelectNone(true, true, false); GEditor->NoteSelectionChange(); }
		GCancellingSelection = false;
		BF6Api::RefuseReadOnly(TEXT("Nothing on a base map can be selected or changed. Name it and press Create, bottom right, to build on it."));
		return false;
	}));
}
static double g_deleteT0 = 0.0;
static TArray<TWeakObjectPtr<AActor>> GStrippedForDelete;
static void BF6_StripMeshesBeforeStockDelete()
{
	g_deleteT0 = FPlatformTime::Seconds();
	if (!GEditor) return;
	USelection* Sel = GEditor->GetSelectedActors();
	TSet<AActor*> All;
	for (int32 i = 0; Sel && i < Sel->Num(); i++)
		if (AActor* A = Cast<AActor>(Sel->GetSelectedObject(i)))
		{
			if (AGroupActor* G = Cast<AGroupActor>(A))
			{
				TArray<AActor*> Members;
				G->GetGroupActors(Members, true);
				for (AActor* M : Members) if (M) All.Add(M);
			}
			// The WHOLE subtree, not just the selection. Deleting a parent records
			// every actor hanging off it in the transaction as well - it has to,
			// to put the attachments back on undo - so a node with a few hundred
			// props under it serialised every one of their vertex payloads and
			// took seconds to disappear.
			TArray<AActor*> Sub;
			BF6_CollectSubtree(A, Sub);
			for (AActor* S : Sub) All.Add(S);
		}
	// Deleting ANY actor makes the editor walk every object in the world looking
	// for references to fix up, and that walk visits our vertex arrays element by
	// element - they are UPROPERTY data. One empty node took 3.1 SECONDS to
	// delete on a 2,000 object map for that reason alone, with our own work
	// measuring 0 ms. So the payload comes out of EVERY object of ours for the
	// duration of the delete, and goes back straight after.
	int32 n = 0;
	const double T0 = FPlatformTime::Seconds();
	if (UWorld* W = GEditor->GetEditorWorldContext().World())
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(kPlacedTag) || It->Tags.Contains(kBaseTag)
				|| It->Tags.Contains(kHandleTag) || It->Tags.Contains(kGroupTag))
				if (UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(It->GetRootComponent()))
					if (M->GetNumSections() > 0)
					{
						BF6Api::EmptySectionsQuietly(M);
						if (!All.Contains(*It)) GStrippedForDelete.Add(*It);   // the doomed ones do not come back
						n++;
					}
	if (n > 0)
		UE_LOG(LogBF6, Log, TEXT("stock delete: stripped %d object(s) in %.0f ms"), n,
			(FPlatformTime::Seconds() - T0) * 1000.0);
}

void FBF6UnrealSDKModule::StartupModule()
{
	// AUTO EXPOSURE OFF BY DEFAULT (user request, 2026-09-04). Eye adaptation
	// makes the viewport re-brighten as you turn, so a creator judging a map's
	// lighting is reading a moving target - and a dark interior looks correctly
	// lit right up until it is exported. Done here rather than only in
	// DefaultEngine.ini because a plugin-only update carries no project config,
	// so an existing install would never receive an ini change.
	//
	// Deferred to post-engine-init: the renderer's cvars are not all registered
	// when an editor module starts, and a FindConsoleVariable that misses is
	// silent. Anyone who wants the engine behaviour back can set the cvar; this
	// only chooses the default.
	FCoreDelegates::GetOnPostEngineInit().AddStatic([]
		{
			if (IConsoleVariable* CV =
				IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AutoExposure")))
			{
				if (CV->GetInt() != 0)
				{
					CV->Set(0, ECVF_SetByProjectSetting);
					UE_LOG(LogBF6, Display,
						TEXT("auto exposure disabled by default so viewport brightness is stable while building"));
				}
			}
			else
			{
				UE_LOG(LogBF6, Warning,
					TEXT("r.DefaultFeature.AutoExposure not found - leaving eye adaptation at the engine default"));
			}
		});
	g_postUndoHandle = FEditorDelegates::PostUndoRedo.AddStatic(&BF6_RepairAfterUndo);
	BF6_HookTransBuffer();
	FCoreDelegates::GetOnPostEngineInit().AddStatic(&BF6_HookTransBuffer);
	g_preDeleteHandle = FEditorDelegates::OnDeleteActorsBegin.AddStatic(&BF6_StripMeshesBeforeStockDelete);
	g_selChangedHandle = USelection::SelectionChangedEvent.AddStatic(&BF6_CancelSelectionOnBase);
	g_postDeleteHandle = FEditorDelegates::OnDeleteActorsEnd.AddStatic([]
		{
			const double Ms = (FPlatformTime::Seconds() - g_deleteT0) * 1000.0;
			const double R0 = FPlatformTime::Seconds();
			int32 Back = 0;
			for (const TWeakObjectPtr<AActor>& Wk : GStrippedForDelete)
				if (AActor* A = Wk.Get()) { BF6_RebuildActorGeometry(A); Back++; }
			GStrippedForDelete.Reset();
			// A deleted object may have been linked FROM somewhere. The name it
			// was linked by outlives it, so the attribute panel would go on
			// reporting it as assigned.
			BF6Api::PruneDeadLinks();
			if (Ms > 100.0)
				UE_LOG(LogBF6, Warning, TEXT("stock delete took %.0f ms, then %d object(s) refilled in %.0f ms"),
					Ms, Back, (FPlatformTime::Seconds() - R0) * 1000.0);
			BF6_Redraw();
		});
	g_pluginDir = IPluginManager::Get().FindPlugin(TEXT("BF6UnrealSDK"))->GetBaseDir();
	BF6_LoadCatOverrides();   // the user's "move to category" choices

	// One-time storage migrations must happen before anything creates the new
	// directory. Older builds first wrote user state to Saved/BF6HighPoly, then
	// wrote multi-gigabyte SDK conversion caches inside the plugin's Source/
	// tree. Preserve both, but make Saved/BF6UnrealSDK/sdkdata the sole runtime
	// location from this point forward.
	{
		const FString OldProductDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6") TEXT("HighPoly"));
		const FString NewProductDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"));
		if (IFileManager::Get().DirectoryExists(*OldProductDir)
			&& !IFileManager::Get().DirectoryExists(*NewProductDir))
		{
			if (IFileManager::Get().Move(*NewProductDir, *OldProductDir))
			{
				UE_LOG(LogBF6, Display, TEXT("migrated legacy Saved data to %s"), *NewProductDir);
			}
			else
			{
				UE_LOG(LogBF6, Warning, TEXT("could not migrate legacy Saved data from %s"), *OldProductDir);
			}
		}

		const FString LegacyData = g_pluginDir / TEXT("Source/ThirdParty/libbf6/data");
		const FString NewData = BF6_DataDir();
		IFileManager::Get().MakeDirectory(*NewData, true);

		auto MoveGeneratedDir = [&](const TCHAR* Name)
		{
			const FString From = LegacyData / Name;
			const FString To = NewData / Name;
			if (!IFileManager::Get().DirectoryExists(*From)
				|| IFileManager::Get().DirectoryExists(*To)) return;
			if (IFileManager::Get().Move(*To, *From))
			{
				UE_LOG(LogBF6, Display, TEXT("migrated generated SDK data: %s"), Name);
			}
			else
			{
				UE_LOG(LogBF6, Warning, TEXT("could not migrate generated SDK data: %s"), Name);
			}
		};
		auto CopySeedDir = [&](const TCHAR* Name)
		{
			const FString From = LegacyData / Name;
			const FString To = NewData / Name;
			if (!IFileManager::Get().DirectoryExists(*From)
				|| IFileManager::Get().DirectoryExists(*To)) return;
			if (!FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*To, *From, true))
				UE_LOG(LogBF6, Warning, TEXT("could not seed SDK data: %s"), Name);
		};

		// These directories were generated and git-ignored; moving avoids a
		// multi-gigabyte duplicate. The small catalogue/base data may still be a
		// tracked compatibility seed, so copy it and leave the source untouched.
		MoveGeneratedDir(TEXT("objmodels"));
		MoveGeneratedDir(TEXT("mapmesh"));
		MoveGeneratedDir(TEXT("sdkhistory"));
		CopySeedDir(TEXT("FbExportData"));
		CopySeedDir(TEXT("basesetup"));
		const FString LegacyStamp = LegacyData / TEXT("sdk.version.json");
		const FString NewStamp = NewData / TEXT("sdk.version.json");
		if (FPaths::FileExists(LegacyStamp) && !FPaths::FileExists(NewStamp))
		{
			if (IFileManager::Get().Copy(*NewStamp, *LegacyStamp) != COPY_OK)
			{
				UE_LOG(LogBF6, Warning, TEXT("could not seed sdk.version.json"));
			}
		}
	}

	// Seed the bundled data the SDK import can NOT produce: the map cards'
	// thumbnails (official Portal site tiles) and the gameplay marker meshes.
	// They ship in Resources/ and copy into the data dir if missing - fresh
	// installs had blank map cards and generic gameplay markers without this.
	{
		auto Seed = [](const FString& FromDir, const FString& ToDir, const TCHAR* Pattern)
		{
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(FromDir / Pattern), true, false);
			if (Files.Num()) IFileManager::Get().MakeDirectory(*ToDir, true);
			for (const FString& F : Files)
				if (!FPaths::FileExists(ToDir / F))
					IFileManager::Get().Copy(*(ToDir / F), *(FromDir / F));
		};
		Seed(g_pluginDir / TEXT("Resources/mapthumbs"),      BF6_DataDir() / TEXT("maps"),     TEXT("*.jpg"));
		Seed(g_pluginDir / TEXT("Resources/gameplaymeshes"), BF6_DataDir() / TEXT("gameplay"), TEXT("*.bf6mesh"));
	}

	// version-history baseline: installs that imported an SDK before the
	// history feature existed get their snapshot now, so the NEXT SDK update
	// has something to diff against
	{
		const FString Root = BF6Api::StoredSdkRoot();
		if (!Root.IsEmpty() && FPaths::FileExists(Root / TEXT("sdk.version.json")))
			BF6Api::BF6_SnapshotSdkHistory(Root);
	}

	FString DllPath = FPaths::Combine(g_pluginDir, TEXT("Binaries/Win64/bf6_core.dll"));
	if (!FPaths::FileExists(DllPath))
	{
		// Development fallback before UBT has staged the package. A packaged
		// plugin never relies on Source/ThirdParty at runtime.
		DllPath = FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/bin/Win64/bf6_core.dll"));
	}
	DllHandle = FPlatformProcess::GetDllHandle(*DllPath);
	if (!DllHandle) { UE_LOG(LogBF6, Error, TEXT("could not load bf6_core.dll")); return; }
	const bf6_abi_version_fn RuntimeAbi = (bf6_abi_version_fn)
		FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_abi_version"));
	if (!RuntimeAbi || RuntimeAbi() != BF6_ABI_VERSION)
	{
		UE_LOG(LogBF6, Error, TEXT("bf6_core ABI mismatch: plugin header=%d runtime=%d"),
			BF6_ABI_VERSION, RuntimeAbi ? RuntimeAbi() : -1);
		FPlatformProcess::FreeDllHandle(DllHandle);
		DllHandle = nullptr;
		return;
	}
	g_open    = (bf6_open_fn)            FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_open"));
	g_close   = (bf6_close_fn)           FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_close"));
	g_cat     = (bf6_catalogue_fn)       FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_catalogue"));
	g_read    = (bf6_read_mesh_fn)       FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_read_mesh"));
	g_free    = (bf6_free_fn)            FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_free"));
	g_loadp   = (bf6_load_placeables_fn) FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_load_placeables"));
	g_lvlcnt  = (bf6_level_count_fn)     FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_level_count"));
	g_lvlname = (bf6_level_name_fn)      FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_level_name"));
	g_listp   = (bf6_list_placeables_fn) FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_list_placeables"));
	g_props   = (bf6_placeable_props_fn) FPlatformProcess::GetDllExport(DllHandle, TEXT("bf6_placeable_props"));
	if (!g_open || !g_close || !g_cat || !g_read || !g_free) { UE_LOG(LogBF6, Error, TEXT("missing core export")); return; }

	char err[256] = {0};
	g_ctx = g_open(kGameDir, err, sizeof(err));
	if (g_ctx) g_gameDir = UTF8_TO_TCHAR(kGameDir);
	if (!g_ctx)
	{
		// No game install on this machine (or not at the Steam path). Fall back
		// to libbf6's no-install mode: mesh decode is unavailable, but the
		// placeable catalogue is SDK data and works fully without the game.
		UE_LOG(LogBF6, Log, TEXT("no game install (%hs); running catalogue-only"), err);
		g_ctx = g_open("", err, sizeof(err));
		g_gameDir.Reset();
	}
	if (!g_ctx) { UE_LOG(LogBF6, Warning, TEXT("bf6_open failed: %hs"), err); }
	else
	{
		UE_LOG(LogBF6, Display, TEXT("libbf6 opened the install: %d resources."), g_cat(g_ctx, "", nullptr, 0));
		if (g_loadp)
		{
			const FString FbDir = FPaths::Combine(BF6_DataDir(), TEXT("FbExportData"));
			if (!FPaths::FileExists(FbDir / TEXT("asset_types.json"))
				|| !FPaths::FileExists(FbDir / TEXT("level_info.json")))
			{
				UE_LOG(LogBF6, Display, TEXT("SDK placeable catalogue is not installed yet; Portal SDK setup is pending."));
			}
			else
			{
				char perr[256] = {0};
				const int np = g_loadp(g_ctx, TCHAR_TO_UTF8(*FbDir), perr, sizeof(perr));
				if (np > 0) { UE_LOG(LogBF6, Display, TEXT("SDK placeables loaded: %d objects across %d levels. Open Window > Tools > BF6 Objects."), np, g_lvlcnt ? g_lvlcnt(g_ctx) : 0); }
				else { UE_LOG(LogBF6, Warning, TEXT("bf6_load_placeables failed: %hs"), perr); }
			}
		}
	}

	// A saved layout from an older build may still restore the retired docked
	// tab; give it a hidden spawner with a redirect note so it never comes back
	// as an "Unrecognized Tab" stub.
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(kTabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
	{
		return SNew(SDockTab).TabRole(ETabRole::NomadTab)
		[
			SNew(SBox).Padding(12.f)
			[ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("The BF6 High-Poly tool now lives on the level viewport itself. Close this tab - the map selector is in the viewport."))) ]
		];
	})).SetMenuType(ETabSpawnerMenuType::Hidden);

	// Attach the tool UI straight onto the level viewport (fills the editor's
	// centre) and arm the space-bar pie handler, once the main window exists.
	IMainFrameModule& MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame"));
	auto EnterFullScreen = []()
	{
		// The raw-water lab is an intentionally empty Unreal world.  The SDK
		// module still supplies its install discovery and extension seam, but its
		// map selector/viewport overlay would cover the lab we are trying to
		// inspect.  Keep the process and viewport alive; skip only the full map UI.
		if (FParse::Param(FCommandLine::Get(), TEXT("bf6waterlab")))
		{
			FGlobalTabmanager::Get()->SetApplicationTitle(
				FText::FromString(TEXT("BF6 Raw Water Lab")));
			return;
		}
		// The outliner becomes OURS: same tab, same dock, SDK contents. Done
		// here rather than at module startup because the level editor - and so
		// its tab manager - does not exist that early.
		BF6Api::RegisterOutlinerTab();
		BF6Api::RegisterContextMenu();

		// UNREAL'S OWN CAMERA PREVIEW, OFF.
		//
		// Selecting a camera pops the engine's picture-in-picture preview in
		// the corner of the viewport, and this tool already shows its own -
		// larger, docked, and updating live while the camera is moved. Two
		// previews of the same camera, in different places and at different
		// sizes, is worse than either alone: the creator has to work out which
		// one is the real answer.
		//
		// Set every session rather than once behind a flag, because it is a
		// global editor preference: a creator who opens another project and
		// comes back should not silently lose it, and one who wants it back can
		// turn it on in Editor Preferences and we will not fight them within
		// the session.
		if (ULevelEditorViewportSettings* VS = GetMutableDefault<ULevelEditorViewportSettings>())
		{
			VS->bPreviewSelectedCameras = false;
			VS->PostEditChange();
		}
		// A bare-minimum editor layout, once: close Unreal's default panels so
		// only the viewport (our whole UI) and the World Outliner remain. Users
		// can reopen anything from the Window menu; we never fight them again.
		{
			bool bLayoutDone = false;
			GConfig->GetBool(TEXT("BF6UnrealSDK"), TEXT("MinimalLayoutApplied"), bLayoutDone, GEditorPerProjectIni);
			if (!bLayoutDone)
			{
				FLevelEditorModule& LE2 = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
				if (TSharedPtr<FTabManager> TM = LE2.GetLevelEditorTabManager())
				{
					static const TCHAR* CloseIds[] = {
						TEXT("LevelEditorSelectionDetails"), TEXT("LevelEditorSelectionDetails2"),
						TEXT("LevelEditorSelectionDetails3"), TEXT("LevelEditorSelectionDetails4"),
						TEXT("ContentBrowserTab1"), TEXT("LevelEditorToolBox"),
						TEXT("PlacementBrowser"), TEXT("OutputLog"),
						TEXT("WorldSettingsTab"), TEXT("LevelEditorStatsViewer"),
						TEXT("LevelEditorLayerBrowser"), TEXT("LevelEditorHierarchicalLODOutliner"),
						TEXT("LevelEditorEnvironmentLightingViewer") };
					for (const TCHAR* Id : CloseIds)
						if (TSharedPtr<SDockTab> Tab = TM->FindExistingLiveTab(FTabId(Id)))
							Tab->RequestCloseTab();
				}
				GConfig->SetBool(TEXT("BF6UnrealSDK"), TEXT("MinimalLayoutApplied"), true, GEditorPerProjectIni);
				GConfig->Flush(false, GEditorPerProjectIni);
			}
		}
		BF6Api::InstallInputHandler();
		BF6Api::ShowStartupUI();
		// Brand the editor window regardless of what the .uproject file is named
		// (older installs still carry the BF6_High_Poly project name).
		FGlobalTabmanager::Get()->SetApplicationTitle(FText::FromString(TEXT("BF6 Unreal SDK")));
		BF6_ReportUpdateOutcome();        // "Updated to vX" / "did not apply"
		BF6Api::CheckForUpdates(false);   // silent unless a newer release exists

		// New-SDK detection: if the remembered SDK folder now holds a different
		// version than our data was built from, offer an incremental re-sync.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
		{
			const FString Stamped = BF6_ReadSdkVersion(BF6_DataDir() / TEXT("sdk.version.json"));
			const FString Root = BF6Api::StoredSdkRoot();
			if (!Stamped.IsEmpty() && !Root.IsEmpty() && !BF6Api::IsImporting())
			{
				const FString Cur = BF6_ReadSdkVersion(Root / TEXT("sdk.version.json"));
				if (!Cur.IsEmpty() && Cur != Stamped)
				{
					const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(
						TEXT("Your SDK folder now holds Portal SDK %s, but this tool's data was built from %s.\n\nRe-sync now? Only new content is converted (use Full re-sync on the SDK Setup screen after big updates)."),
						*Cur, *Stamped)));
					if (Choice == EAppReturnType::Yes)
					{
						BF6Api::ShowSdkSetup();
						BF6Api::StartSdkImport(Root);
					}
				}
			}
			// managed lifecycle: also ask the community archive whether a NEWER
			// SDK exists than the one our data was built from (one offer per
			// version; declining does not nag)
			BF6Api::CheckForNewSdk();
			BF6Api::FetchUploadLimits();
			return false;   // one-shot
		}), 5.0f);
	};
	if (MainFrame.IsWindowInitialized())
	{
		EnterFullScreen();
	}
	else
	{
		MainFrame.OnMainFrameCreationFinished().AddLambda([EnterFullScreen](TSharedPtr<SWindow>, bool)
		{
			EnterFullScreen();
		});
	}

	IConsoleManager::Get().RegisterConsoleCommand(TEXT("bf6.mesh"),
		TEXT("Spawn a BF6 mesh by full resource name."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			SpawnResource(Args.Num() > 0 ? Args[0] : FString(TEXT("common/hardware/weapons/assaultrifle/m16a3/art/ob_wep_assaultrifle_m16a3_receiverauto_3p_mesh")),
				FString(), FTransform(FVector(0,0,100)), true);
		}));
}

void FBF6UnrealSDKModule::ShutdownModule()
{
	if (g_postUndoHandle.IsValid()) { FEditorDelegates::PostUndoRedo.Remove(g_postUndoHandle); g_postUndoHandle.Reset(); }
	if (g_preUndoHandle.IsValid())
	{
		if (UTransBuffer* TB = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr)
			TB->OnBeforeRedoUndo().Remove(g_preUndoHandle);
		g_preUndoHandle.Reset();
	}
	if (g_preDeleteHandle.IsValid()) { FEditorDelegates::OnDeleteActorsBegin.Remove(g_preDeleteHandle); g_preDeleteHandle.Reset(); }
	if (g_selChangedHandle.IsValid()) { USelection::SelectionChangedEvent.Remove(g_selChangedHandle); g_selChangedHandle.Reset(); }
	if (UWorld* SW = g_spawnHookWorld.Get())
		if (g_spawnHandle.IsValid()) SW->RemoveOnActorSpawnedHandler(g_spawnHandle);
	g_spawnHandle.Reset(); g_spawnHookWorld.Reset();
	// release the assign-mode MIDs before static destruction (GLinkPick is a
	// file-scope global; its strong pointers must not outlive the UObject system)
	for (int32 s = 0; s < 3; s++) GLinkPick.Mid[s].Reset();
	// Park the download toast (never destruct widgets during exit).
	if (GUpdateToast.IsValid()) { new TSharedPtr<SNotificationItem>(GUpdateToast); GUpdateToast.Reset(); }
	BF6Api::DetachUI();
	BF6Api::RemoveInputHandler();
	BF6Api::UnregisterOutlinerTab();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(kTabName);
	if (g_ctx && g_close) { g_close(g_ctx); g_ctx = nullptr; }
	if (DllHandle) { FPlatformProcess::FreeDllHandle(DllHandle); DllHandle = nullptr; }
}

namespace
{
	// Composite thumbnail for a Block: every object at its relative transform,
	// framed together with the same iso camera the single-model thumbs use.
	// (Down here because it reads the block JSON via the Blocks code above.)
	bool BF6_RenderBlockThumb(const FString& Name)
	{
		if (!BF6_ThumbRigReady()) return false;
		TSharedPtr<FJsonObject> B = BF6Api::BF6_LoadBlock(Name);
		if (!B.IsValid()) return false;
		const TArray<TSharedPtr<FJsonValue>>* Objs = nullptr;
		if (!B->TryGetArrayField(TEXT("objects"), Objs) || Objs->Num() == 0) return false;

		auto Vec = [](const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const FVector& Def)
		{
			const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
			if (!O->TryGetArrayField(Key, A) || A->Num() < 3) return Def;
			return FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
		};

		TArray<UProceduralMeshComponent*> Comps;
		FBoxSphereBounds All; bool bAny = false;
		for (const TSharedPtr<FJsonValue>& V : *Objs)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject(); if (!O.IsValid()) continue;
			FString Ms; O->TryGetStringField(TEXT("mesh"), Ms);
			if (Ms.IsEmpty()) continue;
			UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(GetTransientPackage());
			float Radius = 100.f;
			if (!BF6_LoadSdkModelInto(M, Ms, Radius)) continue;
			const FVector P = Vec(O, TEXT("pos"), FVector::ZeroVector);
			const FVector R = Vec(O, TEXT("rot"), FVector::ZeroVector);
			const FVector S = Vec(O, TEXT("scale"), FVector::OneVector);
			const FTransform Xf(FRotator(R.X, R.Y, R.Z), P, S);
			g_thumbs.Scene->AddComponent(M, Xf);
			Comps.Add(M);
			const FBoxSphereBounds CB = M->CalcBounds(Xf);
			All = bAny ? (All + CB) : CB;
			bAny = true;
		}
		bool bOk = false;
		if (bAny) bOk = BF6_CaptureRigTo(BF6_ThumbPathForKey(TEXT("block::") + Name), All);
		for (UProceduralMeshComponent* M : Comps) g_thumbs.Scene->RemoveComponent(M);
		return bOk;
	}
}

IMPLEMENT_MODULE(FBF6UnrealSDKModule, BF6UnrealSDK)
