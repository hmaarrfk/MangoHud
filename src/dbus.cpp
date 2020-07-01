#include <cstdio>
#include <iostream>
#include <sstream>
#include <array>
#include "dbus_info.h"
#include "string_utils.h"
#include "dbus_helpers.hpp"

using ms = std::chrono::milliseconds;
using namespace DBus_helpers;
#define DBUS_TIMEOUT 2000 // ms

struct mutexed_metadata main_metadata;

namespace dbusmgr { 
dbus_manager dbus_mgr;
}

template<class T>
static void assign_metadata_value(metadata& meta, const std::string& key, const T& value) {
    std::cerr << "Assigning Metadata: " << key << " -> " << value << "\n";
    if(key == "PlaybackStatus") {
        meta.playing = (value == "Playing");
        meta.got_playback_data = true;
    }
    else if(key == "xesam:title"){
        meta.title = value;
        meta.got_song_data = true;
        meta.valid = true;
    }
    else if(key == "xesam:artist") {
        meta.artists = value;
        meta.got_song_data = true;
        meta.valid = true;
    }
    else if(key == "xesam:album") {
        meta.album = value;
        meta.got_song_data = true;
        meta.valid = true;
    }
    else if(key == "mpris:artUrl"){
        meta.artUrl = value;
        meta.got_song_data = true;
    }
}

std::string format_signal(const dbusmgr::DBusSignal& s)
{
    std::stringstream ss;
    ss << "type='signal',interface='" << s.intf << "'";
    ss << ",member='" << s.signal << "'";
    return ss.str();
}

static void parse_mpris_properties(libdbus_loader& dbus, DBusMessage *msg, std::string& source, metadata& meta)
{
    /**
     *  Expected response Format:
     *      string,
     *      map{
     *          "Metadata" -> multimap,
     *          "PlaybackStatus" -> string
     *      }
    */

    std::string key, val;
    auto iter = DBusMessageIter_wrap(msg, &dbus);

    // Should be 'org.mpris.MediaPlayer2.Player'
    if (not iter.is_string()){
        std::cerr << "Not a string\n";  //TODO
        return;
    }

    source = iter.get_primitive<std::string>();

    if (source != "org.mpris.MediaPlayer2.Player")
        return;

    iter.next();
    if (not iter.is_array())
        return;

    //std::cerr << "Parsing mpris update...\n";
    string_map_for_each(iter, [&](std::string& key, DBusMessageIter_wrap it){
        if(key == "Metadata"){
            //std::cerr << "\tMetadata:\n";
            string_map_for_each(it, [&](const std::string& key, DBusMessageIter_wrap it){
                if(it.is_primitive()){
                    auto val = it.get_stringified();
                    //std::cerr << "\t\t" << key << " -> " << val << "\n";
                    assign_metadata_value(meta, key, val);
                }
                else if(it.is_array()){
                    std::string val;
                    array_for_each_stringify(it, [&](const std::string& str){
                        if(val.empty()){
                            val = str;
                        }
                        else {
                            val += ", " + str;
                        }
                    });
                    //std::cerr << "\t\t" << key << " -> " << val << "\n";
                    assign_metadata_value(meta, key, val);
                }
            });
            string_multimap_for_each_stringify(it, [&](const std::string& key, const std::string& val){
                assign_metadata_value(meta, key, val);
            });
        }
        else if(key == "PlaybackStatus"){
            auto val = it.get_stringified();
            assign_metadata_value(meta, key, val);
        }
    });
    meta.valid = (meta.artists.size() || !meta.title.empty());
}

bool dbus_get_name_owner(dbusmgr::dbus_manager& dbus_mgr, std::string& name_owner, const char *name)
{
    auto reply = DBusMessage_wrap::new_method_call(
        "org.freedesktop.DBus", 
        "/org/freedesktop/DBus", 
        "org.freedesktop.DBus", 
        "GetNameOwner",
        &dbus_mgr.dbus()
    ).argument(name).send_with_reply_and_block(dbus_mgr.get_conn(), DBUS_TIMEOUT);
    if(not reply) return false;

    auto iter = reply.iter();
    if(not iter.is_string()) return false;
    name_owner = iter.get_primitive<std::string>();
    return true;
}

bool dbus_get_player_property(dbusmgr::dbus_manager& dbus_mgr, metadata& meta, const char * dest, const char * prop)
{
    auto reply = DBusMessage_wrap::new_method_call(
        dest,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "Get",
        &dbus_mgr.dbus()
    ).argument("org.mpris.MediaPlayer2.Player")
    .argument(prop)
    .send_with_reply_and_block(dbus_mgr.get_conn(), DBUS_TIMEOUT);

    if(not reply) return false;

    auto iter = reply.iter();
    if(iter.is_array()){
        string_multimap_for_each_stringify(iter, [&](const std::string& key, const std::string& val){
            assign_metadata_value(meta, key, val);
        });
    }
    else if(iter.is_primitive()){
        assign_metadata_value(meta, prop, iter.get_stringified());
    }
    else {
        return false;
    }
    return true;
}

namespace dbusmgr {
bool dbus_manager::get_media_player_metadata(metadata& meta, std::string name) {
    if(name == "") name = m_active_player;
    if(name == "") return false;
    meta.clear();
    dbus_get_player_property(*this, meta, name.c_str(), "Metadata");
    dbus_get_player_property(*this, meta, name.c_str(), "PlaybackStatus");
    meta.valid = (meta.artists.size() || !meta.title.empty());
    return true;
}

bool dbus_manager::init(const std::string& requested_player)
{
    if (m_inited)
        return true;
    
    m_requested_player = "org.mpris.MediaPlayer2." + requested_player;

    if (!m_dbus_ldr.IsLoaded() && !m_dbus_ldr.Load("libdbus-1.so.3")) {
        std::cerr << "MANGOHUD: Could not load libdbus-1.so.3\n";
        return false;
    }

    m_dbus_ldr.error_init(&m_error);

    m_dbus_ldr.threads_init_default();

    if ( nullptr == (m_dbus_conn = m_dbus_ldr.bus_get(DBUS_BUS_SESSION, &m_error)) ) {
        std::cerr << "MANGOHUD: " << m_error.message << std::endl;
        m_dbus_ldr.error_free(&m_error);
        return false;
    }

    std::cout << "MANGOHUD: Connected to D-Bus as \"" << m_dbus_ldr.bus_get_unique_name(m_dbus_conn) << "\"." << std::endl;

    dbus_list_name_to_owner();
    connect_to_signals();

    select_active_player();
    {
        std::lock_guard<std::mutex> lck(main_metadata.mtx);
        get_media_player_metadata(main_metadata.meta);
    }

    m_inited = true;
    return true;
}

bool dbus_manager::select_active_player(metadata* store_meta) {
    // If the requested player is available, use it
    if(m_name_owners.count(m_requested_player) > 0) {
        m_active_player = m_requested_player;
        std::cerr << "Selecting requested player: " << m_requested_player << "\n";
        if(store_meta) get_media_player_metadata(*store_meta, m_active_player);
        return true;
    }

    // Else, use any player that is currently playing..
    for(const auto& entry : m_name_owners) {
        const auto& name = std::get<0>(entry);
        metadata meta;
        get_media_player_metadata(meta, name);
        if(meta.playing) {
            m_active_player = name;
            std::cerr << "Selecting fallback player: " << name << "\n";
            if(store_meta) *store_meta = meta;
            return true;
        }
    }

    // No media players are active
    std::cerr << "No active players\n";
    m_active_player = "";
    if(store_meta) store_meta->clear();
    return false;
}

void dbus_manager::deinit()
{
    if (!m_inited)
        return;

    // unreference system bus connection instead of closing it
    if (m_dbus_conn) {
        disconnect_from_signals();
        m_dbus_ldr.connection_unref(m_dbus_conn);
        m_dbus_conn = nullptr;
    }
    m_dbus_ldr.error_free(&m_error);
    m_inited = false;
}

dbus_manager::~dbus_manager()
{
    deinit();
}

DBusHandlerResult dbus_manager::filter_signals(DBusConnection* conn, DBusMessage* msg, void* userData) {
    auto& manager = *reinterpret_cast<dbus_manager*>(userData);

    for(auto& sig : manager.m_signals) {
        if(manager.m_dbus_ldr.message_is_signal(msg, sig.intf, sig.signal)){
            auto sender = manager.m_dbus_ldr.message_get_sender(msg);
            if((manager.*(sig.handler))(msg, sender)) 
                return DBUS_HANDLER_RESULT_HANDLED;
            else
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool dbus_manager::handle_properties_changed(DBusMessage* msg, const char* sender) {
    std::string source;

    metadata meta;
    parse_mpris_properties(m_dbus_ldr, msg, source, meta);
#ifndef NDEBUG
    std::cerr << "PropertiesChanged Signal received:\n";
    std::cerr << "\tSource: " << source << "\n";
    std::cerr << "active_player:         " << m_active_player << "\n";
    std::cerr << "active_player's owner: " << m_name_owners[m_active_player] << "\n";
    std::cerr << "sender:                " << sender << "\n";
#endif
    if (source != "org.mpris.MediaPlayer2.Player")
        return false;

    if(m_active_player == "") {
        select_active_player(&meta);
    }
    if (m_name_owners[m_active_player] == sender) {
        std::lock_guard<std::mutex> lck(main_metadata.mtx);
        if(meta.got_song_data){
            // If the song has changed, reset the ticker
            if(
                main_metadata.meta.artists != meta.artists ||
                main_metadata.meta.album != meta.album ||
                main_metadata.meta.title != meta.title
            ){
                main_metadata.ticker = {};
            }

            main_metadata.meta = meta;
            main_metadata.meta.playing = true;
        }
        if(meta.got_playback_data){
            main_metadata.meta.playing = meta.playing;
        }
    }
    std::cerr << "Main metadata valid: " << std::boolalpha << main_metadata.meta.valid << "\n";
    return true;
}

bool dbus_manager::handle_name_owner_changed(DBusMessage* msg, const char* sender) {
    DBusMessageIter iter;
    m_dbus_ldr.message_iter_init (msg, &iter);
    std::vector<std::string> str;
    const char *value = nullptr;

    while (m_dbus_ldr.message_iter_get_arg_type (&iter) == DBUS_TYPE_STRING) {
        m_dbus_ldr.message_iter_get_basic (&iter, &value);
        str.push_back(value);
        m_dbus_ldr.message_iter_next (&iter);
    }

    // register new name
    if (str.size() == 3
        && starts_with(str[0], "org.mpris.MediaPlayer2.")
        && !str[2].empty()
    )
    {
        m_name_owners[str[0]] = str[2];
        if(str[0] == m_requested_player){
            metadata tmp;
            select_active_player(&tmp);
            {
                std::lock_guard<std::mutex> lck(main_metadata.mtx);
                main_metadata.meta = tmp;
                main_metadata.ticker = {};
            }
        }
    }

    // did a player quit?
    if (str[2].empty()) {
        if (str.size() == 3
            && str[0] == m_active_player
        ) {
            metadata tmp;
            m_name_owners.erase(str[0]);
            select_active_player();
            get_media_player_metadata(tmp);
            {
                std::lock_guard<std::mutex> lck(main_metadata.mtx);
                std::swap(tmp, main_metadata.meta);
            }
        }
    }
    return true;
}

void dbus_manager::connect_to_signals()
{
    for (auto kv : m_signals) {
        auto signal = format_signal(kv);
        m_dbus_ldr.bus_add_match(m_dbus_conn, signal.c_str(), &m_error);
        if (m_dbus_ldr.error_is_set(&m_error)) {
            ::perror(m_error.name);
            ::perror(m_error.message);
            m_dbus_ldr.error_free(&m_error);
            //return;
        }
    }
    m_dbus_ldr.connection_add_filter(m_dbus_conn, filter_signals, reinterpret_cast<void*>(this), nullptr);

    start_thread();
}

void dbus_manager::disconnect_from_signals()
{
    m_dbus_ldr.connection_remove_filter(m_dbus_conn, filter_signals, reinterpret_cast<void*>(this));
    for (auto kv : m_signals) {
        auto signal = format_signal(kv);
        m_dbus_ldr.bus_remove_match(m_dbus_conn, signal.c_str(), &m_error);
        if (m_dbus_ldr.error_is_set(&m_error)) {
            ::perror(m_error.name);
            ::perror(m_error.message);
            m_dbus_ldr.error_free(&m_error);
        }
    }

    stop_thread();
}

bool dbus_manager::dbus_list_name_to_owner()
{
    auto reply = DBusMessage_wrap::new_method_call(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        &dbus_mgr.dbus()
    ).send_with_reply_and_block(dbus_mgr.get_conn(), DBUS_TIMEOUT);
    if(not reply) return false;

    auto iter = reply.iter();

    if(not iter.is_array()) {
        return false;
    }
    array_for_each<std::string>(iter, [&](std::string name){
        if(!starts_with(name.c_str(), "org.mpris.MediaPlayer2.")) return;
        std::string owner;
        if(dbus_get_name_owner(dbus_mgr, owner, name.c_str())){
            m_name_owners[name] = owner;
        }
    });
    return true;
}

void dbus_manager::stop_thread()
{
    m_quit = true;
    if (m_thread.joinable())
        m_thread.join();
}

void dbus_manager::start_thread()
{
    stop_thread();
    m_quit = false;
    m_thread = std::thread(&dbus_manager::dbus_thread, this);
}

void dbus_manager::dbus_thread()
{
    using namespace std::chrono_literals;
    while(!m_quit && m_dbus_ldr.connection_read_write_dispatch(m_dbus_conn, 0))
        std::this_thread::sleep_for(10ms);
}

}
