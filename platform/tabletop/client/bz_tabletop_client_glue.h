/*
 * bz_tabletop_client_glue.h - internal (non-ABI) seam between the headless
 * tabletop client glue files (in this directory) and
 * platform/bridge/bz_tabletop_transport.c.
 *
 * This header is NOT part of the public bz_tabletop_transport.h ABI: it is
 * only ever included by .c files linked into a headless tabletop engine
 * archive (visionOS today; Android/Meta Quest later), and it is free to
 * use engine types (uiUnitData_t, bzTTUnitLayout_t).
 */
#ifndef BZ_TABLETOP_CLIENT_GLUE_H
#define BZ_TABLETOP_CLIENT_GLUE_H

#include "client/ui.h"
#include "platform/bridge/bz_tabletop_transport.h"

/*
 * Copies up to `cap` cached unit command-card layouts (the most recent data
 * delivered to ui.UpdateUnitUI() by CL_ParseUnitUI(), see
 * cl_scrn_tabletop_null.c) into `out`, converting from the engine-facing
 * uiUnitData_t/uiCommandButton_t/uiInventoryItem_t/uiQueueItem_t shape to
 * the ABI-facing bzTTUnitLayout_t shape. Returns the number written (0 if
 * the server has never sent unit UI data, or ui.UpdateUnitUI(0, NULL) most
 * recently cleared it - never fabricated).
 *
 * Implemented in ui_tabletop_null.c, which owns the cache this reads from.
 */
uint32_t BZTT_CopyCachedUnitUI(bzTTUnitLayout_t *out, uint32_t cap);

#endif /* BZ_TABLETOP_CLIENT_GLUE_H */
