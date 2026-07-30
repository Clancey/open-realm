import OpenRealmTabletopBridge

enum LiveTabletopCatalog {
    static func discover(dataPath: String, edition: TabletopEdition) throws -> [TabletopMapRecord] {
        var catalog: OpaquePointer?
        let rawEdition = edition == .roc ? BZ_TT_EDITION_ROC : BZ_TT_EDITION_TFT
        let result = BZ_TTCatalog_Discover(
            UInt32(BZ_TABLETOP_CATALOG_ABI_VERSION), dataPath, rawEdition, &catalog)
        guard result == BZ_TT_CATALOG_OK, let catalog else {
            throw TabletopTransportError.configuration(
                String(cString: BZ_TTCatalog_ResultString(result)))
        }
        defer { BZ_TTCatalog_Release(catalog) }
        return (0..<BZ_TTCatalog_Count(catalog)).compactMap { index in
            var raw = bzTTMapEntry_t()
            guard BZ_TTCatalog_Entry(catalog, index, &raw) else { return nil }
            return TabletopMapRecord(
                edition: edition,
                source: raw.source == BZ_TT_MAP_SOURCE_CAMPAIGN ? .campaign : .archive,
                campaignIndex: raw.campaign_index, missionIndex: raw.mission_index,
                campaign: string(raw.campaign), title: string(raw.title),
                subtitle: string(raw.subtitle), mapPath: string(raw.map_path))
        }
    }

    private static func string<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) {
            String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }
}
