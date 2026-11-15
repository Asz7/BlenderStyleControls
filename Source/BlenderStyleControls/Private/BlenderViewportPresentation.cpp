#include "BlenderViewportPresentation.h"
#include "Brushes/SlateImageBrush.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Interfaces/IPluginManager.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "SceneView.h"
#include "SceneManagement.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SSeparator.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "LevelEditorViewport.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAdd.h"
#include "MaterialShared.h"
#include "Rendering/DrawElements.h"
#include "SEditorViewport.h"
#include "SLevelViewport.h"
#include "Styling/AppStyle.h"
#include "ToolSetupUtil.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"

namespace BlenderViewportPresentationPrivate
{
	// Slate owns copies of draw data, but brush storage must outlive all widgets.
	const FSlateRoundedBoxBrush ToolbarBrush(FLinearColor(0.025f, 0.029f, 0.036f, 0.94f), 6.0f);
	const FSlateRoundedBoxBrush ButtonBrush(FLinearColor::White, 4.0f);
	const FSlateRoundedBoxBrush SphereBrush(FLinearColor::White, 8.0f);

	/** Small vector glyphs keep all four shading buttons circular, independent of editor icon themes. */
	class SShadingGlyph : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SShadingGlyph) : _Kind(0) {}
			SLATE_ARGUMENT(int32, Kind)
		SLATE_END_ARGS()

		void Construct(const FArguments& Args) { Kind = Args._Kind; }
		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(22.0, 22.0); }

		virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
			FSlateWindowElementList& Elements, int32 Layer, const FWidgetStyle& Style,
			bool bParentEnabled) const override
		{
			const FLinearColor Tint = Style.GetColorAndOpacityTint() *
				(bParentEnabled ? FLinearColor(0.86f, 0.89f, 0.94f) : FLinearColor(0.35f, 0.37f, 0.4f));
			auto Line = [&](const TArray<FVector2f>& Points, FLinearColor Color, float Width = 1.1f)
			{
				FSlateDrawElement::MakeLines(Elements, Layer + 2, Geometry.ToPaintGeometry(), Points,
					ESlateDrawEffect::None, Color, true, Width);
			};
			auto Ellipse = [&](FVector2f Center, FVector2f Radius, FLinearColor Color)
			{
				TArray<FVector2f> Points;
				for (int32 I = 0; I <= 32; ++I)
				{
					const double Angle = 2.0 * PI * I / 32.0;
					Points.Add(Center + FVector2f(FMath::Cos(Angle) * Radius.X, FMath::Sin(Angle) * Radius.Y));
				}
				Line(Points, Color);
			};
			if (Kind == 4)
			{
				Line({ FVector2f(4, 4), FVector2f(14, 4), FVector2f(14, 14), FVector2f(4, 14), FVector2f(4, 4) }, Tint * 0.65f);
				Line({ FVector2f(8, 8), FVector2f(18, 8), FVector2f(18, 18), FVector2f(8, 18), FVector2f(8, 8) }, Tint);
				return Layer + 2;
			}
			if (Kind != 0)
			{
				FSlateDrawElement::MakeBox(Elements, Layer, Geometry.ToPaintGeometry(
					FVector2f(16, 16), FSlateLayoutTransform(FVector2f(3, 3))), &SphereBrush,
					ESlateDrawEffect::None, Tint * (Kind == 1 ? 0.82f : 0.34f));
				if (Kind >= 2)
				{
					// A soft lit cap distinguishes material/rendered from the plain solid sphere.
					FSlateDrawElement::MakeBox(Elements, Layer + 1, Geometry.ToPaintGeometry(
						FVector2f(10, 11), FSlateLayoutTransform(FVector2f(4, 3))), &SphereBrush,
						ESlateDrawEffect::None, Tint * 0.88f);
				}
			}
			Ellipse(FVector2f(11, 11), FVector2f(8, 8), Tint);
			if (Kind == 0)
			{
				Ellipse(FVector2f(11, 11), FVector2f(3.5f, 8), Tint);
				Ellipse(FVector2f(11, 11), FVector2f(8, 3.5f), Tint);
			}
			else if (Kind == 3)
			{
				Line({ FVector2f(15, 1), FVector2f(15, 7) }, FLinearColor::White, 1.4f);
				Line({ FVector2f(12, 4), FVector2f(18, 4) }, FLinearColor::White, 1.4f);
			}
			return Layer + 2;
		}
	private:
		int32 Kind = 0;
	};
}

struct FBlenderViewportPresentation::FImpl
{
	struct FViewportState
	{
		FEditorViewportClient* Client = nullptr;
		TWeakPtr<SLevelViewport> Widget;
		TWeakPtr<SWidget> Overlay;
		EViewModeIndex PerspectiveMode = VMI_Lit;
		EViewModeIndex OrthographicMode = VMI_Wireframe;
		FEngineShowFlags ShowFlags = FEngineShowFlags(ESFIM_Editor);
		FEngineShowFlags LastShowFlags = FEngineShowFlags(ESFIM_Editor);
	};

	FBlenderViewportPresentation* Owner = nullptr;
	TArray<FViewportState> Viewports;
	TSharedPtr<int32> CallbackLifetime = MakeShared<int32>(0);
	TStrongObjectPtr<UMaterialInterface> SolidMaterial;
	TStrongObjectPtr<UMaterialInterface> XRayMaterial;
	TFunction<void()> OnChanged;
	TUniquePtr<FBlenderPreviewLighting> Lighting = MakeUnique<FBlenderPreviewLighting>();
	FBlenderPreviewLightingSettings PreviewSettings;
	FBlenderViewportOverlaySettings OverlaySettings;
	TMap<FString, TSharedPtr<FSlateImageBrush>> ThumbnailBrushes;
	struct FWireMesh { const void* RenderData = nullptr; TArray<TPair<FVector, FVector>> Edges; };
	TMap<TWeakObjectPtr<UStaticMesh>, FWireMesh> WireCache;
	EBlenderViewportShading Shading = EBlenderViewportShading::Solid;
	bool bActive = false;
	bool bMeshEditActive = false;
	bool bWireXRay = true;
	bool bSolidXRay = false;

	bool IsLive(const FViewportState& State) const
	{
		if (!GEditor || !State.Client) { return false; }
		// Do not dereference a saved client until its address is in the current editor list.
		for (FLevelEditorViewportClient* Live : GEditor->GetLevelViewportClients())
		{
			if (Live == State.Client)
			{
				const TSharedPtr<SLevelViewport> Widget = State.Widget.Pin();
				return Widget.IsValid() && Live->GetEditorViewportWidget().Get() == Widget.Get();
			}
		}
		return false;
	}

	EViewModeIndex ViewMode() const
	{
		if (Shading == EBlenderViewportShading::Wireframe) { return VMI_Wireframe; }
		return VMI_Lit;
	}

	void Apply(FViewportState& State)
	{
		if (!IsLive(State)) { return; }
		FEditorViewportClient& Client = *State.Client;
		// Always start from the saved flags, never from another shading mode's temporary flags.
		Client.EngineShowFlags = State.ShowFlags;
		Client.LastEngineShowFlags = State.LastShowFlags;
		Client.SetViewModes(ViewMode(), ViewMode());
		// Legacy mode HUD (including the selection rectangle) is gated by this flag.
		Client.EngineShowFlags.SetModeWidgets(true);
		Client.EngineShowFlags.SetCompositeEditorPrimitives(true);
		const bool bSolid = Shading == EBlenderViewportShading::Solid;
		// Object solid uses the engine's default untextured material. During editing, preserve
		// material drawing so the transient sculpt/transparent preview material is respected.
		Client.EngineShowFlags.SetMaterials(true);
		Client.EngineShowFlags.SetLightingOnlyOverride(false);
		// ApplyViewMode does not immediately restore Lighting when the saved viewport
		// started unlit; rendered mode must establish its own lighting state explicitly.
		Client.EngineShowFlags.SetLighting(Shading != EBlenderViewportShading::Wireframe);
		if (Shading != EBlenderViewportShading::Rendered)
		{
			Client.EngineShowFlags.SetPostProcessing(Shading != EBlenderViewportShading::Wireframe);
			if (Shading != EBlenderViewportShading::Material || !PreviewSettings.bUseSceneWorld)
			{
				Client.EngineShowFlags.SetFog(false);
				Client.EngineShowFlags.SetAtmosphere(false);
			}
		}
		// This is an object-mode aid, not scene-wide transparent-material replacement.
		const bool bOverlays = OverlaySettings.bShowOverlays;
		Client.EngineShowFlags.SetMeshEdges(bOverlays && (State.ShowFlags.MeshEdges || (bSolid && Owner->IsXRay() && !bMeshEditActive)));
		Client.EngineShowFlags.SetGrid(bOverlays && OverlaySettings.bGrid);
		Client.EngineShowFlags.SetSelectionOutline(bOverlays && OverlaySettings.bSelectionOutline);
		Client.EngineShowFlags.SetSelection(bOverlays && OverlaySettings.bSelectionOutline);
		Client.EngineShowFlags.SetBillboardSprites(bOverlays && OverlaySettings.bExtras);
		Client.Invalidate();
	}

	void ApplyAll()
	{
		UpdateLighting();
		for (FViewportState& State : Viewports) { Apply(State); }
		if (OnChanged) { OnChanged(); }
	}

	void UpdateLighting()
	{
		PreviewSettings.Shading = int32(Shading);
		PreviewSettings.bXRay = Owner->IsXRay();
		TArray<FEditorViewportClient*> Clients;
		for (const auto& State : Viewports) if (IsLive(State)) Clients.Add(State.Client);
		Lighting->SetViewports(Clients);
		Lighting->SetSettings(PreviewSettings);
		Lighting->SetEnabled(bActive);
	}

	const FSlateBrush* Thumbnail(const FString& Group, const FString& Stem)
	{
		const FString Key = Group + TEXT("/") + Stem;
		if (auto* Found = ThumbnailBrushes.Find(Key)) return Found->Get();
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("BlenderStyleControls"));
		if (!Plugin) return FAppStyle::GetBrush("WhiteBrush");
		const FString File = Plugin->GetBaseDir() / TEXT("Resources/PreviewV14") / Group / (Stem + TEXT(".png"));
		return ThumbnailBrushes.Add(Key, MakeShared<FSlateImageBrush>(File, FVector2D(52, 52))).Get();
	}

	TSharedRef<SWidget> MakeSettingsPanel(bool bOverlays)
	{
		const TWeakPtr<int32> Lifetime = CallbackLifetime;
		TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
		auto Label = [&](const TCHAR* Text)
		{
			Rows->AddSlot().AutoHeight().Padding(0, 8, 0, 5)
			[ SNew(STextBlock).Text(FText::FromString(Text)).Font(FAppStyle::GetFontStyle("BoldFont")) ];
		};
		auto Toggle = [&](const TCHAR* Text, TFunction<bool()> Get, TFunction<void(bool)> Set)
		{
			Rows->AddSlot().AutoHeight().Padding(0, 3)
			[
				SNew(SCheckBox).IsChecked_Lambda([Lifetime, Get] { return Lifetime.IsValid() && Get() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Lifetime, Set](ECheckBoxState State) { if (Lifetime.IsValid()) Set(State == ECheckBoxState::Checked); })
				[ SNew(STextBlock).Text(FText::FromString(Text)) ]
			];
		};
		auto Number = [&](const TCHAR* Text, float Min, float Max, float Delta, TFunction<float()> Get, TFunction<void(float)> Set)
		{
			Rows->AddSlot().AutoHeight().Padding(0, 3)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center) [ SNew(STextBlock).Text(FText::FromString(Text)) ]
				+ SHorizontalBox::Slot().AutoWidth()
				[ SNew(SBox).WidthOverride(124)
					[ SNew(SSpinBox<float>).MinValue(Min).MaxValue(Max).MinSliderValue(Min).MaxSliderValue(Max).Delta(Delta)
					.Value_Lambda([Lifetime, Get] { return Lifetime.IsValid() ? Get() : 0.f; })
					.OnValueChanged_Lambda([Lifetime, Set](float Value) { if (Lifetime.IsValid()) Set(Value); }) ] ]
			];
		};
		auto Choices = [&](const TArray<FString>& Names, TFunction<int32()> Get, TFunction<void(int32)> Set)
		{
			TSharedRef<SHorizontalBox> Bar = SNew(SHorizontalBox);
			for (int32 Index = 0; Index < Names.Num(); ++Index)
				Bar->AddSlot().FillWidth(1).Padding(1)
				[ SNew(SButton).IsFocusable(false)
					.ButtonColorAndOpacity_Lambda([Lifetime, Get, Index] { return Lifetime.IsValid() && Get() == Index ? FLinearColor(.18f,.40f,.65f) : FLinearColor(.2f,.2f,.2f); })
					.OnClicked_Lambda([Lifetime, Set, Index] { if (Lifetime.IsValid()) Set(Index); return FReply::Handled(); })
					[ SNew(STextBlock).Text(FText::FromString(Names[Index])).Justification(ETextJustify::Center) ] ];
			Rows->AddSlot().AutoHeight().Padding(0, 3)[Bar];
		};
		auto AssetGrid = [&](bool bWorld)
		{
			const TArray<FString> Stems = bWorld ? TArray<FString>{TEXT("city"),TEXT("courtyard"),TEXT("forest"),TEXT("interior"),TEXT("night"),TEXT("studio"),TEXT("sunrise"),TEXT("sunset")} :
				TArray<FString>{TEXT("basic_1"),TEXT("basic_2"),TEXT("basic_dark"),TEXT("basic_side"),TEXT("ceramic_dark"),TEXT("ceramic_lightbulb"),TEXT("check_normal_plus_y"),TEXT("check_rim_dark"),TEXT("check_rim_light"),TEXT("clay_brown"),TEXT("clay_muddy"),TEXT("clay_studio"),TEXT("jade"),TEXT("metal_anisotropic"),TEXT("metal_carpaint"),TEXT("metal_lead"),TEXT("metal_shiny"),TEXT("pearl"),TEXT("reflection_check_horizontal"),TEXT("reflection_check_vertical"),TEXT("resin"),TEXT("skin"),TEXT("toon")};
			TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(2);
			for (int32 Index = 0; Index < Stems.Num(); ++Index)
			{
				const FString Stem = Stems[Index];
				Grid->AddSlot(Index % 4, Index / 4)
				[ SNew(SButton).IsFocusable(false).ContentPadding(2).ToolTipText(FText::FromString(Stem))
					.ButtonColorAndOpacity_Lambda([this, Lifetime, bWorld, Stem] { return Lifetime.IsValid() && (bWorld ? PreviewSettings.WorldStem : PreviewSettings.MatCapStem) == Stem ? FLinearColor(.18f,.40f,.65f) : FLinearColor(.15f,.15f,.15f); })
					.OnClicked_Lambda([this, Lifetime, bWorld, Stem] { if (Lifetime.IsValid()) { if (bWorld) PreviewSettings.WorldStem = Stem; else { PreviewSettings.MatCapStem = Stem; PreviewSettings.SolidMode = 1; } ApplyAll(); } return FReply::Handled(); })
					[ SNew(SImage).Image(Thumbnail(bWorld ? TEXT("world") : TEXT("matcap"), Stem)) ] ];
			}
			Rows->AddSlot().AutoHeight()[Grid];
		};
		if (bOverlays)
		{
			Label(TEXT("视图叠加层"));
			Toggle(TEXT("显示叠加层"), [this] { return OverlaySettings.bShowOverlays; }, [this](bool V) { OverlaySettings.bShowOverlays=V; ApplyAll(); });
			Toggle(TEXT("地面网格与轴线"), [this] { return OverlaySettings.bGrid; }, [this](bool V) { OverlaySettings.bGrid=V; ApplyAll(); });
			Toggle(TEXT("所选物体轮廓"), [this] { return OverlaySettings.bSelectionOutline; }, [this](bool V) { OverlaySettings.bSelectionOutline=V; ApplyAll(); });
			Toggle(TEXT("灯光等辅助图标"), [this] { return OverlaySettings.bExtras; }, [this](bool V) { OverlaySettings.bExtras=V; ApplyAll(); });
			Toggle(TEXT("操作提示文字"), [this] { return OverlaySettings.bTextInfo; }, [this](bool V) { OverlaySettings.bTextInfo=V; ApplyAll(); });
			Toggle(TEXT("物体原点"), [this] { return OverlaySettings.bOrigins; }, [this](bool V) { OverlaySettings.bOrigins=V; ApplyAll(); });
			Label(TEXT("几何数据"));
			Toggle(TEXT("线框叠加（保留实体表面）"), [this] { return OverlaySettings.bWireframe; }, [this](bool V) { OverlaySettings.bWireframe=V; ApplyAll(); });
			Number(TEXT("线条不透明度"), 0.f,1.f,.05f,[this]{return OverlaySettings.WireOpacity;},[this](float V){OverlaySettings.WireOpacity=V;ApplyAll();});
			Number(TEXT("线条粗细"), .5f,3.f,.1f,[this]{return OverlaySettings.WireThickness;},[this](float V){OverlaySettings.WireThickness=V;ApplyAll();});
			Rows->AddSlot().AutoHeight().Padding(0,6)[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("编辑模式始终按点 / 边 / 面显示可编辑网格；关闭叠加层可临时隐藏。线框遵循模型本身拓扑。")))];
		}
		else if (Shading == EBlenderViewportShading::Solid)
		{
			Label(TEXT("实体 · 光照"));
			Choices({TEXT("棚光"),TEXT("MatCap"),TEXT("平面")},[this]{return PreviewSettings.SolidMode;},[this](int32 V){PreviewSettings.SolidMode=V;ApplyAll();});
			Choices({TEXT("均衡"),TEXT("柔和"),TEXT("轮廓光")},[this]{return PreviewSettings.StudioIndex;},[this](int32 V){PreviewSettings.StudioIndex=V;PreviewSettings.SolidMode=0;ApplyAll();});
			Number(TEXT("棚光亮度"),.2f,3.f,.05f,[this]{return PreviewSettings.StudioIntensity;},[this](float V){PreviewSettings.StudioIntensity=V;ApplyAll();});
			Number(TEXT("棚光旋转"),-180.f,180.f,5.f,[this]{return PreviewSettings.StudioRotationDegrees;},[this](float V){PreviewSettings.StudioRotationDegrees=V;ApplyAll();});
			Toggle(TEXT("腔体 · 强调凹凸"),[this]{return PreviewSettings.bCavity;},[this](bool V){PreviewSettings.bCavity=V;ApplyAll();});
			Toggle(TEXT("轮廓 · 强调形体边界"),[this]{return PreviewSettings.bOutline;},[this](bool V){PreviewSettings.bOutline=V;ApplyAll();});
			Label(TEXT("MatCap · Blender 内置"));
			AssetGrid(false);
			Toggle(TEXT("水平翻转 MatCap"),[this]{return PreviewSettings.bMatCapFlip;},[this](bool V){PreviewSettings.bMatCapFlip=V;ApplyAll();});
		}
		else if (Shading == EBlenderViewportShading::Material)
		{
			Label(TEXT("材质预览 · HDRI 环境"));
			Toggle(TEXT("场景灯光"),[this]{return PreviewSettings.bUseSceneLights;},[this](bool V){PreviewSettings.bUseSceneLights=V;ApplyAll();});
			Toggle(TEXT("场景世界（使用原天空与环境）"),[this]{return PreviewSettings.bUseSceneWorld;},[this](bool V){PreviewSettings.bUseSceneWorld=V;ApplyAll();});
			AssetGrid(true);
			Number(TEXT("环境强度"),0.f,5.f,.05f,[this]{return PreviewSettings.EnvironmentIntensity;},[this](float V){PreviewSettings.EnvironmentIntensity=V;ApplyAll();});
			Number(TEXT("世界不透明度"),0.f,1.f,.05f,[this]{return PreviewSettings.BackgroundOpacity;},[this](float V){PreviewSettings.BackgroundOpacity=V;ApplyAll();});
			Number(TEXT("背景模糊"),0.f,1.f,.05f,[this]{return PreviewSettings.BackgroundBlur;},[this](float V){PreviewSettings.BackgroundBlur=V;ApplyAll();});
			Number(TEXT("仅背景旋转"),-180.f,180.f,5.f,[this]{return PreviewSettings.BackgroundRotationDegrees;},[this](float V){PreviewSettings.BackgroundRotationDegrees=V;ApplyAll();});
			Rows->AddSlot().AutoHeight().Padding(0,6)[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("使用模型原材质。背景旋转独立于环境照明方向；游戏使用关卡自己的灯光。")))];
		}
		else
		{
			Label(Shading == EBlenderViewportShading::Wireframe ? TEXT("线框 · Alt+Z 切换透视") : TEXT("渲染 · 使用关卡灯光、材质和天空"));
			Rows->AddSlot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("切换到实体或材质预览，可调整对应的预览光照。叠加层可在旁边的菜单独立设置。")))];
		}
		return SNew(SBox).WidthOverride(300).MaxDesiredHeight(660)
			[ SNew(SBorder).Padding(12)[ SNew(SScrollBox) + SScrollBox::Slot()[Rows] ] ];
	}

	TSharedRef<SWidget> MakeOverlay()
	{
		using namespace BlenderViewportPresentationPrivate;
		const TWeakPtr<int32> Lifetime = CallbackLifetime;
		TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
		const TCHAR* Tooltips[] = {
			TEXT("线框 · Z 着色菜单 / Shift+Z\n查看模型线条；透视开关控制背面元素选择。"),
			TEXT("实体 · Z 着色菜单\n明亮棚光 / MatCap / 平面；右侧菜单调整光照。"),
			TEXT("材质预览 · Z 着色菜单\n使用内置 HDRI 照亮模型原材质；右侧菜单选择环境。"),
			TEXT("渲染 · Z 着色菜单\n使用场景灯光与材质。"),
			TEXT("透视 X-Ray · Alt+Z\n编辑模式：半透明预览并允许选择背面点、边、面。\n对象模式：穿透框选与网格边辅助；不会把整个场景变透明。\n线框与实体分别记住开关；材质预览和渲染不可用。")
		};
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Buttons->AddSlot().AutoWidth().Padding(Index == 4 ? 5.0f : 1.0f, 0.0f, 1.0f, 0.0f)
			[
				SNew(SBorder).BorderImage(&ButtonBrush).Padding(0)
				.BorderBackgroundColor_Lambda([this, Lifetime, Index]()
				{
					const bool bSelected = Lifetime.IsValid() && (Index == 4 ? Owner->IsXRay() : int32(Shading) == Index);
					return bSelected ? FLinearColor(0.12f, 0.30f, 0.48f, 0.96f) : FLinearColor(0, 0, 0, 0);
				})
				[
					SNew(SButton).ButtonStyle(FAppStyle::Get(), "NoBorder").ContentPadding(FMargin(3))
					.IsFocusable(false).ToolTipText(FText::FromString(Tooltips[Index]))
					.IsEnabled_Lambda([this, Lifetime, Index]() { return Lifetime.IsValid() && (Index != 4 || Owner->CanXRay()); })
					.OnClicked_Lambda([this, Lifetime, Index]()
					{
						if (Lifetime.IsValid())
						{
							if (Index == 4) { Owner->ToggleXRay(); }
							else { Owner->SetShading(static_cast<EBlenderViewportShading>(Index)); }
						}
						return FReply::Handled();
					})
					[ SNew(SShadingGlyph).Kind(Index) ]
				]
			];
		}
		Buttons->AddSlot().AutoWidth().Padding(3,0,0,0)
		[ SNew(SComboButton).HasDownArrow(true).ContentPadding(FMargin(6,3))
			.ToolTipText(FText::FromString(TEXT("视图叠加层：线框、网格、轮廓与辅助显示")))
			.OnGetMenuContent_Lambda([this,Lifetime]() -> TSharedRef<SWidget> { return Lifetime.IsValid() ? MakeSettingsPanel(true) : SNullWidget::NullWidget; })
			.ButtonContent()[SNew(STextBlock).Text(FText::FromString(TEXT("叠加")))] ];
		Buttons->AddSlot().AutoWidth().Padding(3,0,0,0)
		[ SNew(SComboButton).HasDownArrow(true).ContentPadding(FMargin(6,3))
			.ToolTipText(FText::FromString(TEXT("当前着色方式的光照、MatCap 与 HDRI 设置")))
			.OnGetMenuContent_Lambda([this,Lifetime]() -> TSharedRef<SWidget> { return Lifetime.IsValid() ? MakeSettingsPanel(false) : SNullWidget::NullWidget; })
			.ButtonContent()[SNew(STextBlock).Text(FText::FromString(TEXT("着色")))] ];
		// A full-size non-hit-testable wrapper leaves the native toolbar and viewport input alone.
		return SNew(SOverlay).Visibility(EVisibility::SelfHitTestInvisible)
			+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(FMargin(0, 42, 12, 0))
			[
				SNew(SBorder).BorderImage(&ToolbarBrush).Padding(FMargin(4))
				[ Buttons ]
			];
	}

	void RegisterLiveViewports()
	{
		bool bChanged = false;
		for (int32 Index = Viewports.Num() - 1; Index >= 0; --Index)
		{
			if (!IsLive(Viewports[Index]))
			{
				if (TSharedPtr<SLevelViewport> Widget = Viewports[Index].Widget.Pin())
				{
					if (TSharedPtr<SWidget> Overlay = Viewports[Index].Overlay.Pin()) { Widget->RemoveOverlayWidget(Overlay.ToSharedRef()); }
				}
				Viewports.RemoveAt(Index);
				bChanged = true;
			}
		}
		if (!GEditor) { return; }
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (!Client || !Client->GetWorld() || Client->GetWorld()->IsGameWorld()) { continue; }
			const TSharedPtr<SEditorViewport> EditorWidget = Client->GetEditorViewportWidget();
			if (!EditorWidget.IsValid() || EditorWidget->GetType() != FName(TEXT("SLevelViewport"))) { continue; }
			if (Viewports.ContainsByPredicate([Client](const FViewportState& State) { return State.Client == Client; })) { continue; }
			FViewportState State;
			State.Client = Client;
			State.Widget = StaticCastSharedPtr<SLevelViewport>(EditorWidget);
			State.PerspectiveMode = Client->GetPerspViewMode();
			State.OrthographicMode = Client->GetOrthoViewMode();
			State.ShowFlags = Client->EngineShowFlags;
			State.LastShowFlags = Client->LastEngineShowFlags;
			const TSharedRef<SWidget> Overlay = MakeOverlay();
			State.Overlay = Overlay;
			State.Widget.Pin()->AddOverlayWidget(Overlay);
			Viewports.Add(MoveTemp(State));
			bChanged = true;
			Apply(Viewports.Last());
		}
		if (bChanged) UpdateLighting();
	}
};

FBlenderViewportPresentation::FBlenderViewportPresentation() : Impl(MakeUnique<FImpl>())
{
	Impl->Owner = this;
}

FBlenderViewportPresentation::~FBlenderViewportPresentation()
{
	Impl->OnChanged = nullptr;
	Impl->CallbackLifetime.Reset();
	Restore();
}

void FBlenderViewportPresentation::ActivateViewport(FEditorViewportClient* ViewportClient)
{
	// Only level viewports discovered from GEditor are ever touched; never dereference input here.
	Impl->bActive = true;
	Tick(ViewportClient);
}

void FBlenderViewportPresentation::Tick(FEditorViewportClient* ViewportClient)
{
	(void)ViewportClient;
	if (!Impl->bActive) { return; }
	Impl->RegisterLiveViewports();
	for (FImpl::FViewportState& State : Impl->Viewports)
	{
		if (!Impl->IsLive(State)) { continue; }
		const bool bSolid = Impl->Shading == EBlenderViewportShading::Solid;
		// View-axis changes reapply native flags; resynchronize only when relevant state changes.
		if (State.Client->GetViewMode() != Impl->ViewMode() ||
			!State.Client->EngineShowFlags.Materials || State.Client->EngineShowFlags.LightingOnlyOverride)
		{
			Impl->Apply(State);
		}
	}
}

void FBlenderViewportPresentation::Restore()
{
	const bool bWasActive = Impl->bActive;
	Impl->bActive = false;
	Impl->Lighting->SetEnabled(false);
	Impl->Lighting->SetViewports({});
	for (FImpl::FViewportState& State : Impl->Viewports)
	{
		if (TSharedPtr<SLevelViewport> Widget = State.Widget.Pin())
		{
			if (TSharedPtr<SWidget> Overlay = State.Overlay.Pin()) { Widget->RemoveOverlayWidget(Overlay.ToSharedRef()); }
		}
		if (Impl->IsLive(State))
		{
			State.Client->SetViewModes(State.PerspectiveMode, State.OrthographicMode);
			State.Client->EngineShowFlags = State.ShowFlags;
			State.Client->LastEngineShowFlags = State.LastShowFlags;
			State.Client->Invalidate();
		}
	}
	Impl->Viewports.Reset();
	Impl->bMeshEditActive = false;
	if (bWasActive && Impl->OnChanged) { Impl->OnChanged(); }
	Impl->SolidMaterial.Reset();
	Impl->XRayMaterial.Reset();
	Impl->WireCache.Reset();
}

EBlenderViewportShading FBlenderViewportPresentation::GetShading() const { return Impl->Shading; }

void FBlenderViewportPresentation::SetShading(EBlenderViewportShading Shading)
{
	if (Impl->Shading == Shading) { return; }
	Impl->Shading = Shading;
	Impl->ApplyAll();
}

bool FBlenderViewportPresentation::CanXRay() const
{
	return Impl->Shading == EBlenderViewportShading::Wireframe || Impl->Shading == EBlenderViewportShading::Solid;
}

bool FBlenderViewportPresentation::IsXRay() const
{
	return Impl->Shading == EBlenderViewportShading::Wireframe ? Impl->bWireXRay :
		(Impl->Shading == EBlenderViewportShading::Solid && Impl->bSolidXRay);
}

void FBlenderViewportPresentation::ToggleXRay()
{
	if (!CanXRay()) { return; }
	bool& bXRay = Impl->Shading == EBlenderViewportShading::Wireframe ? Impl->bWireXRay : Impl->bSolidXRay;
	bXRay = !bXRay;
	Impl->ApplyAll();
}

void FBlenderViewportPresentation::SetMeshEditActive(bool bActive)
{
	if (Impl->bMeshEditActive == bActive) { return; }
	Impl->bMeshEditActive = bActive;
	// The owner calls this from OnChanged while synchronizing its mesh preview.
	for (FImpl::FViewportState& State : Impl->Viewports) { Impl->Apply(State); }
}

UMaterialInterface* FBlenderViewportPresentation::GetPreviewMaterial()
{
	if (!Impl->bActive || !CanXRay()) { return nullptr; }
	if (IsXRay())
	{
		if (!Impl->XRayMaterial.IsValid())
		{
			// The sculpt translucent material is lit and becomes black in an unlit level
			// viewport. A transient emissive material keeps X-Ray readable without scene lights.
			UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
			Material->BlendMode = BLEND_Translucent;
			Material->SetShadingModel(MSM_Unlit);
			Material->TwoSided = true;
			auto* Color = NewObject<UMaterialExpressionConstant3Vector>(Material, NAME_None, RF_Transient);
			Color->Constant = FLinearColor(.46f, .50f, .56f);
			auto* Opacity = NewObject<UMaterialExpressionConstant>(Material, NAME_None, RF_Transient);
			Opacity->R = .30f;
			Material->GetExpressionCollection().AddExpression(Color);
			Material->GetExpressionCollection().AddExpression(Opacity);
			Material->GetEditorOnlyData()->EmissiveColor.Expression = Color;
			Material->GetEditorOnlyData()->Opacity.Expression = Opacity;
			Impl->XRayMaterial.Reset(Material);
			Material->PostEditChange();
			Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
			// Translucency cannot draw an opaque fallback. Finish only this small material
			// before assigning it, so the first toggle does not temporarily hide the surface.
			if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel)) Resource->FinishCompilation();
		}
		return Impl->XRayMaterial.Get();
	}
	if (!Impl->SolidMaterial.IsValid())
	{
		// A lit temporary surface supplies normals for the view-only studio / MatCap pass.
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		Material->SetShadingModel(MSM_DefaultLit);
		auto* Gray = NewObject<UMaterialExpressionConstant3Vector>(Material, NAME_None, RF_Transient);
		Gray->Constant = FLinearColor(.55f, .55f, .55f);
		Material->GetExpressionCollection().AddExpression(Gray);
		Material->GetEditorOnlyData()->BaseColor.Expression = Gray;
		Material->GetEditorOnlyData()->Roughness.Constant = .65f;
		Impl->SolidMaterial.Reset(Material);
		Material->PostEditChange();
		Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
		if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel)) Resource->FinishCompilation();
	}
	return Impl->SolidMaterial.Get();
}

void FBlenderViewportPresentation::SetOnChanged(TFunction<void()> InCallback)
{
	Impl->OnChanged = MoveTemp(InCallback);
}

const FBlenderPreviewLightingSettings& FBlenderViewportPresentation::GetPreviewSettings() const { return Impl->PreviewSettings; }
void FBlenderViewportPresentation::SetPreviewSettings(const FBlenderPreviewLightingSettings& Settings)
{
	Impl->PreviewSettings = Settings;
	Impl->PreviewSettings.StudioIntensity = FMath::Clamp(Settings.StudioIntensity, .2f, 3.f);
	Impl->PreviewSettings.EnvironmentIntensity = FMath::Clamp(Settings.EnvironmentIntensity, 0.f, 5.f);
	Impl->PreviewSettings.BackgroundOpacity = FMath::Clamp(Settings.BackgroundOpacity, 0.f, 1.f);
	Impl->PreviewSettings.BackgroundBlur = FMath::Clamp(Settings.BackgroundBlur, 0.f, 1.f);
	Impl->ApplyAll();
}
const FBlenderViewportOverlaySettings& FBlenderViewportPresentation::GetOverlaySettings() const { return Impl->OverlaySettings; }
void FBlenderViewportPresentation::SetOverlaySettings(const FBlenderViewportOverlaySettings& Settings)
{
	Impl->OverlaySettings = Settings;
	Impl->OverlaySettings.WireOpacity = FMath::Clamp(Settings.WireOpacity,0.f,1.f);
	Impl->OverlaySettings.WireThickness = FMath::Clamp(Settings.WireThickness,.5f,3.f);
	Impl->ApplyAll();
}
FBlenderPreviewLighting* FBlenderViewportPresentation::GetLighting() const { return Impl->Lighting.Get(); }

void FBlenderViewportPresentation::RenderOverlays(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	const auto& Settings = Impl->OverlaySettings;
	if (!Impl->bActive || !Settings.bShowOverlays || !View || !PDI || !GEditor || GEditor->PlayWorld) return;
	if (!Settings.bWireframe && !Settings.bOrigins) return;
	const auto* State = Impl->Viewports.FindByPredicate([&](const FImpl::FViewportState& S) { return Impl->IsLive(S) && S.Client->Viewport == Viewport; });
	if (!State || !State->Client->GetWorld() || State->Client->GetWorld()->WorldType != EWorldType::Editor) return;
	for (auto It = Impl->WireCache.CreateIterator(); It; ++It) if (!It.Key().IsValid()) It.RemoveCurrent();
	for (TActorIterator<AActor> It(State->Client->GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->IsHiddenEd() || (Actor->HiddenEditorViews & View->EditorViewBitflag) != 0) continue;
		if (Settings.bOrigins) PDI->DrawPoint(Actor->GetActorLocation(), Actor->IsSelected() ? FLinearColor(1,.45f,.03f) : FLinearColor(.4f,.4f,.4f), 5, SDPG_World);
		if (!Settings.bWireframe || Impl->bMeshEditActive) continue;
		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (UStaticMeshComponent* Component : Components)
		{
			if (!Component || !Component->IsVisible() || !Component->IsRegistered() || !Component->GetStaticMesh()) continue;
			if ((Component->GetHiddenEditorViews() & View->EditorViewBitflag) != 0) continue;
			if (View->HiddenPrimitives.Contains(Component->GetPrimitiveSceneId())) continue;
			if (!View->ViewFrustum.IntersectBox(Component->Bounds.Origin, Component->Bounds.BoxExtent)) continue;
			UStaticMesh* Mesh = Component->GetStaticMesh();
			auto& Wire = Impl->WireCache.FindOrAdd(Mesh);
			if (Wire.RenderData != Mesh->GetRenderData())
			{
				Wire.RenderData = Mesh->GetRenderData(); Wire.Edges.Reset();
				if (const FMeshDescription* Description = Mesh->GetMeshDescription(0))
				{
					FStaticMeshConstAttributes Attributes(*Description);
					const auto Positions = Attributes.GetVertexPositions();
					TSet<FEdgeID> Edges;
					for (FPolygonID Polygon : Description->Polygons().GetElementIDs())
						for (FEdgeID Edge : Description->GetPolygonPerimeterEdges(Polygon)) Edges.Add(Edge);
					for (FEdgeID Edge : Edges)
					{
						const auto Ends = Description->GetEdgeVertices(Edge);
						Wire.Edges.Add({FVector(Positions[Ends[0]]), FVector(Positions[Ends[1]])});
					}
				}
			}
			if (Settings.WireOpacity <= 0) continue;
			const FLinearColor Color(.015f,.018f,.022f,Settings.WireOpacity);
			TArray<FTransform> Transforms;
			if (auto* Instances = Cast<UInstancedStaticMeshComponent>(Component))
			{
				for (int32 Index = 0; Index < Instances->GetInstanceCount(); ++Index)
				{
					FTransform Transform;
					if (Instances->GetInstanceTransform(Index,Transform,true))
					{
						const FBoxSphereBounds Bounds = Mesh->GetBounds().TransformBy(Transform);
						if (View->ViewFrustum.IntersectBox(Bounds.Origin,Bounds.BoxExtent)) Transforms.Add(Transform);
					}
				}
			}
			else Transforms.Add(Component->GetComponentTransform());
			for (const FTransform& Transform : Transforms)
				for (const auto& Edge : Wire.Edges) PDI->DrawTranslucentLine(Transform.TransformPosition(Edge.Key),Transform.TransformPosition(Edge.Value),Color,SDPG_World,Settings.WireThickness,.2f,true);
		}
	}
}
