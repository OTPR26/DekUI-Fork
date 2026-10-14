# Frame interpolation

Interpolation preserves the native game update rate and adds intermediate rendered
frames. In **F1 > Graphics > Frame rate**, the options are:

- **Original**: the original game's 30fps.
- **60FPS CPU interpolation**: the default for new installations.
- **60FPS GPU interpolation**: uses hardware vertex shaders.

Both interpolation options use Vulkan. CPU interpolation processes vertices on the
CPU; it is not the software renderer. Selecting CPU interpolation disables hardware
shaders; selecting GPU interpolation enables them. Enabling either option enables
VSync. Coverage remains incomplete: some models and effects still animate at 30fps.
Performance depends on the scene, resolution and device.

## Implementation map

Paths below are relative to the repository root.

| Area | Files |
| --- | --- |
| Controls and defaults | `src/mm3d/overlay.cpp`, `src/mm3d/settings.cpp`, `src/mm3d/settings.h` |
| Camera midpoint | `src/mm3d/camera_midpoint.h` |
| Actor pose capture and interpolation | `src/mm3d/link_pose_capture.cpp`, `src/mm3d/link_interpolation.h` |
| Vertex replay | `src/video_core/renderer_vulkan/vk_rasterizer.cpp`, `src/mm3d/cpu_interpolation.h` |
| Presentation | `src/mm3d/prompt_layer.cpp`, `src/video_core/renderer_vulkan/renderer_vulkan.cpp` |
| Guarded replay scheduling | `src/video_core/renderer_vulkan/vk_scheduler.cpp` |
| Visibility depth | `src/mm3d/visibility_depth.h`, `src/video_core/renderer_vulkan/vk_rasterizer_cache.cpp`, `src/video_core/renderer_vulkan/vk_texture_runtime.cpp` |
| Performance capture | `src/mm3d/interpolation_performance.h`, `src/mm3d/interpolation_timeline.h` |

## Correctness constraints

Intermediate replay must not advance guest logic or input, or mutate native game
state. Matching a shader alone is insufficient to identify an actor or object.
Preserve HUD, stencil, transparency and framebuffer feedback ordering.

CPU and GPU interpolation share guarded batching to reduce render-target switches.
Retain captured resources until the appropriate drain, and drain before unknown
resource dependencies. Reordering independent draws does not authorize deferring
work past resource mutations.

The visibility-depth cache applies only to the guarded visibility routine in the
supported game. Other depth reads remain exact. A cached result must be no more
than one game frame and 100 milliseconds old, and must match the surface, scene
and generation. Missing, invalid or stale results fall back to exact reads.
This reuse can delay a visibility response. Background refresh follows a verified
query and stops in Original mode or when invalidated.

For diagnostic comparisons, the current opt-out switches are:

- `DEKUI_ASYNC_VISIBILITY_DEPTH=0`: disable asynchronous visibility-depth reuse.
- `DEKUI_REFRESH_VISIBILITY_DEPTH=0`: disable background refresh.
- `DEKUI_DEPTH_READBACK_PRIORITY=0`: disable depth-readback prioritization.

## Capturing a problem

See [the diagnostics instructions](deck/DIAGNOSTICS.md) for packaged builds.
Enable `DEKUI_DIAGNOSTICS=1` and use the F1 30-second performance capture. Compare
Original, CPU interpolation and GPU interpolation in the same scene with the same
resolution, camera movement and draw distance.

The developer editor's Camera tab also provides actor skeleton captures (10 seconds)
and song effect captures (20 seconds) for identifying missing animation coverage.

CPU timing scopes can overlap. GPU submission spans can include waits. Callback
intervals do not measure physical display scanout. Synthetic benchmark speedups
are not gameplay FPS predictions.

## Validation

Run the relevant focused tests from the repository root:

```sh
ctest --test-dir build-macos --output-on-failure -R 'mm3d_(camera_midpoint|cpu_vertex_replay|visibility_depth|interpolation_timeline|camera_writer_jit)'
```

With a built Vulkan fixture and access to a GPU, run:

```sh
./build-macos/bin/Release/mm3d_vulkan_fixture --pass-switching
./build-macos/bin/Release/mm3d_vulkan_fixture --visibility-depth
./build-macos/bin/Release/mm3d_vulkan_fixture --depth-readback --performance
./build-macos/bin/Release/mm3d_vulkan_fixture --skipped-frames
```

Use the corresponding build directory on other platforms. Fixtures do not replace
manual checks of visuals, audio speed, transitions and resource-heavy effects.
Report the actual hardware and settings tested; a passing Mac run does not establish
Steam Deck or Windows performance.
