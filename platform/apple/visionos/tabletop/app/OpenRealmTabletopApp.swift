import Foundation
import SwiftUI

@main
struct OpenRealmTabletopApp: App {
    @StateObject private var model: TabletopSessionModel

    init() {
        let environment = ProcessInfo.processInfo.environment
        switch environment["BZ_TABLETOP_MODE"] ?? "fixture" {
        case "fixture":
            let fixture = FixtureSnapshotTransport()
            _model = StateObject(wrappedValue: TabletopSessionModel(transport: fixture, commands: fixture))
        case "live":
            var arguments = ["OpenRealmTabletopFixture", "-data",
                             environment["BZ_TABLETOP_DATA_PATH"] ?? Bundle.main.resourcePath!]
            if let map = environment["BZ_TABLETOP_MAP"], !map.isEmpty { arguments += ["+map", map] }
            if let address = environment["BZ_TABLETOP_CONNECT"], !address.isEmpty {
                arguments += ["+connect", address]
            }
            let live = LiveTabletopTransport(arguments: arguments)
            _model = StateObject(wrappedValue: TabletopSessionModel(transport: live, commands: live))
        case let mode:
            let unavailable = UnavailableTabletopTransport("Unknown BZ_TABLETOP_MODE '\(mode)'")
            _model = StateObject(wrappedValue: TabletopSessionModel(transport: unavailable))
        }
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
