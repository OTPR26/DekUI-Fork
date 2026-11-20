// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import GameController

@MainActor final class Prototype: ObservableObject {
    @Published var message = "0.4.4 data is separate."
    @Published var revision = 0
    private var core: OpaquePointer?
    private var observers: [NSObjectProtocol] = []
    private var active = true
    private let profile: URL
    var available: Bool { core != nil }
    init() {
        let root = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("ReMaskedNativePrototype", isDirectory: true)
        profile = root.appendingPathComponent("presentation.json")
        do {
            let source = FileManager.default.fileExists(atPath: profile.path) ? profile : Bundle.main.url(forResource: "presentation", withExtension: "json")!
            let data = try Data(contentsOf: source)
            guard data.count <= 1024 * 1024 else { throw CocoaError(.fileReadTooLarge) }
            core = data.withUnsafeBytes { bytes in
                rm_create(bytes.bindMemory(to: CChar.self).baseAddress, bytes.count)
            }
            guard core != nil else { throw CocoaError(.fileReadCorruptFile) }
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        } catch {
            message = "The prototype profile could not be loaded. It has been left unchanged."
        }
        for name in [NSNotification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.connectControllers() }
            })
        }
        connectControllers()
        #if REMASKED_NATIVE_NRI
        Task.detached {
            let ready = rm_renderer_device_check() == 1
            NSLog("ReMasked NRI/MoltenVK device: %@ (device-only)", ready ? "ready" : "unavailable")
        }
        #endif
        #if REMASKED_NATIVE_TITLE
        Task.detached {
            NSLog("ReMasked static MM3D title: %zu ABI-verified entries (not game boot)", rm_title_entry_count())
        }
        #endif
    }
    deinit {
        observers.forEach(NotificationCenter.default.removeObserver)
        rm_destroy(core)
    }
    func hud(_ index: Int) -> RMHudElement? {
        var item = RMHudElement()
        return rm_hud_element(core, UInt32(index), &item) == 1 ? item : nil
    }
    var hudBounds: RMHudElement {
        var item=RMHudElement();rm_hud_bounds(core,&item);return item
    }
    func changeScale(_ delta: Double) {
        let value = rm_hud_number(core, "player_hud_scale") + delta
        if rm_set_hud_number(core, "player_hud_scale", value) == 1 { save(); revision += 1 }
    }
    func save() {
        guard core != nil else { return }
        let size = rm_copy_config(core, nil, 0)
        guard size > 0 else { return }
        var bytes = [CChar](repeating: 0, count: size)
        guard rm_copy_config(core, &bytes, bytes.count) == size else { return }
        do {
            let data = bytes.withUnsafeBytes { Data($0.dropLast()) }
            try data.write(to: profile, options: .atomic)
            message = "Prototype profile saved."
        } catch { message = "Could not save the prototype profile; previous file retained." }
    }
    func button(_ index: Int, down: Bool, source: Int = 0) {
        rm_input_source_button(core, UInt32(source), UInt32(index), down ? 1 : 0); revision += 1
    }
    func tap(_ index: Int) { button(index, down: true); button(index, down: false) }
    func release() { rm_release_inputs(core); revision += 1 }
    func setActive(_ value: Bool) { active = value; if !value { release() } }
    var input: String { String(rm_input_mask(core), radix: 16) }
    private func connectControllers() {
        rm_release_source(core,1);rm_input_sticks(core,0,0,0,0);revision += 1
        for controller in GCController.controllers() {
            controller.extendedGamepad?.valueChangedHandler = nil
        }
        // Only the active pad owns hardware source 1; touch owns source 0.
        guard let pad = GCController.controllers().first?.extendedGamepad else { return }
        pad.valueChangedHandler = { [weak self] pad, _ in
            Task { @MainActor in
                guard let self, self.active else { return }
                let buttons: [(Int, GCControllerButtonInput?)] = [
                    (0,pad.buttonA),(1,pad.buttonB),(2,pad.buttonOptions),(3,pad.buttonMenu),
                    (4,pad.dpad.right),(5,pad.dpad.left),(6,pad.dpad.up),(7,pad.dpad.down),
                    (8,pad.rightShoulder),(9,pad.leftShoulder),(10,pad.buttonX),(11,pad.buttonY),
                    (14,pad.leftTrigger),(15,pad.rightTrigger)]
                for (index, button) in buttons { self.button(index, down: button?.isPressed ?? false, source: 1) }
                rm_input_sticks(self.core,pad.leftThumbstick.xAxis.value,pad.leftThumbstick.yAxis.value,
                                pad.rightThumbstick.xAxis.value,pad.rightThumbstick.yAxis.value)
            }
        }
    }
}

struct HUDPreview: View {
    @ObservedObject var prototype: Prototype
    var active = true
    let labels = ["D-pad","Magic","Hearts","Items","Map","Clock","Rupees","Counter","Carrots","Timer","X","Y","I","II","A","B"]
    var body: some View {
        Canvas { context, size in
            let bounds=prototype.hudBounds
            guard bounds.width>0, bounds.height>0 else { return }
            let scale = min(size.width / Double(bounds.width), size.height / Double(bounds.height))
            context.scaleBy(x: scale, y: scale)
            context.translateBy(x: -Double(bounds.x), y: -Double(bounds.y))
            for index in 0..<16 {
                guard let h = prototype.hud(index), h.visible != 0 else { continue }
                let box = CGRect(x: Double(h.x), y: Double(h.y), width: Double(h.width), height: Double(h.height))
                context.fill(Path(roundedRect: box, cornerRadius: 3), with: .color(.mint.opacity(Double(h.opacity) * 0.65)))
                context.draw(Text(labels[index]).font(.system(size: 9)).foregroundStyle(.white),
                             at: CGPoint(x: box.minX + 2, y: box.minY + min(9, box.height / 2)), anchor: .leading)
            }
        }
        .id(prototype.revision)
        .aspectRatio(5.0 / 3.0, contentMode: .fit)
        .background(NativeRenderSurface(active: active))
        .accessibilityLabel("HUD layout preview")
    }
}

struct PrototypeScreen: View {
    @StateObject private var prototype = Prototype()
    @Environment(\.scenePhase) private var scenePhase
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Majora ReMasked · Native Prototype").font(.title2.bold())
            Text("HUD layout preview · Game rendering and model mods are not connected yet.").font(.callout)
            HUDPreview(prototype: prototype, active: scenePhase == .active).frame(maxWidth: 900, maxHeight: .infinity)
            HStack {
                Button("HUD smaller") { prototype.changeScale(-1) }
                Button("HUD larger") { prototype.changeScale(1) }
                Button("Save profile") { prototype.save() }
            }.disabled(!prototype.available)
            HStack {
                ForEach(Array(zip(["Select","Start","A","B"],[2,3,0,1])), id: \.0) { name, index in
                    #if os(tvOS)
                    Button(name) { prototype.tap(index) }
                    #else
                    Text(name).padding(.horizontal,24).padding(.vertical,12).background(.gray.opacity(0.25)).clipShape(Capsule())
                        .gesture(DragGesture(minimumDistance: 0).onChanged { _ in prototype.button(index, down: true) }
                            .onEnded { _ in prototype.button(index, down: false) })
                        .accessibilityAddTraits(.isButton)
                        .accessibilityAction { prototype.tap(index) }
                    #endif
                }
                Text("Input: \(prototype.input)").font(.caption.monospaced())
            }.disabled(!prototype.available)
            Text(prototype.message).font(.caption)
        }
        .padding()
        .onChange(of: scenePhase) { phase in prototype.setActive(phase == .active) }
    }
}
@main struct ReMaskedPrototypeApp: App {
    var body: some Scene { WindowGroup { PrototypeScreen() } }
}
