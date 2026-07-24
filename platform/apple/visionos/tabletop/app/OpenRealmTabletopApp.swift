import Foundation
import SwiftUI

@main
struct OpenRealmTabletopApp: App {
    @StateObject private var model: TabletopSessionModel

    init() {
        let environment = ProcessInfo.processInfo.environment
        do {
            let runtimeMode = try TabletopRuntimeModeResolver.resolve(
                environment: environment, bundlePath: Bundle.main.bundlePath)
            let providerMode = try WarcraftRenderProviderModeResolver.resolve(
                environment: environment, runtimeMode: runtimeMode)
            let provider: any WarcraftRenderDescriptorProvider = providerMode == .fixture ?
                FixtureWarcraftRenderProvider() : ProductionWarcraftRenderProvider()
            let providerName = providerMode == .fixture ? "Fixture descriptors" : "Production descriptors"
            switch runtimeMode {
            case .fixture:
                let fixture = FixtureSnapshotTransport()
                _model = StateObject(wrappedValue: TabletopSessionModel(
                    modeName: "Fixture / \(providerName)", transport: fixture,
                    renderProvider: provider, commands: fixture))
            case .live(let dataPath, let map, let connect):
                guard TabletopDataPreflight.isUsable(
                    entries: try Self.dataEntries(dataPath), localMapRequired: map != nil) else {
                    throw TabletopTransportError.configuration(
                        "Live data path '\(dataPath)' is missing required Warcraft III data; " +
                        "run the production bundle staging target or set BZ_TABLETOP_DATA_PATH")
                }
                var arguments = [TabletopProduct.executable, "-data", dataPath]
                if let map { arguments += ["+map", map] }
                if let connect { arguments += ["+connect", connect] }
                let live = LiveTabletopTransport(arguments: arguments)
                _model = StateObject(wrappedValue: TabletopSessionModel(
                    modeName: "Live / \(providerName)", transport: live,
                    renderProvider: provider, commands: live))
            }
        } catch {
            let unavailable = UnavailableTabletopTransport(String(describing: error))
            _model = StateObject(wrappedValue: TabletopSessionModel(
                modeName: "Unavailable", transport: unavailable,
                renderProvider: ProductionWarcraftRenderProvider()))
        }
    }

    private static func dataEntries(_ path: String) throws -> [TabletopDataEntry] {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory), isDirectory.boolValue else {
            throw TabletopTransportError.configuration("Live data directory does not exist: \(path)")
        }
        let root = URL(fileURLWithPath: path, isDirectory: true)
        guard let enumerator = FileManager.default.enumerator(
            at: root, includingPropertiesForKeys: [.isRegularFileKey], options: [.skipsHiddenFiles]) else {
            throw TabletopTransportError.configuration("Live data directory cannot be enumerated: \(path)")
        }
        var entries: [TabletopDataEntry] = []
        for case let url as URL in enumerator {
            let values = try url.resourceValues(forKeys: [.isRegularFileKey])
            let relative = String(url.path.dropFirst(root.path.count)).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            entries.append(TabletopDataEntry(relativePath: relative, isRegularFile: values.isRegularFile == true))
        }
        return entries
    }

    var body: some Scene {
        WindowGroup(id: "launcher") {
            TabletopLauncherView(model: model)
        }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "tabletop") {
            TabletopImmersiveView(model: model)
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}
