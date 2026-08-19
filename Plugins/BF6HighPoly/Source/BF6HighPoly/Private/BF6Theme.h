#pragma once

#include "CoreMinimal.h"

// Seasoned Battlefield-Portal palette, shared by every Build Mode widget so the
// full-screen tool reads as one program (mirrors our Godot high-poly theme).
// Cleanup pass will pull the exact hexes from the Godot style; these are close.
namespace BF6Theme
{
	// grounds
	static const FLinearColor Ink        = FLinearColor(0.055f, 0.062f, 0.070f, 1.0f);   // deepest bg
	static const FLinearColor Panel      = FLinearColor(0.098f, 0.113f, 0.130f, 0.96f);  // card / bar bg
	static const FLinearColor PanelLight = FLinearColor(0.150f, 0.170f, 0.190f, 1.0f);   // hovered
	static const FLinearColor Line       = FLinearColor(0.220f, 0.250f, 0.285f, 1.0f);   // hairlines

	// accents (BF Portal orange)
	static const FLinearColor Accent     = FLinearColor(1.000f, 0.520f, 0.100f, 1.0f);
	static const FLinearColor AccentDim  = FLinearColor(0.720f, 0.360f, 0.070f, 1.0f);

	// text
	static const FLinearColor Text       = FLinearColor(0.905f, 0.930f, 0.960f, 1.0f);
	static const FLinearColor TextDim    = FLinearColor(0.560f, 0.610f, 0.665f, 1.0f);

	// budget bar: blue that shifts to red as it fills
	static const FLinearColor BudgetLow  = FLinearColor(0.150f, 0.560f, 0.960f, 1.0f);   // blue
	static const FLinearColor BudgetMid  = FLinearColor(0.980f, 0.620f, 0.130f, 1.0f);   // amber
	static const FLinearColor BudgetHigh = FLinearColor(0.960f, 0.235f, 0.190f, 1.0f);   // red

	// Fill colour for a budget fraction: mostly blue, ramping to red near the end.
	static inline FLinearColor BudgetFill(float Frac)
	{
		Frac = FMath::Clamp(Frac, 0.0f, 1.0f);
		if (Frac < 0.65f)
		{
			// blue -> amber across the first stretch, but stay blue-dominant
			const float t = FMath::Clamp(Frac / 0.65f, 0.0f, 1.0f) * 0.5f;
			return FMath::Lerp(BudgetLow, BudgetMid, t);
		}
		const float t = FMath::Clamp((Frac - 0.65f) / 0.35f, 0.0f, 1.0f);
		return FMath::Lerp(BudgetMid, BudgetHigh, t);
	}
}
