#pragma once

#include "CoreMinimal.h"

// The REAL Portal-site palette, measured off portal.battlefield.com's editor
// (computed styles, 2026-08-19). FColor values are sRGB hex from the page;
// the FLinearColor constructor converts them correctly for Slate.
// Signature look: near-black grounds, BF blue-grey text (#AEC0CC family),
// SHARP corners (the site uses border-radius 0 everywhere), orange badges.
namespace BF6Theme
{
	// grounds
	static const FLinearColor Ink        = FLinearColor(FColor(0x0D, 0x0F, 0x10));         // page ground
	static const FLinearColor Panel      = FLinearColor(FColor(0x11, 0x13, 0x14));         // card caption / bars
	static const FLinearColor PanelLight = FLinearColor(FColor(0x1A, 0x1D, 0x1F));         // ghost button fill
	static const FLinearColor Line       = FLinearColor(FColor(0xAE, 0xC0, 0xCC)) * FLinearColor(1.f, 1.f, 1.f, 0.5f);   // hairline borders (blue-grey @ 50%)

	// accent (the orange badge / highlight color)
	static const FLinearColor Accent     = FLinearColor(FColor(0xFF, 0x58, 0x1A));
	static const FLinearColor AccentDim  = FLinearColor(FColor(0xB8, 0x3E, 0x10));

	// text - BF blue-grey, NOT white
	static const FLinearColor Text       = FLinearColor(FColor(0xBF, 0xCA, 0xD1));         // captions / labels
	static const FLinearColor TextBlue   = FLinearColor(FColor(0xAE, 0xC0, 0xCC));         // headings / body
	static const FLinearColor TextDim    = FLinearColor(FColor(0x75, 0x82, 0x8A));         // secondary / icons

	// budget bar: blue that shifts to red as it fills
	static const FLinearColor BudgetLow  = FLinearColor(0.150f, 0.560f, 0.960f, 1.0f);
	static const FLinearColor BudgetMid  = FLinearColor(0.980f, 0.620f, 0.130f, 1.0f);
	static const FLinearColor BudgetHigh = FLinearColor(0.960f, 0.235f, 0.190f, 1.0f);

	// Fill colour for a budget fraction: mostly blue, ramping to red near the end.
	static inline FLinearColor BudgetFill(float Frac)
	{
		Frac = FMath::Clamp(Frac, 0.0f, 1.0f);
		if (Frac < 0.65f)
		{
			const float t = FMath::Clamp(Frac / 0.65f, 0.0f, 1.0f) * 0.5f;
			return FMath::Lerp(BudgetLow, BudgetMid, t);
		}
		const float t = FMath::Clamp((Frac - 0.65f) / 0.35f, 0.0f, 1.0f);
		return FMath::Lerp(BudgetMid, BudgetHigh, t);
	}
}
