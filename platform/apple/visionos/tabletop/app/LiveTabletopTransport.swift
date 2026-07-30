import OpenRealmTabletopBridge

private final class LiveWarcraftCopyCache {
    var values = WarcraftExportedAssetCache(modelLimit: 256)
    var teamImages: [UInt64: (image: WarcraftExportedImage, contentKey: String)] = [:]

    func reset() {
        values.reset()
        teamImages.removeAll()
    }
}

private struct LiveSnapshotLease: TabletopSnapshotLease {
    let retained: OpaquePointer
    let sessionID: UInt64
    let assetCache: LiveWarcraftCopyCache

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
        let mapName = copyMapName()
        let warcraftAssets = try LiveWarcraftAssetCopy.copy(
            snapshot: retained, mapName: mapName, entities: entities.values, cache: assetCache)
        return TabletopSnapshot(
            abiVersion: actual, generation: BZ_TTSnapshot_Generation(retained), terrain: [],
            entities: entities.values, sessionID: sessionID, coordinateSpace: .world(bounds),
            connectionState: connectionState(), mapName: mapName, player: copyPlayer(),
            selectedEntityIDs: copySelection(), fog: try copyFog(), unitLayouts: try copyUnitLayouts(),
            actionLayout: copyActionLayout(), configStrings: copyConfigStrings(),
            entitiesOverflowCount: BZ_TTSnapshot_EntitiesOverflowCount(retained),
            duplicateEntityCount: entities.duplicateCount, warcraftAssets: warcraftAssets)
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
                heading: TabletopCoordinateConversion.heading(raw.angle), selected: raw.selected,
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
            heroTokens: raw.resource_hero_tokens, name: tupleString(raw.name),
            target: actionTarget(raw.target),
            gameResult: TabletopGameResult(rawValue: UInt8(raw.game_result.rawValue)) ?? .none)
    }

    private func copySelection() -> [UInt32] {
        var values = [UInt32](repeating: 0, count: Int(BZ_TT_MAX_SELECTED_ENTITIES))
        let count = values.withUnsafeMutableBufferPointer {
            BZ_TTSnapshot_SelectedEntityIds(retained, $0.baseAddress, UInt32($0.count))
        }
        return Array(values.prefix(Int(count)))
    }

    private func copyConfigStrings() -> [UInt32: String] {
        let count = BZ_TTSnapshot_ConfigStringCount(retained)
        return TabletopConfigStringCopy.copy(count: count) { index in
            var value = [CChar](repeating: 0, count: Int(BZ_TT_MAX_CONFIGSTRING_LEN))
            let copied = value.withUnsafeMutableBufferPointer {
                BZ_TTSnapshot_ConfigString(retained, index, $0.baseAddress, $0.count)
            }
            return copied ? String(cString: value) : nil
        }
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

    private func copyActionLayout() -> TabletopActionLayoutSnapshot {
        guard let pointer = BZ_TTSnapshot_ActionLayout(retained) else {
            return TabletopActionLayoutSnapshot()
        }
        let raw = pointer.pointee
        let count = min(Int(raw.num_buttons), Int(BZ_TT_MAX_COMMAND_BUTTONS))
        let buttons = withUnsafeBytes(of: raw.buttons) {
            Array($0.bindMemory(to: bzTTActionButton_t.self).prefix(count)).map {
                TabletopActionButtonSnapshot(
                    imageIndex: $0.image_index, tooltip: tupleString($0.tooltip),
                    actionCode: tupleString($0.action_code), hotkey: $0.hotkey,
                    gridX: $0.grid_x, gridY: $0.grid_y, hidden: $0.hidden, disabled: $0.disabled,
                    cooldown: $0.cooldown, target: actionTarget($0.target),
                    semantic: actionSemantic($0.semantic))
            }
        }
        return TabletopActionLayoutSnapshot(
            present: raw.present, visible: raw.visible, valid: raw.valid,
            currentTarget: actionTarget(raw.current_target), buttons: buttons)
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

    private func actionTarget(_ raw: bzTTActionTarget_t) -> TabletopActionTarget {
        TabletopActionTarget(rawValue: raw.rawValue) ?? .none
    }

    private func actionSemantic(_ raw: bzTTActionSemantic_t) -> TabletopActionSemantic {
        TabletopActionSemantic(rawValue: raw.rawValue) ?? .unsupported
    }
}

private enum LiveWarcraftAssetCopy {
    private static let modelConfigStringBase: UInt32 = 32
    /* common/shared.h's frozen CS_IMAGES = CS_MODELS + MAX_MODELS + MAX_SOUNDS. */
    private static let imageConfigStringBase: UInt32 = 32 + 256 + 256
    private static let maximumBufferElements = 16 * 1_024 * 1_024

    static func copy(
        snapshot: OpaquePointer, mapName: String?, entities: [TabletopEntitySnapshot],
        cache: LiveWarcraftCopyCache) throws
        -> WarcraftProductionAssets {
        let expected = UInt32(BZ_TABLETOP_ASSETS_ABI_VERSION), actual = BZ_TTA_AbiVersion()
        guard actual == expected else {
            throw TabletopTransportError.abiVersion(expected: expected, actual: actual)
        }
        let terrain = try copyTerrain(mapName: mapName, cache: cache)
        var models: [UInt64: WarcraftProductionEntityAsset] = [:]
        var modelDescriptors: [
            WarcraftExportedModelCacheKey: (WarcraftExportedModel, WarcraftProductionEntityAsset)
        ] = [:]
        var baseModels: [UInt32: (WarcraftExportedModelCacheKey, WarcraftExportedModel)] = [:]
        var entityImages: [UInt32: WarcraftExportedImage] = [:]
        for entity in entities where entity.metadata.model != 0 {
            let (metadataStatus, resolvedMetadata) = resolveMetadata(expected, entity: entity)
            let metadata = metadataValue(resolvedMetadata, fallback: entity)
            let base: (WarcraftExportedModelCacheKey, WarcraftExportedModel)
            if let cached = baseModels[entity.metadata.model] {
                base = cached
            } else {
                var registrationMetadata = resolvedMetadata
                guard let asset = BZ_TTA_RegisterConfigString(
                    expected, snapshot, modelConfigStringBase + entity.metadata.model,
                    BZ_TTA_ASSET_MODEL, &registrationMetadata) else {
                    throw TabletopTransportError.malformedSnapshot(
                        "asset registration failed for entity \(entity.id)")
                }
                do {
                    let identity = try copyIdentity(asset)
                    let key = WarcraftExportedModelCacheKey(
                        configStringIndex: entity.metadata.model, identity: identity,
                        status: UInt32(BZ_TTAsset_Status(asset).rawValue),
                        placeholder: BZ_TTAsset_IsPlaceholder(asset))
                    let model: WarcraftExportedModel
                    if let cached = cache.values.model(for: key) {
                        model = cached
                    } else {
                        model = try copyModel(asset, identity: identity, metadataStatus: metadataStatus)
                        cache.values.insert(model, for: key)
                    }
                    base = (key, model)
                    baseModels[entity.metadata.model] = base
                } catch {
                    BZ_TTAsset_Release(asset)
                    throw error
                }
                BZ_TTAsset_Release(asset)
            }
            let overrideImage: WarcraftExportedImage?
            let needsOverride = base.1.textures.contains {
                $0.replaceableID != 0 && $0.replaceableID != 1 && $0.replaceableID != 2
            }
            if !needsOverride || entity.metadata.image == 0 {
                overrideImage = nil
            } else if let cached = entityImages[entity.metadata.image] {
                overrideImage = cached
            } else {
                let copied = try copyEntityImage(
                    expected, snapshot: snapshot, imageIndex: entity.metadata.image)
                entityImages[entity.metadata.image] = copied
                overrideImage = copied
            }
            let needsTeamColor = base.1.textures.contains { $0.replaceableID == 1 }
            let needsTeamGlow = base.1.textures.contains { $0.replaceableID == 2 }
            let teamColorImage = try copyTeamImage(
                expected, kind: BZ_TTA_TEAM_TEXTURE_COLOR, teamColor: UInt32(metadata.teamColor),
                required: needsTeamColor, cache: cache)
            let teamGlowImage = try copyTeamImage(
                expected, kind: BZ_TTA_TEAM_TEXTURE_GLOW, teamColor: UInt32(metadata.teamColor),
                required: needsTeamGlow, cache: cache)
            var key = base.0
            key.overrideIdentity = overrideImage?.identity ?? ""
            key.overrideContentKey = try WarcraftAssetDescriptorAdapter.imageContentKey(overrideImage)
            key.teamColorIndex = needsTeamColor ? UInt32(metadata.teamColor) : .max
            key.teamColorContentKey = teamColorImage?.contentKey ?? ""
            key.teamGlowIndex = needsTeamGlow ? UInt32(metadata.teamColor) : .max
            key.teamGlowContentKey = teamGlowImage?.contentKey ?? ""
            if let descriptor = modelDescriptors[key] {
                var model = descriptor.0
                model.metadata = metadata
                model.metadataStatus = metadataStatus
                models[entity.id] = WarcraftAssetDescriptorAdapter.model(model, template: descriptor.1)
                continue
            }
            var copied = base.1
            copied.overrideImage = overrideImage
            copied.teamColorImage = teamColorImage?.image
            copied.teamGlowImage = teamGlowImage?.image
            if key != base.0 {
                if let cached = cache.values.model(for: key) {
                    copied = cached
                } else {
                    cache.values.insert(copied, for: key)
                }
            }
            let template: WarcraftProductionEntityAsset
            if let cached = cache.values.adaptedModel(for: key) {
                template = cached
            } else {
                template = try WarcraftAssetDescriptorAdapter.modelTemplate(copied)
                cache.values.insertAdapted(template, for: key)
            }
            var model = copied
            model.metadata = metadata
            model.metadataStatus = metadataStatus
            modelDescriptors[key] = (copied, template)
            models[entity.id] = WarcraftAssetDescriptorAdapter.model(model, template: template)
        }
        return try WarcraftAssetDescriptorAdapter.production(
            abiVersion: actual, terrain: terrain, entities: models,
            counters: WarcraftAssetCacheCounters(
                hits: BZ_TTA_CacheHits(), misses: BZ_TTA_CacheMisses(),
                placeholderLogs: BZ_TTA_PlaceholderLogs(), metadataLogs: BZ_TTA_MetadataLogs()))
    }

    /* The server already resolved class/path aliases into the per-entity CS_IMAGES slot. */
    private static func copyEntityImage(_ abiVersion: UInt32, snapshot: OpaquePointer,
                                        imageIndex: UInt32) throws -> WarcraftExportedImage {
        guard imageIndex < 256 else {
            throw TabletopTransportError.malformedSnapshot(
                "entity image \(imageIndex) exceeds the frozen configstring range")
        }
        guard let asset = BZ_TTA_RegisterConfigString(
            abiVersion, snapshot, imageConfigStringBase + imageIndex, BZ_TTA_ASSET_IMAGE, nil) else {
            throw TabletopTransportError.malformedSnapshot(
                "entity image \(imageIndex) registration failed")
        }
        defer { BZ_TTAsset_Release(asset) }
        return try copyImage(asset)
    }

    /* Team imagery is entity state, so copy it by semantic kind/team without mutating shared models. */
    private static func copyTeamImage(
        _ abiVersion: UInt32, kind: bzTTTeamTextureKind_t, teamColor: UInt32, required: Bool,
        cache: LiveWarcraftCopyCache
    ) throws -> (image: WarcraftExportedImage, contentKey: String)? {
        guard required else { return nil }
        let count = BZ_TTA_TeamTextureCount(abiVersion, kind)
        let request = WarcraftTeamTextureRequest.resolve(
            kind: UInt32(kind.rawValue), teamColor: teamColor, count: count, required: true)
        guard case let .valid(key) = request else {
            throw TabletopTransportError.malformedSnapshot(
                "team texture kind \(kind.rawValue) index \(teamColor) is outside provider count \(count)")
        }
        if let cached = cache.teamImages[key] { return cached }
        guard let asset = BZ_TTA_RegisterTeamTexture(abiVersion, kind, teamColor) else {
            throw TabletopTransportError.malformedSnapshot(
                "team texture kind \(kind.rawValue) index \(teamColor) registration failed")
        }
        defer { BZ_TTAsset_Release(asset) }
        let copied = try copyImage(asset)
        let result = (copied, try WarcraftAssetDescriptorAdapter.imageContentKey(copied))
        cache.teamImages[key] = result
        return result
    }

    private static func resolveMetadata(_ abiVersion: UInt32, entity: TabletopEntitySnapshot)
        -> (UInt32, bzTTAssetMetadata_t) {
        var input = bzTTEntityMetadataInput_t()
        input.class_id = entity.metadata.classID
        input.override_mask = UInt32(BZ_TTA_METADATA_OVERRIDE_TEAM_COLOR)
        input.team_color = entity.metadata.player
        var output = bzTTAssetMetadata_t()
        let result = BZ_TTA_ResolveEntityMetadata(abiVersion, &input, &output)
        return (UInt32(result.rawValue), output)
    }

    private static func copyModel(
        _ asset: OpaquePointer, identity: String, metadataStatus: UInt32) throws
        -> WarcraftExportedModel {
        guard BZ_TTAsset_Kind(asset) == BZ_TTA_ASSET_MODEL else {
            throw TabletopTransportError.malformedSnapshot("registered model has the wrong asset kind")
        }
        var info = bzTTModelInfo_t(), metadata = bzTTAssetMetadata_t()
        guard BZ_TTAsset_ModelInfo(asset, &info), BZ_TTAsset_Metadata(asset, &metadata) else {
            throw TabletopTransportError.malformedSnapshot("registered model metadata could not be copied")
        }
        let counts = [info.geoset_count, info.material_count, info.layer_count, info.texture_count,
                      info.sequence_count, info.node_count]
        guard counts.allSatisfy({ $0 <= UInt32(maximumBufferElements) }) else {
            throw TabletopTransportError.malformedSnapshot("asset '\(identity)' descriptor count exceeds bounds")
        }
        var geosets: [WarcraftExportedGeoset] = []
        geosets.reserveCapacity(Int(info.geoset_count))
        for index in 0..<info.geoset_count { geosets.append(try copyGeoset(asset, index: index)) }
        var materials: [WarcraftExportedMaterial] = []
        materials.reserveCapacity(Int(info.material_count))
        for index in 0..<info.material_count {
            var raw = bzTTMaterialInfo_t()
            guard BZ_TTAsset_MaterialInfo(asset, index, &raw) else {
                throw TabletopTransportError.malformedSnapshot(
                    "asset '\(identity)' material \(index) could not be copied")
            }
            materials.append(WarcraftExportedMaterial(
                firstLayer: Int(raw.first_layer), layerCount: Int(raw.layer_count)))
        }
        var layers: [WarcraftExportedLayer] = []
        layers.reserveCapacity(Int(info.layer_count))
        for index in 0..<info.layer_count {
            var raw = bzTTMaterialLayerInfo_t()
            guard BZ_TTAsset_MaterialLayerInfo(asset, index, &raw) else {
                throw TabletopTransportError.malformedSnapshot(
                    "asset '\(identity)' layer \(index) could not be copied")
            }
            layers.append(WarcraftExportedLayer(
                blendMode: raw.blend_mode, flags: raw.flags,
                textureIndex: Int(raw.texture_index), alpha: raw.alpha))
        }
        var textures: [WarcraftExportedTexture] = []
        textures.reserveCapacity(Int(info.texture_count))
        for index in 0..<info.texture_count {
            var raw = bzTTModelTextureInfo_t()
            guard BZ_TTAsset_ModelTextureInfo(asset, index, &raw) else {
                throw TabletopTransportError.malformedSnapshot(
                    "asset '\(identity)' texture \(index) could not be copied")
            }
            let textureIdentity = tupleString(raw.identity)
            var image: WarcraftExportedImage?
            if raw.replaceable_id == 0 {
                guard let texture = BZ_TTA_RegisterModelTexture(
                    UInt32(BZ_TABLETOP_ASSETS_ABI_VERSION), asset, index) else {
                    throw TabletopTransportError.malformedSnapshot(
                        "asset '\(identity)' texture \(index) registration failed")
                }
                defer { BZ_TTAsset_Release(texture) }
                image = try copyImage(texture)
            }
            textures.append(WarcraftExportedTexture(
                identity: textureIdentity.isEmpty ? "<replaceable:\(raw.replaceable_id)>" : textureIdentity,
                replaceableID: raw.replaceable_id, image: image))
        }
        var sequences: [WarcraftExportedSequence] = []
        sequences.reserveCapacity(Int(info.sequence_count))
        for index in 0..<info.sequence_count {
            var raw = bzTTSequenceInfo_t()
            guard BZ_TTAsset_SequenceInfo(asset, index, &raw) else {
                throw TabletopTransportError.malformedSnapshot(
                    "asset '\(identity)' sequence \(index) could not be copied")
            }
            sequences.append(WarcraftExportedSequence(
                name: tupleString(raw.name), startMilliseconds: raw.start_msec,
                endMilliseconds: raw.end_msec, flags: raw.flags))
        }
        var nodes: [WarcraftExportedNode] = []
        nodes.reserveCapacity(Int(info.node_count))
        for index in 0..<info.node_count {
            var raw = bzTTNodeInfo_t()
            guard BZ_TTAsset_NodeInfo(asset, index, &raw) else {
                throw TabletopTransportError.malformedSnapshot(
                    "asset '\(identity)' node \(index) could not be copied")
            }
            nodes.append(WarcraftExportedNode(
                name: tupleString(raw.name), objectID: raw.object_id, parentID: raw.parent_id,
                flags: raw.flags, pivot: vector(raw.pivot),
                initialTranslation: vector(raw.initial_translation),
                initialRotation: (raw.initial_rotation_x, raw.initial_rotation_y,
                                  raw.initial_rotation_z, raw.initial_rotation_w),
                initialScale: vector(raw.initial_scale)))
        }
        return WarcraftExportedModel(
            identity: identity, placeholder: BZ_TTAsset_IsPlaceholder(asset),
            status: UInt32(BZ_TTAsset_Status(asset).rawValue), metadataStatus: metadataStatus,
            version: info.version,
            metadata: metadataValue(metadata), bounds: bounds(info.bounds), geosets: geosets,
            materials: materials, layers: layers, textures: textures,
            sequences: sequences, nodes: nodes)
    }

    private static func copyGeoset(_ asset: OpaquePointer, index: UInt32) throws
        -> WarcraftExportedGeoset {
        var info = bzTTGeosetInfo_t()
        guard BZ_TTAsset_GeosetInfo(asset, index, &info),
              [info.vertex_count, info.normal_count, info.uv_count, info.index_count]
                .allSatisfy({ $0 <= UInt32(maximumBufferElements) }) else {
            throw TabletopTransportError.malformedSnapshot("model geoset \(index) metadata is invalid")
        }
        var positions = [bzTTVec3_t](repeating: bzTTVec3_t(), count: Int(info.vertex_count))
        var normals = [bzTTVec3_t](repeating: bzTTVec3_t(), count: Int(info.normal_count))
        var uvs = [bzTTVec2_t](repeating: bzTTVec2_t(), count: Int(info.uv_count))
        var indices = [UInt16](repeating: 0, count: Int(info.index_count))
        let positionCount = positions.withUnsafeMutableBufferPointer {
            BZ_TTAsset_CopyGeosetVertices(asset, index, $0.baseAddress, UInt32($0.count))
        }
        let normalCount = normals.withUnsafeMutableBufferPointer {
            BZ_TTAsset_CopyGeosetNormals(asset, index, $0.baseAddress, UInt32($0.count))
        }
        let uvCount = uvs.withUnsafeMutableBufferPointer {
            BZ_TTAsset_CopyGeosetUVs(asset, index, $0.baseAddress, UInt32($0.count))
        }
        let indexCount = indices.withUnsafeMutableBufferPointer {
            BZ_TTAsset_CopyGeosetIndices(asset, index, $0.baseAddress, UInt32($0.count))
        }
        guard positionCount == info.vertex_count, normalCount == info.normal_count,
              uvCount == info.uv_count, indexCount == info.index_count else {
            throw TabletopTransportError.malformedSnapshot("model geoset \(index) copy was incomplete")
        }
        return WarcraftExportedGeoset(
            positions: positions.map(vector), normals: normals.map(vector),
            textureCoordinates: uvs.map { WarcraftVector2(x: $0.x, y: $0.y) },
            indices: indices, materialIndex: Int(info.material_index))
    }

    private static func copyImage(_ asset: OpaquePointer) throws -> WarcraftExportedImage {
        guard BZ_TTAsset_Kind(asset) == BZ_TTA_ASSET_IMAGE else {
            throw TabletopTransportError.malformedSnapshot("registered texture has the wrong asset kind")
        }
        var info = bzTTImageInfo_t()
        guard BZ_TTAsset_ImageInfo(asset, &info),
              info.format == UInt32(BZ_TTA_PIXEL_RGBA8.rawValue),
              info.data_bytes <= UInt32(maximumBufferElements * 4) else {
            throw TabletopTransportError.malformedSnapshot("registered texture metadata is invalid")
        }
        var bytes = [UInt8](repeating: 0, count: Int(info.data_bytes))
        let copied = bytes.withUnsafeMutableBufferPointer {
            BZ_TTAsset_CopyImagePixels(asset, $0.baseAddress, UInt32($0.count))
        }
        guard copied == info.data_bytes else {
            throw TabletopTransportError.malformedSnapshot("registered texture copy was incomplete")
        }
        let orientation: WarcraftImageOrientation
        switch info.origin {
        case UInt32(BZ_TTA_ORIGIN_TOP_LEFT.rawValue): orientation = .topLeft
        case UInt32(BZ_TTA_ORIGIN_BOTTOM_LEFT.rawValue): orientation = .bottomLeft
        default: throw TabletopTransportError.malformedSnapshot("registered texture origin is invalid")
        }
        return WarcraftExportedImage(
            identity: try copyIdentity(asset), placeholder: BZ_TTAsset_IsPlaceholder(asset),
            status: UInt32(BZ_TTAsset_Status(asset).rawValue), width: Int(info.width),
            height: Int(info.height), rowBytes: Int(info.row_bytes), rgba8: bytes,
            orientation: orientation)
    }

    private static func copyTerrain(mapName: String?, cache: LiveWarcraftCopyCache) throws
        -> WarcraftExportedTerrain? {
        guard let mapName, !mapName.isEmpty else { return nil }
        guard let terrain = BZ_TTA_LatestTerrain() else { return nil }
        defer { BZ_TTTerrain_Release(terrain) }
        var info = bzTTTerrainInfo_t()
        guard BZ_TTTerrain_Info(terrain, &info),
              info.width <= UInt32(maximumBufferElements),
              info.height <= UInt32(maximumBufferElements),
              info.height == 0 || info.width <= UInt32(maximumBufferElements) / info.height else {
            throw TabletopTransportError.malformedSnapshot("terrain metadata is invalid")
        }
        let groundTypes = try copyTerrainTypes(terrain, count: info.ground_type_count, ground: true)
        let cliffTypes = try copyTerrainTypes(terrain, count: info.cliff_type_count, ground: false)
        let terrainKey = "\(mapName):\(UInt(bitPattern: terrain)):" +
            "\(info.width)x\(info.height):\(info.min_x),\(info.min_y),\(info.max_x),\(info.max_y):" +
            "\(groundTypes):\(cliffTypes)"
        if let cached = cache.values.terrain(for: terrainKey) { return cached }
        var corners: [WarcraftExportedTerrainCorner] = []
        corners.reserveCapacity(Int(info.width * info.height))
        for z in 0..<info.height {
            for x in 0..<info.width {
                var raw = bzTTTerrainCorner_t()
                guard BZ_TTTerrain_Corner(terrain, x, z, &raw) else {
                    throw TabletopTransportError.malformedSnapshot(
                        "terrain corner \(x),\(z) could not be copied")
                }
                corners.append(WarcraftExportedTerrainCorner(
                    height: raw.height, waterHeight: raw.water_height,
                    groundID: raw.ground_id, cliffID: raw.cliff_id,
                    groundVariation: raw.ground_variation, cliffVariation: raw.cliff_variation,
                    cliffLevel: raw.cliff_level, flags: raw.flags))
            }
        }
        let copied = WarcraftExportedTerrain(
            cornerWidth: Int(info.width), cornerHeight: Int(info.height),
            tileWidth: Int(info.tile_width), tileHeight: Int(info.tile_height),
            chunkTiles: Int(info.chunk_tiles), chunkCountX: Int(info.chunk_count_x),
            chunkCountZ: Int(info.chunk_count_y),
            bounds: TabletopBounds2(minX: info.min_x, minZ: info.min_y,
                                    maxX: info.max_x, maxZ: info.max_y),
            groundTypes: groundTypes, cliffTypes: cliffTypes,
            groundTextures: try copyTerrainTextures(
                terrain, types: groundTypes, kind: BZ_TTA_TERRAIN_TEXTURE_GROUND),
            cliffTextures: try copyTerrainTextures(
                terrain, types: cliffTypes, kind: BZ_TTA_TERRAIN_TEXTURE_CLIFF),
            waterTexture: try copyTerrainTextures(
                terrain, types: [0], kind: BZ_TTA_TERRAIN_TEXTURE_WATER).first,
            corners: corners)
        cache.values.insert(copied, terrainKey: terrainKey)
        return copied
    }

    private static func copyTerrainTextures(
        _ terrain: OpaquePointer, types: [UInt32], kind: bzTTTerrainTextureKind_t) throws
        -> [WarcraftExportedTerrainTexture] {
        let count = BZ_TTTerrain_ReferencedTextureCount(terrain, kind)
        guard count <= UInt32(types.count) else {
            throw TabletopTransportError.malformedSnapshot(
                "terrain texture kind \(kind.rawValue) reference count exceeds its type table")
        }
        return try (0..<count).map { referenceIndex in
            var info = bzTTTerrainTextureInfo_t()
            guard BZ_TTTerrain_ReferencedTexture(terrain, kind, referenceIndex, &info),
                  Int(info.type_index) < types.count, types[Int(info.type_index)] == info.type_id,
                  info.corner_count > 0 else {
                throw TabletopTransportError.malformedSnapshot(
                    "terrain texture kind \(kind.rawValue) reference \(referenceIndex) is invalid")
            }
            guard let texture = BZ_TTA_RegisterTerrainTexture(
                UInt32(BZ_TABLETOP_ASSETS_ABI_VERSION), terrain, kind, info.type_index) else {
                throw TabletopTransportError.malformedSnapshot(
                    "terrain texture kind \(kind.rawValue) index \(info.type_index) registration failed")
            }
            defer { BZ_TTAsset_Release(texture) }
            return WarcraftExportedTerrainTexture(
                typeIndex: Int(info.type_index), typeID: info.type_id,
                cornerCount: Int(info.corner_count), image: try copyImage(texture))
        }
    }

    private static func copyTerrainTypes(_ terrain: OpaquePointer, count: UInt32, ground: Bool) throws
        -> [UInt32] {
        guard count <= UInt32(maximumBufferElements) else {
            throw TabletopTransportError.malformedSnapshot("terrain type count exceeds bounds")
        }
        return try (0..<count).map { index in
            var value: UInt32 = 0
            let copied = ground ? BZ_TTTerrain_GroundType(terrain, index, &value) :
                BZ_TTTerrain_CliffType(terrain, index, &value)
            guard copied else {
                throw TabletopTransportError.malformedSnapshot("terrain type \(index) could not be copied")
            }
            return value
        }
    }

    private static func copyIdentity(_ asset: OpaquePointer) throws -> String {
        var value = [CChar](repeating: 0, count: Int(BZ_TTA_MAX_IDENTITY))
        guard value.withUnsafeMutableBufferPointer({
            BZ_TTAsset_Identity(asset, $0.baseAddress, $0.count)
        }) else {
            throw TabletopTransportError.malformedSnapshot("asset identity could not be copied")
        }
        return String(cString: value)
    }

    private static func metadataValue(
        _ raw: bzTTAssetMetadata_t, fallback entity: TabletopEntitySnapshot? = nil)
        -> WarcraftAssetMetadata {
        let category: WarcraftEntityCategory
        switch raw.category {
        case UInt32(BZ_TTA_CATEGORY_MOBILE.rawValue): category = .unit
        case UInt32(BZ_TTA_CATEGORY_BUILDING.rawValue): category = .building
        case UInt32(BZ_TTA_CATEGORY_RESOURCE.rawValue): category = .resource
        case UInt32(BZ_TTA_CATEGORY_DOODAD.rawValue): category = .doodad
        case UInt32(BZ_TTA_CATEGORY_DESTRUCTABLE.rawValue): category = .destructable
        case UInt32(BZ_TTA_CATEGORY_ITEM.rawValue): category = .item
        default: category = .unknown
        }
        return WarcraftAssetMetadata(
            category: category, classID: raw.class_id != 0 ? raw.class_id : entity?.metadata.classID ?? 0,
            teamColor: raw.team_color <= UInt8.max ? UInt8(raw.team_color) :
                UInt8(truncatingIfNeeded: entity?.metadata.player ?? 0),
            tint: WarcraftColor(red: raw.tint_r, green: raw.tint_g, blue: raw.tint_b, alpha: raw.tint_a),
            footprint: WarcraftFootprint(width: raw.footprint_x, depth: raw.footprint_y))
    }

    private static func bounds(_ raw: bzTTBounds3_t) -> WarcraftExportedBounds {
        WarcraftExportedBounds(min: vector(raw.min), max: vector(raw.max), radius: raw.radius)
    }

    private static func vector(_ raw: bzTTVec3_t) -> WarcraftVector3 {
        WarcraftVector3(x: raw.x, y: raw.y, z: raw.z)
    }

    private static func tupleString<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) {
            String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }

}

actor LiveTabletopTransport: TabletopProductTransport, TabletopCommandTransport {
    private let baseArguments: [String]
    private var edition = TabletopEdition.roc
    private let logItemPublication: Bool
    private var bridge: BZTabletopBridge?
    private var sessionID: UInt64 = 0
    private let assetCache = LiveWarcraftCopyCache()
    private let audio = LiveTabletopAudio()
    private var initialAssetCounters: WarcraftAssetCacheCounters?
    private var loggedStableAssetCache = false
    private var observedItemClasses = Set<UInt32>()
    private var loggedItemClassCount = 0

    init(arguments: [String], logItemPublication: Bool = false) {
        self.baseArguments = arguments.filter { $0 != "-tft" }
        self.logItemPublication = logItemPublication
    }

    func configure(edition: TabletopEdition) async throws {
        guard bridge == nil else {
            throw TabletopTransportError.runtime("Edition cannot change while the engine is running")
        }
        self.edition = edition
    }

    func start() async throws {
        guard bridge == nil else { return }
        sessionID &+= 1
        assetCache.reset()
        initialAssetCounters = nil
        loggedStableAssetCache = false
        observedItemClasses.removeAll()
        loggedItemClassCount = 0
        try await audio.start()
        let arguments = edition == .tft ? baseArguments + ["-tft"] : baseArguments
        let bridge = BZTabletopBridge(arguments: arguments)
        self.bridge = bridge
        bridge.start()
        switch lifecycleState(bridge) {
        case .running, .suspended: return
        case .failed:
            let message = bridge.lastError ?? "Tabletop engine failed to start"
            bridge.stop()
            self.bridge = nil
            assetCache.reset()
            await audio.stop()
            throw TabletopTransportError.runtime(message)
        default:
            let message = "Tabletop engine entered unexpected state \(bridge.state.rawValue)"
            bridge.stop()
            self.bridge = nil
            assetCache.reset()
            await audio.stop()
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
            assetCache.reset()
            throw TabletopTransportError.runtime(message)
        default:
            throw TabletopTransportError.runtime("Tabletop engine entered unexpected state \(bridge.state.rawValue)")
        }
        guard let retained = BZ_TT_Latest() else { return nil }
        let snapshot = try TabletopSnapshotLeaseConsumer.consume(
            LiveSnapshotLease(retained: retained, sessionID: sessionID, assetCache: assetCache))
        try await audio.drain()
        logAssetSummary(snapshot)
        return snapshot
    }

    func stop() async {
        let active = bridge
        self.bridge = nil
        assetCache.reset()
        initialAssetCounters = nil
        loggedStableAssetCache = false
        observedItemClasses.removeAll()
        loggedItemClassCount = 0
        active?.stop()
        await audio.stop()
    }

    func submitMap(_ map: String) async throws {
        guard let bridge else { throw TabletopTransportError.terminal }
        guard bridge.submitMap(map) else {
            throw TabletopTransportError.commandRejected(UInt32(BZ_TT_ERR_INVALID_ARGUMENT.rawValue))
        }
    }

    func suspend() async { bridge?.suspend(); await audio.suspend() }
    func resume() async { bridge?.resume(); await audio.resume() }

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
        case .targetPoint(let x, let y, let generation):
            result = BZ_TT_PostTargetPoint(UInt32(BZ_TABLETOP_ABI_VERSION), generation, x, y)
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

    private func logAssetSummary(_ snapshot: TabletopSnapshot) {
        guard let assets = snapshot.warcraftAssets, let terrain = assets.terrain,
              !assets.entities.isEmpty else { return }
        observedItemClasses.formUnion(WarcraftItemPublication.classIDs(
            assets.entities.values.map(\.metadata)))
        let cachePhase: String
        if let initial = initialAssetCounters, !loggedStableAssetCache {
            guard assets.counters.hits > initial.hits, assets.counters.misses == initial.misses else { return }
            loggedStableAssetCache = true
            cachePhase = "stable"
        } else if loggedStableAssetCache {
            guard logItemPublication, observedItemClasses.count > loggedItemClassCount else { return }
            loggedItemClassCount = observedItemClasses.count
            cachePhase = "item"
        } else {
            initialAssetCounters = assets.counters
            cachePhase = "initial"
        }
        let models = assets.entities.values.compactMap(\.model)
        let geosets = models.reduce(0) { $0 + $1.geosets.count }
        let textured = models.flatMap(\.materials).filter { $0.texture != nil }.count
        let placeholderModels = assets.placeholderModelCount
        let placeholderMaterials = assets.placeholderMaterialCount
        let categories = Set(assets.entities.values.map(\.metadata.category.rawValue)).sorted()
            .joined(separator: ",")
        let uniqueModels = Set(assets.entities.values.map(\.identity)).count
        let itemClasses = WarcraftItemPublication.classList(observedItemClasses.sorted())
        let chunks = ((terrain.width + WarcraftAssetDescriptorAdapter.terrainChunkTiles - 1) /
            WarcraftAssetDescriptorAdapter.terrainChunkTiles) *
            ((terrain.height + WarcraftAssetDescriptorAdapter.terrainChunkTiles - 1) /
            WarcraftAssetDescriptorAdapter.terrainChunkTiles)
        let geometry = "terrain=\(terrain.width)x\(terrain.height) chunks=\(chunks) " +
            "terrain_textures=\(assets.terrainTextureCount) no_cliff=\(assets.terrainNoCliffCount) " +
            "fog=\(snapshot.fog == nil ? 0 : 1) entities=\(assets.entities.count) " +
            "active_visible=\(snapshot.entities.count + Int(snapshot.entitiesOverflowCount)) " +
            "overflow=\(snapshot.entitiesOverflowCount)"
        let model = "models=\(models.count) unique_models=\(uniqueModels) geosets=\(geosets) " +
            "textured_materials=\(textured) placeholders=\(assets.placeholderCount) " +
            "placeholder_models=\(placeholderModels) placeholder_materials=\(placeholderMaterials)"
        let cache = "hits=\(assets.counters.hits) misses=\(assets.counters.misses) " +
            "placeholder_logs=\(assets.counters.placeholderLogs) metadata_logs=\(assets.counters.metadataLogs)"
        let message = "OpenRealmTabletopAssets: abi=\(assets.abiVersion) cache_phase=\(cachePhase) " +
            "\(geometry) \(model) categories=\(categories) item_classes=\(itemClasses) \(cache)\n"
        FileHandle.standardError.write(Data(message.utf8))
    }
}
