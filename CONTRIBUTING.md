# Contributing

Keep changes focused on Blender-style controls for the UE 5.4 level editor. Describe the behavior before and after a change, including a short reproduction when fixing a bug. Update the English overview and Chinese manual when controls, saving behavior, or support limits change.

## Development setup

Follow the [source installation steps](README.md#install-from-source), using a **separate disposable UE 5.4 C++ project** with the plugin at `Plugins/BlenderStyleControls/`. Build the project's **Development Editor / Win64** target with Visual Studio 2022 and the UE 5.4 C++ toolchain. Keep generated `Binaries`, `Intermediate`, `Saved`, local IDE state, and test reports out of source contributions.

Do not replace a plugin DLL while an editor has it loaded. Preserve source meshes, user modifications, and any unsaved work.

## Focused validation

Choose checks that cover the change. A documentation correction needs a documentation review; a rendering change needs the preview checks and relevant image inspection; a geometry change needs the affected geometry cases. Broaden testing only when dependencies or failures justify it.

**Never run these test commands inside an editor containing a user's work or unsaved changes.** Both verification paths create and discard temporary maps, reset editor state, and exit the process. Use a fresh process for the isolated test project. The `-BlenderControlsVerify` gate is a guard, not permission to use a working project.

Use `UnrealEditor.exe` with a real level viewport and Slate initialized, not `UnrealEditor-Cmd.exe` or a commandlet. Offscreen rendering still needs a working graphics device; do not use `-NullRHI` for rendering checks. The commands below are invocation templates, not a claim that a new test run has passed.

### Preview and viewport checks

Run this in a new process, replacing the project placeholder with the isolated project's `.uproject`:

```text
UnrealEditor.exe "<isolated-project>.uproject" -RenderOffscreen -BlenderControlsVerify -BlenderControlsPreviewTests -ExecCmds="BlenderControls.Verify"
```

Read `Saved/BlenderStyleControls/verification.json` under that project. Check the report's failures and the process exit status; merely opening the editor is not a passing test. `-BlenderControlsReport="<report-path>"` can override the report location.

### Offscreen preview images

Use a separate new process for image checks:

```text
UnrealEditor.exe "<isolated-project>.uproject" -RenderOffscreen -BlenderControlsVerify -BlenderControlsPreviewVisual -ExecCmds="BlenderControls.VisualVerify"
```

The default output folder is `Saved/BlenderStyleControls/VisualV2/`, including `visual_verification.json` and generated images. `-BlenderControlsVisualDir="<output-directory>"` can override it. Inspect the images relevant to the change as well as the report; offscreen checks do not test physical mouse or keyboard input.

### Other behaviors

The custom test entry points live in `Source/BlenderStyleControls/Private/`:

| Area | Test source |
|---|---|
| Editor integration and test dispatch | `BlenderStyleControlsTests.cpp` |
| Viewport shading and overlays | `BlenderViewportTests.cpp` |
| Rendered images | `BlenderControlsVisualTests.cpp` |
| Navigation | `BlenderNavigationTests.cpp` |
| Mesh editing | `BlenderMeshEditTests.cpp` |
| Selection | `BlenderSelectionTests.cpp` |
| Extrusion and loop cutting | `BlenderTopologyTests.cpp` |

Inspect the dispatcher when choosing a test set. `-BlenderControlsFocusedFixes` selects mesh, navigation, and topology checks. Without the preview or focused flag, `BlenderControls.Verify` runs the broader integration set. `-BlenderControlsSelectionVisual` and `-BlenderControlsTopologyVisual` select other image scenarios. Do not combine unrelated mode flags or assume these custom commands are standard Unreal Automation Framework tests.

## Invariants to preserve

- Temporary display effects apply only to registered level-editor viewports and exclude PIE, game rendering, and scene captures.
- Disabling the controls restores the previous viewport state. Preview lighting must not insert persistent lights or sky actors.
- Mesh commits create independent assets for changed objects and require normal user saving. Do not silently overwrite shared source meshes or auto-save user maps.
- Preserve confirm, cancel, and undo semantics, including zero-distance extrusion and centered loop-cut cancellation.
- Keep unsupported geometry checks and clear rejection messages. Do not advertise support from wireframe display alone.

## Pull requests

Include the engine version and platform, the relevant build/check results, and screenshots for visible changes. State what was not tested. Separate automatic checks, manual input checks, and visual acceptance; do not treat one as evidence of the others. Remove personal paths and private project details from attached logs and screenshots.

The publication review confirmed byte-for-byte agreement with the original installation for all 29 source files. All 31 preview textures loaded and exported stored source images in an isolated UE 5.4 content-only host. It checked source dimensions, sRGB, and compression: eight world TextureCubes with 1024 × 512 source images and 23 MatCap Texture2Ds at 512 × 512, all non-sRGB with HDR compression. All eight world textures used `TMGS_ANGULAR`. These checks cover asset loading and source export, not plugin viewport integration or rendering.

Prior 1.4.0 development records report a passing editor build, 41/41 targeted preview checks, and nine offscreen image checks. The publication review did not perform a new build, rerun those checks, test physical mouse/keyboard input, or package a game. A contribution must report its own relevant evidence rather than reuse historical results as a current pass.

Code contributions must be compatible with the repository's [MIT License](LICENSE). Keep asset provenance and licenses separate: bundled HDRI/MatCap images use the terms recorded in [RESOURCE_LICENSES.md](Resources/PreviewV14/RESOURCE_LICENSES.md). Do not include proprietary Unreal Engine source or unlicensed third-party assets.
