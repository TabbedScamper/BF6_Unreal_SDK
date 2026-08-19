#include "SBF6PreviewViewport.h"
#include "BF6Bridge.h"

#include "AdvancedPreviewScene.h"
#include "EditorViewportClient.h"
#include "ProceduralMeshComponent.h"
#include "UObject/Package.h"

// --- viewport client: turntable orbit around the model ---
class FBF6PreviewClient : public FEditorViewportClient
{
public:
	FBF6PreviewClient(FPreviewScene* InScene, const TSharedRef<SEditorViewport>& InWidget)
		: FEditorViewportClient(nullptr, InScene, InWidget)
	{
		SetViewMode(VMI_Lit);
		bSetListenerPosition = false;
		EngineShowFlags.SetGrid(false);
		EngineShowFlags.SetSelectionOutline(false);

		// Orbit navigation, like an asset preview.
		bUsingOrbitCamera = true;
		SetViewLocationForOrbiting(FVector::ZeroVector, 300.0f);
	}

	// Keep the preview world ticking so lighting/rotation update.
	virtual void Tick(float DeltaSeconds) override
	{
		FEditorViewportClient::Tick(DeltaSeconds);
		if (PreviewScene && PreviewScene->GetWorld())
		{
			PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
		}
	}

	void FrameBounds(const FVector& Center, float Radius)
	{
		const float Dist = FMath::Max(60.0f, Radius * 2.4f);
		// Three-quarter top-down (near-isometric): look down at the model from a
		// corner so its shape reads instantly. Front-on made objects hard to read.
		const FRotator Rot(-35.264f, -135.f, 0.f);
		SetViewLocationForOrbiting(Center, Dist);
		SetViewLocation(Center - Rot.Vector() * Dist);
		SetViewRotation(Rot);
		Invalidate();
	}
};

void SBF6PreviewViewport::Construct(const FArguments& InArgs)
{
	PreviewScene = MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	SEditorViewport::Construct(SEditorViewport::FArguments());
}

SBF6PreviewViewport::~SBF6PreviewViewport()
{
	ClearModel();
}

TSharedRef<FEditorViewportClient> SBF6PreviewViewport::MakeEditorViewportClient()
{
	Client = MakeShareable(new FBF6PreviewClient(PreviewScene.Get(), SharedThis(this)));
	return Client.ToSharedRef();
}

void SBF6PreviewViewport::ClearModel()
{
	if (Mesh)
	{
		if (PreviewScene) PreviewScene->RemoveComponent(Mesh);
		Mesh = nullptr;
	}
}

void SBF6PreviewViewport::ShowModel(const FString& MeshName)
{
	ClearModel();
	if (MeshName.IsEmpty() || !PreviewScene) return;

	Mesh = NewObject<UProceduralMeshComponent>(GetTransientPackage());
	float Radius = 100.0f;
	if (!BF6_LoadSdkModelInto(Mesh, MeshName, Radius))
	{
		Mesh = nullptr;   // no bundled model
		return;
	}
	PreviewScene->AddComponent(Mesh, FTransform::Identity);
	// frame the model's real centre (object origins sit at the base, so aiming
	// at the world origin cut tall models off)
	const FBoxSphereBounds B = Mesh->CalcBounds(FTransform::Identity);
	if (Client.IsValid()) Client->FrameBounds(B.Origin, Radius);
}
