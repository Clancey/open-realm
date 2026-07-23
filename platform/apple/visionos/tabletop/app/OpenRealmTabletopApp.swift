import SwiftUI

@main
struct OpenRealmTabletopApp: App {
    @StateObject private var model: TabletopSessionModel

    init() {
        _model = StateObject(wrappedValue: TabletopSessionModel(transport: FixtureSnapshotTransport()))
    }

    var body: some Scene {
        WindowGroup(id: "launcher") {
            TabletopLauncherView()
        }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "tabletop") {
            TabletopImmersiveView(model: model)
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}
