import OpenRealmTabletopBridge

private struct LiveSnapshotLease: TabletopSnapshotLease {
    let retained: OpaquePointer
    let sessionID: UInt64

    func release() { BZ_TTSnapshot_Release(retained) }

    func copyValue() throws -> TabletopSnapshot {
        let expected = UInt32(BZ_TABLETOP_ABI_VERSION), actual = BZ_TTSnapshot_AbiVersion(retained)
        guard actual == expected else { throw TabletopTransportError.abiVersion(expected: expected, actual: actual) }
        var rawBounds = bzTTBox2_t(), bounds: TabletopBounds2?
        if BZ_TTSnapshot_MapBounds(retained, &rawBounds) {
            bounds = TabletopBounds2(minX: rawBounds.min_x, minZ: rawBounds.min_y,
                                     maxX: rawBounds.max_x, maxZ: rawBounds.max_y)
        }
        let entities = try copyEntities()
        return TabletopSnapshot(
            abiVersion: actual, generation: BZ_TTSnapshot_Generation(retained), terrain: [],
            entities: entities.values, sessionID: sessionID, coordinateSpace: .world(bounds),
            connectionState: connectionState(), mapName: copyMapName(), player: copyPlayer(),
            selectedEntityIDs: copySelection(), fog: try copyFog(), unitLayouts: try copyUnitLayouts(),
            configStrings: [:], entitiesOverflowCount: BZ_TTSnapshot_EntitiesOverflowCount(retained),
            duplicateEntityCount: entities.duplicateCount)
    }

    private func copyEntities() throws -> (values: [TabletopEntitySnapshot], duplicateCount: UInt32) {
        let count = BZ_TTSnapshot_EntityCount(retained)
        var values: [TabletopEntitySnapshot] = []
        var seen = Set<UInt64>(), duplicateCount: UInt32 = 0
        values.reserveCapacity(Int(count))
        for index in 0..<count {
            var raw = bzTTEntity_t()
            guard BZ_TTSnapshot_EntityAt(retained, index, &raw) else {
                throw TabletopTransportError.invalidSnapshotEntity(index)
            }
            let id = UInt64(raw.number)
            guard seen.insert(id).inserted else { duplicateCount &+= 1; continue }
            values.append(TabletopEntitySnapshot(
                id: id, kind: .unit,
                position: TabletopVector3(x: raw.origin_x, y: raw.origin_z, z: raw.origin_y),
                heading: raw.angle, selected: raw.selected,
                metadata: TabletopEntityMetadata(
                    classID: raw.class_id,
                    rotation: TabletopVector3(x: raw.rotation_x, y: raw.rotation_z, z: raw.rotation_y),
                    scale: raw.scale, radius: raw.radius, player: raw.player, model: raw.model,
                    model2: raw.model2, image: raw.image, sound: raw.sound, frame: raw.frame,
                    event: raw.event, flags: raw.flags, renderFX: raw.renderfx, ability: raw.ability,
                    splat: raw.splat, shadow: raw.shadow, shadowRect: raw.shadow_rect)))
        }
        return (values, duplicateCount)
    }

    private func copyMapName() -> String? {
        var value = [CChar](repeating: 0, count: Int(BZ_TT_MAX_CONFIGSTRING_LEN))
        guard value.withUnsafeMutableBufferPointer({
            BZ_TTSnapshot_MapName(retained, $0.baseAddress, $0.count)
        }) else { return nil }
        return String(cString: value)
    }

    private func copyPlayer() -> TabletopPlayerSnapshot? {
        guard let pointer = BZ_TTSnapshot_Player(retained) else { return nil }
        let raw = pointer.pointee
        return TabletopPlayerSnapshot(
            number: raw.number, team: raw.team, color: raw.color, race: raw.race, uiFlags: raw.uiflags,
            clientUIState: raw.client_ui_state, selectedEntity: raw.selected_entity,
            startLocation: raw.start_location, gold: raw.resource_gold, lumber: raw.resource_lumber,
            foodUsed: raw.resource_food_used, foodCap: raw.resource_food_cap,
            heroTokens: raw.resource_hero_tokens, name: tupleString(raw.name))
    }

    private func copySelection() -> [UInt32] {
        var values = [UInt32](repeating: 0, count: Int(BZ_TT_MAX_SELECTED_ENTITIES))
        let count = values.withUnsafeMutableBufferPointer {
            BZ_TTSnapshot_SelectedEntityIds(retained, $0.baseAddress, UInt32($0.count))
        }
        return Array(values.prefix(Int(count)))
    }

    private func copyFog() throws -> TabletopFogSnapshot? {
        var width: UInt32 = 0, height: UInt32 = 0
        guard BZ_TTSnapshot_FogDimensions(retained, &width, &height) else { return nil }
        guard height == 0 || width <= UInt32.max / height,
              let count = Int(exactly: UInt64(width) * UInt64(height)) else {
            throw TabletopTransportError.malformedSnapshot("fog dimensions overflow")
        }
        var visible = [UInt8](repeating: 0, count: count), explored = visible
        let visibleCount = visible.withUnsafeMutableBufferPointer {
            BZ_TTSnapshot_FogVisible(retained, $0.baseAddress, UInt32($0.count))
        }
        let exploredCount = explored.withUnsafeMutableBufferPointer {
            BZ_TTSnapshot_FogExplored(retained, $0.baseAddress, UInt32($0.count))
        }
        guard visibleCount == count, exploredCount == count else {
            throw TabletopTransportError.malformedSnapshot("fog plane length does not match dimensions")
        }
        return try TabletopSnapshotValueValidator.fog(width: width, height: height,
                                                      visible: visible, explored: explored)
    }

    private func copyUnitLayouts() throws -> [TabletopUnitLayoutSnapshot] {
        let count = BZ_TTSnapshot_UnitLayoutCount(retained)
        var values: [TabletopUnitLayoutSnapshot] = []
        values.reserveCapacity(Int(count))
        for index in 0..<count {
            var raw = bzTTUnitLayout_t()
            guard BZ_TTSnapshot_UnitLayoutAt(retained, index, &raw) else {
                throw TabletopTransportError.malformedSnapshot("unit layout \(index) could not be copied")
            }
            try TabletopSnapshotValueValidator.layoutCounts(
                buttons: raw.num_buttons, inventory: raw.num_inventory, queue: raw.num_queue)
            let buttons = withUnsafeBytes(of: raw.buttons) {
                Array($0.bindMemory(to: bzTTCommandButton_t.self).prefix(Int(raw.num_buttons))).map {
                    TabletopCommandButtonSnapshot(
                        art: tupleString($0.art), tooltip: tupleString($0.tooltip),
                        ubertip: tupleString($0.ubertip), command: tupleString($0.command), hotkey: $0.hotkey,
                        gridX: $0.grid_x, gridY: $0.grid_y, research: $0.research, active: $0.active)
                }
            }
            let inventory = withUnsafeBytes(of: raw.inventory) {
                Array($0.bindMemory(to: bzTTInventoryItem_t.self).prefix(Int(raw.num_inventory))).map {
                    TabletopInventoryItemSnapshot(art: tupleString($0.art), tooltip: tupleString($0.tooltip),
                                                   ubertip: tupleString($0.ubertip), slot: $0.slot)
                }
            }
            let queue = withUnsafeBytes(of: raw.queue) {
                Array($0.bindMemory(to: bzTTQueueItem_t.self).prefix(Int(raw.num_queue))).map {
                    TabletopQueueItemSnapshot(art: tupleString($0.art), entityID: $0.entity)
                }
            }
            values.append(TabletopUnitLayoutSnapshot(entityID: raw.entity_num, buttons: buttons,
                                                      inventory: inventory, queue: queue))
        }
        return values
    }

    private func connectionState() -> TabletopConnectionState {
        switch BZ_TTSnapshot_ConnState(retained) {
        case BZ_TT_CONN_CONNECTING: return .connecting
        case BZ_TT_CONN_CONNECTED: return .connected
        case BZ_TT_CONN_ACTIVE: return .active
        default: return .disconnected
        }
    }

    private func tupleString<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) {
            String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }
}

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
        switch lifecycleState(bridge) {
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
        switch lifecycleState(bridge) {
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
        return try TabletopSnapshotLeaseConsumer.consume(LiveSnapshotLease(retained: retained, sessionID: sessionID))
    }

    func stop() async {
        guard let bridge else { return }
        bridge.stop()
        self.bridge = nil
    }

    func post(_ command: TabletopCommand) async throws {
        guard bridge != nil else { throw TabletopTransportError.terminal }
        let command = try TabletopCommandLowering.lower(command, currentSessionID: sessionID)
        let result: bzTTResult_t
        switch command {
        case .select(let ids, let generation):
            result = ids.withUnsafeBufferPointer {
                BZ_TT_PostSelect(UInt32(BZ_TABLETOP_ABI_VERSION), generation, $0.baseAddress, UInt32($0.count))
            }
        case .smartEntity(let id, let generation):
            result = BZ_TT_PostSmartEntity(UInt32(BZ_TABLETOP_ABI_VERSION), generation, id)
        case .smartPoint(let x, let y, let generation):
            result = BZ_TT_PostSmartPoint(UInt32(BZ_TABLETOP_ABI_VERSION), generation, x, y)
        case .button(let bytes, let generation):
            result = bytes.withUnsafeBufferPointer {
                BZ_TT_PostButton(UInt32(BZ_TABLETOP_ABI_VERSION), generation, $0.baseAddress, $0.count)
            }
        case .cancel(let generation):
            result = BZ_TT_PostCancel(UInt32(BZ_TABLETOP_ABI_VERSION), generation)
        }
        guard let mapped = TabletopCommandResult(rawValue: result.rawValue) else {
            throw TabletopTransportError.commandRejected(result.rawValue)
        }
        if let error = mapped.error { throw error }
    }

    private func lifecycleState(_ bridge: BZTabletopBridge) -> TabletopLifecycleState {
        TabletopLifecycleState(rawValue: bridge.state.rawValue) ?? .failed
    }
}
