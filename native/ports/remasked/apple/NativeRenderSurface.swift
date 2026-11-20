// SPDX-License-Identifier: GPL-3.0-or-later
import MetalKit
import SwiftUI

// A real drawable-backed surface below the retained HUD. The native game's
// Vulkan/MoltenVK submission bridge is still a separate qualification gate.
struct NativeRenderSurface: UIViewRepresentable {
    var active: Bool
    func makeCoordinator() -> Renderer { Renderer() }
    func makeUIView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0.06, green: 0.09, blue: 0.11, alpha: 1)
        view.framebufferOnly = true
        view.preferredFramesPerSecond = 60
        view.isPaused = !active
        view.delegate = context.coordinator
        context.coordinator.queue = view.device?.makeCommandQueue()
        view.isUserInteractionEnabled = false
        return view
    }
    func updateUIView(_ view: MTKView, context: Context) { view.isPaused = !active }
    static func dismantleUIView(_ view: MTKView, coordinator: Renderer) {
        view.isPaused = true
        view.delegate = nil
        coordinator.queue = nil
    }
    final class Renderer: NSObject, MTKViewDelegate {
        var queue: MTLCommandQueue?
        private var reported = false
        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
        func draw(in view: MTKView) {
            guard let pass = view.currentRenderPassDescriptor,
                  let drawable = view.currentDrawable,
                  let command = queue?.makeCommandBuffer(),
                  let encoder = command.makeRenderCommandEncoder(descriptor: pass) else { return }
            encoder.endEncoding()
            command.present(drawable)
            if !reported {
                reported = true
                command.addCompletedHandler { command in
                    NSLog("ReMasked render surface: %@ (clear-frame only)",
                          command.status == .completed ? "GPU completed" : "GPU failed")
                }
            }
            command.commit()
        }
    }
}
