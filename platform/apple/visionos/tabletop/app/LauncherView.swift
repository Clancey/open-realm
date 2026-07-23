import SwiftUI

struct TabletopLauncherView: View {
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @State private var state = TabletopLauncherState.waiting

    var body: some View {
        VStack(spacing: 18) {
            Text("Open Realm Tabletop").font(.largeTitle)
            Text("Fixture shell: procedural board and entities only.")
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
        .task { await openTabletop(retry: false) }
    }

    @MainActor
    private func openTabletop(retry: Bool) async {
        state = TabletopLauncherReducer.reduce(state, retry ? .retryRequested : .launchRequested)
        guard state == .opening else { return }
        switch await openImmersiveSpace(id: "tabletop") {
        case .opened:
            state = TabletopLauncherReducer.reduce(state, .openSucceeded)
            dismissWindow(id: "launcher")
        case .userCancelled:
            state = TabletopLauncherReducer.reduce(state, .openCancelled)
        case .error:
            state = TabletopLauncherReducer.reduce(
                state, .openFailed("The immersive space could not open. Check system permissions, then Retry."))
        @unknown default:
            state = TabletopLauncherReducer.reduce(
                state, .openFailed("The immersive space returned an unknown result. Retry or relaunch the app."))
        }
    }
}
