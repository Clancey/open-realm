/*
 * test_bz_tabletop_transport_client.c — real client/cl_parse.c packet-parse
 * -> snapshot integration test, and typed command -> real common/net.c
 * loopback server-delivery integration test.
 *
 * Both scenarios exercise genuine production code, not a hand-rolled
 * substitute:
 *   - The manufactured "server" message is built with the real
 *     MSG_WriteByte/MSG_WriteShort/MSG_WriteLong/MSG_WriteString/
 *     MSG_WriteDeltaEntity/MSG_WriteDeltaPlayerState/MSG_WriteEntityBits
 *     functions from common/msg.c - the exact same functions
 *     server/sv_ents.c uses to build svc_frame/svc_playerinfo/
 *     svc_packetentities (see SV_WriteFrameToClient/SV_WritePlayerstateToClient/
 *     SV_EmitPacketEntities, server/sv_ents.c lines ~169-300) - then fed
 *     byte-for-byte into the real client/cl_parse.c::CL_ParseServerMessage().
 *   - The loopback delivery test drains the transport's command queue
 *     through the real Netchan_Transmit()/NET_GetPacket() path
 *     (common/net.c), the same NA_LOOPBACK mechanism the desktop listen
 *     server uses for same-process client<->server traffic.
 */
#include <string.h>

#include "client/client.h"
#include "common/net.h"
#include "platform/bridge/bz_tabletop_transport.h"
#include "test_framework.h"

void test_transport_stubs_reset(void);

static void reset_all(void) {
    test_transport_stubs_reset();
    BZ_TT_Shutdown();
    BZ_TT_Init();
    NET_Init();
}

/* --- Real client packet parse -> snapshot -------------------------------- */

/* Builds one manufactured server->client message: a configstring, a frame
 * header, a player-state delta, and one entity via packetentities - using
 * only real production MSG_Write*() encoders, in the exact order the real
 * server emits them (svc_frame before svc_playerinfo/svc_packetentities;
 * see server/sv_ents.c). */
static void build_server_message(sizeBuf_t *msg, BYTE *buffer, DWORD bufsize) {
    entityState_t zero_ent;
    entityState_t wanted_ent;
    PLAYER zero_player;
    PLAYER wanted_player;
    char name_buf[] = "P1";

    memset(msg, 0, sizeof(*msg));
    msg->data = buffer;
    msg->maxsize = bufsize;

    MSG_WriteByte(msg, svc_configstring);
    MSG_WriteShort(msg, CS_WORLD);
    MSG_WriteString(msg, "TestMap");

    MSG_WriteByte(msg, svc_frame);
    MSG_WriteLong(msg, 1);   /* serverframe */
    MSG_WriteLong(msg, 16);  /* servertime */
    MSG_WriteLong(msg, -1);  /* oldclientframe */

    memset(&zero_player, 0, sizeof(zero_player));
    memset(&wanted_player, 0, sizeof(wanted_player));
    wanted_player.team = 2;
    wanted_player.color = 1;
    wanted_player.race = 3;
    wanted_player.uiflags = 0x9;
    wanted_player.client_ui_state = 0;
    wanted_player.start_location = 4;
    wanted_player.name = name_buf;
    /* Only stats[2]/stats[4] are wire-transmitted (see playerStateFields[]
     * in common/msg.c) - stats[1]/[3]/[5] (gold/hero_tokens/food_used) are
     * a pre-existing gap in the wire protocol, not exercised here. */
    wanted_player.stats[2] = 60;  /* PLAYERSTATE_RESOURCE_LUMBER */
    wanted_player.stats[4] = 40;  /* PLAYERSTATE_RESOURCE_FOOD_CAP */
    MSG_WriteByte(msg, svc_playerinfo);
    MSG_WriteDeltaPlayerState(msg, &zero_player, &wanted_player);

    memset(&zero_ent, 0, sizeof(zero_ent));
    memset(&wanted_ent, 0, sizeof(wanted_ent));
    wanted_ent.number = 1;
    wanted_ent.class_id = 42;
    wanted_ent.origin.x = 100.0f;
    wanted_ent.origin.y = 200.0f;
    wanted_ent.model = 3;
    wanted_ent.player = 2;
    wanted_ent.flags = 0x1;
    wanted_ent.radius = 32.0f;
    MSG_WriteByte(msg, svc_packetentities);
    MSG_WriteDeltaEntity(msg, &zero_ent, &wanted_ent, true);
    MSG_WriteEntityBits(msg, 0, 0); /* terminator: nument==0 && bits==0 */
}

static void test_real_packet_parse_populates_snapshot(void) {
    reset_all();
    BYTE buffer[MAX_MSGLEN];
    sizeBuf_t msg;
    build_server_message(&msg, buffer, sizeof(buffer));

    /* This is the real production parser - not a test double. */
    CL_ParseServerMessage(&msg);

    ASSERT_EQ_INT(cl.frame.serverframe, 1);
    ASSERT_STR_EQ(cl.configstrings[CS_WORLD], "TestMap");

    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);

    char map_name[64] = {0};
    ASSERT(BZ_TTSnapshot_MapName(snap, map_name, sizeof(map_name)));
    ASSERT_STR_EQ(map_name, "TestMap");

    const bzTTPlayer_t *player = BZ_TTSnapshot_Player(snap);
    ASSERT_NOT_NULL(player);
    ASSERT_EQ_INT(player->team, 2);
    ASSERT_EQ_INT(player->color, 1);
    ASSERT_EQ_INT(player->race, 3);
    ASSERT_EQ_INT(player->uiflags, 0x9);
    ASSERT_EQ_INT(player->start_location, 4);
    ASSERT_EQ_INT(player->resource_lumber, 60);
    ASSERT_EQ_INT(player->resource_food_cap, 40);
    ASSERT_STR_EQ(player->name, "P1");

    /* The parser deliberately exposes MAX_CLIENT_ENTITIES slots; the transport
     * matches CL_AddEntities and publishes only slots with an active base model. */
    ASSERT_EQ_INT(BZ_TTSnapshot_EntityCount(snap), 1);
    ASSERT_EQ_INT(BZ_TTSnapshot_EntitiesOverflowCount(snap), 0);

    bzTTEntity_t ent;
    ASSERT(BZ_TTSnapshot_EntityAt(snap, 0, &ent));
    ASSERT_EQ_INT(ent.number, 1);
    ASSERT_EQ_INT(ent.class_id, 42);
    ASSERT_EQ_FLOAT(ent.origin_x, 100.0f, 0.01f);
    ASSERT_EQ_FLOAT(ent.origin_y, 200.0f, 0.01f);
    ASSERT_EQ_INT(ent.model, 3);
    ASSERT_EQ_INT(ent.player, 2);
    ASSERT_EQ_INT(ent.flags, 0x1);
    ASSERT_EQ_FLOAT(ent.radius, 32.0f, 0.01f);

    BZ_TTSnapshot_Release(snap);
}

static void test_unknown_opcode_stops_parsing_without_corrupting_state(void) {
    reset_all();
    BYTE buffer[64];
    sizeBuf_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.data = buffer;
    msg.maxsize = sizeof(buffer);

    MSG_WriteByte(&msg, svc_configstring);
    MSG_WriteShort(&msg, CS_WORLD);
    MSG_WriteString(&msg, "GoodMap");
    MSG_WriteByte(&msg, 0xEF); /* unrecognized opcode - CL_ParseServerMessage must stop, not crash */

    CL_ParseServerMessage(&msg);
    ASSERT_STR_EQ(cl.configstrings[CS_WORLD], "GoodMap");
}

static void write_action_frame(sizeBuf_t *msg, uiFrame_t *frame) {
    uiFrame_t zero = { 0 };
    MSG_WriteDeltaUIFrame(msg, &zero, frame, true);
    MSG_WriteByte(msg, 0);
}

static void test_action_layout_decodes_authoritative_cached_command_bar(void) {
    reset_all();
    BYTE buffer[2048];
    sizeBuf_t msg = { .data = buffer, .maxsize = sizeof(buffer) };
    uiFrame_t move = { 0 };
    uiFrame_t unsupported = { 0 };
    uiFrame_t cancel = { 0 };
    uiFrame_t injected = { 0 };

    move.number = 1;
    move.flags.type = FT_COMMANDBUTTON;
    move.flags.disabled = true;
    move.flags.target = UI_ACTION_TARGET_POINT;
    move.tex.index = 17;
    move.tooltip = "Move somewhere";
    move.onclick = "button CmdMove";
    move.hotkey = 'M';
    move.value = 0.75f;
    unsupported.number = 2;
    unsupported.flags.type = FT_COMMANDBUTTON;
    unsupported.flags.hidden = true;
    unsupported.tex.index = 18;
    unsupported.tooltip = "Research";
    unsupported.onclick = "research A001";
    cancel.number = 3;
    cancel.flags.type = FT_COMMANDBUTTON;
    cancel.tex.index = 19;
    cancel.onclick = "button CmdCancel";
    injected.number = 4;
    injected.flags.type = FT_COMMANDBUTTON;
    injected.onclick = "button CmdMove extra";

    MSG_WriteByte(&msg, svc_layout);
    MSG_WriteByte(&msg, LAYER_COMMANDBAR);
    write_action_frame(&msg, &move);
    write_action_frame(&msg, &unsupported);
    write_action_frame(&msg, &cancel);
    write_action_frame(&msg, &injected);
    MSG_WriteEntityBits(&msg, 0, 0);
    CL_ParseServerMessage(&msg);

    cl.playerstate.client_ui_state = CLIENT_UI_GAME;
    cl.playerstate.client_ui_target = UI_ACTION_TARGET_ENTITY_OR_POINT;
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    const bzTTActionLayout_t *layout = BZ_TTSnapshot_ActionLayout(snap);
    ASSERT(layout->present);
    ASSERT(layout->valid);
    ASSERT(layout->visible);
    ASSERT_EQ_INT(layout->current_target, BZ_TT_ACTION_TARGET_ENTITY_OR_POINT);
    ASSERT_EQ_INT(layout->num_buttons, 4);
    ASSERT_EQ_INT(layout->buttons[0].image_index, 17);
    ASSERT_STR_EQ(layout->buttons[0].tooltip, "Move somewhere");
    ASSERT_STR_EQ(layout->buttons[0].action_code, "CmdMove");
    ASSERT_EQ_INT(layout->buttons[0].semantic, BZ_TT_ACTION_BUTTON);
    ASSERT_EQ_INT(layout->buttons[0].target, BZ_TT_ACTION_TARGET_POINT);
    ASSERT(layout->buttons[0].disabled);
    ASSERT(!layout->buttons[0].hidden);
    ASSERT_EQ_FLOAT(layout->buttons[0].cooldown, 0.75f, 0.0001f);
    ASSERT_EQ_INT(layout->buttons[0].hotkey, 'M');
    ASSERT_EQ_INT(layout->buttons[0].grid_x, 0);
    ASSERT_EQ_INT(layout->buttons[0].grid_y, 0);
    ASSERT_EQ_INT(layout->buttons[1].semantic, BZ_TT_ACTION_UNSUPPORTED);
    ASSERT(layout->buttons[1].disabled);
    ASSERT(layout->buttons[1].hidden);
    ASSERT_EQ_INT(layout->buttons[1].grid_x, 1);
    ASSERT_EQ_INT(layout->buttons[2].semantic, BZ_TT_ACTION_CANCEL);
    ASSERT_STR_EQ(layout->buttons[2].action_code, "CmdCancel");
    ASSERT_EQ_INT(layout->buttons[3].semantic, BZ_TT_ACTION_UNSUPPORTED);
    ASSERT(layout->buttons[3].disabled);
    BZ_TTSnapshot_Release(snap);

    /* The same authoritative cache is decoded on later publishes; uiflags,
     * not cache presence, controls whether the layer is currently visible. */
    cl.playerstate.uiflags = 1u << LAYER_COMMANDBAR;
    BZ_TT_PublishSnapshotFromClient();
    snap = BZ_TT_Latest();
    layout = BZ_TTSnapshot_ActionLayout(snap);
    ASSERT(layout->present);
    ASSERT(layout->valid);
    ASSERT(!layout->visible);
    ASSERT_EQ_INT(layout->num_buttons, 4);
    BZ_TTSnapshot_Release(snap);

    cl.playerstate.uiflags = 0;
    cl.playerstate.client_ui_state = CLIENT_UI_LOADING;
    BZ_TT_PublishSnapshotFromClient();
    snap = BZ_TT_Latest();
    ASSERT(!BZ_TTSnapshot_ActionLayout(snap)->visible);
    BZ_TTSnapshot_Release(snap);
}

/* --- Typed command -> real loopback server delivery ---------------------- */

static void test_typed_commands_are_delivered_over_real_loopback(void) {
    reset_all();
    cls.netchan.remote_address.type = NA_LOOPBACK;

    uint32_t ids[2] = { 5, 9 };
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, ids, 2), BZ_TT_OK);
    ASSERT_EQ_INT(BZ_TT_PostSmartPoint(BZ_TABLETOP_ABI_VERSION, 0, 42.0f, -7.0f), BZ_TT_OK);
    ASSERT_EQ_INT(BZ_TT_PostTargetPoint(BZ_TABLETOP_ABI_VERSION, 0, 11.0f, 23.0f), BZ_TT_OK);
    ASSERT_EQ_INT(BZ_TT_PostCancel(BZ_TABLETOP_ABI_VERSION, 0), BZ_TT_OK);

    /* Real engine-thread integration path: drain the queue into the
     * client's outgoing netchan message, then transmit it exactly like
     * CL_SendCommand() does every frame. */
    BZ_TT_Drain();
    ASSERT(cls.netchan.message.cursize > 0);
    Netchan_Transmit(NS_CLIENT, &cls.netchan);
    /* Netchan_Transmit() resets cursize to 0 once the datagram is handed
     * to NET_SendPacket() - confirms the real send path actually ran. */
    ASSERT_EQ_INT(cls.netchan.message.cursize, 0);

    /* Now play "server": pull the datagram back out of the real loopback
     * ring exactly like SV_ReadPackets() would. */
    netadr_t from;
    BYTE recv_buffer[MAX_MSGLEN];
    sizeBuf_t recv_msg;
    memset(&recv_msg, 0, sizeof(recv_msg));
    recv_msg.data = recv_buffer;
    recv_msg.maxsize = sizeof(recv_buffer);

    int received = NET_GetPacket(NS_SERVER, &from, &recv_msg);
    ASSERT(received > 0);
    ASSERT_EQ_INT(from.type, NA_LOOPBACK);

    /* Decode the four clc_stringcmd entries in order, exactly like
     * SV_ExecuteClientMessage() would. */
    BYTE opcode = (BYTE)MSG_ReadByte(&recv_msg);
    ASSERT_EQ_INT(opcode, clc_stringcmd);
    ASSERT_STR_EQ(MSG_ReadString2(&recv_msg), "select 5 9");

    opcode = (BYTE)MSG_ReadByte(&recv_msg);
    ASSERT_EQ_INT(opcode, clc_stringcmd);
    ASSERT_STR_EQ(MSG_ReadString2(&recv_msg), "smartpoint 42 -7");

    opcode = (BYTE)MSG_ReadByte(&recv_msg);
    ASSERT_EQ_INT(opcode, clc_stringcmd);
    ASSERT_STR_EQ(MSG_ReadString2(&recv_msg), "point 11 23");

    opcode = (BYTE)MSG_ReadByte(&recv_msg);
    ASSERT_EQ_INT(opcode, clc_stringcmd);
    ASSERT_STR_EQ(MSG_ReadString2(&recv_msg), "button CmdCancel");

    /* No more data: the whole message was accounted for. */
    ASSERT_EQ_INT(recv_msg.readcount, recv_msg.cursize);
}

static void test_no_transmit_when_queue_is_empty(void) {
    reset_all();
    cls.netchan.remote_address.type = NA_LOOPBACK;

    BZ_TT_Drain(); /* nothing queued */
    ASSERT_EQ_INT(cls.netchan.message.cursize, 0);
    Netchan_Transmit(NS_CLIENT, &cls.netchan); /* must be a no-op, not send an empty datagram */

    netadr_t from;
    BYTE recv_buffer[MAX_MSGLEN];
    sizeBuf_t recv_msg;
    memset(&recv_msg, 0, sizeof(recv_msg));
    recv_msg.data = recv_buffer;
    recv_msg.maxsize = sizeof(recv_buffer);
    ASSERT_EQ_INT(NET_GetPacket(NS_SERVER, &from, &recv_msg), 0);
}

void run_bz_tabletop_transport_client_tests(void) {
    RUN_TEST(test_real_packet_parse_populates_snapshot);
    RUN_TEST(test_unknown_opcode_stops_parsing_without_corrupting_state);
    RUN_TEST(test_action_layout_decodes_authoritative_cached_command_bar);
    RUN_TEST(test_typed_commands_are_delivered_over_real_loopback);
    RUN_TEST(test_no_transmit_when_queue_is_empty);
}
