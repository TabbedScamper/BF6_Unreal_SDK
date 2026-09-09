/* rime_ext.inc - read BF6's authored UI directly from the mounted install.
 *
 * Rime is the exceptional EBX family: the partitions ship an older schema
 * than bf6.exe.  Reading these records through executable reflection does not
 * fail; it returns convincing values from the wrong fields.  Every raw-offset
 * read below is therefore gated by the concrete type GUID AND the layout
 * signature stored in that partition's EFIX table.
 *
 * The offsets and accepted signatures are the verified read path documented
 * by BF6_Frostbite_Research/findings/rime-paint-read-path.md and
 * rime-box-solving-law.md.  The research TSVs are controls, never inputs.
 */

static const uint32_t kRimeNameOff      = 176;
static const uint32_t kRimeVLayoutOff   = 184;
static const uint32_t kRimeHLayoutOff   = 208;
static const uint32_t kRimePaddingOff   = 248;
static const uint32_t kRimeNameHashOff  = 296;
static const uint32_t kRimeHeightOff    = 300;
static const uint32_t kRimeAlphaOff     = 308;
static const uint32_t kRimeWidthOff     = 312;
static const uint32_t kRimeVisibleOff   = 316;
static const uint32_t kRimeFitHOff      = 317;
static const uint32_t kRimeFitWOff      = 318;
static const uint32_t kRimeElementsOff  = 320;
static const uint32_t kRimeMasksOff     = 336;
static const uint32_t kRimeMaskGradEndOff   = 344;
static const uint32_t kRimeMaskGradStartOff = 348;
static const uint32_t kRimeMaskThresholdOff = 352;
static const uint32_t kRimeMaskModeOff      = 356;
static const uint32_t kRimeMaskInvertOff    = 360;
/* RimeViewportStretchContainerElementData in the SHIPPED old Rime schema.
 * The current executable's reflection says +0x138/+0x140..143, but using
 * those offsets against retail partitions reads the old Elements rel-array
 * and produces plausible, wrong booleans. The old derived tail is shifted by
 * 16 bytes to +0x148/+0x150..153. */
static const uint32_t kRimeViewportFlowOff   = 328;
static const uint32_t kRimeViewportTopOff    = 336;
static const uint32_t kRimeViewportBottomOff = 337;
static const uint32_t kRimeViewportLeftOff   = 338;
static const uint32_t kRimeViewportRightOff  = 339;

/* DiceUIDataListElementData, shipped type
 * 2420ee41-1c4d-38bd-4203-777f31cf9c9a.  This is not the ordinary Rime stack
 * layout: reading +336/+344 as orientation/spacing yields plausible garbage.
 * Its repetition law lives after RimeDataListElementBaseData instead. */
static const char* kRimeDataListTypeGuid =
    "2420ee41-1c4d-38bd-4203-777f31cf9c9a";
static const uint32_t kRimeDataListOldSignature = 0x2B1AFF01u;
static const uint32_t kRimeDataListNewSignature = 0x3450F88Au;

static const uint32_t kRimeRefBlueprintOff = 112;
static const uint32_t kRimeRefNameOff      = 376;
static const uint32_t kRimeRefHOff         = 384;
static const uint32_t kRimeRefVOff         = 408;
static const uint32_t kRimeRefPaddingOff   = 448;
static const uint32_t kRimeRefAlphaOff     = 496;
static const uint32_t kRimeRefHeightOff    = 508;
static const uint32_t kRimeRefWidthOff     = 512;
static const uint32_t kRimeRefFitWOff      = 516;
static const uint32_t kRimeRefFitHOff      = 517;
static const uint32_t kRimeRefVisibleOff   = 519;
static const uint32_t kRimeRefUseHeightOff = 521;
static const uint32_t kRimeRefUseWidthOff  = 522;
static const uint32_t kRimeRefNameHashOff  = 528;

static const uint32_t kRimeLayerNameOff     = 56;
static const uint32_t kRimeLayerElementsOff = 64;
static const uint32_t kRimeLayerVisibleOff  = 88;

/* PAINT.  ColorValue is a ColorListValue, whose own layout is RawColor Vec3
 * at +0 and SelectedColorId (a CString) at +16.  RawColor is vestigial in
 * practice - no shipped front-end element carries a non-white RawColor with no
 * id - but it is read anyway so that "white" and "unread" stay distinguishable. */
static const uint32_t kRimeColorOff      = 96;    /* ordinary element base   */
static const uint32_t kRimeRefColorOff   = 272;   /* widget reference        */
static const uint32_t kRimeColorRawOff   = 0;     /* within ColorListValue   */
static const uint32_t kRimeColorIdOff    = 16;    /* within ColorListValue   */

/* RimeRepeatShapeElementData.  Verified against all 175 shipped records. */
static const uint32_t kRimeRepeatDistOff  = 388;
static const uint32_t kRimeRepeatCountOff = 392;

/* RimeLineElementData, shipped signature 0xC5E7F3DD.  Points is an inline
 * array of 80-byte PointData values; PointData.Point is the Vec2 at +48.
 * These are the shipped offsets used by the evidence pipeline, not the newer
 * executable reflection layout. */
static const uint32_t kRimeLinePointsOff   = 320;
static const uint32_t kRimeLineWidthOff    = 328;
static const uint32_t kRimeLineCapOff      = 332;
static const uint32_t kRimeLineEndOff      = 336;
static const uint32_t kRimeLineGlowOff     = 340;
static const uint32_t kRimeLineStartOff    = 344;
static const uint32_t kRimeLineSingleOff   = 348;
static const uint32_t kRimeLineCloseOff    = 350;
static const uint32_t kRimeLineRelativeOff = 352;
static const uint32_t kRimePointStride     = 80;
static const uint32_t kRimePointVecOff     = 48;

/* BattlefieldUIShared's attachment leader.  This is not a
 * RimeLineElementData polyline: its endpoints are anchor references populated
 * by PropertyConnections at runtime and the file only authors offsets/style.
 * Exposing those two offsets as the two points returned by bf6_rime_line keeps
 * the read path live while allowing the armory host to join them to its
 * projected StartAnchor and solved EndAnchor. */
static const char* kRimeAttachmentLeaderGuid =
    "c422c2f5-43c4-04b5-369c-392d79ee7b6b";
static const uint32_t kRimeAttachmentLeaderSignature = 0x4F71E199u;
static const uint32_t kRimeAttachmentLeaderStartOffset = 336;
static const uint32_t kRimeAttachmentLeaderEndOffset   = 344;
static const uint32_t kRimeAttachmentLeaderWidthOffset = 356;

/* The fixed fields used by the shipped progress and border painters.  The
 * evidence pipeline validates progress/count/gap and border thickness/
 * alignment/gradient against every instance on menuweaponscreen. Orientation
 * is the enum immediately following the progress FillStyle pointer; the live
 * test independently checks Horizontal/Vertical named controls. */
static const uint32_t kRimeProgressOrientationOff = 332;
static const uint32_t kRimeProgressValueOff       = 336;
static const uint32_t kRimeProgressGapOff         = 344;
static const uint32_t kRimeProgressCountOff       = 348;
static const uint32_t kRimeBorderAlignmentOff     = 456;
static const uint32_t kRimeBorderStartAlphaOff    = 460;
static const uint32_t kRimeBorderGradientOff      = 464;
static const uint32_t kRimeBorderEndAlphaOff      = 472;
static const uint32_t kRimeBorderThicknessOff     = 476;

/* DiceUIAnchorContainerElementData: the attach point, a Vec2 fraction of the
 * container's own box.  Beside it at +380 sits an angle that is 90.0 on all 24
 * customization anchors and at +384 a 1.0; neither is read until something
 * needs them. */
static const uint32_t kRimeAnchorAttachOff = 368;

/* DiceUIDataListElementData and DiceUIUniformGridElementData store
 * ItemTemplates as an ordered Class array at +0x158 in the shipped schema.
 * This is deliberately not the executable's newer singular ItemTemplate
 * layout. The uniform-grid array is immediately followed by Orientation at
 * +0x168; treating +0x190 as one PointerRef selected an unrelated import and
 * made the Home page grid look convincingly one-card wide. */
static const uint32_t kRimeDataListItemTemplatesOff = 0x158;
static const uint32_t kRimeGridItemTemplatesOff = 0x158;
static const uint32_t kRimeGridOrientationOff = 0x168;

/* Shipped DiceUI grid own fields. These offsets are from the partition's old
 * schema, not the executable's newer RimeUniformGridListElementData layout. */
static const uint32_t kRimeGridStaticCountOff = 416;
static const uint32_t kRimeGridColumnSizeOff = 424;
static const uint32_t kRimeGridRowSpacingOff = 428;
static const uint32_t kRimeGridColumnSpacingOff = 432;
static const uint32_t kRimeGridSegmentDistributionOff = 436;
static const uint32_t kRimeGridRowSizeOff = 440;
static const uint32_t kRimeGridSegmentCountModeOff = 452;
static const uint32_t kRimeGridColumnFlowOff = 456;
static const uint32_t kRimeGridRowFlowOff = 464;
static const uint32_t kRimeGridItemFitContentOff = 470;

/* RimeLabelElementData.  The text source is StringId or nothing in the shipped
 * data - no label carries RawText and none carries a LocalizedText pointer -
 * so 1,209 of the 1,513 labels in common/ui are filled by a runtime binding
 * and have nothing to read. */
static const uint32_t kRimeLabelTextOff   = 320;  /* LocalizedText ptr        */
static const uint32_t kRimeLabelSidOff    = 336;  /* StringId CString         */
static const uint32_t kRimeLabelRawOff    = 352;  /* RawText CString          */
static const uint32_t kRimeLabelStyleOff  = 360;  /* FontStyle ptr            */
/* +392 is PointSize (0xD890D585) in the shipped label schema.  +384 is
 * 0xF66DF006, the signed spacing-shaped float that also sits beside PointSize
 * on RimeFontStyle; reading it as a size override was a latent transposition
 * (both are 0 on every sampled label, so nothing drew wrong yet). */
static const uint32_t kRimeLabelPtSizeOff = 392;  /* PointSize override       */
static const uint32_t kRimeLabelSpacingOff = 384; /* 0xF66DF006, spacing-like */
static const uint32_t kRimeLabelVAlignOff  = 368; /* VerticalAlignment enum   */
static const uint32_t kRimeLabelHAlignOff  = 396; /* HorizontalAlignment enum */
static const uint32_t kRimeLabelOverflowOff = 400; /* TextOverflowMode enum   */
static const uint32_t kRimeLabelParseMarkupOff = 404; /* Boolean             */
static const uint32_t kRimeLabelWordWrapOff = 408; /* Boolean                 */

/* Image elements. The SVG element points at a vector asset, the texture
 * element at a TextureAsset; both are ordinary cross-partition imports and the
 * partition path is what a consumer needs to open. */
static const uint32_t kRimeSvgImageOff  = 320;  /* RimeSvgElementData      */
static const uint32_t kRimeTexImageOff  = 368;  /* RimeTextureElementData  */
static const uint32_t kRimeTexUvOff     = 352;  /* Vec4 uv window          */
static const uint32_t kRimeTexResizeModeOff = 376;
static const uint32_t kRimeTexAddressVOff = 380;
static const uint32_t kRimeTexAddressUOff = 392;
/* +400 is field hash 0x6874B59F = ClipToBounds.  NAME CONFIRMED 2026-09-07.
 *
 * History, because this flip-flopped twice: I first flagged the name as
 * unverified because 0x6874B59F is absent from bfreader's
 * `docs/sdk_field_names.tsv`, then retracted a claim that 0xE0BFF9C6 was the
 * "real" ClipToBounds.  BOTH of those were wrong.  The executable's own
 * type-field layout table (research repo `data/exe_type_field_layouts.tsv.gz`)
 * names 0x6874B59F `ClipToBounds` on THIRTEEN types, including
 * RimeFlipbookTextureElementData, RimeMovieElementData,
 * RimeRepeatShapeElementData and RimeListElementData.  It also names
 * 0xE0BFF9C6 `ClipToBounds` - on RimeListElementData only, at a different
 * offset - so the two are distinct same-named fields and RimeListElementData
 * carries both.
 *
 * The exe offsets differ from the shipped ones (Flipbook is 0x152 there,
 * +354 here); that is the documented older-shipped-schema split, not a
 * contradiction.  Lesson: check the EXE table, not only the SDK name dump,
 * before calling a hash unnamed. */
static const uint32_t kRimeTexClipToBoundsOff = 400;
static const uint32_t kRimeMovieImageOff = 336; /* RimeMovieElementData    */
static const uint32_t kRimeFlipbookImageOff = 320; /* FlipbookTexture        */

/* THE IMAGE FAMILY'S FIT AND ALIGNMENT.
 *
 * Every image-ish element authors how its content sits in its solved box -
 * a ResizeMode plus a horizontal and a vertical alignment - and each type
 * puts those three at DIFFERENT offsets.  Reading one type's offsets on
 * another is the transposition this file warns about elsewhere, so the
 * table below is per type and every row was read from the shipped partition
 * dump, not assumed from a sibling.
 *
 * CAUTION on the two alignment enums: h and v read 1 on 100% of sampled
 * elements across all five kinds.  A value that never varies is more
 * consistent with being the DEFAULT than with being deliberately authored,
 * and the enum's member names are not recovered, so do NOT build behaviour on
 * "1 means centre" or cite these as evidence that art is aligned on purpose.
 * ResizeMode is the useful one of the three: it varies (2, 3 and 4 observed).
 * An earlier revision of this comment called all three non-default; that was
 * wrong and is retracted. */
struct bf6__rime_image_layout {
    uint32_t resize_mode, h_align, v_align;
    /* ClipToBounds (0x6874B59F) per kind.  0 = this kind does not carry it.
     * It is NOT texture-only: the shipped Svg (+352) and Flipbook (+354)
     * records carry the same hash, and the executable's type table names it
     * ClipToBounds on thirteen types including both of those.  Reading it
     * only for Texture left every Svg and Flipbook at the "unread" -1, and
     * the layout helper clips only when the flag is exactly 1 - so those
     * kinds could never clip and overflowed their solved box.  That is what
     * put stray artwork outside the header the moment flipbooks began to
     * paint. */
    uint32_t clip_to_bounds;
};
static const bf6__rime_image_layout kRimeTextureLayout   = { 376, 384, 396, 400 };
static const bf6__rime_image_layout kRimeSvgLayout       = { 344, 340, 336, 352 };
static const bf6__rime_image_layout kRimeFlipbookLayout  = { 340, 344, 336, 354 };
static const bf6__rime_image_layout kRimeVecLayout       = { 328, 344, 340, 0 };
static const bf6__rime_image_layout kRimeTexBlendLayout  = { 472, 504, 476, 0 };

/* Svg: CustomRasterSize is a Vec2, and RasterSizeMode selects whether it is
 * used.  The shipped values are 256x256 and 48x48 - an SVG rasterised at the
 * wrong one is either blurry or wasteful. */
static const uint32_t kRimeSvgRasterSizeOff     = 328;
static const uint32_t kRimeSvgRasterSizeModeOff = 348;
/* Flipbook: Progress +328, RandomFrames +348, AutoPlay +350, Loop +352.
 * Both sampled flipbooks author AutoPlay and Loop true. */
static const uint32_t kRimeFlipProgressOff = 328;
static const uint32_t kRimeFlipRandomOff   = 348;
static const uint32_t kRimeFlipAutoPlayOff = 350;
static const uint32_t kRimeFlipLoopOff     = 352;
/* VectorShape: GlowSize +336 (2 or 5 on the sampled shapes),
 * ScaleLineWidths +349. */
static const uint32_t kRimeVecGlowSizeOff       = 336;
static const uint32_t kRimeVecScaleLineWidthOff = 349;
/* TextureBlendContainer: its own Texture is at +464, PAST its Elements
 * array, so the ordinary texture cell finds nothing on this type - which is
 * one reason it currently draws as an empty container.  GradientEnd +484,
 * GradientStart +492, MaskThreshold +496 (0.5 authored), InvertMask +508. */
static const uint32_t kRimeBlendTextureOff   = 464;
static const uint32_t kRimeBlendGradEndOff   = 484;
static const uint32_t kRimeBlendGradStartOff = 492;
static const uint32_t kRimeBlendThresholdOff = 496;
static const uint32_t kRimeBlendInvertOff    = 508;

/* Fill styles. RimeFillElementData.FillStyle is a PointerRef to a
 * RimeFillStyle whose Layers array holds polymorphic layer records. The
 * gradient layer's start and end COLOURS are white throughout the shipped
 * front end; only its alphas vary, so it modulates rather than replaces. */
static const uint32_t kRimeFillStyleOff   = 320;  /* on the fill element   */
static const uint32_t kRimeFillLayersOff  = 32;   /* on RimeFillStyle      */
static const uint32_t kRimeLayerBlendOff  = 36;   /* on the layer base     */
static const uint32_t kRimeGradEndAlpha   = 144;
static const uint32_t kRimeGradDirection  = 148;
static const uint32_t kRimeGradStartAlpha = 152;
static const uint32_t kRimeBlendModeOff   = 304;  /* on the element base   */

/* Vector shapes.  The Shape pointer sits at a different offset on the two
 * element types that carry one, which is the sort of thing a single constant
 * would quietly get wrong on one of them. */
static const uint32_t kRimeVecShapeOff    = 320;  /* RimeVectorShapeElementData */
static const uint32_t kRimeRepeatShapeOff = 336;  /* RimeRepeatShapeElementData */

/* RimeVectorShapeAsset */
static const uint32_t kRimeAssetShapesOff = 32;   /* ptr array */
static const uint32_t kRimeAssetSizeOff   = 40;   /* Vec2      */

/* RimeVectorShape */
static const uint32_t kRimeShapeOffsetsOff = 24;   /* Vec2 array  */
static const uint32_t kRimeShapeIndicesOff = 80;   /* u16 array   */
static const uint32_t kRimeShapeColorsOff  = 88;   /* Vec4 array  */
static const uint32_t kRimeShapeCornersOff = 104;  /* Path, Corners inline */
static const uint32_t kRimeShapeAnchorsOff = 112;  /* Vec2 array  */
static const uint32_t kRimeShapeThickOff   = 120;
static const uint32_t kRimeShapeStyleOff   = 128;
static const uint32_t kRimeShapeAlphaOff   = 132;

/* RimeVectorPathCorner, stride 104 */
static const uint32_t kRimeCornerStride    = 104;
static const uint32_t kRimeCornerOffsetOff = 24;
static const uint32_t kRimeCornerAnchorOff = 80;
static const uint32_t kRimeCornerTypeOff   = 88;
static const uint32_t kRimeCornerCurveOff  = 96;

/* Field name hashes on the font assets. */
static const uint32_t kRimeFieldFontList   = 0xda71fcbau; /* per-locale assets */
static const uint32_t kRimeFieldLanguage   = 0xdc4590b1u;
static const uint32_t kRimeFieldFontPath   = 0xe49179d7u;
static const uint32_t kRimeFieldPointSize  = 0xd890d585u;
static const uint32_t kRimeFieldCapitalization = 0xf6a8334bu; /* RimeTextCapitalization */
static const uint32_t kRimeFieldLineHeight = 0xb60db0fbu;
static const uint32_t kRimeFieldAssetName  = 0x0c59fa06u;
static const uint32_t kRimeFieldFamily     = 0x6fc43179u;
static const uint32_t kRimeFieldWeight     = 0xab033644u;

/* Field name hashes on the palette assets.  Baked in rather than looked up,
 * exactly as every other schema'd read in this library does: the executable
 * type database stores hashes, not names. */
static const uint32_t kRimeFieldName            = 207223302u;
static const uint32_t kRimeFieldColorId         = 4149168364u;
static const uint32_t kRimeFieldColorValue      = 1273682853u;
static const uint32_t kRimeFieldDefaultColorVal = 4169441916u;

/* The seven palette assets.  A missing one is not fatal - it reduces coverage,
 * and the count the caller gets says so.  The default mount carries five of
 * the seven; gla_spectator and colorlist need bf6_mount_all.
 *
 * THE ORDER IS A PRECEDENCE RULE, NOT A LISTING.  A ColorId is unique within
 * the list an element selects from, not across the palette: five ids collide,
 * and gla_spectator's 26 team colours cycle through only FOUR of them - the
 * same four gla_granite uses for rarity tiers 01-04.  FE-Brand-Season and
 * HUD-BleedOut share one too.  So a global id->colour map is ambiguous by
 * construction and first-wins decides those five.
 *
 * Front-end lists therefore come first, and gla_spectator comes after
 * gla_granite deliberately: a front-end renderer asking for that id wants the
 * rarity tier, not spectator team 14.  The strictly correct fix is to scope
 * the lookup by the element's own ColorValue.Asset pointer at +24, which names
 * the list it is selecting from; that is not read yet.  It costs nothing today
 * because no element on the weapon screen reaches an ambiguous id - a fact the
 * test measures on every run rather than assuming. */
static const char* const kRimePalettes[] = {
    "common/ui/assets/styles/colorpalette/gla_frontend",
    "common/ui/assets/styles/colorpalette/gla_hud",
    "common/ui/assets/styles/colorpalette/gla_granite",
    "common/ui/assets/styles/colorpalette/gla_spectator",
    "common/ui/assets/styles/colorpalette/gla_blend",
    "common/ui/assets/styles/colorpalette/gla_debug",
    "common/ui/assets/styles/colorpalette/colorlist"
};

namespace {

struct bf6__rime_type {
    const char* guid;
    uint32_t signature;
    int kind;
    const char* name;
    bool children;
    bool stack;
};

/* Concrete shipped signatures.  Adding a GUID without its measured
 * signature would defeat the guard and is intentionally impossible here. */
static const bf6__rime_type kRimeTypes[] = {
    { "ca959de9-1107-7900-4a87-49f1b9a90f90", 0x4FECC2B2u, BF6_RIME_WIDGET_REFERENCE, "RimeWidgetReference", false, false },
    { "ca959de9-1107-7900-4a87-49f1b9a90f90", 0xB2D8C868u, BF6_RIME_WIDGET_REFERENCE, "RimeWidgetReference", false, false },
    { "9e8e3236-d0b2-2a38-064f-a926c08678cc", 0x37395774u, BF6_RIME_LAYER, "RimeLayerEntityData", true, false },
    { "42c9b97a-d21c-25d2-db47-8ed292bfc0d6", 0x55139317u, BF6_RIME_CONTAINER, "RimeContainerElementData", true, false },
    { "1aea2f67-ab05-f5ea-149d-e7dc345acc02", 0xB9442329u, BF6_RIME_STACK_CONTAINER, "RimeStackContainerElementData", true, true },
    { "562c3c80-3e40-00dd-2fa0-1028b84db932", 0x2B373A07u, BF6_RIME_CONTAINER, "RimeMaskingContainerElementData", true, false },
    { "ff4108ec-cdc5-fd08-2c68-cbb797b7d0c2", 0x24AD20DCu, BF6_RIME_CONTAINER, "RimeViewportStretchContainerElementData", true, false },
    { "d8bde2f6-8244-9e3a-c6db-33f43d22ab75", 0x2CF3CEEFu, BF6_RIME_LABEL, "RimeLabelElementData", false, false },
    { "13cc8964-abd5-122b-5574-852ff36ab2df", 0x5E25FA53u, BF6_RIME_REPEAT_SHAPE, "RimeRepeatShapeElementData", false, false },
    { "67aa214f-2c6e-c00e-a461-d4f9b0e7d0bf", 0xA5145DBAu, BF6_RIME_VECTOR_SHAPE, "RimeVectorShapeElementData", false, false },
    { "c078e35c-aaaf-ea52-2078-feba93f5e9df", 0x2BCCA5B9u, BF6_RIME_FILL, "RimeFillElementData", false, false },
    { "7b8c2acc-7873-2ea0-6619-c0098c19a37a", 0x477FF815u, BF6_RIME_SVG, "RimeSvgElementData", false, false },
    { "69095c7f-4530-2a2a-9d27-6bb0c7afebf0", 0x78E6ECF6u, BF6_RIME_TEXTURE, "RimeTextureElementData", false, false },
    { "e5ecf797-39f5-0ffe-a199-d444a7a7eaa8", 0xC5E7F3DDu, BF6_RIME_LINE, "RimeLineElementData", false, false },
    { "c422c2f5-43c4-04b5-369c-392d79ee7b6b", 0x4F71E199u, BF6_RIME_LINE, "BattlefieldUIAttachmentLeaderLine", false, false },
    { "2ccc4c67-a4e0-0c28-297a-476058737464", 0x825B3268u, BF6_RIME_PROGRESS, "RimeProgressBarElementData", false, false },
    { "83a055ad-dae1-bd63-e915-9fd990e056af", 0x9F9904A0u, BF6_RIME_BORDER, "RimeBorderElementData", false, false },
    { "b3c5f41b-79d3-6d9f-f29c-e68063ea3364", 0xFB1EEC6Fu, BF6_RIME_ARC_PROGRESS, "RimeArcProgressBarElementData", false, false },
    { "b2d8708a-ed17-4b72-1a78-f0707ba9ec41", 0x986517FFu, BF6_RIME_BLUR, "RimeBlurElementData", false, false },
    { "73b1bf98-032b-4ba3-117b-07486dcfd9ac", 0x2B6B0D83u, BF6_RIME_MOVIE, "RimeMovieElementData", false, false },
    { "ef91e7d0-29ce-b702-387f-e44dc49bcfa0", 0xF5E7D854u, BF6_RIME_FLIPBOOK, "RimeFlipbookTextureElementData", false, false },
    { "c8693168-e1ff-5aaf-9b94-e4b22a971686", 0x4F337E5Fu, BF6_RIME_TEXTURE_BLEND, "RimeTextureBlendContainerElementData", true, false },
    { "2420ee41-1c4d-38bd-4203-777f31cf9c9a", 0x3450F88Au, BF6_RIME_CONTAINER, "DiceUIDataListElementData", false, false },
    { "6cc27f98-68ca-c25c-4e3d-fef09567419b", 0x036D24CDu, BF6_RIME_CONTAINER, "DiceUIUniformGridElementData", false, false },
    { "6293e80d-5933-9516-b3f9-a41fd52245b7", 0x3966ACCAu, BF6_RIME_CONTAINER, "DiceUIAnchorContainerElementData", true, false },
    { "295b0545-d165-e962-3ebe-e871ce63aa88", 0x7B914016u, BF6_RIME_INPUT_BEHAVIOR, "DiceUIInputBehaviorElementData", false, false },
    { "26a4798a-af71-7b75-23ae-8cd04884e94b", 0x15DCD86Cu, BF6_RIME_INPUT_BEHAVIOR, "BFUIMouseInputElementData", false, false }
    ,{ "751ec8e6-27dc-bc4f-6c40-7c076ef6eb20", 0x82B96795u, BF6_RIME_LAYERED_ICON_BINDING, "751ec8e6-27dc-bc4f-6c40-7c076ef6eb20", false, false }
    ,{ "ab871b92-e85b-4ba1-c980-7aca88024128", 0xB4383D67u, BF6_RIME_HARDWARE_ICON_BINDING, "ab871b92-e85b-4ba1-c980-7aca88024128", false, false }
    ,{ "9792d266-dad3-ed84-7a55-de19ee7190fa", 0xA6CCF224u, BF6_RIME_REMOTE_WIDGET_PRESENTER, "9792d266-dad3-ed84-7a55-de19ee7190fa", false, false }
};

const bf6__rime_type* bf6__rime_type_of(const bf6::Ebx& e, size_t i)
{
    const std::string g = bf6::TypeDb::guid_str(e.instance_type(i));
    const uint32_t s = e.instance_signature(i);
    for (size_t k = 0; k < sizeof(kRimeTypes) / sizeof(kRimeTypes[0]); k++)
        if (s == kRimeTypes[k].signature && g == kRimeTypes[k].guid)
            return &kRimeTypes[k];
    return nullptr;
}

bool bf6__rime_read(const std::vector<uint8_t>& d, int64_t off, void* dst, size_t n)
{
    if (off < 0 || (uint64_t)off + n > d.size()) return false;
    std::memcpy(dst, d.data() + (size_t)off, n);
    return true;
}

bool bf6__rime_i32(const std::vector<uint8_t>& d, int64_t off, int32_t& out)
{ return bf6__rime_read(d, off, &out, 4); }
bool bf6__rime_u32(const std::vector<uint8_t>& d, int64_t off, uint32_t& out)
{ return bf6__rime_read(d, off, &out, 4); }
bool bf6__rime_f32(const std::vector<uint8_t>& d, int64_t off, float& out)
{ return bf6__rime_read(d, off, &out, 4); }
bool bf6__rime_u8(const std::vector<uint8_t>& d, int64_t off, uint8_t& out)
{ return bf6__rime_read(d, off, &out, 1); }

std::string bf6__rime_trim(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0' || s.back() == '\r'))
        s.pop_back();
    return s;
}

std::string bf6__rime_key(const std::string& s)
{
    std::string out;
    for (char ch : s)
        if (std::isalnum((unsigned char)ch))
            out.push_back((char)std::tolower((unsigned char)ch));
    return out;
}

/* Authored asset paths are stored with capitals; the mount's name table is
 * lowercase. Everything that resolves a path the data names has to fold it. */
std::string bf6__rime_key_path(std::string s)
{
    for (char& ch : s)
    {
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (ch == '\\') ch = '/';
    }
    return s;
}

std::string bf6__rime_basename(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    size_t a = slash == std::string::npos ? 0 : slash + 1;
    size_t b = path.size();
    if (b >= a + 4 && path.compare(b - 4, 4, ".ebx") == 0) b -= 4;
    return path.substr(a, b - a);
}

/* CString is relative to THE FIELD CELL.  The earlier C++ reader used the
 * payload base here; that mistake often lands on another string and therefore
 * looks valid. */
std::string bf6__rime_cstr(const std::vector<uint8_t>& d, int64_t cell)
{
    int32_t rel = 0;
    if (!bf6__rime_i32(d, cell, rel) || rel == 0 || rel == -1) return std::string();
    const int64_t abs = cell + (int64_t)rel;
    if (abs < 0 || abs >= (int64_t)d.size()) return std::string();
    size_t n = 0;
    while ((uint64_t)abs + n < d.size() && d[(size_t)abs + n]) n++;
    return bf6__rime_trim(std::string((const char*)d.data() + abs, n));
}

void bf6__rime_axis(const std::vector<uint8_t>& d, int64_t off, bf6_rime_axis& a)
{
    float v[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 6; i++) bf6__rime_f32(d, off + (int64_t)i * 4, v[i]);
    /* RimeAxisLayoutData: AnchorStart, SizingWeight, SizingPivot, OffsetEnd,
     * AnchorEnd, OffsetStart. */
    a.anchor_start = v[0];
    a.anchor_end   = v[4];
    a.offset_start = v[5];
    a.offset_end   = v[3];
    a.pivot        = v[2];
    a.weight       = v[1];
}

void bf6__rime_padding(const std::vector<uint8_t>& d, int64_t off,
                       float& l, float& t, float& r, float& b)
{
    int32_t q[4] = { 0, 0, 0, 0 };
    bf6__rime_read(d, off, q, sizeof(q));
    /* RimeEdgeSizes is Bottom, Left, Top, Right. */
    b = (float)q[0]; l = (float)q[1]; t = (float)q[2]; r = (float)q[3];
}

/* Linear light -> sRGB byte.  The standard transfer, and it is not optional:
 * 253 of the palette's 261 channels round-trip to an EXACT sRGB byte, which is
 * what a hex palette converted to linear looks like and not what hand-tuned
 * linear floats look like.  Encode wrongly and FE-Neutral #BFCAD1 - the single
 * most-used colour in the front end - comes out olive. */
uint8_t bf6__rime_srgb(float linear)
{
    if (!(linear > 0.f)) return 0;          /* also catches NaN */
    if (linear > 1.f) linear = 1.f;
    const float s = linear <= 0.0031308f
                  ? 12.92f * linear
                  : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    const int b = (int)(s * 255.0f + 0.5f);
    return (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
}

uint32_t bf6__rime_pack_srgb(const float rgb[3])
{
    return ((uint32_t)bf6__rime_srgb(rgb[0]) << 16)
         | ((uint32_t)bf6__rime_srgb(rgb[1]) << 8)
         |  (uint32_t)bf6__rime_srgb(rgb[2]);
}

/* Pull the three Vec3 components out of a decoded struct.  The palette entry
 * stores them as named fields, so this does not depend on field ORDER - which
 * matters, because the two entry shapes (accessible and plain) differ. */
bool bf6__rime_vec3(const bf6::EbxValue& v, float out[3])
{
    static const uint32_t kXYZ[3] = { 956422932u, 1123815262u, 849976220u };
    if (v.kind != bf6::EbxValue::Kind::Struct) return false;
    for (int k = 0; k < 3; k++)
    {
        const bf6::EbxValue* c = v.field(kXYZ[k]);
        if (!c || c->kind != bf6::EbxValue::Kind::Real) return false;
        out[k] = (float)c->f;
    }
    return true;
}

/* Collect every palette entry reachable from one decoded instance.
 *
 * Deliberately shape-driven rather than name-driven at the top level: the seven
 * assets are two different classes (AccessibleColorListAsset holds
 * AccessibleColorData with four accessibility variants, colorlist.ebx holds
 * plain ColorData with one), and both spell the list field differently in the
 * schema.  What they agree on is the ENTRY: a struct that carries a ColorId.
 * So walk arrays and accept an element as an entry when it names one.
 *
 * The DEFAULT colour is taken, never an accessibility variant: those are the
 * deuteranopia/tritanopia/protanopia overrides and picking one would silently
 * repaint the whole front end for a player who did not ask for it. */
void bf6__rime_collect_colors(const bf6::EbxValue& v, const char* palette,
                              std::vector<bf6_rime_color>& out, int depth)
{
    if (depth > 4) return;
    if (v.kind == bf6::EbxValue::Kind::Array)
    {
        for (const bf6::EbxValue& it : v.items)
            bf6__rime_collect_colors(it, palette, out, depth + 1);
        return;
    }
    if (v.kind != bf6::EbxValue::Kind::Struct) return;

    /* ColorId is a STRING here, not a Guid, even though it spells one. That is
     * what makes the join to an element's SelectedColorId a direct string
     * compare with no formatting convention to get wrong. */
    const bf6::EbxValue* id = v.field(kRimeFieldColorId);
    if (!id || id->kind != bf6::EbxValue::Kind::Str || id->s.empty())
    {
        for (const auto& kv : v.fields)
            bf6__rime_collect_colors(kv.second, palette, out, depth + 1);
        return;
    }

    /* DefaultColorValue first, ColorValue second: the accessible entry carries
     * both a default and three variants, the plain entry only ColorValue. */
    const bf6::EbxValue* col = v.field(kRimeFieldDefaultColorVal);
    if (!col) col = v.field(kRimeFieldColorValue);
    float rgb[3] = { 0, 0, 0 };
    if (!col || !bf6__rime_vec3(*col, rgb)) return;

    bf6_rime_color e{};
    put_str(e.id, sizeof(e.id), id->s);
    if (const bf6::EbxValue* nm = v.field(kRimeFieldName))
        if (nm->kind == bf6::EbxValue::Kind::Str)
            put_str(e.name, sizeof(e.name), nm->s);
    put_str(e.palette, sizeof(e.palette), palette);
    e.linear[0] = rgb[0]; e.linear[1] = rgb[1]; e.linear[2] = rgb[2];
    e.rgb = bf6__rime_pack_srgb(rgb);
    out.push_back(e);
}

/* The palette, read once per context.  Returns false only when NOTHING was
 * readable; a partial read is reported through the count so a caller can see
 * the coverage rather than be told everything is fine. */
bool bf6__rime_palette_load(bf6_ctx* c)
{
    if (c->rime_palette_tried) return !c->rime_palette.empty();
    c->rime_palette_tried = true;

    bool loud = false;
#if defined(_MSC_VER)
    char* debug_env = nullptr;
    size_t debug_env_len = 0;
    if (_dupenv_s(&debug_env, &debug_env_len, "BF6_RIME_PALETTE_DEBUG") == 0)
        loud = debug_env_len > 0;
    std::free(debug_env);
#else
    loud = std::getenv("BF6_RIME_PALETTE_DEBUG") != nullptr;
#endif
    for (const char* p : kRimePalettes)
    {
        std::string err;
        std::vector<uint8_t> raw = c->src.get_ebx(std::string(p) + ".ebx", err);
        if (raw.empty()) raw = c->src.get_ebx(p, err);
        if (raw.empty())
        {
            if (loud) std::fprintf(stderr, "palette %s: unreadable (%s)\n",
                                   p, err.c_str());
            continue;
        }

        bf6::Ebx e(*c->types);
        if (!e.parse(std::move(raw), err))
        {
            if (loud) std::fprintf(stderr, "palette %s: parse failed (%s)\n",
                                   p, err.c_str());
            continue;
        }
        const std::string label = bf6__rime_basename(p);
        const size_t before = c->rime_palette.size();
        for (size_t i = 0; i < e.instance_count(); i++)
        {
            bf6__rime_collect_colors(e.read_instance(i), label.c_str(),
                                     c->rime_palette, 0);
        }
        if (loud)
            std::fprintf(stderr, "palette %s: instances=%zu entries=%zu\n", p,
                         e.instance_count(), c->rime_palette.size() - before);
    }

    for (size_t i = 0; i < c->rime_palette.size(); i++)
        c->rime_palette_by_id.emplace(c->rime_palette[i].id, i);
    return !c->rime_palette.empty();
}

/* THE FONT CHAIN, ALL OF IT LIVE.
 *
 *   RimeLabelElementData.FontStyle  ->  a RimeFontStyle asset
 *   RimeFontStyle                   ->  PointSize, LineHeight, and a per-locale
 *                                       list of FontAsset paths
 *   FontAsset                       ->  family, weight, and a RES payload
 *   the RES payload                 ->  raw sfnt bytes
 *
 * Nothing in that chain needs a file beside the viewer. Language 0 is the
 * default entry and is the one taken here; the locale variants are read past
 * because a renderer that has not been told a language must not pick one.
 */
void bf6__rime_font_style_from(const bf6::EbxValue& v, const std::string& path,
                               bf6_rime_font_style& o)
{
    o = bf6_rime_font_style{};
    put_str(o.name, sizeof(o.name), bf6__rime_basename(path));
    if (const bf6::EbxValue* nm = v.field(kRimeFieldAssetName))
        if (nm->kind == bf6::EbxValue::Kind::Str && !nm->s.empty())
            put_str(o.name, sizeof(o.name), nm->s);

    /* THE DEFAULT METRICS ARE TOP-LEVEL FIELDS, NOT ARRAY ROW ZERO.
     *
     * The style carries PointSize and LineHeight on the instance itself, and
     * separately a per-locale OVERRIDE array keyed by Language. That array has
     * no Language 0 row - the languages in it are 1, 6, 11, 12, 13, 16 - so
     * searching it for the default finds either nothing or, worse, a CJK
     * override that happens to sit first. Reading a row instead of the field
     * gave 20.00 pt on a style whose own filename says 21 pt, which is exactly
     * the kind of wrong that renders without complaint. */
    if (const bf6::EbxValue* ps = v.field(kRimeFieldPointSize))
        if (ps->kind == bf6::EbxValue::Kind::Real) o.point_size = (float)ps->f;
    if (const bf6::EbxValue* lh = v.field(kRimeFieldLineHeight))
        o.line_height = lh->kind == bf6::EbxValue::Kind::Real
                      ? (float)lh->f : (float)lh->i;
    /* RimeTextCapitalization.  Read as the enum's raw value; 1 is the only
     * non-zero value in the shipped front end (the *_uppercase styles). */
    if (const bf6::EbxValue* cap = v.field(kRimeFieldCapitalization))
        o.capitalization = cap->kind == bf6::EbxValue::Kind::Int
                         ? (int32_t)cap->i : (int32_t)cap->u;   /* enum: raw u32 */
    if (std::getenv("BF6_FONTSTYLE_DEBUG"))
    {
        std::fprintf(stderr, "fontstyle %s fields:", o.name);
        for (const auto& kv : v.fields)
            std::fprintf(stderr, " %08x(k%d,i%lld,u%llu)", kv.first, (int)kv.second.kind,
                         (long long)kv.second.i, (unsigned long long)kv.second.u);
        std::fprintf(stderr, "\n");
    }

    /* The font-asset list DOES key Language 0 as the default. */
    for (const auto& kv : v.fields)
    {
        const bf6::EbxValue& arr = kv.second;
        if (arr.kind != bf6::EbxValue::Kind::Array) continue;
        for (const bf6::EbxValue& row : arr.items)
        {
            if (row.kind != bf6::EbxValue::Kind::Struct) continue;
            const bf6::EbxValue* lang = row.field(kRimeFieldLanguage);
            const bf6::EbxValue* p = row.field(kRimeFieldFontPath);
            if (!lang || lang->i != 0 || !p) continue;
            if (p->kind == bf6::EbxValue::Kind::Str && !p->s.empty() && !o.font_asset[0])
                put_str(o.font_asset, sizeof(o.font_asset), bf6__rime_key_path(p->s));
        }
    }
}

bool bf6__rime_font_styles_load(bf6_ctx* c)
{
    if (c->rime_fonts_tried) return !c->rime_font_styles.empty();
    c->rime_fonts_tried = true;

    const int n = bf6_list_ebx(c, "fonts/fontstyles", nullptr, 0);
    if (n <= 0) return false;
    std::vector<bf6_asset> rows((size_t)n);
    const int got = bf6_list_ebx(c, "fonts/fontstyles", rows.data(), n);
    for (int i = 0; i < got; i++)
    {
        const std::string path = rows[(size_t)i].name ? rows[(size_t)i].name : "";
        if (path.empty()) continue;
        std::string err;
        std::vector<uint8_t> raw = c->src.get_ebx(path + ".ebx", err);
        if (raw.empty()) raw = c->src.get_ebx(path, err);
        if (raw.empty()) continue;
        bf6::Ebx e(*c->types);
        if (!e.parse(std::move(raw), err)) continue;
        for (size_t k = 0; k < e.instance_count(); k++)
        {
            bf6_rime_font_style st{};
            bf6__rime_font_style_from(e.read_instance(k), path, st);
            if (st.point_size <= 0.f || !st.font_asset[0]) continue;

            /* The family and weight live on the FontAsset, one hop further. */
            std::string ferr;
            std::vector<uint8_t> fraw = c->src.get_ebx(std::string(st.font_asset) + ".ebx", ferr);
            if (fraw.empty()) fraw = c->src.get_ebx(st.font_asset, ferr);
            if (!fraw.empty())
            {
                bf6::Ebx fe(*c->types);
                if (fe.parse(std::move(fraw), ferr) && fe.instance_count())
                {
                    const bf6::EbxValue fv = fe.read_instance(0);
                    if (const bf6::EbxValue* fam = fv.field(kRimeFieldFamily))
                        if (fam->kind == bf6::EbxValue::Kind::Str)
                            put_str(st.family, sizeof(st.family), fam->s);
                    if (const bf6::EbxValue* wt = fv.field(kRimeFieldWeight))
                        st.weight = (int32_t)wt->i;
                }
            }
            c->rime_font_styles.push_back(st);
            /* Keyed on the STEM. The authored Name is a full logical path and
             * a label's import resolves to a partition path; joining those two
             * spellings directly matched nothing, which showed up as 32 of 32
             * labels finding a style and 0 of 32 finding a size. */
            c->rime_font_by_name.emplace(bf6__rime_key(bf6__rime_basename(st.name)),
                                         c->rime_font_styles.size() - 1);
            c->rime_font_by_name.emplace(bf6__rime_key(bf6__rime_basename(path)),
                                         c->rime_font_styles.size() - 1);
            break;   /* one style per asset */
        }
    }
    return !c->rime_font_styles.empty();
}

/* One element's own ColorValue.  `id` is the authored ColorId or empty; `raw`
 * is the vestigial RawColor.  Resolution against the palette and inheritance
 * from an ancestor both happen in the walk, where the tree is known. */
void bf6__rime_read_color(const std::vector<uint8_t>& d, int64_t color_base,
                          std::string& id, float raw[3])
{
    id = bf6__rime_cstr(d, color_base + kRimeColorIdOff);
    for (int k = 0; k < 3; k++)
        if (!bf6__rime_f32(d, color_base + kRimeColorRawOff + k * 4, raw[k]))
            raw[k] = 1.0f;
}

int bf6__rime_instance_at(const bf6::Ebx& e, int64_t payload_relative)
{
    if (payload_relative < 0) return -1;
    for (size_t i = 0; i < e.instance_count(); i++)
        if ((int64_t)e.instance_offset(i) == payload_relative) return (int)i;
    return -1;
}

int bf6__rime_internal_ptr(const bf6::Ebx& e, const std::vector<uint8_t>& d,
                           int64_t cell)
{
    int32_t v = 0;
    if (!bf6__rime_i32(d, cell, v) || v == 0 || (v & 1)) return -1;
    return bf6__rime_instance_at(e, cell + (int64_t)v - e.payload());
}

bool bf6__rime_array(const std::vector<uint8_t>& d, int64_t cell,
                     int64_t& first, int32_t& count)
{
    int32_t rel = 0;
    if (!bf6__rime_i32(d, cell, rel) || rel == 0) return false;
    first = cell + (int64_t)rel;
    if (!bf6__rime_i32(d, first - 4, count) || count < 0 || count > 1000000)
        return false;
    return first >= 0 && first + (int64_t)count * 8 <= (int64_t)d.size();
}

void bf6__rime_init_node(bf6_rime_node& o, const char* partition,
                         const bf6::Ebx& e, size_t i, int depth, int parent)
{
    o = bf6_rime_node{};
    o.progress_orientation = -1;
    o.border_alignment = -1;
    o.border_gradient_direction = -1;
    put_str(o.partition, sizeof(o.partition), partition ? partition : "");
    put_str(o.type_guid, sizeof(o.type_guid), bf6::TypeDb::guid_str(e.instance_type(i)));
    o.type_signature = e.instance_signature(i);
    o.kind = BF6_RIME_UNKNOWN;
    o.depth = depth;
    o.parent = parent;
    o.instance = (int32_t)i;
    o.stack_orientation = -1;
    o.stack_overflow_mode = -1;
    o.stack_size_distribution = -1;
    o.stack_space_distribution = -1;
    o.stack_preserve_fit_content = -1;
    o.widget_use_width = -1;
    o.widget_use_height = -1;
    o.container_flow_direction = -1;
    o.masking_mode = -1;
    o.invert_mask = -1;
    o.mask_count = 0;
    o.mask_owner = -1;
    o.mask_gradient_end = 1.f;
    o.mask_gradient_start = 0.f;
    o.mask_threshold = 0.5f;
    for (size_t mask_index = 0; mask_index < 4; ++mask_index)
        o.mask_target_instances[mask_index] = -1;
    o.visible = 1;
    o.alpha = 1.0f;
    o.color_rgb = 0xFFFFFFu;             /* white until something names it */
    o.color_source = BF6_RIME_COLOR_NONE;
    o.repeat_instances = 0;
    o.repeat_distribution = -1;
    o.has_attach = 0;
    o.attach_x = o.attach_y = 0.f;
    o.item_template[0] = 0;
    o.item_template_count = 0;
    for (size_t template_index = 0; template_index < 8; ++template_index)
        o.item_templates[template_index][0] = 0;
    o.grid_orientation = -1;
    o.grid_static_segment_item_count = -1;
    o.grid_column_size = o.grid_row_size = 0.f;
    o.grid_row_spacing = o.grid_column_spacing = 0.f;
    o.grid_segment_distribution = -1;
    o.grid_segment_count_mode = -1;
    o.grid_column_flow_direction = -1;
    o.grid_row_flow_direction = -1;
    o.grid_item_fit_content = -1;
    o.data_list_size_distribution = -1;
    o.data_list_flow_direction = -1;
    o.data_list_space_distribution = -1;
    o.data_list_preserve_fit_content = -1;
    o.viewport_flow_direction = -1;
    o.viewport_extend_top = -1;
    o.viewport_extend_bottom = -1;
    o.viewport_extend_left = -1;
    o.viewport_extend_right = -1;
    o.image_asset[0] = 0;
    o.image_uv[0] = o.image_uv[1] = 0.f;
    o.image_uv[2] = o.image_uv[3] = 1.f;
    o.image_resize_mode = -1;
    o.image_address_v = -1;
    o.image_address_u = -1;
    o.image_clip_to_bounds = -1;
    o.fill_kind = BF6_RIME_FILL_FLAT;
    o.fill_direction = BF6_RIME_GRADIENT_HORIZONTAL;
    o.fill_alpha_start = o.fill_alpha_end = 1.0f;
    o.blend_mode = -1;
}

struct bf6__rime_walk {
    bf6_ctx* c;
    int max_ref_depth;
    std::vector<std::string> open;
    int active_mask_owner = -1;

    bool load(const std::string& partition, bf6::Ebx& e)
    {
        std::string err;
        std::vector<uint8_t> raw = c->src.get_ebx(partition + ".ebx", err);
        if (raw.empty()) raw = c->src.get_ebx(partition, err);
        if (raw.empty()) return false;
        e.set_guid_index(&c->src.armory_partition_index());
        if (!e.parse(std::move(raw), err)) return false;
        return true;
    }

    bool resolve_import(const bf6::Ebx& owner, int imp, std::string& target)
    {
        target.clear();
        if (imp < 0 || (size_t)imp >= owner.import_count()) return false;
        const auto& ref = owner.import_at((size_t)imp);
        const auto& candidates = c->src.armory_partition_candidates();
        const auto hit = candidates.find(ref.partition);
        if (hit == candidates.end() || hit->second.empty()) return false;
        if (hit->second.size() == 1) target = hit->second.front();
        else
        {
            std::vector<std::string> identity_matches;
            for (const std::string& candidate : hit->second)
            {
                bf6::Ebx probe(*c->types);
                std::string p = candidate;
                if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ebx") == 0)
                    p.resize(p.size() - 4);
                if (!load(p, probe)) continue;
                for (size_t q = 0; q < probe.exported_count(); q++)
                    if (probe.instance_guid(q) == ref.instance)
                    { identity_matches.push_back(candidate); break; }
            }
            if (identity_matches.size() == 1) target = identity_matches.front();
            else return false; /* an ambiguous raw binding must stay empty */
        }
        if (target.size() > 4 && target.compare(target.size() - 4, 4, ".ebx") == 0)
            target.resize(target.size() - 4);
        return true;
    }

    /* Resolve one element's own colour.  Only its OWN: inheritance needs the
     * finished tree and runs afterwards.
     *
     * The order is the measured one - a named id wins, a non-white RawColor is
     * next, and white-with-no-id is left as NONE for the inheritance pass to
     * fill.  Over the 419 elements of the weapon screen 0 carry a non-white
     * RawColor with no id, so the middle branch is expected to be dead; it is
     * here so that if the game ever does author one, it paints rather than
     * silently inheriting something else. */
    void apply_color(bf6_rime_node& o, const std::vector<uint8_t>& d,
                     int64_t color_base)
    {
        std::string id;
        float raw[3] = { 1.f, 1.f, 1.f };
        bf6__rime_read_color(d, color_base, id, raw);

        if (!id.empty())
        {
            put_str(o.color_id, sizeof(o.color_id), id);
            if (bf6__rime_palette_load(c))
            {
                const auto hit = c->rime_palette_by_id.find(id);
                if (hit != c->rime_palette_by_id.end())
                {
                    const bf6_rime_color& e = c->rime_palette[hit->second];
                    o.color_rgb = e.rgb;
                    put_str(o.color_name, sizeof(o.color_name), e.name);
                    o.color_source = BF6_RIME_COLOR_PALETTE;
                    return;
                }
            }
            /* An id we cannot resolve is NOT white. Leaving it NONE lets it
             * inherit, which is the same thing the renderer does with an
             * element that named nothing, and keeps the id visible for a
             * caller that wants to count the misses. */
            return;
        }

        if (raw[0] < 0.999f || raw[1] < 0.999f || raw[2] < 0.999f)
        {
            o.color_rgb = bf6__rime_pack_srgb(raw);
            o.color_source = BF6_RIME_COLOR_RAW;
        }
    }

    /* A label's authored text style.
     *
     * FontStyle is a cross-partition import, so it is resolved the same way a
     * widget reference is: take the import's partition GUID and look it up in
     * the live armory index. The style NAME is what lands on the node, because
     * the styles themselves are read once into their own table and a caller
     * wanting the metrics joins on it.
     *
     * The per-element PointSize at +384 is an override and is 0 on every
     * shipped label, so it is read but only used when non-zero - if the game
     * ever authors one, it wins, which is what an override means. */
    void apply_font(bf6_rime_node& o, const bf6::Ebx& e,
                    const std::vector<uint8_t>& d, int64_t base)
    {
        float over = 0.f;
        if (bf6__rime_f32(d, base + kRimeLabelPtSizeOff, over) && over > 0.f)
            o.point_size = over;

        int32_t v = 0;
        if (!bf6__rime_i32(d, base + kRimeLabelStyleOff, v) || !(v & 1)) return;
        const int imp = v >> 1;
        if (imp < 0 || (size_t)imp >= e.import_count()) return;
        const auto& candidates = c->src.armory_partition_candidates();
        const auto hit = candidates.find(e.import_at((size_t)imp).partition);
        if (hit == candidates.end() || hit->second.empty()) return;

        const std::string stem = bf6__rime_basename(hit->second.front());
        put_str(o.font_style, sizeof(o.font_style), stem);
        if (!bf6__rime_font_styles_load(c)) return;
        const auto fs = c->rime_font_by_name.find(bf6__rime_key(stem));
        if (fs == c->rime_font_by_name.end()) return;
        const bf6_rime_font_style& st = c->rime_font_styles[fs->second];
        if (o.point_size <= 0.f) o.point_size = st.point_size;
        o.line_height = st.line_height;
        o.text_capitalization = st.capitalization;
    }

    /* A fill element's style. Resolves internally or across a partition, like
     * every other Rime reference. A style that resolves to nothing leaves the
     * fill FLAT, which is the same thing a null style means - so a failed
     * resolve degrades to the correct default rather than to a blank box. */
    void apply_fill(bf6_rime_node& o, const bf6::Ebx& e,
                    const std::vector<uint8_t>& d, int64_t base)
    {
        int32_t v = 0;
        if (!bf6__rime_i32(d, base + kRimeFillStyleOff, v) || v == 0) return;

        bf6::Ebx other(*c->types);
        const bf6::Ebx* src = &e;
        int styleInst = -1;
        if (v & 1)
        {
            const int imp = v >> 1;
            if (imp < 0 || (size_t)imp >= e.import_count()) return;
            const auto& cand = c->src.armory_partition_candidates();
            const auto hit = cand.find(e.import_at((size_t)imp).partition);
            if (hit == cand.end() || hit->second.empty()) return;
            std::string p2 = hit->second.front();
            if (p2.size() > 4 && p2.compare(p2.size() - 4, 4, ".ebx") == 0) p2.resize(p2.size() - 4);
            std::string oerr;
            std::vector<uint8_t> oraw = c->src.get_ebx(p2 + ".ebx", oerr);
            if (oraw.empty()) oraw = c->src.get_ebx(p2, oerr);
            if (oraw.empty()) return;
            other.set_guid_index(&c->src.armory_partition_index());
            if (!other.parse(std::move(oraw), oerr)) return;
            src = &other;
            for (size_t q = 0; q < other.exported_count(); q++)
                if (other.instance_guid(q) == e.import_at((size_t)imp).instance)
                { styleInst = (int)q; break; }
            if (styleInst < 0) styleInst = 0;
        }
        else
        {
            styleInst = bf6__rime_internal_ptr(e, d, base + kRimeFillStyleOff);
            if (styleInst < 0) return;
        }

        const std::vector<uint8_t>& sd = src->raw();
        const int64_t sb = src->payload() + (int64_t)src->instance_offset((size_t)styleInst);
        int64_t lf = 0; int32_t ln = 0;
        if (!bf6__rime_array(sd, sb + kRimeFillLayersOff, lf, ln) || ln <= 0) return;

        /* The shipped styles are a single layer each; take the first and record
         * that a second would be ignored rather than silently blended. */
        const int layer = bf6__rime_internal_ptr(*src, sd, lf);
        if (layer < 0) return;
        const int64_t lb = src->payload() + (int64_t)src->instance_offset((size_t)layer);

        int32_t bm = -1;
        if (bf6__rime_i32(sd, lb + kRimeLayerBlendOff, bm)) o.blend_mode = bm;

        float a0 = 1.f, a1 = 1.f;
        int32_t dir = 0;
        const bool ok0 = bf6__rime_f32(sd, lb + kRimeGradStartAlpha, a0);
        const bool ok1 = bf6__rime_f32(sd, lb + kRimeGradEndAlpha, a1);
        bf6__rime_i32(sd, lb + kRimeGradDirection, dir);
        /* Only accept it as a gradient when both alphas read as real fractions
         * and they actually differ - a layer that is 1.0 to 1.0 is a flat fill
         * wearing a gradient's clothes, and drawing it as a ramp would be a
         * change with no authority behind it. */
        if (ok0 && ok1 && a0 >= 0.f && a0 <= 1.f && a1 >= 0.f && a1 <= 1.f &&
            (a0 != a1) && (dir == BF6_RIME_GRADIENT_HORIZONTAL ||
                           dir == BF6_RIME_GRADIENT_VERTICAL))
        {
            o.fill_kind = BF6_RIME_FILL_GRADIENT;
            o.fill_direction = dir;
            o.fill_alpha_start = a0;
            o.fill_alpha_end = a1;
        }
    }

    /* The asset an image element draws. Resolved to a partition path only -
     * decoding the pixels is the texture reader's job, and the vector path is
     * a different one again. */
    void apply_image(bf6_rime_node& o, const bf6::Ebx& e,
                     const std::vector<uint8_t>& d, int64_t base, int kind)
    {
        const uint32_t off = (kind == BF6_RIME_SVG) ? kRimeSvgImageOff
                           : (kind == BF6_RIME_MOVIE) ? kRimeMovieImageOff
                           : (kind == BF6_RIME_FLIPBOOK) ? kRimeFlipbookImageOff
                           : kRimeTexImageOff;
        if (kind == BF6_RIME_TEXTURE)
        {
            for (int i = 0; i < 4; i++)
                bf6__rime_f32(d, base + kRimeTexUvOff + i * 4, o.image_uv[i]);
            uint32_t mode = 0;
            if (bf6__rime_u32(d, base + kRimeTexResizeModeOff, mode))
                o.image_resize_mode = static_cast<int32_t>(mode);
            if (bf6__rime_u32(d, base + kRimeTexAddressVOff, mode))
                o.image_address_v = static_cast<int32_t>(mode);
            if (bf6__rime_u32(d, base + kRimeTexAddressUOff, mode))
                o.image_address_u = static_cast<int32_t>(mode);
            uint8_t clip = 0;
            if (bf6__rime_u8(d, base + kRimeTexClipToBoundsOff, clip))
                o.image_clip_to_bounds = clip ? 1 : 0;
        }

        int32_t v = 0;
        if (!bf6__rime_i32(d, base + off, v) || v == 0 || !(v & 1)) return;
        const int imp = v >> 1;
        if (imp < 0 || (size_t)imp >= e.import_count()) return;
        const auto& cand = c->src.armory_partition_candidates();
        const auto hit = cand.find(e.import_at((size_t)imp).partition);
        if (hit == cand.end() || hit->second.empty()) return;
        std::string p2 = hit->second.front();
        if (p2.size() > 4 && p2.compare(p2.size() - 4, 4, ".ebx") == 0) p2.resize(p2.size() - 4);
        put_str(o.image_asset, sizeof(o.image_asset), p2);
    }

    int add_unknown(const std::string& partition, const bf6::Ebx& e,
                    size_t i, int depth, int parent)
    {
        bf6_rime_node o{};
        bf6__rime_init_node(o, partition.c_str(), e, i, depth, parent);
        o.mask_owner = active_mask_owner;
        put_str(o.type_name, sizeof(o.type_name), "UnknownSignature");
        c->rime_tree_rows.push_back(o);
        c->rime_tree_stats.unknown_types++;
        return (int)c->rime_tree_rows.size() - 1;
    }

    void walk_node(const std::string& partition, bf6::Ebx& e, size_t i,
                   int depth, int parent, int ref_depth)
    {
        if (i >= e.instance_count()) return;
        const bf6__rime_type* ti = bf6__rime_type_of(e, i);
        if (!ti) { add_unknown(partition, e, i, depth, parent); return; }

        const std::vector<uint8_t>& d = e.raw();
        const int64_t base = e.payload() + (int64_t)e.instance_offset(i);
        bf6_rime_node o{};
        bf6__rime_init_node(o, partition.c_str(), e, i, depth, parent);
        o.mask_owner = active_mask_owner;
        o.kind = ti->kind;
        put_str(o.type_name, sizeof(o.type_name), ti->name);

        if (ti->kind == BF6_RIME_LAYER)
        {
            put_str(o.name, sizeof(o.name), bf6__rime_cstr(d, base + kRimeLayerNameOff));
            uint8_t b = 1;
            if (bf6__rime_u8(d, base + kRimeLayerVisibleOff, b)) o.visible = b ? 1 : 0;
        }
        else if (ti->kind == BF6_RIME_WIDGET_REFERENCE)
        {
            put_str(o.name, sizeof(o.name), bf6__rime_cstr(d, base + kRimeRefNameOff));
            bf6__rime_axis(d, base + kRimeRefHOff, o.h);
            bf6__rime_axis(d, base + kRimeRefVOff, o.v);
            bf6__rime_padding(d, base + kRimeRefPaddingOff,
                              o.pad_l, o.pad_t, o.pad_r, o.pad_b);
            bf6__rime_u32(d, base + kRimeRefNameHashOff, o.name_hash);
            bf6__rime_f32(d, base + kRimeRefWidthOff, o.width);
            bf6__rime_f32(d, base + kRimeRefHeightOff, o.height);
            bf6__rime_f32(d, base + kRimeRefAlphaOff, o.alpha);
            uint8_t b = 0;
            if (bf6__rime_u8(d, base + kRimeRefVisibleOff, b)) o.visible = b ? 1 : 0;
            if (bf6__rime_u8(d, base + kRimeRefFitWOff, b)) o.fit_w = b ? 1 : 0;
            if (bf6__rime_u8(d, base + kRimeRefFitHOff, b)) o.fit_h = b ? 1 : 0;
            if (bf6__rime_u8(d, base + kRimeRefUseWidthOff, b))
                o.widget_use_width = b ? 1 : 0;
            if (bf6__rime_u8(d, base + kRimeRefUseHeightOff, b))
                o.widget_use_height = b ? 1 : 0;
            apply_color(o, d, base + kRimeRefColorOff);
        }
        else
        {
            put_str(o.name, sizeof(o.name), bf6__rime_cstr(d, base + kRimeNameOff));
            /* Vertical precedes horizontal only on the ordinary element base. */
            bf6__rime_axis(d, base + kRimeVLayoutOff, o.v);
            bf6__rime_axis(d, base + kRimeHLayoutOff, o.h);
            bf6__rime_padding(d, base + kRimePaddingOff,
                              o.pad_l, o.pad_t, o.pad_r, o.pad_b);
            bf6__rime_u32(d, base + kRimeNameHashOff, o.name_hash);
            bf6__rime_f32(d, base + kRimeWidthOff, o.width);
            bf6__rime_f32(d, base + kRimeHeightOff, o.height);
            bf6__rime_f32(d, base + kRimeAlphaOff, o.alpha);
            uint8_t b = 0;
            if (bf6__rime_u8(d, base + kRimeVisibleOff, b)) o.visible = b ? 1 : 0;
            if (bf6__rime_u8(d, base + kRimeFitWOff, b)) o.fit_w = b ? 1 : 0;
            if (bf6__rime_u8(d, base + kRimeFitHOff, b)) o.fit_h = b ? 1 : 0;
            if (ti->kind == BF6_RIME_CONTAINER ||
                ti->kind == BF6_RIME_STACK_CONTAINER ||
                ti->kind == BF6_RIME_TEXTURE_BLEND)
            {
                uint32_t flow = 0;
                if (bf6__rime_u32(d, base + 328, flow) && flow <= 2)
                    o.container_flow_direction = (int32_t)flow;
            }
            if (std::strcmp(ti->name,
                            "RimeMaskingContainerElementData") == 0)
            {
                int64_t first = 0;
                int32_t count = 0;
                if (bf6__rime_array(d, base + kRimeMasksOff, first, count))
                {
                    o.mask_count = count;
                    const int32_t retained = count < 4 ? count : 4;
                    for (int32_t mask_index = 0; mask_index < retained;
                         ++mask_index)
                        o.mask_target_instances[mask_index] =
                            bf6__rime_internal_ptr(
                                e, d, first + (int64_t)mask_index * 8);
                }
                bf6__rime_f32(d, base + kRimeMaskGradEndOff,
                              o.mask_gradient_end);
                bf6__rime_f32(d, base + kRimeMaskGradStartOff,
                              o.mask_gradient_start);
                bf6__rime_f32(d, base + kRimeMaskThresholdOff,
                              o.mask_threshold);
                bf6__rime_i32(d, base + kRimeMaskModeOff,
                              o.masking_mode);
                uint8_t invert = 0;
                if (bf6__rime_u8(d, base + kRimeMaskInvertOff, invert))
                    o.invert_mask = invert ? 1 : 0;
            }
            if (ti->stack)
            {
                uint32_t orient = 0;
                bf6__rime_u32(d, base + 336, orient);
                o.stack_orientation = (int32_t)orient;
                bf6__rime_f32(d, base + 340, o.stack_wrap_spacing);
                bf6__rime_f32(d, base + 344, o.item_spacing);
                uint32_t mode = 0;
                if (bf6__rime_u32(d, base + 348, mode))
                    o.stack_overflow_mode = (int32_t)mode;
                if (bf6__rime_u32(d, base + 352, mode))
                    o.stack_size_distribution = (int32_t)mode;
                if (bf6__rime_u32(d, base + 356, mode))
                    o.stack_space_distribution = (int32_t)mode;
                if (bf6__rime_u8(d, base + 360, b))
                    o.stack_preserve_fit_content = b ? 1 : 0;
            }
            apply_color(o, d, base + kRimeColorOff);
            if (ti->kind == BF6_RIME_LABEL)
            {
                apply_font(o, e, d, base);
                /* The authored text sources.  StringId is the symbolic key
                 * the executable's localisation service hashes (see
                 * bf6_string_id_hash); RawText is declared but unused in
                 * the shipped front end and is read so that an authored
                 * one would surface rather than vanish. */
                put_str(o.text_string_id, sizeof(o.text_string_id),
                        bf6__rime_cstr(d, base + kRimeLabelSidOff));
                put_str(o.text_raw, sizeof(o.text_raw),
                        bf6__rime_cstr(d, base + kRimeLabelRawOff));
                /* Layout and markup flags the label authors itself.  All
                 * read from the shipped cells named in the 2026-09-05 schema
                 * audit; -1 means the cell was not readable. */
                uint32_t u = 0;
                o.text_valign = bf6__rime_u32(d, base + kRimeLabelVAlignOff, u) ? (int32_t)u : -1;
                o.text_halign = bf6__rime_u32(d, base + kRimeLabelHAlignOff, u) ? (int32_t)u : -1;
                o.text_overflow_mode = bf6__rime_u32(d, base + kRimeLabelOverflowOff, u) ? (int32_t)u : -1;
                uint8_t b8 = 0;
                o.text_parse_markup = bf6__rime_u8(d, base + kRimeLabelParseMarkupOff, b8) ? (b8 ? 1 : 0) : -1;
                o.text_word_wrap = bf6__rime_u8(d, base + kRimeLabelWordWrapOff, b8) ? (b8 ? 1 : 0) : -1;
                bf6__rime_f32(d, base + kRimeLabelSpacingOff, o.text_spacing);
            }
            if (ti->kind == BF6_RIME_FILL) apply_fill(o, e, d, base);
            /* The authored fit/alignment of the image family, plus the
             * per-kind extras a renderer needs for the kinds that currently
             * fall through its paint switch.  -1 means the element does not
             * carry the cell; 0 is a real authored value. */
            {
                const bf6__rime_image_layout* lay = nullptr;
                switch (ti->kind)
                {
                case BF6_RIME_TEXTURE:       lay = &kRimeTextureLayout;  break;
                case BF6_RIME_SVG:           lay = &kRimeSvgLayout;      break;
                case BF6_RIME_FLIPBOOK:      lay = &kRimeFlipbookLayout; break;
                case BF6_RIME_VECTOR_SHAPE:  lay = &kRimeVecLayout;      break;
                case BF6_RIME_TEXTURE_BLEND: lay = &kRimeTexBlendLayout; break;
                default: break;
                }
                uint32_t u = 0;
                uint8_t  b8 = 0;
                if (lay)
                {
                    o.image_h_align = bf6__rime_u32(d, base + lay->h_align, u) ? (int32_t)u : -1;
                    o.image_v_align = bf6__rime_u32(d, base + lay->v_align, u) ? (int32_t)u : -1;
                    /* ClipToBounds for the kinds that carry it.  Texture
                     * already resolved it in apply_image; do not overwrite
                     * that read with a second one at the same cell. */
                    if (lay->clip_to_bounds && ti->kind != BF6_RIME_TEXTURE)
                    {
                        uint8_t clip = 0;
                        if (bf6__rime_u8(d, base + lay->clip_to_bounds, clip))
                            o.image_clip_to_bounds = clip ? 1 : 0;
                    }
                    /* Texture already resolved its ResizeMode in apply_image;
                     * the other four kinds keep it at their own offset. */
                    if (ti->kind != BF6_RIME_TEXTURE &&
                        bf6__rime_u32(d, base + lay->resize_mode, u))
                        o.image_resize_mode = (int32_t)u;
                }
                if (ti->kind == BF6_RIME_SVG)
                {
                    bf6__rime_f32(d, base + kRimeSvgRasterSizeOff, o.svg_raster_size[0]);
                    bf6__rime_f32(d, base + kRimeSvgRasterSizeOff + 4, o.svg_raster_size[1]);
                    o.svg_raster_size_mode =
                        bf6__rime_u32(d, base + kRimeSvgRasterSizeModeOff, u) ? (int32_t)u : -1;
                }
                else if (ti->kind == BF6_RIME_FLIPBOOK)
                {
                    bf6__rime_f32(d, base + kRimeFlipProgressOff, o.flipbook_progress);
                    o.flipbook_auto_play =
                        bf6__rime_u8(d, base + kRimeFlipAutoPlayOff, b8) ? (b8 ? 1 : 0) : -1;
                    o.flipbook_loop =
                        bf6__rime_u8(d, base + kRimeFlipLoopOff, b8) ? (b8 ? 1 : 0) : -1;
                    o.flipbook_random_frames =
                        bf6__rime_u8(d, base + kRimeFlipRandomOff, b8) ? (b8 ? 1 : 0) : -1;
                }
                else if (ti->kind == BF6_RIME_VECTOR_SHAPE)
                {
                    bf6__rime_f32(d, base + kRimeVecGlowSizeOff, o.vector_glow_size);
                    o.vector_scale_line_widths =
                        bf6__rime_u8(d, base + kRimeVecScaleLineWidthOff, b8) ? (b8 ? 1 : 0) : -1;
                }
                else if (ti->kind == BF6_RIME_TEXTURE_BLEND)
                {
                    bf6__rime_f32(d, base + kRimeBlendGradStartOff, o.blend_gradient_start);
                    bf6__rime_f32(d, base + kRimeBlendGradEndOff, o.blend_gradient_end);
                    bf6__rime_f32(d, base + kRimeBlendThresholdOff, o.blend_mask_threshold);
                    o.blend_invert_mask =
                        bf6__rime_u8(d, base + kRimeBlendInvertOff, b8) ? (b8 ? 1 : 0) : -1;
                    /* Resolve the container's own texture through the same
                     * import path apply_image uses for the ordinary kinds. */
                    int32_t ref = 0;
                    if (bf6__rime_i32(d, base + kRimeBlendTextureOff, ref) && ref != 0 && (ref & 1))
                    {
                        const int imp = ref >> 1;
                        if (imp >= 0 && (size_t)imp < e.import_count())
                        {
                            const auto& cand = c->src.armory_partition_candidates();
                            const auto hit = cand.find(e.import_at((size_t)imp).partition);
                            if (hit != cand.end() && !hit->second.empty())
                            {
                                std::string p2 = hit->second.front();
                                if (p2.size() > 4 && p2.compare(p2.size() - 4, 4, ".ebx") == 0)
                                    p2.resize(p2.size() - 4);
                                put_str(o.image_asset, sizeof(o.image_asset), p2);
                            }
                        }
                    }
                }
            }
            if (ti->kind == BF6_RIME_SVG || ti->kind == BF6_RIME_TEXTURE ||
                ti->kind == BF6_RIME_MOVIE || ti->kind == BF6_RIME_FLIPBOOK)
                apply_image(o, e, d, base, ti->kind);
            {
                int32_t bm = -1;
                if (bf6__rime_i32(d, base + kRimeBlendModeOff, bm) &&
                    bm >= 0 && bm < 16)
                    o.blend_mode = bm;
            }
            /* Anchor containers carry the attach point the leader line leaves
             * from.  Gated on the concrete type name, not the kind: the kind
             * is CONTAINER, which several unrelated types also are. */
            if (std::strcmp(ti->name, "DiceUIAnchorContainerElementData") == 0)
            {
                float ax = 0.f, ay = 0.f;
                if (bf6__rime_f32(d, base + kRimeAnchorAttachOff, ax) &&
                    bf6__rime_f32(d, base + kRimeAnchorAttachOff + 4, ay))
                {
                    o.attach_x = ax;
                    o.attach_y = ay;
                    o.has_attach = 1;
                }
            }
            if (ti->kind == BF6_RIME_REPEAT_SHAPE)
            {
                /* The pip strip's own count.  It was drawn as ten evenly
                 * spread pips before this, which is a plausible-looking
                 * number that appears nowhere in the data. */
                int32_t dist = 0, count = 0;
                if (bf6__rime_i32(d, base + kRimeRepeatCountOff, count) &&
                    count >= 0 && count < 4096)
                    o.repeat_instances = count;
                if (bf6__rime_i32(d, base + kRimeRepeatDistOff, dist) &&
                    dist >= BF6_RIME_DIST_RADIAL && dist <= BF6_RIME_DIST_GRID)
                    o.repeat_distribution = dist;
            }
            if (std::strcmp(ti->name,
                            "RimeViewportStretchContainerElementData") == 0)
            {
                bf6__rime_i32(d, base + kRimeViewportFlowOff,
                               o.viewport_flow_direction);
                uint8_t edge = 0;
                if (bf6__rime_u8(d, base + kRimeViewportTopOff, edge))
                    o.viewport_extend_top = edge ? 1 : 0;
                if (bf6__rime_u8(d, base + kRimeViewportBottomOff, edge))
                    o.viewport_extend_bottom = edge ? 1 : 0;
                if (bf6__rime_u8(d, base + kRimeViewportLeftOff, edge))
                    o.viewport_extend_left = edge ? 1 : 0;
                if (bf6__rime_u8(d, base + kRimeViewportRightOff, edge))
                    o.viewport_extend_right = edge ? 1 : 0;
            }
            if (ti->kind == BF6_RIME_PROGRESS)
            {
                bf6__rime_f32(d, base + kRimeProgressValueOff, o.progress);
                bf6__rime_f32(d, base + kRimeProgressGapOff,
                               o.progress_segment_gap);
                bf6__rime_i32(d, base + kRimeProgressCountOff,
                               o.progress_segment_count);
                bf6__rime_i32(d, base + kRimeProgressOrientationOff,
                               o.progress_orientation);
            }
            if (ti->kind == BF6_RIME_BORDER)
            {
                bf6__rime_f32(d, base + kRimeBorderThicknessOff,
                               o.border_thickness);
                bf6__rime_f32(d, base + kRimeBorderStartAlphaOff,
                               o.border_start_alpha);
                bf6__rime_f32(d, base + kRimeBorderEndAlphaOff,
                               o.border_end_alpha);
                bf6__rime_i32(d, base + kRimeBorderAlignmentOff,
                               o.border_alignment);
                bf6__rime_i32(d, base + kRimeBorderGradientOff,
                               o.border_gradient_direction);
            }
            if (std::strcmp(ti->name, "DiceUIUniformGridElementData") == 0)
            {
                int64_t first = 0;
                int32_t count = 0;
                if (bf6__rime_array(d, base + kRimeGridItemTemplatesOff,
                                    first, count))
                {
                    const int32_t retained = count < 8 ? count : 8;
                    for (int32_t template_index = 0;
                         template_index < retained; ++template_index)
                    {
                        int32_t value = 0;
                        if (!bf6__rime_i32(
                                d, first + (int64_t)template_index * 8,
                                value) || !(value & 1))
                            continue;
                        std::string target;
                        if (!resolve_import(e, value >> 1, target)) continue;
                        put_str(o.item_templates[template_index],
                                sizeof(o.item_templates[template_index]),
                                target);
                        if (!o.item_template[0])
                            put_str(o.item_template,
                                    sizeof(o.item_template), target);
                    }
                    o.item_template_count = retained;
                }
            }
            if (std::strcmp(ti->name, "DiceUIDataListElementData") == 0)
            {
                int64_t first = 0;
                int32_t count = 0;
                if (bf6__rime_array(d, base + kRimeDataListItemTemplatesOff,
                                    first, count))
                {
                    const int32_t retained = count < 8 ? count : 8;
                    for (int32_t template_index = 0;
                         template_index < retained; ++template_index)
                    {
                        int32_t v = 0;
                        if (!bf6__rime_i32(d,
                                first + (int64_t)template_index * 8, v) ||
                            !(v & 1))
                            continue;
                        std::string target;
                        if (!resolve_import(e, v >> 1, target)) continue;
                        put_str(o.item_templates[template_index],
                                sizeof(o.item_templates[template_index]), target);
                        if (!o.item_template[0])
                            put_str(o.item_template, sizeof(o.item_template),
                                    target);
                    }
                    o.item_template_count = retained;
                }
            }
            if (std::strcmp(o.type_guid, kRimeDataListTypeGuid) == 0)
            {
                /* Both layouts ship in the current type universe.  The newer
                 * one grows the parent by 16 bytes, shifting all own fields;
                 * the signature selects it so an update cannot silently turn
                 * a relative pointer at +444 into SpaceDistribution. */
                const bool oldLayout =
                    o.type_signature == kRimeDataListOldSignature;
                const bool newLayout =
                    o.type_signature == kRimeDataListNewSignature;
                const uint32_t shift = newLayout ? 16u : 0u;
                if (oldLayout || newLayout)
                {
                bf6__rime_f32(d, base + 408u + shift,
                              o.item_spacing);
                bf6__rime_i32(d, base + 420u + shift,
                              o.data_list_size_distribution);
                bf6__rime_i32(d, base + 436u + shift,
                              o.data_list_flow_direction);
                bf6__rime_i32(d, base + 444u + shift,
                              o.data_list_space_distribution);
                uint8_t preserve = 0;
                if (bf6__rime_u8(d, base + 449u + shift,
                                 preserve))
                    o.data_list_preserve_fit_content = preserve ? 1 : 0;
                }
            }
            if (std::strcmp(ti->name, "DiceUIUniformGridElementData") == 0)
            {
                bf6__rime_i32(d, base + kRimeGridOrientationOff,
                              o.grid_orientation);
                bf6__rime_i32(d, base + kRimeGridStaticCountOff,
                              o.grid_static_segment_item_count);
                bf6__rime_f32(d, base + kRimeGridColumnSizeOff, o.grid_column_size);
                bf6__rime_f32(d, base + kRimeGridRowSizeOff, o.grid_row_size);
                bf6__rime_f32(d, base + kRimeGridRowSpacingOff, o.grid_row_spacing);
                bf6__rime_f32(d, base + kRimeGridColumnSpacingOff, o.grid_column_spacing);
                bf6__rime_i32(d, base + kRimeGridSegmentDistributionOff,
                              o.grid_segment_distribution);
                bf6__rime_i32(d, base + kRimeGridSegmentCountModeOff,
                              o.grid_segment_count_mode);
                bf6__rime_i32(d, base + kRimeGridColumnFlowOff,
                              o.grid_column_flow_direction);
                bf6__rime_i32(d, base + kRimeGridRowFlowOff,
                              o.grid_row_flow_direction);
                uint8_t fit = 0;
                if (bf6__rime_u8(d, base + kRimeGridItemFitContentOff, fit))
                    o.grid_item_fit_content = fit ? 1 : 0;
            }
        }

        c->rime_tree_rows.push_back(o);
        c->rime_tree_stats.gated_nodes++;
        const int row = (int)c->rime_tree_rows.size() - 1;

        if (ti->kind == BF6_RIME_WIDGET_REFERENCE)
        {
            int32_t v = 0;
            if (!bf6__rime_i32(d, base + kRimeRefBlueprintOff, v) || !(v & 1))
            { c->rime_tree_stats.unresolved_refs++; return; }
            const int imp = v >> 1;
            if (imp < 0 || (size_t)imp >= e.import_count())
            { c->rime_tree_stats.unresolved_refs++; return; }
            const std::string& pg = e.import_at((size_t)imp).partition;
            put_str(c->rime_tree_rows[(size_t)row].reference,
                    sizeof(c->rime_tree_rows[(size_t)row].reference),
                    std::string("guid:") + pg);
            /* UI widget targets sit in sibling bundles that are intentionally
             * absent from the narrow mounted-level index. The armory index is
             * still built live from the install; it is the correct scope for
             * these cross-partition references. */
            const auto& candidates = c->src.armory_partition_candidates();
            const auto hit = candidates.find(pg);
            if (hit == candidates.end() || hit->second.empty())
            { c->rime_tree_stats.unresolved_refs++; return; }
            std::string target;
            if (hit->second.size() == 1)
                target = hit->second.front();
            else
            {
                c->rime_tree_stats.duplicate_guid_refs++;
                /* The import record supplies both halves of the Frostbite
                 * identity.  PartitionGuid alone is not unique in shipped UI;
                 * match InstanceGuid against each candidate's exported roots
                 * before considering the authored widget name. */
                std::vector<std::string> identity_matches;
                std::vector<uint8_t> first_bytes;
                size_t loaded_candidates = 0;
                bool byte_identical = true;
                for (const std::string& candidate : hit->second)
                {
                    bf6::Ebx probe(*c->types);
                    std::string p = candidate;
                    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ebx") == 0)
                        p.resize(p.size() - 4);
                    if (!load(p, probe)) continue;
                    loaded_candidates++;
                    if (first_bytes.empty()) first_bytes = probe.raw();
                    else if (probe.raw() != first_bytes) byte_identical = false;
                    bool owns = false;
                    for (size_t q = 0; q < probe.exported_count(); q++)
                        if (probe.instance_guid(q) == e.import_at((size_t)imp).instance)
                        { owns = true; break; }
                    if (owns) identity_matches.push_back(candidate);
                }
                if (identity_matches.size() == 1)
                {
                    target = identity_matches.front();
                    c->rime_tree_stats.instance_disambiguated_refs++;
                }
                if (target.empty() && identity_matches.size() > 1)
                {
                    /* The runtime identity itself collides.  Resolve in stable
                     * logical-path order, with the later authored alias taking
                     * precedence.  The reverse control picks cl_frontendbase
                     * instead of pp_frontendbase and cb_interactable instead
                     * of cb_interactable_advanced; it reproduces only 413/419
                     * oracle rows with six ungated records. */
                    target = *std::max_element(identity_matches.begin(), identity_matches.end());
                    c->rime_tree_stats.identity_collision_refs++;
                }
                if (target.empty())
                {
                    const std::string hint = bf6__rime_key(c->rime_tree_rows[(size_t)row].name);
                    std::vector<std::string> name_matches;
                    for (const std::string& candidate : hit->second)
                        if (bf6__rime_key(bf6__rime_basename(candidate)) == hint)
                            name_matches.push_back(candidate);
                    if (name_matches.size() == 1)
                    {
                        target = name_matches.front();
                        c->rime_tree_stats.name_disambiguated_refs++;
                    }
                }
                if (target.empty() && loaded_candidates == hit->second.size() &&
                    loaded_candidates > 1 && byte_identical)
                {
                    /* The two names are aliases for the exact same shipped
                     * bytes (including their exported identity).  Selecting
                     * mount-first changes only the diagnostic path string. */
                    target = hit->second.front();
                    c->rime_tree_stats.equivalent_alias_refs++;
                }
                if (target.empty())
                {
                    c->rime_tree_stats.ambiguous_refs++;
                    c->rime_tree_stats.unresolved_refs++;
                    return;
                }
            }
            if (target.size() > 4 && target.compare(target.size() - 4, 4, ".ebx") == 0)
                target.resize(target.size() - 4);
            put_str(c->rime_tree_rows[(size_t)row].reference,
                    sizeof(c->rime_tree_rows[(size_t)row].reference), target);
            if (ref_depth >= max_ref_depth)
            { c->rime_tree_stats.depth_limited++; return; }
            if (std::find(open.begin(), open.end(), target) != open.end())
            { c->rime_tree_stats.cycles++; return; }

            bf6::Ebx child(*c->types);
            if (!load(target, child)) { c->rime_tree_stats.unresolved_refs++; return; }
            open.push_back(target);
            walk_layer_children(target, child, depth + 1, row, ref_depth + 1);
            open.pop_back();
            return;
        }

        if (!ti->children) return;
        const int64_t array_cell = base + (ti->kind == BF6_RIME_LAYER
                                           ? kRimeLayerElementsOff : kRimeElementsOff);
        walk_array(partition, e, array_cell, depth + 1, row, ref_depth, ti->stack);

        /* Mask geometry is not part of Elements and therefore cannot be
         * recovered from the ordinary child walk.  Retain it as an auxiliary
         * subtree, parented to the masking container for layout but tagged so
         * the paint pass never draws it as content.  Nested mask materializers
         * remain a separate renderer boundary; do not recursively promote a
         * mask's own mask list into the wrong owner. */
        if (std::strcmp(ti->name, "RimeMaskingContainerElementData") == 0 &&
            active_mask_owner < 0 && o.mask_count > 0)
        {
            const int previous_owner = active_mask_owner;
            active_mask_owner = row;
            walk_array(partition, e, base + kRimeMasksOff,
                       depth + 1, row, ref_depth, false);
            active_mask_owner = previous_owner;
        }
    }

    void walk_array(const std::string& partition, bf6::Ebx& e, int64_t cell,
                    int depth, int parent, int ref_depth, bool reverse)
    {
        const std::vector<uint8_t>& d = e.raw();
        int64_t first = 0;
        int32_t count = 0;
        if (!bf6__rime_array(d, cell, first, count)) return;
        for (int32_t q = 0; q < count; q++)
        {
            const int32_t k = reverse ? count - 1 - q : q;
            const int child = bf6__rime_internal_ptr(e, d, first + (int64_t)k * 8);
            if (child >= 0) walk_node(partition, e, (size_t)child, depth, parent, ref_depth);
        }
    }

    bool walk_layer_children(const std::string& partition, bf6::Ebx& e,
                             int depth, int parent, int ref_depth)
    {
        /* References expand the same authored multi-layer blueprint shape as
         * a root build. Do not stop at an earlier empty layer. */
        bool found_layer = false;
        for (size_t i = 0; i < e.instance_count(); i++)
        {
            const bf6__rime_type* ti = bf6__rime_type_of(e, i);
            if (!ti || ti->kind != BF6_RIME_LAYER) continue;
            const int64_t base = e.payload() + (int64_t)e.instance_offset(i);
            walk_array(partition, e, base + kRimeLayerElementsOff,
                       depth, parent, ref_depth, false);
            found_layer = true;
        }
        return found_layer;
    }

    bool build(const std::string& root)
    {
        bf6::Ebx e(*c->types);
        if (!load(root, e)) return false;
        open.push_back(root);
        bool found_layer = false;
        for (size_t i = 0; i < e.instance_count(); i++)
        {
            const bf6__rime_type* ti = bf6__rime_type_of(e, i);
            if (!ti || ti->kind != BF6_RIME_LAYER) continue;
            /* A widget blueprint may author more than one Layer entity.  The
             * metacustomization info view in the current install has an empty
             * "New Layer" followed by the populated "Default" layer.  The old
             * first-match return silently discarded that second layer and all
             * of its elements.  Walk every shipped layer in instance order;
             * empty layers contribute only their own row, while populated
             * layers retain their exact authored child order. */
            walk_node(root, e, i, 0, -1, 0);
            found_layer = true;
        }
        open.pop_back();
        return found_layer;
    }
};

}  // namespace

int bf6_rime_elements(bf6_ctx* c, const char* partition,
                      bf6_rime_element* out, int out_max)
{
    if (!c || !partition) return -1;
    std::string terr;
    if (!ensure_types(c, terr)) return -1;

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(partition) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(partition, err);
    if (raw.empty()) return -1;

    bf6::Ebx e(*c->types);
    if (!e.parse(std::move(raw), err)) return -1;
    const std::vector<uint8_t>& d = e.raw();

    c->rime_names.clear();
    c->rime_names.reserve(e.instance_count());
    int n = 0;
    for (size_t i = 0; i < e.instance_count(); i++)
    {
        const bf6__rime_type* ti = bf6__rime_type_of(e, i);
        if (!ti || ti->kind == BF6_RIME_LAYER) continue;
        const int64_t base = e.payload() + (int64_t)e.instance_offset(i);
        const bool is_ref = ti->kind == BF6_RIME_WIDGET_REFERENCE;
        const std::string nm = bf6__rime_cstr(d, base + (is_ref ? kRimeRefNameOff : kRimeNameOff));
        if (nm.empty()) continue; /* legacy API promised named elements only */

        c->rime_names.push_back(nm);
        if (out && n < out_max)
        {
            bf6_rime_element& o = out[n];
            o = bf6_rime_element{};
            o.is_widget_ref = is_ref ? 1 : 0;
            if (is_ref)
            {
                bf6__rime_axis(d, base + kRimeRefHOff, o.h);
                bf6__rime_axis(d, base + kRimeRefVOff, o.v);
                bf6__rime_u32(d, base + kRimeRefNameHashOff, o.name_hash);
                bf6__rime_f32(d, base + kRimeRefWidthOff, o.width);
                bf6__rime_f32(d, base + kRimeRefHeightOff, o.height);
                bf6__rime_f32(d, base + kRimeRefAlphaOff, o.alpha);
                uint8_t b = 0;
                if (bf6__rime_u8(d, base + kRimeRefVisibleOff, b)) o.visible = b ? 1 : 0;
                if (bf6__rime_u8(d, base + kRimeRefFitWOff, b)) o.fit_w = b ? 1 : 0;
                if (bf6__rime_u8(d, base + kRimeRefFitHOff, b)) o.fit_h = b ? 1 : 0;
            }
            else
            {
                bf6__rime_axis(d, base + kRimeVLayoutOff, o.v);
                bf6__rime_axis(d, base + kRimeHLayoutOff, o.h);
                bf6__rime_u32(d, base + kRimeNameHashOff, o.name_hash);
                bf6__rime_f32(d, base + kRimeWidthOff, o.width);
                bf6__rime_f32(d, base + kRimeHeightOff, o.height);
                bf6__rime_f32(d, base + kRimeAlphaOff, o.alpha);
                uint8_t b = 0;
                if (bf6__rime_u8(d, base + kRimeVisibleOff, b)) o.visible = b ? 1 : 0;
                if (bf6__rime_u8(d, base + kRimeFitWOff, b)) o.fit_w = b ? 1 : 0;
                if (bf6__rime_u8(d, base + kRimeFitHOff, b)) o.fit_h = b ? 1 : 0;
            }
            o.instance = (int32_t)i;
        }
        n++;
    }
    if (out)
    {
        const int lim = n < out_max ? n : out_max;
        for (int i = 0; i < lim; i++) out[i].name = c->rime_names[(size_t)i].c_str();
    }
    return n;
}

int bf6_rime_tree(bf6_ctx* c, const char* root_partition,
                  int max_ref_depth, bf6_rime_node* out, int out_max,
                  bf6_rime_tree_stats* stats)
{
    if (!c || !root_partition || !*root_partition) return -1;
    std::string terr;
    if (!ensure_types(c, terr)) return -1;
    if (max_ref_depth <= 0) max_ref_depth = 6;

    const std::string key = std::string(root_partition) + "#" + std::to_string(max_ref_depth);
    if (c->rime_tree_key != key)
    {
        c->rime_tree_rows.clear();
        c->rime_tree_stats = bf6_rime_tree_stats{};
        bf6__rime_walk w{ c, max_ref_depth, {} };
        if (!w.build(root_partition)) return -1;

        /* COLOUR INHERITANCE.  Not an optimisation - a correctness step.
         *
         * Only a minority of elements name a ColorId. Many of the rest are
         * white-with-no-id sitting under an ancestor that does name one, and
         * painting those literally white is wrong: every IconFill_Add in the
         * input-icon widget, for instance, is FE-Neutral #BFCAD1 from its
         * parent. Rows are appended parent-before-child by the walk, so one
         * forward pass suffices and no recursion is needed. */
        std::vector<bf6_rime_node>& rows = c->rime_tree_rows;
        for (size_t i = 0; i < rows.size(); i++)
        {
            if (rows[i].color_source != BF6_RIME_COLOR_NONE) continue;
            const int p = rows[i].parent;
            if (p < 0 || (size_t)p >= i) continue;
            if (rows[(size_t)p].color_source == BF6_RIME_COLOR_NONE) continue;
            rows[i].color_rgb = rows[(size_t)p].color_rgb;
            put_str(rows[i].color_name, sizeof(rows[i].color_name),
                    rows[(size_t)p].color_name);
            rows[i].color_source = BF6_RIME_COLOR_INHERITED;
        }

        c->rime_tree_stats.nodes = (int32_t)c->rime_tree_rows.size();
        c->rime_tree_key = key;
    }

    if (stats) *stats = c->rime_tree_stats;
    const int n = (int)c->rime_tree_rows.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; i++) out[i] = c->rime_tree_rows[(size_t)i];
    return n;
}

int bf6_rime_palette(bf6_ctx* c, bf6_rime_color* out, int out_max)
{
    if (!c) return -1;
    std::string terr;
    if (!ensure_types(c, terr)) return -1;
    if (!bf6__rime_palette_load(c)) return -1;

    const int n = (int)c->rime_palette.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; i++) out[i] = c->rime_palette[(size_t)i];
    return n;
}

int bf6_rime_font_styles(bf6_ctx* c, bf6_rime_font_style* out, int out_max)
{
    if (!c) return -1;
    std::string terr;
    if (!ensure_types(c, terr)) return -1;
    if (!bf6__rime_font_styles_load(c)) return -1;

    const int n = (int)c->rime_font_styles.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; i++)
            out[i] = c->rime_font_styles[(size_t)i];
    return n;
}

int bf6_rime_font_data(bf6_ctx* c, const char* font_asset,
                       unsigned char* out, int out_max)
{
    if (!c || !font_asset || !*font_asset) return -1;
    std::string terr;
    if (!ensure_types(c, terr)) return -1;

    /* Held on the context so the size call and the fill call read the archive
     * once between them, which is the pattern every other sized ABI here uses. */
    const std::string want = bf6__rime_key_path(font_asset);
    if (c->rime_font_blob_name != want)
    {
        std::string err;
        c->rime_font_blob = c->src.get_res(want, err);
        if (c->rime_font_blob.empty()) return -1;
        c->rime_font_blob_name = want;
    }

    const int n = (int)c->rime_font_blob.size();
    if (out && out_max > 0)
    {
        const int lim = n < out_max ? n : out_max;
        std::memcpy(out, c->rime_font_blob.data(), (size_t)lim);
    }
    return n;
}

static bool bf6__rime_markup_svgs_load(bf6_ctx* c)
{
    if (!c) return false;
    if (c->rime_markup_svgs_tried) return !c->rime_markup_svgs.empty();
    c->rime_markup_svgs_tried = true;
    std::string terr;
    if (!ensure_types(c, terr)) return false;

    /* PcTextMarkupConfig's Resources array is heterogeneous. Follow every
     * authored import and accept only the one concrete RimeSvgListAsset; its
     * path spelling is not used as a selector. */
    static const char* kConfig = "common/ui/gamesetup/pctextmarkupconfig";
    static const uint32_t kResources = 0x2AD9AC08u;
    static const uint32_t kEntries = 0xED94E8D7u;
    static const uint32_t kName = 0x0C59FA06u;
    static const uint32_t kImage = 0xD70E99D8u;
    static const char* kSvgListType =
        "c2684b98-30d3-97c9-45bb-e67a910809c1";

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(kConfig) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(kConfig, err);
    if (raw.empty()) return false;
    bf6::Ebx config(*c->types);
    config.set_guid_index(&c->src.armory_partition_index());
    if (!config.parse(std::move(raw), err) || config.exported_count() != 1)
        return false;
    const bf6::EbxValue root = config.read_instance(0);
    const bf6::EbxValue* resources = root.field(kResources);
    if (!resources || resources->kind != bf6::EbxValue::Kind::Array)
        return false;

    std::string list_path;
    for (const bf6::EbxValue& resource : resources->items)
    {
        if (resource.kind != bf6::EbxValue::Kind::ImportRef ||
            resource.import_path.empty())
            continue;
        std::string imported = bf6__rime_key_path(resource.import_path);
        if (imported.size() > 4 &&
            imported.compare(imported.size() - 4, 4, ".ebx") == 0)
            imported.resize(imported.size() - 4);
        std::vector<uint8_t> list_raw =
            c->src.get_ebx(imported + ".ebx", err);
        if (list_raw.empty()) list_raw = c->src.get_ebx(imported, err);
        if (list_raw.empty()) continue;
        bf6::Ebx candidate(*c->types);
        if (!candidate.parse(std::move(list_raw), err) ||
            candidate.exported_count() != 1)
            continue;
        const std::string candidate_type =
            bf6::TypeDb::guid_str(candidate.instance_type(0));
        if (candidate_type != kSvgListType) continue;
        if (!list_path.empty() && list_path != imported)
            return false;
        list_path = std::move(imported);
    }
    if (list_path.empty()) return false;

    std::vector<uint8_t> list_raw = c->src.get_ebx(list_path + ".ebx", err);
    if (list_raw.empty()) list_raw = c->src.get_ebx(list_path, err);
    if (list_raw.empty()) return false;
    bf6::Ebx list(*c->types);
    list.set_guid_index(&c->src.armory_partition_index());
    if (!list.parse(std::move(list_raw), err) || list.exported_count() != 1 ||
        bf6::TypeDb::guid_str(list.instance_type(0)) != kSvgListType)
        return false;
    const bf6::EbxValue list_root = list.read_instance(0);
    const bf6::EbxValue* entries = list_root.field(kEntries);
    if (!entries || entries->kind != bf6::EbxValue::Kind::Array)
        return false;

    std::set<std::string> names;
    for (const bf6::EbxValue& entry : entries->items)
    {
        if (entry.kind != bf6::EbxValue::Kind::Struct) continue;
        const bf6::EbxValue* name = entry.field(kName);
        const bf6::EbxValue* image = entry.field(kImage);
        if (!name || name->kind != bf6::EbxValue::Kind::Str || name->s.empty() ||
            !image || image->kind != bf6::EbxValue::Kind::ImportRef ||
            image->import_path.empty() || !names.insert(name->s).second)
            return false;
        bf6_rime_markup_svg row{};
        put_str(row.name, sizeof(row.name), name->s);
        std::string image_path = bf6__rime_key_path(image->import_path);
        if (image_path.size() > 4 &&
            image_path.compare(image_path.size() - 4, 4, ".ebx") == 0)
            image_path.resize(image_path.size() - 4);
        put_str(row.image_asset, sizeof(row.image_asset), image_path);
        c->rime_markup_svgs.push_back(row);
    }
    return !c->rime_markup_svgs.empty();
}

int bf6_rime_markup_svgs(bf6_ctx* c, bf6_rime_markup_svg* out, int out_max)
{
    if (!c || out_max < 0 || !bf6__rime_markup_svgs_load(c)) return -1;
    const int count = (int)c->rime_markup_svgs.size();
    if (out && out_max > 0)
        for (int i = 0; i < count && i < out_max; ++i)
            out[i] = c->rime_markup_svgs[(size_t)i];
    return count;
}

static bool bf6__rime_image_resource(bf6_ctx* c, const char* image_asset,
                                     std::string& resolved,
                                     std::string* type_guid)
{
    resolved.clear();
    if (type_guid) type_guid->clear();
    if (!c || !image_asset || !*image_asset) return false;
    std::string terr;
    if (!ensure_types(c, terr)) return false;

    const std::string asset = bf6__rime_key_path(image_asset);
    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(asset + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(asset, err);
    if (raw.empty()) return false;

    bf6::Ebx e(*c->types);
    if (!e.parse(std::move(raw), err) || e.exported_count() == 0) return false;
    if (type_guid) *type_guid = bf6::TypeDb::guid_str(e.instance_type(0));

    static const uint32_t kResource = 0xAA4FA860u; /* fieldname_dict: Resource */
    const std::vector<uint32_t> want{ kResource };
    const bf6::EbxValue root = e.read_instance(0, &want);
    const bf6::EbxValue* resource = root.field(kResource);
    if (!resource || resource->kind != bf6::EbxValue::Kind::ResRef || !resource->u)
        return false;

    /* ResourceId is the authored join.  Asset name resemblance is not used:
     * the RES may have a generated or unrelated path while its id is exact. */
    for (const auto& kv : c->src.res())
        if (kv.second.rid == resource->u)
        {
            if (!resolved.empty() && resolved != kv.first) return false; /* ambiguous */
            resolved = kv.first;
        }
    return !resolved.empty();
}

int bf6_rime_image_resource(bf6_ctx* c, const char* image_asset,
                            char* out, int out_len)
{
    std::string resolved;
    if (!bf6__rime_image_resource(c, image_asset, resolved, nullptr)) return -1;
    if (out && out_len > 0) std::snprintf(out, (size_t)out_len, "%s", resolved.c_str());
    return (int)resolved.size();
}

int bf6_rime_texture_id(bf6_ctx* c, const char* image_asset)
{
    std::string resolved, type;
    if (!bf6__rime_image_resource(c, image_asset, resolved, &type)) return -1;

    /* Both concrete asset classes below name native Texture resources.  The
     * first is TextureAsset; the second is the sized/canvas image asset used
     * by legacy decals. A vector asset has the same Resource field but a
     * different concrete type and must go through the vector decoder. */
    const bool texture = type == "eec133de-42e8-cfb5-ce15-c428de4d2f08" ||
                         type == "4e942709-08c6-46ed-f1a3-24aed42eba70";
    return texture ? bf6_texture_id_by_name(c, resolved.c_str()) : -1;
}

int bf6_rime_flipbook(bf6_ctx* c, const char* image_asset,
                      bf6_rime_flipbook_info* info,
                      bf6_rime_flipbook_frame* frames, int frame_max)
{
    if (info) *info = bf6_rime_flipbook_info{};
    if (!c || !image_asset || !*image_asset || frame_max < 0) return -1;
    std::string terr;
    if (!ensure_types(c, terr)) return -1;

    const std::string asset = bf6__rime_key_path(image_asset);
    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(asset + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(asset, err);
    if (raw.empty()) return -1;

    bf6::Ebx e(*c->types);
    // Front-end flipbooks and their atlas assets live inside the bounded
    // armory/UI family index, which the viewer has already warmed. Asking for
    // the global index here scanned every EBX partition in the game the first
    // time a tiny UI spinner appeared (8-12 seconds on the current install).
    // Retain the public API's generality: retry against the global index only
    // when an asset outside those proven families cannot be resolved.
    e.set_guid_index(&c->src.armory_partition_index());
    if (!e.parse(std::move(raw), err) || e.exported_count() != 1) return -2;
    if (bf6::TypeDb::guid_str(e.instance_type(0)) !=
        "be894924-7f31-3b98-3f09-090ee535f5ce")
        return -1;

    /* This asset also ships the older Rime schema. Its executable-reflected
     * names are different from the hashes written into retail (a schema read
     * reports the atlas as Null), while its concrete old-schema offsets are
     * stable: FrameUvs +0x20, TextureAtlas +0x28, FrameRate +0x30. Read those
     * fixed cells only after the exact concrete-type gate above. */
    const std::vector<uint8_t>& data = e.raw();
    const int64_t base = e.payload() + (int64_t)e.instance_offset(0);
    int32_t relative = 0, frame_count = 0;
    if (!bf6__rime_i32(data, base + 0x20, relative) || relative == 0)
        return -2;
    const int64_t first = base + 0x20 + (int64_t)relative;
    if (!bf6__rime_i32(data, first - 4, frame_count) ||
        frame_count <= 0 || frame_count > 4096 || first < 0 ||
        first + (int64_t)frame_count * 16 > (int64_t)data.size())
        return -2;
    std::string atlas_guid, atlas_path;
    if (!e.import_ref_at(base + 0x28, atlas_guid, atlas_path) ||
        atlas_path.empty() || atlas_path == "<not indexed>")
    {
        e.set_guid_index(&c->src.partition_index());
        atlas_guid.clear();
        atlas_path.clear();
        if (!e.import_ref_at(base + 0x28, atlas_guid, atlas_path) ||
            atlas_path.empty() || atlas_path == "<not indexed>")
            return -2;
    }
    float frame_rate = 0.f;
    if (!bf6__rime_f32(data, base + 0x30, frame_rate) ||
        !std::isfinite(frame_rate) || !(frame_rate > 0.f) ||
        frame_rate > 1000.f)
        return -2;

    std::string atlas_asset = bf6__rime_key_path(atlas_path);
    if (atlas_asset.size() > 4 &&
        atlas_asset.compare(atlas_asset.size() - 4, 4, ".ebx") == 0)
        atlas_asset.resize(atlas_asset.size() - 4);
    const int texture_id = bf6_rime_texture_id(c, atlas_asset.c_str());
    if (texture_id < 0) return -2;

    std::vector<bf6_rime_flipbook_frame> decoded;
    decoded.reserve((size_t)frame_count);
    for (int frame_index = 0; frame_index < frame_count; ++frame_index)
    {
        bf6_rime_flipbook_frame frame{};
        for (int component = 0; component < 4; ++component)
        {
            if (!bf6__rime_f32(data,
                first + (int64_t)frame_index * 16 + component * 4,
                frame.uv[component]) || !std::isfinite(frame.uv[component]))
                return -2;
        }
        if (frame.uv[0] < 0.f || frame.uv[1] < 0.f ||
            frame.uv[2] > 1.f || frame.uv[3] > 1.f ||
            !(frame.uv[2] > frame.uv[0]) || !(frame.uv[3] > frame.uv[1]))
            return -2;
        decoded.push_back(frame);
    }

    bf6_rime_flipbook_info result{};
    put_str(result.atlas_asset, sizeof(result.atlas_asset), atlas_asset);
    result.frame_rate = frame_rate;
    result.frame_count = (int32_t)decoded.size();
    result.texture_id = texture_id;
    if (frames && frame_max > 0)
        for (int i = 0; i < frame_max && i < result.frame_count; ++i)
            frames[i] = decoded[(size_t)i];
    if (info) *info = result;
    return result.frame_count;
}

/* SvgImageData paint records have three measured sizes and can mix them from
 * one shape to the next.  The selector inside the paint record is not decoded
 * yet, so navigation is constrained by the independently verified cubic
 * contour grammar and exact end-of-resource closure.  Reject an ambiguous
 * walk instead of choosing the first plausible stride. */
static void bf6__rime_svg_walk(
    const std::vector<uint8_t>& d, uint32_t shape_count, uint32_t shape,
    int64_t at, std::vector<bf6_rime_svg_contour>& working_contours,
    std::vector<bf6_rime_svg_point>& working_points,
    int& solutions, std::vector<bf6_rime_svg_contour>& solved_contours,
    std::vector<bf6_rime_svg_point>& solved_points)
{
    if (solutions > 1) return;
    if (shape == shape_count)
    {
        if (at == (int64_t)d.size())
        {
            ++solutions;
            if (solutions == 1)
            {
                solved_contours = working_contours;
                solved_points = working_points;
            }
        }
        return;
    }

    static const int kPaintSizes[3] = { 50, 54, 58 };
    for (int paint_size : kPaintSizes)
    {
        int64_t cursor = at + paint_size;
        if (cursor + 1 + 16 + 4 > (int64_t)d.size()) continue;

        uint8_t shape_tag = 0;
        float shape_bounds[4]{};
        uint32_t contour_count = 0;
        if (!bf6__rime_u8(d, cursor++, shape_tag) || shape_tag > 1) continue;
        bool valid = true;
        for (int k = 0; k < 4; ++k)
            if (!bf6__rime_f32(d, cursor + k * 4, shape_bounds[k])) valid = false;
        cursor += 16;
        if (!valid || !bf6__rime_u32(d, cursor, contour_count) ||
            contour_count > 65535) continue;
        cursor += 4;

        const size_t contour_base = working_contours.size();
        const size_t point_base = working_points.size();
        for (uint32_t ci = 0; valid && ci < contour_count; ++ci)
        {
            uint32_t point_count = 0;
            if (!bf6__rime_u32(d, cursor, point_count) || point_count < 4 ||
                point_count > 1000000 || ((point_count - 1) % 3) != 0)
            {
                valid = false;
                break;
            }
            cursor += 4;
            if (cursor + (int64_t)point_count * 8 + 1 + 16 > (int64_t)d.size() ||
                working_points.size() > (size_t)0x7fffffff - point_count)
            {
                valid = false;
                break;
            }

            bf6_rime_svg_contour co{};
            co.shape = (int32_t)shape;
            co.point_first = (int32_t)working_points.size();
            co.point_count = (int32_t)point_count;
            for (uint32_t pi = 0; pi < point_count; ++pi)
            {
                bf6_rime_svg_point p{};
                if (!bf6__rime_f32(d, cursor + (int64_t)pi * 8, p.x) ||
                    !bf6__rime_f32(d, cursor + (int64_t)pi * 8 + 4, p.y))
                {
                    valid = false;
                    break;
                }
                working_points.push_back(p);
            }
            if (!valid) break;
            cursor += (int64_t)point_count * 8;

            uint8_t flag = 0;
            if (!bf6__rime_u8(d, cursor++, flag) || flag > 1)
            {
                valid = false;
                break;
            }
            co.flag = (int32_t)flag;
            for (int k = 0; k < 4; ++k)
                if (!bf6__rime_f32(d, cursor + k * 4, co.bounds[k])) valid = false;
            cursor += 16;
            if (valid) working_contours.push_back(co);
        }

        if (valid)
            bf6__rime_svg_walk(d, shape_count, shape + 1, cursor,
                                working_contours, working_points, solutions,
                                solved_contours, solved_points);
        working_contours.resize(contour_base);
        working_points.resize(point_base);
    }
}

int bf6_rime_svg(bf6_ctx* c, const char* image_asset,
                 bf6_rime_svg_info* info,
                 bf6_rime_svg_contour* contours, int contour_max,
                 bf6_rime_svg_point* points, int point_max)
{
    if (info) *info = bf6_rime_svg_info{};
    std::string resolved, type;
    if (!bf6__rime_image_resource(c, image_asset, resolved, &type)) return -1;

    /* Exact concrete SvgImageAsset gate.  Texture assets carry the same
     * Resource field and accepting one here would parse arbitrary texture
     * headers as contour counts. */
    if (type != "ddedd2f1-d108-633f-52cb-1d35c5078c3b") return -1;

    std::string err;
    const std::vector<uint8_t> d = c->src.get_res(resolved, err);
    if (d.size() < 12) return -1;

    bf6_rime_svg_info si{};
    uint32_t shape_count = 0;
    if (!bf6__rime_f32(d, 0, si.canvas[0]) ||
        !bf6__rime_f32(d, 4, si.canvas[1]) ||
        !bf6__rime_u32(d, 8, shape_count) ||
        !(si.canvas[0] > 0.f) || !(si.canvas[1] > 0.f) ||
        shape_count == 0 || shape_count > 4096)
        return -2;
    si.shape_count = (int32_t)shape_count;

    std::vector<bf6_rime_svg_contour> working_contours, solved_contours;
    std::vector<bf6_rime_svg_point> working_points, solved_points;
    int solutions = 0;
    bf6__rime_svg_walk(d, shape_count, 0, 12, working_contours, working_points,
                       solutions, solved_contours, solved_points);
    if (solutions != 1) return -2;

    si.contour_count = (int32_t)solved_contours.size();
    si.point_count = (int32_t)solved_points.size();
    if (contours && contour_max > 0)
        for (int i = 0; i < contour_max && i < si.contour_count; ++i)
            contours[i] = solved_contours[(size_t)i];
    if (points && point_max > 0)
        for (int i = 0; i < point_max && i < si.point_count; ++i)
            points[i] = solved_points[(size_t)i];
    if (info) *info = si;
    return si.contour_count;
}

int bf6_ui_hub_entries(bf6_ctx* c, const char* hub_asset,
                       bf6_ui_hub_entry* out, int out_max)
{
    if (!c || !hub_asset || !*hub_asset) return -1;
    std::string terr;
    if (!ensure_types(c, terr)) return -1;

    std::string path = bf6__rime_key_path(hub_asset);
    /* Blueprint ImportRefs retain their .ebx suffix.  get_ebx accepts either
     * spelling, but appending another suffix first turns a recursive hub walk
     * into `foo.ebx.ebx` and the fallback then depends on a mount alias.  Keep
     * one canonical spelling, as the Rime widget expander already does. */
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".ebx") == 0)
        path.resize(path.size() - 4);
    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(path + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(path, err);
    if (raw.empty()) return -1;

    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), err) || e.exported_count() == 0) return -1;

    /* These two hashes are the long-established Frostbite ObjectBlueprint
     * fields (also used by the level walker): Objects and Blueprint. */
    static const uint32_t kObjects   = 0x5A6115B2u;
    static const uint32_t kBlueprint = 0x6B831B88u;
    const std::vector<uint32_t> root_want{ kObjects };
    const bf6::EbxValue root = e.read_instance(0, &root_want);
    const bf6::EbxValue* objects = root.field(kObjects);
    if (!objects || objects->kind != bf6::EbxValue::Kind::Array) return -1;

    int total = 0;
    const std::vector<uint32_t> child_want{ kBlueprint };
    for (const bf6::EbxValue& ref : objects->items)
    {
        if (ref.kind != bf6::EbxValue::Kind::InstanceRef || ref.instance < 0 ||
            (size_t)ref.instance >= e.instance_count())
            continue;
        const bf6::EbxValue child = e.read_instance((size_t)ref.instance, &child_want);
        const bf6::EbxValue* bp = child.field(kBlueprint);
        if (!bp || bp->kind != bf6::EbxValue::Kind::ImportRef ||
            bp->import_path.empty())
            continue;
        if (out && total < out_max)
        {
            bf6_ui_hub_entry row{};
            row.instance = ref.instance;
            std::snprintf(row.blueprint, sizeof(row.blueprint), "%s",
                          bp->import_path.c_str());
            out[total] = row;
        }
        ++total;
    }
    return total;
}

namespace {

uint64_t bf6__photon_u64(const bf6::EbxValue* value, bool& ok)
{
    ok = false;
    if (!value) return 0;
    if (value->kind == bf6::EbxValue::Kind::Uint ||
        value->kind == bf6::EbxValue::Kind::ResRef)
    { ok = true; return value->u; }
    if (value->kind == bf6::EbxValue::Kind::Int && value->i >= 0)
    { ok = true; return (uint64_t)value->i; }
    return 0;
}

bool bf6__photon_suffix(const char* name, const char* suffix)
{
    if (!name || !suffix) return false;
    const size_t n = std::strlen(name), s = std::strlen(suffix);
    if (n < s) return false;
    for (size_t i = 0; i < s; ++i)
        if (std::tolower((unsigned char)name[n - s + i]) !=
            std::tolower((unsigned char)suffix[i])) return false;
    return true;
}

int bf6__photon_kind(const char* name)
{
    if (bf6__photon_suffix(name, ".png")) return BF6_PHOTON_ASSET_PNG;
    if (bf6__photon_suffix(name, ".svg")) return BF6_PHOTON_ASSET_SVG;
    if (bf6__photon_suffix(name, ".ttf") ||
        bf6__photon_suffix(name, ".ttc") ||
        bf6__photon_suffix(name, ".otf") ||
        bf6__photon_suffix(name, ".woff") ||
        bf6__photon_suffix(name, ".woff2")) return BF6_PHOTON_ASSET_FONT;
    return BF6_PHOTON_ASSET_UNKNOWN;
}

bool bf6__photon_magic(const bf6_photon_asset& row,
                       const std::vector<uint8_t>& chunk)
{
    if ((uint64_t)row.offset + row.size > chunk.size()) return false;
    const uint8_t* p = chunk.data() + row.offset;
    const size_t n = row.size;
    if (row.kind == BF6_PHOTON_ASSET_PNG)
    {
        static const uint8_t kPng[] = { 0x89, 'P', 'N', 'G', 13, 10, 0x1a, 10 };
        return n >= sizeof(kPng) && std::memcmp(p, kPng, sizeof(kPng)) == 0;
    }
    if (row.kind == BF6_PHOTON_ASSET_SVG)
    {
        size_t i = 0;
        while (i < n && std::isspace((unsigned char)p[i])) ++i;
        if (i + 4 <= n && std::memcmp(p + i, "<svg", 4) == 0) return true;
        const size_t cap = (std::min)(n, i + (size_t)512);
        if (i + 5 <= n && std::memcmp(p + i, "<?xml", 5) == 0)
            for (size_t j = i + 5; j + 4 <= cap; ++j)
                if (std::memcmp(p + j, "<svg", 4) == 0) return true;
        return false;
    }
    if (row.kind == BF6_PHOTON_ASSET_FONT)
    {
        if (n < 4) return false;
        return (p[0] == 0 && p[1] == 1 && p[2] == 0 && p[3] == 0) ||
               std::memcmp(p, "OTTO", 4) == 0 ||
               std::memcmp(p, "ttcf", 4) == 0 ||
               std::memcmp(p, "wOFF", 4) == 0 ||
               std::memcmp(p, "wOF2", 4) == 0;
    }
    return true;
}

bool bf6__load_photon_assets(bf6_ctx* c)
{
    if (c->photon_assets_tried) return !c->photon_assets.empty();
    c->photon_assets_tried = true;
    c->photon_assets.clear();
    c->photon_bundle_info = bf6_photon_bundle_info{};

    std::string terr;
    if (!ensure_types(c, terr)) return false;
    static const char* kBundle = "systems/twinkle/offline/photonbundle";
    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(kBundle) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(kBundle, err);
    if (raw.empty()) return false;

    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), err) || e.exported_count() != 1) return false;

    static const uint32_t kAssets = 0x1A054761u;
    static const uint32_t kChunkGuid = 0x5FC3D8E5u;
    static const uint32_t kChunkSize = 0xDC11B5CBu;
    static const uint32_t kAssetName = 0x5B6DC343u;
    static const uint32_t kAssetOffset = 0x53583A96u;
    static const uint32_t kSize = 0xC998527Du;
    const std::vector<uint32_t> want{ kAssets, kChunkGuid, kChunkSize };
    const bf6::EbxValue root = e.read_instance(0, &want);
    const bf6::EbxValue* guid = root.field(kChunkGuid);
    const bf6::EbxValue* declaredSize = root.field(kChunkSize);
    const bf6::EbxValue* assets = root.field(kAssets);
    bool sizeOk = false;
    const uint64_t manifestSize = bf6__photon_u64(declaredSize, sizeOk);
    if (!guid || guid->kind != bf6::EbxValue::Kind::Guid || !sizeOk ||
        !assets || assets->kind != bf6::EbxValue::Kind::Array) return false;

    const std::string chunkGuid = bf6__resolve_guid(c, guid->guid);
    if (chunkGuid.empty()) return false;
    const std::vector<uint8_t> chunk = c->src.get_chunk(chunkGuid, err);
    if (chunk.empty() || manifestSize != chunk.size()) return false;

    c->photon_assets.reserve(assets->items.size());
    uint64_t expected = 0;
    for (const bf6::EbxValue& item : assets->items)
    {
        if (item.kind != bf6::EbxValue::Kind::Struct) return false;
        const bf6::EbxValue* nv = item.field(kAssetName);
        bool offsetOk = false, bytesOk = false;
        const uint64_t offset = bf6__photon_u64(item.field(kAssetOffset), offsetOk);
        const uint64_t bytes = bf6__photon_u64(item.field(kSize), bytesOk);
        if (!nv || nv->kind != bf6::EbxValue::Kind::Str || nv->s.empty() ||
            !offsetOk || !bytesOk || offset > UINT32_MAX || bytes > UINT32_MAX)
            return false;
        bf6_photon_asset row{};
        std::snprintf(row.name, sizeof(row.name), "%s", nv->s.c_str());
        row.offset = (uint32_t)offset;
        row.size = (uint32_t)bytes;
        row.kind = bf6__photon_kind(row.name);
        if (offset <= chunk.size() && bytes <= chunk.size() - offset)
            ++c->photon_bundle_info.ranges_in_bounds;
        if (offset == expected) ++c->photon_bundle_info.ranges_contiguous;
        if (bf6__photon_magic(row, chunk)) ++c->photon_bundle_info.signatures_valid;
        expected = offset + bytes;
        c->photon_assets.push_back(row);
    }
    if (expected != chunk.size()) return false;
    std::snprintf(c->photon_bundle_info.chunk_guid,
                  sizeof(c->photon_bundle_info.chunk_guid), "%s", chunkGuid.c_str());
    c->photon_bundle_info.chunk_size = (int64_t)chunk.size();
    c->photon_bundle_info.asset_count = (int32_t)c->photon_assets.size();
    return !c->photon_assets.empty();
}

} // namespace

int bf6_photon_offline_assets(bf6_ctx* c, bf6_photon_bundle_info* info,
                              bf6_photon_asset* out, int out_max)
{
    if (!c || !bf6__load_photon_assets(c)) return -1;
    if (info) *info = c->photon_bundle_info;
    const int total = (int)c->photon_assets.size();
    if (out && out_max > 0)
        std::copy_n(c->photon_assets.data(), (std::min)(total, out_max), out);
    return total;
}

namespace {

/* A typed array whose ELEMENTS are plain values rather than PointerRefs.
 * bf6__rime_array assumes an 8-byte pointer stride, which is right for the
 * Elements/Shapes/Corners arrays and wrong for a Vec2 or u16 list. */
bool bf6__rime_value_array(const std::vector<uint8_t>& d, int64_t cell,
                           size_t stride, int64_t& first, int32_t& count)
{
    int32_t rel = 0;
    if (!bf6__rime_i32(d, cell, rel) || rel == 0) return false;
    first = cell + (int64_t)rel;
    if (!bf6__rime_i32(d, first - 4, count) || count < 0 || count > 65535)
        return false;
    return first >= 0 &&
           first + (int64_t)count * (int64_t)stride <= (int64_t)d.size();
}

}  // namespace

int bf6_rime_shape(bf6_ctx* c, const char* partition, int instance,
                   bf6_rime_shape_info* info,
                   bf6_rime_shape_vertex* verts, int vmax,
                   unsigned short* indices, int imax,
                   bf6_rime_shape_corner* corners, int cmax)
{
    if (!c || !partition || instance < 0) return -1;
    std::string terr;
    if (!ensure_types(c, terr)) return -1;

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(partition) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(partition, err);
    if (raw.empty()) return -1;

    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.armory_partition_index());
    if (!e.parse(std::move(raw), err)) return -1;
    if ((size_t)instance >= e.instance_count()) return -1;

    const bf6__rime_type* ti = bf6__rime_type_of(e, (size_t)instance);
    if (!ti) return -1;
    uint32_t shapeOff;
    if      (ti->kind == BF6_RIME_VECTOR_SHAPE) shapeOff = kRimeVecShapeOff;
    else if (ti->kind == BF6_RIME_REPEAT_SHAPE) shapeOff = kRimeRepeatShapeOff;
    else return -1;

    const std::vector<uint8_t>& d = e.raw();
    const int64_t base = e.payload() + (int64_t)e.instance_offset((size_t)instance);

    /* The asset may live in this partition or be imported from another.
     * Resolve both: a reader that handled only the internal case would find no
     * geometry on the shared shapes, which is most of them. */
    bf6::Ebx other(*c->types);
    const bf6::Ebx* src = &e;
    int assetInst = -1;
    {
        int32_t v = 0;
        if (!bf6__rime_i32(d, base + shapeOff, v) || v == 0) return -1;
        if (v & 1)
        {
            const int imp = v >> 1;
            if (imp < 0 || (size_t)imp >= e.import_count()) return -1;
            const auto& cand = c->src.armory_partition_candidates();
            const auto hit = cand.find(e.import_at((size_t)imp).partition);
            if (hit == cand.end() || hit->second.empty()) return -1;
            std::string p = hit->second.front();
            if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ebx") == 0) p.resize(p.size() - 4);
            std::string oerr;
            std::vector<uint8_t> oraw = c->src.get_ebx(p + ".ebx", oerr);
            if (oraw.empty()) oraw = c->src.get_ebx(p, oerr);
            if (oraw.empty()) return -1;
            other.set_guid_index(&c->src.armory_partition_index());
            if (!other.parse(std::move(oraw), oerr)) return -1;
            src = &other;
            /* The import names an exported instance; take the one whose GUID
             * matches rather than instance 0, which is only right by luck. */
            for (size_t q = 0; q < other.exported_count(); q++)
                if (other.instance_guid(q) == e.import_at((size_t)imp).instance)
                { assetInst = (int)q; break; }
            if (assetInst < 0) assetInst = 0;
        }
        else
        {
            assetInst = bf6__rime_internal_ptr(e, d, base + shapeOff);
            if (assetInst < 0) return -1;
        }
    }

    const std::vector<uint8_t>& ad = src->raw();
    const int64_t abase = src->payload() + (int64_t)src->instance_offset((size_t)assetInst);

    bf6_rime_shape_info out{};
    bf6__rime_f32(ad, abase + kRimeAssetSizeOff, out.size[0]);
    bf6__rime_f32(ad, abase + kRimeAssetSizeOff + 4, out.size[1]);

    /* The asset holds an array of shapes; the screens use one each. */
    int64_t sfirst = 0; int32_t scount = 0;
    if (!bf6__rime_array(ad, abase + kRimeAssetShapesOff, sfirst, scount) || scount <= 0)
    { if (info) *info = out; return 0; }
    const int shapeInst = bf6__rime_internal_ptr(*src, ad, sfirst);
    if (shapeInst < 0) { if (info) *info = out; return 0; }
    const int64_t sbase = src->payload() + (int64_t)src->instance_offset((size_t)shapeInst);

    int32_t style = 0;
    bf6__rime_i32(ad, sbase + kRimeShapeStyleOff, style);
    out.draw_style = style;
    bf6__rime_f32(ad, sbase + kRimeShapeThickOff, out.thickness);
    bf6__rime_f32(ad, sbase + kRimeShapeAlphaOff, out.alpha);

    int64_t af = 0, of = 0, cf = 0, xf = 0, kf = 0;
    int32_t an = 0, on = 0, cn = 0, xn = 0, kn = 0;
    const bool hasA = bf6__rime_value_array(ad, sbase + kRimeShapeAnchorsOff, 8, af, an);
    const bool hasO = bf6__rime_value_array(ad, sbase + kRimeShapeOffsetsOff, 8, of, on);
    const bool hasC = bf6__rime_value_array(ad, sbase + kRimeShapeColorsOff, 16, cf, cn);
    const bool hasX = bf6__rime_value_array(ad, sbase + kRimeShapeIndicesOff, 2, xf, xn);
    const bool hasK = bf6__rime_array(ad, sbase + kRimeShapeCornersOff, kf, kn);

    /* The three vertex arrays are parallel by construction; taking the minimum
     * rather than one of them means a truncated read cannot walk off the end
     * of a shorter sibling. */
    int32_t vn = 0;
    if (hasA && hasO) vn = an < on ? an : on;
    if (hasC && cn < vn) vn = cn;
    out.vertex_count = vn;
    out.index_count = hasX ? xn : 0;
    out.corner_count = hasK ? kn : 0;

    if (verts && vmax > 0)
        for (int i = 0; i < vn && i < vmax; i++)
        {
            bf6_rime_shape_vertex& v = verts[i];
            v = bf6_rime_shape_vertex{};
            bf6__rime_f32(ad, af + (int64_t)i * 8, v.anchor[0]);
            bf6__rime_f32(ad, af + (int64_t)i * 8 + 4, v.anchor[1]);
            bf6__rime_f32(ad, of + (int64_t)i * 8, v.offset[0]);
            bf6__rime_f32(ad, of + (int64_t)i * 8 + 4, v.offset[1]);
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
            if (hasC)
                for (int k = 0; k < 4; k++)
                    bf6__rime_f32(ad, cf + (int64_t)i * 16 + k * 4, v.color[k]);
        }

    if (indices && imax > 0 && hasX)
        for (int i = 0; i < xn && i < imax; i++)
        {
            uint8_t lo = 0, hi = 0;
            bf6__rime_u8(ad, xf + (int64_t)i * 2, lo);
            bf6__rime_u8(ad, xf + (int64_t)i * 2 + 1, hi);
            indices[i] = (unsigned short)(lo | ((unsigned)hi << 8));
        }

    if (corners && cmax > 0 && hasK)
        for (int i = 0; i < kn && i < cmax; i++)
        {
            bf6_rime_shape_corner& k = corners[i];
            k = bf6_rime_shape_corner{};
            const int ci = bf6__rime_internal_ptr(*src, ad, kf + (int64_t)i * 8);
            if (ci < 0) continue;
            const int64_t cb = src->payload() + (int64_t)src->instance_offset((size_t)ci);
            bf6__rime_f32(ad, cb + kRimeCornerAnchorOff, k.anchor[0]);
            bf6__rime_f32(ad, cb + kRimeCornerAnchorOff + 4, k.anchor[1]);
            bf6__rime_f32(ad, cb + kRimeCornerOffsetOff, k.offset[0]);
            bf6__rime_f32(ad, cb + kRimeCornerOffsetOff + 4, k.offset[1]);
            bf6__rime_i32(ad, cb + kRimeCornerTypeOff, k.corner_type);
            bf6__rime_f32(ad, cb + kRimeCornerCurveOff, k.curvature);
        }

    if (info) *info = out;
    return 1;
}

int bf6_rime_line(bf6_ctx* c, const char* partition, int instance,
                  bf6_rime_line_info* info,
                  bf6_rime_line_point* points, int point_max)
{
    if (!c || !partition || instance < 0 || point_max < 0) return -1;
    std::string terr;
    if (!ensure_types(c, terr)) return -1;

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(partition) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(partition, err);
    if (raw.empty()) return -1;

    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.armory_partition_index());
    if (!e.parse(std::move(raw), err) || (size_t)instance >= e.instance_count())
        return -1;
    const bf6__rime_type* ti = bf6__rime_type_of(e, (size_t)instance);
    if (!ti || ti->kind != BF6_RIME_LINE) return -1;

    const std::vector<uint8_t>& d = e.raw();
    const int64_t base = e.payload() + (int64_t)e.instance_offset((size_t)instance);

    /* The customization leader is endpoint-bound rather than a polyline.
     * Return its authored StartOffset/EndOffset as two points; the caller owns
     * the runtime anchor references.  Both GUID and shipped signature are
     * checked even though kRimeTypes already gates them, so this fixed-layout
     * branch cannot silently survive a patched record. */
    if (std::strcmp(ti->guid, kRimeAttachmentLeaderGuid) == 0 &&
        ti->signature == kRimeAttachmentLeaderSignature)
    {
        bf6_rime_line_info out{};
        out.point_count = 2;
        out.start_progress = 0.f;
        out.end_progress = 1.f;
        if (!bf6__rime_f32(d, base + kRimeAttachmentLeaderWidthOffset,
                           out.width))
            return -1;
        if (points && point_max > 0)
        {
            const int retained = point_max < 2 ? point_max : 2;
            for (int point_index = 0; point_index < retained; ++point_index)
            {
                const int64_t offset = base +
                    (point_index == 0 ? kRimeAttachmentLeaderStartOffset
                                      : kRimeAttachmentLeaderEndOffset);
                if (!bf6__rime_f32(d, offset, points[point_index].x) ||
                    !bf6__rime_f32(d, offset + 4, points[point_index].y))
                    return -1;
            }
        }
        if (info) *info = out;
        return 2;
    }

    int64_t first = 0;
    int32_t count = 0;
    if (!bf6__rime_value_array(d, base + kRimeLinePointsOff,
                               kRimePointStride, first, count))
        return -1;

    bf6_rime_line_info out{};
    out.point_count = count;
    if (!bf6__rime_f32(d, base + kRimeLineWidthOff, out.width) ||
        !bf6__rime_i32(d, base + kRimeLineCapOff, out.cap_type) ||
        !bf6__rime_f32(d, base + kRimeLineEndOff, out.end_progress) ||
        !bf6__rime_f32(d, base + kRimeLineGlowOff, out.glow_size) ||
        !bf6__rime_f32(d, base + kRimeLineStartOff, out.start_progress))
        return -1;
    uint8_t relative = 0, closed = 0, single = 0;
    if (!bf6__rime_u8(d, base + kRimeLineRelativeOff, relative) ||
        !bf6__rime_u8(d, base + kRimeLineCloseOff, closed) ||
        !bf6__rime_u8(d, base + kRimeLineSingleOff, single)) return -1;
    out.relative_coordinates = relative ? 1 : 0;
    out.close_line_shape = closed ? 1 : 0;
    out.single_pixel = single ? 1 : 0;

    if (points && point_max > 0)
        for (int i = 0; i < count && i < point_max; ++i)
        {
            bf6_rime_line_point& p = points[i];
            p = bf6_rime_line_point{};
            const int64_t pb = first + (int64_t)i * kRimePointStride +
                               kRimePointVecOff;
            if (!bf6__rime_f32(d, pb, p.x) || !bf6__rime_f32(d, pb + 4, p.y))
                return -1;
        }
    if (info) *info = out;
    return count;
}

/* Property/event connections and localized-string entities.
 *
 * PropertyConnections are part of the shipped Blueprint base and MUST NOT be
 * reflected through the current executable schema.  The current Steam data
 * carries the older RimeWidgetBlueprint identity and its exact record is:
 *
 *   blueprint +48 : relative array
 *   record +0      : source PointerRef (8 bytes)
 *          +8      : target PointerRef (8 bytes)
 *          +16     : source field id
 *          +20     : flags/realm
 *          +24     : target field id
 *          +28     : zero padding
 *
 * The 32-byte stride validates every one of the current common/UI records;
 * 24/28/36/40-byte controls do not.  Keeping this raw read here also prevents
 * the exact silent failure that omitted the shipped widget graph when only
 * the executable-era blueprint GUID was accepted.
 */
static const char* const kRimeBlueprintTypes[] = {
    "79c84e20-71ad-d567-89ea-89cb6d87a61f", /* Blueprint */
    "9f439e38-30e7-ecb0-36bf-cfd8662efc86", /* LogicPrefabBlueprint */
    "27cb02e6-f2b4-da2c-a523-3d22c07bf63a", /* executable-era widget */
    "916c49b2-6aa4-d60b-d99d-4aee39944388"  /* shipped widget */
};
static const uint32_t kRimePropertyConnectionsOff = 48;
static const uint32_t kRimePropertyConnectionSize = 32;
static const uint32_t kRimeEventConnectionsOff = 64;
static const uint32_t kRimeEventConnectionSize = 32;
static const uint32_t kRimeLinkConnectionsOff = 32;
static const uint32_t kRimeLinkConnectionSize = 24;
static const uint32_t kRimeFieldStringKey   = 0x2bdc236au;
static const char* const kRimeStringEntityType =
    "7f8406ab-1c16-4449-dea4-5d6c68619c3b";
static const char* const kRimeConditionalFloatType =
    "70732d52-d009-fe2d-9e88-a7f128e5d138";
static const char* const kRimeFloatInterpolatorType =
    "b7e8b8fb-9272-3a65-d159-536528f3c6e5";
static const char* const kRimeConditionalPropertyType =
    "e0256328-8258-8afe-c1d6-dee7882820fd";
static const char* const kRimeAndOperationType =
    "1c8375b9-d39b-6801-9eab-230e981dfdf5";
static const char* const kRimeOrOperationType =
    "dffac108-43f6-e75a-7201-048d93fc1803";
static const char* const kRimeNotOperationType =
    "60c0d5e9-7990-23e6-2713-5e12e0286226";
static const char* const kRimePropertyDefaultType =
    "c28d8c2c-8e20-5361-9f06-76415aa81450";
static const char* const kRimePropertyStatusType =
    "fb2e9266-3f4d-4f14-fb99-aadaaacf33d7";
static const char* const kRimeMathEntityType =
    "bd6c87d2-2954-dd96-b9c8-a4db10cb22c4";
static const char* const kRimeMathOperationType =
    "e8e6d2a9-b00e-0112-d622-4cc0209cb187";
static const char* const kRimeRoundingType =
    "938f9afe-bce5-6b0f-b188-89dcf9779930";
static const char* const kRimePropertyCastType =
    "07aae756-3e17-42e7-a1ff-915b05aee393";
static const char* const kRimeCompareFloatType =
    "e8252d49-9b2b-88d3-053e-181777f46128";
static const char* const kRimeArrayElementType =
    "64c6d47e-4a24-3218-6670-8f0e6dc6987b";
static const char* const kRimeLogicPrefabReferenceType =
    "0de9bfc7-a83d-8d72-a684-c3ec2650aa83";
static const char* const kRimeInterfaceDescriptorType =
    "f2e1dfd3-7b56-e6f1-2976-0bf6658c162e";
static const uint32_t kRimeFieldBlueprintHash          = 0x6b831b88u;
static const uint32_t kRimeFieldValueIfTrue  = 0xb205664du;
static const uint32_t kRimeFieldValueIfFalse = 0xe1b1a100u;
static const uint32_t kRimeFieldCondition    = 0xc2cc8649u;
static const uint32_t kRimeFieldDefaultValue = 0x42c8b257u;
static const uint32_t kRimeFieldDuration = 0xed10e827u;
static const uint32_t kRimeFieldVelocity = 0xe16f4c60u;
static const uint32_t kRimeFieldUseVelocity = 0xbce780e1u;
static const uint32_t kRimeFieldInterpolationType = 0x0211e93au;
static const uint32_t kRimeFieldInterpolationMode = 0x35ee533bu;
static const uint32_t kRimeFieldDynamicDuration = 0x0b243303u;
static const uint32_t kRimeFieldUseRealTimeClock = 0x4cc65a17u;
static const uint32_t kRimeFieldForceFrameCorrectOutput = 0x88deb19eu;
static const uint32_t kRimeFieldValueIfTruePropertyHash  = 0x33b902dau;
static const uint32_t kRimeFieldValueIfFalsePropertyHash = 0x90eb005du;
static const uint32_t kRimeFieldOutHash                  = 0x2d0c77a4u;
static const uint32_t kRimeInterfaceFieldsHash           = 0x21943083u;
static const uint32_t kRimeDataFieldBoxedValueHash       = 0x47f25917u;
static const uint32_t kRimeDataFieldIdHash               = 0xc1ad37dau;
static const uint32_t kRimeDataFieldAccessTypeHash       = 0x331d8a7bu;
static const uint32_t kRimeDbdDataNameHash               = 0xbf303f90u;
static const uint32_t kRimeDbdFieldsHash                 = 0xda58bbd4u;
static const uint32_t kRimeDbdFieldNameHash              = 0x0c59fa06u;
static const uint32_t kRimeFieldTypeRefHash              = 0x207099beu;
static const uint32_t kRimePinValueHash                  = 0x0d6ef36eu;
static const uint32_t kRimePinInValueHash                = 0x62da2189u;
static const uint32_t kRimePinDefaultValueHash           = 0x7b256865u;
static const uint32_t kRimePinArrayHash                  = 0x0c47719cu;
static const uint32_t kRimePinIndexHash                  = 0x0cc9279bu;
static const uint32_t kRimePinElementHash                = 0x043d775bu;
static const uint32_t kRimePropertyStatusInHash          = 0x0561f944u;
static const uint32_t kRimeStringArgumentFieldsHash      = 0xba863ec2u;
static const uint32_t kRimeMathAssemblyHash              = 0x9f5a51b5u;
static const uint32_t kRimeMathInstructionsHash          = 0x81cbda77u;
static const uint32_t kRimeMathParam1Hash                = 0x0eaa6aacu;
static const uint32_t kRimeMathParam2Hash                = 0x4767b705u;
static const uint32_t kRimeMathCodeHash                  = 0x97a3169cu;
static const uint32_t kRimeMathResultHash                = 0xaee62a28u;
static const uint32_t kRimeMathOperatorsHash             = 0x0e1b35fcu;
static const uint32_t kRimeRoundingModeHash              = 0x8a2825e3u;
static const uint32_t kRimeCompareAHash                  = 0x885fb446u;
static const uint32_t kRimeCompareBHash                  = 0x501476d8u;
static const uint32_t kRimePinIn0Hash                    = 0x0b87d372u;
static const uint32_t kRimePinIn1Hash                    = 0x0b87d373u;
static const uint32_t kRimePinOutHash                    = 0x0b87df4bu;
static const uint32_t kRimePinFloatInHash                = 0x7f39a652u;
static const uint32_t kRimePinIntOutHash                 = 0xa5f78658u;
static const uint32_t kRimePinFloatOutHash               = 0x666e571bu;
static const uint32_t kRimePinIntValueHash               = 0x0056c23du;
static const uint32_t kRimePinFloatValueHash             = 0xba31493eu;
static const uint32_t kRimePinBoolValueHash              = 0x0a7e28e0u;
static const uint32_t kRimePinUintValueHash              = 0xef81c8e8u;
static const uint32_t kRimePinStringValueHash            = 0x4bb0cd9bu;
static const uint32_t kRimePinUint64ValueHash            = 0xaeabcb6au;
static const uint32_t kRimePinCastToIntHash              = 0x0ea01568u;
static const uint32_t kRimePinCastToUintHash             = 0xe2a032ddu;
static const uint32_t kRimePinCastToFloatHash            = 0x37d7840bu;
static const uint32_t kRimePinCastToDoubleHash           = 0x401a44ceu;
static const uint32_t kRimePinCastToBoolHash             = 0xe2a75c55u;
static const uint32_t kRimePinCastToStringHash           = 0x0885f34eu;

/* A DBD TypeRef is still opaque to this layer.  The generic diagnostic dump
 * represented its one unhandled scalar as `raw=...`; walk the decoded value
 * directly and require that same one-to-one structural invariant.  Rejecting
 * zero or multiple raw leaves is important: choosing the first would turn a
 * schema mismatch into convincing type signatures. */
static void bf6__rime_dbd_unknowns(const bf6::EbxValue& value,
                                   int& count, uint64_t& raw)
{
    if (value.kind == bf6::EbxValue::Kind::Unknown)
    {
        ++count;
        raw = value.u;
        return;
    }
    if (value.kind == bf6::EbxValue::Kind::Array)
        for (const bf6::EbxValue& item : value.items)
            bf6__rime_dbd_unknowns(item, count, raw);
    else if (value.kind == bf6::EbxValue::Kind::Struct)
        for (const auto& field : value.fields)
            bf6__rime_dbd_unknowns(field.second, count, raw);
}

int bf6_rime_dbd_fields(bf6_ctx* c, const char* partition,
                        char* data_name, int data_name_len,
                        bf6_rime_dbd_field* out, int out_max)
{
    if (data_name && data_name_len > 0) data_name[0] = 0;
    if (!c || !partition || !*partition || data_name_len < 0 || out_max < 0)
        return -1;

    std::string type_err;
    if (!ensure_types(c, type_err)) return -1;

    std::string err;
    std::vector<uint8_t> raw =
        c->src.get_ebx(std::string(partition) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(partition, err);
    if (raw.empty()) return -1;

    bf6::Ebx ebx(*c->types);
    if (!ebx.parse(std::move(raw), err)) return -1;

    const std::vector<uint32_t> wanted = {
        kRimeDbdDataNameHash, kRimeDbdFieldsHash
    };
    for (size_t instance = 0; instance < ebx.instance_count(); ++instance)
    {
        const bf6::EbxValue root = ebx.read_instance(instance, &wanted);
        const bf6::EbxValue* name = root.field(kRimeDbdDataNameHash);
        const bf6::EbxValue* fields = root.field(kRimeDbdFieldsHash);
        if (!name || name->kind != bf6::EbxValue::Kind::Str || name->s.empty() ||
            !fields || fields->kind != bf6::EbxValue::Kind::Array ||
            fields->items.empty() || fields->items.size() > 256)
            continue;

        if (data_name && data_name_len > 0)
            std::snprintf(data_name, (size_t)data_name_len, "%s", name->s.c_str());

        const int total = (int)fields->items.size();
        for (int index = 0; index < total; ++index)
        {
            const bf6::EbxValue& item = fields->items[(size_t)index];
            const bf6::EbxValue* field_name =
                item.field(kRimeDbdFieldNameHash);
            int unknown_count = 0;
            uint64_t signature = 0;
            bf6__rime_dbd_unknowns(item, unknown_count, signature);
            if (!field_name || field_name->kind != bf6::EbxValue::Kind::Str ||
                field_name->s.empty() || unknown_count != 1)
                return -1;

            if (out && index < out_max)
            {
                bf6_rime_dbd_field row{};
                std::snprintf(row.name, sizeof(row.name), "%s",
                              field_name->s.c_str());
                row.type_signature = signature;
                out[index] = row;
            }
        }
        return total;
    }
    return -1;
}

static int32_t bf6__rime_public_value_kind(const bf6::EbxValue& value)
{
    using Kind = bf6::EbxValue::Kind;
    switch (value.kind)
    {
    case Kind::Null:   return BF6_RIME_VALUE_NULL;
    case Kind::Bool:   return BF6_RIME_VALUE_BOOL;
    case Kind::Int:    return BF6_RIME_VALUE_INT;
    case Kind::Uint:   return BF6_RIME_VALUE_UINT;
    case Kind::Real:   return BF6_RIME_VALUE_REAL;
    case Kind::Str:    return BF6_RIME_VALUE_STRING;
    case Kind::Struct: return BF6_RIME_VALUE_STRUCT;
    case Kind::Array:  return BF6_RIME_VALUE_ARRAY;
    default:           return BF6_RIME_VALUE_UNRESOLVED;
    }
}

static void bf6__rime_copy_interface_value(
    const bf6::EbxValue& value, bf6_rime_interface_field& row)
{
    row.value_kind = bf6__rime_public_value_kind(value);
    switch (value.kind)
    {
    case bf6::EbxValue::Kind::Bool: row.bool_value = value.b ? 1 : 0; break;
    case bf6::EbxValue::Kind::Int: row.int_value = value.i; break;
    case bf6::EbxValue::Kind::Uint: row.uint_value = value.u; break;
    case bf6::EbxValue::Kind::Real: row.real_value = value.f; break;
    case bf6::EbxValue::Kind::Str:
        std::snprintf(row.string_value, sizeof(row.string_value), "%s",
                      value.s.c_str());
        break;
    default: break;
    }
}

static bool bf6__rime_is_blueprint(const bf6::Ebx& e, size_t instance)
{
    const std::string guid = bf6::TypeDb::guid_str(e.instance_type(instance));
    for (const char* accepted : kRimeBlueprintTypes)
        if (guid == accepted) return true;
    return false;
}

static void bf6__rime_read_property_connections(
    const bf6::Ebx& e, const std::vector<uint8_t>& d,
    std::vector<bf6_rime_connection>& out)
{
    for (size_t instance = 0; instance < e.instance_count(); ++instance)
    {
        if (!bf6__rime_is_blueprint(e, instance)) continue;
        const int64_t base = e.payload() + (int64_t)e.instance_offset(instance);
        int64_t first = 0;
        int32_t count = 0;
        if (!bf6__rime_array(d, base + kRimePropertyConnectionsOff, first, count))
            continue;
        if (first < 0 || first + (int64_t)count * kRimePropertyConnectionSize >
                             (int64_t)d.size())
            continue;

        for (int32_t index = 0; index < count; ++index)
        {
            const int64_t row = first + (int64_t)index * kRimePropertyConnectionSize;
            const int source = bf6__rime_internal_ptr(e, d, row);
            const int target = bf6__rime_internal_ptr(e, d, row + 8);
            uint32_t source_upper = 1, target_upper = 1, target_field = 0;
            uint32_t flags = 0, source_field = 0, trailing = 1;
            if (source < 0 || target < 0 ||
                !bf6__rime_u32(d, row + 4, source_upper) ||
                !bf6__rime_u32(d, row + 12, target_upper) ||
                /* Shipped Rime's old-schema connection record stores the
                 * TARGET endpoint's dynamic property at +16 and the SOURCE
                 * endpoint's property at +24.  Reading those through the
                 * current executable schema gives the opposite field names,
                 * but fails the semantic control: Interface.Description must
                 * feed Label.LocalizedText, not vice versa. */
                !bf6__rime_u32(d, row + 16, target_field) ||
                !bf6__rime_u32(d, row + 20, flags) ||
                !bf6__rime_u32(d, row + 24, source_field) ||
                !bf6__rime_u32(d, row + 28, trailing) ||
                source_upper != 0 || target_upper != 0 || trailing != 0 ||
                (flags & 7u) > 3u)
                continue;

            bf6_rime_connection connection{};
            connection.source = source;
            connection.target = target;
            connection.source_field = source_field;
            connection.target_field = target_field;
            connection.mode = (int32_t)flags;
            out.push_back(connection);
        }
    }
}

static void bf6__rime_read_event_connections(
    const bf6::Ebx& e, const std::vector<uint8_t>& d,
    std::vector<bf6_rime_event_connection>& out)
{
    for (size_t instance = 0; instance < e.instance_count(); ++instance)
    {
        if (!bf6__rime_is_blueprint(e, instance)) continue;
        const int64_t base = e.payload() + (int64_t)e.instance_offset(instance);
        int64_t first = 0;
        int32_t count = 0;
        if (!bf6__rime_array(d, base + kRimeEventConnectionsOff, first, count))
            continue;
        if (first < 0 || first + (int64_t)count * kRimeEventConnectionSize >
                             (int64_t)d.size())
            continue;

        for (int32_t index = 0; index < count; ++index)
        {
            const int64_t row = first + (int64_t)index * kRimeEventConnectionSize;
            const int source = bf6__rime_internal_ptr(e, d, row);
            const int target = bf6__rime_internal_ptr(e, d, row + 8);
            uint32_t source_upper = 1, target_upper = 1, target_event = 0;
            uint32_t mode = 0, source_event = 0, trailing = 1;
            if (source < 0 || target < 0 ||
                !bf6__rime_u32(d, row + 4, source_upper) ||
                !bf6__rime_u32(d, row + 12, target_upper) ||
                !bf6__rime_u32(d, row + 16, target_event) ||
                !bf6__rime_u32(d, row + 20, mode) ||
                !bf6__rime_u32(d, row + 24, source_event) ||
                !bf6__rime_u32(d, row + 28, trailing) ||
                source_upper != 0 || target_upper != 0 || trailing != 0 ||
                (mode != 2u && mode != 3u))
                continue;

            bf6_rime_event_connection connection{};
            connection.source = source;
            connection.target = target;
            connection.source_event = source_event;
            connection.target_event = target_event;
            connection.mode = (int32_t)mode;
            out.push_back(connection);
        }
    }
}

struct bf6__rime_local_link_connection
{
    int32_t source = -1;
    int32_t target = -1;
    uint32_t source_field = 0;
    uint32_t target_field = 0;
};

/* The UI timeline target is not a property wire.  EntityTrackData names its
 * visual through Blueprint.LinkConnections (+32, 24-byte records), with the
 * independently verified EntityLink source pin and the target's implicit
 * zero slot.  Only local sources are useful to a track enumerated from this
 * partition; imported sources are deliberately not coerced into an instance
 * index. */
static void bf6__rime_read_local_link_connections(
    const bf6::Ebx& e, const std::vector<uint8_t>& d,
    std::vector<bf6__rime_local_link_connection>& out)
{
    for (size_t instance = 0; instance < e.instance_count(); ++instance)
    {
        if (!bf6__rime_is_blueprint(e, instance)) continue;
        const int64_t base = e.payload() + (int64_t)e.instance_offset(instance);
        int64_t first = 0;
        int32_t count = 0;
        if (!bf6__rime_array(d, base + kRimeLinkConnectionsOff, first, count) ||
            first < 0 || first + (int64_t)count * kRimeLinkConnectionSize >
                             (int64_t)d.size())
            continue;
        for (int32_t index = 0; index < count; ++index)
        {
            const int64_t row = first +
                (int64_t)index * kRimeLinkConnectionSize;
            const int source = bf6__rime_internal_ptr(e, d, row);
            const int target = bf6__rime_internal_ptr(e, d, row + 8);
            uint32_t source_upper = 1, target_upper = 1;
            bf6__rime_local_link_connection link{};
            if (source < 0 || target < 0 ||
                !bf6__rime_u32(d, row + 4, source_upper) ||
                !bf6__rime_u32(d, row + 12, target_upper) ||
                !bf6__rime_u32(d, row + 16, link.source_field) ||
                !bf6__rime_u32(d, row + 20, link.target_field) ||
                source_upper != 0 || target_upper != 0)
                continue;
            link.source = source;
            link.target = target;
            out.push_back(link);
        }
    }
}

static bool bf6__rime_load_bindings(bf6_ctx* c, const char* partition)
{
    if (c->rime_bindings_key == partition) return true;

    const auto cached = c->rime_bindings_by_partition.find(partition);
    if (cached != c->rime_bindings_by_partition.end())
    {
        const bf6_rime_binding_cache& rows = cached->second;
        c->rime_binding_connections = rows.connections;
        c->rime_binding_event_connections = rows.event_connections;
        c->rime_binding_conditional_floats = rows.conditional_floats;
        c->rime_binding_float_interpolators = rows.float_interpolators;
        c->rime_binding_conditional_properties = rows.conditional_properties;
        c->rime_binding_logic_operations = rows.logic_operations;
        c->rime_binding_property_statuses = rows.property_statuses;
        c->rime_binding_math_instructions = rows.math_instructions;
        c->rime_binding_math_operations = rows.math_operations;
        c->rime_binding_roundings = rows.roundings;
        c->rime_binding_property_casts = rows.property_casts;
        c->rime_binding_compare_floats = rows.compare_floats;
        c->rime_binding_array_elements = rows.array_elements;
        c->rime_binding_logic_references = rows.logic_references;
        c->rime_binding_interface_descriptors = rows.interface_descriptors;
        c->rime_binding_interface_fields = rows.interface_fields;
        c->rime_binding_interface_struct_types = rows.interface_struct_types;
        c->rime_binding_string_entities = rows.string_entities;
        c->rime_binding_string_arguments = rows.string_arguments;
        c->rime_bindings_key = partition;
        return true;
    }

    std::string terr;
    if (!ensure_types(c, terr)) return false;

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(partition) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(partition, err);
    if (raw.empty()) return false;

    bf6::Ebx e(*c->types);
    /* The focused front-end index contains the UI graph imports and is also
     * what the Rime tree walk uses.  Building the full 465k-partition index
     * here made opening a single button graph pay a whole-game scan. */
    e.set_guid_index(&c->src.armory_partition_index());
    if (!e.parse(std::move(raw), err)) return false;

    c->rime_binding_connections.clear();
    c->rime_binding_event_connections.clear();
    c->rime_binding_conditional_floats.clear();
    c->rime_binding_float_interpolators.clear();
    c->rime_binding_conditional_properties.clear();
    c->rime_binding_logic_operations.clear();
    c->rime_binding_property_statuses.clear();
    c->rime_binding_math_instructions.clear();
    c->rime_binding_math_operations.clear();
    c->rime_binding_roundings.clear();
    c->rime_binding_property_casts.clear();
    c->rime_binding_compare_floats.clear();
    c->rime_binding_array_elements.clear();
    c->rime_binding_logic_references.clear();
    c->rime_binding_interface_descriptors.clear();
    c->rime_binding_interface_fields.clear();
    c->rime_binding_interface_struct_types.clear();
    c->rime_binding_string_entities.clear();
    c->rime_binding_string_arguments.clear();
    /* Dynamic pin operators are admitted only when their payload and the
     * independently authored connection table agree.  Read connections first
     * so the gate below does not trust a type name alone. */
    bf6__rime_read_property_connections(
        e, e.raw(), c->rime_binding_connections);
    for (size_t i = 0; i < e.instance_count(); i++)
    {
        const bf6::EbxValue v = e.read_instance(i);
        const std::string type_guid =
            bf6::TypeDb::guid_str(e.instance_type(i));

        int32_t logic_operation = 0;
        if (type_guid == kRimeAndOperationType)
            logic_operation = BF6_RIME_LOGIC_AND;
        else if (type_guid == kRimeOrOperationType)
            logic_operation = BF6_RIME_LOGIC_OR;
        else if (type_guid == kRimeNotOperationType)
            logic_operation = BF6_RIME_LOGIC_NOT;
        else if (type_guid == kRimePropertyDefaultType)
        {
            const bf6::EbxValue* typeRef = v.field(kRimeFieldTypeRefHash);
            bool topologyValid = typeRef != nullptr;
            bool hasOutput = false;
            for (const bf6_rime_connection& connection :
                 c->rime_binding_connections)
            {
                if (connection.target == (int32_t)i &&
                    connection.target_field != kRimePinInValueHash &&
                    connection.target_field != kRimePinDefaultValueHash)
                    topologyValid = false;
                if (connection.source == (int32_t)i)
                {
                    hasOutput = true;
                    if (connection.source_field != kRimePinValueHash)
                        topologyValid = false;
                }
            }
            if (topologyValid && hasOutput)
                logic_operation = BF6_RIME_LOGIC_PROPERTY_DEFAULT;
        }
        if (logic_operation)
        {
            bf6_rime_logic_operation row{};
            row.instance = (int32_t)i;
            row.operation = logic_operation;
            c->rime_binding_logic_operations.push_back(row);
        }

        if (type_guid == kRimePropertyStatusType)
        {
            const bf6::EbxValue* inputHash =
                v.field(kRimePropertyStatusInHash);
            uint32_t field = 0;
            if (inputHash && inputHash->kind == bf6::EbxValue::Kind::Uint)
                field = (uint32_t)inputHash->u;
            else if (inputHash &&
                     inputHash->kind == bf6::EbxValue::Kind::Int)
                field = (uint32_t)inputHash->i;
            bool hasInput = false;
            bool hasOutput = false;
            for (const bf6_rime_connection& connection :
                 c->rime_binding_connections)
            {
                if (connection.target == (int32_t)i)
                    hasInput |= connection.target_field == field;
                if (connection.source == (int32_t)i)
                    hasOutput = true;
            }
            if (field && hasInput && hasOutput)
            {
                bf6_rime_property_status row{};
                row.instance = (int32_t)i;
                row.input_field = field;
                c->rime_binding_property_statuses.push_back(row);
            }
        }

        if (type_guid == kRimeMathEntityType)
        {
            const bf6::EbxValue* assembly = v.field(kRimeMathAssemblyHash);
            const bf6::EbxValue* instructions = assembly
                ? assembly->field(kRimeMathInstructionsHash) : nullptr;
            bool hasOutput = false;
            for (const bf6_rime_connection& connection :
                 c->rime_binding_connections)
                hasOutput |= connection.source == (int32_t)i;

            std::vector<bf6_rime_math_instruction> rows;
            bool valid = hasOutput && assembly && instructions &&
                assembly->kind == bf6::EbxValue::Kind::Struct &&
                instructions->kind == bf6::EbxValue::Kind::Array &&
                !instructions->items.empty() &&
                instructions->items.size() <= 4096;
            auto integer = [](const bf6::EbxValue* value, int32_t& out) {
                if (!value) return false;
                if (value->kind == bf6::EbxValue::Kind::Int &&
                    value->i >= (std::numeric_limits<int32_t>::min)() &&
                    value->i <= (std::numeric_limits<int32_t>::max)())
                { out = (int32_t)value->i; return true; }
                if (value->kind == bf6::EbxValue::Kind::Uint &&
                    value->u <= (uint64_t)(std::numeric_limits<uint32_t>::max)())
                { out = (int32_t)(uint32_t)value->u; return true; }
                return false;
            };
            bool hasReturn = false;
            if (valid)
                for (size_t instruction = 0;
                     instruction < instructions->items.size(); ++instruction)
                {
                    const bf6::EbxValue& item =
                        instructions->items[instruction];
                    bf6_rime_math_instruction row{};
                    row.instance = (int32_t)i;
                    row.instruction_index = (int32_t)instruction;
                    valid = item.kind == bf6::EbxValue::Kind::Struct &&
                        integer(item.field(kRimeMathParam1Hash), row.param1) &&
                        integer(item.field(kRimeMathParam2Hash), row.param2) &&
                        integer(item.field(kRimeMathCodeHash), row.code) &&
                        integer(item.field(kRimeMathResultHash), row.result) &&
                        row.code >= 0 && row.code <= 68 && row.result >= 0;
                    if (!valid) break;
                    hasReturn |= row.code == 68;
                    if (row.code >= 3 && row.code <= 9)
                    {
                        bool connected = false;
                        for (const bf6_rime_connection& connection :
                             c->rime_binding_connections)
                            if (connection.target == (int32_t)i &&
                                connection.target_field ==
                                    (uint32_t)row.param1)
                            { connected = true; break; }
                        if (!connected) { valid = false; break; }
                    }
                    rows.push_back(row);
                }
            if (valid && hasReturn)
                c->rime_binding_math_instructions.insert(
                    c->rime_binding_math_instructions.end(),
                    rows.begin(), rows.end());
        }

        if (type_guid == kRimeMathOperationType)
        {
            const bf6::EbxValue* operators = v.field(kRimeMathOperatorsHash);
            bf6_rime_math_operation row{};
            row.instance = (int32_t)i;
            bool valid = operators &&
                operators->kind == bf6::EbxValue::Kind::Array &&
                !operators->items.empty() && operators->items.size() <= 16;
            if (valid)
                for (size_t op = 0; op < operators->items.size(); ++op)
                {
                    const bf6::EbxValue& value = operators->items[op];
                    int64_t code = -1;
                    if (value.kind == bf6::EbxValue::Kind::Int) code = value.i;
                    else if (value.kind == bf6::EbxValue::Kind::Uint &&
                             value.u <= (uint64_t)(std::numeric_limits<int32_t>::max)())
                        code = (int64_t)value.u;
                    if (code < 0 || code > 7) { valid = false; break; }
                    row.operators[op] = (int32_t)code;
                }
            row.operator_count = valid ? (int32_t)operators->items.size() : 0;
            static const uint32_t kInputs[17] = {
                0x0b87d372u, 0x0b87d373u, 0x0b87d370u, 0x0b87d371u,
                0x0b87d376u, 0x0b87d377u, 0x0b87d374u, 0x0b87d375u,
                0x0b87d37au, 0x0b87d37bu, 0x7c8241e3u, 0x7c8241e2u,
                0x7c8241e1u, 0x7c8241e0u, 0x7c8241e7u, 0x7c8241e6u,
                0x7c8241e5u
            };
            if (valid)
            {
                bool hasOutput = false;
                bool inputs[17]{};
                for (const bf6_rime_connection& connection :
                     c->rime_binding_connections)
                {
                    if (connection.target == (int32_t)i)
                        for (int pin = 0; pin <= row.operator_count; ++pin)
                            if (connection.target_field == kInputs[pin])
                                inputs[pin] = true;
                    if (connection.source == (int32_t)i &&
                        connection.source_field == kRimePinOutHash)
                        hasOutput = true;
                }
                valid = hasOutput;
                for (int pin = 0; valid && pin <= row.operator_count; ++pin)
                    valid = inputs[pin];
            }
            if (valid) c->rime_binding_math_operations.push_back(row);
        }

        if (type_guid == kRimeRoundingType)
        {
            const bf6::EbxValue* mode = v.field(kRimeRoundingModeHash);
            int64_t decoded = -1;
            if (mode && mode->kind == bf6::EbxValue::Kind::Int)
                decoded = mode->i;
            else if (mode && mode->kind == bf6::EbxValue::Kind::Uint)
                decoded = (int64_t)mode->u;
            bool hasInput = false, hasOutput = false, topologyValid = true;
            for (const bf6_rime_connection& connection :
                 c->rime_binding_connections)
            {
                if (connection.target == (int32_t)i)
                {
                    hasInput |= connection.target_field == kRimePinFloatInHash;
                    topologyValid &= connection.target_field == kRimePinFloatInHash;
                }
                if (connection.source == (int32_t)i)
                {
                    const bool accepted =
                        connection.source_field == kRimePinIntOutHash ||
                        connection.source_field == kRimePinFloatOutHash;
                    hasOutput |= accepted;
                    topologyValid &= accepted;
                }
            }
            if (decoded >= 0 && decoded <= 2 && hasInput && hasOutput &&
                topologyValid)
            {
                bf6_rime_rounding row{};
                row.instance = (int32_t)i;
                row.rounding_type = (int32_t)decoded;
                c->rime_binding_roundings.push_back(row);
            }
        }

        if (type_guid == kRimePropertyCastType)
        {
            static const uint32_t kInputs[] = {
                kRimePinIntValueHash, kRimePinFloatValueHash,
                kRimePinBoolValueHash, kRimePinUintValueHash,
                kRimePinStringValueHash, kRimePinUint64ValueHash
            };
            static const uint32_t kOutputs[] = {
                kRimePinCastToIntHash, kRimePinCastToUintHash,
                kRimePinCastToFloatHash, kRimePinCastToDoubleHash,
                kRimePinCastToBoolHash, kRimePinCastToStringHash
            };
            auto contains = [](const uint32_t* first, const uint32_t* last,
                               uint32_t value) {
                return std::find(first, last, value) != last;
            };
            bool hasInput = false, hasOutput = false, topologyValid = true;
            for (const bf6_rime_connection& connection :
                 c->rime_binding_connections)
            {
                if (connection.target == (int32_t)i)
                {
                    const bool accepted = contains(
                        kInputs, kInputs + sizeof(kInputs) / sizeof(kInputs[0]),
                        connection.target_field);
                    hasInput |= accepted;
                    topologyValid &= accepted;
                }
                if (connection.source == (int32_t)i)
                {
                    const bool accepted = contains(
                        kOutputs,
                        kOutputs + sizeof(kOutputs) / sizeof(kOutputs[0]),
                        connection.source_field);
                    hasOutput |= accepted;
                    topologyValid &= accepted;
                }
            }
            if (hasInput && hasOutput && topologyValid)
                c->rime_binding_property_casts.push_back({(int32_t)i});
        }

        if (type_guid == kRimeCompareFloatType)
        {
            const bf6::EbxValue* a = v.field(kRimeCompareAHash);
            const bf6::EbxValue* b = v.field(kRimeCompareBHash);
            bool hasInput = false, hasOutput = false;
            for (const bf6_rime_connection& connection :
                 c->rime_binding_connections)
            {
                if (connection.target == (int32_t)i &&
                    (connection.target_field == 0x0002b5e4u ||
                     connection.target_field == 0x0002b5e7u))
                    hasInput = true;
                if (connection.source == (int32_t)i &&
                    (connection.source_field == 0x0b87bd3bu ||
                     connection.source_field == 0x7c7f159au ||
                     connection.source_field == 0x7c7f72c5u ||
                     connection.source_field == 0x7c7f6a47u ||
                     connection.source_field == 0x0b87bdd8u ||
                     connection.source_field == 0x0b87bd1au))
                    hasOutput = true;
            }
            if (a && b && a->kind == bf6::EbxValue::Kind::Real &&
                b->kind == bf6::EbxValue::Kind::Real && hasInput && hasOutput)
            {
                bf6_rime_compare_float row{};
                row.instance = (int32_t)i;
                row.authored_a = a->f;
                row.authored_b = b->f;
                c->rime_binding_compare_floats.push_back(row);
            }
        }

        if (type_guid == kRimeArrayElementType)
        {
            bool has_array = false;
            bool has_index = false;
            bool has_element = false;
            bool topology_valid = true;
            for (const bf6_rime_connection& connection :
                 c->rime_binding_connections)
            {
                if (connection.target == (int32_t)i)
                {
                    has_array |= connection.target_field == kRimePinArrayHash;
                    has_index |= connection.target_field == kRimePinIndexHash;
                    if (connection.target_field != kRimePinArrayHash &&
                        connection.target_field != kRimePinIndexHash)
                        topology_valid = false;
                }
                if (connection.source == (int32_t)i)
                {
                    has_element |=
                        connection.source_field == kRimePinElementHash;
                    if (connection.source_field != kRimePinElementHash)
                        topology_valid = false;
                }
            }
            if (topology_valid && has_array && has_index && has_element)
            {
                bf6_rime_array_element row{};
                row.instance = (int32_t)i;
                c->rime_binding_array_elements.push_back(row);
            }
        }

        if (type_guid == kRimeLogicPrefabReferenceType)
        {
            const bf6::EbxValue* blueprint =
                v.field(kRimeFieldBlueprintHash);
            if (blueprint &&
                blueprint->kind == bf6::EbxValue::Kind::ImportRef &&
                !blueprint->import_path.empty() &&
                blueprint->import_path != "<not indexed>")
            {
                bf6_rime_logic_reference row{};
                row.instance = (int32_t)i;
                std::snprintf(row.blueprint, sizeof(row.blueprint), "%s",
                              blueprint->import_path.c_str());
                c->rime_binding_logic_references.push_back(row);
            }
        }

        if (type_guid == kRimeConditionalFloatType)
        {
            const bf6::EbxValue* whenTrue = v.field(kRimeFieldValueIfTrue);
            const bf6::EbxValue* whenFalse = v.field(kRimeFieldValueIfFalse);
            const bf6::EbxValue* condition = v.field(kRimeFieldCondition);
            if (whenTrue && whenFalse && condition &&
                whenTrue->kind == bf6::EbxValue::Kind::Real &&
                whenFalse->kind == bf6::EbxValue::Kind::Real &&
                condition->kind == bf6::EbxValue::Kind::Bool)
            {
                bf6_rime_conditional_float row{};
                row.instance = (int32_t)i;
                row.value_if_true = (float)whenTrue->f;
                row.value_if_false = (float)whenFalse->f;
                row.authored_condition = condition->b ? 1 : 0;
                c->rime_binding_conditional_floats.push_back(row);
            }
        }

        if (type_guid == kRimeFloatInterpolatorType)
        {
            const bf6::EbxValue* defaultValue =
                v.field(kRimeFieldDefaultValue);
            const bf6::EbxValue* duration = v.field(kRimeFieldDuration);
            const bf6::EbxValue* velocity = v.field(kRimeFieldVelocity);
            const bf6::EbxValue* useVelocity =
                v.field(kRimeFieldUseVelocity);
            const bf6::EbxValue* interpolationType =
                v.field(kRimeFieldInterpolationType);
            const bf6::EbxValue* interpolationMode =
                v.field(kRimeFieldInterpolationMode);
            const bf6::EbxValue* dynamicDuration =
                v.field(kRimeFieldDynamicDuration);
            const bf6::EbxValue* useRealTime =
                v.field(kRimeFieldUseRealTimeClock);
            const bf6::EbxValue* frameCorrect =
                v.field(kRimeFieldForceFrameCorrectOutput);

            std::set<uint32_t> inputs, outputs;
            for (const bf6_rime_connection& connection :
                 c->rime_binding_connections)
            {
                if (connection.target == (int32_t)i)
                    inputs.insert(connection.target_field);
                if (connection.source == (int32_t)i)
                    outputs.insert(connection.source_field);
            }
            auto integer = [](const bf6::EbxValue* value, int32_t& out) {
                if (!value) return false;
                if (value->kind == bf6::EbxValue::Kind::Int)
                { out = (int32_t)value->i; return true; }
                if (value->kind == bf6::EbxValue::Kind::Uint)
                { out = (int32_t)value->u; return true; }
                return false;
            };
            int32_t interpolationTypeValue = 0;
            int32_t interpolationModeValue = 0;
            if (defaultValue && duration && velocity && useVelocity &&
                interpolationType && interpolationMode && dynamicDuration &&
                useRealTime && frameCorrect && inputs.size() == 1 &&
                outputs.size() == 1 &&
                defaultValue->kind == bf6::EbxValue::Kind::Real &&
                duration->kind == bf6::EbxValue::Kind::Real &&
                velocity->kind == bf6::EbxValue::Kind::Real &&
                useVelocity->kind == bf6::EbxValue::Kind::Bool &&
                dynamicDuration->kind == bf6::EbxValue::Kind::Bool &&
                useRealTime->kind == bf6::EbxValue::Kind::Bool &&
                frameCorrect->kind == bf6::EbxValue::Kind::Bool &&
                integer(interpolationType, interpolationTypeValue) &&
                integer(interpolationMode, interpolationModeValue))
            {
                bf6_rime_float_interpolator row{};
                row.instance = (int32_t)i;
                row.input_field = *inputs.begin();
                row.output_field = *outputs.begin();
                row.default_value = (float)defaultValue->f;
                row.duration = (float)duration->f;
                row.velocity = (float)velocity->f;
                row.use_velocity = useVelocity->b ? 1 : 0;
                row.interpolation_type = interpolationTypeValue;
                row.interpolation_mode = interpolationModeValue;
                row.dynamic_duration = dynamicDuration->b ? 1 : 0;
                row.use_real_time_clock = useRealTime->b ? 1 : 0;
                row.force_frame_correct_output = frameCorrect->b ? 1 : 0;
                c->rime_binding_float_interpolators.push_back(row);
            }
        }

        if (type_guid == kRimeConditionalPropertyType)
        {
            const bf6::EbxValue* whenTrue =
                v.field(kRimeFieldValueIfTruePropertyHash);
            const bf6::EbxValue* whenFalse =
                v.field(kRimeFieldValueIfFalsePropertyHash);
            const bf6::EbxValue* output = v.field(kRimeFieldOutHash);
            const bf6::EbxValue* condition = v.field(kRimeFieldCondition);
            auto as_hash = [](const bf6::EbxValue* value, uint32_t& out) {
                if (!value) return false;
                if (value->kind == bf6::EbxValue::Kind::Uint)
                { out = (uint32_t)value->u; return true; }
                if (value->kind == bf6::EbxValue::Kind::Int)
                { out = (uint32_t)value->i; return true; }
                return false;
            };
            bf6_rime_conditional_property row{};
            row.instance = (int32_t)i;
            if (as_hash(whenTrue, row.value_if_true_property_hash) &&
                as_hash(whenFalse, row.value_if_false_property_hash) &&
                as_hash(output, row.out_hash) && condition &&
                condition->kind == bf6::EbxValue::Kind::Bool)
            {
                row.authored_condition = condition->b ? 1 : 0;
                c->rime_binding_conditional_properties.push_back(row);
            }
        }

        if (type_guid == kRimeStringEntityType)
        {
            if (const bf6::EbxValue* k = v.field(kRimeFieldStringKey))
            {
                const uint32_t sid = (uint32_t)(uint64_t)(
                    k->kind == bf6::EbxValue::Kind::Uint ? k->u : (uint64_t)k->i);
                if (sid) c->rime_binding_string_entities.emplace((int32_t)i, sid);
            }
            if (const bf6::EbxValue* arguments =
                    v.field(kRimeStringArgumentFieldsHash))
                if (arguments->kind == bf6::EbxValue::Kind::Array)
                    for (size_t argument = 0;
                         argument < arguments->items.size(); ++argument)
                    {
                        const bf6::EbxValue& value =
                            arguments->items[argument];
                        uint32_t field = 0;
                        if (value.kind == bf6::EbxValue::Kind::Uint)
                            field = (uint32_t)value.u;
                        else if (value.kind == bf6::EbxValue::Kind::Int)
                            field = (uint32_t)value.i;
                        if (!field) continue;
                        bool connected = false;
                        for (const bf6_rime_connection& connection :
                             c->rime_binding_connections)
                            if (connection.target == (int32_t)i &&
                                connection.target_field == field)
                            { connected = true; break; }
                        if (!connected) continue;
                        bf6_rime_string_argument row{};
                        row.instance = (int32_t)i;
                        row.index = (int32_t)argument;
                        row.field_id = field;
                        c->rime_binding_string_arguments.push_back(row);
                    }
        }

        if (type_guid == kRimeInterfaceDescriptorType)
        {
            c->rime_binding_interface_descriptors.push_back((int32_t)i);
            const bf6::EbxValue* fields = v.field(kRimeInterfaceFieldsHash);
            if (fields && fields->kind == bf6::EbxValue::Kind::Array)
                for (const bf6::EbxValue& field : fields->items)
                {
                    if (field.kind != bf6::EbxValue::Kind::Struct) continue;
                    const bf6::EbxValue* id = field.field(kRimeDataFieldIdHash);
                    const bf6::EbxValue* access =
                        field.field(kRimeDataFieldAccessTypeHash);
                    const bf6::EbxValue* boxed =
                        field.field(kRimeDataFieldBoxedValueHash);
                    if (!id || !access || !boxed) continue;
                    bf6_rime_interface_field row{};
                    row.interface_instance = (int32_t)i;
                    if (id->kind == bf6::EbxValue::Kind::Int)
                        row.field_id = (uint32_t)id->i;
                    else if (id->kind == bf6::EbxValue::Kind::Uint)
                        row.field_id = (uint32_t)id->u;
                    else continue;
                    if (access->kind == bf6::EbxValue::Kind::Int)
                        row.access_type = (int32_t)access->i;
                    else if (access->kind == bf6::EbxValue::Kind::Uint)
                        row.access_type = (int32_t)access->u;
                    else continue;
                    bf6__rime_copy_interface_value(*boxed, row);
                    c->rime_binding_interface_fields.push_back(row);
                    if (boxed->kind == bf6::EbxValue::Kind::Struct)
                    {
                        bf6_rime_interface_struct_type type_row{};
                        type_row.interface_instance = row.interface_instance;
                        type_row.field_id = row.field_id;
                        std::snprintf(type_row.type_guid,
                                      sizeof(type_row.type_guid), "%s",
                                      bf6::TypeDb::guid_str(boxed->guid).c_str());
                        c->rime_binding_interface_struct_types.push_back(type_row);
                    }
                }
        }

    }
    bf6__rime_read_event_connections(e, e.raw(),
                                     c->rime_binding_event_connections);
    bf6_rime_binding_cache rows;
    rows.connections = c->rime_binding_connections;
    rows.event_connections = c->rime_binding_event_connections;
    rows.conditional_floats = c->rime_binding_conditional_floats;
    rows.float_interpolators = c->rime_binding_float_interpolators;
    rows.conditional_properties = c->rime_binding_conditional_properties;
    rows.logic_operations = c->rime_binding_logic_operations;
    rows.property_statuses = c->rime_binding_property_statuses;
    rows.math_instructions = c->rime_binding_math_instructions;
    rows.math_operations = c->rime_binding_math_operations;
    rows.roundings = c->rime_binding_roundings;
    rows.property_casts = c->rime_binding_property_casts;
    rows.compare_floats = c->rime_binding_compare_floats;
    rows.array_elements = c->rime_binding_array_elements;
    rows.logic_references = c->rime_binding_logic_references;
    rows.interface_descriptors = c->rime_binding_interface_descriptors;
    rows.interface_fields = c->rime_binding_interface_fields;
    rows.interface_struct_types = c->rime_binding_interface_struct_types;
    rows.string_entities = c->rime_binding_string_entities;
    rows.string_arguments = c->rime_binding_string_arguments;
    c->rime_bindings_by_partition.emplace(partition, std::move(rows));
    c->rime_bindings_key = partition;
    return true;
}

int bf6_rime_connections(bf6_ctx* c, const char* partition,
                         bf6_rime_connection* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_connections.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_connections[(size_t)i];
    return total;
}

int bf6_rime_event_connections(bf6_ctx* c, const char* partition,
                               bf6_rime_event_connection* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_event_connections.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_event_connections[(size_t)i];
    return total;
}

/* Authored Rime timeline records.  These types are not part of the old Rime
 * element-schema family, but the raw path is still signature gated so a game
 * update cannot turn a moved field into a convincing float.  The offsets and
 * six equal-length CurveData arrays are independently present in the current
 * install; see the timeline census/read path in the research corpus. */
static const char* const kRimeTimelineEntityGuid =
    "732e5a78-9c5f-4119-de07-a1bc77fa5161";
static const uint32_t kRimeTimelineEntitySignature = 0x82BF5B15u;
static const char* const kRimeTimelineDataGuid =
    "31b549ed-9aa9-3914-af0d-6b7259e37a69";
static const uint32_t kRimeTimelineDataSignature = 0x61F8786Cu;
static const char* const kRimeFloatTrackGuid =
    "540b2d7d-9203-c63f-4c15-87dbc45190e3";
static const uint32_t kRimeFloatTrackSignature = 0x44235320u;
static const char* const kRimeCurveDataGuid =
    "5a6717bf-472a-bc45-e296-b174e549848f";
static const uint32_t kRimeCurveDataSignature = 0x794A3B9Du;
static const char* const kRimeLayoutTrackGuid =
    "5f99837e-b01b-dc8d-89f6-38b7d44c5dbf";
/* The shipped Rime revision differs from the newer SDK signature F259B077.
 * This value is the exact signature present on all 93 current-install UI
 * instances and agrees with bf6.exe's own registered layout. */
static const uint32_t kRimeLayoutTrackSignature = 0x44420C5Fu;
static const char* const kRimeEntityTrackGuid =
    "35e02bae-0815-be62-05d9-41a688738271";
static const uint32_t kRimeEntityTrackSignature = 0x9114A1D5u;
static const uint32_t kRimeHorizontalLayoutHash = 0x36BBCDA1u;
static const uint32_t kRimeVerticalLayoutHash = 0x8A1C220Du;
static const uint32_t kRimeEntityLinkHash = 0x652DE8FEu;
static const char* const kRimeCompareBoolGuid =
    "ac5c2cc6-a99e-ef31-bc82-82d273eac9e3";
static const uint32_t kRimeCompareBoolSignature = 0xCED276D5u;
static const uint32_t kRimeCompareBoolInputHash = 0x7C80C0CBu;
static const uint32_t kRimeCompareBoolOnTrueHash = 0xAA8E5552u;
static const uint32_t kRimeCompareBoolOnFalseHash = 0xFB1E95D9u;

static bool bf6__rime_exact_type(const bf6::Ebx& e, int instance,
                                 const char* guid, uint32_t signature)
{
    return instance >= 0 && instance < (int)e.instance_count() &&
        e.instance_signature((size_t)instance) == signature &&
        bf6::TypeDb::guid_str(e.instance_type((size_t)instance)) == guid;
}

static bool bf6__rime_scalar_array(const std::vector<uint8_t>& data,
                                   int64_t cell, int64_t& first,
                                   int32_t& count, int32_t stride)
{
    int32_t relative = 0;
    if (stride <= 0 || !bf6__rime_i32(data, cell, relative) || relative == 0)
        return false;
    first = cell + (int64_t)relative;
    if (!bf6__rime_i32(data, first - 4, count) || count < 0 || count > 1000000)
        return false;
    return first >= 0 && first + (int64_t)count * stride <= (int64_t)data.size();
}

static bool bf6__rime_load_partition(bf6_ctx* c, const char* partition,
                                     bf6::Ebx& ebx)
{
    std::string error;
    std::vector<uint8_t> raw =
        c->src.get_ebx(std::string(partition) + ".ebx", error);
    if (raw.empty()) raw = c->src.get_ebx(partition, error);
    if (raw.empty()) return false;
    ebx.set_guid_index(&c->src.armory_partition_index());
    return ebx.parse(std::move(raw), error);
}

static bool bf6__rime_curve_arrays(
    const bf6::Ebx& ebx, int curve, int64_t starts[6], int32_t& count,
    int32_t& curve_type, int32_t& weighted)
{
    if (!bf6__rime_exact_type(ebx, curve, kRimeCurveDataGuid,
                              kRimeCurveDataSignature))
        return false;
    const std::vector<uint8_t>& data = ebx.raw();
    const int64_t base = ebx.payload() + ebx.instance_offset((size_t)curve);
    static const uint32_t offsets[6] = {
        0x40, 0x28, 0x38, 0x18, 0x20, 0x30
    }; /* time, value, in-x, in-y, out-x, out-y */
    count = -1;
    for (int array = 0; array < 6; ++array)
    {
        int32_t array_count = 0;
        if (!bf6__rime_scalar_array(data, base + offsets[array], starts[array],
                                    array_count, 4) ||
            (count >= 0 && array_count != count))
            return false;
        count = array_count;
    }
    uint8_t weighted_byte = 0;
    if (!bf6__rime_i32(data, base + 0x50, curve_type) ||
        !bf6__rime_u8(data, base + 0x54, weighted_byte))
        return false;
    weighted = weighted_byte ? 1 : 0;
    return count > 0;
}

static bool bf6__rime_float_track_curve(const bf6::Ebx& ebx, int track,
                                         int& curve, uint32_t& source_pin,
                                         uint32_t& target_field)
{
    if (!bf6__rime_exact_type(ebx, track, kRimeFloatTrackGuid,
                              kRimeFloatTrackSignature))
        return false;
    const std::vector<uint8_t>& data = ebx.raw();
    const int64_t base = ebx.payload() + ebx.instance_offset((size_t)track);
    curve = bf6__rime_internal_ptr(ebx, data, base + 0x58);
    return curve >= 0 && bf6__rime_u32(data, base + 0x44, source_pin) &&
        bf6__rime_u32(data, base + 0x40, target_field) && source_pin != 0 &&
        target_field != 0;
}

static void bf6__rime_collect_float_tracks(
    const bf6::Ebx& ebx, std::vector<bf6_rime_float_track>& rows)
{
    const std::vector<uint8_t>& data = ebx.raw();
    std::vector<bf6_rime_connection> connections;
    bf6__rime_read_property_connections(ebx, data, connections);
    for (int timeline = 0; timeline < (int)ebx.instance_count(); ++timeline)
    {
        if (!bf6__rime_exact_type(ebx, timeline, kRimeTimelineEntityGuid,
                                  kRimeTimelineEntitySignature))
            continue;
        const int64_t timeline_base = ebx.payload() +
            ebx.instance_offset((size_t)timeline);
        const int timeline_data = bf6__rime_internal_ptr(
            ebx, data, timeline_base + 0x20);
        if (!bf6__rime_exact_type(ebx, timeline_data, kRimeTimelineDataGuid,
                                  kRimeTimelineDataSignature))
            continue;
        const int64_t data_base = ebx.payload() +
            ebx.instance_offset((size_t)timeline_data);
        int64_t children = 0;
        int32_t child_count = 0;
        if (!bf6__rime_array(data, data_base + 0x38, children, child_count))
            continue;
        for (int32_t child = 0; child < child_count; ++child)
        {
            const int track = bf6__rime_internal_ptr(
                ebx, data, children + (int64_t)child * 8);
            int curve = -1;
            uint32_t source_pin = 0, target_field = 0;
            if (!bf6__rime_float_track_curve(
                    ebx, track, curve, source_pin, target_field))
                continue;
            int64_t arrays[6]{};
            int32_t keys = 0, curve_type = 0, weighted = 0;
            if (!bf6__rime_curve_arrays(
                    ebx, curve, arrays, keys, curve_type, weighted))
                continue;
            int target = -1;
            int exact_matches = 0;
            for (const bf6_rime_connection& connection : connections)
                if (connection.source == timeline &&
                    connection.source_field == source_pin &&
                    connection.target_field == target_field)
                {
                    target = connection.target;
                    ++exact_matches;
                }
            if (exact_matches != 1) continue;
            bf6_rime_float_track row{};
            row.timeline_instance = timeline;
            row.timeline_data_instance = timeline_data;
            row.track_instance = track;
            row.curve_instance = curve;
            row.target_instance = target;
            row.source_pin = source_pin;
            row.target_field = target_field;
            row.curve_type = curve_type;
            row.is_weighted = weighted;
            row.key_count = keys;
            if (!bf6__rime_f32(data, arrays[0], row.first_time) ||
                !bf6__rime_f32(data, arrays[0] + (int64_t)(keys - 1) * 4,
                               row.last_time))
                continue;
            rows.push_back(row);
        }
    }
}

int bf6_rime_float_tracks(bf6_ctx* c, const char* partition,
                          bf6_rime_float_track* out, int out_max)
{
    if (!c || !partition || !*partition || out_max < 0) return -1;
    std::string type_error;
    if (!ensure_types(c, type_error)) return -1;
    bf6::Ebx ebx(*c->types);
    if (!bf6__rime_load_partition(c, partition, ebx)) return -1;
    std::vector<bf6_rime_float_track> rows;
    bf6__rime_collect_float_tracks(ebx, rows);
    if (out && out_max > 0)
        for (int index = 0; index < (int)rows.size() && index < out_max; ++index)
            out[index] = rows[(size_t)index];
    return (int)rows.size();
}

int bf6_rime_float_track_keys(bf6_ctx* c, const char* partition,
                              int32_t track_instance,
                              bf6_rime_float_key* out, int out_max)
{
    if (!c || !partition || !*partition || track_instance < 0 || out_max < 0)
        return -1;
    std::string type_error;
    if (!ensure_types(c, type_error)) return -1;
    bf6::Ebx ebx(*c->types);
    if (!bf6__rime_load_partition(c, partition, ebx)) return -1;
    int curve = -1;
    uint32_t source_pin = 0, target_field = 0;
    if (!bf6__rime_float_track_curve(
            ebx, track_instance, curve, source_pin, target_field))
        return -1;
    int64_t arrays[6]{};
    int32_t count = 0, curve_type = 0, weighted = 0;
    if (!bf6__rime_curve_arrays(
            ebx, curve, arrays, count, curve_type, weighted))
        return -1;
    if (out && out_max > 0)
        for (int32_t index = 0; index < count && index < out_max; ++index)
        {
            float* fields[6] = {
                &out[index].time, &out[index].value,
                &out[index].in_tangent_x, &out[index].in_tangent_y,
                &out[index].out_tangent_x, &out[index].out_tangent_y
            };
            for (int field = 0; field < 6; ++field)
                if (!bf6__rime_f32(ebx.raw(), arrays[field] +
                                    (int64_t)index * 4, *fields[field]))
                    return -1;
        }
    return count;
}

static bool bf6__rime_layout_value(const std::vector<uint8_t>& data,
                                   int64_t cell, bf6_rime_axis& axis)
{
    /* Shipped RimeAxisLayoutData order is not the public bf6_rime_axis order:
     * AnchorStart, SizingWeight, SizingPivot, OffsetEnd, AnchorEnd,
     * OffsetStart. */
    const bool read = bf6__rime_f32(data, cell + 0x00, axis.anchor_start) &&
        bf6__rime_f32(data, cell + 0x10, axis.anchor_end) &&
        bf6__rime_f32(data, cell + 0x14, axis.offset_start) &&
        bf6__rime_f32(data, cell + 0x0C, axis.offset_end) &&
        bf6__rime_f32(data, cell + 0x08, axis.pivot) &&
        bf6__rime_f32(data, cell + 0x04, axis.weight);
    return read && std::isfinite(axis.anchor_start) &&
        std::isfinite(axis.anchor_end) && std::isfinite(axis.offset_start) &&
        std::isfinite(axis.offset_end) && std::isfinite(axis.pivot) &&
        std::isfinite(axis.weight);
}

static bool bf6__rime_layout_track(
    const bf6::Ebx& ebx, int track, bf6_rime_layout_track& row,
    int64_t& keyframes)
{
    if (!bf6__rime_exact_type(ebx, track, kRimeLayoutTrackGuid,
                              kRimeLayoutTrackSignature))
        return false;
    const std::vector<uint8_t>& data = ebx.raw();
    const int64_t base = ebx.payload() + ebx.instance_offset((size_t)track);
    int32_t count = 0;
    uint8_t property_flag = 0;
    if (!bf6__rime_u32(data, base + 0x40, row.target_field) ||
        !bf6__rime_u32(data, base + 0x44, row.source_pin) ||
        !bf6__rime_u32(data, base + 0x48, row.target_pin) ||
        !bf6__rime_i32(data, base + 0x50, row.end_rule) ||
        !bf6__rime_u8(data, base + 0x54, property_flag) ||
        !bf6__rime_scalar_array(data, base + 0x58, keyframes, count, 36) ||
        !bf6__rime_layout_value(data, base + 0x60, row.default_layout))
        return false;
    if ((row.target_field != kRimeHorizontalLayoutHash &&
         row.target_field != kRimeVerticalLayoutHash) ||
        property_flag > 1 || row.end_rule < 0 || row.end_rule > 1 ||
        count > 4096)
        return false;
    row.timeline_instance = -1;
    row.timeline_data_instance = -1;
    row.entity_track_instance = -1;
    row.target_instance = -1;
    row.track_instance = track;
    row.property_track_flag = property_flag;
    row.key_count = count;
    row.first_time = 0.f;
    row.last_time = 0.f;
    float previous = 0.f;
    for (int32_t index = 0; index < count; ++index)
    {
        const int64_t cell = keyframes + (int64_t)index * 36;
        bf6_rime_axis value{};
        uint32_t interpolation_type = 0, interpolation_mode = 0;
        float time = 0.f;
        if (!bf6__rime_layout_value(data, cell, value) ||
            !bf6__rime_u32(data, cell + 0x18, interpolation_type) ||
            !bf6__rime_u32(data, cell + 0x1C, interpolation_mode) ||
            !bf6__rime_f32(data, cell + 0x20, time) ||
            interpolation_type > 11 || interpolation_mode > 3 ||
            !std::isfinite(time) || (index > 0 && time < previous))
            return false;
        if (index == 0) row.first_time = time;
        row.last_time = time;
        previous = time;
    }
    return true;
}

static int bf6__rime_array_pointer_matches(
    const bf6::Ebx& ebx, int owner, int64_t array_offset, int target)
{
    if (owner < 0 || owner >= (int)ebx.instance_count()) return 0;
    const int64_t base = ebx.payload() + ebx.instance_offset((size_t)owner);
    int64_t entries = 0;
    int32_t count = 0;
    if (!bf6__rime_array(ebx.raw(), base + array_offset, entries, count))
        return 0;
    int matches = 0;
    for (int32_t index = 0; index < count; ++index)
        matches += bf6__rime_internal_ptr(
            ebx, ebx.raw(), entries + (int64_t)index * 8) == target;
    return matches;
}

static void bf6__rime_layout_ownership(
    const bf6::Ebx& ebx,
    const std::vector<bf6__rime_local_link_connection>& links,
    bf6_rime_layout_track& row)
{
    int entity_matches = 0;
    for (int instance = 0; instance < (int)ebx.instance_count(); ++instance)
        if (bf6__rime_exact_type(ebx, instance, kRimeEntityTrackGuid,
                                 kRimeEntityTrackSignature) &&
            bf6__rime_array_pointer_matches(ebx, instance, 0x48,
                                            row.track_instance) == 1)
        {
            row.entity_track_instance = instance;
            ++entity_matches;
        }
    if (entity_matches != 1) {
        row.entity_track_instance = -1;
        return;
    }

    int data_matches = 0;
    for (int instance = 0; instance < (int)ebx.instance_count(); ++instance)
        if (bf6__rime_exact_type(ebx, instance, kRimeTimelineDataGuid,
                                 kRimeTimelineDataSignature) &&
            bf6__rime_array_pointer_matches(ebx, instance, 0x38,
                                            row.entity_track_instance) == 1)
        {
            row.timeline_data_instance = instance;
            ++data_matches;
        }
    if (data_matches != 1) {
        row.timeline_data_instance = -1;
        return;
    }

    int timeline_matches = 0;
    for (int instance = 0; instance < (int)ebx.instance_count(); ++instance)
    {
        if (!bf6__rime_exact_type(ebx, instance, kRimeTimelineEntityGuid,
                                  kRimeTimelineEntitySignature))
            continue;
        const int64_t base = ebx.payload() +
            ebx.instance_offset((size_t)instance);
        if (bf6__rime_internal_ptr(ebx, ebx.raw(), base + 0x20) ==
            row.timeline_data_instance)
        {
            row.timeline_instance = instance;
            ++timeline_matches;
        }
    }
    if (timeline_matches != 1) row.timeline_instance = -1;

    int target_matches = 0;
    for (const bf6__rime_local_link_connection& link : links)
        if (link.source == row.entity_track_instance &&
            link.source_field == kRimeEntityLinkHash &&
            link.target_field == 0)
        {
            row.target_instance = link.target;
            ++target_matches;
        }
    if (target_matches != 1) row.target_instance = -1;
}

int bf6_rime_layout_tracks(bf6_ctx* c, const char* partition,
                           bf6_rime_layout_track* out, int out_max)
{
    if (!c || !partition || !*partition || out_max < 0) return -1;
    std::string type_error;
    if (!ensure_types(c, type_error)) return -1;
    bf6::Ebx ebx(*c->types);
    if (!bf6__rime_load_partition(c, partition, ebx)) return -1;
    std::vector<bf6_rime_layout_track> rows;
    std::vector<bf6__rime_local_link_connection> links;
    bf6__rime_read_local_link_connections(ebx, ebx.raw(), links);
    for (int instance = 0; instance < (int)ebx.instance_count(); ++instance)
    {
        bf6_rime_layout_track row{};
        int64_t keyframes = 0;
        if (bf6__rime_layout_track(ebx, instance, row, keyframes))
        {
            bf6__rime_layout_ownership(ebx, links, row);
            rows.push_back(row);
        }
    }
    if (out && out_max > 0)
        for (int index = 0; index < (int)rows.size() && index < out_max; ++index)
            out[index] = rows[(size_t)index];
    return (int)rows.size();
}

int bf6_rime_layout_track_keys(bf6_ctx* c, const char* partition,
                               int32_t track_instance,
                               bf6_rime_layout_key* out, int out_max)
{
    if (!c || !partition || !*partition || track_instance < 0 || out_max < 0)
        return -1;
    std::string type_error;
    if (!ensure_types(c, type_error)) return -1;
    bf6::Ebx ebx(*c->types);
    if (!bf6__rime_load_partition(c, partition, ebx)) return -1;
    bf6_rime_layout_track track{};
    int64_t keyframes = 0;
    if (!bf6__rime_layout_track(ebx, track_instance, track, keyframes))
        return -1;
    if (out && out_max > 0)
        for (int32_t index = 0; index < track.key_count && index < out_max;
             ++index)
        {
            const int64_t cell = keyframes + (int64_t)index * 36;
            uint32_t interpolation_type = 0, interpolation_mode = 0;
            if (!bf6__rime_layout_value(ebx.raw(), cell, out[index].layout) ||
                !bf6__rime_u32(ebx.raw(), cell + 0x18,
                               interpolation_type) ||
                !bf6__rime_u32(ebx.raw(), cell + 0x1C,
                               interpolation_mode) ||
                !bf6__rime_f32(ebx.raw(), cell + 0x20, out[index].time) ||
                interpolation_type > 11 || interpolation_mode > 3 ||
                !std::isfinite(out[index].time))
                return -1;
            out[index].interpolation_type = (int32_t)interpolation_type;
            out[index].interpolation_mode = (int32_t)interpolation_mode;
            if (index > 0 && out[index].time < out[index - 1].time)
                return -1;
        }
    return track.key_count;
}

static bool bf6__rime_timeline(
    const bf6::Ebx& ebx, int instance, bf6_rime_timeline& row)
{
    if (!bf6__rime_exact_type(ebx, instance, kRimeTimelineEntityGuid,
                              kRimeTimelineEntitySignature))
        return false;
    const std::vector<uint8_t>& data = ebx.raw();
    const int64_t base = ebx.payload() + ebx.instance_offset((size_t)instance);
    row.timeline_instance = instance;
    row.timeline_data_instance = bf6__rime_internal_ptr(
        ebx, data, base + 0x20);
    if (!bf6__rime_exact_type(ebx, row.timeline_data_instance,
                              kRimeTimelineDataGuid,
                              kRimeTimelineDataSignature))
        return false;
    const std::string debug_name = bf6__rime_cstr(data, base + 0x28);
    uint32_t end_rule = 0, realm = 0, autoplay = 0, clock = 0;
    uint8_t flags[5]{};
    if (!bf6__rime_u32(data, base + 0x30, end_rule) ||
        !bf6__rime_u32(data, base + 0x38, realm) ||
        !bf6__rime_u32(data, base + 0x50, autoplay) ||
        !bf6__rime_u32(data, base + 0x54, clock) ||
        !bf6__rime_f32(data, base + 0x58, row.start_time) ||
        !bf6__rime_f32(data, base + 0x5C, row.init_time) ||
        !bf6__rime_f32(data, base + 0x60,
                       row.encoded_runtime_framerate) ||
        !bf6__rime_f32(data, base + 0x64, row.end_time))
        return false;
    for (int flag = 0; flag < 5; ++flag)
        if (!bf6__rime_u8(data, base + 0x6F + flag, flags[flag]) ||
            flags[flag] > 1)
            return false;
    if (end_rule > 4 || autoplay > 3 || clock > 2 ||
        !std::isfinite(row.start_time) || !std::isfinite(row.init_time) ||
        !std::isfinite(row.encoded_runtime_framerate) ||
        !std::isfinite(row.end_time) || row.end_time < row.start_time)
        return false;
    std::snprintf(row.debug_name, sizeof(row.debug_name), "%s",
                  debug_name.c_str());
    row.end_rule = (int32_t)end_rule;
    row.realm = (int32_t)realm;
    row.autoplay_option = (int32_t)autoplay;
    row.delta_time_clock = (int32_t)clock;
    row.sync_timelines = flags[0];
    row.predicted_timelines = flags[1];
    row.print_current_time = flags[2];
    row.allow_animation_carry_forward = flags[3];
    row.reset_time_on_started = flags[4];
    return true;
}

int bf6_rime_timelines(bf6_ctx* c, const char* partition,
                       bf6_rime_timeline* out, int out_max)
{
    if (!c || !partition || !*partition || out_max < 0) return -1;
    std::string type_error;
    if (!ensure_types(c, type_error)) return -1;
    bf6::Ebx ebx(*c->types);
    if (!bf6__rime_load_partition(c, partition, ebx)) return -1;
    std::vector<bf6_rime_timeline> rows;
    for (int instance = 0; instance < (int)ebx.instance_count(); ++instance)
    {
        bf6_rime_timeline row{};
        if (bf6__rime_timeline(ebx, instance, row)) rows.push_back(row);
    }
    if (out && out_max > 0)
        for (int index = 0; index < (int)rows.size() && index < out_max; ++index)
            out[index] = rows[(size_t)index];
    return (int)rows.size();
}

int bf6_rime_compare_bools(bf6_ctx* c, const char* partition,
                           bf6_rime_compare_bool* out, int out_max)
{
    if (!c || !partition || !*partition || out_max < 0) return -1;
    std::string type_error;
    if (!ensure_types(c, type_error)) return -1;
    bf6::Ebx ebx(*c->types);
    if (!bf6__rime_load_partition(c, partition, ebx)) return -1;
    std::vector<bf6_rime_compare_bool> rows;
    for (int instance = 0; instance < (int)ebx.instance_count(); ++instance)
    {
        if (!bf6__rime_exact_type(ebx, instance, kRimeCompareBoolGuid,
                                  kRimeCompareBoolSignature))
            continue;
        const int64_t base = ebx.payload() +
            ebx.instance_offset((size_t)instance);
        uint32_t realm = 0;
        uint8_t always_send = 0, on_change = 0, on_start = 0, value = 0;
        if (!bf6__rime_u32(ebx.raw(), base + 0x20, realm) || realm > 3 ||
            /* Shipped CompareEntityBaseData stores TriggerOnStart,
             * AlwaysSend, TriggerOnPropertyChange in this physical order. */
            !bf6__rime_u8(ebx.raw(), base + 0x24, on_start) ||
            !bf6__rime_u8(ebx.raw(), base + 0x25, always_send) ||
            !bf6__rime_u8(ebx.raw(), base + 0x26, on_change) ||
            !bf6__rime_u8(ebx.raw(), base + 0x28, value) ||
            always_send > 1 || on_change > 1 || on_start > 1 || value > 1)
            continue;
        bf6_rime_compare_bool row{};
        row.instance = instance;
        row.input_field = kRimeCompareBoolInputHash;
        row.on_true_event = kRimeCompareBoolOnTrueHash;
        row.on_false_event = kRimeCompareBoolOnFalseHash;
        row.authored_value = value;
        row.always_send = always_send;
        row.trigger_on_property_change = on_change;
        row.trigger_on_start = on_start;
        rows.push_back(row);
    }
    if (out && out_max > 0)
        for (int index = 0; index < (int)rows.size() && index < out_max; ++index)
            out[index] = rows[(size_t)index];
    return (int)rows.size();
}

int bf6_rime_conditional_floats(bf6_ctx* c, const char* partition,
                                bf6_rime_conditional_float* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_conditional_floats.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_conditional_floats[(size_t)i];
    return total;
}

int bf6_rime_float_interpolators(
    bf6_ctx* c, const char* partition,
    bf6_rime_float_interpolator* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_float_interpolators.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_float_interpolators[(size_t)i];
    return total;
}

int bf6_rime_conditional_properties(
    bf6_ctx* c, const char* partition,
    bf6_rime_conditional_property* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_conditional_properties.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_conditional_properties[(size_t)i];
    return total;
}

int bf6_rime_logic_operations(
    bf6_ctx* c, const char* partition,
    bf6_rime_logic_operation* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_logic_operations.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_logic_operations[(size_t)i];
    return total;
}

int bf6_rime_property_statuses(
    bf6_ctx* c, const char* partition,
    bf6_rime_property_status* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_property_statuses.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_property_statuses[(size_t)i];
    return total;
}

int bf6_rime_math_instructions(
    bf6_ctx* c, const char* partition,
    bf6_rime_math_instruction* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_math_instructions.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_math_instructions[(size_t)i];
    return total;
}

int bf6_rime_math_operations(
    bf6_ctx* c, const char* partition,
    bf6_rime_math_operation* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_math_operations.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_math_operations[(size_t)i];
    return total;
}

int bf6_rime_roundings(
    bf6_ctx* c, const char* partition,
    bf6_rime_rounding* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_roundings.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_roundings[(size_t)i];
    return total;
}

int bf6_rime_property_casts(
    bf6_ctx* c, const char* partition,
    bf6_rime_property_cast* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_property_casts.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_property_casts[(size_t)i];
    return total;
}

int bf6_rime_compare_floats(
    bf6_ctx* c, const char* partition,
    bf6_rime_compare_float* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_compare_floats.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_compare_floats[(size_t)i];
    return total;
}

int bf6_rime_array_elements(
    bf6_ctx* c, const char* partition,
    bf6_rime_array_element* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_array_elements.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_array_elements[(size_t)i];
    return total;
}

int bf6_rime_logic_references(
    bf6_ctx* c, const char* partition,
    bf6_rime_logic_reference* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_logic_references.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_logic_references[(size_t)i];
    return total;
}

int bf6_rime_interface_descriptors(bf6_ctx* c, const char* partition,
                                   int32_t* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_interface_descriptors.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_interface_descriptors[(size_t)i];
    return total;
}

int bf6_rime_interface_fields(bf6_ctx* c, const char* partition,
                              bf6_rime_interface_field* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_interface_fields.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_interface_fields[(size_t)i];
    return total;
}

int bf6_rime_interface_struct_types(
    bf6_ctx* c, const char* partition,
    bf6_rime_interface_struct_type* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_interface_struct_types.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_interface_struct_types[(size_t)i];
    return total;
}

uint32_t bf6_rime_string_entity(bf6_ctx* c, const char* partition, int instance)
{
    if (!c || !partition || instance < 0) return 0;
    if (!bf6__rime_load_bindings(c, partition)) return 0;
    const auto it = c->rime_binding_string_entities.find(instance);
    return it == c->rime_binding_string_entities.end() ? 0 : it->second;
}

int bf6_rime_string_arguments(
    bf6_ctx* c, const char* partition,
    bf6_rime_string_argument* out, int out_max)
{
    if (!c || !partition) return -1;
    if (!bf6__rime_load_bindings(c, partition)) return -1;
    const int total = (int)c->rime_binding_string_arguments.size();
    if (out && out_max > 0)
        for (int i = 0; i < total && i < out_max; ++i)
            out[i] = c->rime_binding_string_arguments[(size_t)i];
    return total;
}
