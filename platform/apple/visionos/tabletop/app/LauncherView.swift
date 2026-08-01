import SwiftUI

struct TabletopLauncherView: View {
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @ObservedObject var model: TabletopSessionModel
    var automatedLaunch = false
    @State private var launcherState = TabletopLauncherState.waiting
    @State private var source: TabletopMapSource
    @State private var attemptedAutomatedLaunch = false

    init(model: TabletopSessionModel, automatedLaunch: Bool = false) {
        self.model = model
        self.automatedLaunch = automatedLaunch
        _source = State(initialValue: TabletopMapSourceResolver.resolve(model.selectedMap))
    }

    private var maps: [TabletopMapRecord] {
        let filtered = model.availableMaps.filter { $0.source == source }
        return filtered.isEmpty ? model.availableMaps : filtered
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Open Realm Tabletop").font(.largeTitle)
            Text("\(model.modeName) mode").foregroundStyle(.secondary)
            if case .missingData(let message) = model.productState {
                ContentUnavailableView("Warcraft III data unavailable",
                                       systemImage: "externaldrive.badge.exclamationmark",
                                       description: Text(message))
            } else {
                Picker("Game", selection: Binding(
                    get: { model.selectedEdition },
                    set: { selectEdition($0) })) {
                    ForEach(TabletopEdition.allCases, id: \.self) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)

                Picker("Source", selection: $source) {
                    Text("Campaign").tag(TabletopMapSource.campaign)
                    Text("Archive maps").tag(TabletopMapSource.archive)
                }
                .pickerStyle(.segmented)

                Picker("Playable map", selection: Binding(
                    get: { model.selectedMapID ?? "" },
                    set: { model.selectMap($0) })) {
                    ForEach(maps) { map in
                        VStack(alignment: .leading) {
                            Text(map.title.isEmpty ? map.mapPath : map.title)
                            if !map.campaign.isEmpty { Text(map.campaign).font(.caption) }
                        }.tag(map.id)
                    }
                }

                mapDetails
                launchControls
            }
        }
        .padding(32)
        .frame(minWidth: 560, minHeight: 430)
        .onChange(of: source) { _, _ in
            if let first = maps.first, !maps.contains(where: { $0.id == model.selectedMapID }) {
                model.selectMap(first.id)
            }
        }
        .task {
            guard automatedLaunch, !attemptedAutomatedLaunch else { return }
            attemptedAutomatedLaunch = true
            await openTabletop()
        }
    }

    @ViewBuilder private var mapDetails: some View {
        if let map = model.selectedMap {
            VStack(alignment: .leading, spacing: 4) {
                Text(map.title.isEmpty ? "Untitled map" : map.title).font(.headline)
                if !map.subtitle.isEmpty { Text(map.subtitle) }
                Text(map.mapPath).font(.caption.monospaced()).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private var launchControls: some View {
        switch model.productState {
        case .loading(let map):
            ProgressView("Loading \(map.title.isEmpty ? map.mapPath : map.title)…")
        case .failed(let message):
            Text(message).foregroundStyle(.red)
            Button("Retry") { Task { await openTabletop() } }.buttonStyle(.borderedProminent)
        default:
            Button("Play") { Task { await openTabletop() } }
                .buttonStyle(.borderedProminent)
                .disabled(model.selectedMap == nil || launcherState == .opening)
        }
    }

    private func selectEdition(_ edition: TabletopEdition) {
        model.selectEdition(edition)
        source = TabletopMapSourceResolver.resolve(model.selectedMap)
    }

    @MainActor
    private func openTabletop() async {
        guard launcherState != .opening else { return }
        launcherState = .opening
        do { try await model.prepare() }
        catch {
            launcherState = .failed(String(describing: error))
            return
        }
        switch await openImmersiveSpace(id: "tabletop") {
        case .opened:
            launcherState = .open
            dismissWindow(id: "launcher")
        case .userCancelled:
            await model.returnToMenu()
            launcherState = .failed("Opening the immersive space was cancelled.")
        case .error:
            await model.returnToMenu()
            launcherState = .failed("The immersive space could not open.")
        @unknown default:
            await model.returnToMenu()
            launcherState = .failed("The immersive space returned an unknown result.")
        }
    }
}
