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
#include "UObject/StrongObjectPtr.h"
#include "Styling/SlateBrush.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
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
	Actor->SetActorLabel(FString::Printf(TEXT("BF6_%s"), *(Label.IsEmpty() ? ResName : Label)));
	Actor->Tags.Add(kPlacedTag);
	Actor->Tags.Add(FName(*(FString(TEXT("res:")) + ResName)));
	if (!Label.IsEmpty()) Actor->Tags.Add(FName(*(FString(TEXT("label:")) + Label)));

	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Actor, TEXT("ProcMesh"));
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
	if (!Placed) { Actor->Tags.Remove(kPlacedTag); Actor->Tags.Add(kContextTag); }
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
	Actor->SetActorLabel(FString::Printf(TEXT("BF6_%s"), *(Label.IsEmpty() ? MeshName : Label)));
	Actor->Tags.Add(kPlacedTag);
	Actor->Tags.Add(FName(*(FString(TEXT("mesh:")) + MeshName)));
	if (!Label.IsEmpty()) Actor->Tags.Add(FName(*(FString(TEXT("label:")) + Label)));
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

static TMap<TWeakObjectPtr<AActor>, TArray<FVector>> GVolumeLoops;

struct FBF6VolEdit
{
	TWeakObjectPtr<AActor> Volume;
	TArray<TWeakObjectPtr<AActor>> Handles;   // vertex order
	TArray<FVector> LastLoop;
};
static FBF6VolEdit GVolEdit;

struct FBF6LinkPick
{
	TWeakObjectPtr<AActor> Owner;
	FString Prop;
	bool bArray = false;
	bool bActive = false;
};
static FBF6LinkPick GLinkPick;

// A small centered cube the user can grab with the normal move gizmo.
static void BuildHandleCube(UProceduralMeshComponent* M)
{
	const float r = 35.f;
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
	ApplyObjectWhite(M);
	A->SetActorLocation(Pos);
	A->SetActorLabel(FString::Printf(TEXT("BF6_Point_%d"), Idx));
	A->Tags.Add(kHandleTag);
	A->SetFlags(RF_Transient);
	return A;
}

static void RebuildVolumeWalls(AActor* Vol, const TArray<FVector>& Loop)
{
	UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(Vol->GetRootComponent());
	if (!M) return;
	// the zone's real height (Godot metres) when it carries one
	double H = 5.0;
	const FString HS = BF6Api::GetActorProp(Vol, TEXT("height"));
	if (HS.IsNumeric()) H = FCString::Atod(*HS);
	M->ClearAllMeshSections();
	BuildWalls(M, Loop, (float)FMath::Max(H, 0.5) * 100.f);
	if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Volume.M_Volume")))
		M->SetMaterial(0, Mat);
}

static bool GatherHandleLoop(TArray<FVector>& Out)
{
	Out.Reset();
	for (const TWeakObjectPtr<AActor>& H : GVolEdit.Handles)
	{
		if (!H.IsValid()) return false;
		Out.Add(H->GetActorLocation());
	}
	return Out.Num() >= 3;
}

// index of the currently selected handle in the edit session (else the last)
static int32 SelectedHandleIndex()
{
	int32 Sel = GVolEdit.Handles.Num() - 1;
	if (GEditor)
		for (int32 i = 0; i < GVolEdit.Handles.Num(); i++)
			if (GVolEdit.Handles[i].IsValid() && GVolEdit.Handles[i]->IsSelected()) { Sel = i; break; }
	return Sel;
}

// ---- session save / load (JSON of placed objects) ----
static FString SessionDir(const FString& Level)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), Level);
}

static TArray<FString> ListSaves(const FString& Level)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(SessionDir(Level) / TEXT("*.json")), true, false);
	for (FString& F : Files) F = FPaths::GetBaseFilename(F);
	return Files;
}

static FString TagValue(AActor* A, const FString& Prefix)
{
	for (const FName& T : A->Tags) { FString S = T.ToString(); if (S.StartsWith(Prefix)) return S.RightChop(Prefix.Len()); }
	return FString();
}

static void SaveSession(const FString& Level, const FString& Name)
{
	if (!GEditor || Name.IsEmpty()) return;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return;
	TArray<TSharedPtr<FJsonValue>> Objs;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(kPlacedTag)) continue;
		const FString MeshName = TagValue(*It, TEXT("mesh:"));
		if (MeshName.IsEmpty()) continue;
		const FTransform Xf = It->GetActorTransform();
		const FVector L = Xf.GetLocation(); const FRotator R = Xf.Rotator(); const FVector S = Xf.GetScale3D();
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("mesh"), MeshName);
		O->SetStringField(TEXT("label"), TagValue(*It, TEXT("label:")));
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
		Objs.Add(MakeShared<FJsonValueObject>(O));
	}
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("level"), Level);
	Root->SetArrayField(TEXT("objects"), Objs);
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), W);
	const FString Path = SessionDir(Level) / (Name + TEXT(".json"));
	FFileHelper::SaveStringToFile(Out, *Path);
	UE_LOG(LogBF6, Warning, TEXT("Saved %d object(s) to %s"), Objs.Num(), *Path);
}

static void LoadSession(const FString& Level, const FString& Name)
{
	if (Name.IsEmpty()) return;
	const FString Path = SessionDir(Level) / (Name + TEXT(".json"));
	FString In;
	if (!FFileHelper::LoadFileToString(In, *Path)) { UE_LOG(LogBF6, Warning, TEXT("no save at %s"), *Path); return; }
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
	ClearActorsWithTag(kPlacedTag);
	const TArray<TSharedPtr<FJsonValue>>* Objs = nullptr;
	if (!Root->TryGetArrayField(TEXT("objects"), Objs)) return;
	int n = 0;
	for (const auto& V : *Objs)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) continue;
		const FString MeshName = O->GetStringField(TEXT("mesh"));
		const FString Label = O->GetStringField(TEXT("label"));
		FVector L(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")), O->GetNumberField(TEXT("z")));
		FRotator Rot(O->GetNumberField(TEXT("pitch")), O->GetNumberField(TEXT("yaw")), O->GetNumberField(TEXT("roll")));
		FVector Sc(O->GetNumberField(TEXT("sx")), O->GetNumberField(TEXT("sy")), O->GetNumberField(TEXT("sz")));
		if (AActor* A = SpawnSdkModel(MeshName, Label, FTransform(Rot, L, Sc)))
		{
			// restore edited attribute values
			const TArray<TSharedPtr<FJsonValue>>* PTags = nullptr;
			if (O->TryGetArrayField(TEXT("props"), PTags))
				for (const auto& PV : *PTags)
				{
					FString KV;
					if (PV->TryGetString(KV) && !KV.IsEmpty()) A->Tags.Add(FName(*(FString(TEXT("p:")) + KV)));
				}
			n++;
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
		auto WorldG = [&](FString nm)
		{
			FVector w(0,0,0); int g = 0;
			while (!nm.IsEmpty() && nm != TEXT(".") && g++ < 8)
			{ if (const FVector* lp = LocalPos.Find(nm)) w += *lp; const FString* pp = ParentOf.Find(nm); nm = pp ? *pp : FString(); }
			return w;
		};
		auto ToUnreal = [](const FVector& G){ return FVector((float)G.X, (float)G.Z, (float)G.Y) * 100.f; };  // Godot -> Unreal
		// Godot's `basis` is the row-major Transform3D matrix; its COLUMNS are the
		// local axes. Converting a rotation across the Y/Z swap is a similarity by an
		// orthogonal matrix (preserves rotation), so the Unreal axes are the Godot
		// columns with Y/Z swapped: ActorX=Swap(X_g), ActorY=Swap(Z_g), ActorZ=Swap(Y_g),
		// Swap(v)=(v.x,v.z,v.y). Without this every spawner faced identity (all one way).
		auto BasisToRot = [](const TArray<TSharedPtr<FJsonValue>>* b) -> FRotator
		{
			if (!b || b->Num() < 9) return FRotator::ZeroRotator;
			auto N = [&](int i){ return (float)(*b)[i]->AsNumber(); };
			const FVector Ax(N(0), N(6), N(3));   // Swap( X_g = col0 = b0,b3,b6 )
			const FVector Ay(N(2), N(8), N(5));   // Swap( Z_g = col2 = b2,b5,b8 )
			const FVector Az(N(1), N(7), N(4));   // Swap( Y_g = col1 = b1,b4,b7 )
			return FMatrix(Ax, Ay, Az, FVector::ZeroVector).Rotator();
		};

		int32 oid = 0, spawned = 0;
		for (const auto& v : *Objs)
		{
			const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
			const FString nm = o->GetStringField(TEXT("name"));
			const FString ty = o->GetStringField(TEXT("type"));
			const FVector gw = WorldG(nm);
			const TArray<TSharedPtr<FJsonValue>>* bz = nullptr;
			o->TryGetArrayField(TEXT("basis"), bz);
			const FRotator Rot = BasisToRot(bz);

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
			A->SetActorLabel(FString::Printf(TEXT("BF6_%s"), *nm));
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
			TMap<FString,FVector> LP; TMap<FString,FString> PA;
			for (const auto& v : *BObjs){ const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue; const FString nm=o->GetStringField(TEXT("name")); FVector p(0,0,0); const TArray<TSharedPtr<FJsonValue>>* a=nullptr; if(o->TryGetArrayField(TEXT("pos"),a)&&a->Num()>=3){p.X=(*a)[0]->AsNumber();p.Y=(*a)[1]->AsNumber();p.Z=(*a)[2]->AsNumber();} LP.Add(nm,p); FString par; o->TryGetStringField(TEXT("parent"),par); PA.Add(nm,par); }
			auto WorldG=[&](FString nm){ FVector w(0,0,0); int g=0; while(!nm.IsEmpty()&&nm!=TEXT(".")&&g++<8){ if(const FVector* lp=LP.Find(nm)) w+=*lp; const FString* pp=PA.Find(nm); nm=pp?*pp:FString(); } return w; };
			for (const auto& v : *BObjs)
			{
				const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue;
				const FString nm=o->GetStringField(TEXT("name")), ty=o->GetStringField(TEXT("type"));
				if (ty.StartsWith(TEXT("MP_"))) continue;   // terrain/assets -> Static
				const FVector gw=WorldG(nm);
				TSharedPtr<FJsonObject> e = MakeShared<FJsonObject>();
				e->SetStringField(TEXT("name"), ShortName(nm));
				e->SetStringField(TEXT("type"), ty);
				const TArray<TSharedPtr<FJsonValue>>* b=nullptr;
				if (o->TryGetArrayField(TEXT("basis"), b) && b->Num()>=9)
				{
					// Portal right/up/front are the basis COLUMNS (Godot local axes),
					// not the row-major storage order. Emit columns so it matches the
					// real Portal format and re-imports correctly.
					e->SetObjectField(TEXT("right"), Vec((*b)[0]->AsNumber(),(*b)[3]->AsNumber(),(*b)[6]->AsNumber()));
					e->SetObjectField(TEXT("up"),    Vec((*b)[1]->AsNumber(),(*b)[4]->AsNumber(),(*b)[7]->AsNumber()));
					e->SetObjectField(TEXT("front"), Vec((*b)[2]->AsNumber(),(*b)[5]->AsNumber(),(*b)[8]->AsNumber()));
				}
				else { e->SetObjectField(TEXT("right"),Vec(1,0,0)); e->SetObjectField(TEXT("up"),Vec(0,1,0)); e->SetObjectField(TEXT("front"),Vec(0,0,1)); }
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
				A->SetActorLabel(FString::Printf(TEXT("BF6_%s"), *Type));
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
				A->SetActorLabel(FString::Printf(TEXT("BF6_%s"), *Type));
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

// Top-level directory segment = the radial category ("Props/Vehicles" -> "Props").
static FString BF6_TopSeg(const FString& Dir)
{
	if (Dir.IsEmpty()) return TEXT("Uncategorized");
	FString L, Rr;
	if (Dir.Split(TEXT("/"), &L, &Rr) || Dir.Split(TEXT("\\"), &L, &Rr)) return L;
	return Dir;
}

static void BF6_LoadPlaceables(const FString& Level)
{
	g_ss.AllItems.Reset(); g_ss.TypeCost.Reset(); g_ss.TypeToMesh.Reset();
	if (!g_ctx || !g_listp) return;
	const int32 kMax = 4000;
	TArray<bf6_placeable> Buf; Buf.SetNum(kMax);
	const int total = g_listp(g_ctx, Level.IsEmpty() ? "" : TCHAR_TO_UTF8(*Level), "", Buf.GetData(), kMax);
	const int n = FMath::Min(total, kMax);
	for (int i = 0; i < n; i++)
	{
		TSharedPtr<FPlaceableRow> r = MakeShared<FPlaceableRow>();
		r->Type = UTF8_TO_TCHAR(Buf[i].type); r->Directory = UTF8_TO_TCHAR(Buf[i].directory);
		r->Mesh = UTF8_TO_TCHAR(Buf[i].mesh); r->PhysicsCost = Buf[i].physics_cost; r->Universal = Buf[i].universal != 0;
		g_ss.AllItems.Add(r);
		g_ss.TypeCost.Add(r->Type, r->PhysicsCost);
		if (!r->Mesh.IsEmpty()) { g_ss.TypeCost.Add(r->Mesh, r->PhysicsCost); g_ss.TypeToMesh.Add(r->Type, r->Mesh); }
	}
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
	g_ss.BudgetFrac = bCap ? FMath::Clamp((float)g_ss.TotalCost / (float)g_ss.BudgetMax, 0.f, 1.f) : 0.f;
	if (bCap)
	{
		g_ss.BudgetColor = BF6Theme::BudgetFill(g_ss.BudgetFrac);
		g_ss.BudgetText = FText::FromString(FString::Printf(TEXT("%s / %s   (%d%%)   %d obj"),
			*FText::AsNumber(g_ss.TotalCost).ToString(), *FText::AsNumber(g_ss.BudgetMax).ToString(),
			FMath::RoundToInt(g_ss.BudgetFrac * 100.f), g_ss.TotalObjects));
	}
	else
	{
		g_ss.BudgetColor = BF6Theme::BudgetLow;
		g_ss.BudgetText = FText::FromString(FString::Printf(TEXT("%s physics cost   (no cap)   %d obj"),
			*FText::AsNumber(g_ss.TotalCost).ToString(), g_ss.TotalObjects));
	}
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
	auto WorldG = [&](FString nm){ FVector w(0,0,0); int g=0; while(!nm.IsEmpty()&&nm!=TEXT(".")&&g++<8){ if(const FVector* lp=LocalPos.Find(nm)) w+=*lp; const FString* pp=ParentOf.Find(nm); nm=pp?*pp:FString(); } return w; };
	auto ToUnreal = [](const FVector& G){ return FVector((float)G.X,(float)G.Z,(float)G.Y)*100.f; };
	auto BasisToRot = [](const TArray<TSharedPtr<FJsonValue>>* b)->FRotator{ if(!b||b->Num()<9) return FRotator::ZeroRotator; auto N=[&](int i){return (float)(*b)[i]->AsNumber();}; const FVector Ax(N(0),N(6),N(3)); const FVector Ay(N(2),N(8),N(5)); const FVector Az(N(1),N(7),N(4)); return FMatrix(Ax,Ay,Az,FVector::ZeroVector).Rotator(); };

	int32 oid = 0, spawned = 0;
	for (const auto& v : *Objs)
	{
		const TSharedPtr<FJsonObject> o = v->AsObject(); if (!o.IsValid()) continue;
		const FString nm = o->GetStringField(TEXT("name")); const FString ty = o->GetStringField(TEXT("type"));
		const FVector gw = WorldG(nm);
		const TArray<TSharedPtr<FJsonValue>>* bz = nullptr; o->TryGetArrayField(TEXT("basis"), bz);
		const FRotator Rot = BasisToRot(bz);
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
			if (A){ UProceduralMeshComponent* VM=MakeProcMesh(A,TEXT("Volume")); BuildWalls(VM,Loop,(float)FMath::Max(VolH,0.5)*100.f); if(UMaterialInterface* Mat=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Volume.M_Volume"))) VM->SetMaterial(0,Mat); GVolumeLoops.Add(A, Loop); }
		}
		else
		{
			A = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (A){ UProceduralMeshComponent* MM=MakeProcMesh(A,TEXT("Model")); if(!FillProcFromBf6Mesh(MM,ObjModelPath(ty))) BuildMarker(MM); ApplyObjectWhite(MM); }
		}
		if (!A) continue;
		if (!bVolume) A->SetActorTransform(FTransform(Rot, ToUnreal(gw), FVector::OneVector));
		A->SetActorLabel(FString::Printf(TEXT("BF6_%s"), *nm));
		A->Tags.Add(kBaseTag);
		A->Tags.Add(FName(*(FString(TEXT("type:")) + ty)));
		A->Tags.Add(FName(*(FString(TEXT("oid:")) + FString::FromInt(oid))));
		// seed the object's shipped field values (Team, ObjId, timers, links) so
		// the attribute radial edits real data
		const TSharedPtr<FJsonObject>* PObj = nullptr;
		if (o->TryGetObjectField(TEXT("props"), PObj) && PObj->IsValid())
			for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : (*PObj)->Values)
			{
				FString Val;
				if (KV.Value.IsValid() && KV.Value->TryGetString(Val) && !Val.IsEmpty())
					A->Tags.Add(FName(*FString::Printf(TEXT("p:%s=%s"), *KV.Key, *Val)));
			}
		A->SetFlags(RF_Transient);
		FBaseObj bo; bo.Type = ty; g_ss.BaseObjects.Add(oid, bo);
		oid++; spawned++;
	}
	UE_LOG(LogBF6, Warning, TEXT("Base setup loaded for %s: %d objects."), *Level, spawned);
}

static void BF6_OpenMapWorldImpl(const FString& Level, const FString& Save)
{
	if (!GEditor) return;
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
static void BF6_ExportSpatial(bool bMinify)
{
	if (!GEditor) return;
	UWorld* World = GEditor->GetEditorWorldContext().World(); if (!World) return;
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
		TMap<FString,FVector> LP; TMap<FString,FString> PA;
		for (const auto& v:*BObjs){ const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue; const FString nm=o->GetStringField(TEXT("name")); FVector p(0,0,0); const TArray<TSharedPtr<FJsonValue>>* a=nullptr; if(o->TryGetArrayField(TEXT("pos"),a)&&a->Num()>=3){p.X=(*a)[0]->AsNumber();p.Y=(*a)[1]->AsNumber();p.Z=(*a)[2]->AsNumber();} LP.Add(nm,p); FString par; o->TryGetStringField(TEXT("parent"),par); PA.Add(nm,par); }
		auto WorldG=[&](FString nm){ FVector w(0,0,0); int g=0; while(!nm.IsEmpty()&&nm!=TEXT(".")&&g++<8){ if(const FVector* lp=LP.Find(nm)) w+=*lp; const FString* pp=PA.Find(nm); nm=pp?*pp:FString(); } return w; };
		for (const auto& v:*BObjs)
		{
			const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue;
			const FString nm=o->GetStringField(TEXT("name")), ty=o->GetStringField(TEXT("type"));
			if (ty.StartsWith(TEXT("MP_"))) continue;
			const FVector gw=WorldG(nm);
			TSharedPtr<FJsonObject> e=MakeShared<FJsonObject>();
			e->SetStringField(TEXT("name"), ShortName(nm)); e->SetStringField(TEXT("type"), ty);
			const TArray<TSharedPtr<FJsonValue>>* b=nullptr;
			if (o->TryGetArrayField(TEXT("basis"),b)&&b->Num()>=9){ e->SetObjectField(TEXT("right"),Vec((*b)[0]->AsNumber(),(*b)[3]->AsNumber(),(*b)[6]->AsNumber())); e->SetObjectField(TEXT("up"),Vec((*b)[1]->AsNumber(),(*b)[4]->AsNumber(),(*b)[7]->AsNumber())); e->SetObjectField(TEXT("front"),Vec((*b)[2]->AsNumber(),(*b)[5]->AsNumber(),(*b)[8]->AsNumber())); }
			else { e->SetObjectField(TEXT("right"),Vec(1,0,0)); e->SetObjectField(TEXT("up"),Vec(0,1,0)); e->SetObjectField(TEXT("front"),Vec(0,0,1)); }
			e->SetObjectField(TEXT("position"), Vec(gw.X,gw.Y,gw.Z));
			e->SetStringField(TEXT("id"), ShortName(nm));
			AActor* Live = LiveByName.FindRef(nm);
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
					for (const FVector& Wv : *EditedLoop)
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
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(kPlacedTag)) continue;
		FString Type=TagValue(*It,TEXT("label:")); if(Type.IsEmpty())Type=TagValue(*It,TEXT("mesh:")); if(Type.IsEmpty())continue;
		const FTransform Xf=It->GetActorTransform(); const FVector L=Xf.GetLocation();
		const FVector Rr=Swap(Xf.GetUnitAxis(EAxis::X)), U=Swap(Xf.GetUnitAxis(EAxis::Z)), F=Swap(Xf.GetUnitAxis(EAxis::Y));
		TSharedPtr<FJsonObject> e=MakeShared<FJsonObject>();
		const FString nm=ShortName(FString::Printf(TEXT("placed_%d"),pid));
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
		Dynamic.Add(MakeShared<FJsonValueObject>(e)); pid++;
	}

	TArray<TSharedPtr<FJsonValue>> Static;
	const TCHAR* Suffixes[2]={ TEXT("_Terrain"), TEXT("_Assets") };
	for (const TCHAR* Suffix : Suffixes)
	{
		const FString snm=Level+Suffix; TSharedPtr<FJsonObject> e=MakeShared<FJsonObject>();
		e->SetStringField(TEXT("name"),snm); e->SetBoolField(TEXT("metadata/_edit_lock_"),true); e->SetStringField(TEXT("type"),snm);
		e->SetObjectField(TEXT("right"),Vec(1,0,0)); e->SetObjectField(TEXT("up"),Vec(0,1,0)); e->SetObjectField(TEXT("front"),Vec(0,0,1)); e->SetObjectField(TEXT("position"),Vec(0,0,0));
		e->SetStringField(TEXT("id"),FString::Printf(TEXT("Static/%s"),*snm));
		Static.Add(MakeShared<FJsonValueObject>(e));
	}

	TSharedPtr<FJsonObject> Root=MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("Portal_Dynamic"),Dynamic); Root->SetArrayField(TEXT("Static"),Static);
	FString Out; TSharedRef<TJsonWriter<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>> W=TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(),W);
	const FString Path=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("BF6UnrealSDK"),TEXT("export"),Level+TEXT(".spatial.json"));
	FFileHelper::SaveStringToFile(Out,*Path);
	Notify(FString::Printf(TEXT("Exported %d objects -> %s"), Dynamic.Num(), *Path));
	UE_LOG(LogBF6, Warning, TEXT("Exported spatial.json (%d dynamic): %s"), Dynamic.Num(), *Path);
}

// Import any Portal .spatial.json as an editable custom map (file dialog + data
// load). Free twin of SBF6Browser::OnImportSpatial, minus the tab UI bits.
static bool BF6_ImportSpatialDialog()
{
	if (!GEditor) return false;
	IDesktopPlatform* DP = FDesktopPlatformModule::Get(); if (!DP) { Notify(TEXT("File dialog unavailable.")); return false; }
	TArray<FString> Picked;
	const FString DefaultDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("export"));
	const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	if (!DP->OpenFileDialog(Parent, TEXT("Import Portal .spatial.json"), DefaultDir, TEXT(""), TEXT("Portal spatial (*.spatial.json)|*.spatial.json|JSON (*.json)|*.json"), EFileDialogFlags::None, Picked) || Picked.Num() == 0) return false;
	const FString File = Picked[0];

	FString In; if (!FFileHelper::LoadFileToString(In, *File)) { Notify(TEXT("Could not read the file.")); return false; }
	TSharedPtr<FJsonObject> Root; TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(In);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) { Notify(TEXT("Not valid JSON.")); return false; }

	FString Level; const TArray<TSharedPtr<FJsonValue>>* StaticArr = nullptr;
	if (Root->TryGetArrayField(TEXT("Static"), StaticArr))
		for (const auto& v : *StaticArr){ const TSharedPtr<FJsonObject> o=v->AsObject(); if(!o.IsValid())continue; FString nm; o->TryGetStringField(TEXT("type"),nm); if(nm.IsEmpty())o->TryGetStringField(TEXT("name"),nm); int32 cut; if(nm.FindLastChar('_',cut)){ Level=nm.Left(cut); break; } }
	if (Level.IsEmpty()) Level = g_ss.CurrentLevel;
	if (Level.IsEmpty()) { Notify(TEXT("Could not tell which map this file is for.")); return false; }

	const TArray<TSharedPtr<FJsonValue>>* Dyn = nullptr;
	if (!Root->TryGetArrayField(TEXT("Portal_Dynamic"), Dyn)) { Notify(TEXT("No Portal_Dynamic list.")); return false; }

	// editable custom map named after the file; file is authoritative (no base setup)
	g_ss.CurrentLevel = Level; g_ss.CurrentSave = FPaths::GetBaseFilename(File).Replace(TEXT(".spatial"), TEXT("")); g_ss.bEditing = true;
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
		if (Type.IsEmpty() || Type.StartsWith(TEXT("MP_"))) continue;
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
				A->SetActorLabel(FString::Printf(TEXT("BF6_%s"), *Type)); A->Tags.Add(kPlacedTag); A->Tags.Add(FName(*(FString(TEXT("label:"))+Type))); A->SetFlags(RF_Transient);
				RestoreProps(A, o);
				GVolumeLoops.Add(A, Loop);   // imported zones are point-editable too
				spawned++;
			}
			continue;
		}
		const FVector Rg=ReadVec(o,TEXT("right"),FVector(1,0,0)), Ug=ReadVec(o,TEXT("up"),FVector(0,1,0)), Fg=ReadVec(o,TEXT("front"),FVector(0,0,1));
		const FVector Ax=Swap(Rg), Ay=Swap(Fg), Az=Swap(Ug);
		const FTransform Xf(FMatrix(Ax,Ay,Az,FVector::ZeroVector).Rotator(), ToUnreal(gpos.X,gpos.Y,gpos.Z), FVector::OneVector);
		const FString Mesh = BF6_ResolveMeshForType(Type);
		AActor* A = Mesh.IsEmpty() ? nullptr : SpawnSdkModel(Mesh, Type, Xf);
		if (!A){ A=World->SpawnActor<AActor>(AActor::StaticClass(),FVector::ZeroVector,FRotator::ZeroRotator); if(!A)continue; UProceduralMeshComponent* MM=MakeProcMesh(A,TEXT("Model")); BuildMarker(MM); A->SetActorTransform(Xf); A->SetActorLabel(FString::Printf(TEXT("BF6_%s"),*Type)); A->Tags.Add(kPlacedTag); A->Tags.Add(FName(*(FString(TEXT("label:"))+Type))); A->SetFlags(RF_Transient); }
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
	FProcHandle Proc;
	int32   ObjTotal = 0, MapTotal = 0, LastCount = 0, Stagnant = 0;
	FString Status;
	float   Frac = 0.f;
	FTSTicker::FDelegateHandle Tick;
};
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
	const FRegexPattern KeyValPat(TEXT("^([A-Za-z_][A-Za-z0-9_]*) = (.+)$"));

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
			// scalar + link properties (raw strings, like the original extractor)
			TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
			FRegexMatcher PM(KeyValPat, Body);
			while (PM.FindNext())
			{
				const FString K = PM.GetCaptureGroup(1);
				if (K == TEXT("transform") || K == TEXT("points") || K.StartsWith(TEXT("metadata"))) continue;
				Props->SetStringField(K, PM.GetCaptureGroup(2).TrimStartAndEnd());
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

// ---- the generated Godot extraction scripts (skip-existing, so re-sync after
// an SDK update only converts what's new) ----
static FString BF6_ObjectScript(const FString& OutDir)
{
	FString S = TEXT(
		"extends SceneTree\n"
		"func _initialize():\n"
		"\tvar OUT := \"__OUT__\"\n"
		"\tDirAccess.make_dir_recursive_absolute(OUT)\n"
		"\tvar d := DirAccess.open(\"res://raw/models\")\n"
		"\tif d == null: print(\"no raw/models\"); quit(); return\n"
		"\tvar names: Array[String] = []\n"
		"\tfor f in d.get_files():\n"
		"\t\tif f.ends_with(\".glb.import\"): names.append(f.substr(0, f.length() - 7))\n"
		"\trandomize(); names.shuffle()\n"
		"\tvar did := 0\n"
		"\tfor gname in names:\n"
		"\t\tvar outp := \"%s/%s.bf6mesh\" % [OUT, gname.get_basename()]\n"
		"\t\tif FileAccess.file_exists(outp): continue\n"
		"\t\tvar path := \"res://raw/models/\" + gname\n"
		"\t\tif not ResourceLoader.exists(path): continue\n"
		"\t\tvar ps: PackedScene = ResourceLoader.load(path, \"\", ResourceLoader.CACHE_MODE_IGNORE)\n"
		"\t\tif ps == null: continue\n"
		"\t\tvar root := ps.instantiate()\n"
		"\t\t_dump(root, outp, false)\n"
		"\t\troot.free()\n"
		"\t\tdid += 1\n"
		"\t\tif did >= 1200: print(\"BATCH\"); quit(); return\n"
		"\tprint(\"DONE\"); quit()\n");
	S += TEXT(
		"func _dump(root: Node, out_path: String, use_global: bool) -> bool:\n"
		"\tvar meshes: Array = []\n"
		"\t_collect(root, meshes)\n"
		"\tvar surfaces: Array = []\n"
		"\tfor mi in meshes:\n"
		"\t\tvar m: Mesh = mi.mesh\n"
		"\t\tif m == null: continue\n"
		"\t\tvar xf: Transform3D = mi.global_transform if use_global else mi.transform\n"
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
		"func _collect(node: Node, out: Array) -> void:\n"
		"\tif node is MeshInstance3D and node.mesh != null: out.append(node)\n"
		"\tfor c in node.get_children(): _collect(c, out)\n");
	return S.Replace(TEXT("__OUT__"), *OutDir.Replace(TEXT("\\"), TEXT("/")));
}

static FString BF6_MapScript(const FString& OutDir)
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
	return S.Replace(TEXT("__OUT__"), *OutDir.Replace(TEXT("\\"), TEXT("/")));
}

static void BF6_ImportFail(const FString& Why)
{
	g_imp.Phase = FBF6Import::EPhase::Failed;
	g_imp.Status = Why;
	if (g_imp.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(g_imp.Tick); g_imp.Tick.Reset(); }
	Notify(FString::Printf(TEXT("SDK import failed: %s"), *Why));
}

static bool BF6_LaunchGodot(const FString& ScriptPath)
{
	const FString Args = FString::Printf(TEXT("--headless --path \"%s\" --script \"%s\""),
		*(g_imp.SdkRoot / TEXT("GodotProject")), *ScriptPath);
	g_imp.Proc = FPlatformProcess::CreateProc(*g_imp.GodotExe, *Args, false, true, true, nullptr, 0, nullptr, nullptr);
	return g_imp.Proc.IsValid();
}

static void BF6_ImportTickPhase()
{
	const FString ObjDir = BF6_DataDir() / TEXT("objmodels");
	const FString MapDir = BF6_DataDir() / TEXT("mapmesh");
	const FString ScriptDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("import"));
	const bool bObjects = g_imp.Phase == FBF6Import::EPhase::Objects;
	const FString OutDir = bObjects ? ObjDir : MapDir;
	const int32 Target = bObjects ? g_imp.ObjTotal : g_imp.MapTotal;
	const int32 Count = BF6_CountFiles(OutDir, TEXT("*.bf6mesh"));

	// still running: just report progress
	if (g_imp.Proc.IsValid() && FPlatformProcess::IsProcRunning(g_imp.Proc))
	{
		g_imp.Status = FString::Printf(TEXT("%s  %d / %d"), bObjects ? TEXT("Converting object models") : TEXT("Converting map meshes"), Count, Target);
		// objects are ~90%% of the work
		const float PhaseFrac = Target > 0 ? (float)Count / (float)Target : 0.f;
		g_imp.Frac = bObjects ? 0.05f + 0.80f * PhaseFrac : 0.85f + 0.15f * PhaseFrac;
		return;
	}
	if (g_imp.Proc.IsValid()) { FPlatformProcess::CloseProc(g_imp.Proc); g_imp.Proc = FProcHandle(); }

	// the batch/crash-resume loop: relaunch while progress is being made
	const bool bComplete = Count >= Target || (Count == g_imp.LastCount && ++g_imp.Stagnant >= 2);
	if (Count > g_imp.LastCount) g_imp.Stagnant = 0;
	g_imp.LastCount = Count;
	if (!bComplete)
	{
		const FString Script = ScriptDir / (bObjects ? TEXT("extract_objects.gd") : TEXT("extract_maps.gd"));
		if (!BF6_LaunchGodot(Script)) { BF6_ImportFail(TEXT("could not relaunch the SDK's Godot")); }
		return;
	}

	if (bObjects)
	{
		// objects finished (or gave all they can) - move to the map meshes
		UE_LOG(LogBF6, Warning, TEXT("Object models: %d of %d converted."), Count, Target);
		g_imp.Phase = FBF6Import::EPhase::Maps;
		g_imp.LastCount = BF6_CountFiles(MapDir, TEXT("*.bf6mesh"));
		g_imp.Stagnant = 0;
		if (!BF6_LaunchGodot(ScriptDir / TEXT("extract_maps.gd"))) BF6_ImportFail(TEXT("could not launch the SDK's Godot"));
		return;
	}

	// all done
	UE_LOG(LogBF6, Warning, TEXT("Map meshes: %d of %d converted."), Count, Target);
	g_imp.Phase = FBF6Import::EPhase::Done;
	g_imp.Status = TEXT("Import complete");
	g_imp.Frac = 1.f;
	if (g_imp.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(g_imp.Tick); g_imp.Tick.Reset(); }
	Notify(TEXT("SDK import complete - all maps and models are ready."));
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
	FString Ps;
	Ps += FString::Printf(TEXT("try { Wait-Process -Id %u -ErrorAction SilentlyContinue } catch {}\r\n"), Pid);
	Ps += TEXT("Start-Sleep -Seconds 2\r\n");
	Ps += FString::Printf(TEXT("Remove-Item -Recurse -Force \"%s\" -ErrorAction SilentlyContinue\r\n"), *Staging);
	Ps += FString::Printf(TEXT("Expand-Archive -Path \"%s\" -DestinationPath \"%s\" -Force\r\n"), *ZipPath, *Staging);
	// the zip may carry a BF6UnrealSDK/ root folder or the plugin files directly
	Ps += FString::Printf(TEXT("$src = Join-Path \"%s\" \"BF6UnrealSDK\"\r\n"), *Staging);
	Ps += TEXT("if (-not (Test-Path $src)) { $src = \"") + Staging + TEXT("\" }\r\n");
	Ps += FString::Printf(TEXT("robocopy $src \"%s\" /E /NFL /NDL /NJH /NJS\r\n"), *PluginDir);
	Ps += FString::Printf(TEXT("Start-Process \"%s\"\r\n"), *Project);
	FFileHelper::SaveStringToFile(Ps, *Script);

	FPlatformProcess::CreateProc(TEXT("powershell.exe"),
		*FString::Printf(TEXT("-ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\""), *Script),
		true, false, false, nullptr, 0, nullptr, nullptr);
	UE_LOG(LogBF6, Warning, TEXT("Update %s staged - closing the editor to apply."), *Tag);
	FPlatformMisc::RequestExit(false);
}

static void BF6_DownloadUpdate(const FString& Url, const FString& Tag)
{
	Notify(FString::Printf(TEXT("Downloading update %s..."), *Tag));
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("User-Agent"), TEXT("BF6UnrealSDK"));
	Req->OnProcessRequestComplete().BindLambda([Tag](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
	{
		if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() != 200)
		{ Notify(TEXT("Update download failed - try again later.")); return; }
		BF6_StageUpdateAndRestart(Resp->GetContent(), Tag);
	});
	Req->ProcessRequest();
}

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
		IFileManager::Get().Copy(*(BF6_DataDir() / TEXT("sdk.version.json")), *(g_imp.SdkRoot / TEXT("sdk.version.json")));
		BF6_ExtractBaseSetups(g_imp.SdkRoot);
		if (g_ctx && g_loadp)   // refresh the placeable catalogue from the new jsons
		{
			char perr[256] = {0};
			g_loadp(g_ctx, TCHAR_TO_UTF8(*Fb), perr, sizeof(perr));
		}

		// write the extraction scripts + count the work
		const FString ScriptDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BF6UnrealSDK"), TEXT("import"));
		IFileManager::Get().MakeDirectory(*ScriptDir, true);
		FFileHelper::SaveStringToFile(BF6_ObjectScript(BF6_DataDir() / TEXT("objmodels")), *(ScriptDir / TEXT("extract_objects.gd")));
		FFileHelper::SaveStringToFile(BF6_MapScript(BF6_DataDir() / TEXT("mapmesh")),      *(ScriptDir / TEXT("extract_maps.gd")));
		g_imp.ObjTotal = BF6_CountFiles(g_imp.SdkRoot / TEXT("GodotProject/raw/models"), TEXT("*.glb.import"));
		g_imp.MapTotal = 2 * BF6_CountFiles(g_imp.SdkRoot / TEXT("GodotProject/static"), TEXT("*_Terrain.tscn"));
		IFileManager::Get().MakeDirectory(*(BF6_DataDir() / TEXT("objmodels")), true);
		IFileManager::Get().MakeDirectory(*(BF6_DataDir() / TEXT("mapmesh")), true);

		// phase 1: object models via the SDK's own Godot, headless
		g_imp.Phase = FBF6Import::EPhase::Objects;
		g_imp.LastCount = BF6_CountFiles(BF6_DataDir() / TEXT("objmodels"), TEXT("*.bf6mesh"));
		if (!BF6_LaunchGodot(ScriptDir / TEXT("extract_objects.gd"))) { BF6_ImportFail(TEXT("could not launch the SDK's Godot")); return; }
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
	FString StoredSdkRoot()
	{
		FString P;
		GConfig->GetString(TEXT("BF6UnrealSDK"), TEXT("SdkRoot"), P, GEditorPerProjectIni);
		return P;
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
			const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
			if (Root->TryGetArrayField(TEXT("assets"), Assets))
				for (const auto& v : *Assets)
				{
					const TSharedPtr<FJsonObject> a = v->AsObject(); if (!a.IsValid()) continue;
					FString Nm, Url;
					a->TryGetStringField(TEXT("name"), Nm);
					a->TryGetStringField(TEXT("browser_download_url"), Url);
					if (!Nm.EndsWith(TEXT(".zip"))) continue;
					if (AssetUrl.IsEmpty() || Nm.Contains(TEXT("Plugin"))) { AssetUrl = Url; AssetName = Nm; }
					if (Nm.Contains(TEXT("Plugin"))) break;
				}
			if (AssetUrl.IsEmpty())
			{ if (bManual) Notify(FString::Printf(TEXT("%s is out, but has no plugin package attached yet."), *Tag)); return; }

			const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(
				TEXT("BF6 Unreal SDK %s is available (you have v%s).\n\nDownload now? The editor will close to apply the update and reopen when it's done."),
				*Tag, *Local)));
			if (Choice == EAppReturnType::Yes) BF6_DownloadUpdate(AssetUrl, Tag);
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
		for (const auto& r : g_ss.AllItems) Counts.FindOrAdd(BF6_TopSeg(r->Directory))++;
		Counts.ValueSort([](int32 A, int32 B){ return A > B; });
		TArray<FString> Out; for (const auto& P : Counts) Out.Add(P.Key);
		return Out;
	}
	int32 CategoryCount(const FString& Category)
	{
		int32 n = 0; for (const auto& r : g_ss.AllItems) if (BF6_TopSeg(r->Directory) == Category) n++;
		return n;
	}
	TArray<FPlaceableInfo> PlaceablesIn(const FString& Category, const FString& Query, int32 Max)
	{
		const FString P = Query.ToLower();
		TArray<TPair<int32, const FPlaceableRow*>> Scored;
		for (const auto& r : g_ss.AllItems)
		{
			if (BF6_TopSeg(r->Directory) != Category) continue;
			int32 s = 0;
			if (P.IsEmpty()) Scored.Emplace(0, r.Get());
			else if (FuzzyScore(P, r->Type, s)) Scored.Emplace(s + 20, r.Get());
			else { int32 sd=0; if (FuzzyScore(P, r->Directory, sd)) Scored.Emplace(sd, r.Get()); }
		}
		if (!P.IsEmpty()) Scored.Sort([](const TPair<int32,const FPlaceableRow*>& A, const TPair<int32,const FPlaceableRow*>& B){ return A.Key > B.Key; });
		TArray<FPlaceableInfo> Out;
		for (const auto& pr : Scored){ if (Out.Num() >= Max) break; FPlaceableInfo I; I.Type=pr.Value->Type; I.Directory=pr.Value->Directory; I.Mesh=pr.Value->Mesh; I.PhysicsCost=pr.Value->PhysicsCost; Out.Add(I); }
		return Out;
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

	void SaveCurrent()
	{
		if (g_ss.CurrentSave.IsEmpty()) { Notify(TEXT("Name and create your custom map first.")); return; }
		SaveSession(g_ss.CurrentLevel, g_ss.CurrentSave);
		Notify(FString::Printf(TEXT("Saved '%s'."), *g_ss.CurrentSave));
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
	}

	// ---- zone point editing ----
	bool IsVolumeActor(AActor* A) { return A && GVolumeLoops.Contains(A); }
	bool IsVolumeEditing() { return GVolEdit.Handles.Num() > 0; }

	void BeginVolumeEdit(AActor* Volume)
	{
		if (!GEditor || !Volume) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		FinishVolumeEdit();
		const TArray<FVector>* Loop = GVolumeLoops.Find(Volume);
		if (!Loop || Loop->Num() < 3) { Notify(TEXT("No editable points on this volume.")); return; }
		GVolEdit.Volume = Volume;
		GVolEdit.Handles.Reset();
		for (int32 i = 0; i < Loop->Num(); i++)
			GVolEdit.Handles.Add(SpawnVolumeHandle(W, (*Loop)[i], i));
		GVolEdit.LastLoop = *Loop;
		if (GVolEdit.Handles.Num() && GVolEdit.Handles[0].IsValid())
		{
			GEditor->SelectNone(false, true, false);
			GEditor->SelectActor(GVolEdit.Handles[0].Get(), true, true);
		}
		Notify(TEXT("Editing zone points: drag the handles with the gizmo. SPACE = add / delete / finish."));
	}

	void TickVolumeEdit()
	{
		if (!IsVolumeEditing()) return;
		AActor* Vol = GVolEdit.Volume.Get();
		if (!Vol) { GVolEdit = FBF6VolEdit(); return; }
		TArray<FVector> Loop;
		if (!GatherHandleLoop(Loop)) return;
		bool bChanged = Loop.Num() != GVolEdit.LastLoop.Num();
		if (!bChanged)
			for (int32 i = 0; i < Loop.Num(); i++)
				if (!Loop[i].Equals(GVolEdit.LastLoop[i], 0.5f)) { bChanged = true; break; }
		if (!bChanged) return;
		GVolEdit.LastLoop = Loop;
		RebuildVolumeWalls(Vol, Loop);
		GVolumeLoops.Add(Vol, Loop);
	}

	void VolumeAddPoint()
	{
		if (!IsVolumeEditing() || !GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		const int32 Sel = SelectedHandleIndex();
		const int32 Next = (Sel + 1) % GVolEdit.Handles.Num();
		if (!GVolEdit.Handles[Sel].IsValid() || !GVolEdit.Handles[Next].IsValid()) return;
		const FVector Mid = (GVolEdit.Handles[Sel]->GetActorLocation() + GVolEdit.Handles[Next]->GetActorLocation()) * 0.5f;
		FScopedTransaction Tx(FText::FromString(TEXT("Add Zone Point")));
		AActor* H = SpawnVolumeHandle(W, Mid, Sel + 1);
		if (!H) return;
		GVolEdit.Handles.Insert(H, Sel + 1);
		GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(H, true, true);
	}

	void VolumeAddPointAt(const FVector& WorldPos)
	{
		if (!IsVolumeEditing() || !GEditor) return;
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		TArray<FVector> Loop;
		if (!GatherHandleLoop(Loop)) return;
		// nearest edge segment to the click
		int32 Best = 0; float BestD = FLT_MAX; FVector BestP = WorldPos;
		for (int32 i = 0; i < Loop.Num(); i++)
		{
			const FVector A = Loop[i], B = Loop[(i + 1) % Loop.Num()];
			const FVector P = FMath::ClosestPointOnSegment(WorldPos, A, B);
			const float D = FVector::Dist2D(WorldPos, P);
			if (D < BestD) { BestD = D; Best = i; BestP = P; }
		}
		FScopedTransaction Tx(FText::FromString(TEXT("Add Zone Point")));
		AActor* H = SpawnVolumeHandle(W, BestP, Best + 1);
		if (!H) return;
		GVolEdit.Handles.Insert(H, Best + 1);
		GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(H, true, true);
	}

	void TickZoneAutoEdit()
	{
		if (!GEditor || !g_ss.bEditing) return;
		AActor* Sel = nullptr;
		if (USelection* S = GEditor->GetSelectedActors())
			for (int32 i = 0; i < S->Num() && !Sel; i++) Sel = Cast<AActor>(S->GetSelectedObject(i));

		if (IsVolumeEditing())
		{
			// keep the session while the zone or its handles are selected (or
			// nothing is - the user may just be orbiting); end it when the user
			// moves on to a different object
			const bool bKeep = !Sel || Sel == GVolEdit.Volume.Get() || Sel->Tags.Contains(kHandleTag);
			if (!bKeep) FinishVolumeEdit();
		}
		if (!IsVolumeEditing() && Sel && GVolumeLoops.Contains(Sel))
			BeginVolumeEdit(Sel);
	}

	void VolumeDeletePoint()
	{
		if (!IsVolumeEditing() || !GEditor) return;
		if (GVolEdit.Handles.Num() <= 3) { Notify(TEXT("A zone needs at least 3 points.")); return; }
		UWorld* W = GEditor->GetEditorWorldContext().World(); if (!W) return;
		const int32 Sel = SelectedHandleIndex();
		FScopedTransaction Tx(FText::FromString(TEXT("Delete Zone Point")));
		if (GVolEdit.Handles[Sel].IsValid()) W->EditorDestroyActor(GVolEdit.Handles[Sel].Get(), false);
		GVolEdit.Handles.RemoveAt(Sel);
		const int32 NewSel = FMath::Clamp(Sel, 0, GVolEdit.Handles.Num() - 1);
		if (GVolEdit.Handles[NewSel].IsValid())
		{
			GEditor->SelectNone(false, true, false);
			GEditor->SelectActor(GVolEdit.Handles[NewSel].Get(), true, true);
		}
	}

	void FinishVolumeEdit()
	{
		if (GVolEdit.Handles.Num() == 0) { GVolEdit = FBF6VolEdit(); return; }
		TArray<FVector> Loop;
		const bool bHave = GatherHandleLoop(Loop);
		if (AActor* Vol = GVolEdit.Volume.Get())
			if (bHave) { RebuildVolumeWalls(Vol, Loop); GVolumeLoops.Add(Vol, Loop); }
		if (GEditor)
			if (UWorld* W = GEditor->GetEditorWorldContext().World())
				for (const TWeakObjectPtr<AActor>& H : GVolEdit.Handles)
					if (H.IsValid()) W->EditorDestroyActor(H.Get(), false);
		GVolEdit = FBF6VolEdit();
	}

	// ---- link picking (assign spawn points / volumes) ----
	bool IsLinkPicking() { return GLinkPick.bActive; }

	void BeginLinkPick(AActor* Owner, const FString& PropName, bool bArray)
	{
		GLinkPick.Owner = Owner;
		GLinkPick.Prop = PropName;
		GLinkPick.bArray = bArray;
		GLinkPick.bActive = true;
		if (GEditor) GEditor->SelectNone(false, true, false);
		Notify(FString::Printf(TEXT("Assigning %s: select the target %s in the viewport, then press SPACE (ESC cancels)."),
			*PropName, bArray ? TEXT("objects") : TEXT("object")));
	}

	void ConfirmLinkPick()
	{
		if (!GLinkPick.bActive) return;
		GLinkPick.bActive = false;
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
		Notify(TEXT("Link assignment cancelled."));
	}

	AActor* PlaceType(const FString& Type, const FVector& WorldPos)
	{
		if (!g_ss.bEditing) { Notify(TEXT("Open a custom map (a save) to place objects.")); return nullptr; }
		const FString Mesh = BF6_ResolveMeshForType(Type);
		if (Mesh.IsEmpty()) { Notify(FString::Printf(TEXT("No SDK model for '%s'."), *Type)); return nullptr; }
		FScopedTransaction Tx(FText::FromString(FString::Printf(TEXT("Place %s"), *Type)));
		AActor* A = SpawnSdkModel(Mesh, Type, FTransform(WorldPos));
		if (A && GEditor) { GEditor->SelectNone(false, true, false); GEditor->SelectActor(A, true, true); }
		BF6_RecomputeBudget();
		return A;
	}

	bool WorldFromViewportCursor(FVector& OutWorld)
	{
		if (!GCurrentLevelEditingViewportClient) return false;
		const FViewportCursorLocation Cursor = GCurrentLevelEditingViewportClient->GetCursorWorldLocationFromMousePos();
		const FVector O = Cursor.GetOrigin(), D = Cursor.GetDirection();
		if (D.IsNearlyZero()) return false;
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
	// an undone add-point leaves a stale handle entry behind
	GVolEdit.Handles.RemoveAll([](const TWeakObjectPtr<AActor>& H){ return !H.IsValid(); });
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		AActor* A = *It;
		if (!A->Tags.Contains(kPlacedTag) && !A->Tags.Contains(kBaseTag) && !A->Tags.Contains(kHandleTag)) continue;
		UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
		if (!M || M->GetNumSections() > 0) continue;
		if (A->Tags.Contains(kHandleTag)) { BuildHandleCube(M); ApplyObjectWhite(M); continue; }
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
		BF6Api::InstallInputHandler();
		BF6Api::ShowStartupUI();
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
	BF6Api::DetachUI();
	BF6Api::RemoveInputHandler();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(kTabName);
	if (g_ctx && g_close) { g_close(g_ctx); g_ctx = nullptr; }
	if (DllHandle) { FPlatformProcess::FreeDllHandle(DllHandle); DllHandle = nullptr; }
}

IMPLEMENT_MODULE(FBF6UnrealSDKModule, BF6UnrealSDK)
