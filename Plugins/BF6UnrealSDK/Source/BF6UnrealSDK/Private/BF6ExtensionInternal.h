#pragma once

#include "CoreMinimal.h"
#include "BF6SDKExtension.h"

// The tool's side of the add-on seam. Private on purpose: add-ons see
// BF6SDKExtension.h and nothing here.
namespace BF6ExtInternal
{
	// Registered pills, already in Order. The ring appends them after its own.
	const TArray<BF6Ext::FPieEntry>& PieEntries();

	// Offered a label the tool did not recognise. True when an add-on took it.
	bool DispatchPie(const FString& Label, const FVector2D& Center);

	// The sub-ring an add-on most recently opened; the wheel reads these while
	// in its add-on mode. Owned by BF6Extension.cpp.
	const TArray<BF6Ext::FPieSubEntry>& AddonSubEntries();

	void BroadcastMapOpened(const FString& Level, const FString& Save);
	void BroadcastMapClosing(const FString& Level);
}
