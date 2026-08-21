#include "BF6UnrealSDK.h"

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/MemoryReader.h"
#include "Misc/Compression.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "ScopedTransaction.h"
#include "LevelEditorViewport.h"
#include "Input/DragAndDrop.h"
#include "ImageUtils.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
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

#include "BF6MapManifest.h"
#include "BF6BuildMode.h"
#include "BF6Theme.h"
#include "BF6Bridge.h"
#include "SBF6PreviewViewport.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"

THIRD_PARTY_INCLUDES_START
#include "bf6_core.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogBF6, Log, All);

typedef bf6_ctx*  (*bf6_open_fn)(const char*, char*, int);
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
static FString                g_pluginDir;

static const char* kGameDir =
    "C:/Program Files (x86)/Steam/steamapps/common/Battlefield 6";
static const FName kTabName("BF6Objects");
static const FName kPlacedTag("BF6Placed");
static const FName kContextTag("BF6Context");
static const FName kBaseTag("BF6Base");

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
static void BF6_SetPrettyLabel(AActor* A, const FString& InName)
{
	if (!A) return;
	FString Nm = InName;
	Nm.RemoveFromStart(TEXT("BF6_"));
	if (Nm.IsEmpty()) Nm = TEXT("Object");
	FActorLabelUtilities::SetActorLabelUnique(A, Nm);
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

static bool FuzzyScore(const FString& PatternLower, const FString& Str, int32& OutScore)
{
	OutScore = 0;
	if (PatternLower.IsEmpty()) return true;
	const FString S = Str.ToLower();
	int32 pi = 0, score = 0, run = 0, firstHit = -1;
	for (int32 si = 0; si < S.Len() && pi < PatternLower.Len(); si++)
	{
		if (S[si] == PatternLower[pi])
		{
			if (firstHit < 0) firstHit = si;
			run++; score += 1 + run;
			const bool wordStart = (si == 0) || S[si - 1] == '_' || S[si - 1] == ' ' || S[si - 1] == '/';
			if (wordStart) score += 5;
			pi++;
		}
		else run = 0;
	}
	if (pi != PatternLower.Len()) return false;
	if (firstHit == 0) score += 8;
	score -= firstHit;
	OutScore = score;
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
		// context is scenery: not clickable, not movable, foldered out of the way
		Mesh->bSelectable = false;
		Actor->SetLockLocation(true);
		Actor->SetFolderPath(FName(TEXT("BF6 Map Context")));
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
static bool FillProcFromBf6Mesh(UProceduralMeshComponent* Mesh, const FString& FilePath, bool bCollision = false);
static void ApplyObjectWhite(UProceduralMeshComponent* Mesh);

// ---- low-poly map context: load an extracted .bf6mesh into a proc-mesh actor ----
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
	Actor->SetLockLocation(true);
	Actor->SetFolderPath(FName(TEXT("BF6 Map Context")));   // out of the way in the outliner
	UProceduralMeshComponent* Mesh = MakeProcMesh(Actor, TEXT("ContextMesh"));
	// Collision ON: the space-bar placement ray traces this surface so objects
	// land where the crosshair points (not on a flat z=0 plane under the map).
	if (!FillProcFromBf6Mesh(Mesh, FilePath, true)) { World->EditorDestroyActor(Actor, false); return nullptr; }
	// The map context is scenery: never selectable, never movable.
	Mesh->bSelectable = false;
	// SDK proxy look: flat unlit green terrain / orange assets.
	const bool bAssets = Label.Contains(TEXT("_Assets"));
	const TCHAR* MatPath = bAssets ? TEXT("/Game/Materials/M_LevelAssets.M_LevelAssets")
	                               : TEXT("/Game/Materials/M_LevelTerrain.M_LevelTerrain");
	if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, MatPath))
		for (int32 s = 0; s < Mesh->GetNumSections(); s++) Mesh->SetMaterial(s, Mat);
	Mesh->SetVisibility(true, true);
	UE_LOG(LogBF6, Warning, TEXT("Loaded context %s: %d section(s)."), *Label, Mesh->GetNumSections());
	return Actor;
}

// Path to a placeable's SDK low-poly model (e.g. "AAGun_01" -> .../objmodels/AAGun_01.bf6mesh).
static FString ObjModelPath(const FString& MeshName)
{
	return FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/objmodels"), MeshName + TEXT(".bf6mesh"));
}

// Place a placeable using the SDK's shipped low-poly model (complete, fast, matches
// the Godot object library). MeshName is the placeable's 'mesh' constant.
static AActor* SpawnSdkModel(const FString& MeshName, const FString& Label, const FTransform& Xform)
{
	if (!GEditor || MeshName.IsEmpty()) return nullptr;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return nullptr;
	const FString Path = ObjModelPath(MeshName);
	if (!FPaths::FileExists(Path)) { UE_LOG(LogBF6, Warning, TEXT("no SDK model bundled for '%s'"), *MeshName); return nullptr; }
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), Xform);
	if (!Actor) return nullptr;
	BF6_SetPrettyLabel(Actor, Label.IsEmpty() ? MeshName : Label);
	Actor->Tags.Add(kPlacedTag);
	Actor->Tags.Add(FName(*(FString(TEXT("mesh:")) + MeshName)));
	if (!Label.IsEmpty()) Actor->Tags.Add(FName(*(FString(TEXT("label:")) + Label)));
	BF6_FileActor(Actor);   // self-sorting outliner: role/category folder
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
	A->SetFlags(RF_Transient);
	A->SetRootComponent(M);
	M->RegisterComponent();
	A->AddInstanceComponent(M);
	return M;
}

// Objects render pure white like Godot's object library (not proc-mesh grey).
static void ApplyObjectWhite(UProceduralMeshComponent* Mesh)
{
	if (!Mesh) return;
	if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_ObjectWhite.M_ObjectWhite")))
		for (int32 s = 0; s < Mesh->GetNumSections(); s++) Mesh->SetMaterial(s, Mat);
}

// Load a .bf6mesh file into a proc-mesh component (Godot->Unreal axis swap +
// winding fix). Verts are used as-is: world-space for map context, local for the
// small object/gameplay models. Returns false if missing/unreadable.
// bCollision cooks collision (async) - used by the map context so the space-bar
// placement ray can find the actual surface under the cursor.
static bool FillProcFromBf6Mesh(UProceduralMeshComponent* Mesh, const FString& FilePath, bool bCollision)
{
	if (!Mesh || !FPaths::FileExists(FilePath)) return false;
	if (bCollision) Mesh->bUseAsyncCooking = true;   // don't hitch the load
	TArray<uint8> File;
	if (!FFileHelper::LoadFileToArray(File, *FilePath) || File.Num() < 12) return false;

	// 'BF6Z' = gzip-compressed payload; 'BF6S' = raw payload (legacy, uncompressed).
	uint32 fileMagic = 0; FMemory::Memcpy(&fileMagic, File.GetData(), 4);
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
	for (uint32 s = 0; s < surf; s++)
	{
		uint32 vc = 0, ic = 0; Ar << vc; Ar << ic;
		TArray<FVector> V; V.Reserve(vc);
		for (uint32 v = 0; v < vc; v++) { float x, y, z; Ar << x; Ar << y; Ar << z; V.Add(FVector(x * M, z * M, y * M)); }
		uint8 hasN = 0, hasU = 0; Ar << hasN; Ar << hasU;
		TArray<FVector> N;
		if (hasN) { N.Reserve(vc); for (uint32 v = 0; v < vc; v++) { float x, y, z; Ar << x; Ar << y; Ar << z; N.Add(FVector(x, z, y)); } }
		TArray<FVector2D> UV;
		if (hasU) { UV.Reserve(vc); for (uint32 v = 0; v < vc; v++) { float u, w; Ar << u; Ar << w; UV.Add(FVector2D(u, w)); } }
		TArray<int32> T; T.Reserve(ic);
		for (uint32 i = 0; i < ic; i++) { int32 idx = 0; Ar << idx; T.Add(idx); }
		for (int32 t = 0; t + 2 < T.Num(); t += 3) { const int32 tmp = T[t + 1]; T[t + 1] = T[t + 2]; T[t + 2] = tmp; }
		const TArray<FLinearColor> NC; const TArray<FProcMeshTangent> NT;
		Mesh->CreateMeshSection_LinearColor((int32)s, V, T, N, UV, NC, NT, bCollision);
	}
	return true;
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

static void BF6_GhostRestoreSet(TArray<FBF6Ghosted>& Set)
{
	for (FBF6Ghosted& G : Set)
		if (UProceduralMeshComponent* M = G.Comp.Get())
		{
			for (int32 i = 0; i < G.Mats.Num(); i++) M->SetMaterial(i, G.Mats[i]);
			M->bSelectable = G.bWasSelectable;
		}
	Set.Reset();
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
};
static FBF6LinkPick GLinkPick;

static void BF6_LinkGhostRestore()
{
	BF6_GhostRestoreSet(GLinkPick.Ghosted);
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
	if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_LevelAssets.M_LevelAssets")))
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

static void RebuildVolumeWalls(AActor* Vol, const TArray<FVector>& Loop)
{
	UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(Vol->GetRootComponent());
	if (!M) return;
	// the zone's real height (Godot metres) when it carries one; 0 means
	// INFINITE in the SDK, which Godot's gizmo draws at 5 m - match that
	double H = 5.0;
	const FString HS = BF6Api::GetActorProp(Vol, TEXT("height"));
	if (HS.IsNumeric()) H = FCString::Atod(*HS);
	if (H <= 0.01) H = 5.0;
	M->ClearAllMeshSections();
	BuildWalls(M, Loop, (float)H * 100.f);
	if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Volume.M_Volume")))
		M->SetMaterial(0, Mat);
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
	if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Volume.M_Volume")))
		M->SetMaterial(0, Mat);
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

static void SaveSession(const FString& Level, const FString& Name)
{
	if (!GEditor || Name.IsEmpty()) return;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return;
	TArray<TSharedPtr<FJsonValue>> Objs;
	TMap<AGroupActor*, int32> GroupIdx;   // group root -> stable save index
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(kPlacedTag)) continue;
		if (It->Tags.Contains(kHandleTag)) continue;
		const FString MeshName = TagValue(*It, TEXT("mesh:"));
		const FString LabelName = TagValue(*It, TEXT("label:"));
		// volumes carry no mesh - the label identifies them
		if (MeshName.IsEmpty() && LabelName.IsEmpty()) continue;
		const FTransform Xf = It->GetActorTransform();
		const FVector L = Xf.GetLocation(); const FRotator R = Xf.Rotator(); const FVector S = Xf.GetScale3D();
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("mesh"), MeshName);
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
				if (TS.StartsWith(TEXT("blk:")) || TS.StartsWith(TEXT("blkid:")) || TS.StartsWith(TEXT("blkat:")))
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
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), W);
	const FString Path = BF6_SessionPathNew(Level, Name);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	FFileHelper::SaveStringToFile(Out, *Path);
	// the same save under the OLD flat layout is superseded - remove it so
	// the resume list never shows doubles
	const FString Old = BF6_SessionPathOld(Level, Name);
	if (FPaths::FileExists(Old)) IFileManager::Get().Delete(*Old);
	UE_LOG(LogBF6, Warning, TEXT("Saved %d object(s) to %s"), Objs.Num(), *Path);
}

static void LoadSession(const FString& Level, const FString& Name)
{
	if (Name.IsEmpty()) return;
	const FString Path = BF6_SessionPathFor(Level, Name);
	FString In;
	if (!FFileHelper::LoadFileToString(In, *Path)) { UE_LOG(LogBF6, Warning, TEXT("no save at %s"), *Path); return; }
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
	ClearActorsWithTag(kPlacedTag);
	const TArray<TSharedPtr<FJsonValue>>* Objs = nullptr;
	if (!Root->TryGetArrayField(TEXT("objects"), Objs)) return;
	int n = 0;
	TMap<int32, TArray<AActor*>> PendingGroups;   // saved group index -> members
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
		const TArray<TSharedPtr<FJsonValue>>* LArr = nullptr;
		if (O->TryGetArrayField(TEXT("loop"), LArr) && LArr->Num() >= 9)
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
		else if (Label == TEXT("OBBVolume"))
		{
			if (UWorld* Wld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
				A = SpawnObbActor(Wld, FTransform(Rot, L, Sc), FVector(10, 10, 10));
		}
		else
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
			if (O->TryGetStringField(TEXT("nm"), Nm) && !Nm.IsEmpty()) BF6_SetPrettyLabel(A, Nm);
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
			// visuals that depend on the restored values
			if (const TArray<FVector>* Lp = GVolumeLoops.Find(A)) RebuildVolumeWalls(A, *Lp);
			if (BF6Api::IsObbActor(A)) RebuildObbBox(A);
			n++;
		}
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
	UE_LOG(LogBF6, Warning, TEXT("Loaded %d object(s) from %s"), n, *Path);
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
static void Notify(const FString& Msg)
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
		const FString Path = FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/maps"), PngFile);
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
				[ SAssignNew(SaveNameBox, SEditableTextBox).HintText(FText::FromString(TEXT("custom map name"))) ]
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
		ClearActorsWithTag(kContextTag);
		ClearActorsWithTag(kPlacedTag);
		ClearActorsWithTag(kBaseTag);
		LoadLevel(); ApplyFilter();
		LoadBudgetMax();
		if (ListView.IsValid()) ListView->RequestListRefresh();
		LoadTerrainContext();
		// Always bring in the low-poly asset mesh too, so the buildings/props make
		// the map recognizable (terrain alone is an anonymous green shape).
		{
			const FString AP = MeshPath(TEXT("_assets.bf6mesh"));
			if (FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *CurrentLevel));
		}
		LoadBaseSetup();   // the map's shipped HQs, spawns, combat area (from the SDK level scene)
		if (!SaveName.IsEmpty()) LoadSession(CurrentLevel, SaveName);
	}

	FString MeshPath(const FString& Suffix) const
	{
		return FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/mapmesh"), CurrentLevel + Suffix);
	}

	void LoadTerrainContext()
	{
		const FString P = MeshPath(TEXT("_terrain.bf6mesh"));
		if (FPaths::FileExists(P)) SpawnContextMesh(P, FString::Printf(TEXT("%s_Terrain"), *CurrentLevel));
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
		const FString Path = FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/basesetup"), CurrentLevel + TEXT(".base.json"));
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
					if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Volume.M_Volume")))
						VM->SetMaterial(0, Mat);   // translucent teal, like Godot's PolygonVolume
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
		UE_LOG(LogBF6, Warning, TEXT("Base setup loaded for %s: %d objects."), *CurrentLevel, spawned);
	}

	FReply OnLoadAssets()
	{
		const FString P = MeshPath(TEXT("_assets.bf6mesh"));
		if (FPaths::FileExists(P)) SpawnContextMesh(P, FString::Printf(TEXT("%s_Assets"), *CurrentLevel));
		else UE_LOG(LogBF6, Warning, TEXT("no low-poly assets extracted for %s yet"), *CurrentLevel);
		return FReply::Handled();
	}
	FReply OnReloadTerrain() { LoadTerrainContext(); return FReply::Handled(); }

	// Turn the read-only base into an editable custom map. Until this is done, no
	// objects can be placed - so the base is never built on by accident.
	FReply OnCreateCustom()
	{
		FString Name = SaveNameBox.IsValid() ? SaveNameBox->GetText().ToString().TrimStartAndEnd() : FString();
		if (Name.IsEmpty()) { Notify(TEXT("Enter a name for your custom map first.")); return FReply::Handled(); }
		CurrentSave = Name;
		bEditing = true;
		SaveSession(CurrentLevel, CurrentSave);   // create the save file now
		Notify(FString::Printf(TEXT("Custom map '%s' created - you can now place objects."), *CurrentSave));
		return FReply::Handled();
	}

	FReply OnSaveClicked()
	{
		if (!bEditing || CurrentSave.IsEmpty()) { Notify(TEXT("Create a custom map first.")); return FReply::Handled(); }
		SaveSession(CurrentLevel, CurrentSave);
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
		const FString BasePath = FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/basesetup"), CurrentLevel + TEXT(".base.json"));
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
		ClearActorsWithTag(kContextTag);
		ClearActorsWithTag(kPlacedTag);
		ClearActorsWithTag(kBaseTag);
		LoadLevel(); ApplyFilter(); LoadBudgetMax();
		if (ListView.IsValid()) ListView->RequestListRefresh();
		LoadTerrainContext();
		{
			const FString AP = MeshPath(TEXT("_assets.bf6mesh"));
			if (FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *CurrentLevel));
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
				if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Volume.M_Volume")))
					VM->SetMaterial(0, Mat);
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
		const FString Path = FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/FbExportData/level_info.json"));
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
		if (!bEditing) { Notify(TEXT("This is the read-only base map. Create a custom map first to place objects.")); return FReply::Unhandled(); }
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
		if (!bEditing) { Notify(TEXT("This is the read-only base map. Create a custom map first to place objects.")); return; }
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
{ return FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/mapmesh"), Level + Suffix); }
static FString BF6_BaseJsonPath(const FString& Level)
{ return FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/basesetup"), Level + TEXT(".base.json")); }

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

static FString BF6_EffectiveCategory(const FPlaceableRow& r)
{
	if (const FString* O = GCatOverrides.Find(r.Type)) return *O;
	return BF6_TopSeg(r.Directory);
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

// The outliner folder an object belongs in. Gameplay gets role folders;
// everything else groups under BF6 Build by its library category.
static FString BF6_FolderForActor(AActor* A)
{
	if (!A) return TEXT("BF6 Build");
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

// File one actor into its computed folder (no-op for handles/context).
static void BF6_FileActor(AActor* A)
{
	if (!A || A->Tags.Contains(kHandleTag) || A->Tags.Contains(kContextTag)) return;
	if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) return;
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
}

static void BF6_LoadBudgetMax(const FString& Level)
{
	g_ss.BudgetMax = -1;
	const FString Path = FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/FbExportData/level_info.json"));
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

	TMap<FString, FVector> LocalPos; TMap<FString, FString> ParentOf;
	auto ReadPos = [](const TSharedPtr<FJsonObject>& O){ FVector p(0,0,0); const TArray<TSharedPtr<FJsonValue>>* a=nullptr; if(O->TryGetArrayField(TEXT("pos"),a)&&a->Num()>=3){p.X=(*a)[0]->AsNumber();p.Y=(*a)[1]->AsNumber();p.Z=(*a)[2]->AsNumber();} return p; };
	for (const auto& v : *Objs){ const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue; const FString nm=o->GetStringField(TEXT("name")); LocalPos.Add(nm,ReadPos(o)); FString par; o->TryGetStringField(TEXT("parent"),par); ParentOf.Add(nm,par); }
	auto ToUnreal = [](const FVector& G){ return FVector((float)G.X,(float)G.Z,(float)G.Y)*100.f; };

	int32 oid = 0, spawned = 0;
	TMap<FString, FBF6GNode> GMap;
	BF6_BuildGNodeMap(*Objs, GMap);
	for (const auto& v : *Objs)
	{
		const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
		const FString nm = o->GetStringField(TEXT("name")); const FString ty = o->GetStringField(TEXT("type"));
		// Godot pivots/cameras stay in the json for parent-chain math but
		// never become actors (and never export - the site rejects them)
		if (BF6_IsEngineNodeType(ty)) continue;
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
			if (A){ UProceduralMeshComponent* VM=MakeProcMesh(A,TEXT("Volume")); BuildWalls(VM,Loop,(float)VolH*100.f); if(UMaterialInterface* Mat=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Volume.M_Volume"))) VM->SetMaterial(0,Mat); GVolumeLoops.Add(A, Loop); BF6_WriteLoopTags(A); }
		}
		else
		{
			A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (A){ UProceduralMeshComponent* MM=MakeProcMesh(A,TEXT("Model")); if(!FillProcFromBf6Mesh(MM,ObjModelPath(ty))) BuildMarker(MM); ApplyObjectWhite(MM); }
		}
		if (!A) continue;
		if (!bVolume) A->SetActorTransform(FTransform(Rot, ToUnreal(gw), Scl));
		A->SetActorLabel(nm);   // exact base name: saves match base objects by it
		BF6_FileActor(A);
		A->Tags.Add(kBaseTag);
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
	UE_LOG(LogBF6, Warning, TEXT("Base setup loaded for %s: %d objects."), *Level, spawned);
}

static void BF6_EnsureBaseSetupFormat();   // regenerates stale base.json packs

static void BF6_OpenMapWorldImpl(const FString& Level, const FString& Save)
{
	if (!GEditor) return;
	BF6_EnsureBaseSetupFormat();
	g_ss.CurrentLevel = Level; g_ss.CurrentSave = Save; g_ss.bEditing = !Save.IsEmpty();
	ClearActorsWithTag(kContextTag); ClearActorsWithTag(kPlacedTag); ClearActorsWithTag(kBaseTag);
	BF6_LoadPlaceables(Level); BF6_LoadBudgetMax(Level);
	const FString TP = BF6_MapMeshPath(Level, TEXT("_terrain.bf6mesh"));
	if (FPaths::FileExists(TP)) SpawnContextMesh(TP, FString::Printf(TEXT("%s_Terrain"), *Level));
	const FString AP = BF6_MapMeshPath(Level, TEXT("_assets.bf6mesh"));
	if (FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *Level));
	BF6_LoadBaseSetup(Level);
	if (!Save.IsEmpty()) LoadSession(Level, Save);
	BF6_RecomputeBudget();
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
	auto EmitTyped = [&](const TSharedPtr<FJsonObject>& e, const BF6Api::FPropDef& D, const FString& V)
	{
		if (V.IsEmpty() || V.Contains(TEXT("NodePath")) || V.Contains(TEXT("ExtResource"))) return;
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
		}

	TArray<TSharedPtr<FJsonValue>> Dynamic;
	FString In; TSharedPtr<FJsonObject> BaseRoot;
	if (FFileHelper::LoadFileToString(In, *BF6_BaseJsonPath(Level))) { TSharedRef<TJsonReader<>> R=TJsonReaderFactory<>::Create(In); FJsonSerializer::Deserialize(R,BaseRoot); }
	const TArray<TSharedPtr<FJsonValue>>* BObjs = nullptr;
	if (BaseRoot.IsValid() && BaseRoot->TryGetArrayField(TEXT("objects"), BObjs))
	{
		TMap<FString, FBF6GNode> GMap;
		BF6_BuildGNodeMap(*BObjs, GMap);
		for (const auto& v:*BObjs)
		{
			const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue;
			const FString nm=o->GetStringField(TEXT("name")), ty=o->GetStringField(TEXT("type"));
			// statics emit separately; Godot engine pivots/cameras never emit
			// (the site errors on them as "unknown types")
			if (ty.StartsWith(TEXT("MP_")) || BF6_IsEngineNodeType(ty)) continue;
			// ACCUMULATED transform (parent pivots carry the deploy camera's
			// aim), unless the actor was MOVED in the editor - then the live
			// transform wins, converted like the placed-object path
			double WB[9]; FVector gw;
			BF6_GWorldOf(GMap, nm, WB, gw);
			TSharedPtr<FJsonObject> e=MakeShared<FJsonObject>();
			e->SetStringField(TEXT("name"), ShortName(nm)); e->SetStringField(TEXT("type"), ty);
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
			e->SetStringField(TEXT("id"), ShortName(nm));
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
		if (Ln.IsEmpty() || UsedPlacedNames.Contains(Ln)) Ln = FString::Printf(TEXT("placed_%d"), pid);
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
			TArray<TSharedPtr<FJsonValue>> PtsArr;
			for (const FVector& Wv : BF6_LoopToWorld(*It, *Loop))
				PtsArr.Add(MakeShared<FJsonValueObject>(Vec(Wv.X / 100.0, Wv.Z / 100.0, Wv.Y / 100.0)));
			e->SetArrayField(TEXT("points"), PtsArr);
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
		e->SetObjectField(TEXT("right"),Vec(1,0,0)); e->SetObjectField(TEXT("up"),Vec(0,1,0)); e->SetObjectField(TEXT("front"),Vec(0,0,1)); e->SetObjectField(TEXT("position"),Vec(0,0,0));
		e->SetStringField(TEXT("id"),FString::Printf(TEXT("Static/%s"),*snm));
		Static.Add(MakeShared<FJsonValueObject>(e));
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
static FString BF6_TscnAttr(const FString& Line, const TCHAR* Key)
{
	int32 i = Line.Find(Key, ESearchCase::CaseSensitive);
	if (i == INDEX_NONE) return FString();
	i += FCString::Strlen(Key);
	const int32 e = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, i);
	return e == INDEX_NONE ? FString() : Line.Mid(i, e - i);
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
		bool bHasSize = false; double Size[3] = { 10,10,10 };
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
		if (Line.StartsWith(TEXT("[")) ) { Cur.Reset(); continue; }   // sub_resource / connection blocks
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
		else if (Key.StartsWith(TEXT("metadata/")) || Key == TEXT("visible") || Key == TEXT("color")
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
	ClearActorsWithTag(kContextTag); ClearActorsWithTag(kPlacedTag); ClearActorsWithTag(kBaseTag);
	BF6_LoadPlaceables(Level); BF6_LoadBudgetMax(Level);
	const FString TP = BF6_MapMeshPath(Level, TEXT("_terrain.bf6mesh")); if (FPaths::FileExists(TP)) SpawnContextMesh(TP, FString::Printf(TEXT("%s_Terrain"), *Level));
	const FString AP = BF6_MapMeshPath(Level, TEXT("_assets.bf6mesh"));  if (FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *Level));
	UWorld* World = GEditor->GetEditorWorldContext().World(); if (!World) return false;

	auto ToUnreal = [](double gx, double gy, double gz) { return FVector((float)gx, (float)gz, (float)gy) * 100.f; };
	auto GodotXform = [](const double (&B)[9], const double (&O)[3], double x, double y, double z)
	{
		return FVector(
			(float)(B[0] * x + B[1] * y + B[2] * z + O[0]),
			(float)(B[3] * x + B[4] * y + B[5] * z + O[1]),
			(float)(B[6] * x + B[7] * y + B[8] * z + O[2]));
	};

	// pass A: spawn, assigning each actor a unique link name from its node name
	TMap<FString, AActor*> ActorByPath;
	TMap<FString, FString> LinkNameByPath;
	TSet<FString> UsedNames;
	int32 spawned = 0, skippedTypes = 0;
	for (const TSharedPtr<FTNode>& N : Nodes)
	{
		if (N->bSkip || N->Type.IsEmpty()) continue;
		// engine-class nodes that came through without instance/script
		if (N->Type == TEXT("Node3D") || N->Type == TEXT("Camera3D") || N->Type == TEXT("AnimationPlayer") || N->Type == TEXT("Path3D")) continue;

		FString Nm = N->Name;
		for (int32 sfx = 2; UsedNames.Contains(Nm); sfx++) Nm = FString::Printf(TEXT("%s_%d"), *N->Name, sfx);
		UsedNames.Add(Nm);

		AActor* A = nullptr;
		if (N->Type == TEXT("PolygonVolume") && N->Points.Num() >= 6)
		{
			TArray<FVector> Loop;
			for (int32 i = 0; i + 1 < N->Points.Num(); i += 2)
			{
				const FVector G = GodotXform(N->WM, N->WO, N->Points[i], 0.0, N->Points[i + 1]);
				Loop.Add(ToUnreal(G.X, G.Y, G.Z));
			}
			double H = N->Height; if (H <= 0.01) H = 5.0;   // 0 = infinite, drawn at 5 m
			A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (!A) continue;
			UProceduralMeshComponent* VM = MakeProcMesh(A, TEXT("Volume"));
			BuildWalls(VM, Loop, (float)H * 100.f);
			if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Volume.M_Volume"))) VM->SetMaterial(0, Mat);
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

	BF6_RecomputeBudget();
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
	ClearActorsWithTag(kContextTag); ClearActorsWithTag(kPlacedTag); ClearActorsWithTag(kBaseTag);
	BF6_LoadPlaceables(Level); BF6_LoadBudgetMax(Level);
	const FString TP = BF6_MapMeshPath(Level, TEXT("_terrain.bf6mesh")); if (FPaths::FileExists(TP)) SpawnContextMesh(TP, FString::Printf(TEXT("%s_Terrain"), *Level));
	const FString AP = BF6_MapMeshPath(Level, TEXT("_assets.bf6mesh"));  if (FPaths::FileExists(AP)) SpawnContextMesh(AP, FString::Printf(TEXT("%s_Assets"), *Level));

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

	int32 spawned = 0;
	for (const auto& dv : *Dyn)
	{
		const TSharedPtr<FJsonObject> o = dv->AsObject(); if (!o.IsValid()) continue;
		FString Type; o->TryGetStringField(TEXT("type"), Type);
		if (Type.IsEmpty() || Type.StartsWith(TEXT("MP_")) || BF6_IsEngineNodeType(Type)) continue;
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
				if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Volume.M_Volume"))) VM->SetMaterial(0, Mat);
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
		spawned++;
	}
	BF6_RecomputeBudget();
	Notify(FString::Printf(TEXT("Imported '%s' onto %s: %d objects. Editable now."), *g_ss.CurrentSave, *Level, spawned));
	return true;
}

// ============================================================================
// SDK data import. The tool's data packs are generated from the user's own
// unzipped Portal SDK download: catalogue jsons are copied, base setups are
// parsed natively from levels/*.tscn, and the mesh packs are extracted by
// driving the SDK's own bundled Godot headlessly with generated scripts
// (skip-existing + capped batches + relaunch-on-crash, the proven recipe).
// ============================================================================
#include "Internationalization/Regex.h"
#include "Misc/ConfigCacheIni.h"
#include "Containers/Ticker.h"

static FString BF6_DataDir() { return FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data")); }

struct FBF6Import
{
	enum class EPhase { Idle, Objects, Maps, Done, Failed };
	EPhase  Phase = EPhase::Idle;
	FString SdkRoot, GodotExe;
	// PARALLEL workers: each converts a deterministic shard of the work
	// (name.hash() % shards), so N headless Godots run at once - the
	// conversion is CPU-bound per process and one worker took ~4x as long
	TArray<FProcHandle> Procs;
	TArray<void*> PipeReads;
	TArray<void*> PipeWrites;
	int32   ObjTotal = 0, MapTotal = 0, LastCount = 0, Stagnant = 0;
	// StartCount = converted files when the phase began, so a phase that adds
	// NOTHING can be told apart from one that legitimately skipped existing work
	int32   StartCount = 0, ObjDone = 0;
	FString Status;
	float   Frac = 0.f;
	FTSTicker::FDelegateHandle Tick;
};
// worker counts: objects are many small loads (4 wide); map meshes are huge
// whole-map scenes, so 2 wide keeps peak memory sane
static constexpr int32 kBF6ObjShards = 4;
static constexpr int32 kBF6MapShards = 2;
static FBF6Import g_imp;

static int32 BF6_CountFiles(const FString& Dir, const TCHAR* Pattern)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / Pattern), true, false);
	return Files.Num();
}

static FString BF6_ReadSdkVersion(const FString& JsonPath)
{
	FString In, Ver;
	if (!FFileHelper::LoadFileToString(In, *JsonPath)) return Ver;
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (FJsonSerializer::Deserialize(R, Root) && Root.IsValid()) Root->TryGetStringField(TEXT("version"), Ver);
	return Ver;
}

// Find the Godot exe the SDK ships at its root (name carries the version).
static FString BF6_FindSdkGodot(const FString& SdkRoot)
{
	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *(SdkRoot / TEXT("Godot*win64*.exe")), true, false);
	// prefer the non-console build
	for (const FString& F : Found) if (!F.Contains(TEXT("console"))) return SdkRoot / F;
	return Found.Num() ? SdkRoot / Found[0] : FString();
}

// ---- base setups: parse levels/MP_*.tscn into <map>.base.json (native port of
// the extraction script - plain text parsing, no Godot needed) ----
static void BF6_ExtractBaseSetups(const FString& SdkRoot)
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
static void BF6_EnsureBaseSetupFormat()
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
static FString BF6_ObjectScript(const FString& OutDir, int32 Shard = 0, int32 Shards = 1)
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

static FString BF6_MapScript(const FString& OutDir, int32 Shard = 0, int32 Shards = 1)
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

static void BF6_ImportFail(const FString& Why)
{
	BF6_CloseGodotPipe();
	g_imp.Phase = FBF6Import::EPhase::Failed;
	g_imp.Status = FString::Printf(TEXT("Import failed: %s"), *Why);
	if (g_imp.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(g_imp.Tick); g_imp.Tick.Reset(); }
	Notify(FString::Printf(TEXT("SDK import failed: %s"), *Why));
}

// Everything the headless Godot prints lands here, so a machine where the
// conversion produces nothing is diagnosable from a tester's report.
static FString BF6_ImportLogPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("import"), TEXT("godot_run.log"));
}
static void BF6_ImportLog(const FString& Line)
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
static bool BF6_LaunchGodotWorkers(bool bObjects)
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

static void BF6_ImportTickPhase()
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
	g_imp.Phase = FBF6Import::EPhase::Done;
	g_imp.Status = FString::Printf(TEXT("Import complete - %d of %d models, %d of %d map meshes"), g_imp.ObjDone, g_imp.ObjTotal, Count, Target);
	g_imp.Frac = 1.f;
	if (g_imp.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(g_imp.Tick); g_imp.Tick.Reset(); }
	Notify(FString::Printf(TEXT("SDK import complete - %d models and %d map meshes ready."), g_imp.ObjDone, Count));
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
	const FString UpdateDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("update"));
	const FString ZipPath   = FPaths::Combine(UpdateDir, TEXT("plugin_update.zip"));
	const FString Staging   = FPaths::Combine(UpdateDir, TEXT("staging"));
	const FString Script    = FPaths::Combine(UpdateDir, TEXT("apply_update.ps1"));
	const FString PluginDir = FPaths::ConvertRelativePathToFull(g_pluginDir);
	const FString Project   = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / FApp::GetProjectName()) + TEXT(".uproject");
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
	Ps += FString::Printf(TEXT("Set-Content -Path $log -Value ((Get-Date -Format s) + '  applying update %s')\r\n"), *Tag);
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
	FFileHelper::SaveStringToFile(Ps, *Script);

	// Marker for the next launch: if the plugin version then matches this tag
	// the update applied; if not, the apply failed and the user is told so.
	FFileHelper::SaveStringToFile(Tag, *FPaths::Combine(UpdateDir, TEXT("pending.txt")));

	FPlatformProcess::CreateProc(TEXT("powershell.exe"),
		*FString::Printf(TEXT("-ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\""), *Script),
		true, false, false, nullptr, 0, nullptr, nullptr);
	UE_LOG(LogBF6, Warning, TEXT("Update %s staged - closing the editor to apply."), *Tag);
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

	void StartSdkImport(const FString& SdkRoot, bool bFullResync)
	{
		if (IsImporting()) return;
		FString Err;
		if (!ValidateSdkRoot(SdkRoot, Err)) { Notify(Err); return; }

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

	// Delete a save from BOTH layouts (the old flat file would otherwise
	// resurface as a zombie in the resume list). Refuses the OPEN session:
	// its autosave would just rewrite the file within the minute.
	bool DeleteSave(const FString& Level, const FString& Name)
	{
		if (Name.IsEmpty()) return false;
		if (g_ss.bEditing && g_ss.CurrentLevel == Level && g_ss.CurrentSave == Name)
		{
			Notify(TEXT("That save is open right now - its autosave would just recreate the file. Open another map first, then delete it."));
			return false;
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
			const FString Path = FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/maps"), FString(Png));
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

	void SaveCurrent(bool bSilent)
	{
		if (g_ss.CurrentSave.IsEmpty()) { if (!bSilent) Notify(TEXT("Name and create your custom map first.")); return; }
		SaveSession(g_ss.CurrentLevel, g_ss.CurrentSave);
		if (!bSilent) Notify(FString::Printf(TEXT("Saved '%s'."), *g_ss.CurrentSave));
	}

	void CreateCustom(const FString& Name)
	{
		const FString Clean = Name.TrimStartAndEnd();
		if (Clean.IsEmpty()) { Notify(TEXT("Enter a name for your custom map first.")); return; }
		g_ss.CurrentSave = Clean;
		g_ss.bEditing = true;
		SaveSession(g_ss.CurrentLevel, g_ss.CurrentSave);
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
	bool ResetVolumeCenter()
	{
		AActor* Vol = GVolEdit.Volume.Get();
		const TArray<FVector>* L = Vol ? GVolumeLoops.Find(Vol) : nullptr;
		if (!L || L->Num() < 2) return false;
		const TArray<FVector> World = BF6_LoopToWorld(Vol, *L);
		FVector C = FVector::ZeroVector;
		for (const FVector& P : World) C += P;
		C /= (double)World.Num();
		if (FVector::Dist(C, Vol->GetActorLocation()) < 1.0) return false;   // already centred
		FScopedTransaction Tx(FText::FromString(TEXT("Reset Zone Center")));
		Vol->Modify();
		Vol->SetActorLocation(C);
		BF6_ApplyLoop(Vol, BF6_LoopToLocal(Vol, World));
		return true;
	}

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
			const double NewH = FMath::Clamp((Base.Z + t - GVolEdit.DragBottomZ) / 100.0, 0.5, 2000.0);
			// SetActorProp stretches the walls immediately; its transaction
			// folds into the drag's own, so one undo reverts the whole drag
			SetActorProp(Vol, TEXT("height"), FString::Printf(TEXT("%.2f"), NewH));
			GVolEdit.DragZ = GVolEdit.DragBottomZ + NewH * 100.0;
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
	}

	void ClearSelection()
	{
		if (GEditor) { GEditor->SelectNone(false, true, false); GEditor->NoteSelectionChange(); }
	}

	void EndZoneDotDrag()
	{
		if (GVolEdit.Drag != INDEX_NONE)
			if (AActor* Vol = GVolEdit.Volume.Get())
				BF6_WriteLoopTags(Vol);   // mirror the final shape for undo
		GVolEdit.Drag = INDEX_NONE;
		if (GVolEdit.bTx && GEditor) { GEditor->EndTransaction(); GVolEdit.bTx = false; }
	}

	// Ghost every placed/base actor NOT in Keep: translucent + unselectable.
	// The originals go into Out so BF6_GhostRestoreSet can undo it exactly.
	static void BF6_GhostAllExcept(const TSet<AActor*>& Keep, TArray<FBF6Ghosted>& Out)
	{
		UMaterialInterface* Ghost = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Ghost.M_Ghost"));
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
		Notify(TEXT("Link assignment cancelled."));
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
		if (!g_ss.bEditing) { Notify(TEXT("Open a custom map (a save) to place objects.")); return nullptr; }
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

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

	int32 AutoAssignObjIds(int32 StartId)
	{
		if (!GEditor) return 0;
		USelection* S = GEditor->GetSelectedActors(); if (!S) return 0;
		TArray<AActor*> Targets;
		for (int32 i = 0; i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
			{
				if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
				const FString Ty = BF6_ObjIdType(A);
				if (!Ty.IsEmpty() && BF6_TypeHasObjId(Ty)) Targets.Add(A);
			}
		if (Targets.Num() == 0) return 0;
		// left-to-right, then front-to-back sweep: predictable numbering for
		// rows of flags/spawns, like the community's auto-id tools
		Targets.Sort([](const AActor& A, const AActor& B)
		{
			const FVector PA = A.GetActorLocation(), PB = B.GetActorLocation();
			if (PA.X != PB.X) return PA.X < PB.X;
			return PA.Y < PB.Y;
		});
		FScopedTransaction Tx(FText::FromString(TEXT("Auto-assign ObjIds")));
		int32 Id = StartId;
		for (AActor* A : Targets) SetActorProp(A, TEXT("ObjId"), FString::FromInt(Id++));
		return Targets.Num();
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
		USelection* S = GEditor->GetSelectedActors(); if (!S) return false;
		for (int32 i = 0; i < S->Num(); i++)
			if (AActor* A = Cast<AActor>(S->GetSelectedObject(i)))
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
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 500), P - FVector(0, 0, 50000), ECC_Visibility, QP))
				P.Z = Hit.Location.Z;
			else if (W->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 50000), P - FVector(0, 0, 50000), ECC_Visibility, QP))
				P.Z = Hit.Location.Z;
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
	int32 OrganizeOutliner()
	{
		if (!GEditor) return 0;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return 0;
		int32 n = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
			if (It->Tags.Contains(kPlacedTag) || It->Tags.Contains(kBaseTag))
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
	static void BF6_EnsureCameraComponent(AActor* Cam)
	{
		if (!Cam || Cam->FindComponentByClass<UCameraComponent>()) return;
		UCameraComponent* C = NewObject<UCameraComponent>(Cam, TEXT("BF6CamView"));
		C->SetFlags(RF_Transient);
		C->bIsEditorOnly = true;
		C->SetupAttachment(Cam->GetRootComponent());
		C->RegisterComponent();
		C->SetRelativeRotation(FRotator(0.f, BF6_CamFacesPlusY(Cam) ? 90.f : -90.f, 0.f));
		C->SetFieldOfView(75.f);   // Godot Camera3D default, same as the PIP
		C->SetConstraintAspectRatio(false);
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
						if (BF6_IsCameraActor(A)) { Cam = A; break; }
		GCamTarget = Cam;
		if (Cam) BF6_EnsureCameraComponent(Cam);
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
		if (AActor* R = GCamRig.Get())
			R->SetActorLocationAndRotation(Cam->GetActorLocation(), BF6_CamViewQuat(Cam));
	}

	void SetCameraFromView()
	{
		AActor* Cam = GCamTarget.Get();
		FLevelEditorViewportClient* VC = GCurrentLevelEditingViewportClient;
		if (!Cam || !VC) return;
		FScopedTransaction Tx(FText::FromString(TEXT("Set Camera From View")));
		Cam->Modify();
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
		VC->SetViewLocation(Cam->GetActorLocation());
		VC->SetViewRotation(BF6_CamViewQuat(Cam).Rotator());
	}

	// ---- live scatter editor (the Proton Scatter feel) ----
	// Opened from the SCATTER pill: the scatter exists immediately and
	// REGENERATES as the sliders move. A fixed seed keeps the pattern stable
	// while dragging; New pattern re-rolls it. Apply rebuilds the exact
	// preview inside one transaction (single Ctrl+Z), Esc removes it all.
	struct FBF6ScatterLive
	{
		bool bActive = false;
		FBF6MultiUnit U;
		TArray<TWeakObjectPtr<AActor>> Preview;
		struct FTarget { FVector P; double Yaw = 0; double Tx = 0; double Ty = 0; double Scl = 1; };
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
		bool bDrawing = false;                // clicking out a custom outline right now
		TArray<FVector> Poly;                 // the drawn outline (world, >= 3 points)
	};
	static FBF6ScatterLive GScat;
	static int32 GScatSession = 0;   // bumps per session so the panel knows to reset
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

	static double BF6_ScatterGroundZ(UWorld* W, FCollisionQueryParams& QP, double X, double Y)
	{
		const FVector P(X, Y, GScat.U.Center.Z);
		if (GScat.bFollowTerrain)
		{
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 500), P - FVector(0, 0, 50000), ECC_Visibility, QP)) return Hit.Location.Z;
			if (W->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 50000), P - FVector(0, 0, 50000), ECC_Visibility, QP)) return Hit.Location.Z;
		}
		return GScat.U.Center.Z;
	}

	static void BF6_ScatterRegionRebuild()
	{
		BF6_ScatterRegionDestroy();
		if (!GScat.bActive || !GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		FCollisionQueryParams QP(FName(TEXT("BF6ScatterRegion")), true);
		for (AActor* M : GScat.U.Members) if (IsValid(M)) QP.AddIgnoredActor(M);
		for (TWeakObjectPtr<AActor>& Wk : GScat.Preview) if (AActor* A = Wk.Get()) QP.AddIgnoredActor(A);

		const double Lift = 20.0;
		const double R = FMath::Max(GScat.RadiusM, 1.f) * 100.0;
		const FVector C = GScat.U.Center;
		TArray<FVector> V; TArray<int32> T; TArray<FLinearColor> VC;
		auto Vert = [&](double X, double Y)
		{
			V.Add(FVector(X, Y, BF6_ScatterGroundZ(W, QP, X, Y) + Lift));
			VC.Add(FLinearColor(0.15f, 0.45f, 1.f, 0.35f));
		};
		auto Quad = [&](int32 a, int32 b, int32 c2, int32 d)
		{
			T.Append({ a, b, c2, a, c2, d, a, c2, b, a, d, c2 });   // both windings
		};
		const bool bDrawn = GScat.Shape == 3 && GScat.Poly.Num() >= 3;
		if (bDrawn)
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
					TArray<double> Xs;
					for (int32 i = 0, j = GScat.Poly.Num() - 1; i < GScat.Poly.Num(); j = i++)
					{
						const FVector& A = GScat.Poly[i];
						const FVector& B = GScat.Poly[j];
						if ((A.Y > Ym) != (B.Y > Ym))
							Xs.Add((B.X - A.X) * (Ym - A.Y) / (B.Y - A.Y) + A.X);
					}
					Xs.Sort();
					for (int32 s = 0; s + 1 < Xs.Num(); s += 2)
					{
						// subdivide long strips so they follow the terrain
						const double X0 = Xs[s], X1 = Xs[s + 1];
						const int32 SubN = FMath::Clamp((int32)((X1 - X0) / 400.0), 1, 24);
						for (int32 k = 0; k < SubN; k++)
						{
							const double Xa = FMath::Lerp(X0, X1, (double)k / SubN);
							const double Xb = FMath::Lerp(X0, X1, (double)(k + 1) / SubN);
							const int32 Base = V.Num();
							Vert(Xa, Y0); Vert(Xb, Y0); Vert(Xb, Y1); Vert(Xa, Y1);
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
					Vert(C.X + ((double)x / N * 2.0 - 1.0) * R, C.Y + ((double)y / N * 2.0 - 1.0) * R);
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
					Vert(C.X + FMath::Cos(Th) * Rr, C.Y + FMath::Sin(Th) * Rr);
				}
			for (int32 ri = 0; ri < Rad; ri++)
				for (int32 s = 0; s < Seg; s++)
				{
					const int32 a = Base + ri * (Seg + 1) + s;
					Quad(a, a + 1, a + Seg + 2, a + Seg + 1);
				}
		}
		if (V.Num() == 0) return;
		AActor* A = W->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
		if (!A) return;
		UProceduralMeshComponent* M = MakeProcMesh(A, TEXT("ScatterRegion"));
		const TArray<FVector> NN; const TArray<FVector2D> UV; const TArray<FProcMeshTangent> NT;
		M->CreateMeshSection_LinearColor(0, V, T, NN, UV, VC, NT, false);   // no collision: never blocks traces
		if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Volume.M_Volume")))
			M->SetMaterial(0, Mat);
		M->bSelectable = false;   // clicks pass straight through to the map
		A->SetActorLabel(TEXT("ScatterRegion"));
		A->SetFlags(RF_Transient);
		GScatRegion = A;
	}

	static void BF6_ScatterRegenerate()
	{
		if (!GScat.bActive || !GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		for (AActor* M : GScat.U.Members) if (!IsValid(M)) { GScat.bActive = false; BF6_ScatterDestroyPreview(); BF6_ScatterRegionDestroy(); return; }
		BF6_ScatterDestroyPreview();
		GScat.Targets.Reset();
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
		const double FitDist = FMath::Sqrt(Area / ((double)Count * 2.6));
		const double MinDist = FMath::Clamp(FMath::Min(FMath::Max(GScat.U.Size.X, GScat.U.Size.Y) * 0.7, FitDist), 50.0, FMath::Max(R, 100.0));
		FRandomStream Rng(GScat.Seed);
		FCollisionQueryParams QP(FName(TEXT("BF6ScatterLive")), true);
		for (AActor* M : GScat.U.Members) QP.AddIgnoredActor(M);
		TArray<FVector> Placed;
		Placed.Add(GScat.U.Center);
		TArray<AActor*> NewOnes;
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
			if (bDrawn)
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
			if (GScat.bFollowTerrain)
			{
				FHitResult Hit;
				if (W->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 500), P - FVector(0, 0, 50000), ECC_Visibility, QP))
					P.Z = Hit.Location.Z;
				else if (W->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 50000), P - FVector(0, 0, 50000), ECC_Visibility, QP))
					P.Z = Hit.Location.Z;
			}
			else P.Z = GScat.U.Center.Z;
			P.Z += Elev;
			if (BF6_SpawnUnitCopy(GScat.U, P, Yaw, Scl, QP, NewOnes, Tx, Ty) > 0)
			{
				Placed.Add(P);
				FBF6ScatterLive::FTarget T; T.P = P; T.Yaw = Yaw; T.Tx = Tx; T.Ty = Ty; T.Scl = Scl;
				GScat.Targets.Add(T);
				n++;
			}
		}
		for (AActor* A : NewOnes) GScat.Preview.Add(A);
		BF6_ScatterRegionRebuild();
		BF6_RecomputeBudget();
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
		GScat.Shape = Shape;
		if (Shape != 3) GScat.bDrawing = false;
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
		GScatDotDrag = INDEX_NONE;
		GScatPolyUndo.Reset();
		GScatPolyRedo.Reset();
		BF6_ScatterRegionDestroy();
		for (AActor* M : GScat.U.Members) if (!IsValid(M)) { BF6_ScatterDestroyPreview(); return; }
		// swap the preview for an identical, UNDOABLE rebuild
		BF6_ScatterDestroyPreview();
		FScopedTransaction Tx(FText::FromString(TEXT("Scatter")));
		FCollisionQueryParams QP(FName(TEXT("BF6ScatterLive")), true);
		TArray<AActor*> NewOnes;
		for (const FBF6ScatterLive::FTarget& T : GScat.Targets)
			BF6_SpawnUnitCopy(GScat.U, T.P, T.Yaw, T.Scl, QP, NewOnes, T.Tx, T.Ty);
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
				if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag)) continue;
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
		if (GIsSavingPackage || IsGarbageCollecting()) return nullptr;
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
		if (GIsSavingPackage || IsGarbageCollecting()) return false;
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
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)) continue;
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
			if (!It->Tags.Contains(kPlacedTag) && !It->Tags.Contains(kBaseTag)) continue;
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
			FHitResult Hit;
			FCollisionQueryParams QP(FName(TEXT("BF6Pick")), true);
			if (W->LineTraceSingleByChannel(Hit, O, End, ECC_Visibility, QP)
				&& (double)Hit.Time < BestT - 1e-4)
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
			GEditor->BeginTransaction(FText::FromString(TEXT("Move")));
			for (TWeakObjectPtr<AActor>& Wk : GDragMove.Movers)
				if (AActor* M = Wk.Get()) M->Modify();
		}
		for (TWeakObjectPtr<AActor>& Wk : GDragMove.Movers)
			if (AActor* M = Wk.Get())
				M->SetActorLocation(M->GetActorLocation() + Delta);
		GDragMove.GrabW = W;
	}

	void EndDragMove()
	{
		if (GDragMove.bMoving && GEditor)
		{
			GEditor->EndTransaction();
			GEditor->NoteSelectionChange();
		}
		GDragMove = FBF6DragMove();
	}

	void CancelDragMove()
	{
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
		TArray<FVector> Offsets;   // per mover, from the reference ground point
		FVector Ref = FVector::ZeroVector;
	};
	static FBF6PickPlace GPickPlace;

	bool IsPickPlacing() { return GPickPlace.bActive; }

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
		}
		GEditor->BeginTransaction(FText::FromString(TEXT("Pick Place")));
		for (TWeakObjectPtr<AActor>& Wk : GPickPlace.Movers)
			if (AActor* M = Wk.Get()) M->Modify();
		GPickPlace.bActive = true;
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
		for (int32 i = 0; i < GPickPlace.Movers.Num(); i++)
			if (AActor* M = GPickPlace.Movers[i].Get())
				M->SetActorLocation(W + GPickPlace.Offsets[i]);
		GPickPlace.Ref = W;
	}

	void FinishPickPlace()
	{
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
		USelection* Sel = GEditor->GetSelectedActors(); if (!Sel) return 0;
		TArray<AActor*> Picked;
		for (int32 i = 0; i < Sel->Num(); i++)
			if (AActor* A = Cast<AActor>(Sel->GetSelectedObject(i)))
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
		if (!g_ss.bEditing) { Notify(TEXT("Open a custom map (a save) to place objects.")); return false; }
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

	TArray<FLintItem> RunLint()
	{
		TArray<FLintItem> Out;
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

			if (Ty == TEXT("CombatArea"))
			{
				LinkTargets(A, TEXT("CombatVolume"), T, bLegacy);
				if (T.Num() == 0 && !bLegacy)
					Add(1, A, TEXT("Combat area has no combat volume linked - the whole map counts as out of bounds."));
				for (AActor* Vol : T)
				{
					TArray<FVector> Loop;
					if (!LoopOf(Vol, Loop)) continue;
					CombatLoop = Loop;
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
					if (VolHeight(Vol) <= 0.01)
						Add(0, Vol, TEXT("Capture area height is 0 - a flat volume never registers players. Set a height (10 works)."));
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
					if (VolHeight(Vol) <= 0.01)
						Add(0, Vol, TEXT("Area trigger volume height is 0 - it will never fire. Set a height (10 works)."));
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
					Add(1, A, FString::Printf(TEXT("Non-uniform scale (%.2f, %.2f, %.2f) - collision desyncs from the visual mesh in game. Scale uniformly."), Sc.X, Sc.Y, Sc.Z));
			}
		}

		// HQs must sit OUTSIDE the combat volume (inside = instant out of bounds)
		if (CombatLoop.Num() >= 3)
			for (AActor* A : Ours)
				if (BF6_ObjIdType(A) == TEXT("HQ_PlayerSpawner") && PointInPoly(A->GetActorLocation(), CombatLoop))
					Add(1, A, TEXT("HQ sits INSIDE the combat volume - HQs belong outside it, inside the surrounding area."));

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

	FString ModeWizardBody()
	{
		if (!GWiz.bActive) return FString();
		if (GWiz.bConquest && GWiz.Step <= 1)
			return TEXT("Aim at the ground and click. You get the HQ, its protected area, and four linked spawn points. Keep it OUTSIDE the combat area walls.");
		if (GWiz.bConquest)
			return TEXT("Click where the flag goes. You get the capture point, its capture area, and four spawn points per team, all linked. The sector wires up automatically after the last flag.");
		const int32 sub = GWiz.Step % 4;
		if (sub <= 1)
			return TEXT("Aim at the ground and click. You get the HQ, its protected area, and four linked spawn points.");
		return TEXT("Click where the objective goes. You get the capture point, its capture area, and spawn points for both teams. The sector wires up after objective B.");
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
		return BF6_ActorLinkName(Cp);
	}

	static void BF6_WizSector(const FVector& Pos, int32 ObjIdVal, const FString& Prefix, const TArray<FString>& CpNames)
	{
		AActor* Sec = BF6_WizSpawn(TEXT("Sector"), Pos, 0, Prefix);
		if (!Sec) return;
		Sec->Tags.Add(FName(*FString::Printf(TEXT("p:ObjId=%d"), ObjIdVal)));
		if (CpNames.Num()) Sec->Tags.Add(FName(*(TEXT("p:CapturePoints=") + FString::Join(CpNames, TEXT(",")))));
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
		if (!g_ss.bEditing) { Notify(TEXT("Open a custom map (a save) first.")); return; }
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

	bool WorldFromViewportCursor(FVector& OutWorld)
	{
		if (!GCurrentLevelEditingViewportClient) return false;
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

	static bool TraceToSurface(const FVector& O, const FVector& D, FVector& OutWorld)
	{
		// Trace the actual map surface (the context meshes carry collision), so
		// placements land on the bridge deck / terrain under the crosshair instead
		// of a flat z=0 plane below the map.
		if (GEditor)
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				FHitResult Hit;
				FCollisionQueryParams QP(FName(TEXT("BF6PlaceTrace")), true);
				if (World->LineTraceSingleByChannel(Hit, O, O + D * 500000.f, ECC_Visibility, QP))
				{ OutWorld = Hit.Location; return true; }
			}
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
static void BF6_RepairAfterUndo()
{
	if (!GEditor) return;
	UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
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
		UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
		if (!M || M->GetNumSections() > 0) continue;
		if (A->Tags.Contains(kHandleTag)) { BuildHandleCube(M); ApplyHandleStyle(M); continue; }
		if (const TArray<FVector>* Loop = GVolumeLoops.Find(A)) { RebuildVolumeWalls(A, *Loop); continue; }
		FString Mesh = TagValue(A, TEXT("mesh:"));
		if (Mesh.IsEmpty()) Mesh = TagValue(A, TEXT("type:"));
		if (!Mesh.IsEmpty() && FillProcFromBf6Mesh(M, ObjModelPath(Mesh))) { ApplyObjectWhite(M); continue; }
		BuildMarker(M); ApplyObjectWhite(M);
	}
	BF6_RecomputeBudget();
}

void FBF6UnrealSDKModule::StartupModule()
{
	g_postUndoHandle = FEditorDelegates::PostUndoRedo.AddStatic(&BF6_RepairAfterUndo);
	g_pluginDir = IPluginManager::Get().FindPlugin(TEXT("BF6UnrealSDK"))->GetBaseDir();
	BF6_LoadCatOverrides();   // the user's "move to category" choices

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

	// One-time migration: saves/exports from before the plugin was renamed.
	{
		const FString OldDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6") TEXT("HighPoly"));
		const FString NewDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"));
		if (IFileManager::Get().DirectoryExists(*OldDir) && !IFileManager::Get().DirectoryExists(*NewDir))
			IFileManager::Get().Move(*NewDir, *OldDir);
	}
	const FString DllPath = FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/bin/Win64/bf6_core.dll"));
	DllHandle = FPlatformProcess::GetDllHandle(*DllPath);
	if (!DllHandle) { UE_LOG(LogBF6, Error, TEXT("could not load bf6_core.dll")); return; }
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
	if (!g_ctx)
	{
		// No game install on this machine (or not at the Steam path). Fall back
		// to libbf6's no-install mode: mesh decode is unavailable, but the
		// placeable catalogue is SDK data and works fully without the game.
		UE_LOG(LogBF6, Log, TEXT("no game install (%hs); running catalogue-only"), err);
		g_ctx = g_open("", err, sizeof(err));
	}
	if (!g_ctx) { UE_LOG(LogBF6, Warning, TEXT("bf6_open failed: %hs"), err); }
	else
	{
		UE_LOG(LogBF6, Warning, TEXT("libbf6 opened the install: %d resources."), g_cat(g_ctx, "", nullptr, 0));
		if (g_loadp)
		{
			const FString FbDir = FPaths::Combine(g_pluginDir, TEXT("Source/ThirdParty/libbf6/data/FbExportData"));
			char perr[256] = {0};
			const int np = g_loadp(g_ctx, TCHAR_TO_UTF8(*FbDir), perr, sizeof(perr));
			if (np > 0) { UE_LOG(LogBF6, Warning, TEXT("SDK placeables loaded: %d objects across %d levels. Open Window > Tools > BF6 Objects."), np, g_lvlcnt ? g_lvlcnt(g_ctx) : 0); }
			else { UE_LOG(LogBF6, Warning, TEXT("bf6_load_placeables failed: %hs"), perr); }
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
					// the scene tree stays: it's how users select like in Godot
					TM->TryInvokeTab(FTabId(TEXT("LevelEditorSceneOutliner")));
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
	// Park the download toast (never destruct widgets during exit).
	if (GUpdateToast.IsValid()) { new TSharedPtr<SNotificationItem>(GUpdateToast); GUpdateToast.Reset(); }
	BF6Api::DetachUI();
	BF6Api::RemoveInputHandler();
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
