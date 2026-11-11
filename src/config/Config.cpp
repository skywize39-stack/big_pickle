#include "config/Config.hpp"
#include "features/combat/players/PlayerList.hpp"
#include "platform/windows/Platform.hpp"   // DBGLOG
#include <cstdio>
#include <algorithm>
#include <map>
#include <utility>
#include <cmath>
#include <cstdlib>

// ============================================================
//  Config.cpp — config serialisation (save / load / refresh)
// ============================================================

// Returns the directory of the current DLL/EXE.
static std::string config_dir()
{
    char path[MAX_PATH];
    GetModuleFileNameA( nullptr, path, MAX_PATH );
    std::string dir( path );
    const auto pos = dir.find_last_of( '\\' );
    if ( pos != std::string::npos )
        dir = dir.substr( 0, pos + 1 );
    return dir;
}

static void CopyLegacyRageToProfiles( cfg::aimbot_t& aim )
{
    for ( auto& p : aim.weapon_profiles.profile )
    {
        p.auto_fire = aim.auto_fire;
        p.auto_wall = aim.auto_wall;
        p.auto_scope = aim.auto_scope;
        p.min_damage = aim.min_damage;
        p.wall_min_dmg = aim.wall_min_dmg;
        p.hit_chance = aim.hit_chance;
        p.fov = aim.fov;
        p.hitgroup_mask = aim.hitgroup_mask;
        p.multipoint_scale = aim.multipoint_scale;
        p.stomach_scale = aim.stomach_scale;
        p.multipoint_head = aim.multipoint_head;
        p.multipoint_chest = aim.multipoint_chest;
        p.multipoint_stomach = aim.multipoint_stomach;
        p.multipoint_legs = aim.multipoint_legs;
        p.multipoint_feet = aim.multipoint_feet;
        p.auto_stop = aim.auto_stop;
        p.auto_stop_on_key = aim.auto_stop_on_key;
        p.auto_stop_key = aim.auto_stop_key;
        p.auto_stop_key_mode = aim.auto_stop_key_mode;
        p.auto_stop_slide_speed = aim.auto_stop_slide_speed;
    }
}

static void SaveRageProfile( std::ofstream& f, int index,
                             const cfg::rage_weapon_profile_t& p )
{
    const std::string k = "aimbot.profile." + std::to_string( index ) + ".";
    f << k << "enabled=" << p.enabled << "\n";
    f << k << "auto_fire=" << p.auto_fire << "\n";
    f << k << "auto_wall=" << p.auto_wall << "\n";
    f << k << "auto_scope=" << p.auto_scope << "\n";
    f << k << "min_damage=" << p.min_damage << "\n";
    f << k << "wall_min_dmg=" << p.wall_min_dmg << "\n";
    f << k << "min_damage_override_on_key=" << p.min_damage_override_on_key << "\n";
    f << k << "min_damage_override_key=" << p.min_damage_override_key << "\n";
    f << k << "min_damage_override_key_mode=" << p.min_damage_override_key_mode << "\n";
    f << k << "min_damage_override=" << p.min_damage_override << "\n";
    f << k << "min_damage_override_indicator=" << p.min_damage_override_indicator << "\n";
    f << k << "hit_chance=" << p.hit_chance << "\n";
    f << k << "fov=" << p.fov << "\n";
    f << k << "hitgroup_mask=" << p.hitgroup_mask << "\n";
    f << k << "multipoint_scale=" << p.multipoint_scale << "\n";
    f << k << "stomach_scale=" << p.stomach_scale << "\n";
    f << k << "multipoint_head=" << p.multipoint_head << "\n";
    f << k << "multipoint_chest=" << p.multipoint_chest << "\n";
    f << k << "multipoint_stomach=" << p.multipoint_stomach << "\n";
    f << k << "multipoint_legs=" << p.multipoint_legs << "\n";
    f << k << "multipoint_feet=" << p.multipoint_feet << "\n";
    f << k << "hitbox_preference=" << p.hitbox_preference << "\n";
    f << k << "force_body_on_key=" << p.force_body_on_key << "\n";
    f << k << "force_body_key=" << p.force_body_key << "\n";
    f << k << "force_body_key_mode=" << p.force_body_key_mode << "\n";
    f << k << "force_head_key_mode=" << p.force_head_key_mode << "\n";
    f << k << "auto_stop_key_mode=" << p.auto_stop_key_mode << "\n";
    f << k << "force_head_on_key=" << p.force_head_on_key << "\n";
    f << k << "force_head_key=" << p.force_head_key << "\n";
    f << k << "safe_point_mode=" << p.safe_point_mode << "\n";
    f << k << "auto_stop=" << p.auto_stop << "\n";
    f << k << "auto_stop_on_key=" << p.auto_stop_on_key << "\n";
    f << k << "auto_stop_key=" << p.auto_stop_key << "\n";
    f << k << "auto_stop_slide_speed=" << p.auto_stop_slide_speed << "\n";
    f << k << "fire_delay=" << p.fire_delay << "\n";
}

static bool LoadRageProfileField( const std::string& key, const std::string& val,
                                  cfg::rage_profiles_t& profiles )
{
    static const std::string prefix = "aimbot.profile.";
    if ( key.rfind( prefix, 0 ) != 0 )
        return false;

    const auto numberStart = prefix.size();
    const auto dot = key.find( '.', numberStart );
    if ( dot == std::string::npos )
        return true;

    const int index = std::stoi( key.substr( numberStart, dot - numberStart ) );
    if ( index < 0 || index >= cfg::RAGE_WEAPON_PROFILE_COUNT )
        return true;

    auto& p = profiles.profile[index];
    const std::string field = key.substr( dot + 1 );
    const bool b = val == "1";
    if      ( field == "enabled" ) p.enabled = b;
    else if ( field == "auto_fire" ) p.auto_fire = b;
    else if ( field == "auto_wall" ) p.auto_wall = b;
    else if ( field == "auto_scope" ) p.auto_scope = b;
    else if ( field == "min_damage" ) p.min_damage = std::stoi( val );
    else if ( field == "wall_min_dmg" ) p.wall_min_dmg = std::stof( val );
    else if ( field == "min_damage_override_on_key" ) p.min_damage_override_on_key = b;
    else if ( field == "min_damage_override_key" ) p.min_damage_override_key = std::stoi( val );
    else if ( field == "min_damage_override_key_mode" ) p.min_damage_override_key_mode = std::stoi( val );
    else if ( field == "min_damage_override" ) p.min_damage_override = std::stoi( val );
    else if ( field == "min_damage_override_indicator" ) p.min_damage_override_indicator = b;
    else if ( field == "hit_chance" ) p.hit_chance = std::stof( val );
    else if ( field == "fov" ) p.fov = std::stof( val );
    else if ( field == "hitgroup_mask" ) p.hitgroup_mask = std::stoi( val );
    else if ( field == "multipoint_scale" ) p.multipoint_scale = std::stof( val );
    else if ( field == "stomach_scale" ) p.stomach_scale = std::stof( val );
    else if ( field == "multipoint_head" ) p.multipoint_head = b;
    else if ( field == "multipoint_chest" ) p.multipoint_chest = b;
    else if ( field == "multipoint_stomach" ) p.multipoint_stomach = b;
    else if ( field == "multipoint_legs" ) p.multipoint_legs = b;
    else if ( field == "multipoint_feet" ) p.multipoint_feet = b;
    else if ( field == "hitbox_preference" ) p.hitbox_preference = std::stoi( val );
    else if ( field == "force_body_on_key" ) p.force_body_on_key = b;
    else if ( field == "force_body_key" ) p.force_body_key = std::stoi( val );
    else if ( field == "force_body_key_mode" ) p.force_body_key_mode = std::stoi( val );
    else if ( field == "force_head_key_mode" ) p.force_head_key_mode = std::stoi( val );
    else if ( field == "auto_stop_key_mode" ) p.auto_stop_key_mode = std::stoi( val );
    else if ( field == "force_head_on_key" ) p.force_head_on_key = b;
    else if ( field == "force_head_key" ) p.force_head_key = std::stoi( val );
    else if ( field == "safe_point_mode" ) p.safe_point_mode = std::stoi( val );
    else if ( field == "auto_stop" ) p.auto_stop = std::stoi( val );
    else if ( field == "auto_stop_on_key" ) p.auto_stop_on_key = b;
    else if ( field == "auto_stop_key" ) p.auto_stop_key = std::stoi( val );
    else if ( field == "auto_stop_slide_speed" ) p.auto_stop_slide_speed = std::stof( val );
    else if ( field == "fire_delay" ) p.fire_delay = std::stoi( val );
    return true;
}

// Strips path separators and other filesystem-hostile characters, collapses
// spaces to underscores and caps the length. Falls back to "config".
static std::string sanitize_config_name( const std::string& name )
{
    std::string out;
    for ( char c : name )
    {
        if ( c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
             c == '"' || c == '<' || c == '>' || c == '|' || c == '.' )
            continue;
        if ( c == ' ' )
            c = '_';
        out += c;
        if ( out.size() >= 32 )
            break;
    }
    return out.empty() ? "config" : out;
}

void config_wrapper_t::save( const std::string& name )
{
    const auto file = config_dir() + sanitize_config_name( name ) + ".cfg";
    std::ofstream f( file );
    if ( !f.is_open() )
    {
        DBGLOG( "[cfg] cannot open %s for writing\n", file.c_str() );
        return;
    }

    auto& v = settings.visuals;

    // ---- ESP ----
    f << "esp.esp="           << v.esp.esp           << "\n";
    f << "esp.box="           << v.esp.box           << "\n";
    f << "esp.box_type="      << v.esp.box_type      << "\n";
    f << "esp.fill_box="      << v.esp.fill_box      << "\n";
    f << "esp.outline_box="   << v.esp.outline_box   << "\n";
    f << "esp.name="          << v.esp.name          << "\n";
    f << "esp.distance="      << v.esp.distance      << "\n";
    f << "esp.health="        << v.esp.health        << "\n";
    f << "esp.armor="         << v.esp.armor         << "\n";
    f << "esp.money="         << v.esp.money         << "\n";
    f << "esp.weapon="        << v.esp.weapon        << "\n";
    f << "esp.weapon_style="  << v.esp.weapon_style  << "\n";
    f << "esp.weapon_icon_size=" << v.esp.weapon_icon_size << "\n";
    f << "esp.ammo="          << v.esp.ammo          << "\n";
    f << "esp.snaplines="     << v.esp.snaplines     << "\n";
    f << "esp.skeletons="     << v.esp.skeletons     << "\n";
    f << "esp.visible="       << v.esp.visible       << "\n";
    f << "esp.vulnerability=" << v.esp.vulnerability << "\n";
    f << "esp.scoped="        << v.esp.scoped        << "\n";
    f << "esp.defusing="      << v.esp.defusing      << "\n";
    f << "esp.flashed="       << v.esp.flashed       << "\n";
    f << "esp.show_flags="    << v.esp.show_flags    << "\n";
    f << "esp.flag_scoped="   << v.esp.flag_scoped   << "\n";
    f << "esp.flag_defuse="   << v.esp.flag_defuse   << "\n";
    f << "esp.flag_plant="    << v.esp.flag_plant    << "\n";
    f << "esp.flag_kit="      << v.esp.flag_kit      << "\n";
    f << "esp.flag_helmet="   << v.esp.flag_helmet   << "\n";
    f << "esp.flag_flashed="  << v.esp.flag_flashed  << "\n";
    f << "esp.local="         << v.esp.local         << "\n";
    f << "esp.enemies="       << v.esp.enemies       << "\n";
    f << "esp.team="          << v.esp.team          << "\n";
    f << "esp.weapons="       << v.esp.weapons       << "\n";
    f << "esp.weapon_boxes="  << v.esp.weapon_boxes  << "\n";
    f << "esp.chickens="      << v.esp.chickens      << "\n";
    f << "esp.name_font_size=" << v.esp.name_font_size << "\n";
    f << "esp.item_font_size=" << v.esp.item_font_size << "\n";
    f << "esp.offscreen_arrows=" << v.esp.offscreen_arrows << "\n";
    f << "esp.offscreen_arrow_color.r=" << v.esp.offscreen_arrow_color.r << "\n";
    f << "esp.offscreen_arrow_color.g=" << v.esp.offscreen_arrow_color.g << "\n";
    f << "esp.offscreen_arrow_color.b=" << v.esp.offscreen_arrow_color.b << "\n";
    f << "esp.offscreen_arrow_color.a=" << v.esp.offscreen_arrow_color.a << "\n";
    f << "esp.offscreen_arrow_style=" << v.esp.offscreen_arrow_style << "\n";
    f << "esp.offscreen_arrow_scale=" << v.esp.offscreen_arrow_scale << "\n";
    f << "esp.offscreen_arrow_outline=" << v.esp.offscreen_arrow_outline << "\n";
    f << "esp.box_color.r="   << v.esp.box_color.r   << "\n";
    f << "esp.box_color.g="   << v.esp.box_color.g   << "\n";
    f << "esp.box_color.b="   << v.esp.box_color.b   << "\n";
    f << "esp.box_color.a="   << v.esp.box_color.a   << "\n";
    f << "esp.fill_color.r="  << v.esp.fill_color.r  << "\n";
    f << "esp.fill_color.g="  << v.esp.fill_color.g  << "\n";
    f << "esp.fill_color.b="  << v.esp.fill_color.b  << "\n";
    f << "esp.fill_color.a="  << v.esp.fill_color.a  << "\n";

    // ---- Chams ----
    f << "chams.chams="           << v.chams.chams               << "\n";
    f << "chams.local="           << v.chams.local               << "\n";
    f << "chams.enemy="           << v.chams.enemy               << "\n";
    f << "chams.team="            << v.chams.team                << "\n";
    f << "chams.arms="            << v.chams.arms                << "\n";
    f << "chams.weapon="          << v.chams.weapon              << "\n";
    f << "chams.visible_r="       << v.chams.visible_r           << "\n";
    f << "chams.visible_g="       << v.chams.visible_g           << "\n";
    f << "chams.visible_b="       << v.chams.visible_b           << "\n";
    f << "chams.visible_a="       << v.chams.visible_a           << "\n";
    f << "chams.occluded_r="      << v.chams.occluded_r          << "\n";
    f << "chams.occluded_g="      << v.chams.occluded_g          << "\n";
    f << "chams.occluded_b="      << v.chams.occluded_b          << "\n";
    f << "chams.occluded_a="      << v.chams.occluded_a          << "\n";
    f << "chams.style_visible="   << v.chams.chams_style_visible << "\n";
    f << "chams.style_occluded="  << v.chams.chams_style_occluded<< "\n";
    f << "chams.local_r="  << v.chams.local_r  << "\n";
    f << "chams.local_g="  << v.chams.local_g  << "\n";
    f << "chams.local_b="  << v.chams.local_b  << "\n";
    f << "chams.local_a="  << v.chams.local_a  << "\n";
    f << "chams.local_style=" << v.chams.local_style << "\n";
    f << "chams.arms_r="   << v.chams.arms_r   << "\n";
    f << "chams.arms_g="   << v.chams.arms_g   << "\n";
    f << "chams.arms_b="   << v.chams.arms_b   << "\n";
    f << "chams.arms_a="   << v.chams.arms_a   << "\n";
    f << "chams.arms_style=" << v.chams.arms_style << "\n";
    f << "chams.weapon_r=" << v.chams.weapon_r << "\n";
    f << "chams.weapon_g=" << v.chams.weapon_g << "\n";
    f << "chams.weapon_b=" << v.chams.weapon_b << "\n";
    f << "chams.weapon_a=" << v.chams.weapon_a << "\n";
    f << "chams.weapon_style=" << v.chams.weapon_style << "\n";
    f << "chams.rainbow="         << v.chams.rainbow             << "\n";
    f << "chams.rainbow_speed="   << v.chams.rainbow_speed       << "\n";
    f << "chams.overlay_style=" << v.chams.overlay_style << "\n";
    f << "chams.overlay=" << v.chams.overlay << "\n";
    f << "chams.overlay_r=" << v.chams.overlay_r << "\n";
    f << "chams.overlay_g=" << v.chams.overlay_g << "\n";
    f << "chams.overlay_b=" << v.chams.overlay_b << "\n";
    f << "chams.overlay_a=" << v.chams.overlay_a << "\n";
    f << "chams.local_overlay=" << v.chams.local_overlay << "\n";
    f << "chams.local_ov_r=" << v.chams.local_ov_r << "\n";
    f << "chams.local_ov_g=" << v.chams.local_ov_g << "\n";
    f << "chams.local_ov_b=" << v.chams.local_ov_b << "\n";
    f << "chams.local_ov_a=" << v.chams.local_ov_a << "\n";
    f << "chams.arms_overlay=" << v.chams.arms_overlay << "\n";
    f << "chams.arms_ov_r=" << v.chams.arms_ov_r << "\n";
    f << "chams.arms_ov_g=" << v.chams.arms_ov_g << "\n";
    f << "chams.arms_ov_b=" << v.chams.arms_ov_b << "\n";
    f << "chams.arms_ov_a=" << v.chams.arms_ov_a << "\n";
    f << "chams.weapon_overlay=" << v.chams.weapon_overlay << "\n";
    f << "chams.weapon_ov_r=" << v.chams.weapon_ov_r << "\n";
    f << "chams.weapon_ov_g=" << v.chams.weapon_ov_g << "\n";
    f << "chams.weapon_ov_b=" << v.chams.weapon_ov_b << "\n";
    f << "chams.weapon_ov_a=" << v.chams.weapon_ov_a << "\n";

    // ---- Glow ----
    f << "glow.glow="          << v.glow.glow          << "\n";
    f << "glow.visible="       << v.glow.visible       << "\n";
    f << "glow.vulnerability=" << v.glow.vulnerability << "\n";
    f << "glow.style="         << v.glow.style         << "\n";
    f << "glow.local="         << v.glow.local         << "\n";
    f << "glow.enemy="         << v.glow.enemy         << "\n";
    f << "glow.team="          << v.glow.team          << "\n";
    f << "glow.weapons="       << v.glow.weapons       << "\n";
    f << "glow.chickens="      << v.glow.chickens      << "\n";

    // ---- DLights ----
    f << "dlights.enable=" << v.dlights.enable << "\n";
    f << "dlights.local="  << v.dlights.local  << "\n";
    f << "dlights.enemy="  << v.dlights.enemy  << "\n";
    f << "dlights.team="   << v.dlights.team   << "\n";
    f << "dlights.radius=" << v.dlights.radius << "\n";

// ---- Others / Misc ----
    f << "others.watermark="            << v.others.watermark            << "\n";
    f << "others.keybind_list="         << v.others.keybind_list         << "\n";
    f << "others.keybind_gradient=" << v.others.keybind_gradient << "\n";
    f << "others.keybind_color.r=" << v.others.keybind_color.r << "\n";
    f << "others.keybind_color.g=" << v.others.keybind_color.g << "\n";
    f << "others.keybind_color.b=" << v.others.keybind_color.b << "\n";
    f << "others.keybind_color.a=" << v.others.keybind_color.a << "\n";
    f << "others.keybind_x=" << v.others.keybind_x << "\n";
    f << "others.keybind_y=" << v.others.keybind_y << "\n";
    f << "others.event_logs="           << v.others.event_logs           << "\n";
    f << "others.event_logs_console="   << v.others.event_logs_console   << "\n";
    f << "others.bullet_tracers="       << v.others.bullet_tracers       << "\n";
    f << "others.hitmarker="            << v.others.hitmarker            << "\n";
    f << "others.hitmarker_damage="     << v.others.hitmarker_damage     << "\n";
    f << "others.night_mode="           << v.others.night_mode           << "\n";
    f << "others.no_smoke=" << v.others.no_smoke << "\n";
    f << "others.no_smoke_particles=" << v.others.no_smoke_particles << "\n";
    f << "others.molotov_zone=" << v.others.molotov_zone << "\n";
    f << "others.smoke_timer=" << v.others.smoke_timer << "\n";
    f << "others.molotov_color.r=" << v.others.molotov_color.r << "\n";
    f << "others.molotov_color.g=" << v.others.molotov_color.g << "\n";
    f << "others.molotov_color.b=" << v.others.molotov_color.b << "\n";
    f << "others.molotov_color.a=" << v.others.molotov_color.a << "\n";
    f << "others.smoke_timer_color.r=" << v.others.smoke_timer_color.r << "\n";
    f << "others.smoke_timer_color.g=" << v.others.smoke_timer_color.g << "\n";
    f << "others.smoke_timer_color.b=" << v.others.smoke_timer_color.b << "\n";
    f << "others.smoke_timer_color.a=" << v.others.smoke_timer_color.a << "\n";
    f << "others.night_mode_intensity=" << v.others.night_mode_intensity << "\n";
    f << "others.night_mode_color.r="   << v.others.night_mode_color.r   << "\n";
    f << "others.night_mode_color.g="   << v.others.night_mode_color.g   << "\n";
    f << "others.night_mode_color.b="   << v.others.night_mode_color.b   << "\n";
    f << "others.night_mode_color.a="   << v.others.night_mode_color.a   << "\n";
    f << "misc.bhop="        << settings.misc.bhop    << "\n";
    f << "misc.autostrafe="  << settings.misc.autostrafe << "\n";
    f << "misc.autostrafe_smoothing=" << settings.misc.autostrafe_smoothing << "\n";
    f << "misc.autostrafe_avoid_walls=" << settings.misc.autostrafe_avoid_walls << "\n";
    f << "misc.no_flash="    << settings.misc.no_flash << "\n";
    f << "misc.remove_scope_overlay=" << settings.misc.remove_scope_overlay << "\n";
    f << "misc.scope_fov=" << settings.misc.scope_fov << "\n";
    f << "misc.crosshair.scope_thickness=" << settings.misc.crosshair.scope_thickness << "\n";
    f << "misc.crosshair.scope_length="    << settings.misc.crosshair.scope_length    << "\n";
    f << "misc.crosshair.scope_gap="       << settings.misc.crosshair.scope_gap       << "\n";
    f << "misc.crosshair.scope_dot="       << settings.misc.crosshair.scope_dot       << "\n";
    f << "misc.crosshair.scope_dot_size="  << settings.misc.crosshair.scope_dot_size  << "\n";
    f << "misc.crosshair.scope_outline="   << settings.misc.crosshair.scope_outline   << "\n";
    f << "misc.crosshair.scope_t_style="   << settings.misc.crosshair.scope_t_style   << "\n";
    f << "misc.crosshair.scope_r=" << settings.misc.crosshair.scope_color.r << "\n";
    f << "misc.crosshair.scope_g=" << settings.misc.crosshair.scope_color.g << "\n";
    f << "misc.crosshair.scope_b=" << settings.misc.crosshair.scope_color.b << "\n";
    f << "misc.crosshair.scope_a=" << settings.misc.crosshair.scope_color.a << "\n";
    f << "misc.menu_gamma=" << settings.misc.menu_gamma << "\n";
    f << "misc.menu_layout=" << settings.misc.menu_layout << "\n";
    f << "misc.menu_key=" << settings.misc.menu_key << "\n";
    f << "misc.menu_width=" << settings.misc.menu_width << "\n";
    f << "misc.menu_height=" << settings.misc.menu_height << "\n";
    f << "misc.menu_box_heights=" << settings.misc.menu_box_heights << "\n";
    f << "misc.thirdperson=" << settings.misc.thirdperson << "\n";
    f << "misc.thirdperson_on_key=" << settings.misc.thirdperson_on_key << "\n";
    f << "misc.thirdperson_key="   << settings.misc.thirdperson_key << "\n";
    f << "misc.thirdperson_key_mode=" << settings.misc.thirdperson_key_mode << "\n";
    f << "misc.auto_peek=" << settings.misc.auto_peek << "\n";
    f << "misc.auto_peek_key=" << settings.misc.auto_peek_key << "\n";
    f << "misc.auto_peek_mode=" << settings.misc.auto_peek_mode << "\n";
    f << "misc.auto_peek_max_distance=" << settings.misc.auto_peek_max_distance << "\n";
    f << "misc.auto_peek_color.r=" << settings.misc.auto_peek_color.r << "\n";
    f << "misc.auto_peek_color.g=" << settings.misc.auto_peek_color.g << "\n";
    f << "misc.auto_peek_color.b=" << settings.misc.auto_peek_color.b << "\n";
    f << "misc.auto_peek_color.a=" << settings.misc.auto_peek_color.a << "\n";

    // ---- Anti-Aim ----
    f << "misc.antiaim.enabled="    << settings.misc.antiaim.enabled    << "\n";
    f << "misc.antiaim.yaw_mode="   << settings.misc.antiaim.yaw_mode   << "\n";
    f << "misc.antiaim.pitch_mode=" << settings.misc.antiaim.pitch_mode << "\n";
    f << "misc.antiaim.spin_speed=" << settings.misc.antiaim.spin_speed << "\n";
    f << "misc.antiaim.freestand_on_key=" << settings.misc.antiaim.freestand_on_key << "\n";
    f << "misc.antiaim.freestand_key=" << settings.misc.antiaim.freestand_key << "\n";
    f << "misc.antiaim.freestand_key_mode=" << settings.misc.antiaim.freestand_key_mode << "\n";
    f << "misc.antiaim.jitter_enabled=" << settings.misc.antiaim.jitter_enabled << "\n";
    f << "misc.antiaim.jitter_mode="    << settings.misc.antiaim.jitter_mode    << "\n";
    f << "misc.antiaim.jitter_range="   << settings.misc.antiaim.jitter_range   << "\n";
    f << "misc.antiaim.jitter_speed="   << settings.misc.antiaim.jitter_speed   << "\n";
    f << "misc.antiaim.freestand_flip=" << settings.misc.antiaim.freestand_flip << "\n";
    f << "misc.antiaim.freestand_disable_jitter=" << settings.misc.antiaim.freestand_disable_jitter << "\n";
    f << "misc.antiaim.base_yaw=" << settings.misc.antiaim.base_yaw << "\n";
    f << "misc.antiaim.manual_enabled=" << settings.misc.antiaim.manual_enabled << "\n";
    f << "misc.antiaim.manual_left_key=" << settings.misc.antiaim.manual_left_key << "\n";
    f << "misc.antiaim.manual_right_key=" << settings.misc.antiaim.manual_right_key << "\n";
    f << "misc.antiaim.manual_back_key=" << settings.misc.antiaim.manual_back_key << "\n";
    f << "misc.antiaim.manual_disable_jitter=" << settings.misc.antiaim.manual_disable_jitter << "\n";
    f << "misc.antiaim.manual_indicator=" << settings.misc.antiaim.manual_indicator << "\n";

    // ---- Fake Lag ----
    f << "misc.fakelag.enabled="     << settings.misc.fakelag.enabled     << "\n";
    f << "misc.fakelag.choke_ticks=" << settings.misc.fakelag.choke_ticks << "\n";
    f << "misc.fakelag.variance="    << settings.misc.fakelag.variance    << "\n";
    f << "misc.fake_ping.enabled="   << settings.misc.fake_ping.enabled   << "\n";
    f << "misc.fake_ping.delay_ms="  << settings.misc.fake_ping.delay_ms  << "\n";
    f << "misc.fake_ping.indicator=" << settings.misc.fake_ping.indicator << "\n";

    f << "misc.fake_duck.enabled=" << settings.misc.fake_duck.enabled << "\n";
    f << "misc.fake_duck.key=" << settings.misc.fake_duck.key << "\n";
    f << "misc.fake_duck.key_mode=" << settings.misc.fake_duck.key_mode << "\n";

    // ---- Double Tap ----
    f << "misc.doubletap.enabled="   << settings.misc.doubletap.enabled   << "\n";
    f << "misc.doubletap.ticks="     << settings.misc.doubletap.ticks     << "\n";
    f << "misc.doubletap.key="       << settings.misc.doubletap.key       << "\n";
    f << "misc.doubletap.key_mode="  << settings.misc.doubletap.key_mode  << "\n";
    f << "misc.doubletap.indicator=" << settings.misc.doubletap.indicator << "\n";
    f << "misc.doubletap.second_shot=" << settings.misc.doubletap.second_shot << "\n";
    f << "misc.doubletap.defensive=" << settings.misc.doubletap.defensive << "\n";

    // ---- Viewmodel ----
    f << "misc.viewmodel.enabled="  << settings.misc.viewmodel.enabled  << "\n";
    f << "misc.viewmodel.fov="      << settings.misc.viewmodel.fov      << "\n";
    f << "misc.viewmodel.offset_x=" << settings.misc.viewmodel.offset_x << "\n";
    f << "misc.viewmodel.offset_y=" << settings.misc.viewmodel.offset_y << "\n";
    f << "misc.viewmodel.offset_z=" << settings.misc.viewmodel.offset_z << "\n";
    f << "misc.viewmodel.recoil="   << settings.misc.viewmodel.recoil   << "\n";

    // ---- Custom grenade prediction ----
    f << "misc.grenade_prediction.enabled=" << settings.misc.grenade_prediction.enabled << "\n";
    f << "misc.grenade_prediction.damage_indicator="
      << settings.misc.grenade_prediction.damage_indicator << "\n";
    f << "misc.grenade_prediction.warning=" << settings.misc.grenade_prediction.warning << "\n";

    // ---- Animated ClanTag ----
    f << "misc.clantag.enabled="  << settings.misc.clantag.enabled  << "\n";
    f << "misc.clantag.text="     << settings.misc.clantag.text     << "\n";
    f << "misc.clantag.speed_ms=" << settings.misc.clantag.speed_ms << "\n";

    // ---- KillSay ----
    f << "misc.killsay.enabled="     << settings.misc.killsay.enabled     << "\n";
    f << "misc.killsay.message="     << settings.misc.killsay.message     << "\n";
    f << "misc.killsay.cooldown_ms=" << settings.misc.killsay.cooldown_ms << "\n";

    // ---- Voice Kill ----
    f << "misc.voice_kill.enabled=" << settings.misc.voice_kill.enabled << "\n";
    f << "misc.voice_kill.mode=" << settings.misc.voice_kill.mode << "\n";
    f << "misc.voice_kill.selected=" << settings.misc.voice_kill.selected << "\n";
    f << "misc.voice_kill.death_enabled=" << settings.misc.voice_kill.death_enabled << "\n";
    f << "misc.voice_kill.death_mode=" << settings.misc.voice_kill.death_mode << "\n";
    f << "misc.voice_kill.death_selected=" << settings.misc.voice_kill.death_selected << "\n";

    // ---- Aimbot ----
    f << "aimbot.enabled=" << settings.aimbot.enabled << "\n";
    f << "aimbot.on_key="  << settings.aimbot.on_key  << "\n";
    f << "aimbot.key="     << settings.aimbot.key     << "\n";
    f << "aimbot.key_mode=" << settings.aimbot.key_mode << "\n";
    f << "aimbot.auto_stop_key_mode=" << settings.aimbot.auto_stop_key_mode << "\n";
    f << "aimbot.bone="    << settings.aimbot.bone    << "\n";
    f << "aimbot.bone_index=" << settings.aimbot.bone_index << "\n";
    f << "aimbot.fov="     << settings.aimbot.fov     << "\n";
    f << "aimbot.silent="  << settings.aimbot.silent  << "\n";
    f << "aimbot.rage="           << settings.aimbot.rage           << "\n";
    f << "aimbot.auto_fire="      << settings.aimbot.auto_fire      << "\n";
    f << "aimbot.auto_wall="      << settings.aimbot.auto_wall      << "\n";
    f << "aimbot.min_damage="     << settings.aimbot.min_damage     << "\n";
    f << "aimbot.wall_min_dmg="   << settings.aimbot.wall_min_dmg   << "\n";
    f << "aimbot.hitgroup_mask=" << settings.aimbot.hitgroup_mask << "\n";
    f << "aimbot.auto_stop="      << settings.aimbot.auto_stop      << "\n";
    f << "aimbot.auto_stop_on_key=" << settings.aimbot.auto_stop_on_key << "\n";
    f << "aimbot.auto_stop_key="  << settings.aimbot.auto_stop_key  << "\n";
    f << "aimbot.auto_stop_slide_speed=" << settings.aimbot.auto_stop_slide_speed << "\n";
    f << "aimbot.multipoint_scale=" << settings.aimbot.multipoint_scale << "\n";
    f << "aimbot.stomach_scale="    << settings.aimbot.stomach_scale    << "\n";
    f << "aimbot.multipoint_head="   << settings.aimbot.multipoint_head   << "\n";
    f << "aimbot.multipoint_chest="  << settings.aimbot.multipoint_chest  << "\n";
    f << "aimbot.multipoint_stomach=" << settings.aimbot.multipoint_stomach << "\n";
    f << "aimbot.multipoint_legs="   << settings.aimbot.multipoint_legs   << "\n";
    f << "aimbot.multipoint_feet="   << settings.aimbot.multipoint_feet   << "\n";
    f << "aimbot.hit_chance="     << settings.aimbot.hit_chance     << "\n";
    f << "aimbot.auto_scope="     << settings.aimbot.auto_scope     << "\n";
    f << "aimbot.auto_revolver="  << settings.aimbot.auto_revolver  << "\n";
    f << "aimbot.auto_revolver_partial_cock="
      << settings.aimbot.auto_revolver_partial_cock << "\n";
    f << "aimbot.backtrack="      << settings.aimbot.backtrack      << "\n";
    f << "aimbot.backtrack_ticks=" << settings.aimbot.backtrack_ticks << "\n";
    f << "aimbot.backtrack_only_test=" << settings.aimbot.backtrack_only_test << "\n";
    f << "aimbot.weapon_profiles.enabled=" << settings.aimbot.weapon_profiles.enabled << "\n";
    f << "aimbot.weapon_profiles.editor_profile=" << settings.aimbot.weapon_profiles.editor_profile << "\n";
    for ( int i = 0; i < cfg::RAGE_WEAPON_PROFILE_COUNT; ++i )
        SaveRageProfile( f, i, settings.aimbot.weapon_profiles.profile[i] );

    // ---- Triggerbot ----
    f << "triggerbot.enabled="    << settings.triggerbot.enabled    << "\n";
    f << "triggerbot.on_key="     << settings.triggerbot.on_key     << "\n";
    f << "triggerbot.key="        << settings.triggerbot.key        << "\n";
    f << "triggerbot.key_mode="   << settings.triggerbot.key_mode   << "\n";
    f << "triggerbot.delay_ms="   << settings.triggerbot.delay_ms   << "\n";
    f << "triggerbot.smoke_check=" << settings.triggerbot.smoke_check << "\n";
    f << "triggerbot.flash_check=" << settings.triggerbot.flash_check << "\n";
    f << "triggerbot.hitgroup="   << settings.triggerbot.hitgroup   << "\n";

    // ---- PlayerList (nicknames percent-encoded, see PlayerList::EncodeName) ----
    for ( const auto& [name, entry] : PlayerList::Export() )
    {
        const std::string k = "playerlist." + PlayerList::EncodeName( name ) + ".";
        f << k << "aim="       << entry.aim       << "\n";
        f << k << "prefer="    << entry.prefer    << "\n";
        f << k << "visuals="   << entry.visuals   << "\n";
        f << k << "highlight=" << entry.highlight << "\n";
        f << k << "hl_r="      << entry.hl_r      << "\n";
        f << k << "hl_g="      << entry.hl_g      << "\n";
        f << k << "hl_b="      << entry.hl_b      << "\n";
        f << k << "hl_a="      << entry.hl_a      << "\n";
    }

    DBGLOG( f.good() ? "[cfg] saved %s\n" : "[cfg] write error: %s\n", file.c_str() );
}

void config_wrapper_t::reset()
{
    auto list = settings.config_list;
    const int selected = settings.config_id;
    settings = cfg::settings_t{};
    settings.config_list = std::move(list);
    settings.config_id = selected >= 0 && selected < static_cast<int>(settings.config_list.size())
        ? selected : -1;
    PlayerList::Import({});
}

void config_wrapper_t::load( const std::string& name )
{
    const std::string base = sanitize_config_name( name );
    std::ifstream f( config_dir() + base + ".cfg" );
    if ( !f.is_open() )
        f.open( config_dir() + base );   // legacy extension-less configs
    if ( !f.is_open() )
    {
        DBGLOG( "[cfg] config '%s' not found in %s\n", base.c_str(), config_dir().c_str() );
        return;
    }

    // Legacy configs without geometry use the original menu dimensions.
    settings.misc.menu_width=720.f;settings.misc.menu_height=606.f;
    settings.misc.menu_box_heights.clear();
    settings.misc.menu_key = VK_INSERT;
    settings.misc.auto_peek_color = cfg::misc_t{}.auto_peek_color;
    settings.misc.crosshair = cfg::misc_t{}.crosshair; // absent in older configs
    settings.misc.fake_duck = {}; // absent in older configs: never inherit an armed feature
    auto& v = settings.visuals;
    bool sawWeaponProfile = false;
    std::string line;

    // PlayerList rows are spread over several lines per player, so they are
    // accumulated here and imported once after the file is consumed.
    std::map<std::string, PlayerList::Entry> plPending;

    while ( std::getline( f, line ) )
    {
        const auto eq = line.find( '=' );
        if ( eq == std::string::npos )
            continue;

        const auto key = line.substr( 0, eq );
        const auto val = line.substr( eq + 1 );

        // Weapon profile dispatch remains separate from the macro chains below
        // to keep MSVC C1061 nesting shallow and old configs fully compatible.
        if ( key == "aimbot.weapon_profiles.enabled" )
        {
            settings.aimbot.weapon_profiles.enabled = ( val == "1" );
            continue;
        }
        if ( key == "aimbot.weapon_profiles.editor_profile" )
        {
            settings.aimbot.weapon_profiles.editor_profile = std::stoi( val );
            continue;
        }
        if ( LoadRageProfileField( key, val, settings.aimbot.weapon_profiles ) )
        {
            sawWeaponProfile = true;
            continue;
        }

        // ---- PlayerList ("playerlist.<encoded-name>.<field>") ----
        if ( key.rfind( "playerlist.", 0 ) == 0 )
        {
            const auto lastDot = key.rfind( '.' );
            if ( lastDot != std::string::npos && lastDot > 11 )
            {
                const std::string enc   = key.substr( 11, lastDot - 11 );
                const std::string field = key.substr( lastDot + 1 );
                PlayerList::Entry& e = plPending[ enc ];
                const bool b = ( val == "1" );
                if      ( field == "aim" )       e.aim       = b;
                else if ( field == "prefer" )    e.prefer    = b;
                else if ( field == "visuals" )   e.visuals   = b;
                else if ( field == "highlight" ) e.highlight = b;
                else if ( field == "hl_r" )      e.hl_r      = std::stoi( val );
                else if ( field == "hl_g" )      e.hl_g      = std::stoi( val );
                else if ( field == "hl_b" )      e.hl_b      = std::stoi( val );
                else if ( field == "hl_a" )      e.hl_a      = std::stoi( val );
            }
            continue;
        }

        // ---- Viewmodel (kept out of the big else-if chain below to stay
        //      under MSVC's C1061 block-nesting limit) ----
        if ( key.rfind( "misc.viewmodel.", 0 ) == 0 )
        {
            if ( key == "misc.viewmodel.enabled"  ) settings.misc.viewmodel.enabled  = ( val == "1" );
            else if ( key == "misc.viewmodel.fov" ) settings.misc.viewmodel.fov      = std::stof( val );
            else if ( key == "misc.viewmodel.offset_x" ) settings.misc.viewmodel.offset_x = std::stof( val );
            else if ( key == "misc.viewmodel.offset_y" ) settings.misc.viewmodel.offset_y = std::stof( val );
            else if ( key == "misc.viewmodel.offset_z" ) settings.misc.viewmodel.offset_z = std::stof( val );
            else if ( key == "misc.viewmodel.recoil" ) settings.misc.viewmodel.recoil = std::stof( val );
            continue;
        }

        // ---- Custom grenade prediction ----
        if ( key.rfind( "misc.grenade_prediction.", 0 ) == 0 )
        {
            if ( key == "misc.grenade_prediction.enabled" )
                settings.misc.grenade_prediction.enabled = ( val == "1" );
            else if ( key == "misc.grenade_prediction.damage_indicator" )
                settings.misc.grenade_prediction.damage_indicator = ( val == "1" );
            else if ( key == "misc.grenade_prediction.warning" )
                settings.misc.grenade_prediction.warning = ( val == "1" );
            continue;
        }

        // ---- Animated ClanTag ----
        if ( key.rfind( "misc.clantag.", 0 ) == 0 )
        {
            if ( key == "misc.clantag.enabled" )
                settings.misc.clantag.enabled = ( val == "1" );
            else if ( key == "misc.clantag.text" )
                strncpy_s( settings.misc.clantag.text, val.c_str(), _TRUNCATE );
            else if ( key == "misc.clantag.speed_ms" )
                settings.misc.clantag.speed_ms = std::clamp( std::stof( val ), 100.f, 2000.f );
            continue;
        }

        // ---- KillSay ----
        if ( key.rfind( "misc.killsay.", 0 ) == 0 )
        {
            if ( key == "misc.killsay.enabled" )
            {
                settings.misc.killsay.enabled = ( val == "1" );
            }
            else if ( key == "misc.killsay.message" )
            {
                strncpy_s( settings.misc.killsay.message, val.c_str(), _TRUNCATE );
            }
            else if ( key == "misc.killsay.cooldown_ms" )
            {
                settings.misc.killsay.cooldown_ms = std::stof( val );
            }
            continue;
        }

        // ---- Voice Kill ----
        if ( key == "misc.voice_kill.enabled" )
        {
            settings.misc.voice_kill.enabled = ( val == "1" );
            continue;
        }
        if ( key == "misc.voice_kill.mode" )
        {
            settings.misc.voice_kill.mode = std::clamp( std::stoi( val ), 0, 2 );
            continue;
        }
        if ( key == "misc.voice_kill.selected" )
        {
            strncpy_s( settings.misc.voice_kill.selected, val.c_str(), _TRUNCATE );
            continue;
        }
        if ( key == "misc.voice_kill.death_enabled" )
        {
            settings.misc.voice_kill.death_enabled = ( val == "1" );
            continue;
        }
        if ( key == "misc.voice_kill.death_mode" )
        {
            settings.misc.voice_kill.death_mode = std::clamp( std::stoi( val ), 0, 2 );
            continue;
        }
        if ( key == "misc.voice_kill.death_selected" )
        {
            strncpy_s( settings.misc.voice_kill.death_selected, val.c_str(), _TRUNCATE );
            continue;
        }

#define CFG_BOOL(k, m)  if (key == k) { v.m = (val == "1"); }
#define CFG_INT(k, m)   if (key == k) { v.m = std::stoi(val); }
#define CFG_FLOAT(k, m) if (key == k) { v.m = std::stof(val); }
#define CFG_FLT(k, m)  if (key == k) { v.m = std::stof(val); }

        CFG_BOOL("esp.esp",           esp.esp)
        CFG_BOOL("esp.box",           esp.box)
        CFG_INT ("esp.box_type",      esp.box_type)
        CFG_BOOL("esp.fill_box",      esp.fill_box)
        CFG_BOOL("esp.outline_box",   esp.outline_box)
        CFG_BOOL("esp.name",          esp.name)
        CFG_BOOL("esp.distance",      esp.distance)
        CFG_INT ("esp.health",        esp.health)
        CFG_INT ("esp.armor",         esp.armor)
        CFG_BOOL("esp.money",         esp.money)
        CFG_BOOL("esp.weapon",        esp.weapon)
        CFG_INT ("esp.weapon_style",  esp.weapon_style)
        CFG_INT ("esp.weapon_icon_size", esp.weapon_icon_size)
        CFG_BOOL("esp.ammo",          esp.ammo)
        CFG_BOOL("esp.snaplines",     esp.snaplines)
        CFG_BOOL("esp.skeletons",     esp.skeletons)
        CFG_BOOL("esp.visible",       esp.visible)
        CFG_BOOL("esp.vulnerability", esp.vulnerability)
        CFG_BOOL("esp.scoped",        esp.scoped)
        CFG_BOOL("esp.defusing",      esp.defusing)
        CFG_BOOL("esp.flashed",       esp.flashed)
        CFG_BOOL("esp.show_flags",    esp.show_flags)
        CFG_BOOL("esp.flag_scoped",   esp.flag_scoped)
        CFG_BOOL("esp.flag_defuse",   esp.flag_defuse)
        CFG_BOOL("esp.flag_plant",    esp.flag_plant)
        CFG_BOOL("esp.flag_kit",      esp.flag_kit)
        CFG_BOOL("esp.flag_helmet",   esp.flag_helmet)
        CFG_BOOL("esp.flag_flashed",  esp.flag_flashed)
        CFG_BOOL("esp.local",         esp.local)
        CFG_BOOL("esp.enemies",       esp.enemies)
        CFG_BOOL("esp.team",          esp.team)
        CFG_BOOL("esp.weapons",       esp.weapons)
        CFG_BOOL("esp.weapon_boxes",  esp.weapon_boxes)
        CFG_BOOL("esp.chickens",      esp.chickens)
        CFG_INT ("esp.name_font_size", esp.name_font_size)
        CFG_INT ("esp.item_font_size", esp.item_font_size)
        CFG_BOOL("esp.offscreen_arrows", esp.offscreen_arrows)
        CFG_INT ("esp.offscreen_arrow_color.r", esp.offscreen_arrow_color.r)
        CFG_INT ("esp.offscreen_arrow_color.g", esp.offscreen_arrow_color.g)
        CFG_INT ("esp.offscreen_arrow_color.b", esp.offscreen_arrow_color.b)
        CFG_INT ("esp.offscreen_arrow_color.a", esp.offscreen_arrow_color.a)
        CFG_INT ("esp.offscreen_arrow_style", esp.offscreen_arrow_style)
        CFG_FLOAT("esp.offscreen_arrow_scale", esp.offscreen_arrow_scale)
        CFG_BOOL("esp.offscreen_arrow_outline", esp.offscreen_arrow_outline)
        CFG_INT ("esp.box_color.r",   esp.box_color.r)
        CFG_INT ("esp.box_color.g",   esp.box_color.g)
        CFG_INT ("esp.box_color.b",   esp.box_color.b)
        CFG_INT ("esp.box_color.a",   esp.box_color.a)
        CFG_INT ("esp.fill_color.r",  esp.fill_color.r)
        CFG_INT ("esp.fill_color.g",  esp.fill_color.g)
        CFG_INT ("esp.fill_color.b",  esp.fill_color.b)
        CFG_INT ("esp.fill_color.a",  esp.fill_color.a)
        CFG_BOOL("chams.chams",           chams.chams)
        CFG_BOOL("chams.local",           chams.local)
        CFG_BOOL("chams.enemy",           chams.enemy)
        CFG_BOOL("chams.team",            chams.team)
        CFG_BOOL("chams.arms",            chams.arms)
        CFG_BOOL("chams.weapon",          chams.weapon)
        CFG_INT ("chams.visible_r",       chams.visible_r)
        CFG_INT ("chams.visible_g",       chams.visible_g)
        CFG_INT ("chams.visible_b",       chams.visible_b)
        CFG_INT ("chams.visible_a",       chams.visible_a)
        CFG_INT ("chams.occluded_r",      chams.occluded_r)
        CFG_INT ("chams.occluded_g",      chams.occluded_g)
        CFG_INT ("chams.occluded_b",      chams.occluded_b)
        CFG_INT ("chams.occluded_a",      chams.occluded_a)
        CFG_INT ("chams.style_visible",   chams.chams_style_visible)
        CFG_INT ("chams.style_occluded",  chams.chams_style_occluded)
        CFG_INT ("chams.local_r",  chams.local_r)
        CFG_INT ("chams.local_g",  chams.local_g)
        CFG_INT ("chams.local_b",  chams.local_b)
        CFG_INT ("chams.local_a",  chams.local_a)
        CFG_INT ("chams.local_style", chams.local_style)
        CFG_INT ("chams.arms_r",   chams.arms_r)
        CFG_INT ("chams.arms_g",   chams.arms_g)
        CFG_INT ("chams.arms_b",   chams.arms_b)
        CFG_INT ("chams.arms_a",   chams.arms_a)
        CFG_INT ("chams.arms_style", chams.arms_style)
        CFG_INT ("chams.weapon_r", chams.weapon_r)
        CFG_INT ("chams.weapon_g", chams.weapon_g)
        CFG_INT ("chams.weapon_b", chams.weapon_b)
        CFG_INT ("chams.weapon_a", chams.weapon_a)
        CFG_INT ("chams.weapon_style", chams.weapon_style)
        CFG_BOOL("chams.rainbow",         chams.rainbow)
        CFG_FLT ("chams.rainbow_speed",   chams.rainbow_speed)
        CFG_INT ("chams.overlay_style", chams.overlay_style)
        CFG_BOOL("chams.overlay", chams.overlay)
        CFG_INT ("chams.overlay_r", chams.overlay_r)
        CFG_INT ("chams.overlay_g", chams.overlay_g)
        CFG_INT ("chams.overlay_b", chams.overlay_b)
        CFG_INT ("chams.overlay_a", chams.overlay_a)
        CFG_BOOL("chams.local_overlay", chams.local_overlay)
        CFG_INT ("chams.local_ov_r", chams.local_ov_r)
        CFG_INT ("chams.local_ov_g", chams.local_ov_g)
        CFG_INT ("chams.local_ov_b", chams.local_ov_b)
        CFG_INT ("chams.local_ov_a", chams.local_ov_a)
        CFG_BOOL("chams.arms_overlay", chams.arms_overlay)
        CFG_INT ("chams.arms_ov_r", chams.arms_ov_r)
        CFG_INT ("chams.arms_ov_g", chams.arms_ov_g)
        CFG_INT ("chams.arms_ov_b", chams.arms_ov_b)
        CFG_INT ("chams.arms_ov_a", chams.arms_ov_a)
        CFG_BOOL("chams.weapon_overlay", chams.weapon_overlay)
        CFG_INT ("chams.weapon_ov_r", chams.weapon_ov_r)
        CFG_INT ("chams.weapon_ov_g", chams.weapon_ov_g)
        CFG_INT ("chams.weapon_ov_b", chams.weapon_ov_b)
        CFG_INT ("chams.weapon_ov_a", chams.weapon_ov_a)
        CFG_BOOL("glow.glow",          glow.glow)
        CFG_BOOL("glow.visible",       glow.visible)
        CFG_BOOL("glow.vulnerability", glow.vulnerability)
        CFG_INT ("glow.style",         glow.style)
        CFG_BOOL("glow.local",         glow.local)
        CFG_BOOL("glow.enemy",         glow.enemy)
        CFG_BOOL("glow.team",          glow.team)
        CFG_BOOL("glow.weapons",       glow.weapons)
        CFG_BOOL("glow.chickens",      glow.chickens)
        CFG_BOOL("dlights.enable", dlights.enable)
        CFG_BOOL("dlights.local",  dlights.local)
        CFG_BOOL("dlights.enemy",  dlights.enemy)
        CFG_BOOL("dlights.team",   dlights.team)
        CFG_FLT ("dlights.radius", dlights.radius)
        CFG_BOOL("others.watermark", others.watermark)
        CFG_BOOL("others.keybind_list", others.keybind_list)
        CFG_BOOL("others.keybind_gradient", others.keybind_gradient)
        CFG_INT ("others.keybind_color.r", others.keybind_color.r)
        CFG_INT ("others.keybind_color.g", others.keybind_color.g)
        CFG_INT ("others.keybind_color.b", others.keybind_color.b)
        CFG_INT ("others.keybind_color.a", others.keybind_color.a)
        CFG_INT ("others.keybind_x", others.keybind_x)
        CFG_INT ("others.keybind_y", others.keybind_y)
        CFG_BOOL("others.event_logs", others.event_logs)
        CFG_BOOL("others.event_logs_console", others.event_logs_console)
        CFG_BOOL("others.bullet_tracers", others.bullet_tracers)
        CFG_BOOL("others.hitmarker", others.hitmarker)
        CFG_BOOL("others.hitmarker_damage", others.hitmarker_damage)
        CFG_BOOL("others.night_mode", others.night_mode)
        CFG_BOOL("others.no_smoke", others.no_smoke)
        CFG_BOOL("others.no_smoke_particles", others.no_smoke_particles)
        CFG_BOOL("others.molotov_zone", others.molotov_zone)
        CFG_BOOL("others.smoke_timer", others.smoke_timer)
        CFG_INT ("others.molotov_color.r", others.molotov_color.r)
        CFG_INT ("others.molotov_color.g", others.molotov_color.g)
        CFG_INT ("others.molotov_color.b", others.molotov_color.b)
        CFG_INT ("others.molotov_color.a", others.molotov_color.a)
        CFG_INT ("others.smoke_timer_color.r", others.smoke_timer_color.r)
        CFG_INT ("others.smoke_timer_color.g", others.smoke_timer_color.g)
        CFG_INT ("others.smoke_timer_color.b", others.smoke_timer_color.b)
        CFG_INT ("others.smoke_timer_color.a", others.smoke_timer_color.a)
        CFG_INT ("others.night_mode_intensity", others.night_mode_intensity)
        CFG_INT ("others.night_mode_color.r", others.night_mode_color.r)
        CFG_INT ("others.night_mode_color.g", others.night_mode_color.g)
        CFG_INT ("others.night_mode_color.b", others.night_mode_color.b)
        CFG_INT ("others.night_mode_color.a", others.night_mode_color.a)
        // Start a new independent dispatch chain here. Keeping this as
        // `else if` after the ESP/chams/glow macro chain exceeds MSVC's C1061
        // block-nesting limit in Debug/x64 builds.
        if ( key == "misc.bhop" ) settings.misc.bhop = ( val == "1" );
        if ( key == "misc.autostrafe" ) settings.misc.autostrafe = ( val == "1" );
        if ( key == "misc.autostrafe_smoothing" ) settings.misc.autostrafe_smoothing = std::stof( val );
        if ( key == "misc.autostrafe_avoid_walls" ) settings.misc.autostrafe_avoid_walls = ( val == "1" );
        if ( key == "misc.no_flash" ) settings.misc.no_flash = ( val == "1" );
        if ( key == "misc.remove_scope_overlay" ) settings.misc.remove_scope_overlay = ( val == "1" );
        if ( key == "misc.scope_fov" ) settings.misc.scope_fov = std::stof( val );
        if ( key.rfind( "misc.crosshair.", 0 ) == 0 &&
             ( key == "misc.crosshair.scope_r" || key == "misc.crosshair.scope_g" ||
               key == "misc.crosshair.scope_b" || key == "misc.crosshair.scope_a" ) )
        {
            try { const int channel = std::clamp( std::stoi( val ), 0, 255 );
                if ( key == "misc.crosshair.scope_r" ) settings.misc.crosshair.scope_color.r = channel;
                if ( key == "misc.crosshair.scope_g" ) settings.misc.crosshair.scope_color.g = channel;
                if ( key == "misc.crosshair.scope_b" ) settings.misc.crosshair.scope_color.b = channel;
                if ( key == "misc.crosshair.scope_a" ) settings.misc.crosshair.scope_color.a = channel;
            } catch ( ... ) {}
        }
        if ( key == "misc.crosshair.scope_thickness" ) settings.misc.crosshair.scope_thickness = std::stof( val );
        if ( key == "misc.crosshair.scope_length" )    settings.misc.crosshair.scope_length    = std::stof( val );
        if ( key == "misc.crosshair.scope_gap" )       settings.misc.crosshair.scope_gap       = std::stof( val );
        if ( key == "misc.crosshair.scope_dot" )       settings.misc.crosshair.scope_dot       = ( val == "1" );
        if ( key == "misc.crosshair.scope_dot_size" )  settings.misc.crosshair.scope_dot_size  = std::stof( val );
        if ( key == "misc.crosshair.scope_outline" )   settings.misc.crosshair.scope_outline   = ( val == "1" );
        if ( key == "misc.crosshair.scope_t_style" )   settings.misc.crosshair.scope_t_style   = ( val == "1" );
        if (key == "misc.menu_key")
        {
            char* end = nullptr;
            const long value = std::strtol(val.c_str(), &end, 10);
            settings.misc.menu_key = end != val.c_str() && *end == '\0' &&
                value >= VK_BACK && value < 256 ? static_cast<int>(value) : VK_INSERT;
            continue;
        }
        if(key=="misc.menu_width" || key=="misc.menu_height")
        {
            char* end=nullptr;const float n=std::strtof(val.c_str(),&end);
            if(end!=val.c_str() && *end=='\0' && std::isfinite(n))
            {
                if(key=="misc.menu_width")settings.misc.menu_width=std::clamp(n,500.f,4096.f);
                else settings.misc.menu_height=std::clamp(n,586.f,4096.f);
            }
            continue;
        }
        if(key=="misc.menu_box_heights")settings.misc.menu_box_heights=val;
        if ( key == "misc.menu_layout" ) settings.misc.menu_layout = val;
        if ( key == "misc.menu_gamma" ) settings.misc.menu_gamma = std::clamp( std::stoi( val ), 0, 2 );
        if ( key == "misc.thirdperson" ) settings.misc.thirdperson = ( val == "1" );
        if ( key == "misc.thirdperson_on_key" ) settings.misc.thirdperson_on_key = ( val == "1" );
        if ( key == "misc.thirdperson_key" )
        {
            const int savedKey = std::stoi( val );
            // F3 was the old built-in default. Migrate only that legacy default
            // to Mouse3 while preserving every user-selected custom key.
            settings.misc.thirdperson_key = savedKey == VK_F3 ? VK_MBUTTON : savedKey;
        }
        if ( key == "misc.thirdperson_key_mode" )
            settings.misc.thirdperson_key_mode = std::clamp( std::stoi( val ), 0, 3 );
        if ( key == "misc.auto_peek" ) settings.misc.auto_peek = ( val == "1" );
        if ( key == "misc.auto_peek_key" ) settings.misc.auto_peek_key = std::stoi( val );
        if ( key == "misc.auto_peek_mode" ) settings.misc.auto_peek_mode = std::stoi( val );
        if ( key == "misc.auto_peek_max_distance" ) settings.misc.auto_peek_max_distance = std::stof( val );
        if (key.rfind("misc.auto_peek_color.", 0) == 0)
        {
            char* end = nullptr;
            const long value = std::strtol(val.c_str(), &end, 10);
            if (end != val.c_str() && *end == '\0')
            {
                const int channel = static_cast<int>(std::clamp(value, 0L, 255L));
                auto& color = settings.misc.auto_peek_color;
                if (key == "misc.auto_peek_color.r") color.r = channel;
                if (key == "misc.auto_peek_color.g") color.g = channel;
                if (key == "misc.auto_peek_color.b") color.b = channel;
                if (key == "misc.auto_peek_color.a") color.a = channel;
            }
        }
        if ( key == "misc.antiaim.enabled"    ) settings.misc.antiaim.enabled    = ( val == "1" );
        if ( key == "misc.antiaim.yaw_mode"   ) settings.misc.antiaim.yaw_mode   = std::stoi( val );
        if ( key == "misc.antiaim.pitch_mode" ) settings.misc.antiaim.pitch_mode = std::stoi( val );
        if ( key == "misc.antiaim.spin_speed" ) settings.misc.antiaim.spin_speed = std::stof( val );
        if ( key == "misc.antiaim.freestand_on_key" ) settings.misc.antiaim.freestand_on_key = ( val == "1" );
        if ( key == "misc.antiaim.freestand_key" ) settings.misc.antiaim.freestand_key = std::stoi( val );
        if ( key == "misc.antiaim.freestand_key_mode" ) settings.misc.antiaim.freestand_key_mode = std::stoi( val );
        if ( key == "misc.antiaim.jitter_enabled" ) settings.misc.antiaim.jitter_enabled = ( val == "1" );
        if ( key == "misc.antiaim.jitter_mode"    ) settings.misc.antiaim.jitter_mode    = std::stoi( val );
        if ( key == "misc.antiaim.jitter_range"   ) settings.misc.antiaim.jitter_range   = std::stof( val );
        if ( key == "misc.antiaim.jitter_speed"   ) settings.misc.antiaim.jitter_speed   = std::clamp( std::stoi( val ), 1, 16 );
        if ( key == "misc.antiaim.freestand_flip" ) settings.misc.antiaim.freestand_flip = ( val == "1" );
        if ( key == "misc.antiaim.freestand_disable_jitter" ) settings.misc.antiaim.freestand_disable_jitter = ( val == "1" );
        if ( key == "misc.antiaim.base_yaw" ) settings.misc.antiaim.base_yaw = std::stoi( val );
        if ( key == "misc.antiaim.manual_enabled" ) settings.misc.antiaim.manual_enabled = ( val == "1" );
        if ( key == "misc.antiaim.manual_left_key" ) settings.misc.antiaim.manual_left_key = std::stoi( val );
        if ( key == "misc.antiaim.manual_right_key" ) settings.misc.antiaim.manual_right_key = std::stoi( val );
        if ( key == "misc.antiaim.manual_back_key" ) settings.misc.antiaim.manual_back_key = std::stoi( val );
        if ( key == "misc.antiaim.manual_disable_jitter" ) settings.misc.antiaim.manual_disable_jitter = ( val == "1" );
        if ( key == "misc.antiaim.manual_indicator" ) settings.misc.antiaim.manual_indicator = ( val == "1" );
        if ( key == "misc.fakelag.enabled"     ) settings.misc.fakelag.enabled     = ( val == "1" );
        if ( key == "misc.fakelag.choke_ticks" ) settings.misc.fakelag.choke_ticks = std::stoi( val );
        if ( key == "misc.fakelag.variance"    ) settings.misc.fakelag.variance    = std::stoi( val );
        if ( key == "misc.fake_ping.enabled"   ) settings.misc.fake_ping.enabled   = ( val == "1" );
        if ( key == "misc.fake_ping.delay_ms"  ) settings.misc.fake_ping.delay_ms  = std::clamp( std::stoi( val ), 16, 218 );
        if ( key == "misc.fake_ping.indicator" ) settings.misc.fake_ping.indicator = ( val == "1" );
        if ( key == "misc.fake_duck.enabled" ) settings.misc.fake_duck.enabled = ( val == "1" );
        if ( key == "misc.fake_duck.key" ) settings.misc.fake_duck.key = std::clamp( std::stoi( val ), 0, 255 );
        if ( key == "misc.fake_duck.key_mode" ) settings.misc.fake_duck.key_mode = std::clamp( std::stoi( val ), 0, 3 );
        if ( key == "misc.doubletap.enabled" ) settings.misc.doubletap.enabled = ( val == "1" );
        if ( key == "misc.doubletap.ticks"   ) settings.misc.doubletap.ticks   = std::clamp( std::stoi( val ), 2, 15 );
        if ( key == "misc.doubletap.key" ) settings.misc.doubletap.key = std::stoi( val );
        if ( key == "misc.doubletap.key_mode" ) settings.misc.doubletap.key_mode = std::stoi( val );
        if ( key == "misc.doubletap.defensive" ) settings.misc.doubletap.defensive = ( val == "1" );
        if ( key == "misc.doubletap.indicator" ) settings.misc.doubletap.indicator = ( val == "1" );
        if ( key == "misc.doubletap.second_shot" ) settings.misc.doubletap.second_shot = std::stoi( val );
        if ( key == "aimbot.enabled" ) settings.aimbot.enabled = ( val == "1" );
        if ( key == "aimbot.on_key"  ) settings.aimbot.on_key  = ( val == "1" );
        if ( key == "aimbot.key"     ) settings.aimbot.key     = std::stoi( val );
        if ( key == "aimbot.key_mode" ) settings.aimbot.key_mode = std::stoi( val );
        if ( key == "aimbot.auto_stop_key_mode" ) settings.aimbot.auto_stop_key_mode = std::stoi( val );
        if ( key == "aimbot.bone"       ) settings.aimbot.bone       = std::stoi( val );
        if ( key == "aimbot.bone_index" ) settings.aimbot.bone_index = std::stoi( val );
        if ( key == "aimbot.fov"     ) settings.aimbot.fov     = std::stof( val );
        if ( key == "aimbot.silent"  ) settings.aimbot.silent  = ( val == "1" );
        if ( key == "aimbot.rage"           ) settings.aimbot.rage           = ( val == "1" );
        if ( key == "aimbot.auto_fire"      ) settings.aimbot.auto_fire      = ( val == "1" );
        if ( key == "aimbot.auto_wall"      ) settings.aimbot.auto_wall      = ( val == "1" );
        if ( key == "aimbot.min_damage"     ) settings.aimbot.min_damage     = std::stoi( val );
        if ( key == "aimbot.wall_min_dmg"   ) settings.aimbot.wall_min_dmg   = std::stof( val );
        if ( key == "aimbot.hitgroup_mask" ) settings.aimbot.hitgroup_mask = std::stoi( val );
        if ( key == "aimbot.auto_stop"      ) settings.aimbot.auto_stop      = std::stoi( val );
        if ( key == "aimbot.auto_stop_on_key" ) settings.aimbot.auto_stop_on_key = ( val == "1" );
        if ( key == "aimbot.auto_stop_key"  ) settings.aimbot.auto_stop_key  = std::stoi( val );
        if ( key == "aimbot.auto_stop_slide_speed" ) settings.aimbot.auto_stop_slide_speed = std::stof( val );
        if ( key == "aimbot.multipoint_scale" ) settings.aimbot.multipoint_scale = std::stof( val );
        if ( key == "aimbot.stomach_scale"    ) settings.aimbot.stomach_scale    = std::stof( val );
        if ( key == "aimbot.multipoint_head"   ) settings.aimbot.multipoint_head   = ( val == "1" );
        if ( key == "aimbot.multipoint_chest"  ) settings.aimbot.multipoint_chest  = ( val == "1" );
        if ( key == "aimbot.multipoint_stomach" ) settings.aimbot.multipoint_stomach = ( val == "1" );
        if ( key == "aimbot.multipoint_legs"   ) settings.aimbot.multipoint_legs   = ( val == "1" );
        if ( key == "aimbot.multipoint_feet"   ) settings.aimbot.multipoint_feet   = ( val == "1" );
        if ( key == "aimbot.hit_chance"     ) settings.aimbot.hit_chance     = std::stof( val );
        if ( key == "aimbot.auto_revolver"  ) settings.aimbot.auto_revolver  = ( val == "1" );
        if ( key == "aimbot.auto_revolver_partial_cock" )
            settings.aimbot.auto_revolver_partial_cock = ( val == "1" );
        if ( key == "aimbot.auto_scope"     ) settings.aimbot.auto_scope     = ( val == "1" );
        if ( key == "aimbot.backtrack"      ) settings.aimbot.backtrack      = ( val == "1" );
        if ( key == "aimbot.backtrack_ticks" )
            settings.aimbot.backtrack_ticks = std::clamp( std::stoi( val ), 1, 12 );
        if ( key == "aimbot.backtrack_only_test" )
            settings.aimbot.backtrack_only_test = ( val == "1" );
        if ( key == "triggerbot.enabled"    ) settings.triggerbot.enabled    = ( val == "1" );
        if ( key == "triggerbot.on_key"     ) settings.triggerbot.on_key     = ( val == "1" );
        if ( key == "triggerbot.key"        ) settings.triggerbot.key        = std::stoi( val );
        if ( key == "triggerbot.key_mode"   ) settings.triggerbot.key_mode   = std::stoi( val );
        if ( key == "triggerbot.delay_ms"   ) settings.triggerbot.delay_ms   = std::stoi( val );
        if ( key == "triggerbot.smoke_check" ) settings.triggerbot.smoke_check = ( val == "1" );
        if ( key == "triggerbot.flash_check" ) settings.triggerbot.flash_check = ( val == "1" );
        if ( key == "triggerbot.hitgroup"   ) settings.triggerbot.hitgroup   = std::stoi( val );

#undef CFG_BOOL
#undef CFG_INT
#undef CFG_FLT
    }

    // Configs saved before per-weapon rage existed contain only the legacy
    // global aimbot keys. Copy their loaded values into every profile once so
    // existing setups keep their exact behaviour after the update.
    if ( !sawWeaponProfile )
        CopyLegacyRageToProfiles( settings.aimbot );

    if ( !plPending.empty() )
    {
        std::vector<std::pair<std::string, PlayerList::Entry>> plRows;
        plRows.reserve( plPending.size() );
        for ( auto& [enc, entry] : plPending )
            plRows.emplace_back( PlayerList::DecodeName( enc ), entry );
        PlayerList::Import( plRows );
    }

    if ( settings.aimbot.weapon_profiles.editor_profile < 0 ||
         settings.aimbot.weapon_profiles.editor_profile >= cfg::RAGE_WEAPON_PROFILE_COUNT )
        settings.aimbot.weapon_profiles.editor_profile = 0;
}

void config_wrapper_t::remove( const std::string& name )
{
    const std::string base = sanitize_config_name( name );
    DeleteFileA( ( config_dir() + base + ".cfg" ).c_str() );
    DeleteFileA( ( config_dir() + base ).c_str() );   // legacy copy
    refresh();
}

void config_wrapper_t::refresh()
{
    settings.config_list.clear();
    settings.config_id = 0;

    // Canonical format is "<name>.cfg" next to the game exe; the extension
    // is stripped for display.
    const std::string dir = config_dir();
    const std::string pattern = dir + "*.cfg";

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA( pattern.c_str(), &fd );
    if ( hFind != INVALID_HANDLE_VALUE )
    {
        do {
            std::string n = fd.cFileName;
            const auto dot = n.find_last_of( '.' );
            if ( dot != std::string::npos )
                n = n.substr( 0, dot );
            if ( !n.empty() )
                settings.config_list.push_back( n );
        } while ( FindNextFileA( hFind, &fd ) );
        FindClose( hFind );
    }

    if ( settings.config_list.empty() )
        settings.config_list.push_back( "default" );
}
