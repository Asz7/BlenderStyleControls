# Preview asset licenses and provenance / 预览资源授权与来源

The plugin's own source code is covered by the repository's MIT license.
The third-party images below retain their original CC0/public-domain terms.
Unreal Engine itself is not included and remains subject to Epic's licenses.

## Included images

The original images came from the Blender **4.3.2** distribution's
`datafiles/studiolights` directory. The accompanying upstream license texts
are preserved verbatim in [`licenses/`](licenses/).

| Group | Distributed files (relative to the repository root) | Upstream terms and attribution |
|---|---|---|
| HDRI (8) | `Content/PreviewV14/World/T_World_*.uasset` and `Resources/PreviewV14/world/*.png` | CC0. Created by **Greg Zaal / [Poly Haven](https://polyhaven.com/)**. See [world-license.txt](licenses/world-license.txt). |
| MatCap (23) | `Content/PreviewV14/MatCap/T_MatCap_*.uasset` and `Resources/PreviewV14/matcap/*.png` | The upstream text states **CC0 or public domain** and thanks the Blender community. It does not identify an author for each image. See [matcap-license.txt](licenses/matcap-license.txt). |

HDRI names: `city`, `courtyard`, `forest`, `interior`, `night`, `studio`,
`sunrise`, `sunset`.

MatCap names:

```text
basic_1
basic_2
basic_dark
basic_side
ceramic_dark
ceramic_lightbulb
check_normal+y (filename: check_normal_plus_y)
check_rim_dark
check_rim_light
clay_brown
clay_muddy
clay_studio
jade
metal_anisotropic
metal_carpaint
metal_lead
metal_shiny
pearl
reflection_check_horizontal
reflection_check_vertical
resin
skin
toon
```

The `.uasset` files and PNG thumbnails are derived from those images.
[`manifest.json`](manifest.json) records original source-image hashes,
relative Blender source paths, distributed file paths, and their hashes.
The source EXR/HDR files are not included in this repository.
The plugin does not require a local Blender installation to use these assets.

Blender's `.sl` studio-light data, extracted lighting parameters, preparation
scripts, and GPL C++/GLSL implementation files are **not distributed here**.
The plugin's three solid-lighting presets are implemented in its own shader
code. The HDRI named `studio` above is a CC0 image and is distinct from
Blender's `.sl` studio-light presets.

## 中文说明

本仓库只分发 8 张 HDRI、23 张 MatCap 对应的 UE 纹理及 PNG 缩略图。
HDRI 原许可为 CC0，作者为 Greg Zaal（Poly Haven）；MatCap 原许可写明为
CC0 或公有领域，并感谢 Blender 社区。两份原许可文本完整保留。

源码 MIT 许可不替代这些图像的原许可。来源清单保留原图及发布文件的
SHA-256，并采用相对路径。Blender 的 `.sl` 棚光数据、提取参数及 GPL
实现源码不在本仓库内；插件的三个实体棚光预设由插件自身代码实现。
