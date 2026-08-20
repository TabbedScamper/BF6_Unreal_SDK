#include "SBF6PreviewViewport.h"
#include "BF6Bridge.h"

#include "AdvancedPreviewScene.h"
#include "EditorViewportClient.h"
#include "ProceduralMeshComponent.h"
#include "UObject/Package.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"

// --- viewport client: just renders; the widget's input layer moves the camera ---
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
		// repaint every frame - camera moves come from outside the viewport's
		// own input path, so we can't rely on its invalidate-on-input
		SetRealtime(true);
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
};

// --- invisible layer above the viewport: LMB/RMB drag spins, wheel zooms.
// Sits on top of the SViewport so it gets the events FIRST, which makes the
// orbit work identically whether the preview is docked or inside a popup. ---
class SBF6OrbitLayer : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SBF6OrbitLayer) : _Owner(nullptr) {}
		SLATE_ARGUMENT(SBF6PreviewViewport*, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Owner = InArgs._Owner;
		SetCursor(EMouseCursor::GrabHand);
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1.f, 1.f); }
	virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&,
		FSlateWindowElementList&, int32 LayerId, const FWidgetStyle&, bool) const override
	{ return LayerId; }   // draws nothing

	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& E) override
	{
		if (E.GetEffectingButton() == EKeys::LeftMouseButton || E.GetEffectingButton() == EKeys::RightMouseButton)
			return FReply::Handled().CaptureMouse(SharedThis(this));
		return FReply::Unhandled();
	}

	virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& E) override
	{
		if (!HasMouseCapture() || !Owner) return FReply::Unhandled();
		Owner->OrbitBy(E.GetCursorDelta());
		return FReply::Handled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& E) override
	{
		if (HasMouseCapture()) return FReply::Handled().ReleaseMouseCapture();
		return FReply::Unhandled();
	}

	virtual FReply OnMouseWheel(const FGeometry&, const FPointerEvent& E) override
	{
		if (Owner) { Owner->ZoomBy(E.GetWheelDelta()); return FReply::Handled(); }
		return FReply::Unhandled();
	}

private:
	SBF6PreviewViewport* Owner = nullptr;   // parent widget; lifetimes are tied
};

void SBF6PreviewViewport::Construct(const FArguments& InArgs)
{
	PreviewScene = MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	SEditorViewport::Construct(SEditorViewport::FArguments());

	// wrap whatever SEditorViewport built with our orbit input layer on top
	TSharedRef<SWidget> Inner = ChildSlot.GetWidget();
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()[ Inner ]
		+ SOverlay::Slot()[ SNew(SBF6OrbitLayer).Owner(this) ]
	];
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

void SBF6PreviewViewport::OrbitBy(const FVector2D& CursorDelta)
{
	Yaw  += CursorDelta.X * 0.5f;
	Pitch = FMath::Clamp(Pitch - CursorDelta.Y * 0.5f, -85.f, 85.f);
	ApplyCamera();
}

void SBF6PreviewViewport::ZoomBy(float WheelDelta)
{
	Dist = FMath::Clamp(Dist * (1.f - WheelDelta * 0.12f), MinDist, MaxDist);
	ApplyCamera();
}

void SBF6PreviewViewport::ApplyCamera()
{
	if (!Client.IsValid()) return;
	const FRotator Rot((float)Pitch, (float)Yaw, 0.f);
	Client->SetViewLocation(Center - Rot.Vector() * Dist);
	Client->SetViewRotation(Rot);
	Client->Invalidate();
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
	// at the world origin cut tall models off) from the corner iso, then let
	// the orbit layer take it from there
	const FBoxSphereBounds B = Mesh->CalcBounds(FTransform::Identity);
	Center  = B.Origin;
	Dist    = FMath::Max(60.f, Radius * 2.4f);
	MinDist = FMath::Max(20.f, Radius * 0.7f);
	MaxDist = FMath::Max(300.f, Radius * 10.f);
	Yaw     = -135.f;
	Pitch   = -35.264f;
	ApplyCamera();
}
