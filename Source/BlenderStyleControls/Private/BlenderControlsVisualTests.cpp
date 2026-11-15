#include "BlenderControlsVisualTests.h"
#include "BlenderStyleEdMode.h"
#include "BlenderMeshEditSession.h"
#include "BlenderViewportPresentation.h"
#include "BlenderPreviewLighting.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/Selection.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "ImageUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "SEditorViewport.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "MaterialShared.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "SceneView.h"
#include "Slate/SceneViewport.h"
#include "LevelEditorViewport.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

namespace
{
struct FVisualState
{
    FString Folder;
    FLevelEditorViewportClient* Client=nullptr;
    TWeakObjectPtr<UWorld> World;
    TArray<TWeakObjectPtr<AStaticMeshActor>> Actors;
    int32 Phase=0;
    int32 Frames=0;
    bool bSelectionFixes=false;
    bool bTopology=false;
    bool bPreview=false;
    TArray<TWeakObjectPtr<UMaterialInterface>> SourceMaterials;
    TArray<TSharedPtr<FJsonValue>> Metrics;
    TArray<FVector> WhiteSphereSamples;
    TWeakObjectPtr<UStaticMesh> GeneratedCube;
    double Start=0;
    TArray<FString> Images;
    TArray<FString> Errors;
    FTSTicker::FDelegateHandle Handle;
};
TSharedPtr<FVisualState> Visual;

FBlenderStyleEdMode* Mode()
{
    return static_cast<FBlenderStyleEdMode*>(GLevelEditorModeTools().GetActiveMode(FBlenderStyleEdMode::ModeId));
}

void FinishVisual()
{
    if (!Visual) return;
    if (Mode()) { Mode()->CancelActiveTransform(); Mode()->EndMeshEdit(false); }
    if (Visual->bPreview)
    {
        for (int32 I=0; I<Visual->Actors.Num(); ++I)
        {
            if (!Visual->Actors[I].IsValid() || !Visual->SourceMaterials.IsValidIndex(I)
                || Visual->Actors[I]->GetStaticMeshComponent()->GetMaterial(0) != Visual->SourceMaterials[I].Get())
                Visual->Errors.Add(TEXT("Preview changed an original component material"));
        }
        if (Visual->WhiteSphereSamples.Num() == 9 &&
            FVector::Dist(Visual->WhiteSphereSamples[3],Visual->WhiteSphereSamples[5]) < 1.0/255.0)
            Visual->Errors.Add(TEXT("Changing forest to studio did not change the white material patch"));
    }
    GLevelEditorModeTools().DeactivateMode(FBlenderStyleEdMode::ModeId);
    GEditor->SelectNone(false,true,false);
    if (Visual->World.IsValid()) Visual->World->GetOutermost()->SetDirtyFlag(false);
    UWorld* Empty=GEditor->NewMap(false);
    if (Empty) Empty->GetOutermost()->SetDirtyFlag(false);
    if(auto* Cube=Visual->GeneratedCube.Get())
    {
        UPackage* Package=Cube->GetOutermost();
        if(Package->HasAnyPackageFlags(PKG_NewlyCreated) && !FPackageName::DoesPackageExist(Package->GetName()))
        {
            FAssetRegistryModule::AssetDeleted(Cube); Cube->ClearFlags(RF_Public|RF_Standalone);
            Cube->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
            Cube->MarkAsGarbage();Package->SetDirtyFlag(false);
        }
    }
    const int32 ExpectedImages=Visual->bPreview?9:Visual->bTopology?6:Visual->bSelectionFixes?5:11;
    TSharedRef<FJsonObject> Report=MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("passed"),Visual->Errors.IsEmpty() && Visual->Images.Num()==ExpectedImages);
    Report->SetBoolField(TEXT("native_os_input_tested"),false);
    Report->SetBoolField(TEXT("saved_user_maps"),false);
    TArray<TSharedPtr<FJsonValue>> Images,Errors;
    for(const auto& S:Visual->Images) Images.Add(MakeShared<FJsonValueString>(S));
    for(const auto& S:Visual->Errors) Errors.Add(MakeShared<FJsonValueString>(S));
    Report->SetArrayField(TEXT("images"),Images); Report->SetArrayField(TEXT("errors"),Errors);
    if (Visual->bPreview) Report->SetArrayField(TEXT("render_metrics"),Visual->Metrics);
    FString Json; FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Json));
    FFileHelper::SaveStringToFile(Json,*(Visual->Folder/TEXT("visual_verification.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    const bool Passed=Visual->Errors.IsEmpty() && Visual->Images.Num()==ExpectedImages;
    Visual.Reset();
    FPlatformMisc::RequestExitWithStatus(false,Passed?0:1);
}

bool Capture(const FString& Name)
{
    if(Name==TEXT("solid_xray_toolbar") && Mode() && Mode()->GetPresentation())
    {
        if(auto* Material=Mode()->GetPresentation()->GetPreviewMaterial())
        {
            if(auto* Resource=Material->GetMaterialResource(Visual->Client->GetWorld()->GetFeatureLevel()))
            {
                UE_LOG(LogTemp,Display,TEXT("BlenderVisual XRay shader ready before wait: %d"),Resource->IsCompilationFinished());
                if(!Resource->IsCompilationFinished()) Visual->Errors.Add(TEXT("XRay preview assigned before shader readiness"));
            }
        }
    }
    const FEngineShowFlags& Flags=Visual->Client->EngineShowFlags;
    UE_LOG(LogTemp,Display,TEXT("BlenderVisual %s: Translucency=%d Separate=%d Materials=%d Lighting=%d LightingOnly=%d Post=%d"),
        *Name,int32(Flags.Translucency),int32(Flags.SeparateTranslucency),int32(Flags.Materials),int32(Flags.Lighting),int32(Flags.LightingOnlyOverride),int32(Flags.PostProcessing));
    FViewport* Viewport=Visual->Client->Viewport;
    Viewport->Draw(); FlushRenderingCommands();
    TArray<FColor> Pixels;
    FIntPoint Size=Viewport->GetSizeXY();
    bool bSlateCaptured=false;
    if(!Visual->bPreview) if(auto Widget=Visual->Client->GetEditorViewportWidget())
    {
        FIntVector SlateSize;
        bSlateCaptured=FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(),Pixels,SlateSize);
        if(bSlateCaptured) Size=FIntPoint(SlateSize.X,SlateSize.Y);
    }
    if(!bSlateCaptured && !Viewport->ReadPixels(Pixels)) return false;
    if(Pixels.Num()!=Size.X*Size.Y) return false;
    for(auto& P:Pixels) P.A=255;
    if (Visual->bPreview)
    {
        TSharedRef<FJsonObject> Metric=MakeShared<FJsonObject>();
        Metric->SetStringField(TEXT("image"),Name);
        Metric->SetNumberField(TEXT("pixel_crc32"),FCrc::MemCrc32(Pixels.GetData(),Pixels.Num()*sizeof(FColor)));
        Metric->SetNumberField(TEXT("width"),Size.X); Metric->SetNumberField(TEXT("height"),Size.Y);
        TArray<TSharedPtr<FJsonValue>> MaterialDiagnostics;
        for(int32 ActorIndex=0;ActorIndex<Visual->Actors.Num();++ActorIndex)
        {
            UMaterialInterface* Interface=Visual->Actors[ActorIndex].IsValid()?Visual->Actors[ActorIndex]->GetStaticMeshComponent()->GetMaterial(0):nullptr;
            UMaterial* Material=Interface?Interface->GetMaterial():nullptr;
            FMaterialResource* Resource=Material?Material->GetMaterialResource(Visual->Client->GetWorld()->GetFeatureLevel()):nullptr;
            TSharedRef<FJsonObject> Diagnostic=MakeShared<FJsonObject>();
            Diagnostic->SetNumberField(TEXT("actor"),ActorIndex);
            Diagnostic->SetStringField(TEXT("material"),Interface?Interface->GetPathName():TEXT("missing"));
            Diagnostic->SetBoolField(TEXT("source_material_unchanged"),Visual->SourceMaterials.IsValidIndex(ActorIndex)&&Visual->SourceMaterials[ActorIndex].Get()==Interface);
            Diagnostic->SetBoolField(TEXT("unlit"),Material&&Material->GetShadingModels().HasShadingModel(MSM_Unlit));
            Diagnostic->SetBoolField(TEXT("translucent"),Material&&Material->GetBlendMode()==BLEND_Translucent);
            Diagnostic->SetStringField(TEXT("translucency_pass"),Material&&Material->TranslucencyPass==MTP_AfterDOF?TEXT("AfterDOF"):TEXT("Other"));
            Diagnostic->SetBoolField(TEXT("shader_complete"),Material&&Material->IsComplete());
            Diagnostic->SetBoolField(TEXT("compilation_finished"),Resource&&Resource->IsCompilationFinished());
            Diagnostic->SetStringField(TEXT("compile_errors"),Resource?FString::Join(Resource->GetCompileErrors(),TEXT("; ")):TEXT("No material resource"));
            if(Material)
            {
                const auto* Data=Material->GetEditorOnlyData();
                const auto* Base=Cast<UMaterialExpressionConstant3Vector>(Data->BaseColor.Expression);
                const auto* Emissive=Cast<UMaterialExpressionConstant3Vector>(Data->EmissiveColor.Expression);
                const auto* Metal=Cast<UMaterialExpressionConstant>(Data->Metallic.Expression);
                const auto* Rough=Cast<UMaterialExpressionConstant>(Data->Roughness.Expression);
                const auto* Opacity=Cast<UMaterialExpressionConstant>(Data->Opacity.Expression);
                Diagnostic->SetStringField(TEXT("base_color"),Base?Base->Constant.ToString():TEXT("no constant expression"));
                Diagnostic->SetStringField(TEXT("emissive"),Emissive?Emissive->Constant.ToString():TEXT("no constant expression"));
                Diagnostic->SetNumberField(TEXT("metallic"),Metal?Metal->R:-1);
                Diagnostic->SetNumberField(TEXT("roughness"),Rough?Rough->R:-1);
                Diagnostic->SetNumberField(TEXT("opacity"),Opacity?Opacity->R:1);
            }
            UE_LOG(LogTemp,Display,TEXT("BlenderPreviewFixture phase=%d actor=%d material=%s unlit=%d complete=%d finished=%d"),
                Visual->Phase,ActorIndex,Interface?*Interface->GetPathName():TEXT("missing"),Material&&Material->GetShadingModels().HasShadingModel(MSM_Unlit),Material&&Material->IsComplete(),Resource&&Resource->IsCompilationFinished());
            MaterialDiagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
        }
        Metric->SetArrayField(TEXT("source_materials"),MaterialDiagnostics);
        TArray<TSharedPtr<FJsonValue>> Samples;
        FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport,Visual->Client->GetScene(),Visual->Client->EngineShowFlags));
        auto* View=Visual->Client->CalcSceneView(&Family);
        for (int32 ActorIndex : {0,1,2,4})
        {
            FVector Sum=FVector::ZeroVector; int32 Count=0; FVector2D Center;
            if (View && Visual->Actors.IsValidIndex(ActorIndex) && Visual->Actors[ActorIndex].IsValid()
                && View->WorldToPixel(Visual->Actors[ActorIndex]->GetActorLocation(),Center))
            {
                const FIntPoint Start(FMath::Clamp(FMath::RoundToInt(Center.X)-12,0,Size.X-1),FMath::Clamp(FMath::RoundToInt(Center.Y)-12,0,Size.Y-1));
                const FIntPoint End(FMath::Clamp(FMath::RoundToInt(Center.X)+12,0,Size.X-1),FMath::Clamp(FMath::RoundToInt(Center.Y)+12,0,Size.Y-1));
                for(int32 Y=Start.Y;Y<=End.Y;++Y)for(int32 X=Start.X;X<=End.X;++X)
                {const auto& P=Pixels[Y*Size.X+X];Sum+=FVector(P.R,P.G,P.B)/255.0;++Count;}
            }
            const FVector Mean=Count?Sum/Count:FVector::ZeroVector;
            TSharedRef<FJsonObject> Sample=MakeShared<FJsonObject>();
            Sample->SetNumberField(TEXT("actor"),ActorIndex);Sample->SetNumberField(TEXT("pixels"),Count);
            Sample->SetNumberField(TEXT("r"),Mean.X);Sample->SetNumberField(TEXT("g"),Mean.Y);Sample->SetNumberField(TEXT("b"),Mean.Z);
            Samples.Add(MakeShared<FJsonValueObject>(Sample));
            if (ActorIndex==0) Visual->WhiteSphereSamples.Add(Mean);
            if (Visual->Phase==0 && ActorIndex<3 && (Count==0 || Mean.GetMin()<0.18))
                Visual->Errors.Add(FString::Printf(TEXT("Default solid object %d is too dark or not visible at its center"),ActorIndex));
            if (Visual->Phase==8 && ActorIndex==4 && (Count==0 || Mean.X<.35 || Mean.X<Mean.Y+.12 || Mean.X<Mean.Z+.08))
                Visual->Errors.Add(TEXT("HDRI background erased or obscured the translucent red test sphere"));
        }
        Metric->SetArrayField(TEXT("surface_patches"),Samples);
        Visual->Metrics.Add(MakeShared<FJsonValueObject>(Metric));
    }
    TArray<uint8> Png;FImageUtils::CompressImageArray(Size.X,Size.Y,Pixels,Png);
    FString File=Visual->Folder/(Name+TEXT(".png"));
    if(!FFileHelper::SaveArrayToFile(Png,*File)) return false;
    Visual->Images.Add(File);return true;
}

void Press(FKey Key)
{
    if(Mode()) Mode()->InputKey(Visual->Client,Visual->Client->Viewport,Key,IE_Pressed);
}

UMaterial* MakePreviewSurface(const FLinearColor& Color,float Metallic,float Roughness,ERHIFeatureLevel::Type FeatureLevel,bool bUnlit=false,float Opacity=1.f)
{
    UMaterial* Material=NewObject<UMaterial>(GetTransientPackage(),NAME_None,RF_Transient);
    Material->SetShadingModel(bUnlit?MSM_Unlit:MSM_DefaultLit);
    if(Opacity<1.f){Material->BlendMode=BLEND_Translucent;Material->TranslucencyPass=MTP_AfterDOF;}
    auto* Base=NewObject<UMaterialExpressionConstant3Vector>(Material,NAME_None,RF_Transient);Base->Constant=Color;
    auto* Metal=NewObject<UMaterialExpressionConstant>(Material,NAME_None,RF_Transient);Metal->R=Metallic;
    auto* Rough=NewObject<UMaterialExpressionConstant>(Material,NAME_None,RF_Transient);Rough->R=Roughness;
    for(UMaterialExpression* Expression:TArray<UMaterialExpression*>{Base,Metal,Rough})Material->GetExpressionCollection().AddExpression(Expression);
    if(bUnlit)Material->GetEditorOnlyData()->EmissiveColor.Expression=Base;
    else Material->GetEditorOnlyData()->BaseColor.Expression=Base;
    Material->GetEditorOnlyData()->Metallic.Expression=Metal;
    Material->GetEditorOnlyData()->Roughness.Expression=Rough;
    if(Opacity<1.f)
    {
        auto* Alpha=NewObject<UMaterialExpressionConstant>(Material,NAME_None,RF_Transient);Alpha->R=Opacity;
        Material->GetExpressionCollection().AddExpression(Alpha);Material->GetEditorOnlyData()->Opacity.Expression=Alpha;
    }
    Material->PostEditChange();
    // PostEditChange uses PrecompileMode::None in UE 5.4: FinishCompilation alone
    // can return before on-demand shader work has even been requested. Make the
    // fixture ready before testing the production preview's first frame.
    {
        FMaterialUpdateContext Context;
        Context.AddMaterial(Material);
        Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
    }
    if(auto* Resource=Material->GetMaterialResource(FeatureLevel))Resource->FinishCompilation();
    if(!Material->IsComplete() && Visual)Visual->Errors.Add(TEXT("Fixture material did not finish synchronous shader preparation"));
    FlushRenderingCommands();
    return Material;
}

bool TickVisual(float)
{
    if(!Visual) return false;
    if(FPlatformTime::Seconds()-Visual->Start>(Visual->bPreview?180:60) || !Mode() || !Visual->World.IsValid())
    { Visual->Errors.Add(TEXT("Visual verification lost mode/world or timed out"));FinishVisual();return false; }
    Visual->Client->Invalidate();
    if(++Visual->Frames<15) return true;
    Visual->Frames=0;
    if(Visual->bPreview)
    {
        static const TCHAR* PreviewNames[]={TEXT("solid_studio_bright"),TEXT("solid_matcap_metal"),TEXT("solid_cavity_wire_overlay"),
            TEXT("material_forest_gray_background"),TEXT("material_forest_environment"),TEXT("material_studio_rotated_blurred"),
            TEXT("edit_unselected_gray"),TEXT("edit_only_one_face_selected"),TEXT("glass_against_hdri")};
        auto* Display=Mode()->GetPresentation();
        if(!Display){Visual->Errors.Add(TEXT("Lost preview presentation"));FinishVisual();return false;}
        if(!Capture(PreviewNames[Visual->Phase]))Visual->Errors.Add(TEXT("Preview capture failed"));
        ++Visual->Phase;
        auto Settings=Display->GetPreviewSettings();
        auto Overlays=Display->GetOverlaySettings();
        if(Visual->Phase==1)
        { Settings.SolidMode=1;Settings.MatCapStem=TEXT("metal_shiny");Display->SetPreviewSettings(Settings); }
        else if(Visual->Phase==2)
        {
            Settings.SolidMode=0;Settings.bCavity=true;Settings.bOutline=true;Display->SetPreviewSettings(Settings);
            Overlays.bWireframe=true;Display->SetOverlaySettings(Overlays);
        }
        else if(Visual->Phase==3)
        {
            Settings.bCavity=false;Settings.bOutline=false;Settings.WorldStem=TEXT("forest");Settings.BackgroundOpacity=0;
            Display->SetPreviewSettings(Settings);Overlays.bWireframe=false;Display->SetOverlaySettings(Overlays);
            Display->SetShading(EBlenderViewportShading::Material);
        }
        else if(Visual->Phase==4)
        { Settings.BackgroundOpacity=1;Display->SetPreviewSettings(Settings); }
        else if(Visual->Phase==5)
        {
            Settings.WorldStem=TEXT("studio");Settings.BackgroundRotationDegrees=50;Settings.BackgroundBlur=.5f;
            Display->SetPreviewSettings(Settings);
        }
        else if(Visual->Phase==6)
        {
            Display->SetShading(EBlenderViewportShading::Solid);
            GEditor->SelectNone(false,true,false);GEditor->SelectActor(Visual->Actors[2].Get(),true,false);GEditor->NoteSelectionChange();
            if(!Mode()->BeginMeshEdit(Visual->Client,Visual->Client->Viewport))
            {Visual->Errors.Add(TEXT("Cannot enter cube edit preview"));FinishVisual();return false;}
            if(!Mode()->GetMeshEditSession()->GetSelectedVertices().IsEmpty())Visual->Errors.Add(TEXT("Edit entry forced mesh selection"));
        }
        else if(Visual->Phase==7)
        {
            auto* Session=Mode()->GetMeshEditSession();Session->SetSelectionMode(EBlenderMeshSelectionMode::Face);
            int32 Best=INDEX_NONE;double BestDistance=DBL_MAX;
            for(int32 ID:Session->GetElementIDs(0,EBlenderMeshSelectionMode::Face))
            {
                Session->SelectElement(0,EBlenderMeshSelectionMode::Face,ID,false);
                const double Distance=FVector::DistSquared(Session->GetSelectionCenter(),Visual->Client->GetViewLocation());
                if(Distance<BestDistance){BestDistance=Distance;Best=ID;}
            }
            Session->SelectAll(false);
            if(Best==INDEX_NONE || !Session->SelectElement(0,EBlenderMeshSelectionMode::Face,Best,false)
                || Session->GetSelectedElementIDs(0,EBlenderMeshSelectionMode::Face).Num()!=1 || Session->HasChanges())
                Visual->Errors.Add(TEXT("Could not select one face without geometry changes"));
        }
        else if(Visual->Phase==8)
        {
            Mode()->EndMeshEdit(false);GEditor->SelectNone(false,true,false);GEditor->NoteSelectionChange();
            Settings.WorldStem=TEXT("forest");Settings.BackgroundOpacity=1.f;Settings.BackgroundBlur=0.f;Settings.BackgroundRotationDegrees=0.f;
            Display->SetPreviewSettings(Settings);Display->SetShading(EBlenderViewportShading::Material);
        }
        else {FinishVisual();return false;}
        return true;
    }
    if(Visual->bTopology)
    {
        static const TCHAR* TopologyNames[]={TEXT("quad_cube_gray"),TEXT("E_extrusion_preview"),TEXT("E_extrusion_confirmed"),TEXT("CtrlR_loop_preview"),TEXT("CtrlR_loop_slide"),TEXT("CtrlR_loop_confirmed")};
        auto* Session=Mode()->GetMeshEditSession();
        if(!Session){Visual->Errors.Add(TEXT("Topology visual lost session"));FinishVisual();return false;}
        if(!Capture(TopologyNames[Visual->Phase]))Visual->Errors.Add(TEXT("Topology capture failed"));
        ++Visual->Phase;
        if(Visual->Phase==1)
        {
            Session->SetSelectionMode(EBlenderMeshSelectionMode::Face);
            int32 Top=INDEX_NONE;double Height=-DBL_MAX;
            for(int32 ID:Session->GetElementIDs(0,EBlenderMeshSelectionMode::Face))
            {
                Session->SelectElement(0,EBlenderMeshSelectionMode::Face,ID,false);
                if(Session->GetSelectionCenter().Z>Height){Height=Session->GetSelectionCenter().Z;Top=ID;}
            }
            Session->SelectElement(0,EBlenderMeshSelectionMode::Face,Top,false);
            Press(EKeys::E);Press(EKeys::Five);Press(EKeys::Zero);
            if(!Mode()->IsTopologyModal() || !Session->IsTopologyEditing())Visual->Errors.Add(TEXT("E key did not begin extrusion preview"));
        }
        else if(Visual->Phase==2) Press(EKeys::Enter);
        else if(Visual->Phase==3)
        {
            if(!Session->Undo())Visual->Errors.Add(TEXT("Extrusion undo failed before loop visual"));
            const FVector Point=Session->GetSelectionCenter();
            auto Widget=Visual->Client->GetEditorViewportWidget();
            auto Scene=Widget.IsValid()?Widget->GetSceneViewport():nullptr;
            FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Visual->Client->Viewport,Visual->Client->GetScene(),Visual->Client->EngineShowFlags));
            auto* View=Visual->Client->CalcSceneView(&Family);FVector2D Pixel;
            if(Scene.IsValid() && View && View->WorldToPixel(Point,Pixel))
            {
                const FGeometry Geometry=Scene->GetCachedGeometry();const FVector2D Screen=Geometry.LocalToAbsolute(Pixel/Geometry.Scale);
                Scene->OnMouseMove(Geometry,FPointerEvent(0,Screen,Screen,TSet<FKey>(),EKeys::Invalid,0,FModifierKeysState()));
                const FModifierKeysState Ctrl(false,false,true,false,false,false,false,false,false);
                Scene->OnKeyDown(Geometry,FKeyEvent(EKeys::LeftControl,Ctrl,0,false,0,0));Press(EKeys::R);
                Scene->OnKeyUp(Geometry,FKeyEvent(EKeys::LeftControl,FModifierKeysState(),0,false,0,0));
                if(!Mode()->IsTopologyModal())Visual->Errors.Add(TEXT("CtrlR did not begin hover"));
            }
            else Visual->Errors.Add(TEXT("Loop visual projection unavailable"));
        }
        else if(Visual->Phase==4)
        {
            Press(EKeys::LeftMouseButton);
            Mode()->InputKey(Visual->Client,Visual->Client->Viewport,EKeys::LeftMouseButton,IE_Released);
            Press(EKeys::Zero);Press(EKeys::Period);Press(EKeys::Five);
            if(!Session->IsTopologyEditing())Visual->Errors.Add(TEXT("Loop click did not insert quad cut"));
        }
        else if(Visual->Phase==5) Press(EKeys::Enter);
        else {FinishVisual();return false;}
        return true;
    }
    if(Visual->bSelectionFixes)
    {
        static const TCHAR* SelectionNames[]={TEXT("edit_entry_unselected_gray"),TEXT("one_vertex_selected"),TEXT("one_edge_selected"),TEXT("one_face_selected"),TEXT("selection_cleared_gray")};
        auto* Session=Mode()->GetMeshEditSession();
        if(!Session) {Visual->Errors.Add(TEXT("Edit selection visual lost session"));FinishVisual();return false;}
        const bool bEmpty=Visual->Phase==0 || Visual->Phase==4;
        if((bEmpty && !Session->GetSelectedVertices().IsEmpty()) || Session->HasChanges())
            Visual->Errors.Add(TEXT("Entry/clear selection or geometry state incorrect"));
        if(!Capture(SelectionNames[Visual->Phase]))Visual->Errors.Add(TEXT("Selection capture failed"));
        if(++Visual->Phase>=5){FinishVisual();return false;}
        Session->SelectAll(false);
        if(Visual->Phase<=3)
        {
            const auto SelectionMode=Visual->Phase==1?EBlenderMeshSelectionMode::Vertex:Visual->Phase==2?EBlenderMeshSelectionMode::Edge:EBlenderMeshSelectionMode::Face;
            Session->SetSelectionMode(SelectionMode);
            int32 Best=INDEX_NONE;double BestDistance=DBL_MAX;
            for(int32 ID:Session->GetElementIDs(0,SelectionMode))
            {
                Session->SelectElement(0,SelectionMode,ID,false);
                const double Distance=FVector::DistSquared(Session->GetSelectionCenter(),Visual->Client->GetViewLocation());
                if(Distance<BestDistance){BestDistance=Distance;Best=ID;}
            }
            Session->SelectAll(false);
            if(Best==INDEX_NONE || !Session->SelectElement(0,SelectionMode,Best,false))Visual->Errors.Add(TEXT("Could not select one visible element"));
            if(Session->GetSelectedElementIDs(0,SelectionMode).Num()!=1 || !Session->GetSelectedElementIDs(1,SelectionMode).IsEmpty())
                Visual->Errors.Add(TEXT("Partial selection leaked to other elements or object"));
        }
        return true;
    }
    static const TCHAR* Names[]={TEXT("object_X_red"),TEXT("object_Y_green"),TEXT("object_Z_blue"),TEXT("multi_mesh_faces_X"),TEXT("multi_mesh_vertices_Z"),
        TEXT("solid_mesh_toolbar"),TEXT("solid_xray_toolbar"),TEXT("wireframe_xray_toolbar"),TEXT("material_preview_toolbar"),TEXT("rendered_toolbar"),TEXT("box_selection_drag")};
    if(!Capture(Names[Visual->Phase])) Visual->Errors.Add(TEXT("Capture failed"));
    ++Visual->Phase;
    if(Visual->Phase==1) Press(EKeys::Y);
    else if(Visual->Phase==2) Press(EKeys::Z);
    else if(Visual->Phase==3)
    {
        Press(EKeys::Escape);
        if(!Mode()->BeginMeshEdit(Visual->Client,Visual->Client->Viewport))
        {Visual->Errors.Add(TEXT("Could not begin multi-object mesh edit"));FinishVisual();return false;}
        auto* Session=Mode()->GetMeshEditSession();
        Session->SelectAll(false);Session->SetSelectionMode(EBlenderMeshSelectionMode::Face);
        TArray<int32> TopFaces;
        for(int32 Object=0;Object<Session->GetObjectCount();++Object)
        {
            int32 Best=INDEX_NONE;double BestZ=-DBL_MAX;
            for(int32 Face:Session->GetElementIDs(Object,EBlenderMeshSelectionMode::Face))
            {
                Session->SelectElement(Object,EBlenderMeshSelectionMode::Face,Face,false);
                double MeanZ=0;auto V=Session->GetSelectedVertices();
                for(const auto& P:V) MeanZ+=P.WorldPosition.Z;
                if(!V.IsEmpty()) MeanZ/=V.Num();
                if(MeanZ>BestZ){BestZ=MeanZ;Best=Face;}
            }
            TopFaces.Add(Best);
        }
        Session->SelectAll(false);
        for(int32 Object=0;Object<TopFaces.Num();++Object) Session->SelectElement(Object,EBlenderMeshSelectionMode::Face,TopFaces[Object],true);
        Mode()->StartTransform(EBlenderTransformOperation::Move,Visual->Client,Visual->Client->Viewport);
        Press(EKeys::X);Press(EKeys::Five);Press(EKeys::Zero);
    }
    else if(Visual->Phase==4)
    {
        Press(EKeys::Enter);
        Mode()->GetMeshEditSession()->SetSelectionMode(EBlenderMeshSelectionMode::Vertex);
        Mode()->StartTransform(EBlenderTransformOperation::Move,Visual->Client,Visual->Client->Viewport);
        Press(EKeys::Z);Press(EKeys::Two);Press(EKeys::Five);
    }
    else if(Visual->Phase==5)
    {
        Press(EKeys::Escape);
        Mode()->GetPresentation()->SetShading(EBlenderViewportShading::Solid);
    }
    else if(Visual->Phase==6) Mode()->GetPresentation()->ToggleXRay();
    else if(Visual->Phase==7) Mode()->GetPresentation()->SetShading(EBlenderViewportShading::Wireframe);
    else if(Visual->Phase==8) Mode()->GetPresentation()->SetShading(EBlenderViewportShading::Material);
    else if(Visual->Phase==9) Mode()->GetPresentation()->SetShading(EBlenderViewportShading::Rendered);
    else if(Visual->Phase==10)
    {
        Mode()->GetPresentation()->SetShading(EBlenderViewportShading::Solid);
        Press(EKeys::B); Press(EKeys::LeftMouseButton);
        Mode()->MouseMove(Visual->Client,Visual->Client->Viewport,650,400);
    }
    else if(Visual->Phase>=11){FinishVisual();return false;}
    return true;
}
}

static void StartBlenderControlsVisualVerification()
{
    if(Visual || !GEditor || !FParse::Param(FCommandLine::Get(),TEXT("BlenderControlsVerify"))) return;
    Visual=MakeShared<FVisualState>();Visual->Start=FPlatformTime::Seconds();
    Visual->bSelectionFixes=FParse::Param(FCommandLine::Get(),TEXT("BlenderControlsSelectionVisual"));
    Visual->bTopology=FParse::Param(FCommandLine::Get(),TEXT("BlenderControlsTopologyVisual"));
    Visual->bPreview=FParse::Param(FCommandLine::Get(),TEXT("BlenderControlsPreviewVisual"));
    Visual->Folder=FPaths::ProjectSavedDir()/TEXT("BlenderStyleControls/VisualV2");
    FParse::Value(FCommandLine::Get(),TEXT("BlenderControlsVisualDir="),Visual->Folder);
    IFileManager::Get().MakeDirectory(*Visual->Folder,true);
    UWorld* World=GEditor->NewMap(false);Visual->World=World;
    for(auto* Client:GEditor->GetLevelViewportClients()) if(Client && Client->Viewport && Client->IsPerspective()){Visual->Client=Client;break;}
    if(!World || !Visual->Client){Visual->Errors.Add(TEXT("No editor world/viewport"));FinishVisual();return;}
    if(Visual->bPreview)
    {
        UStaticMesh* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
        UStaticMesh* Sphere=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere"));
        if(!Cube || !Sphere){Visual->Errors.Add(TEXT("Preview sample meshes missing"));FinishVisual();return;}
        FActorSpawnParameters LightParams;LightParams.ObjectFlags|=RF_Transient;
        if(auto* Light=World->SpawnActor<ADirectionalLight>(FVector::ZeroVector,FRotator(-45,-30,0),LightParams))
        {
            Light->GetLightComponent()->SetIntensity(4.0f);
            Light->GetLightComponent()->SetLightColor(FLinearColor(1.f,.12f,.04f));
        }
        const FVector Locations[]={FVector(-165,0,80),FVector(165,0,80),FVector(0,0,68),FVector(0,0,-15)};
        const FVector Scales[]={FVector(1.5),FVector(1.5),FVector(1.2),FVector(7,4,.2)};
        const FLinearColor Colors[]={FLinearColor(.65f,.65f,.65f),FLinearColor(.8f,.5f,.2f),FLinearColor(.08f,.26f,.35f),FLinearColor(.32f,.32f,.32f)};
        for(int32 I=0;I<4;++I)
        {
            FActorSpawnParameters Params;Params.ObjectFlags|=RF_Transient;
            auto* Actor=World->SpawnActor<AStaticMeshActor>(Locations[I],FRotator::ZeroRotator,Params);
            Actor->GetStaticMeshComponent()->SetStaticMesh(I<2?Sphere:Cube);Actor->SetActorScale3D(Scales[I]);
            UMaterial* Material=MakePreviewSurface(Colors[I],I==1?1.f:0.f,I==1?.16f:.7f,World->GetFeatureLevel(),I==2);
            Actor->GetStaticMeshComponent()->SetMaterial(0,Material);
            Visual->Actors.Add(Actor);Visual->SourceMaterials.Add(Material);
        }
        // Index 4 deliberately writes no opaque scene depth. Its center sits beyond
        // the stage so HDRI composition must preserve normal translucency over sky.
        FActorSpawnParameters GlassParams;GlassParams.ObjectFlags|=RF_Transient;
        auto* Glass=World->SpawnActor<AStaticMeshActor>(FVector(390,10,135),FRotator::ZeroRotator,GlassParams);
        Glass->GetStaticMeshComponent()->SetStaticMesh(Sphere);Glass->SetActorScale3D(FVector(1.25));
        UMaterial* GlassMaterial=MakePreviewSurface(FLinearColor(1.f,.015f,.08f),0.f,.4f,World->GetFeatureLevel(),true,.65f);
        Glass->GetStaticMeshComponent()->SetMaterial(0,GlassMaterial);
        Visual->Actors.Add(Glass);Visual->SourceMaterials.Add(GlassMaterial);
        GEditor->SelectNone(false,true,false);GEditor->NoteSelectionChange();
        Visual->Client->SetViewLocation(FVector(400,-900,450));
        Visual->Client->SetViewRotation((FVector(0,0,65)-Visual->Client->GetViewLocation()).Rotation());
        Visual->Client->SetViewMode(VMI_Lit);Visual->Client->SetRealtime(true);
        World->GetOutermost()->SetDirtyFlag(false);
        GLevelEditorModeTools().ActivateMode(FBlenderStyleEdMode::ModeId);
        if(!Mode() || !Mode()->GetPresentation()){Visual->Errors.Add(TEXT("Cannot activate preview mode"));FinishVisual();return;}
        auto* Display=Mode()->GetPresentation();Display->SetShading(EBlenderViewportShading::Solid);
        if(Display->IsXRay())Display->ToggleXRay();
        auto Settings=Display->GetPreviewSettings();Settings.SolidMode=0;Settings.bUseSceneLights=false;Settings.bUseSceneWorld=false;
        Settings.bCavity=false;Settings.bOutline=false;Display->SetPreviewSettings(Settings);
        Visual->Start=FPlatformTime::Seconds();
        Visual->Handle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickVisual),.05f);
        return;
    }
    UStaticMesh* Cube=Visual->bTopology?CreateBlenderQuadCube():LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
    if(Visual->bTopology)Visual->GeneratedCube=Cube;
    if(!Cube){Visual->Errors.Add(TEXT("No cube mesh"));FinishVisual();return;}
    FActorSpawnParameters LightParams; LightParams.ObjectFlags|=RF_Transient;
    if(auto* Light=World->SpawnActor<ADirectionalLight>(FVector::ZeroVector,FRotator(-45,-30,0),LightParams))
        Light->GetLightComponent()->SetIntensity(3.0f);
    for(int32 I=0;I<3;++I)
    {
        FActorSpawnParameters Params;Params.ObjectFlags|=RF_Transactional;
        auto* Actor=World->SpawnActor<AStaticMeshActor>(FVector(I<2 ? (I==0?-140:140) : 0,I<2?0:240,50),FRotator::ZeroRotator,Params);
        Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
        Actor->SetActorLabel(I<2?FString::Printf(TEXT("编辑对象%d"),I+1):TEXT("共享原件_未选中"));
        Visual->Actors.Add(Actor);
    }
    GEditor->SelectNone(false,true,false);
    for(int32 I=0;I<2;++I) GEditor->SelectActor(Visual->Actors[I].Get(),true,false);
    GEditor->NoteSelectionChange();
    Visual->Client->SetViewLocation(FVector(550,-850,650));
    Visual->Client->SetViewRotation((FVector(0,40,50)-Visual->Client->GetViewLocation()).Rotation());
    Visual->Client->SetViewMode(VMI_Unlit);Visual->Client->SetRealtime(true);
    GLevelEditorModeTools().ActivateMode(FBlenderStyleEdMode::ModeId);
    if(Visual->bSelectionFixes || Visual->bTopology)
    {
        Visual->Client->SetViewLocation(FVector(250,-500,300));
        Visual->Client->SetViewRotation((FVector(0,40,50)-Visual->Client->GetViewLocation()).Rotation());
        if(!Mode() || !Mode()->BeginMeshEdit(Visual->Client,Visual->Client->Viewport))
        {Visual->Errors.Add(TEXT("Could not begin unselected edit visual"));FinishVisual();return;}
        Visual->Handle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickVisual),.05f);
        return;
    }
    if(!Mode() || !Mode()->StartTransform(EBlenderTransformOperation::Move,Visual->Client,Visual->Client->Viewport))
    {Visual->Errors.Add(TEXT("Could not start axis preview"));FinishVisual();return;}
    Press(EKeys::X);
    Visual->Handle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickVisual),.05f);
}

void RunBlenderControlsVisualVerification()
{
    if(Visual || !GEditor || !FParse::Param(FCommandLine::Get(),TEXT("BlenderControlsVerify"))) return;
    // ExecCmds runs during startup. Creating new mesh assets there can wait on startup
    // asset work while starving the editor tick; use the same deferred pattern as tests.
    static bool bQueued=false;
    if(bQueued)return;
    bQueued=true;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
    {
        static int32 Attempts=0;
        bool Ready=false;
        if(GEditor) for(auto* Client:GEditor->GetLevelViewportClients())
            Ready |= Client && Client->Viewport && Client->Viewport->GetSizeXY().GetMin()>0;
        auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        if((!Ready || Registry.IsLoadingAssets()) && ++Attempts<60)return true;
        bQueued=false;Attempts=0;
        StartBlenderControlsVisualVerification();
        return false;
    }),.5f);
}
