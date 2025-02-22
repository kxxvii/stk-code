//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2013-2015 SuperTuxKart-Team
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "network/protocols/client_lobby.hpp"

#include "audio/sfx_manager.hpp"
#include "config/user_config.hpp"
#include "config/player_manager.hpp"
#include "graphics/camera.hpp"
#include "guiengine/modaldialog.hpp"
#include "guiengine/message_queue.hpp"
#include "guiengine/screen_keyboard.hpp"
#include "input/device_manager.hpp"
#include "input/input_device.hpp"
#include "items/network_item_manager.hpp"
#include "items/powerup_manager.hpp"
#include "karts/abstract_kart.hpp"
#include "karts/controller/controller.hpp"
#include "karts/kart_properties.hpp"
#include "karts/kart_properties_manager.hpp"
#include "modes/linear_world.hpp"
#include "network/crypto.hpp"
#include "network/event.hpp"
#include "network/game_setup.hpp"
#include "network/network_config.hpp"
#include "network/network_player_profile.hpp"
#include "network/network_timer_synchronizer.hpp"
#include "network/peer_vote.hpp"
#include "network/protocols/connect_to_server.hpp"
#include "network/protocols/game_protocol.hpp"
#include "network/protocols/game_events_protocol.hpp"
#include "network/protocol_manager.hpp"
#include "network/race_event_manager.hpp"
#include "network/server.hpp"
#include "network/server_config.hpp"
#include "network/stk_host.hpp"
#include "network/stk_peer.hpp"
#include "online/online_profile.hpp"
#include "online/xml_request.hpp"
#include "states_screens/online/networking_lobby.hpp"
#include "states_screens/online/network_kart_selection.hpp"
#include "states_screens/race_result_gui.hpp"
#include "states_screens/online/tracks_screen.hpp"
#include "tracks/track.hpp"
#include "tracks/track_manager.hpp"
#include "utils/log.hpp"
#include "utils/string_utils.hpp"
#include "utils/translation.hpp"

// ============================================================================
/** The protocol that manages starting a race with the server. It uses a 
 *  finite state machine:
\dot
digraph interaction {
"NONE" -> "LINKED" [label="ENet connection with server established"]
"LINKED" -> "REQUESTING_CONNECTION" [label="Request connection from server"]
"REQUESTING_CONNECTION" -> CONNECTED [label="Connection accepted by server"]
"REQUESTING_CONNECTION" -> "?? TO BE DONE ??" [label="Connection refused by server"]
"CONNECTED" -> "KART_SELECTION" [label="Server tells us to start kart selection"]
"KART_SELECTION" -> "SELECTING_KARTS" [label="Show kart selection screen"]
"SELECTING_KARTS" -> "RACING" [label="Server sends start race message"]
}
\enddot
Note that some states are actually managed outside of the client lobby. For
example to select race details after selecting a kart is managed by the GUI
engine.

*/

ClientLobby::ClientLobby(const TransportAddress& a, std::shared_ptr<Server> s)
           : LobbyProtocol()
{
    m_auto_started = false;
    m_waiting_for_game = false;
    m_server_auto_game_time = false;
    m_received_server_result = false;
    m_state.store(NONE);
    m_server_address = a;
    m_server = s;
    setHandleDisconnections(true);
    m_disconnected_msg[PDI_TIMEOUT] = _("Server connection timed out.");
    m_disconnected_msg[PDI_NORMAL] = _("Server has been shut down.");
    m_disconnected_msg[PDI_KICK] = _("You were kicked from the server.");
    m_disconnected_msg[PDI_KICK_HIGH_PING] =
        _("You were kicked: Ping too high.");
    m_first_connect = true;
    m_spectator = false;
    m_server_live_joinable = false;
    m_server_send_live_load_world = false;
    m_server_enabled_chat = true;
    m_server_enabled_track_voting = true;
    m_server_enabled_report_player = false;
}   // ClientLobby

//-----------------------------------------------------------------------------
ClientLobby::~ClientLobby()
{
    if (m_server->supportsEncryption())
    {
        Online::XMLRequest* request =
            new Online::XMLRequest(true/*manager_memory*/);
        NetworkConfig::get()->setServerDetails(request,
            "clear-user-joined-server");
        request->queue();
        ConnectToServer::m_previous_unjoin = request->observeExistence();
    }
}   // ClientLobby

//-----------------------------------------------------------------------------
void ClientLobby::setup()
{
    m_spectator = false;
    m_server_send_live_load_world = false;
    m_auto_back_to_lobby_time = std::numeric_limits<uint64_t>::max();
    m_start_live_game_time = std::numeric_limits<uint64_t>::max();
    m_received_server_result = false;
    TracksScreen::getInstance()->resetVote();
    LobbyProtocol::setup();
    m_state.store(NONE);
}   // setup

//-----------------------------------------------------------------------------
/** Called from the gui when a client clicked on 'continue' on the race result
 *  screen. It notifies the server that this client has exited the screen and
 *  is back at the lobby.
 */
void ClientLobby::doneWithResults()
{
    NetworkString* done = getNetworkString(1);
    done->setSynchronous(true);
    done->addUInt8(LE_RACE_FINISHED_ACK);
    sendToServer(done, /*reliable*/true);
    delete done;
}   // doneWithResults

//-----------------------------------------------------------------------------
bool ClientLobby::notifyEvent(Event* event)
{
    assert(m_game_setup); // assert that the setup exists
    if (event->getType() != EVENT_TYPE_MESSAGE)
        return true;

    NetworkString &data = event->data();
    assert(data.size()); // assert that data isn't empty
    uint8_t message_type = data.getUInt8();
    Log::info("ClientLobby", "Synchronous message of type %d",
              message_type);
    switch(message_type)
    {
        case LE_START_SELECTION:       startSelection(event);      break;
        case LE_LOAD_WORLD:            addAllPlayers(event);       break;
        case LE_RACE_FINISHED:         raceFinished(event);        break;
        case LE_BACK_LOBBY:            backToLobby(event);         break;
        case LE_UPDATE_PLAYER_LIST:    updatePlayerList(event);    break;
        case LE_CHAT:                  handleChat(event);          break;
        case LE_CONNECTION_ACCEPTED:   connectionAccepted(event);  break;
        case LE_SERVER_INFO:           handleServerInfo(event);    break;
        case LE_PLAYER_DISCONNECTED :  disconnectedPlayer(event);  break;
        case LE_CONNECTION_REFUSED:    connectionRefused(event);   break;
        case LE_VOTE:                  receivePlayerVote(event);   break;
        case LE_SERVER_OWNERSHIP:      becomingServerOwner();      break;
        case LE_BAD_TEAM:              handleBadTeam();            break;
        case LE_BAD_CONNECTION:        handleBadConnection();      break;
        case LE_LIVE_JOIN_ACK:        liveJoinAcknowledged(event); break;
        case LE_KART_INFO:             handleKartInfo(event);      break;
        case LE_START_RACE:            startGame(event);           break;
        case LE_REPORT_PLAYER:         reportSuccess(event);       break;
        default:
            break;
    }   // switch
    return true;
}   // notifyEvent

//-----------------------------------------------------------------------------
bool ClientLobby::notifyEventAsynchronous(Event* event)
{
    assert(m_game_setup); // assert that the setup exists
    if (event->getType() == EVENT_TYPE_MESSAGE)
    {
        NetworkString &data = event->data();
        assert(data.size()); // assert that data isn't empty
        uint8_t message_type = data.getUInt8();

        Log::info("ClientLobby", "Asynchronous message of type %d",
                  message_type);
        switch(message_type)
        {
            default:                                                     break;
        }   // switch
    } // message
    else if (event->getType() == EVENT_TYPE_DISCONNECTED) 
    {
        // This means we left essentially.
        // We can't delete STKHost from this thread, since the main
        // thread might still test if STKHost exists and then call
        // the ProtocolManager, which might already have been deleted.
        // So only signal that STKHost should exit, which will be tested
        // from the main thread.
        STKHost::get()->disconnectAllPeers(false/*timeout_waiting*/);
        STKHost::get()->setErrorMessage(
            m_disconnected_msg.at(event->getPeerDisconnectInfo()));
        STKHost::get()->requestShutdown();
    } // disconnection
    return true;
}   // notifyEventAsynchronous

//-----------------------------------------------------------------------------
void ClientLobby::addAllPlayers(Event* event)
{
    if (World::getWorld())
    {
        Log::warn("ClientLobby", "World already loaded.");
        return;
    }

    // In case the user opened a user info dialog
    GUIEngine::ModalDialog::dismiss();
    GUIEngine::ScreenKeyboard::dismiss();

    if (!checkDataSize(event, 1))
    {
        // If recieved invalid message for players leave now
        STKHost::get()->disconnectAllPeers(false/*timeout_waiting*/);
        STKHost::get()->requestShutdown();
        return;
    }
    // Timeout is too slow to synchronize, force it to stop and set current
    // time
    if (!STKHost::get()->getNetworkTimerSynchronizer()->isSynchronised())
    {
        if (ServerConfig::m_voting_timeout >= 10.0f)
        {
            core::stringw msg = _("Bad network connection is detected.");
            MessageQueue::add(MessageQueue::MT_ERROR, msg);
            Log::warn("ClientLobby", "Failed to synchronize timer before game "
                "start, maybe you enter the game too quick? (at least 5 "
                "seconds are required for synchronization.");
        }
        STKHost::get()->getNetworkTimerSynchronizer()->enableForceSetTimer();
    }

    NetworkString& data = event->data();
    uint32_t winner_peer_id = data.getUInt32();
    PeerVote winner_vote(data);

    m_game_setup->setRace(winner_vote);
    TracksScreen::getInstance()->setResult(winner_peer_id, winner_vote);

    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    peer->cleanPlayerProfiles();
    m_server_send_live_load_world = data.getUInt8() == 1;

    bool is_specator = true;
    std::vector<std::shared_ptr<NetworkPlayerProfile> > players =
        decodePlayers(data, peer, &is_specator);
    setSpectator(is_specator);

    uint32_t random_seed = data.getUInt32();
    ItemManager::updateRandomSeed(random_seed);
    if (race_manager->isBattleMode())
    {
        int hit_capture_limit = data.getUInt32();
        float time_limit = data.getFloat();
        m_game_setup->setHitCaptureTime(hit_capture_limit, time_limit);
        uint16_t flag_return_timeout = data.getUInt16();
        race_manager->setFlagReturnTicks(flag_return_timeout);
        unsigned flag_deactivated_time = data.getUInt16();
        race_manager->setFlagDeactivatedTicks(flag_deactivated_time);
    }
    configRemoteKart(players, isSpectator() ? 1 :
        (int)NetworkConfig::get()->getNetworkPlayers().size());
    loadWorld();
    // Disable until render gui during loading is bug free
    //StateManager::get()->enterGameState();

    if (m_server_send_live_load_world)
    {
        World* w = World::getWorld();
        w->setLiveJoinWorld(true);
        Camera* cam = Camera::getCamera(0);
        for (unsigned i = 0; i < w->getNumKarts(); i++)
        {
            AbstractKart* k = w->getKart(i);
            // Change spectating target to first non-eliminated kart
            if (isSpectator() && cam && !k->isEliminated())
            {
                cam->setKart(k);
                cam = NULL;
            }
            // The final joining ticks will be set by server later
            if (k->getController()->isLocalPlayerController())
                k->setLiveJoinKart(std::numeric_limits<int>::max());
            else
                k->getNode()->setVisible(false);
        }
    }

    // Switch to assign mode in case a player hasn't chosen any karts
    input_manager->getDeviceManager()->setAssignMode(ASSIGN);
}   // addAllPlayers

//-----------------------------------------------------------------------------
/* Get list of players from server and see if we are spectating it. */
std::vector<std::shared_ptr<NetworkPlayerProfile> >
  ClientLobby::decodePlayers(const BareNetworkString& data,
                             std::shared_ptr<STKPeer> peer,
                             bool* is_specator) const
{
    std::vector<std::shared_ptr<NetworkPlayerProfile> > players;
    unsigned player_count = data.getUInt8();
    for (unsigned i = 0; i < player_count; i++)
    {
        core::stringw player_name;
        data.decodeStringW(&player_name);
        uint32_t host_id = data.getUInt32();
        float kart_color = data.getFloat();
        uint32_t online_id = data.getUInt32();
        PerPlayerDifficulty ppd = (PerPlayerDifficulty)data.getUInt8();
        uint8_t local_id = data.getUInt8();
        KartTeam team = (KartTeam)data.getUInt8();
        std::string country_code;
        data.decodeString(&country_code);
        if (is_specator && host_id == STKHost::get()->getMyHostId())
            *is_specator = false;
        auto player = std::make_shared<NetworkPlayerProfile>(peer, player_name,
            host_id, kart_color, online_id, ppd, local_id, team, country_code);
        std::string kart_name;
        data.decodeString(&kart_name);
        player->setKartName(kart_name);
        players.push_back(player);
    }
    return players;
}   // decodePlayers

//-----------------------------------------------------------------------------
void ClientLobby::update(int ticks)
{
    switch (m_state.load())
    {
    case NONE:
        if (STKHost::get()->isConnectedTo(m_server_address))
        {
            m_state.store(LINKED);
        }
        break;
    case LINKED:
    {
        NetworkConfig::get()->clearServerCapabilities();
        std::string ua = StringUtils::getUserAgentString();
        if (NetworkConfig::get()->isNetworkAITester())
            ua = "AI";
        NetworkString* ns = getNetworkString();
        ns->addUInt8(LE_CONNECTION_REQUESTED)
            .addUInt32(ServerConfig::m_server_version).encodeString(ua)
            .addUInt16((uint16_t)stk_config->m_network_capabilities.size());
        for (const std::string& cap : stk_config->m_network_capabilities)
            ns->encodeString(cap);

        auto all_k = kart_properties_manager->getAllAvailableKarts();
        auto all_t = track_manager->getAllTrackIdentifiers();
        if (all_k.size() >= 65536)
            all_k.resize(65535);
        if (all_t.size() >= 65536)
            all_t.resize(65535);
        ns->addUInt16((uint16_t)all_k.size()).addUInt16((uint16_t)all_t.size());
        for (const std::string& kart : all_k)
        {
            ns->encodeString(kart);
        }
        for (const std::string& track : all_t)
        {
            ns->encodeString(track);
        }
        assert(!NetworkConfig::get()->isAddingNetworkPlayers());
        const uint8_t player_count =
            (uint8_t)NetworkConfig::get()->getNetworkPlayers().size();
        ns->addUInt8(player_count);

        bool encryption = false;
        uint32_t id = PlayerManager::getCurrentOnlineId();
        bool lan_ai = !m_server->supportsEncryption() &&
            NetworkConfig::get()->isNetworkAITester();
        if (lan_ai)
            id = 0;
        BareNetworkString* rest = new BareNetworkString();
        if (m_server->supportsEncryption() && id != 0)
        {
            ns->addUInt32(id);
            encryption = true;
        }
        else
        {
            ns->addUInt32(id).addUInt32(0);
            if (id != 0)
            {
                ns->encodeString(
                    PlayerManager::getCurrentOnlineProfile()->getUserName());
            }
        }

        rest->encodeString(ServerConfig::m_private_server_password)
            .addUInt8(player_count);
        for (unsigned i = 0;
             i < NetworkConfig::get()->getNetworkPlayers().size(); i++)
        {
            auto& p = NetworkConfig::get()->getNetworkPlayers()[i];
            PlayerProfile* player = std::get<1>(p);
            core::stringw name = player->getName();
            if (NetworkConfig::get()->isNetworkAITester())
            {
                // I18N: Shown in lobby to indicate it's a bot in LAN game
#ifdef SERVER_ONLY
                name = L"Bot";
#else
                name = _("Bot");
#endif
                if (i > 0)
                {
                    name += core::stringw(" ") + StringUtils::toWString(i);
                }
            }
            rest->encodeString(name).
                addFloat(player->getDefaultKartColor());
            // Per-player handicap
            rest->addUInt8(std::get<2>(p));
        }

        finalizeConnectionRequest(ns, rest, encryption);
        m_state.store(REQUESTING_CONNECTION);
    }
    break;
    case RACE_FINISHED:
        if (!RaceEventManager::getInstance()->protocolStopped() ||
            !GameProtocol::emptyInstance())
            return;
        if (!m_received_server_result)
        {
            m_received_server_result = true;
            m_auto_back_to_lobby_time = StkTime::getMonoTimeMs() + 5000;
            // In case someone opened paused race dialog or menu in network game
            GUIEngine::ModalDialog::dismiss();
            GUIEngine::ScreenKeyboard::dismiss();
            if (StateManager::get()->getGameState() == GUIEngine::INGAME_MENU)
                StateManager::get()->enterGameState();
            World::getWorld()->enterRaceOverState();
        }
        if (NetworkConfig::get()->isAutoConnect() &&
            StkTime::getMonoTimeMs() > m_auto_back_to_lobby_time)
        {
            m_auto_back_to_lobby_time = std::numeric_limits<uint64_t>::max();
            doneWithResults();
        }
        break;
    case DONE:
        m_state.store(EXITING);
        requestTerminate();
        break;
    case REQUESTING_CONNECTION:
    case CONNECTED:
        if (m_start_live_game_time != std::numeric_limits<uint64_t>::max() &&
            STKHost::get()->getNetworkTimer() >= m_start_live_game_time)
        {
            finishLiveJoin();
        }
        if (NetworkConfig::get()->isAutoConnect() && !m_auto_started)
        {
            // Send a message to the server to start
            m_auto_started = true;
            NetworkString start(PROTOCOL_LOBBY_ROOM);
            start.addUInt8(LobbyProtocol::LE_REQUEST_BEGIN);
            STKHost::get()->sendToServer(&start, true);
        }
    case SELECTING_ASSETS:
    case RACING:
    case EXITING:
        break;
    }
}   // update

//-----------------------------------------------------------------------------
void ClientLobby::finalizeConnectionRequest(NetworkString* header,
                                            BareNetworkString* rest,
                                            bool encrypt)
{
    if (encrypt)
    {
        auto crypto = Crypto::getClientCrypto();
        Crypto::resetClientAES();
        BareNetworkString* result = new BareNetworkString();
        if (!crypto->encryptConnectionRequest(*rest))
        {
            // Failed
            result->addUInt32(0);
            *result += BareNetworkString(rest->getData(), rest->getTotalSize());
            encrypt = false;
        }
        else
        {
            Log::info("ClientLobby", "Server will validate this online player.");
            result->addUInt32(rest->getTotalSize());
            *result += BareNetworkString(rest->getData(), rest->getTotalSize());
        }
        delete rest;
        *header += *result;
        delete result;
        sendToServer(header);
        delete header;
        if (encrypt)
        {
            STKHost::get()->getServerPeerForClient()
                ->setCrypto(std::move(crypto));
        }
    }
    else
    {
        *header += *rest;
        delete rest;
        sendToServer(header);
        delete header;
    }
}   // finalizeConnectionRequest

//-----------------------------------------------------------------------------
void ClientLobby::receivePlayerVote(Event* event)
{
    if (!checkDataSize(event, 4)) return;
    // Get the player name who voted
    NetworkString& data = event->data();
    uint32_t host_id = data.getUInt32();
    PeerVote vote(data);
    Log::debug("ClientLobby",
        "Vote from server: host %d, track %s, laps %d, reverse %d.",
        host_id, vote.m_track_name.c_str(), vote.m_num_laps, vote.m_reverse);

    Track* track = track_manager->getTrack(vote.m_track_name);
    if (!track)
    {
        Log::fatal("ClientLobby", "Missing track %s",
            vote.m_track_name.c_str());
    }
    addVote(host_id, vote);
    TracksScreen::getInstance()->addVote(host_id, vote);
    TracksScreen::getInstance()->updatePlayerVotes();
}   // receivePlayerVote

//-----------------------------------------------------------------------------
/*! \brief Called when a new player is disconnected
 *  \param event : Event providing the information.
 *
 *  Format of the data :
 *  Byte 0 
 *       --------------
 *  Size |    1       |
 *  Data | player id *|
 *       --------------
 */
void ClientLobby::disconnectedPlayer(Event* event)
{
    if (!checkDataSize(event, 1)) return;

    NetworkString &data = event->data();
    unsigned disconnected_player_count = data.getUInt8();
    uint32_t host_id = data.getUInt32();
    m_peers_votes.erase(host_id);
    // If in-game world exists the kart rewinder will know which player
    // disconnects
    bool in_game_world = World::getWorld() &&
        RaceEventManager::getInstance() &&
        RaceEventManager::getInstance()->isRunning() &&
        !RaceEventManager::getInstance()->isRaceOver();

    if (!in_game_world)
        SFXManager::get()->quickSound("appear");
    for (unsigned i = 0; i < disconnected_player_count; i++)
    {
        std::string name;
        data.decodeString(&name);
        if (in_game_world)
            continue;
        core::stringw player_name = StringUtils::utf8ToWide(name);
        core::stringw msg = _("%s disconnected.", player_name);
        // Use the friend icon to avoid an error-like message
        MessageQueue::add(MessageQueue::MT_FRIEND, msg);
    }
    TracksScreen::getInstance()->removeVote(host_id);
    TracksScreen::getInstance()->updatePlayerVotes();
}   // disconnectedPlayer

//-----------------------------------------------------------------------------
/*! \brief Called when the server accepts the connection.
 *  \param event : Event providing the information.
 */
void ClientLobby::connectionAccepted(Event* event)
{
    // At least 8 bytes should remain now
    if (!checkDataSize(event, 8)) return;

    NetworkString &data = event->data();
    // Accepted
    // ========
    Log::info("ClientLobby", "The server accepted the connection.");

    static bool shown_msg = false;
    if (!shown_msg)
    {
        shown_msg = true;
        // I18N: Message shown in network lobby to tell user that
        // player name is clickable
        core::stringw msg = _("Press player name in the list for player"
            " management and ranking information.");
        MessageQueue::add(MessageQueue::MT_GENERIC, msg);
    }

    STKHost::get()->setMyHostId(data.getUInt32());
    assert(!NetworkConfig::get()->isAddingNetworkPlayers());
    uint32_t server_version = data.getUInt32();
    NetworkConfig::get()->setJoinedServerVersion(server_version);
    assert(server_version != 0);
    m_auto_started = false;
    m_state.store(CONNECTED);

    unsigned list_caps = data.getUInt16();
    std::set<std::string> caps;
    for (unsigned i = 0; i < list_caps; i++)
    {
        std::string cap;
        data.decodeString(&cap);
        caps.insert(cap);
    }
    NetworkConfig::get()->setServerCapabilities(caps);

    float auto_start_timer = data.getFloat();
    int state_frequency_in_server = data.getUInt32();
    NetworkConfig::get()->setStateFrequency(state_frequency_in_server);
    if (auto_start_timer != std::numeric_limits<float>::max())
        NetworkingLobby::getInstance()->setStartingTimerTo(auto_start_timer);
    m_server_enabled_chat = data.getUInt8() == 1;
    if (NetworkConfig::get()->getServerCapabilities().find("report_player") !=
        NetworkConfig::get()->getServerCapabilities().end())
        m_server_enabled_report_player = data.getUInt8() == 1;
}   // connectionAccepted

//-----------------------------------------------------------------------------
void ClientLobby::handleServerInfo(Event* event)
{
    // At least 6 bytes should remain now
    if (!checkDataSize(event, 6)) return;

    core::stringw str, total_lines;
    if (!m_first_connect)
    {
        total_lines = L"--------------------";
        total_lines += L"\n";
    }
    m_first_connect = false;

    NetworkString &data = event->data();
    // Add server info
    uint8_t u_data;
    data.decodeStringW(&str);

    //I18N: In the networking lobby
    total_lines += _("Server name: %s", str);
    total_lines += L"\n";

    u_data = data.getUInt8();
    const core::stringw& difficulty_name =
        race_manager->getDifficultyName((RaceManager::Difficulty)u_data);
    race_manager->setDifficulty((RaceManager::Difficulty)u_data);
    //I18N: In the networking lobby
    total_lines += _("Difficulty: %s", difficulty_name);
    total_lines += L"\n";

    unsigned max_player = data.getUInt8();
    //I18N: In the networking lobby
    total_lines += _("Max players: %d", (int)max_player);
    total_lines += L"\n";

    // Reserved for extra spectators
    u_data = data.getUInt8();
    u_data = data.getUInt8();
    auto game_mode = ServerConfig::getLocalGameMode(u_data);
    race_manager->setMinorMode(game_mode.first);
    // We use single mode in network even it's grand prix
    race_manager->setMajorMode(RaceManager::MAJOR_MODE_SINGLE);

    //I18N: In the networking lobby
    core::stringw mode_name = ServerConfig::getModeName(u_data);
    total_lines += _("Game mode: %s", mode_name);
    total_lines += L"\n";

    uint8_t extra_server_info = data.getUInt8();
    bool grand_prix_started = false;
    m_game_setup->resetExtraServerInfo();
    switch (extra_server_info)
    {
        case 0:
            break;
        case 1:
        {
            u_data = data.getUInt8();
            core::stringw tl = _("Time limit");
            core::stringw gl = _("Goals limit");
            core::stringw sgt = u_data == 0 ? tl : gl;
            m_game_setup->setSoccerGoalTarget(u_data != 0);
            //I18N: In the networking lobby
            total_lines += _("Soccer game type: %s", sgt);
            total_lines += L"\n";
            break;
        }
        case 2:
        {
            unsigned cur_gp_track = data.getUInt8();
            grand_prix_started = cur_gp_track != 0;
            unsigned total_gp_track = data.getUInt8();
            m_game_setup->setGrandPrixTrack(total_gp_track);
            total_lines += _("Grand prix progress: %d / %d", cur_gp_track,
                total_gp_track);
            total_lines += L"\n";
            break;
        }
    }
    // Auto start info
    unsigned min_players = data.getUInt8();
    float start_timeout = data.getFloat();
    NetworkingLobby::getInstance()->initAutoStartTimer(grand_prix_started,
        min_players, start_timeout, max_player);

    // MOTD
    core::stringw motd;
    data.decodeString16(&motd);
    if (!motd.empty())
        total_lines += motd;

    // Remove last newline added, network lobby will add it back later after
    // removing old server info (with chat)
    if (total_lines[total_lines.size() - 1] == L'\n')
        total_lines.erase(total_lines.size() - 1);

    NetworkingLobby::getInstance()->addMoreServerInfo(total_lines);

    bool server_config = data.getUInt8() == 1;
    NetworkingLobby::getInstance()->toggleServerConfigButton(server_config);
    m_server_live_joinable = data.getUInt8() == 1;
}   // handleServerInfo

//-----------------------------------------------------------------------------
void ClientLobby::updatePlayerList(Event* event)
{
    if (!checkDataSize(event, 1)) return;
    NetworkString& data = event->data();
    bool waiting = data.getUInt8() == 1;
    if (m_waiting_for_game && !waiting)
    {
        // The waiting game finished
        SFXManager::get()->quickSound("wee");
    }

    m_waiting_for_game = waiting;
    unsigned player_count = data.getUInt8();
    core::stringw total_players;
    m_lobby_players.clear();
    bool client_server_owner = false;
    for (unsigned i = 0; i < player_count; i++)
    {
        LobbyPlayer lp = {};
        lp.m_host_id = data.getUInt32();
        lp.m_online_id = data.getUInt32();
        uint8_t local_id = data.getUInt8();
        lp.m_difficulty = PLAYER_DIFFICULTY_NORMAL;
        lp.m_local_player_id = local_id;
        data.decodeStringW(&lp.m_user_name);
        total_players += lp.m_user_name;
        uint8_t boolean_combine = data.getUInt8();
        bool is_peer_waiting_for_game = (boolean_combine & 1) == 1;
        bool is_spectator = ((boolean_combine >> 1) & 1) == 1;
        bool is_peer_server_owner = ((boolean_combine >> 2) & 1) == 1;
        bool ready = ((boolean_combine >> 3) & 1) == 1;
        bool ai = ((boolean_combine >> 4) & 1) == 1;
        // icon to be used, see NetworkingLobby::loadedFromFile
        lp.m_icon_id = is_peer_server_owner ? 0 :
            lp.m_online_id != 0 /*if online account*/ ? 1 : 2;
        if (ai)
            lp.m_icon_id = 6;
        if (waiting && !is_peer_waiting_for_game)
            lp.m_icon_id = 3;
        if (is_spectator)
            lp.m_icon_id = 5;
        if (ready)
            lp.m_icon_id = 4;
        lp.m_difficulty = (PerPlayerDifficulty)data.getUInt8();
        if (lp.m_difficulty == PLAYER_DIFFICULTY_HANDICAP)
        {
            lp.m_user_name = _("%s (handicapped)", lp.m_user_name);
        }
        lp.m_kart_team = (KartTeam)data.getUInt8();
        if (lp.m_host_id == STKHost::get()->getMyHostId())
        {
            if (is_peer_server_owner)
                client_server_owner = true;
            auto& local_players = NetworkConfig::get()->getNetworkPlayers();
            std::get<2>(local_players.at(local_id)) = lp.m_difficulty;
        }
        data.decodeString(&lp.m_country_code);
        m_lobby_players.push_back(lp);
    }
    STKHost::get()->setAuthorisedToControl(client_server_owner);

    // Notification sound for new player
    if (!m_total_players.empty() &&
        total_players.size() > m_total_players.size())
        SFXManager::get()->quickSound("energy_bar_full");
    m_total_players = total_players;

    NetworkingLobby::getInstance()->updatePlayers();
}   // updatePlayerList

//-----------------------------------------------------------------------------
void ClientLobby::handleBadTeam()
{
    SFXManager::get()->quickSound("anvil");
    //I18N: Display when all players are in red or blue team, which the race
    //will not be allowed to start
    core::stringw msg = _("All players joined red or blue team.");
    MessageQueue::add(MessageQueue::MT_ERROR, msg);
}   // handleBadTeam

//-----------------------------------------------------------------------------
void ClientLobby::handleBadConnection()
{
    SFXManager::get()->quickSound("anvil");
    core::stringw msg = _("Bad network connection is detected.");
    MessageQueue::add(MessageQueue::MT_ERROR, msg);
}   // handleBadConnection

//-----------------------------------------------------------------------------
void ClientLobby::becomingServerOwner()
{
    if (STKHost::get()->isClientServer())
        return;

    SFXManager::get()->quickSound("wee");
    //I18N: Display when a player is allow to control the server
    core::stringw msg = _("You are now the owner of server.");
    MessageQueue::add(MessageQueue::MT_GENERIC, msg);
}   // becomingServerOwner

//-----------------------------------------------------------------------------
void ClientLobby::handleChat(Event* event)
{
    if (!UserConfigParams::m_lobby_chat)
        return;
    SFXManager::get()->quickSound("plopp");
    core::stringw message;
    event->data().decodeString16(&message);
    Log::info("ClientLobby", "%s", StringUtils::wideToUtf8(message).c_str());
    if (message.size() > 0)
    {
        if (GUIEngine::getCurrentScreen() == NetworkingLobby::getInstance())
            NetworkingLobby::getInstance()->addMoreServerInfo(message);
        else
            MessageQueue::add(MessageQueue::MT_GENERIC, message);
    }
}   // handleChat

//-----------------------------------------------------------------------------
/*! \brief Called when the server refuses the connection.
 *  \param event : Event providing the information.
 *
 *  Format of the data :
 *  Byte 0 
 *       ----------------
 *  Size |      1       |
 *  Data | refusal code |
 *       ----------------
 */
void ClientLobby::connectionRefused(Event* event)
{
    if (!checkDataSize(event, 1)) return;
    const NetworkString &data = event->data();
    switch ((RejectReason)data.getUInt8()) // the second byte
    {
    case RR_BUSY:
        STKHost::get()->setErrorMessage(
            _("Connection refused: Server is busy."));
        break;
    case RR_BANNED:
    {
        core::stringw msg =
            _("Connection refused: You are banned from the server.");
        core::stringw reason;
        data.decodeStringW(&reason);
        if (!reason.empty())
        {
            msg += L"\n";
            msg += reason;
        }
        STKHost::get()->setErrorMessage(msg);
        break;
    }
    case RR_INCORRECT_PASSWORD:
        STKHost::get()->setErrorMessage(
            _("Connection refused: Server password is incorrect."));
        break;
    case RR_INCOMPATIBLE_DATA:
        STKHost::get()->setErrorMessage(
            _("Connection refused: Game data is incompatible."));
        break;
    case RR_TOO_MANY_PLAYERS:
        STKHost::get()->setErrorMessage(
            _("Connection refused: Server is full."));
        break;
    case RR_INVALID_PLAYER:
        STKHost::get()->setErrorMessage(
            _("Connection refused: Invalid player connecting."));
        break;
    }
    STKHost::get()->disconnectAllPeers(false/*timeout_waiting*/);
    STKHost::get()->requestShutdown();
}   // connectionRefused

//-----------------------------------------------------------------------------

/*! \brief Called when the server broadcasts to start the race to all clients.
 *  \param event : Event providing the time the client should start game.
 */
void ClientLobby::startGame(Event* event)
{
    World::getWorld()->setPhase(WorldStatus::SERVER_READY_PHASE);
    uint64_t start_time = event->data().getUInt64();
    powerup_manager->setRandomSeed(start_time);

    unsigned check_structure_count = event->data().getUInt8();
    LinearWorld* lw = dynamic_cast<LinearWorld*>(World::getWorld());
    if (lw)
        lw->handleServerCheckStructureCount(check_structure_count);

    NetworkItemManager* nim =
    dynamic_cast<NetworkItemManager*>(ItemManager::get());
    assert(nim);
    nim->restoreCompleteState(event->data());

    core::stringw err_msg = _("Failed to start the network game.");
    joinStartGameThread();
    m_start_game_thread = std::thread([start_time, this, err_msg]()
        {
            const uint64_t cur_time = STKHost::get()->getNetworkTimer();
            if (!(start_time > cur_time))
            {
                Log::error("ClientLobby", "Network timer is too slow to catch "
                    "up, you must have a poor network.");
                STKHost::get()->setErrorMessage(err_msg);
                STKHost::get()->requestShutdown();
                return;
            }
            int sleep_time = (int)(start_time - cur_time);
            Log::info("ClientLobby", "Start game after %dms", sleep_time);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
            Log::info("ClientLobby", "Started at %lf", StkTime::getRealTime());
            m_state.store(RACING);
        });
}   // startGame

//-----------------------------------------------------------------------------
/*! \brief Called when the kart selection starts.
 *  \param event : Event providing the information (no additional information
 *                 in this case).
 */
void ClientLobby::startSelection(Event* event)
{
    SFXManager::get()->quickSound("wee");
    const NetworkString& data = event->data();
    startVotingPeriod(data.getFloat());
    bool skip_kart_screen = data.getUInt8() == 1;
    m_server_auto_game_time = data.getUInt8() == 1;
    m_server_enabled_track_voting = data.getUInt8() == 1;
    const unsigned kart_num = data.getUInt16();
    const unsigned track_num = data.getUInt16();
    m_available_karts.clear();
    m_available_tracks.clear();
    for (unsigned i = 0; i < kart_num; i++)
    {
        std::string kart;
        data.decodeString(&kart);
        m_available_karts.insert(kart);
    }
    for (unsigned i = 0; i < track_num; i++)
    {
        std::string track;
        data.decodeString(&track);
        m_available_tracks.insert(track);
    }

    // In case the user opened a user info dialog
    GUIEngine::ModalDialog::dismiss();
    GUIEngine::ScreenKeyboard::dismiss();
    NetworkKartSelectionScreen* screen =
        NetworkKartSelectionScreen::getInstance();
    screen->setAvailableKartsFromServer(m_available_karts);
    screen->setLiveJoin(false);
    // In case of auto-connect or continue a grand prix, use random karts
    // (or previous kart) from server and go to track selection
    if ((NetworkConfig::get()->isAutoConnect() || skip_kart_screen) &&
        m_server_enabled_track_voting)
    {
        input_manager->setMasterPlayerOnly(true);
        for (auto& p : NetworkConfig::get()->getNetworkPlayers())
        {
            StateManager::get()
                ->createActivePlayer(std::get<1>(p), std::get<0>(p));
        }
        input_manager->getDeviceManager()->setAssignMode(ASSIGN);
        TracksScreen::getInstance()->setQuitServer();
        TracksScreen::getInstance()->setNetworkTracks();
        TracksScreen::getInstance()->push();
    }
    else
    {
        screen->push();
    }

    TracksScreen *ts = TracksScreen::getInstance();
    ts->resetVote();
    m_state.store(SELECTING_ASSETS);
    Log::info("ClientLobby", "Selection starts now");
}   // startSelection

//-----------------------------------------------------------------------------

/*! \brief Called when all karts have finished the race.
 *  \param event : Event providing the information.
 *
 *  Format of the data :
 *  Byte 0           1
 *       -------------------------------
 *  Size |     1     |     1     |     |
 *  Data | Kart 1 ID | kart id 2 | ... |
 *       -------------------------------
 */
void ClientLobby::raceFinished(Event* event)
{
    NetworkString &data = event->data();
    Log::info("ClientLobby", "Server notified that the race is finished.");
    LinearWorld* lw = dynamic_cast<LinearWorld*>(World::getWorld());
    if (m_game_setup->isGrandPrix())
    {
        int t = data.getUInt32();
        core::stringw kart_name;
        data.decodeStringW(&kart_name);
        lw->setFastestLapTicks(t);
        lw->setFastestKartName(kart_name);
        race_manager->configGrandPrixResultFromNetwork(data);
    }
    else if (race_manager->modeHasLaps())
    {
        int t = data.getUInt32();
        core::stringw kart_name;
        data.decodeStringW(&kart_name);
        lw->setFastestLapTicks(t);
        lw->setFastestKartName(kart_name);
    }

    if (lw)
    {
        // Eliminate all karts which have not finished the race, it can happen
        // if the last player leave the game instead of crossing the finish
        // line
        lw->updateRacePosition();
        for (unsigned i = 0; i < lw->getNumKarts(); i++)
        {
            AbstractKart* k = lw->getKart(i);
            if (!k->hasFinishedRace() && !k->isEliminated())
            {
                core::stringw player_name = k->getController()->getName();
                core::stringw msg = _("%s left the game.", player_name);
                MessageQueue::add(MessageQueue::MT_FRIEND, msg);
                World::getWorld()->eliminateKart(i,
                    false/*notify_of_elimination*/);
                k->finishedRace(World::getWorld()->getTime(),
                    true/*from_server*/);
            }
        }
    }

    // stop race protocols
    RaceEventManager::getInstance()->stop();
    ProtocolManager::lock()->findAndTerminate(PROTOCOL_GAME_EVENTS);
    ProtocolManager::lock()->findAndTerminate(PROTOCOL_CONTROLLER_EVENTS);
    m_state.store(RACE_FINISHED);
}   // raceFinished

//-----------------------------------------------------------------------------
/** Called when the server informs the clients to exit the race result screen.
 *  It exits the race, and goes back to the lobby.
 */
void ClientLobby::backToLobby(Event *event)
{
    // In case the user opened a user info dialog
    GUIEngine::ModalDialog::dismiss();
    GUIEngine::ScreenKeyboard::dismiss();

    NetworkConfig::get()->clearActivePlayersForClient();
    setup();
    m_auto_started = false;
    m_state.store(CONNECTED);

    if (RaceEventManager::getInstance())
    {
        RaceEventManager::getInstance()->stop();
        ProtocolManager::lock()->findAndTerminate(PROTOCOL_GAME_EVENTS);
    }
    auto gp = GameProtocol::lock();
    if (gp)
    {
        auto lock = gp->acquireWorldDeletingMutex();
        ProtocolManager::lock()->findAndTerminate(PROTOCOL_CONTROLLER_EVENTS);
        RaceResultGUI::getInstance()->backToLobby();
    }
    else
        RaceResultGUI::getInstance()->backToLobby();

    NetworkString &data = event->data();
    core::stringw msg;
    switch ((BackLobbyReason)data.getUInt8()) // the second byte
    {
    case BLR_NO_GAME_FOR_LIVE_JOIN:
        // I18N: Error message shown if live join or spectate failed in network
        msg = _("The game has ended, you can't live join or spectate anymore.");
        break;
    case BLR_NO_PLACE_FOR_LIVE_JOIN:
        // I18N: Error message shown if live join failed in network
        msg = _("No remaining place in the arena - live join disabled.");
        break;
    case BLR_ONE_PLAYER_IN_RANKED_MATCH:
        // I18N: Error message shown if only 1 player remains in network
        msg = _("Only 1 player remaining, returning to lobby.");
        break;
    default:
        break;
    }
    if (!msg.empty())
    {
        SFXManager::get()->quickSound("anvil");
        MessageQueue::add(MessageQueue::MT_ERROR, msg);
    }
}   // backToLobby

//-----------------------------------------------------------------------------
/** Callback when the world is loaded. The client will inform the server
 *  that the players on this host are ready to start the race. It is called by
 *  the RaceManager after the world is loaded.
 */
void ClientLobby::finishedLoadingWorld()
{
    NetworkString* ns = getNetworkString(1);
    ns->setSynchronous(m_server_send_live_load_world);
    ns->addUInt8(LE_CLIENT_LOADED_WORLD);
    sendToServer(ns, true);
    delete ns;
}   // finishedLoadingWorld

//-----------------------------------------------------------------------------
void ClientLobby::liveJoinAcknowledged(Event* event)
{
    World* w = World::getWorld();
    if (!w)
        return;

    const NetworkString& data = event->data();
    m_start_live_game_time = data.getUInt64();
    powerup_manager->setRandomSeed(m_start_live_game_time);

    unsigned check_structure_count = event->data().getUInt8();
    LinearWorld* lw = dynamic_cast<LinearWorld*>(World::getWorld());
    if (lw)
        lw->handleServerCheckStructureCount(check_structure_count);

    m_start_live_game_time = data.getUInt64();
    m_last_live_join_util_ticks = data.getUInt32();
    for (unsigned i = 0; i < w->getNumKarts(); i++)
    {
        AbstractKart* k = w->getKart(i);
        if (k->getController()->isLocalPlayerController())
            k->setLiveJoinKart(m_last_live_join_util_ticks);
    }

    NetworkItemManager* nim =
    dynamic_cast<NetworkItemManager*>(ItemManager::get());
    assert(nim);
    nim->restoreCompleteState(data);
    w->restoreCompleteState(data);

    if (race_manager->supportsLiveJoining() && data.size() > 0)
    {
        // Get and update the players list 1 more time in case the was
        // player connection or disconnection
        std::vector<std::shared_ptr<NetworkPlayerProfile> > players =
            decodePlayers(data);
        w->resetElimination();
        for (unsigned i = 0; i < players.size(); i++)
        {
            AbstractKart* k = w->getKart(i);
            if (k->getController()->isLocalPlayerController())
                continue;
            k->reset();
            // Only need to change non local player karts
            RemoteKartInfo& rki = race_manager->getKartInfo(i);
            rki.copyFrom(players[i], players[i]->getLocalPlayerId());
            if (rki.isReserved())
            {
                World::getWorld()->eliminateKart(i,
                    false/*notify_of_elimination*/);
                k->setPosition(
                    World::getWorld()->getCurrentNumKarts() + 1);
                k->finishedRace(World::getWorld()->getTime(),
                    true/*from_server*/);
            }
            else
            {
                // Will be reset once again after live join is finished
                addLiveJoiningKart(i, rki, 0/*live_join_util_ticks*/);
                k->getNode()->setVisible(false);
            }
        }
    }
}   // liveJoinAcknowledged

//-----------------------------------------------------------------------------
void ClientLobby::finishLiveJoin()
{
    m_start_live_game_time = std::numeric_limits<uint64_t>::max();
    World* w = World::getWorld();
    if (!w)
        return;
    Log::info("ClientLobby", "Live join started at %lf",
        StkTime::getRealTime());

    w->setLiveJoinWorld(false);
    w->endLiveJoinWorld(m_last_live_join_util_ticks);
    for (unsigned i = 0; i < w->getNumKarts(); i++)
    {
        AbstractKart* k = w->getKart(i);
        if (!k->getController()->isLocalPlayerController() &&
            !k->isEliminated())
            k->getNode()->setVisible(true);
    }
    m_state.store(RACING);
}   // finishLiveJoin

//-----------------------------------------------------------------------------
void ClientLobby::requestKartInfo(uint8_t kart_id)
{
    NetworkString* ns = getNetworkString(1);
    ns->setSynchronous(true);
    ns->addUInt8(LE_KART_INFO).addUInt8(kart_id);
    sendToServer(ns, true/*reliable*/);
    delete ns;
}   // requestKartInfo

//-----------------------------------------------------------------------------
void ClientLobby::handleKartInfo(Event* event)
{
    World* w = World::getWorld();
    if (!w)
        return;

    const NetworkString& data = event->data();
    int live_join_util_ticks = data.getUInt32();
    uint8_t kart_id = data.getUInt8();
    core::stringw player_name;
    data.decodeStringW(&player_name);
    uint32_t host_id = data.getUInt32();
    float kart_color = data.getFloat();
    uint32_t online_id = data.getUInt32();
    PerPlayerDifficulty ppd = (PerPlayerDifficulty)data.getUInt8();
    uint8_t local_id = data.getUInt8();
    std::string kart_name;
    data.decodeString(&kart_name);
    std::string country_code;
    data.decodeString(&country_code);

    RemoteKartInfo& rki = race_manager->getKartInfo(kart_id);
    rki.setPlayerName(player_name);
    rki.setHostId(host_id);
    rki.setDefaultKartColor(kart_color);
    rki.setOnlineId(online_id);
    rki.setPerPlayerDifficulty(ppd);
    rki.setLocalPlayerId(local_id);
    rki.setKartName(kart_name);
    rki.setCountryCode(country_code);
    addLiveJoiningKart(kart_id, rki, live_join_util_ticks);

    core::stringw msg;
    if (race_manager->teamEnabled())
    {
        if (w->getKartTeam(kart_id) == KART_TEAM_RED)
        {
            // I18N: Show when player join red team of the started game in
            // network
            msg = _("%s joined the red team.", player_name);
        }
        else
        {
            // I18N: Show when player join blue team of the started game in
            // network
            msg = _("%s joined the blue team.", player_name);
        }
    }
    else
    {
        // I18N: Show when player join the started game in network
        msg = _("%s joined the game.", player_name);
    }
    SFXManager::get()->quickSound("energy_bar_full");
    MessageQueue::add(MessageQueue::MT_FRIEND, msg);
}   // handleKartInfo

//-----------------------------------------------------------------------------
void ClientLobby::startLiveJoinKartSelection()
{
    NetworkKartSelectionScreen::getInstance()->setLiveJoin(true);
    std::vector<int> all_k =
        kart_properties_manager->getKartsInGroup("standard");
    std::set<std::string> karts;
    for (int kart : all_k)
    {
        const KartProperties* kp = kart_properties_manager->getKartById(kart);
        if (!kp->isAddon())
            karts.insert(kp->getIdent());
    }
    NetworkKartSelectionScreen::getInstance()
        ->setAvailableKartsFromServer(karts);
    NetworkKartSelectionScreen::getInstance()->push();
}   // startLiveJoinKartSelection

// ----------------------------------------------------------------------------
void ClientLobby::sendChat(irr::core::stringw text)
{
    text = text.trim().removeChars(L"\n\r");
    if (text.size() > 0)
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LobbyProtocol::LE_CHAT);

        core::stringw name;
        PlayerProfile* player = PlayerManager::getCurrentPlayer();
        if (PlayerManager::getCurrentOnlineState() ==
            PlayerProfile::OS_SIGNED_IN)
            name = PlayerManager::getCurrentOnlineProfile()->getUserName();
        else
            name = player->getName();
        chat->encodeString16(name + L": " + text, 1000/*max_len*/);

        STKHost::get()->sendToServer(chat, true);
        delete chat;
    }
}   // sendChat

// ----------------------------------------------------------------------------
void ClientLobby::changeSpectateTarget(PlayerAction action, int value,
                                       Input::InputType type) const
{
    Camera* cam = Camera::getActiveCamera();
    if (!cam)
        return;

    // Only 1 local player will be able to change target, and this will replace
    // the end camera with normal
    if (cam->getType() != Camera::CM_TYPE_NORMAL)
        Camera::changeCamera(0, Camera::CM_TYPE_NORMAL);

    // Update if the camera again beacuse when race finished cam will be
    // changed above and invalid
    cam = Camera::getActiveCamera();
    if (!cam)
        return;

    // Copied from EventHandler::processGUIAction
    const bool pressed_down = value > Input::MAX_VALUE * 2 / 3;

    if (!pressed_down && type == Input::IT_STICKMOTION)
        return;

    if (action == PA_LOOK_BACK)
    {
        if (cam->getMode() == Camera::CM_REVERSE)
            cam->setMode(Camera::CM_NORMAL);
        else
            cam->setMode(Camera::CM_REVERSE);
        return;
    }

    World::KartList karts = World::getWorld()->getKarts();
    bool sort_kart_for_position =
        race_manager->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL ||
        race_manager->getMinorMode() == RaceManager::MINOR_MODE_CAPTURE_THE_FLAG ||
        race_manager->modeHasLaps();
    if (sort_kart_for_position)
    {
        std::sort(karts.begin(), karts.end(), []
            (const std::shared_ptr<AbstractKart>& a,
            const std::shared_ptr<AbstractKart>& b)->bool
        {
            return a->getPosition() < b->getPosition();
        });
    }

    const int num_karts = (int)karts.size();
    int current_idx = -1;
    if (cam->getKart())
    {
        if (sort_kart_for_position)
            current_idx = cam->getKart()->getPosition() - 1;
        else
            current_idx = cam->getKart()->getWorldKartId();
    }
    if (current_idx < 0 || current_idx >= num_karts)
        return;

    bool up = false;
    if (action == PA_STEER_LEFT)
        up = false;
    else if (action == PA_STEER_RIGHT)
        up = true;
    else
        return;
    for (int i = 0; i < num_karts; i++)
    {
        current_idx = up ? current_idx + 1 : current_idx - 1;
        // Handle looping
        if (current_idx == -1)
            current_idx = num_karts - 1;
        else if (current_idx == num_karts)
            current_idx = 0;

        if (!karts[current_idx]->isEliminated())
        {
            cam->setKart(karts[current_idx].get());
            break;
        }
    }
}   // changeSpectateTarget

// ----------------------------------------------------------------------------
void ClientLobby::addSpectateHelperMessage() const
{
    auto& local_players = NetworkConfig::get()->getNetworkPlayers();
    if (local_players.empty())
        return;
    InputDevice* id = std::get<0>(local_players[0]);
    if (!id)
        return;
    DeviceConfig* dc = id->getConfiguration();
    if (!dc)
        return;
    core::stringw left = dc->getBindingAsString(PA_STEER_LEFT);
    core::stringw right = dc->getBindingAsString(PA_STEER_RIGHT);
    core::stringw back = dc->getBindingAsString(PA_LOOK_BACK);

    // I18N: Message shown in game to tell the player it's possible to change
    // the camera target in spectate mode of network
    core::stringw msg = _("Press <%s> or <%s> to change the targeted player "
        "or <%s> for the camera position.", left, right, back);
    MessageQueue::add(MessageQueue::MT_GENERIC, msg);
}   // addSpectateHelperMessage

// ----------------------------------------------------------------------------
void ClientLobby::reportSuccess(Event* event)
{
    bool succeeded = false;
    core::stringw reporting_name;
    succeeded = event->data().getUInt8() == 1;
    if (succeeded)
        event->data().decodeStringW(&reporting_name);
    if (succeeded && !reporting_name.empty())
    {
        // I18N: Tell player he has successfully report this named player
        core::stringw msg = _("Successfully reported %s.", reporting_name);
        MessageQueue::add(MessageQueue::MT_GENERIC, msg);
    }
}   // reportSuccess
