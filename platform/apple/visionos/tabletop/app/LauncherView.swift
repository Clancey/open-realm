import SwiftUI

struct TabletopLauncherView: View {
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @State private var state = TabletopLauncherState.waiting
    @ObservedObject var model: TabletopSessionModel

    var body: some View {
        VStack(spacing: 18) {
            Text("Open Realm Tabletop").font(.largeTitle)
            Text("\(model.modeName) mode")
            switch state {
            case .waiting, .opening:
                ProgressView("Opening mixed immersive space...")
            case .open:
                Text("Immersive space opened.")
            case .failed(let message):
                Text(message).multilineTextAlignment(.center)
                Button("Retry") { Task { await openTabletop(retry: true) } }
                    .buttonStyle(.borderedProminent)
            }
        }
        .padding(32)
        .frame(minWidth: 460, minHeight: 260)
        .task {
            if let error = model.errorMessage { state = .failed(error) }
            else { await openTabletop(retry: false) }
        }
    }

    @MainActor
    private func openTabletop(retry: Bool) async {
        let current = state
        let next = TabletopLauncherReducer.reduce(current, retry ? .retryRequested : .launchRequested)
        guard current != .opening, next == .opening else { return }
        state = next
        do {
            try await model.prepare()
        } catch {
            state = TabletopLauncherReducer.reduce(
                state, .openFailed("Tabletop startup failed: \(error). Check live data/mode settings, then Retry."))
            return
        }
        switch await openImmersiveSpace(id: "tabletop") {
        case .opened:
            state = TabletopLauncherReducer.reduce(state, .openSucceeded)
            dismissWindow(id: "launcher")
        case .userCancelled:
            await model.stop()
            state = TabletopLauncherReducer.reduce(state, .openCancelled)
        case .error:
            await model.stop()
            state = TabletopLauncherReducer.reduce(
                state, .openFailed("The immersive space could not open. Check system permissions, then Retry."))
        @unknown default:
            await model.stop()
            state = TabletopLauncherReducer.reduce(
                state, .openFailed("The immersive space returned an unknown result. Retry or relaunch the app."))
        }
    }
}
