# Frame interpolation

The guest simulation runs at its native cadence. CPU and GPU interpolation modes
insert presentation frames between native frames without advancing guest logic,
input, or game time. Original mode presents the native frames.

## Rendering paths

- `camera_midpoint.h` selects consecutive camera samples and rejects discontinuities.
- `cpu_interpolation.h` replays captured vertices with interpolated transforms.
- `link_interpolation.h` matches actor poses by model identity and skeleton layout.
- The `*_interpolation.h` helpers handle specific UI, water, and particle materials.
- `vk_rasterizer.cpp` applies the selected transforms and preserves draw ordering.
- `renderer_vulkan.cpp` and `prompt_layer.cpp` schedule presentation frames.

## Safety rules

Interpolation requires consecutive samples from the same scene and generation.
Ambiguous model ownership, camera transitions, animation restarts, and missing
samples fall back to native data. First-person aiming uses the native camera
handover so aiming and releasing do not replay a stale follow-camera transform.
Preserve stencil, transparency, framebuffer feedback, and HUD ordering.

The fork keeps the ordinary guest depth-read path. The experimental visibility
readback cache is not enabled.

## Validation

Run the native camera, vertex replay, actor pose, and material interpolation tests
with CTest. Automated checks establish matching and fallback behavior; gameplay
checks must also cover camera pans, aiming, transformations, scene transitions,
menus, and custom player models in Original and interpolated modes.

Developer captures are written to the local presentation-settings directory.
Keep captures and performance reports out of release packages: they may contain
local paths and gameplay state. Compare native and intermediate frames using
sample timestamps and scene identity rather than frame IDs alone.
