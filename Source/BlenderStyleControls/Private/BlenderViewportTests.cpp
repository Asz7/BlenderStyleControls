#include "BlenderViewportTests.h"
#include "BlenderStyleEdMode.h"
#include "BlenderViewportPresentation.h"
#include "BlenderPreviewLighting.h"
#include "BlenderMeshEditSession.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/TextureCube.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "PreviewScene.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "FinalPostProcessSettings.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"

void AppendBlenderViewportVerification(FEditorViewportClient* Client, FViewport* Viewport,
    TArray<TSharedPtr<FJsonValue>>& Checks, int32& Failures)
{
    auto Check = [&](const TCHAR* Name, bool bPassed, const FString& Detail = TEXT("Editor mode input routing; no physical OS input."))
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Name);
        Entry->SetBoolField(TEXT("passed"), bPassed);
        Entry->SetStringField(TEXT("details"), Detail);
        Checks.Add(MakeShared<FJsonValueObject>(Entry));
        Failures += !bPassed;
    };
    auto& Modes = GLevelEditorModeTools();
    const FEngineShowFlags OriginalFlags = Client->EngineShowFlags;
    const auto OriginalViewMode = Client->GetViewMode();
    Modes.ActivateMode(FBlenderStyleEdMode::ModeId);
    auto* Mode = static_cast<FBlenderStyleEdMode*>(Modes.GetActiveMode(FBlenderStyleEdMode::ModeId));
    if (!Mode || !Client || !Viewport) { Check(TEXT("viewport12.mode_available"), false); return; }
    Check(TEXT("viewport12.B_starts_box_selection"), Mode->InputKey(Client, Viewport, EKeys::B, IE_Pressed));
    Mode->InputKey(Client, Viewport, EKeys::Escape, IE_Pressed);
    Check(TEXT("viewport12.escape_cancels_box_without_leaking_modal"), !Mode->IsSelecting());
    Check(TEXT("viewport12.middle_button_owned_by_Blender_navigation"), Mode->InputKey(Client, Viewport, EKeys::MiddleMouseButton, IE_Pressed));
    Mode->InputKey(Client, Viewport, EKeys::B, IE_Pressed);
    Check(TEXT("viewport12.B_during_navigation_cannot_steal_release"), !Mode->IsSelecting());
    Mode->InputKey(Client, Viewport, EKeys::MiddleMouseButton, IE_Released);
    const FVector LocationAfterNav = Client->GetViewLocation();
    Check(TEXT("viewport12.released_navigation_does_not_keep_moving"), !Mode->MouseMove(Client, Viewport, 555, 333)
        && Client->GetViewLocation().Equals(LocationAfterNav));
    Mode->InputKey(Client, Viewport, EKeys::LeftShift, IE_Pressed);
    Mode->InputKey(Client, Viewport, EKeys::B, IE_Pressed);
    Mode->InputKey(Client, Viewport, EKeys::LeftShift, IE_Released);
    Mode->InputKey(Client, Viewport, EKeys::Escape, IE_Pressed);
    const FRotator BeforeReleasedModifier = Client->GetViewRotation();
    Mode->InputKey(Client, Viewport, EKeys::MiddleMouseButton, IE_Pressed);
    Mode->MouseMove(Client, Viewport, Viewport->GetMouseX() + 20, Viewport->GetMouseY());
    Mode->InputKey(Client, Viewport, EKeys::MiddleMouseButton, IE_Released);
    Check(TEXT("viewport12.modifier_released_during_box_is_not_latched_for_navigation"), !Client->GetViewRotation().Equals(BeforeReleasedModifier));
    auto Widget = Client->GetEditorViewportWidget();
    auto SceneViewport = Widget.IsValid() ? Widget->GetSceneViewport() : nullptr;
    bool bXRayHandled = false;
    if (SceneViewport.IsValid())
    {
        const FGeometry Geometry = SceneViewport->GetCachedGeometry();
        const FModifierKeysState Alt(false, false, false, false, true, false, false, false, false);
        SceneViewport->OnKeyDown(Geometry, FKeyEvent(EKeys::LeftAlt, Alt, 0, false, 0, 0));
        bXRayHandled = Mode->InputKey(Client, Viewport, EKeys::Z, IE_Pressed);
        Mode->InputKey(Client, Viewport, EKeys::Z, IE_Released);
        SceneViewport->OnKeyUp(Geometry, FKeyEvent(EKeys::LeftAlt, FModifierKeysState(), 0, false, 0, 0));
    }
    Check(TEXT("viewport12.alt_z_owned_by_xray"), bXRayHandled);
    if (auto* Display = Mode->GetPresentation())
    {
        Check(TEXT("viewport12.alt_z_toggled_solid_xray"), Display->IsXRay());
        auto* XRayMaterial = Display->GetPreviewMaterial();
        Check(TEXT("viewport12.xray_material_is_transient_unlit_translucent"), XRayMaterial
            && XRayMaterial->HasAnyFlags(RF_Transient) && XRayMaterial->GetBlendMode() == BLEND_Translucent
            && XRayMaterial->GetShadingModels().HasShadingModel(MSM_Unlit));
        Check(TEXT("viewport12.xray_material_ready_before_preview_assignment"), XRayMaterial
            && XRayMaterial->GetMaterialResource(Client->GetWorld()->GetFeatureLevel())
            && XRayMaterial->GetMaterialResource(Client->GetWorld()->GetFeatureLevel())->IsCompilationFinished());
        Check(TEXT("viewport12.selection_rectangle_HUD_enabled"), Client->EngineShowFlags.ModeWidgets);
        const auto SavedOverlays=Display->GetOverlaySettings();
        auto OverlaysOff=SavedOverlays;OverlaysOff.bShowOverlays=false;
        Display->SetOverlaySettings(OverlaysOff);
        Check(TEXT("viewport14.overlay_master_hides_object_xray_mesh_edges"),Display->IsXRay()&&!Client->EngineShowFlags.MeshEdges);
        Display->SetOverlaySettings(SavedOverlays);
        Check(TEXT("viewport14.restoring_overlays_restores_object_xray_mesh_edges"),Display->IsXRay()&&Client->EngineShowFlags.MeshEdges);
        Display->SetShading(EBlenderViewportShading::Material);
        Check(TEXT("viewport14.material_preview_uses_lit_original_materials"), !Display->CanXRay() && !Display->IsXRay()
            && Client->GetViewMode() == VMI_Lit && Client->EngineShowFlags.Materials && Client->EngineShowFlags.Lighting,
            FString::Printf(TEXT("mode=%d materials=%d lighting=%d canxray=%d xray=%d"), int32(Client->GetViewMode()),
                int32(Client->EngineShowFlags.Materials), int32(Client->EngineShowFlags.Lighting), Display->CanXRay(), Display->IsXRay()));
        // Use the editor's real view-extension gathering path: a toolbar flag alone does
        // not prove that an environment reaches the rendered view.
        const auto SavedPreviewSettings = Display->GetPreviewSettings();
        auto PreviewSettings = SavedPreviewSettings;
        PreviewSettings.WorldStem = TEXT("forest");
        PreviewSettings.bUseSceneLights = false;
        PreviewSettings.bUseSceneWorld = false;
        Display->SetPreviewSettings(PreviewSettings);
        auto* Lighting = Display->GetLighting();
        Check(TEXT("viewport14.preview_resources_and_shader_ready"), Lighting && Lighting->IsMaterialReady()
            && Lighting->HasSelectedWorldTexture() && Lighting->HasSelectedMatCapTexture(),
            Lighting ? Lighting->GetStatusText() : TEXT("No preview lighting owner"));
        if (Lighting)
        {
            FPreviewScene ForeignScene{FPreviewScene::ConstructionValues()};
            FEditorViewportClient ForeignClient(nullptr, &ForeignScene);
            Check(TEXT("viewport14.only_registered_level_viewport_is_eligible"), Lighting->IsRegisteredViewport(Client)
                && !Lighting->IsRegisteredViewport(nullptr) && !Lighting->IsRegisteredViewport(&ForeignClient));
        }
        auto InspectPreviewView = [&](const TCHAR* Name, bool bExpectEnvironment, bool bExpectSceneLights)
        {
            FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport, Client->GetScene(), Client->EngineShowFlags));
            Family.ViewMode = Client->GetViewMode();
            Family.ViewExtensions = GEngine->ViewExtensions->GatherActiveExtensions(FSceneViewExtensionContext(Viewport));
            for (auto& Extension : Family.ViewExtensions) Extension->SetupViewFamily(Family);
            const FSceneView* View = Client->CalcSceneView(&Family);
            bool bHasEnvironment = false;
            if (View) for (const auto& Entry : View->FinalPostProcessSettings.ContributingCubemaps)
                bHasEnvironment |= Entry.AmbientCubemap && Entry.AmbientCubemap->GetName() == TEXT("T_World_forest");
            Check(Name, View && bHasEnvironment == bExpectEnvironment
                && bool(Family.EngineShowFlags.DirectLighting) == bExpectSceneLights,
                FString::Printf(TEXT("view=%d forest_cube=%d direct_lights=%d"), View != nullptr,
                    bHasEnvironment, int32(Family.EngineShowFlags.DirectLighting)));
        };
        InspectPreviewView(TEXT("viewport14.HDRI_reaches_final_view_without_scene_lights"), true, false);
        PreviewSettings.bUseSceneLights = true;
        Display->SetPreviewSettings(PreviewSettings);
        InspectPreviewView(TEXT("viewport14.scene_light_toggle_reaches_final_view"), true, true);
        PreviewSettings.bUseSceneWorld=true;
        Display->SetPreviewSettings(PreviewSettings);
        InspectPreviewView(TEXT("viewport14.scene_world_uses_original_environment"),false,true);
        auto InspectWorldFlags=[&](const TCHAR* Name,bool bSceneWorld)
        {
            FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport,Client->GetScene(),Client->EngineShowFlags));
            Family.ViewExtensions=GEngine->ViewExtensions->GatherActiveExtensions(FSceneViewExtensionContext(Viewport));
            for(auto& Extension:Family.ViewExtensions)Extension->SetupViewFamily(Family);
            const auto& Flags=Family.EngineShowFlags;
            const bool bMatches=bSceneWorld
                ? Flags.SkyLighting==OriginalFlags.SkyLighting && Flags.Fog==OriginalFlags.Fog
                    && Flags.Atmosphere==OriginalFlags.Atmosphere && Flags.ReflectionEnvironment==OriginalFlags.ReflectionEnvironment
                : !Flags.SkyLighting && !Flags.Fog && !Flags.Atmosphere && !Flags.ReflectionEnvironment;
            Check(Name,bMatches,FString::Printf(TEXT("sky=%d fog=%d atmosphere=%d reflection=%d"),int32(Flags.SkyLighting),int32(Flags.Fog),int32(Flags.Atmosphere),int32(Flags.ReflectionEnvironment)));
        };
        InspectWorldFlags(TEXT("viewport14.scene_world_restores_original_sky_flags"),true);
        PreviewSettings.bUseSceneWorld=false;
        Display->SetPreviewSettings(PreviewSettings);
        InspectPreviewView(TEXT("viewport14.return_to_HDRI_reinjects_preview_environment"),true,true);
        InspectWorldFlags(TEXT("viewport14.return_to_HDRI_disables_scene_sky_flags"),false);
        Display->SetShading(EBlenderViewportShading::Rendered);
        InspectPreviewView(TEXT("viewport14.rendered_does_not_inject_preview_environment"), false, true);
        Display->SetPreviewSettings(SavedPreviewSettings);
        int32 LoadedWorlds = 0;
        for (const TCHAR* Name : {TEXT("city"), TEXT("courtyard"), TEXT("forest"), TEXT("interior"), TEXT("night"), TEXT("studio"), TEXT("sunrise"), TEXT("sunset")})
        {
            const FString AssetName = FString(TEXT("T_World_")) + Name;
            const FString AssetPath = TEXT("/BlenderStyleControls/PreviewV14/World/") + AssetName + TEXT(".") + AssetName;
            LoadedWorlds += LoadObject<UTextureCube>(nullptr, *AssetPath) != nullptr;
        }
        Check(TEXT("viewport14.eight_bundled_HDRIs_are_cubemaps"), LoadedWorlds == 8,
            FString::Printf(TEXT("loaded_world_cubemaps=%d"), LoadedWorlds));
        Display->SetShading(EBlenderViewportShading::Rendered);
        Check(TEXT("viewport12.rendered_uses_scene_lighting"), Client->GetViewMode() == VMI_Lit && Client->EngineShowFlags.Lighting);
        Display->SetShading(EBlenderViewportShading::Material);
        Display->SetShading(EBlenderViewportShading::Wireframe);
        if (SceneViewport.IsValid())
        {
            const FModifierKeysState Shift(true, false, false, false, false, false, false, false, false);
            SceneViewport->OnKeyDown(SceneViewport->GetCachedGeometry(), FKeyEvent(EKeys::LeftShift, Shift, 0, false, 0, 0));
            Mode->InputKey(Client, Viewport, EKeys::Z, IE_Pressed);
            SceneViewport->OnKeyUp(SceneViewport->GetCachedGeometry(), FKeyEvent(EKeys::LeftShift, FModifierKeysState(), 0, false, 0, 0));
        }
        Check(TEXT("viewport12.shift_z_restores_last_mode_even_after_toolbar_change"), Display->GetShading() == EBlenderViewportShading::Material);
        Display->SetShading(EBlenderViewportShading::Wireframe);
        const bool bWireInitiallyOn = Display->IsXRay(); Display->ToggleXRay();
        Display->SetShading(EBlenderViewportShading::Solid);
        Check(TEXT("viewport12.wire_and_solid_remember_independent_xray"), bWireInitiallyOn && Display->IsXRay());
        Display->SetShading(EBlenderViewportShading::Wireframe);
        Check(TEXT("viewport12.return_to_wire_restores_its_own_toggle"), !Display->IsXRay());
        // Exercise this plugin's lifecycle callbacks only. Global EndPIE listeners require a
        // real play world and must not be broadcast by an isolated editor-only fixture.
        Mode->HandlePreBeginPIE(false);
        Mode->HandleEndPIE(false);
        Modes.Tick(Client, .016f);
        Check(TEXT("viewport12.viewport_display_reactivates_after_PIE"), Client->GetViewMode() == VMI_Wireframe);

        // The gesture path uses a real cached SceneViewport position and the mode's mouse routing.
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        TArray<AStaticMeshActor*> Actors;
        if (Cube && SceneViewport.IsValid())
        {
            GEditor->SelectNone(false, true, false);
            for (int32 I = 0; I < 2; ++I)
            {
                FActorSpawnParameters Params; Params.ObjectFlags |= RF_Transient;
                auto* Actor = Client->GetWorld()->SpawnActor<AStaticMeshActor>(FVector(I ? 120 : -120, 0, 0), FRotator::ZeroRotator, Params);
                if (Actor) { Actor->GetStaticMeshComponent()->SetStaticMesh(Cube); Actors.Add(Actor); GEditor->SelectActor(Actor, true, false); }
            }
            GEditor->NoteSelectionChange();
            Display->SetShading(EBlenderViewportShading::Solid);
            if (!Display->IsXRay()) Display->ToggleXRay();
            Client->SetViewportType(LVT_Perspective);
            Client->SetViewLocation(FVector(0, -800, 400));
            Client->SetViewRotation((-Client->GetViewLocation()).Rotation());
            if (Mode->BeginMeshEdit(Client, Viewport))
            {
                auto* Session = Mode->GetMeshEditSession(); Session->SelectAll(false);
                auto Move = [&](int32 X, int32 Y)
                {
                    const FGeometry Geometry = SceneViewport->GetCachedGeometry();
                    const FVector2D Absolute = Geometry.LocalToAbsolute(FVector2D(X, Y) / Geometry.Scale);
                    const FPointerEvent Pointer(0, Absolute, Absolute, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState());
                    SceneViewport->OnMouseMove(Geometry, Pointer);
                    Mode->MouseMove(Client, Viewport, X, Y);
                };
                Move(1, 1); Mode->InputKey(Client, Viewport, EKeys::B, IE_Pressed);
                Mode->InputKey(Client, Viewport, EKeys::LeftMouseButton, IE_Pressed);
                Move(Viewport->GetSizeXY().X - 2, Viewport->GetSizeXY().Y - 2);
                Mode->InputKey(Client, Viewport, EKeys::LeftMouseButton, IE_Released);
                int32 Expected = 0;
                for (int32 Object = 0; Object < Session->GetObjectCount(); ++Object) Expected += Session->GetElementIDs(Object, EBlenderMeshSelectionMode::Vertex).Num();
                Check(TEXT("viewport12.B_drag_selects_rear_vertices_across_two_meshes"), !Mode->IsSelecting()
                    && Expected > 0 && Session->GetSelectedVertices().Num() == Expected && !Session->HasChanges());
                Mode->InputKey(Client, Viewport, EKeys::B, IE_Pressed);
                Mode->InputKey(Client, Viewport, EKeys::Escape, IE_Pressed);
                Check(TEXT("viewport12.canceled_box_preserves_geometry_selection"), Mode->IsMeshEditing()
                    && !Mode->IsSelecting() && Session->GetSelectedVertices().Num() == Expected);
                Mode->EndMeshEdit(false);
            }
            else Check(TEXT("viewport12.box_fixture_begins_mesh_edit"), false);
        }
        else Check(TEXT("viewport12.box_fixture_available"), false);
        GEditor->SelectNone(false, true, false);
        for (auto* Actor : Actors) Client->GetWorld()->DestroyActor(Actor);
    }
    Modes.DeactivateMode(FBlenderStyleEdMode::ModeId);
    // A rapid off/on happens before UE's deferred Exit. This must still reattach the toolbar.
    Modes.ActivateMode(FBlenderStyleEdMode::ModeId);
    Mode = static_cast<FBlenderStyleEdMode*>(Modes.GetActiveMode(FBlenderStyleEdMode::ModeId));
    if (Mode && Mode->GetPresentation())
    {
        Mode->GetPresentation()->SetShading(EBlenderViewportShading::Material);
        Check(TEXT("viewport14.rapid_off_on_reactivates_lit_material_preview"), Client->GetViewMode() == VMI_Lit && Client->EngineShowFlags.Materials);
    }
    else Check(TEXT("viewport12.rapid_off_on_reactivates_presentation"), false);
    Modes.DeactivateMode(FBlenderStyleEdMode::ModeId);
    Modes.Tick(Client, .016f);
    Check(TEXT("viewport12.disable_restores_previous_display"), Client->GetViewMode() == OriginalViewMode
        && Client->EngineShowFlags.ToString() == OriginalFlags.ToString(),
        FString::Printf(TEXT("mode=%d original=%d flags_equal=%d"), int32(Client->GetViewMode()), int32(OriginalViewMode),
            Client->EngineShowFlags.ToString() == OriginalFlags.ToString()));
    FSceneViewFamilyContext RestoredFamily(FSceneViewFamily::ConstructionValues(Viewport, Client->GetScene(), Client->EngineShowFlags));
    RestoredFamily.ViewExtensions=GEngine->ViewExtensions->GatherActiveExtensions(FSceneViewExtensionContext(Viewport));
    for(auto& Extension:RestoredFamily.ViewExtensions)Extension->SetupViewFamily(RestoredFamily);
    const FSceneView* RestoredView=Client->CalcSceneView(&RestoredFamily);
    bool bPluginEnvironmentRemaining=false;
    if(RestoredView)for(const auto& Entry:RestoredView->FinalPostProcessSettings.ContributingCubemaps)
        bPluginEnvironmentRemaining|=Entry.AmbientCubemap && Entry.AmbientCubemap->GetPathName().StartsWith(TEXT("/BlenderStyleControls/"));
    Check(TEXT("viewport14.disable_removes_preview_from_actual_view"),RestoredView && !bPluginEnvironmentRemaining);
}
