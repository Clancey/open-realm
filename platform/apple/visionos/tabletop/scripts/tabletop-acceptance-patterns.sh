#!/bin/sh

BZ_TABLETOP_METADATA_FAILURE_RE='metadata unavailable \([1-9][0-9]*\)|metadata status [1-9][0-9]*; using explicit placeholder|Missing production descriptor .*using placeholder'
BZ_TABLETOP_EXPECTED_ITEM_CLASSES='34656472,66696c72,66746172,676e6b63,74767270,7a697772'

bz_tabletop_exact_item_classes() { test "$1" = "$BZ_TABLETOP_EXPECTED_ITEM_CLASSES"; }
