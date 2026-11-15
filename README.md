# Blender Style Controls

Blender-style navigation, transforms, mesh editing, and viewport shading for the **Unreal Engine 5.4 level editor on Windows (Win64)**. Version **1.4.0** is an editor-only plugin; Blender does not need to be installed.

[详细中文操作说明](README.zh-CN.md) · [Contributing and testing](CONTRIBUTING.md)

The current toolbar, menus, and help messages are in Chinese. The main toggle is **Blender 操作：关** (off) / **Blender 操作：开** (on). Controls start disabled each editor session.

## Features

- Middle-mouse orbit, pan, zoom, framing, and numpad views.
- `G` / `R` / `S` transforms, axis constraints, numeric input, snapping, duplication, and primitive creation.
- Multi-object vertex, edge, and face editing for supported static mesh actors.
- Box, loop, ring, shortest-path, and connected selection; X-Ray selection.
- Face-region extrusion and a single quad loop cut with sliding.
- Solid studio lighting, 23 MatCaps, eight HDRI material-preview environments, and configurable overlays.

## Install from source

Requirements: **UE 5.4**, **Windows x64**, and **Visual Studio 2022 with the UE 5.4 C++ toolchain** (Game development with C++, a compatible MSVC toolset, and Windows SDK). Enable the **Modeling Tools Editor Mode** plugin (`MeshModelingToolset`), which is declared as a dependency.

1. Save your work and close the target project's editor before installing or replacing the plugin.
2. Put this **entire repository** at `<YourProject>/Plugins/BlenderStyleControls/`. The descriptor must be directly at `Plugins/BlenderStyleControls/BlenderStyleControls.uplugin`, alongside `Source`, `Content`, and `Resources`; avoid an extra nested repository folder.
3. Use a C++ project. For a Blueprint-only project, first add an empty C++ class through Unreal Editor to generate the project build targets, then close the editor.
4. Generate Visual Studio project files from the `.uproject`. Open the solution, select **Development Editor** and **Win64**, and build the project's Editor target.
5. Open the project in UE 5.4. Under **Edit → Plugins**, confirm **Blender Style Controls** and its modeling dependency are enabled; restart if prompted.
6. Click **Blender 操作：关** in the level editor toolbar to enable the controls.

For step 2, you can optionally clone from your project root:

```text
git clone https://github.com/Asz7/BlenderStyleControls.git Plugins/BlenderStyleControls
```

The repository is a source distribution. Build the plugin locally; a successful build on another UE version or platform is not claimed.

## Quick use

Keep the pointer inside the level viewport. Disabling the plugin restores UE navigation and shortcuts; text fields and Blueprint editors are not taken over.

| Action | Control |
|---|---|
| Orbit / pan / zoom | Middle drag / `Shift` + middle drag / wheel or `Ctrl` + middle drag |
| Frame selection / all | Numpad `.` / `Home` |
| Front / right / top | Numpad `1` / `3` / `7`; add `Ctrl` for the opposite side |
| Move / rotate / scale | `G` / `R` / `S`, then optional axis and numeric input |
| Confirm / cancel transform | `Enter` or left click / `Esc` or right click |
| Add primitive / duplicate actor | `Shift+A` / `Shift+D` |
| Enter mesh edit / commit mesh edit | `Tab` |
| Vertex / edge / face selection | `1` / `2` / `3` in mesh edit |
| Select all / clear selection | `A` / `Alt+A` |
| Box selection | `B`, then drag; or drag directly |
| Face-region extrusion / extrusion menu | `E` / `Alt+E` |
| Single quad loop cut | `Ctrl+R`, click to insert, then click or `Enter` to finish sliding |
| Shading menu / wireframe / X-Ray | `Z` / `Shift+Z` / `Alt+Z` |

For a first edit, use `Shift+A` to add a cube, press `Tab`, press `3`, select a face, and enter `E → 25 → Enter`. Press `Tab` to commit, then save. Movement and extrusion distances are in **centimeters**, angles in degrees, and scale values are multipliers.

Cancellation depends on the active operation:

- `Esc` during an ordinary transform cancels that transform. Outside an active operation, `Esc` in mesh edit discards the whole uncommitted mesh-edit session.
- `Esc` during extrusion cancels movement **but keeps the zero-distance extrusion**. Use `Ctrl+Z` to remove the extrusion.
- `Esc` during loop-cut preview inserts nothing. After insertion, it centers the cut and keeps it; use `Ctrl+Z` to remove it.

See the [Chinese manual](README.zh-CN.md) for selection combinations, local/world axis behavior, and the complete shortcut reference.

## Data and saving

Mesh editing uses temporary previews. Committing with `Tab` creates a separate static mesh **only for each changed object**, under `/Game/BlenderEdits/`, and assigns it to that actor. The original mesh asset and other actors sharing it remain unchanged. Generated cubes use `/Game/BlenderPrimitives/`.

**Commit first, then save the new assets and level** with UE's normal save workflow (`Ctrl+Shift+S`). The plugin does not automatically save them. Undo after committing restores the actor's previous mesh reference but does not delete the generated asset. Disabling the plugin, changing maps or editor modes, or starting play discards an uncommitted mesh-edit session.

Shading and overlay settings are session-only viewport previews. They do not create scene lights or sky actors, replace saved mesh materials, or save the level. Disabling the plugin restores the previous viewport display. These preview effects exclude PIE, game rendering, and scene captures.

## Scope and limitations

- **UE 5.4 / Win64 / editor only.** Other engine versions and platforms are unverified. Preview shading requires the deferred rendering path.
- Mesh editing supports eligible ordinary static mesh actors. Skeletal meshes, Blueprint assemblies, multiple geometry components, individual ISM/HISM instances, multiple LODs, separate high-resolution source meshes, missing editable source geometry, and per-instance vertex paint are unsupported. Unsupported selections are rejected as a group.
- Source quad topology is required for loop cuts; triangulated imports are not automatically converted back to quads. Extrusion operates on faces, and loop cutting inserts one loop at a time.
- Rebuilding geometry recalculates normals and tangents. Generated meshes use their edited surface for collision; create simple collision before using them for physics simulation.
- This is not a port of Blender's Workbench renderer or complete modeling toolset. Cavity and outline effects are screen-space approximations. Rotating the HDRI background does **not** rotate the lighting.
- Special `BeforeDOF` translucent materials can be covered by the preview background; enable **Scene World** to view the original world. Solid X-Ray keeps the translucent editing preview without studio/MatCap shading.
- UE 5.4 orthographic views snap to the nearest axis. Actor-locked or piloted cameras are not controlled.

The [Chinese manual](README.zh-CN.md) documents the full support conditions and rendering caveats.

## Validation status

The publication review confirmed that all **29 source files** match the original installation byte for byte. All **31 preview textures** loaded and exported their stored source images successfully in an isolated **UE 5.4 content-only host**. Source dimensions, sRGB, and compression settings matched expectations: eight world TextureCubes at 1024 × 512 source size and 23 MatCap Texture2Ds at 512 × 512, all non-sRGB with HDR compression; all eight world textures used `TMGS_ANGULAR`. This verifies asset loading and source export, not plugin viewport rendering.

Prior 1.4.0 development validation recorded a successful Windows editor build, **41/41 targeted preview checks**, and **nine offscreen viewport image checks**. The publication review did **not** perform a new build, rerun those checks, test physical mouse/keyboard input, or package a game. Cross-platform compatibility remains unverified. See [CONTRIBUTING.md](CONTRIBUTING.md) for the isolated test workflow.

## License

The plugin source code is licensed under the **MIT License**; see [LICENSE](LICENSE).

Bundled HDRI and MatCap images and their derived preview assets have **CC0/public-domain** provenance, separate from the code license. See [resource licenses and attribution](Resources/PreviewV14/RESOURCE_LICENSES.md) and the preserved upstream license texts for details.

Unreal Engine and its dependencies remain subject to Epic's applicable licenses. This repository does not include Unreal Engine source code or grant rights to it. Blender GPL implementation code is not included. This is an independent community project, not an official Epic Games or Blender Foundation product.
