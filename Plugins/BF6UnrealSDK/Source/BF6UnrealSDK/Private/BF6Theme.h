#pragma once

#include "CoreMinimal.h"

// The Portal-site palette, rebased on the design tokens extracted from
// portal.battlefield.com's own stylesheet (build 15249075, index-CKMQIwzL.css).
// Each value below names the CSS custom property it came from; the full table,
// with the selector every value was read from, is in
// Resources/portalstyle/tokens.json.
//
// FColor values are sRGB hex exactly as the site declares them; the
// FLinearColor constructor converts them correctly for Slate.
//
// Signature look: near-black grounds, BF blue-grey text (#AEC0CC family),
// SQUARE corners (the site declares no radius on any control; hover and focus
// are a 2px chamfered ring drawn outside the control, not a rounded outline).
namespace BF6Theme
{
	// grounds
	static const FLinearColor Ink        = FLinearColor(FColor(0x09, 0x0A, 0x0A));         // --tmln-colors-gray900, the app chrome's deepest ground
	static const FLinearColor Panel      = FLinearColor(FColor(0x11, 0x13, 0x14));         // --tmln-colors-gray800, .well-module_well.secondary background
	static const FLinearColor PanelLight = FLinearColor(FColor(0x27, 0x2B, 0x2E));         // --tmln-colors-gray700, .well primary and .dialog surface
	static const FLinearColor Line       = FLinearColor(FColor(0xAE, 0xC0, 0xCC)) * FLinearColor(1.f, 1.f, 1.f, 0.25f);  // --tmln-colors-neutral-100-t-25, the .well and .divider hairline

	// accent (the orange badge / highlight color)
	static const FLinearColor Accent     = FLinearColor(FColor(0xFF, 0x3C, 0x00));         // --tmln-colors-brand100, the Battlefield brand orange
	static const FLinearColor AccentDim  = FLinearColor(FColor(0xA7, 0x67, 0x00));         // --tmln-colors-orange400; nearest dimmed-warm token (checkbox unavailable fill), not a tint of brand100

	// text - BF blue-grey, NOT white
	static const FLinearColor Text       = FLinearColor(FColor(0xBF, 0xCA, 0xD1));         // --tmln-colors-neutral-200, .table and .well body text
	static const FLinearColor TextBlue   = FLinearColor(FColor(0xAE, 0xC0, 0xCC));         // --tmln-colors-neutral-100 (= --tmln-colors-text), body and headings
	static const FLinearColor TextDim    = FLinearColor(FColor(0x75, 0x82, 0x8A));         // --tmln-colors-gray300, .caption and disabled text

	// budget bar: blue that shifts to red as it fills
	static const FLinearColor BudgetLow  = FLinearColor(FColor(0x59, 0xBF, 0xF8));         // --tmln-colors-tertiary (blue100)
	static const FLinearColor BudgetMid  = FLinearColor(FColor(0xF2, 0xC5, 0x73));         // --tmln-colors-warning-vibrant (yellow100)
	static const FLinearColor BudgetHigh = FLinearColor(FColor(0xFB, 0x69, 0x4D));         // --tmln-colors-danger-vibrant (red100); the site's own over-budget colour

	// semantic accents, straight off the site's label and chip variants
	static const FLinearColor Positive   = FLinearColor(FColor(0x8E, 0xED, 0x6C));         // --tmln-colors-positive-vibrant (green100)
	static const FLinearColor Warning    = FLinearColor(FColor(0xF2, 0xC5, 0x73));         // --tmln-colors-warning-vibrant (yellow100)
	static const FLinearColor Negative   = FLinearColor(FColor(0xFB, 0x69, 0x4D));         // --tmln-colors-danger-vibrant (red100)
	static const FLinearColor Info       = FLinearColor(FColor(0x59, 0xBF, 0xF8));         // --tmln-colors-tertiary (blue100)

	// interaction
	static const FLinearColor Hover      = FLinearColor(FColor(0x46, 0x4D, 0x52));         // --tmln-colors-gray500, the site's hover fill on inputs, tabs and dropdown rows
	static const FLinearColor Pressed    = FLinearColor(FColor(0x11, 0x13, 0x14));         // --tmln-colors-gray800, .button:active and .dropdown row :active
	static const FLinearColor Selected   = FLinearColor(FColor(0xAE, 0xC0, 0xCC));         // --tmln-colors-neutral-100, .dropdown .selected and .radio .selected fill
	static const FLinearColor OnSelected = FLinearColor(FColor(0x00, 0x00, 0x00));         // --tmln-colors-text-invert, the text on a selected row
	static const FLinearColor Disabled   = FLinearColor(FColor(0x60, 0x6A, 0x70));         // --tmln-colors-gray400, every disabled control's fill
	static const FLinearColor Focus      = FLinearColor(FColor(0xAE, 0xC0, 0xCC));         // --tmln-colors-neutral-100, the focus and hover ring colour

	// metrics the site is strict about
	static constexpr float CornerRadius  = 0.0f;                                            // the site declares no radius on any control
	static constexpr float HairlineWidth = 1.0f;                                            // default control border
	static constexpr float RingWidth     = 2.0f;                                            // hover / focus ring
	static constexpr float RingInset     = 5.0f;                                            // ring is drawn 5px outside a button, 4px outside an input

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
