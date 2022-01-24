// #define THREAD_COUNT 256
#define THREAD_COUNT 1

#include "../enum_reflection.h"
#include "instance.h"
#include "common.h"
#include "index.h"
#ifdef WINDOWS
#include <processthreadsapi.h>
#else
#include <pthread.h>
#endif
#include "../debug.h"
#include <mbedtls/error.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define FOR_ALL_PLAYERS(room, id) \
	struct Counter128 CONCAT(_i_,__LINE__) = (room)->playerSort; for(uint32_t (id) = 0; Counter128_set_next(&CONCAT(_i_,__LINE__), &id, 0); ++id)

#define FOR_EXCLUDING_PLAYER(room, session, id) \
	FOR_ALL_PLAYERS(room, id) \
		if(id != indexof((room)->players, (session)))

struct Counter128 {
	uint32_t bits[4];
};

void Counter128_clear(struct Counter128 *set) {
	memset(set->bits, 0, sizeof(set->bits));
}

_Bool Counter128_set(struct Counter128 *set, uint32_t bit, _Bool state) {
	uint32_t i = bit / (sizeof(*set->bits) * 8);
	bit %= sizeof(*set->bits) * 8;
	_Bool prev = (set->bits[i] >> bit) & 1;
	if(state)
		set->bits[i] |= 1 << bit;
	else
		set->bits[i] &= ~(1 << bit);
	return prev;
}

_Bool Counter128_set_next(struct Counter128 *set, uint32_t *bit, _Bool state) {
	for(uint32_t i = *bit / (sizeof(*set->bits) * 8); i < lengthof(set->bits); ++i) {
		uint32_t v = state ? ~set->bits[i] : set->bits[i];
		if(v) {
			*bit = i * (sizeof(*set->bits) * 8) +  __builtin_ctz(v);
			if(state)
				set->bits[i] |= set->bits[i] + 1;
			else
				set->bits[i] &= set->bits[i] - 1;
			return 1;
		}
	}
	return 0;
}

_Bool Counter128_contains(struct Counter128 set, struct Counter128 subset) {
	for(uint32_t i = 0; i < lengthof(set.bits); ++i)
		if((set.bits[i] & subset.bits[i]) != subset.bits[i])
			return 0;
	return 1;
}

_Bool Counter128_containsNone(struct Counter128 set, struct Counter128 subset) { // TODO: think of a better name for this
	for(uint32_t i = 0; i < lengthof(set.bits); ++i)
		if(set.bits[i] & subset.bits[i])
			return 0;
	return 1;
}

_Bool Counter128_isEmpty(struct Counter128 set) {
	for(uint32_t i = 0; i < lengthof(set.bits); ++i)
		if(set.bits[i])
			return 0;
	return 1;
}

struct Counter128 Counter128_or(struct Counter128 a, struct Counter128 b) {
	for(uint32_t i = 0; i < lengthof(a.bits); ++i)
		a.bits[i] |= b.bits[i];
	return a;
}

struct InstanceSession {
	struct NetSession net;
	ClientState clientState;
	struct String secret, userName;
	struct PlayerLobbyPermissionConfigurationNetSerializable permissions;

	struct Pong pong;
	struct Channels channels;
	struct PlayerStateHash stateHash;
	struct MultiplayerAvatarData avatar;

	union {
		struct {
			struct SongPackMask ownedSongPacks;
			struct BeatmapIdentifierNetSerializable recommendedBeatmap;
			struct GameplayModifiers recommendedModifiers;
		} lobby;
		struct {
			struct PlayerSpecificSettingsNetSerializable settings;
		} game;
	};
};
struct Room {
	struct NetKeypair keys;
	struct String managerId;
	struct GameplayServerConfiguration configuration;
	float syncBase;

	struct Counter128 inLobby;
	struct Counter128 levelFinished;
	struct BeatmapIdentifierNetSerializable selectedBeatmap;
	struct GameplayModifiers selectedModifiers; // TODO: recommend modifiers

	ServerState state;
	union {
		struct {
			float countdownEnd;
			struct Counter128 isReady;
			struct Counter128 isEntitled;
			struct Counter128 isSpectating;
			struct PlayersMissingEntitlementsNetSerializable buzzkills;
			_Bool canStart;
		} lobby;

		struct {
			float timeout;
			struct Counter128 isLoaded;
		} loadingScene;

		struct {
			float timeout;
			struct Counter128 isLoaded;
		} loadingSong;
	};

	struct Counter128 playerSort;
	struct InstanceSession players[];
};

struct Context {
	struct NetContext net;
	struct Room *TEMPglobalRoom; // TODO: room allocation (interface for wire.c)
};

static struct NetSession *instance_onResolve(struct Context *ctx, struct SS addr, void **userdata_out) { // TODO: needs mutex
	struct Room *room = ctx->TEMPglobalRoom;
	if(!room)
		return NULL;
	FOR_ALL_PLAYERS(room, id) {
		if(addrs_are_equal(&addr, NetSession_get_addr(&room->players[id].net))) {
			*userdata_out = room;
			return &room->players[id].net;
		}
	}
	return NULL;
}

static float room_get_syncTime(struct Room *room) {
	struct timespec now;
	if(clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return now.tv_sec + (now.tv_nsec / 1000) / 1000000.f - room->syncBase;
}

static _Bool should_countdown(struct Room *room) {
	return room->lobby.canStart && Counter128_contains(Counter128_or(room->lobby.isReady, room->lobby.isSpectating), room->playerSort);
}

static void refresh_countdown(struct Context *ctx, struct Room *room) {
	if(room->state != ServerState_Lobby)
		return;
	if(should_countdown(room)) {
		if(room->lobby.countdownEnd == 0) {
			room->lobby.countdownEnd = room_get_syncTime(room) + 5;
			struct SetCountdownEndTime r_countdown;
			r_countdown.base.syncTime = room_get_syncTime(room);
			r_countdown.newTime = room->lobby.countdownEnd;

			uint8_t resp[65536], *resp_end = resp;
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
			SERIALIZE_MENURPC(&resp_end, SetCountdownEndTime, r_countdown);
			FOR_ALL_PLAYERS(room, id)
				instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
		}
	} else {
		if(room->lobby.countdownEnd == 0)
			return;
		room->lobby.countdownEnd = 0;
		struct CancelCountdown r_cancelC;
		r_cancelC.base.syncTime = room_get_syncTime(room);

		uint8_t resp[65536], *resp_end = resp;
		pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
		uint8_t *routing_end = resp_end;
		SERIALIZE_MENURPC(&resp_end, CancelCountdown, r_cancelC);
		FOR_ALL_PLAYERS(room, id)
			instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);

		struct CancelLevelStart r_cancelL;
		r_cancelL.base.syncTime = room_get_syncTime(room);

		resp_end = routing_end;
		SERIALIZE_MENURPC(&resp_end, CancelLevelStart, r_cancelL);
		FOR_ALL_PLAYERS(room, id)
			instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
	}
}

static void refresh_button(struct Context *ctx, struct Room *room) {
	if(room->state != ServerState_Lobby)
		return;
	uint8_t resp[65536], *resp_end = resp;
	pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
	struct SetIsStartButtonEnabled r_button;
	r_button.base.syncTime = room_get_syncTime(room);
	if(Counter128_containsNone(room->inLobby, room->playerSort))
		r_button.reason = CannotStartGameReason_AllPlayersNotInLobby;
	if(!room->selectedBeatmap.beatmapCharacteristicSerializedName.length)
		r_button.reason = CannotStartGameReason_NoSongSelected;
	else if(room->lobby.buzzkills.count)
		r_button.reason = CannotStartGameReason_DoNotOwnSong;
	else if(Counter128_contains(room->lobby.isSpectating, room->playerSort))
		r_button.reason = CannotStartGameReason_AllPlayersSpectating;
	else
		r_button.reason = CannotStartGameReason_None;
	SERIALIZE_MENURPC(&resp_end, SetIsStartButtonEnabled, r_button);
	FOR_ALL_PLAYERS(room, id)
		instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
	room->lobby.canStart = (r_button.reason == CannotStartGameReason_None);
	refresh_countdown(ctx, room);
}

static _Bool PlayerStateHash_contains(struct PlayerStateHash state, const char *key) {
	uint32_t len = strlen(key);
	uint32_t hash = 0x21 ^ len;
	int32_t num3 = 0;
	while(len >= 4) {
		uint32_t num4 = (key[num3 + 3] << 24) | (key[num3 + 2] << 16) | (key[num3 + 1] << 8) | key[num3];
		num4 *= 1540483477;
		num4 ^= num4 >> 24;
		num4 *= 1540483477;
		hash *= 1540483477;
		hash ^= num4;
		num3 += 4;
		len -= 4;
	}
	switch(len) {
		case 3:
			hash ^= key[num3 + 2] << 16;
		case 2:
			hash ^= key[num3 + 1] << 8;
		case 1:
			hash ^= key[num3];
			hash *= 1540483477;
		case 0:
			break;
	}
	hash ^= hash >> 13;
	hash *= 1540483477;
	hash ^= (hash >> 15);
	for(uint_fast8_t i = 0; i < 3; ++i) {
		uint_fast8_t ind = (hash % 128);
		if(!(((ind >= 64) ? state.bloomFilter.d0 >> (ind - 64) : state.bloomFilter.d1 >> ind) & 1))
			return 0;
		hash >>= 8;
	}
	return 1;
}

static void session_refresh_stateHash(struct Context *ctx, struct Room *room, struct InstanceSession *session) {
	if(room->state != ServerState_Lobby)
		return;
	_Bool isSpectating = !PlayerStateHash_contains(session->stateHash, "wants_to_play_next_level");
	_Bool allPlayersSpectating = Counter128_contains(room->lobby.isSpectating, room->playerSort);
	if(Counter128_set(&room->lobby.isSpectating, indexof(room->players, session), isSpectating) != isSpectating)
		if(Counter128_contains(room->lobby.isSpectating, room->playerSort) != allPlayersSpectating)
			refresh_button(ctx, room);
}

static void session_refresh_state(struct Context *ctx, struct Room *room, struct InstanceSession *session) {
	if(room->state == ServerState_Lobby) {
		session->lobby.ownedSongPacks.bloomFilter = (struct BitMask128){0, 0};
		session->lobby.recommendedBeatmap = CLEAR_BEATMAP;
		session->lobby.recommendedModifiers = CLEAR_MODIFIERS;
		session_refresh_stateHash(ctx, room, session);
	} else {
		session->game.settings = (struct PlayerSpecificSettingsNetSerializable){
			.userId = session->permissions.userId,
			.userName = session->userName,
			.leftHanded = 0,
			.automaticPlayerHeight = 0,
			.playerHeight = 1.8,
			.headPosToPlayerHeightOffset = .1,
			.colorScheme = CLEAR_COLORSCHEME,
		};
	}
}

static void room_set_state(struct Context *ctx, struct Room *room, ServerState state) {
	room->state = state;
	switch(state) {
		case ServerState_Lobby: {
			room->selectedBeatmap = CLEAR_BEATMAP;
			room->selectedModifiers = CLEAR_MODIFIERS;
			room->lobby.countdownEnd = 0;
			Counter128_clear(&room->lobby.isReady);
			Counter128_clear(&room->lobby.isEntitled);
			Counter128_clear(&room->lobby.isSpectating);
			room->lobby.buzzkills.count = 0;
			room->lobby.canStart = 0;

			struct ReturnToMenu r_menu;
			r_menu.base.syncTime = room_get_syncTime(room);
			uint8_t resp[65536], *resp_end = resp;
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
			SERIALIZE_GAMEPLAYRPC(&resp_end, ReturnToMenu, r_menu);
			FOR_ALL_PLAYERS(room, id) {
				instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
				session_refresh_state(ctx, room, &room->players[id]);
			}
			refresh_button(ctx, room);
			break;
		}
		case ServerState_LoadingScene: {
			float startTime = room->lobby.countdownEnd;
			Counter128_clear(&room->levelFinished);
			room->loadingScene.timeout = room_get_syncTime(room) + LOAD_TIMEOUT;
			Counter128_clear(&room->loadingScene.isLoaded);

			struct SetSelectedBeatmap r_beatmap;
			struct SetSelectedGameplayModifiers r_modifiers;
			struct StartLevel r_start;
			struct GetGameplaySceneReady r_ready;
			r_ready.base.syncTime = r_start.base.syncTime = r_modifiers.base.syncTime = r_beatmap.base.syncTime = room_get_syncTime(room);
			r_start.beatmapId = r_beatmap.identifier = room->selectedBeatmap;
			r_start.gameplayModifiers = r_modifiers.gameplayModifiers = room->selectedModifiers;
			r_start.startTime = startTime;
			FOR_ALL_PLAYERS(room, id) {
				if(PER_PLAYER_DIFFICULTY) { // The "gaslighting" approach
					if(String_eq(room->players[id].lobby.recommendedBeatmap.levelID, room->selectedBeatmap.levelID)) {
						r_start.beatmapId.beatmapCharacteristicSerializedName = room->players[id].lobby.recommendedBeatmap.beatmapCharacteristicSerializedName;
						r_start.beatmapId.difficulty = room->players[id].lobby.recommendedBeatmap.difficulty;
					} else {
						r_start.beatmapId.beatmapCharacteristicSerializedName = room->selectedBeatmap.beatmapCharacteristicSerializedName;
						r_start.beatmapId.difficulty = room->selectedBeatmap.difficulty;
					}
					r_beatmap.identifier = r_start.beatmapId;
					r_modifiers.gameplayModifiers = r_start.gameplayModifiers;
				}
				uint8_t resp[65536], *resp_end = resp;
				pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
				SERIALIZE_MENURPC(&resp_end, SetSelectedBeatmap, r_beatmap);
				SERIALIZE_MENURPC(&resp_end, SetSelectedGameplayModifiers, r_modifiers);
				SERIALIZE_MENURPC(&resp_end, StartLevel, r_start);
				SERIALIZE_GAMEPLAYRPC(&resp_end, GetGameplaySceneReady, r_ready);
				instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
				session_refresh_state(ctx, room, &room->players[id]);
			}
			break;
		}
		case ServerState_LoadingSong: {
			room->loadingSong.timeout = room_get_syncTime(room) + LOAD_TIMEOUT;
			Counter128_clear(&room->loadingSong.isLoaded);

			uint8_t resp[65536], *resp_end = resp;
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
			struct GetGameplaySongReady r_ready;
			struct SetGameplaySceneSyncFinish r_sync;
			r_sync.base.syncTime = r_ready.base.syncTime = room_get_syncTime(room);
			r_sync.playersAtGameStart.count = 0;
			FOR_ALL_PLAYERS(room, id)
				r_sync.playersAtGameStart.activePlayerSpecificSettingsAtGameStart[r_sync.playersAtGameStart.count++] = room->players[id].game.settings;
			r_sync.sessionGameId.length = sprintf(r_sync.sessionGameId.data, "00000000-0000-0000-0000-000000000000");
			SERIALIZE_GAMEPLAYRPC(&resp_end, GetGameplaySongReady, r_ready);
			SERIALIZE_GAMEPLAYRPC(&resp_end, SetGameplaySceneSyncFinish, r_sync);
			FOR_ALL_PLAYERS(room, id)
				instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case ServerState_Game: {
			uint8_t resp[65536], *resp_end = resp;
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
			struct SetSongStartTime r_start;
			r_start.base.syncTime = room_get_syncTime(room);
			r_start.startTime = r_start.base.syncTime + .5;
			SERIALIZE_GAMEPLAYRPC(&resp_end, SetSongStartTime, r_start);
			FOR_ALL_PLAYERS(room, id)
				instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
	}
}

static void swap(uint8_t *a, uint8_t *b) {
	uint8_t c = *a;
	*a = *b, *b = c;
}

static void room_disconnect(struct Context *ctx, struct Room *room, struct InstanceSession *session, _Bool notify) { // TODO: misleading name; handles both disconnect and reconnect
	if(session->clientState == ClientState_disconnected)
		return;
	session->clientState = ClientState_disconnected;
	char addrstr[INET6_ADDRSTRLEN + 8];
	net_tostr(NetSession_get_addr(&session->net), addrstr);
	fprintf(stderr, "[INSTANCE] %sconnect %s\n", ctx ? "re" : "dis", addrstr);
	if(ctx)
		net_session_reset(&ctx->net, &session->net);
	else
		net_session_free(&session->net);
	while(session->channels.incomingFragmentsList) {
		struct IncomingFragments *e = session->channels.incomingFragmentsList;
		session->channels.incomingFragmentsList = session->channels.incomingFragmentsList->next;
		free(e);
	}
	uint32_t id = indexof(room->players, session);
	Counter128_set(&room->playerSort, id, 0);
	fprintf(stderr, "TODO: recount entitlement for selectedBeatmap\n");

	fprintf(stderr, "[INSTANCE] player bits: ");
	for(uint32_t i = 0; i < lengthof(room->playerSort.bits); ++i)
		for(uint32_t b = 0; b < sizeof(*room->playerSort.bits) * 8; ++b)
			fprintf(stderr, "%u", (room->playerSort.bits[i] >> b) & 1);
	fprintf(stderr, "\n");

	if(notify) {
		refresh_button(ctx, room);
		struct PlayerDisconnected r_disconnect;
		r_disconnect.disconnectedReason = DisconnectedReason_ClientConnectionClosed;

		uint8_t resp[65536], *resp_end = resp;
		pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
		SERIALIZE_CUSTOM(&resp_end, InternalMessageType_PlayerDisconnected)
			pkt_writePlayerDisconnected(&resp_end, r_disconnect);
		FOR_EXCLUDING_PLAYER(room, session, id)
			instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
	}
	/* TODO: close rooms with zero players
	 * calling `room_close()` here would crash the server if processing a `Merged` packet containing `Disconnect` followed by any other message
	 */
}

static void instance_onResend(struct Context *ctx, uint32_t currentTime, uint32_t *nextTick) { // TODO: needs mutex
	struct Room *room = ctx->TEMPglobalRoom;
	if(!room)
		return;
	FOR_ALL_PLAYERS(room, id) {
		struct InstanceSession *session = &room->players[id];
		uint32_t kickTime = NetSession_get_lastKeepAlive(&session->net) + 10000;
		if(currentTime > kickTime) {
			room_disconnect(NULL, room, session, 1);
		} else {
			if(kickTime < *nextTick)
				*nextTick = kickTime;
			flush_ack(&ctx->net, &session->net, &session->channels.ru.base.ack);
			flush_ack(&ctx->net, &session->net, &session->channels.ro.base.ack);
			for(uint_fast8_t i = 0; i < 64; ++i)
				try_resend(&ctx->net, &session->net, &session->channels.ru.base.resend[i], currentTime);
			for(uint_fast8_t i = 0; i < 64; ++i)
				try_resend(&ctx->net, &session->net, &session->channels.ro.base.resend[i], currentTime);
			try_resend(&ctx->net, &session->net, &session->channels.rs.resend, currentTime);
		}
	}
	switch(room->state) {
		case ServerState_Lobby: {
			if(!should_countdown(room))
				refresh_countdown(ctx, room);
			if(room->lobby.countdownEnd == 0)
				break;
			float delta = room->lobby.countdownEnd - room_get_syncTime(room);
			if(0) {
				case ServerState_LoadingScene:
				delta = room->loadingScene.timeout - room_get_syncTime(room);
			}
			if(0) {
				case ServerState_LoadingSong:
				delta = room->loadingSong.timeout - room_get_syncTime(room);
			}
			if(delta > 0) {
				uint32_t ctick = delta * 1000;
				if(ctick < 10)
					ctick = 10;
				if(*nextTick - currentTime > ctick)
					*nextTick = currentTime + ctick;
			} else {
				room_set_state(ctx, room, room->state + 1);
			}
			break;
		}
		default:;
	}
	FOR_ALL_PLAYERS(room, id)
		net_flush_merged(&ctx->net, &room->players[id].net);
}

static void handle_MenuRpc(struct Context *ctx, struct Room *room, struct InstanceSession *session, const uint8_t **data) {
	struct MenuRpcHeader rpc = pkt_readMenuRpcHeader(data);
	switch(rpc.type) {
		case MenuRpcType_SetPlayersMissingEntitlementsToLevel: fprintf(stderr, "[INSTANCE] MenuRpcType_SetPlayersMissingEntitlementsToLevel not implemented\n"); abort();
		case MenuRpcType_GetIsEntitledToLevel: fprintf(stderr, "[INSTANCE] MenuRpcType_GetIsEntitledToLevel not implemented\n"); abort();
		case MenuRpcType_SetIsEntitledToLevel: {
			struct SetIsEntitledToLevel entitlement = pkt_readSetIsEntitledToLevel(data);
			if(room->state != ServerState_Lobby || !String_eq(entitlement.levelId, room->selectedBeatmap.levelID))
				break;
			if(Counter128_set(&room->lobby.isEntitled, indexof(room->players, session), 1))
				break;
			if(entitlement.entitlementStatus != EntitlementsStatus_Ok)
				room->lobby.buzzkills.playersWithoutEntitlements[room->lobby.buzzkills.count++] = session->permissions.userId;
			if(Counter128_contains(room->lobby.isEntitled, room->playerSort)) {
				uint8_t resp[65536], *resp_end = resp;
				pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
				struct SetPlayersMissingEntitlementsToLevel r_missing;
				r_missing.base.syncTime = room_get_syncTime(room);
				r_missing.playersMissingEntitlements = room->lobby.buzzkills;
				SERIALIZE_MENURPC(&resp_end, SetPlayersMissingEntitlementsToLevel, r_missing);
				FOR_ALL_PLAYERS(room, id)
					instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
				refresh_button(ctx, room);
			}
			break;
		}
		case MenuRpcType_InvalidateLevelEntitlementStatuses: fprintf(stderr, "[INSTANCE] MenuRpcType_InvalidateLevelEntitlementStatuses not implemented\n"); abort();
		case MenuRpcType_SelectLevelPack: fprintf(stderr, "[INSTANCE] MenuRpcType_SelectLevelPack not implemented\n"); abort();
		case MenuRpcType_SetSelectedBeatmap: fprintf(stderr, "[INSTANCE] MenuRpcType_SetSelectedBeatmap not implemented\n"); abort();
		case MenuRpcType_GetSelectedBeatmap: fprintf(stderr, "[INSTANCE] MenuRpcType_GetSelectedBeatmap not implemented\n"); abort();
		case MenuRpcType_RecommendBeatmap: {
			struct RecommendBeatmap beatmap = pkt_readRecommendBeatmap(data);
			if(room->state != ServerState_Lobby)
				break;
			fprintf(stderr, "\n\n\n%.*s\n\n\n\n", beatmap.identifier.levelID.length, beatmap.identifier.levelID.data);
			session->lobby.recommendedBeatmap = beatmap.identifier;
			if(0) {
				case MenuRpcType_ClearRecommendedBeatmap:
				pkt_readClearRecommendedBeatmap(data);
				if(room->state != ServerState_Lobby)
					break;
				session->lobby.recommendedBeatmap = CLEAR_BEATMAP;
			}
			if(session->permissions.hasRecommendBeatmapsPermission) {
				uint32_t select = ~0;
				switch(room->configuration.songSelectionMode) {
					case SongSelectionMode_Vote: {
						uint8_t counter[126], max = 0;
						FOR_ALL_PLAYERS(room, id) {
							counter[id] = 0;
							if(room->players[id].permissions.hasRecommendBeatmapsPermission && room->players[id].lobby.recommendedBeatmap.beatmapCharacteristicSerializedName.length) {
								FOR_ALL_PLAYERS(room, cmp) {
									if(!room->players[cmp].permissions.hasRecommendBeatmapsPermission)
										continue;
									if(id == cmp || (String_eq(room->players[id].lobby.recommendedBeatmap.levelID, room->players[cmp].lobby.recommendedBeatmap.levelID) && String_eq(room->players[id].lobby.recommendedBeatmap.beatmapCharacteristicSerializedName, room->players[cmp].lobby.recommendedBeatmap.beatmapCharacteristicSerializedName) && (PER_PLAYER_DIFFICULTY || room->players[id].lobby.recommendedBeatmap.difficulty == room->players[cmp].lobby.recommendedBeatmap.difficulty))) {
										if(++counter[cmp] > max) {
											select = cmp;
											max = counter[cmp];
										}
										break;
									}
								}
							}
						}
						break;
					}
					case SongSelectionMode_Random:
					case SongSelectionMode_OwnerPicks: {
						FOR_ALL_PLAYERS(room, id) {
							if(room->players[id].permissions.isServerOwner) {
								select = id;
								break;
							}
						}
						break;
					}
					case SongSelectionMode_RandomPlayerPicks: {
						uint32_t validPlayerCount = 0, luckyFellow;
						mbedtls_ctr_drbg_random(net_get_ctr_drbg(&ctx->net), (uint8_t*)&luckyFellow, sizeof(luckyFellow));
						FOR_ALL_PLAYERS(room, id)
							validPlayerCount += room->players[id].permissions.hasRecommendBeatmapsPermission;
						if(validPlayerCount) {
							luckyFellow %= validPlayerCount;
							FOR_ALL_PLAYERS(room, id) {
								if(--validPlayerCount == luckyFellow) {
									select = id;
									break;
								}
							}
						}
					}
				}
				room->lobby.canStart = 0, room->lobby.buzzkills.count = 0;
				if(select != ~0 && room->players[select].lobby.recommendedBeatmap.beatmapCharacteristicSerializedName.length) {
					Counter128_clear(&room->lobby.isEntitled);
					room->selectedBeatmap = room->players[select].lobby.recommendedBeatmap;
					uint8_t resp[65536], *resp_end = resp;
					struct GetIsEntitledToLevel r_level;
					r_level.base.syncTime = room_get_syncTime(room);
					r_level.levelId = session->lobby.recommendedBeatmap.levelID;
					pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
					SERIALIZE_MENURPC(&resp_end, GetIsEntitledToLevel, r_level);
					FOR_ALL_PLAYERS(room, id)
						instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
				} else {
					room->lobby.isEntitled = room->playerSort;
					room->selectedBeatmap = CLEAR_BEATMAP;
				}
				refresh_button(ctx, room);
			}
			break;
		}
		case MenuRpcType_GetRecommendedBeatmap: pkt_readGetRecommendedBeatmap(data); break;
		case MenuRpcType_SetSelectedGameplayModifiers: fprintf(stderr, "[INSTANCE] BAD TYPE: MenuRpcType_SetSelectedGameplayModifiers\n"); break;
		case MenuRpcType_GetSelectedGameplayModifiers: fprintf(stderr, "[INSTANCE] MenuRpcType_GetSelectedGameplayModifiers not implemented\n"); abort();
		case MenuRpcType_RecommendGameplayModifiers: {
			struct RecommendGameplayModifiers modifiers = pkt_readRecommendGameplayModifiers(data);
			if(room->state != ServerState_Lobby)
				break;
			if(0) {
				case MenuRpcType_ClearRecommendedGameplayModifiers:
				if(room->state != ServerState_Lobby)
					break;
				modifiers.gameplayModifiers = CLEAR_MODIFIERS;
			}
			uint8_t resp[65536], *resp_end = resp;
			struct SetSelectedGameplayModifiers r_modifiers;
			r_modifiers.base.syncTime = room_get_syncTime(room);
			if(session->permissions.hasRecommendGameplayModifiersPermission)
				session->lobby.recommendedModifiers = modifiers.gameplayModifiers;
			r_modifiers.gameplayModifiers = room->selectedModifiers;
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
			SERIALIZE_MENURPC(&resp_end, SetSelectedGameplayModifiers, r_modifiers);
			instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case MenuRpcType_GetRecommendedGameplayModifiers: pkt_readGetRecommendedGameplayModifiers(data); break;
		case MenuRpcType_LevelLoadError: fprintf(stderr, "[INSTANCE] MenuRpcType_LevelLoadError not implemented\n"); abort();
		case MenuRpcType_LevelLoadSuccess: fprintf(stderr, "[INSTANCE] MenuRpcType_LevelLoadSuccess not implemented\n"); abort();
		case MenuRpcType_StartLevel: fprintf(stderr, "[INSTANCE] MenuRpcType_StartLevel not implemented\n"); abort();
		case MenuRpcType_GetStartedLevel: {
			pkt_readGetStartedLevel(data);
			if(room->state != ServerState_Lobby) {
				fprintf(stderr, "[INSTANCE] MenuRpcType_GetStartedLevel not implemented\n"); abort();
			} else {
				uint8_t resp[65536], *resp_end = resp;
				struct SetIsStartButtonEnabled r_button;
				r_button.base.syncTime = room_get_syncTime(room);
				r_button.reason = CannotStartGameReason_NoSongSelected;
				pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
				SERIALIZE_MENURPC(&resp_end, SetIsStartButtonEnabled, r_button);
				instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			}
			break;
		}
		case MenuRpcType_CancelLevelStart: fprintf(stderr, "[INSTANCE] MenuRpcType_CancelLevelStart not implemented\n"); abort();
		case MenuRpcType_GetMultiplayerGameState: {
			pkt_readGetMultiplayerGameState(data);
			uint8_t resp[65536], *resp_end = resp;
			struct SetMultiplayerGameState r_state;
			r_state.base.syncTime = room_get_syncTime(room);
			if(room->state == ServerState_Lobby)
				r_state.lobbyState = MultiplayerGameState_Lobby;
			else
				r_state.lobbyState = MultiplayerGameState_Game;
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
			SERIALIZE_MENURPC(&resp_end, SetMultiplayerGameState, r_state);
			instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case MenuRpcType_SetMultiplayerGameState: fprintf(stderr, "[INSTANCE] BAD TYPE: MenuRpcType_SetMultiplayerGameState\n"); break;
		case MenuRpcType_GetIsReady: pkt_readGetIsReady(data); break;
		case MenuRpcType_SetIsReady: {
			_Bool isReady = pkt_readSetIsReady(data).isReady;
			if(room->state == ServerState_Lobby)
				if(Counter128_set(&room->lobby.isReady, indexof(room->players, session), isReady) != isReady)
					refresh_countdown(ctx, room);
			break;
		}
		case MenuRpcType_SetStartGameTime: fprintf(stderr, "[INSTANCE] MenuRpcType_SetStartGameTime not implemented\n"); abort();
		case MenuRpcType_CancelStartGameTime: fprintf(stderr, "[INSTANCE] MenuRpcType_CancelStartGameTime not implemented\n"); abort();
		case MenuRpcType_GetIsInLobby: pkt_readGetIsInLobby(data); break;
		case MenuRpcType_SetIsInLobby: {
			_Bool inLobby = pkt_readSetIsInLobby(data).isBack;
			if(Counter128_set(&room->inLobby, indexof(room->players, session), inLobby) != inLobby)
				refresh_button(ctx, room);
			break;
		}
		case MenuRpcType_GetCountdownEndTime: {
			pkt_readGetCountdownEndTime(data);
			uint8_t resp[65536], *resp_end = resp;
			struct SetIsStartButtonEnabled r_button;
			r_button.base.syncTime = room_get_syncTime(room);
			r_button.reason = CannotStartGameReason_NoSongSelected;
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
			SERIALIZE_MENURPC(&resp_end, SetIsStartButtonEnabled, r_button);
			instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case MenuRpcType_SetCountdownEndTime: fprintf(stderr, "[INSTANCE] BAD TYPE: MenuRpcType_SetCountdownEndTime\n"); break;
		case MenuRpcType_CancelCountdown: fprintf(stderr, "[INSTANCE] BAD TYPE: MenuRpcType_CancelCountdown\n"); break;
		case MenuRpcType_GetOwnedSongPacks: fprintf(stderr, "[INSTANCE] MenuRpcType_GetOwnedSongPacks not implemented\n"); abort();
		case MenuRpcType_SetOwnedSongPacks: {
			struct SetOwnedSongPacks owned = pkt_readSetOwnedSongPacks(data);
			if(room->state == ServerState_Lobby)
				session->lobby.ownedSongPacks = owned.songPackMask;
			break;
		}
		case MenuRpcType_RequestKickPlayer: fprintf(stderr, "[INSTANCE] MenuRpcType_RequestKickPlayer not implemented\n"); abort();
		case MenuRpcType_GetPermissionConfiguration:  {
			pkt_readGetPermissionConfiguration(data);
			uint8_t resp[65536], *resp_end = resp;
			struct SetPermissionConfiguration r_permission;
			r_permission.base.syncTime = room_get_syncTime(room);
			r_permission.playersPermissionConfiguration.count = 0;
			FOR_ALL_PLAYERS(room, id)
				r_permission.playersPermissionConfiguration.playersPermission[r_permission.playersPermissionConfiguration.count++] = room->players[id].permissions;
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
			SERIALIZE_MENURPC(&resp_end, SetPermissionConfiguration, r_permission);
			instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
			break;
		}
		case MenuRpcType_SetPermissionConfiguration: fprintf(stderr, "[INSTANCE] BAD TYPE: MenuRpcType_SetPermissionConfiguration\n"); break;
		case MenuRpcType_GetIsStartButtonEnabled: fprintf(stderr, "[INSTANCE] MenuRpcType_GetIsStartButtonEnabled not implemented\n"); abort();
		case MenuRpcType_SetIsStartButtonEnabled: fprintf(stderr, "[INSTANCE] BAD TYPE: MenuRpcType_SetIsStartButtonEnabled\n"); break;
		default: fprintf(stderr, "[INSTANCE] BAD MENU RPC TYPE\n");
	}
}

static void handle_GameplayRpc(struct Context *ctx, struct Room *room, struct InstanceSession *session, const uint8_t **data) {
	struct GameplayRpcHeader rpc = pkt_readGameplayRpcHeader(data);
	switch(rpc.type) {
		case GameplayRpcType_SetGameplaySceneSyncFinish: fprintf(stderr, "[INSTANCE] GameplayRpcType_SetGameplaySceneSyncFinish not implemented\n"); abort();
		case GameplayRpcType_SetGameplaySceneReady: {
			struct SetGameplaySceneReady ready = pkt_readSetGameplaySceneReady(data);
			if(room->state != ServerState_LoadingScene)
				break;
			session->game.settings = ready.playerSpecificSettingsNetSerializable;
			if(!Counter128_set(&room->loadingScene.isLoaded, indexof(room->players, session), 1))
				if(Counter128_contains(room->loadingScene.isLoaded, room->playerSort))
					room_set_state(ctx, room, ServerState_LoadingSong);
			break;
		}
		case GameplayRpcType_GetGameplaySceneReady: fprintf(stderr, "[INSTANCE] GameplayRpcType_GetGameplaySceneReady not implemented\n"); abort();
		case GameplayRpcType_SetActivePlayerFailedToConnect: fprintf(stderr, "[INSTANCE] GameplayRpcType_SetActivePlayerFailedToConnect not implemented\n"); abort();
		case GameplayRpcType_SetGameplaySongReady: {
			pkt_readSetGameplaySongReady(data);
			if(room->state != ServerState_LoadingSong)
				break;
			if(!Counter128_set(&room->loadingSong.isLoaded, indexof(room->players, session), 1))
				if(Counter128_contains(room->loadingSong.isLoaded, room->playerSort))
					room_set_state(ctx, room, ServerState_Game);
			break;
		}
		case GameplayRpcType_GetGameplaySongReady: fprintf(stderr, "[INSTANCE] GameplayRpcType_GetGameplaySongReady not implemented\n"); abort();
		case GameplayRpcType_SetSongStartTime: fprintf(stderr, "[INSTANCE] GameplayRpcType_SetSongStartTime not implemented\n"); abort();
		case GameplayRpcType_NoteCut: pkt_readNoteCut(data); break;
		case GameplayRpcType_NoteMissed: pkt_readNoteMissed(data); break;
		case GameplayRpcType_LevelFinished: {
			pkt_readLevelFinished(data);
			if(room->state == ServerState_Lobby)
				break;
			if(!Counter128_set(&room->levelFinished, indexof(room->players, session), 1)) {
				if(Counter128_contains(room->levelFinished, room->playerSort)) {
					fprintf(stderr, "TODO: wait on results screen\n");
					room_set_state(ctx, room, ServerState_Lobby);
				}
			}
			break;
		}
		case GameplayRpcType_ReturnToMenu: fprintf(stderr, "[INSTANCE] BAD TYPE: GameplayRpcType_ReturnToMenu\n"); break;
		case GameplayRpcType_RequestReturnToMenu: {
			pkt_readRequestReturnToMenu(data);
			if(room->state != ServerState_Lobby && String_eq(session->permissions.userId, room->managerId))
				room_set_state(ctx, room, ServerState_Lobby);
			break;
		}
		default: fprintf(stderr, "[INSTANCE] BAD GAMEPLAY RPC TYPE\n");
	}
}

static void handle_PlayerIdentity(struct Context *ctx, struct Room *room, struct InstanceSession *session, const uint8_t **data) {
	struct PlayerIdentity identity = pkt_readPlayerIdentity(data);
	session->stateHash = identity.playerState;
	session_refresh_stateHash(ctx, room, session);
	session->avatar = identity.playerAvatar;
	if(session->clientState != ClientState_accepted)
		return;
	session->clientState = ClientState_connected;

	uint8_t resp[65536], *resp_end = resp;
	{
		struct PlayerConnected r_connected;
		r_connected.remoteConnectionId = indexof(room->players, session) + 1;
		r_connected.userId = session->permissions.userId;
		r_connected.userName = session->userName;
		r_connected.isConnectionOwner = 0;
		pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
		SERIALIZE_CUSTOM(&resp_end, InternalMessageType_PlayerConnected)
			pkt_writePlayerConnected(&resp_end, r_connected);
		FOR_EXCLUDING_PLAYER(room, session, id)
			instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);

		struct PlayerSortOrderUpdate r_sort;
		r_sort.userId = session->permissions.userId;
		r_sort.sortIndex = indexof(room->players, session);
		resp_end = resp;
		pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
		SERIALIZE_CUSTOM(&resp_end, InternalMessageType_PlayerSortOrderUpdate)
			pkt_writePlayerSortOrderUpdate(&resp_end, r_sort);
		FOR_ALL_PLAYERS(room, id)
			instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
	}

	FOR_EXCLUDING_PLAYER(room, session, id) {
		struct PlayerConnected r_connected;
		r_connected.remoteConnectionId = id + 1;
		r_connected.userId = room->players[id].permissions.userId;
		r_connected.userName = room->players[id].userName;
		r_connected.isConnectionOwner = 0;
		resp_end = resp;
		pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
		SERIALIZE_CUSTOM(&resp_end, InternalMessageType_PlayerConnected)
			pkt_writePlayerConnected(&resp_end, r_connected);
		instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);

		struct PlayerSortOrderUpdate r_sort;
		r_sort.userId = room->players[id].permissions.userId;
		r_sort.sortIndex = id;
		resp_end = resp;
		pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
		SERIALIZE_CUSTOM(&resp_end, InternalMessageType_PlayerSortOrderUpdate)
			pkt_writePlayerSortOrderUpdate(&resp_end, r_sort);
		instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);

		struct PlayerIdentity r_identity;
		r_identity.playerState = room->players[id].stateHash;
		r_identity.playerAvatar = room->players[id].avatar;
		r_identity.random.length = 0;
		r_identity.publicEncryptionKey.length = 0;
		fprintf(stderr, "TODO: do we need to include the encrytion key?\n");
		resp_end = resp;
		pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
		SERIALIZE_CUSTOM(&resp_end, InternalMessageType_PlayerSortOrderUpdate)
			pkt_writePlayerIdentity(&resp_end, r_identity);
		instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
	}

	resp_end = resp;
	pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
	struct RemoteProcedureCall base;
	base.syncTime = room_get_syncTime(room);
	SERIALIZE_MENURPC(&resp_end, GetRecommendedBeatmap, (struct GetRecommendedBeatmap){.base = base});
	SERIALIZE_MENURPC(&resp_end, GetRecommendedGameplayModifiers, (struct GetRecommendedGameplayModifiers){.base = base});
	SERIALIZE_MENURPC(&resp_end, GetIsReady, (struct GetIsReady){.base = base});
	SERIALIZE_MENURPC(&resp_end, GetIsInLobby, (struct GetIsInLobby){.base = base});
	FOR_ALL_PLAYERS(room, id)
		instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
	fprintf(stderr, "TODO: are these necessary?\n");
}

static void handle_MultiplayerSession(struct Context *ctx, struct Room *room, struct InstanceSession *session, const uint8_t **data) {
	struct MultiplayerSessionMessageHeader message = pkt_readMultiplayerSessionMessageHeader(data);
	switch(message.type) {
		case MultiplayerSessionMessageType_MenuRpc: handle_MenuRpc(ctx, room, session, data); break;
		case MultiplayerSessionMessageType_GameplayRpc: handle_GameplayRpc(ctx, room, session, data); break;
		case MultiplayerSessionMessageType_NodePoseSyncState: pkt_readNodePoseSyncState(data); break;
		case MultiplayerSessionMessageType_ScoreSyncState: pkt_readScoreSyncState(data); break;
		case MultiplayerSessionMessageType_NodePoseSyncStateDelta: pkt_readNodePoseSyncStateDelta(data); break;
		case MultiplayerSessionMessageType_ScoreSyncStateDelta: pkt_readScoreSyncStateDelta(data); break;
		case MultiplayerSessionMessageType_MpCore: {
			struct MpCore mpHeader = pkt_readMpCore(data);
			if(mpHeader.packetType.length == 15 && memcmp(mpHeader.packetType.data, "MpBeatmapPacket", 15) == 0) {
				fprintf(stderr, "[INSTANCE] 'MpBeatmapPacket' not implemented\n");
				abort();
			} else {
				fprintf(stderr, "[INSTANCE] BAD MPCORE MESSAGE TYPE: '%.*s'\n", mpHeader.packetType.length, mpHeader.packetType.data);
			}
		}
		default: fprintf(stderr, "[INSTANCE] BAD MULTIPLAYER SESSION MESSAGE TYPE\n");
	}
}

static void handle_Unreliable(struct Context *ctx, struct Room *room, struct InstanceSession *session, const uint8_t **data, const uint8_t *end) {
	struct RoutingHeader routing = pkt_readRoutingHeader(data);
	if(routing.connectionId) {
		if(routing.connectionId == 127) {
			uint8_t resp[65536], *resp_end = resp;
			pkt_writeNetPacketHeader(&resp_end, (struct NetPacketHeader){PacketProperty_Unreliable, 0, 0});
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){indexof(room->players, session) + 1, 127, 0});
			pkt_writeUint8Array(&resp_end, *data, end - *data);
			FOR_EXCLUDING_PLAYER(room, session, id)
				net_send_internal(&ctx->net, &room->players[id].net, resp, resp_end - resp, 1);
		} else {
			fprintf(stderr, "Routed packets not implemented\n");
			abort();
		}
	}
	while(*data < end) {
		struct SerializeHeader serial = pkt_readSerializeHeader(data);
		const uint8_t *sub = (*data)--;
		*data += serial.length;
		switch(serial.type) {
			case InternalMessageType_SyncTime: fprintf(stderr, "[INSTANCE] UNRELIABLE : InternalMessageType_SyncTime not implemented\n"); abort();
			case InternalMessageType_PlayerConnected: fprintf(stderr, "[INSTANCE] UNRELIABLE : InternalMessageType_PlayerConnected not implemented\n"); abort();
			case InternalMessageType_PlayerIdentity: fprintf(stderr, "[INSTANCE] BAD TYPE: InternalMessageType_PlayerIdentity\n"); break;
			case InternalMessageType_PlayerLatencyUpdate: fprintf(stderr, "[INSTANCE] UNRELIABLE : InternalMessageType_PlayerLatencyUpdate not implemented\n"); abort();
			case InternalMessageType_PlayerDisconnected: fprintf(stderr, "[INSTANCE] UNRELIABLE : InternalMessageType_PlayerDisconnected not implemented\n"); abort();
			case InternalMessageType_PlayerSortOrderUpdate: fprintf(stderr, "[INSTANCE] UNRELIABLE : InternalMessageType_PlayerSortOrderUpdate not implemented\n"); abort();
			case InternalMessageType_Party: fprintf(stderr, "[INSTANCE] UNRELIABLE : InternalMessageType_Party not implemented\n"); abort();
			case InternalMessageType_MultiplayerSession: handle_MultiplayerSession(ctx, room, session, &sub); break;
			case InternalMessageType_KickPlayer: fprintf(stderr, "[INSTANCE] UNRELIABLE : InternalMessageType_KickPlayer not implemented\n"); abort();
			case InternalMessageType_PlayerStateUpdate: fprintf(stderr, "[INSTANCE] BAD TYPE: InternalMessageType_PlayerStateUpdate\n"); break;
			case InternalMessageType_PlayerAvatarUpdate: fprintf(stderr, "[INSTANCE] UNRELIABLE : InternalMessageType_PlayerAvatarUpdate not implemented\n"); abort();
			default: fprintf(stderr, "[INSTANCE] UNRELIABLE : BAD INTERNAL MESSAGE TYPE\n");
		}
		if(sub != *data) {
			fprintf(stderr, "UNRELIABLE : BAD INTERNAL MESSAGE LENGTH (expected %u, read %zu)\n", serial.length, sub - (*data - serial.length));
			if(sub < *data) {
				fprintf(stderr, "\t");
				for(const uint8_t *it = *data - serial.length; it < *data; ++it)
					fprintf(stderr, "%02hhx", *it);
				fprintf(stderr, "\n\t");
				for(const uint8_t *it = *data - serial.length; it < sub; ++it)
					fprintf(stderr, "  ");
				fprintf(stderr, "^ extra data starts here");
			}
			fprintf(stderr, "\n");
		}
	}
}

static void process_Channeled(struct Context *ctx, struct Room *room, struct InstanceSession *session, const uint8_t **data, const uint8_t *end, DeliveryMethod channelId) {
	struct RoutingHeader routing = pkt_readRoutingHeader(data);
	if(routing.connectionId) {
		if(routing.connectionId == 127) {
			uint8_t resp[65536], *resp_end = resp;
			pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){indexof(room->players, session) + 1, 127, 0});
			pkt_writeUint8Array(&resp_end, *data, end - *data);
			FOR_EXCLUDING_PLAYER(room, session, id)
				instance_send_channeled(&room->players[id].channels, resp, resp_end - resp, channelId);
		} else {
			fprintf(stderr, "Routed packets not implemented\n");
			abort();
		}
	}
	while(*data < end) {
		struct SerializeHeader serial = pkt_readSerializeHeader(data);
		const uint8_t *sub = (*data)--;
		*data += serial.length;
		switch(serial.type) {
			case InternalMessageType_SyncTime: fprintf(stderr, "[INSTANCE] InternalMessageType_SyncTime not implemented\n"); break;
			case InternalMessageType_PlayerConnected: fprintf(stderr, "[INSTANCE] InternalMessageType_PlayerConnected not implemented\n"); break;
			case InternalMessageType_PlayerIdentity: handle_PlayerIdentity(ctx, room, session, &sub); break;
			case InternalMessageType_PlayerLatencyUpdate: fprintf(stderr, "[INSTANCE] InternalMessageType_PlayerLatencyUpdate not implemented\n"); break;
			case InternalMessageType_PlayerDisconnected: fprintf(stderr, "[INSTANCE] InternalMessageType_PlayerDisconnected not implemented\n"); break;
			case InternalMessageType_PlayerSortOrderUpdate: fprintf(stderr, "[INSTANCE] InternalMessageType_PlayerSortOrderUpdate not implemented\n"); break;
			case InternalMessageType_Party: fprintf(stderr, "[INSTANCE] InternalMessageType_Party not implemented\n"); break;
			case InternalMessageType_MultiplayerSession: handle_MultiplayerSession(ctx, room, session, &sub); break;
			case InternalMessageType_KickPlayer: fprintf(stderr, "[INSTANCE] InternalMessageType_KickPlayer not implemented\n"); break;
			case InternalMessageType_PlayerStateUpdate: {
				session->stateHash = pkt_readPlayerStateUpdate(&sub).playerState;
				session_refresh_stateHash(ctx, room, session);
				break;
			}
			case InternalMessageType_PlayerAvatarUpdate: fprintf(stderr, "[INSTANCE] InternalMessageType_PlayerAvatarUpdate not implemented\n"); break;
			default: fprintf(stderr, "[INSTANCE] BAD INTERNAL MESSAGE TYPE\n");
		}
		if(sub != *data) {
			fprintf(stderr, "BAD INTERNAL MESSAGE LENGTH (expected %u, read %zu)\n", serial.length, sub - (*data - serial.length));
			if(sub < *data) {
				fprintf(stderr, "\t");
				for(const uint8_t *it = *data - serial.length; it < *data; ++it)
					fprintf(stderr, "%02hhx", *it);
				fprintf(stderr, "\n\t");
				for(const uint8_t *it = *data - serial.length; it < sub; ++it)
					fprintf(stderr, "  ");
				fprintf(stderr, "^ extra data starts here");
			}
			fprintf(stderr, "\n");
		}
	}
}

static void handle_ConnectRequest(struct Context *ctx, struct Room *room, struct InstanceSession *session, const uint8_t **data) {
	struct ConnectRequest req = pkt_readConnectRequest(data);
	if(!(String_eq(req.secret, session->secret) && String_eq(req.userId, session->permissions.userId)))
		return;
	uint8_t resp[65536], *resp_end = resp;
	pkt_writeNetPacketHeader(&resp_end, (struct NetPacketHeader){PacketProperty_ConnectAccept, 0, 0});
	pkt_writeConnectAccept(&resp_end, (struct ConnectAccept){
		.connectId = req.connectId,
		.connectNum = 0,
		.reusedPeer = 0,
	});
	net_send_internal(&ctx->net, &session->net, resp, resp_end - resp, 1);

	if(session->clientState != ClientState_disconnected)
		return;
	session->clientState = ClientState_accepted;

	resp_end = resp;
	pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 127, 0});
	struct SyncTime r_sync;
	r_sync.syncTime = room_get_syncTime(room);
	SERIALIZE_CUSTOM(&resp_end, InternalMessageType_SyncTime)
		pkt_writeSyncTime(&resp_end, r_sync);
	instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);

	resp_end = resp;
	pkt_writeRoutingHeader(&resp_end, (struct RoutingHeader){0, 0, 0});
	struct PlayerIdentity r_identity;
	r_identity.playerState.bloomFilter.d0 = 288266110296588352;
	r_identity.playerState.bloomFilter.d1 = 576531121051926529;
	memset(&r_identity.playerAvatar, 0, sizeof(r_identity.playerAvatar));
	r_identity.random.length = 32;
	memcpy(r_identity.random.data, NetKeypair_get_random(&room->keys), 32);
	r_identity.publicEncryptionKey.length = sizeof(r_identity.publicEncryptionKey.data);
	NetKeypair_write_key(&room->keys, &ctx->net, r_identity.publicEncryptionKey.data, &r_identity.publicEncryptionKey.length);
	SERIALIZE_CUSTOM(&resp_end, InternalMessageType_PlayerIdentity)
		pkt_writePlayerIdentity(&resp_end, r_identity);
	instance_send_channeled(&session->channels, resp, resp_end - resp, DeliveryMethod_ReliableOrdered);
}
static void handle_Disconnect(struct Context *ctx, struct Room *room, struct InstanceSession *session, const uint8_t **data) {
	pkt_readDisconnect(data);
	room_disconnect(NULL, room, session, 1);
}

static void handle_packet(struct Context *ctx, struct Room *room, struct InstanceSession *session, const uint8_t *pkt, uint32_t len) {
	const uint8_t *data = pkt, *end = &pkt[len];
	struct NetPacketHeader header = pkt_readNetPacketHeader(&data);
	if(session->clientState == ClientState_disconnected && header.property != PacketProperty_ConnectRequest)
		return;
	if(header.isFragmented && header.property != PacketProperty_Channeled) {
		fprintf(stderr, "MALFORMED HEADER\n");
		return;
	}
	switch(header.property) {
		case PacketProperty_Unreliable: handle_Unreliable(ctx, room, session, &data, end); break;
		case PacketProperty_Channeled: handle_Channeled((ChanneledHandler)process_Channeled, &ctx->net, &session->net, &session->channels, ctx, room, session, &data, end, header.isFragmented); break;
		case PacketProperty_Ack: handle_Ack(&session->channels, &data); break;
		case PacketProperty_Ping: handle_Ping(&ctx->net, &session->net, &session->pong, &data); break;
		case PacketProperty_Pong: fprintf(stderr, "[INSTANCE] PacketProperty_Pong not implemented\n"); break;
		case PacketProperty_ConnectRequest: handle_ConnectRequest(ctx, room, session, &data); break;
		case PacketProperty_ConnectAccept: fprintf(stderr, "[INSTANCE] BAD PROPERTY: PacketProperty_ConnectAccept\n"); break;
		case PacketProperty_Disconnect: handle_Disconnect(ctx, room, session, &data); break;
		case PacketProperty_UnconnectedMessage: fprintf(stderr, "[INSTANCE] BAD PROPERTY: PacketProperty_UnconnectedMessage\n"); break;
		case PacketProperty_MtuCheck: handle_MtuCheck(&ctx->net, &session->net, &data); break;
		case PacketProperty_MtuOk: fprintf(stderr, "[INSTANCE] PacketProperty_MtuOk not implemented\n"); break;
		case PacketProperty_Broadcast: fprintf(stderr, "[INSTANCE] PacketProperty_Broadcast not implemented\n"); break;
		case PacketProperty_Merged: {
			for(uint16_t sublen; data < end; data += sublen) {
				sublen = pkt_readUint16(&data);
				const uint8_t *subdata = data;
				handle_packet(ctx, room, session, subdata, sublen);
			}
			break;
		}
		case PacketProperty_ShutdownOk: fprintf(stderr, "[INSTANCE] PacketProperty_ShutdownOk not implemented\n"); break;
		case PacketProperty_PeerNotFound: fprintf(stderr, "[INSTANCE] PacketProperty_PeerNotFound not implemented\n"); break;
		case PacketProperty_InvalidProtocol: fprintf(stderr, "[INSTANCE] PacketProperty_InvalidProtocol not implemented\n"); break;
		case PacketProperty_NatMessage: fprintf(stderr, "[INSTANCE] PacketProperty_NatMessage not implemented\n"); break;
		case PacketProperty_Empty: fprintf(stderr, "[INSTANCE] PacketProperty_Empty not implemented\n"); break;
		default: fprintf(stderr, "[INSTANCE] BAD PACKET PROPERTY\n");
	}
	if(data != end)
		fprintf(stderr, "[INSTANCE] BAD PACKET LENGTH (expected %u, read %zu)\n", len, data - pkt);
}

#ifdef WINDOWS
static DWORD WINAPI
#else
static void*
#endif
instance_handler(struct Context *ctx) {
	fprintf(stderr, "[INSTANCE] Started\n");
	uint8_t buf[262144];
	memset(buf, 0, sizeof(buf));
	uint32_t len;
	struct Room *room;
	struct InstanceSession *session;
	const uint8_t *pkt;
	while((len = net_recv(&ctx->net, buf, sizeof(buf), (struct NetSession**)&session, &pkt, (void**)&room))) { // TODO: close instances with zero sessions
		#ifdef PACKET_LOGGING_FUNCS
		{
			const uint8_t *read = pkt;
			struct NetPacketHeader header = pkt_readNetPacketHeader(&read);
			if(!header.isFragmented) {
				fprintf(stderr, "recieve[%s]:\n", reflect(PacketProperty, header.property));
				debug_logPacket(read, &pkt[len], header);
			}
		}
		#endif
		handle_packet(ctx, room, session, pkt, len); // TODO: needs mutex
	}
	return 0;
}

static void room_close(struct Room *room) {
	FOR_ALL_PLAYERS(room, id)
		room_disconnect(NULL, room, &room->players[id], 0);
	free(room);
}

#ifdef WINDOWS
static HANDLE instance_threads[THREAD_COUNT];
#else
static pthread_t instance_threads[THREAD_COUNT];
#endif
static struct Context contexts[THREAD_COUNT];
// static uint32_t instance_count = 1; // ServerCode 0 ("") is not valid
static const char *instance_domain = NULL, *instance_domainIPv4 = NULL;
_Bool instance_init(const char *domain, const char *domainIPv4) {
	instance_domain = domain;
	instance_domainIPv4 = domainIPv4;
	memset(instance_threads, 0, sizeof(instance_threads));
	for(uint32_t i = 0; i < lengthof(instance_threads); ++i) {
		if(net_init(&contexts[i].net, 0)) {
			fprintf(stderr, "net_init() failed\n");
			return 1;
		}
		contexts[i].net.user = &contexts[i];
		contexts[i].net.onResolve = instance_onResolve;
		contexts[i].net.onResend = instance_onResend;
		contexts[i].TEMPglobalRoom = NULL;

		#ifdef WINDOWS
		instance_threads[i] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)instance_handler, &contexts[i], 0, NULL);
		#else
		if(pthread_create(&instance_threads[i], NULL, (void*(*)(void*))&instance_handler, &contexts[i]))
			instance_threads[i] = 0;
		#endif
		if(!instance_threads[i]) {
			fprintf(stderr, "[INSTANCE] Instance thread creation failed\n");
			return 1;
		}
	}
	return index_init();
}

void instance_cleanup() {
	for(uint32_t i = 0; i < lengthof(instance_threads); ++i) {
		if(instance_threads[i]) {
			net_stop(&contexts[i].net);
			fprintf(stderr, "[INSTANCE] Stopping #%u\n", i);
			#ifdef WINDOWS
			WaitForSingleObject(instance_threads[i], INFINITE);
			#else
			pthread_join(instance_threads[i], NULL);
			#endif
			instance_threads[i] = 0;
			if(contexts[i].TEMPglobalRoom)
				room_close(contexts[i].TEMPglobalRoom);
			contexts[i].TEMPglobalRoom = NULL;
			net_cleanup(&contexts[i].net);
		}
	}
	index_cleanup();
}

_Bool instance_get_isopen(ServerCode code, struct String *managerId_out, struct GameplayServerConfiguration *configuration) {
	if(code == StringToServerCode("INDEX", 5))
		return index_get_isopen(managerId_out, configuration);
	if(contexts[0].TEMPglobalRoom && code == StringToServerCode("HELLO", 5)) { // TODO: temporary hack
		*managerId_out = contexts[0].TEMPglobalRoom->managerId;
		configuration->maxPlayerCount = contexts[0].TEMPglobalRoom->configuration.maxPlayerCount;
		return 1;
	}
	return 0;
}

struct NetSession *instance_resolve_session(ServerCode code, struct SS addr, struct String secret, struct String userId, struct String userName) {
	if(code == StringToServerCode("INDEX", 5))
		return index_create_session(addr, secret, userId, userName);
	if(code != StringToServerCode("HELLO", 5))
		return NULL;
	struct Context *ctx = &contexts[0];
	struct Room *room = ctx->TEMPglobalRoom;
	if(!room)
		return NULL;
	struct InstanceSession *session = NULL;
	FOR_ALL_PLAYERS(room, id) {
		if(addrs_are_equal(&addr, NetSession_get_addr(&room->players[id].net))) {
			session = &room->players[id];
			room_disconnect(ctx, room, session, 1);
			break;
		}
	}
	if(!session) {
		struct Counter128 tmp = room->playerSort;
		uint32_t id = 0;
		if((!Counter128_set_next(&tmp, &id, 1)) || id >= room->configuration.maxPlayerCount) {
			fprintf(stderr, "ROOM FULL\n");
			return NULL;
		}
		session = &room->players[id];
		net_session_init(&ctx->net, &session->net, addr);
		room->playerSort = tmp;
	}
	session->clientState = ClientState_disconnected;
	session->secret = secret;
	session->userName = userName;
	session->permissions.userId = userId;
	session->permissions.isServerOwner = String_eq(userId, room->managerId);
	session->permissions.hasRecommendBeatmapsPermission = (room->configuration.songSelectionMode != SongSelectionMode_Random) && (session->permissions.isServerOwner || room->configuration.songSelectionMode != SongSelectionMode_OwnerPicks);
	session->permissions.hasRecommendGameplayModifiersPermission = (room->configuration.gameplayServerControlSettings == GameplayServerControlSettings_AllowModifierSelection || room->configuration.gameplayServerControlSettings == GameplayServerControlSettings_All);
	session->permissions.hasKickVotePermission = session->permissions.isServerOwner;
	session->permissions.hasInvitePermission = (room->configuration.invitePolicy == InvitePolicy_AnyoneCanInvite) || (session->permissions.isServerOwner && room->configuration.invitePolicy == InvitePolicy_OnlyConnectionOwnerCanInvite);

	session->pong.sequence = 0;
	instance_channels_init(&session->channels);
	session->stateHash.bloomFilter = (struct BitMask128){0, 0};
	session->avatar = CLEAR_AVATARDATA;

	session_refresh_state(ctx, room, session);

	char addrstr[INET6_ADDRSTRLEN + 8];
	net_tostr(&addr, addrstr);
	fprintf(stderr, "[INSTANCE] connect %s\n", addrstr);
	fprintf(stderr, "[INSTANCE] player bits: ");
	for(uint32_t i = 0; i < lengthof(room->playerSort.bits); ++i)
		for(uint32_t b = 0; b < sizeof(*room->playerSort.bits) * 8; ++b)
			fprintf(stderr, "%u", (room->playerSort.bits[i] >> b) & 1);
	fprintf(stderr, "\n");
	return &session->net;
}

struct NetSession *instance_open(ServerCode *out, struct String managerId, struct GameplayServerConfiguration *configuration, struct SS addr, struct String secret, struct String userId, struct String userName) {
	struct Room *room = malloc(sizeof(struct Room) + configuration->maxPlayerCount * sizeof(*room->players));
	if(!room) {
		fprintf(stderr, "alloc error\n");
		abort();
	}
	net_keypair_init(&contexts[0].net, &room->keys);
	room->managerId = managerId;
	room->configuration = *configuration;
	room->syncBase = 0;
	room->syncBase = room_get_syncTime(room);
	Counter128_clear(&room->playerSort);
	room_set_state(&contexts[0], room, ServerState_Lobby);
	if(contexts[0].TEMPglobalRoom)
		room_close(contexts[0].TEMPglobalRoom);
	contexts[0].TEMPglobalRoom = room;
	*out = StringToServerCode("HELLO", 5); // TODO: temporary hack
	return instance_resolve_session(*out, addr, secret, userId, userName);
}

struct NetContext *instance_get_net(ServerCode code) {
	if(code == StringToServerCode("INDEX", 5))
		return index_get_net();
	return (code == StringToServerCode("HELLO", 5)) ? &contexts[0].net : NULL;
}

struct IPEndPoint instance_get_address(ServerCode code, _Bool ipv4) {
	struct IPEndPoint out;
	out.address.length = 0;
	out.port = 0;
	if(code != StringToServerCode("INDEX", 5) && code != StringToServerCode("HELLO", 5)) // TODO: temporary hack
		return out;
	if(ipv4)
		out.address.length = sprintf(out.address.data, "%s", instance_domainIPv4);
	else
		out.address.length = sprintf(out.address.data, "%s", instance_domain);
	if(code == StringToServerCode("INDEX", 5)) {
		out.port = index_get_port();
	} else {
		struct SS addr = {sizeof(struct sockaddr_storage)};
		getsockname(net_get_sockfd(&contexts[0].net), &addr.sa, &addr.len);
		switch(addr.ss.ss_family) {
			case AF_INET: out.port = htons(addr.in.sin_port); break;
			case AF_INET6: out.port = htons(addr.in6.sin6_port); break;
			default:;
		}
	}
	return out;
}
