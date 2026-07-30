import Foundation

enum TabletopEdition: String, CaseIterable, Codable, Equatable, Sendable {
    case roc
    case tft

    var title: String { self == .roc ? "Reign of Chaos" : "The Frozen Throne" }
}

enum TabletopMapSource: UInt8, Codable, Equatable, Sendable {
    case campaign
    case archive
}

struct TabletopMapRecord: Identifiable, Codable, Equatable, Sendable {
    var edition: TabletopEdition
    var source: TabletopMapSource
    var campaignIndex: UInt32
    var missionIndex: UInt32
    var campaign: String
    var title: String
    var subtitle: String
    var mapPath: String
    var id: String { "\(edition.rawValue):\(mapPath.lowercased())" }

    func matches(_ selection: String) -> Bool {
        if mapPath.caseInsensitiveCompare(selection) == .orderedSame { return true }
        let name = mapPath.replacingOccurrences(of: "\\", with: "/").split(separator: "/").last.map(String.init) ?? ""
        return (name as NSString).deletingPathExtension.caseInsensitiveCompare(selection) == .orderedSame
    }
}

enum TabletopGameResult: UInt8, Codable, Equatable, Sendable {
    case none
    case victory
    case defeat
    case draw
}

enum TabletopProductState: Equatable, Sendable {
    case menu
    case loading(TabletopMapRecord)
    case playing(TabletopMapRecord)
    case paused(TabletopMapRecord)
    case terminal(TabletopMapRecord, TabletopGameResult)
    case failed(String)
    case missingData(String)
}

enum TabletopProductEvent: Equatable, Sendable {
    case launch(TabletopMapRecord)
    case loaded
    case pause
    case resume
    case result(TabletopGameResult)
    case retry
    case returnToMenu
    case fail(String)
}

enum TabletopProductReducer {
    static func reduce(_ state: TabletopProductState, _ event: TabletopProductEvent) -> TabletopProductState {
        switch (state, event) {
        case (.menu, .launch(let map)): return .loading(map)
        case (.loading(let map), .loaded): return .playing(map)
        case (.playing(let map), .pause): return .paused(map)
        case (.paused(let map), .resume): return .playing(map)
        case (.playing(let map), .result(let result)) where result != .none: return .terminal(map, result)
        case (.terminal(let map, _), .retry): return .loading(map)
        case (_, .returnToMenu): return .menu
        case (_, .fail(let message)): return .failed(message)
        default: return state
        }
    }

    static func recordsCompletion(from old: TabletopProductState, to new: TabletopProductState) -> Bool {
        guard old != new, case .terminal(_, .victory) = new else { return false }
        return true
    }
}

enum TabletopPauseReason: Hashable, Sendable {
    case user
    case background
}

enum TabletopAudioLifetime {
    static func shouldRetain(isPlaying: Bool, intentionallyPaused: Bool) -> Bool {
        isPlaying || intentionallyPaused
    }
}

protocol TabletopProductTransport: TabletopSnapshotTransport {
    func configure(edition: TabletopEdition) async throws
    func submitMap(_ map: String) async throws
    func suspend() async
    func resume() async
}

struct TabletopProgress: Codable, Equatable, Sendable {
    static let version = 1
    var version = Self.version
    var selectedMap: String?
    var completedMaps: Set<String> = []
}

struct TabletopProgressStore {
    let applicationSupport: URL

    func load(_ edition: TabletopEdition) throws -> TabletopProgress {
        let url = fileURL(edition)
        guard FileManager.default.fileExists(atPath: url.path) else { return TabletopProgress() }
        let value = try JSONDecoder().decode(TabletopProgress.self, from: Data(contentsOf: url))
        guard value.version == TabletopProgress.version else {
            throw TabletopTransportError.configuration(
                "Unsupported \(edition.title) progress version \(value.version)")
        }
        return value
    }

    func save(_ value: TabletopProgress, edition: TabletopEdition) throws {
        guard value.version == TabletopProgress.version else {
            throw TabletopTransportError.configuration("Refusing to write an unsupported progress version")
        }
        let url = fileURL(edition)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        try JSONEncoder().encode(value).write(to: url, options: [.atomic])
    }

    private func fileURL(_ edition: TabletopEdition) -> URL {
        applicationSupport.appendingPathComponent(
            "OpenRealm/WarcraftTabletop/v\(TabletopProgress.version)/\(edition.rawValue)/progress.json")
            .standardizedFileURL
    }
}
