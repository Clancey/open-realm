import Foundation
import CryptoKit

enum WarcraftContentHash {
    static func hash(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }
}

struct WarcraftContentHasher {
    private var data = Data()

    mutating func update(_ bytes: some Sequence<UInt8>) { data.append(contentsOf: bytes) }

    mutating func update(_ value: UInt64) {
        var encoded = value.littleEndian
        withUnsafeBytes(of: &encoded) { data.append(contentsOf: $0) }
    }

    mutating func update(_ value: Int) { update(UInt64(bitPattern: Int64(value))) }
    mutating func update(_ value: UInt32) { update(UInt64(value)) }
    mutating func update(_ value: Float) { update(value.bitPattern) }
    mutating func update(_ value: Bool) { update([value ? 1 : 0]) }

    mutating func update(_ value: String) {
        let bytes = Array(value.utf8)
        update(bytes.count)
        update(bytes)
    }

    func digest() -> String { "v2-\(WarcraftContentHash.hash(data))" }
}

enum WarcraftCacheRoot {
    static func applicationSupport(fileManager: FileManager = .default) throws -> URL {
        guard let root = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
            throw WarcraftDescriptorError.cache("Application Support directory is unavailable")
        }
        return root
    }
}

struct WarcraftCacheKey: Codable, Equatable, Hashable, Sendable {
    static let version = 2
    var namespace: String
    var digest: String

    var fileName: String { "v\(Self.version)-\(namespace)-\(digest).cache" }

    static func content(namespace: String, data: Data) throws -> WarcraftCacheKey {
        guard safe(namespace), !namespace.isEmpty else {
            throw WarcraftDescriptorError.cache("cache namespace contains unsafe characters")
        }
        return WarcraftCacheKey(namespace: namespace, digest: WarcraftContentHash.hash(data))
    }

    static func safe(_ value: String) -> Bool {
        value.utf8.allSatisfy {
            ($0 >= 48 && $0 <= 57) || ($0 >= 65 && $0 <= 90) || ($0 >= 97 && $0 <= 122) || $0 == 45
        }
    }
}

struct WarcraftCacheCounters: Equatable, Sendable {
    var hits = 0
    var misses = 0
    var evictions = 0
    var writes = 0
}

struct WarcraftMemoryCache {
    private struct Entry {
        var fingerprint: String
        var data: Data
        var tick: UInt64
    }

    let byteLimit: Int
    private var values: [WarcraftCacheKey: Entry] = [:]
    private var bytes = 0
    private var tick: UInt64 = 0
    private(set) var counters = WarcraftCacheCounters()

    init(byteLimit: Int) { self.byteLimit = max(byteLimit, 0) }

    mutating func value(for key: WarcraftCacheKey, fingerprint: String) throws -> Data? {
        tick &+= 1
        guard var entry = values[key] else { counters.misses += 1; return nil }
        guard entry.fingerprint == fingerprint else {
            throw WarcraftDescriptorError.cache("memory cache key collision")
        }
        entry.tick = tick
        values[key] = entry
        counters.hits += 1
        return entry.data
    }

    mutating func insert(_ data: Data, for key: WarcraftCacheKey, fingerprint: String) throws {
        if let existing = values[key], existing.fingerprint != fingerprint {
            throw WarcraftDescriptorError.cache("memory cache key collision")
        }
        tick &+= 1
        if let old = values[key] { bytes -= old.data.count }
        values[key] = Entry(fingerprint: fingerprint, data: data, tick: tick)
        bytes += data.count
        while bytes > byteLimit, let victim = values.min(by: { $0.value.tick < $1.value.tick }) {
            bytes -= victim.value.data.count
            values.removeValue(forKey: victim.key)
            counters.evictions += 1
        }
    }
}

final class WarcraftDiskCache {
    private struct Envelope: Codable {
        var key: WarcraftCacheKey
        var fingerprint: String
        var payloadHash: String
        var payload: Data
    }

    let root: URL
    let byteLimit: Int
    private(set) var counters = WarcraftCacheCounters()

    init(applicationSupport: URL, byteLimit: Int, editionScope: String = "shared") throws {
        let safeScope = editionScope.allSatisfy { $0.isLetter || $0.isNumber || $0 == "-" } ?
            editionScope : "invalid"
        root = applicationSupport.appendingPathComponent(
            "OpenRealm/WarcraftRenderer/v\(WarcraftCacheKey.version)/\(safeScope)",
                                                         isDirectory: true).standardizedFileURL
        self.byteLimit = max(byteLimit, 0)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    func value(for key: WarcraftCacheKey, fingerprint: String) throws -> Data? {
        let url = try confinedURL(for: key)
        guard FileManager.default.fileExists(atPath: url.path) else { counters.misses += 1; return nil }
        let envelope = try JSONDecoder().decode(Envelope.self, from: Data(contentsOf: url))
        guard envelope.key == key, envelope.fingerprint == fingerprint,
              envelope.payloadHash == WarcraftContentHash.hash(envelope.payload) else {
            throw WarcraftDescriptorError.cache("disk cache reload validation failed")
        }
        counters.hits += 1
        try FileManager.default.setAttributes([.modificationDate: Date()], ofItemAtPath: url.path)
        return envelope.payload
    }

    func insert(_ payload: Data, for key: WarcraftCacheKey, fingerprint: String) throws {
        let url = try confinedURL(for: key)
        if FileManager.default.fileExists(atPath: url.path) {
            let existing = try JSONDecoder().decode(Envelope.self, from: Data(contentsOf: url))
            guard existing.key == key, existing.fingerprint == fingerprint else {
                throw WarcraftDescriptorError.cache("disk cache key collision")
            }
        }
        let envelope = Envelope(key: key, fingerprint: fingerprint,
                                payloadHash: WarcraftContentHash.hash(payload), payload: payload)
        try JSONEncoder.sorted.encode(envelope).write(to: url, options: .atomic)
        counters.writes += 1
        try evict()
    }

    func confinedURL(for key: WarcraftCacheKey) throws -> URL {
        guard WarcraftCacheKey.safe(key.namespace), WarcraftCacheKey.safe(key.digest),
              !key.namespace.isEmpty, !key.digest.isEmpty else {
            throw WarcraftDescriptorError.cache("cache key path is unsafe")
        }
        let url = root.appendingPathComponent(key.fileName, isDirectory: false).standardizedFileURL
        let prefix = root.path.hasSuffix("/") ? root.path : root.path + "/"
        guard url.path.hasPrefix(prefix) else { throw WarcraftDescriptorError.cache("cache path escapes root") }
        return url
    }

    private func evict() throws {
        let urls = try FileManager.default.contentsOfDirectory(
            at: root, includingPropertiesForKeys: [.fileSizeKey, .contentModificationDateKey],
            options: [.skipsHiddenFiles])
        var entries = try urls.map {
            let values = try $0.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
            return ($0, values.fileSize ?? 0, values.contentModificationDate ?? .distantPast)
        }.sorted { $0.2 < $1.2 }
        var total = entries.reduce(0) { $0 + $1.1 }
        while total > byteLimit, !entries.isEmpty {
            let victim = entries.removeFirst()
            try FileManager.default.removeItem(at: victim.0)
            total -= victim.1
            counters.evictions += 1
        }
    }
}

struct WarcraftLogOnce {
    private var messages: Set<String> = []

    mutating func record(_ message: String) -> Bool { messages.insert(message).inserted }
}
