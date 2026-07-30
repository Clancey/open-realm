import Foundation
import SwiftUI

@main
struct OpenRealmTabletopApp: App {
    @StateObject private var model: TabletopSessionModel
    private let automatedLaunch: Bool

    init() {
        let environment = ProcessInfo.processInfo.environment
        automatedLaunch = environment["BZ_TABLETOP_AUTOSTART"] == "1"
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
                let map = TabletopMapRecord(
                    edition: .roc, source: .archive, campaignIndex: 0, missionIndex: 0,
                    campaign: "Fixture", title: "Fixture battlefield", subtitle: "",
                    mapPath: "fixture")
                _model = StateObject(wrappedValue: TabletopSessionModel(
                    modeName: "Fixture / \(providerName)", transport: fixture,
                    renderProvider: provider, commands: fixture, catalogs: [.roc: [map]]))
            case .live(let dataPath, let map, let connect, let tft):
                guard connect == nil else {
                    throw TabletopTransportError.configuration(
                        "Remote connect is not supported by the native single-player product flow")
                }
                let dataEntries = try Self.dataEntries(dataPath)
                guard TabletopDataPreflight.isUsable(entries: dataEntries, localMapRequired: true),
                      TabletopDataPreflight.supportsProductCatalog(entries: dataEntries) else {
                    throw TabletopTransportError.configuration(
                        "Live data path '\(dataPath)' requires root-level Warcraft III MPQ archives; " +
                        "run the production bundle staging target or set BZ_TABLETOP_DATA_PATH")
                }
                let arguments = [TabletopProduct.executable, "-data", dataPath]
                let live = LiveTabletopTransport(
                    arguments: arguments, logItemPublication: environment["BZ_TABLETOP_ACCEPTANCE"] == "1")
                var catalogs: [TabletopEdition: [TabletopMapRecord]] = [:]
                for edition in TabletopEdition.allCases {
                    do {
                        let maps = try LiveTabletopCatalog.discover(dataPath: dataPath, edition: edition)
                        catalogs[edition] = maps
                        let campaignCount = maps.filter { $0.source == .campaign }.count
                        let message = "OpenRealmTabletopCatalog[\(edition.rawValue)]: \(maps.count) maps, " +
                            "\(campaignCount) campaign\n"
                        FileHandle.standardError.write(Data(message.utf8))
                    }
                    catch {
                        catalogs[edition] = []
                        FileHandle.standardError.write(Data(
                            "OpenRealmTabletopCatalog[\(edition.rawValue)]: \(error)\n".utf8))
                    }
                }
                guard let applicationSupport = FileManager.default.urls(
                    for: .applicationSupportDirectory, in: .userDomainMask).first else {
                    throw TabletopTransportError.configuration("Application Support directory is unavailable")
                }
                let initialEdition: TabletopEdition = tft ? .tft : .roc
                let session = TabletopSessionModel(
                    modeName: "Live / \(providerName)", transport: live,
                    renderProvider: provider, commands: live, catalogs: catalogs,
                    initialEdition: initialEdition,
                    progressStore: TabletopProgressStore(applicationSupport: applicationSupport))
                if let map {
                    guard let match = session.availableMaps.first(where: { $0.matches(map) }) else {
                        throw TabletopTransportError.configuration(
                            "Requested map '\(map)' is not present in the \(initialEdition.title) catalog")
                    }
                    session.selectMap(match.id)
                }
                _model = StateObject(wrappedValue: session)
            }
        } catch {
            let unavailable = UnavailableTabletopTransport(String(describing: error))
            _model = StateObject(wrappedValue: TabletopSessionModel(
                modeName: "Unavailable", transport: unavailable,
                renderProvider: ProductionWarcraftRenderProvider(),
                unavailableReason: String(describing: error)))
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
            TabletopLauncherView(model: model, automatedLaunch: automatedLaunch)
        }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "tabletop") {
            TabletopImmersiveView(model: model)
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}
