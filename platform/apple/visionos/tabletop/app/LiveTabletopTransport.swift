import OpenRealmTabletopBridge

actor LiveTabletopTransport: TabletopSnapshotTransport, TabletopCommandTransport {
    private let arguments: [String]
    private var bridge: BZTabletopBridge?
    private var sessionID: UInt64 = 0

    init(arguments: [String]) {
        self.arguments = arguments
    }

    func start() async throws {
        guard bridge == nil else { return }
        sessionID &+= 1
        let bridge = BZTabletopBridge(arguments: arguments)
        self.bridge = bridge
        bridge.start()
        switch bridge.state {
        case .running, .suspended: return
        case .failed:
            let message = bridge.lastError ?? "Tabletop engine failed to start"
            bridge.stop()
            self.bridge = nil
            throw TabletopTransportError.runtime(message)
        default:
            let message = "Tabletop engine entered unexpected state \(bridge.state.rawValue)"
            bridge.stop()
            self.bridge = nil
            throw TabletopTransportError.runtime(message)
        }
    }

    func poll() async throws -> TabletopSnapshot? {
        guard let bridge else { return nil }
        switch bridge.state {
        case .running, .suspended: break
        case .failed, .stopped:
            let message = bridge.lastError ?? "Tabletop engine stopped"
            bridge.stop()
            self.bridge = nil
            throw TabletopTransportError.runtime(message)
        default:
            throw TabletopTransportError.runtime("Tabletop engine entered unexpected state \(bridge.state.rawValue)")
        }
        guard let retained = BZ_TT_Latest() else { return nil }
        defer { BZ_TTSnapshot_Release(retained) }
        let expected = UInt32(BZ_TABLETOP_ABI_VERSION), actual = BZ_TTSnapshot_AbiVersion(retained)
        guard actual == expected else { throw TabletopTransportError.abiVersion(expected: expected, actual: actual) }
        var rawBounds = bzTTBox2_t(), bounds: TabletopBounds2?
        if BZ_TTSnapshot_MapBounds(retained, &rawBounds) {
            bounds = TabletopBounds2(minX: rawBounds.min_x, minZ: rawBounds.min_y,
                                     maxX: rawBounds.max_x, maxZ: rawBounds.max_y)
        }
        let count = BZ_TTSnapshot_EntityCount(retained)
        var entities: [TabletopEntitySnapshot] = []
        var seenEntityIDs = Set<UInt64>(), duplicateEntityCount: UInt32 = 0
        entities.reserveCapacity(Int(count))
        for index in 0..<count {
            var raw = bzTTEntity_t()
            guard BZ_TTSnapshot_EntityAt(retained, index, &raw) else {
                throw TabletopTransportError.invalidSnapshotEntity(index)
            }
            let id = UInt64(raw.number)
            guard seenEntityIDs.insert(id).inserted else {
                duplicateEntityCount &+= 1
                continue
            }
            entities.append(TabletopEntitySnapshot(
                id: id, kind: .unit,
                position: TabletopVector3(x: raw.origin_x, y: raw.origin_z, z: raw.origin_y),
                heading: raw.angle, selected: raw.selected))
        }
        return TabletopSnapshot(generation: BZ_TTSnapshot_Generation(retained), terrain: [], entities: entities,
                                sessionID: sessionID,
                                coordinateSpace: .world(bounds), connectionState: connectionState(retained),
                                entitiesOverflowCount: BZ_TTSnapshot_EntitiesOverflowCount(retained),
                                duplicateEntityCount: duplicateEntityCount)
    }

    func stop() async {
        guard let bridge else { return }
        bridge.stop()
        self.bridge = nil
    }

    func post(_ command: TabletopCommand) async throws {
        guard bridge != nil, command.sessionID == sessionID else { throw TabletopTransportError.staleSession }
        let result: bzTTResult_t
        switch command {
        case .select(let ids, let generation, _):
            result = ids.withUnsafeBufferPointer {
                BZ_TT_PostSelect(UInt32(BZ_TABLETOP_ABI_VERSION), generation, $0.baseAddress, UInt32($0.count))
            }
        case .smartEntity(let id, let generation, _):
            result = BZ_TT_PostSmartEntity(UInt32(BZ_TABLETOP_ABI_VERSION), generation, id)
        case .smartPoint(let x, let y, let generation, _):
            result = BZ_TT_PostSmartPoint(UInt32(BZ_TABLETOP_ABI_VERSION), generation, x, y)
        case .button(let code, let generation, _):
            let bytes = code.utf8.map { CChar(bitPattern: $0) }
            result = bytes.withUnsafeBufferPointer {
                BZ_TT_PostButton(UInt32(BZ_TABLETOP_ABI_VERSION), generation, $0.baseAddress, $0.count)
            }
        case .cancel(let generation, _):
            result = BZ_TT_PostCancel(UInt32(BZ_TABLETOP_ABI_VERSION), generation)
        }
        guard result == BZ_TT_OK else { throw TabletopTransportError.commandRejected(result.rawValue) }
    }

    private func connectionState(_ snapshot: OpaquePointer) -> TabletopConnectionState {
        switch BZ_TTSnapshot_ConnState(snapshot) {
        case BZ_TT_CONN_CONNECTING: return .connecting
        case BZ_TT_CONN_CONNECTED: return .connected
        case BZ_TT_CONN_ACTIVE: return .active
        default: return .disconnected
        }
    }
}
