#pragma once

// ============================================================
//  Config.hpp — settings tree and config serialisation
// ============================================================

#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <Windows.h>

namespace cfg
{
    // ----------------------------------------------------------
    //  Box colour helper
    // ----------------------------------------------------------
    struct box_color_t
    {
        int r = 255, g = 0, b = 0, a = 255;
    };

    // ----------------------------------------------------------
    //  Hitgroup selection bits (bit = 1 << HITGROUP_*, SDK.hpp)
    // ----------------------------------------------------------
    inline constexpr int HITGROUP_BIT_HEAD      = 1 << 1;
    inline constexpr int HITGROUP_BIT_CHEST     = 1 << 2;
    inline constexpr int HITGROUP_BIT_STOMACH   = 1 << 3;
    inline constexpr int HITGROUP_BIT_LEFTARM   = 1 << 4;
    inline constexpr int HITGROUP_BIT_RIGHTARM  = 1 << 5;
    inline constexpr int HITGROUP_BIT_LEFTLEG   = 1 << 6;
    inline constexpr int HITGROUP_BIT_RIGHTLEG  = 1 << 7;

    // ----------------------------------------------------------
    //  Rage weapon profiles
    // ----------------------------------------------------------
    // CSSO exposes a stable active-weapon classname but not a verified shared
    // item-definition table. Profiles therefore map weapon classes exactly as
    // professional rage menus do: pistols, heavy pistols, SMGs, rifles, Scout,
    // AWP, auto-snipers, shotguns and LMGs, plus a safe Default fallback.
    enum class rage_weapon_profile_id : int
    {
        Default = 0,
        Pistol,
        HeavyPistol,
        SMG,
        Rifle,
        Scout,
        AWP,
        AutoSniper,
        Shotgun,
        LMG,
        Knife,
        Count
    };

    inline constexpr int RAGE_WEAPON_PROFILE_COUNT =
        static_cast<int>( rage_weapon_profile_id::Count );

    inline const char* RageWeaponProfileName( int index )
    {
        switch ( static_cast<rage_weapon_profile_id>( index ) )
        {
        case rage_weapon_profile_id::Default:     return "Default / Unknown";
        case rage_weapon_profile_id::Pistol:      return "Pistols";
        case rage_weapon_profile_id::HeavyPistol: return "Heavy Pistols (Deagle/R8)";
        case rage_weapon_profile_id::SMG:         return "SMGs";
        case rage_weapon_profile_id::Rifle:       return "Rifles";
        case rage_weapon_profile_id::Scout:       return "Scout / SSG 08";
        case rage_weapon_profile_id::AWP:         return "AWP";
        case rage_weapon_profile_id::AutoSniper:  return "Auto Snipers";
        case rage_weapon_profile_id::Shotgun:     return "Shotguns";
        case rage_weapon_profile_id::LMG:         return "LMGs";
        case rage_weapon_profile_id::Knife:       return "Knife / Utility";
        default:                                  return "Default / Unknown";
        }
    }

    struct rage_weapon_profile_t
    {
        bool  enabled = true;
        bool  auto_fire = false;
        bool  auto_wall = false;
        bool  auto_scope = true;
        int   min_damage = 20;
        float wall_min_dmg = 10.f;

        // While this Hold bind is down, both visible and wall min-damage
        // gates use min_damage_override for the current weapon profile.
        bool  min_damage_override_on_key = false;
        int   min_damage_override_key = VK_XBUTTON1;
        int   min_damage_override_key_mode = 0; // 0 = hold, 1 = toggle
        int   min_damage_override = 5;
        bool  min_damage_override_indicator = true;

        float hit_chance = 80.f;
        float fov = 180.f;

        int   hitgroup_mask = HITGROUP_BIT_HEAD | HITGROUP_BIT_CHEST | HITGROUP_BIT_STOMACH;
        float multipoint_scale = 100.f;
        float stomach_scale = 100.f;
        bool  multipoint_head = true;
        bool  multipoint_chest = true;
        bool  multipoint_stomach = true;
        bool  multipoint_legs = true;
        bool  multipoint_feet = true;

        // 0 = neutral, 1 = prefer head, 2 = prefer body. Force modes below
        // replace the hitbox mask only while their Hold bind is physically down.
        int   hitbox_preference = 0;
        bool  force_body_on_key = false;
        int   force_body_key = VK_XBUTTON1;
        int   force_body_key_mode = 0;
        bool  force_head_on_key = false;
        int   force_head_key = VK_XBUTTON2;
        int   force_head_key_mode = 0;

        // 0 = off, 1 = prefer hitbox-center safety, 2 = force center-only.
        // CSSO has one current matrix per snapshot; center-only is the safe
        // geometrical policy available without pretending to have resolver data.
        int   safe_point_mode = 0;

        // 0 = off, 1 = Slide (~one third weapon speed), 2 = Full (zero).
    int   auto_stop = 1;
    bool  auto_stop_on_key = false;
    int   auto_stop_key = VK_LSHIFT;
    int   auto_stop_key_mode = 0;
    float auto_stop_slide_speed = 85.f; // legacy persisted field; unused
    int   fire_delay = 0;   // min ticks between committed rage shots (0 = off)
    };

    struct rage_profiles_t
    {
        bool enabled = true;
        int  editor_profile = 0;
        std::array<rage_weapon_profile_t, RAGE_WEAPON_PROFILE_COUNT> profile{};
    };

    // ----------------------------------------------------------
    //  ESP settings
    // ----------------------------------------------------------
    struct esp_t
    {
        bool esp          = true;
        bool box          = true;
        int  box_type     = 0;       // 0 = full, 1 = corner
        bool fill_box     = false;
        bool outline_box  = true;
        bool name         = true;
        bool distance     = false;
        int  health       = 1;       // 0 = off, 1 = bar
        int  armor        = 0;       // 0 = off, 1 = bar
        bool money        = false;
        bool weapon       = true;
        // 0 = classname text (Surface font), 1 = icon glyph from the
        // embedded CS2GunIcons.ttf surface font (weapons::gunIcons ASCII keys).
        int  weapon_style = 1;
        // CS2GunIcons glyph size in pixels. Clamped to the pre-created
        // surface font range (10..24) at draw time.
        int  weapon_icon_size = 16;
        bool ammo         = false;
        bool snaplines    = false;
        bool skeletons    = false;
        bool visible      = false;
        bool vulnerability= false;
        bool scoped       = false;
        bool defusing     = false;
        bool flashed      = false;

        // Flags panel
        bool show_flags   = true;
        bool flag_scoped  = true;
        bool flag_defuse  = true;
        bool flag_plant   = true;
        bool flag_kit     = true;
        bool flag_helmet  = true;
        bool flag_flashed = true;

        bool local        = false;
        bool enemies      = true;
        bool team         = false;
        bool weapons      = true;
        bool weapon_boxes = true;
        bool chickens     = false;

        // Direct VGuiSurface033 text sizes. `name_font_size` drives the
        // antialiased Verdana used for nicknames; `item_font_size` drives the
        // non-antialiased Small Fonts used for flags and weapon labels.
        // Both are clamped to the pre-created font range at draw time.
        int  name_font_size = 12;
        int  item_font_size = 9;

        // Edge markers for enemies that are behind or outside the screen.
        // Resolved from live entity data inside the PaintTraverse pass.
        bool offscreen_arrows = false;
        box_color_t offscreen_arrow_color{ 235, 80, 80, 220 };

        // Marker shape. Keep in sync with kArrowStyleNames in Menu.cpp.
        //   0 = Triangle, 1 = Chevron, 2 = Radar Pip, 3 = Arrow
        int   offscreen_arrow_style  = 0;
        // Scales the marker and its distance from the screen centre.
        float offscreen_arrow_scale  = 1.f;
        // Dark drop shadow behind the marker so it stays readable on
        // bright maps (snow, sand).
        bool  offscreen_arrow_outline = true;

        box_color_t box_color;
        box_color_t fill_color;
    };

    // ----------------------------------------------------------
    //  Chams settings
    // ----------------------------------------------------------
    struct chams_t
    {
        bool  chams               = false;
        bool  local               = false;
        bool  enemy               = true;
        bool  team                = false;
        bool  arms                = false;
        bool  weapon              = false;
        int   visible_r           = 0;
        int   visible_g           = 255;
        int   visible_b           = 0;
        int   visible_a           = 255;
        int   occluded_r          = 255;
        int   occluded_g          = 0;
        int   occluded_b          = 0;
        int   occluded_a          = 255;
        int   chams_style_visible = 0;
        int   chams_style_occluded= 0;
        // local player / viewmodel arms / viewmodel weapon groups
        int   local_r             = 0;
        int   local_g             = 200;
        int   local_b             = 255;
        int   local_a             = 255;
        int   local_style         = 0;
        int   arms_r              = 255;
        int   arms_g              = 170;
        int   arms_b              = 60;
        int   arms_a              = 255;
        int   arms_style          = 0;
        int   weapon_r            = 110;
        int   weapon_g            = 255;
        int   weapon_b            = 110;
        int   weapon_a            = 255;
        int   weapon_style        = 0;
        bool  rainbow             = false;
        float rainbow_speed       = 1.0f;
        // overlay layer drawn over the base material (0 glow, 1 wireframe, 2 ghost, 3 pulse)
        int   overlay_style       = 0;
        bool  overlay             = false;   // enemies
        int   overlay_r = 160, overlay_g = 120, overlay_b = 255, overlay_a = 255;
        bool  local_overlay       = false;
        int   local_ov_r = 255, local_ov_g = 255, local_ov_b = 255, local_ov_a = 200;
        bool  arms_overlay        = false;
        int   arms_ov_r = 255, arms_ov_g = 255, arms_ov_b = 255, arms_ov_a = 200;
        bool  weapon_overlay      = false;
        int   weapon_ov_r = 255, weapon_ov_g = 255, weapon_ov_b = 255, weapon_ov_a = 200;
    };

    // ----------------------------------------------------------
    //  Glow settings
    // ----------------------------------------------------------
    struct glow_t
    {
        bool glow          = false;
        bool visible       = false;
        bool vulnerability = false;
        int  style         = 0;
        bool local         = false;
        bool enemy         = true;
        bool team          = false;
        bool weapons       = false;
        bool chickens      = false;
    };

    // ----------------------------------------------------------
    //  Dynamic lights settings
    // ----------------------------------------------------------
    struct dlights_t
    {
        bool  enable = false;
        bool  local  = false;
        bool  enemy  = true;
        bool  team   = false;
        float radius = 200.f;
    };

    // ----------------------------------------------------------
    //  Misc visual settings
    // ----------------------------------------------------------
    struct others_t
    {
        bool watermark  = true;
        bool keybind_list = true;   // skeet-style active keybind list
        bool keybind_gradient = false;                 // teamskeet: cyan-magenta-lime top line
        box_color_t keybind_color{ 175, 255, 0, 255 }; // accent line + bind names
        int  keybind_x = -1, keybind_y = -1;           // list position (-1 = default)
        bool event_logs   = true;   // skeet-style hit / miss / hurt logs
        bool event_logs_console = true; // direct tier0 print, independent of screen logs
        bool bullet_tracers = true;  // skeet-style beam eye -> impact
        bool hitmarker      = true;  // world cross at hit point
        bool hitmarker_damage = true; // floating damage number
        bool night_mode = false; // applies only through verified MaterialSystem084/IMaterial slots
        bool bomb_timer = true;  // planted C4 countdown panel with defuse indicator
        bool no_smoke = false;            // no gray fog overlay inside smoke
        bool no_smoke_particles = true;   // also skip building the smoke cloud particles
        bool molotov_zone = true;         // ground fill + burn countdown at active fires
        bool smoke_timer = true;          // countdown + soft ring at active smokes
        box_color_t molotov_color{ 255, 96, 32, 120 };
        box_color_t smoke_timer_color{ 185, 185, 200, 140 };

        // 0% = original material color; 100% = full custom tint multiplier.
        int night_mode_intensity = 75;
        box_color_t night_mode_color{ 45, 55, 85, 255 };
    };

    // ----------------------------------------------------------
    //  Fake lag settings
    // ----------------------------------------------------------
    struct fake_duck_t
    {
        bool enabled = false;
        int key = 0;
        int key_mode = 0;
    };

    struct fakelag_t
    {
        bool  enabled     = false;
        int   choke_ticks = 8;
        int   variance    = 0;
    };

    // Delays outgoing command packets through the already verified CL_Move
    // choke site. This is outgoing delay, not a spoofed server latency value;
    // the maximum stays below the established 14-command safety cap.
    struct fake_ping_t
    {
        bool enabled = false;
        int  delay_ms = 100; // quantized to 64-tick command intervals
        bool indicator = true;
    };

    // ----------------------------------------------------------
    //  Double tap (command-shift) settings
    // ----------------------------------------------------------
    struct doubletap_t
    {
        bool enabled   = false;
        int  ticks     = 15;          // matching server's confirmed command ceiling
        int  key       = VK_XBUTTON2; // Mouse 5 by default
        int  key_mode  = 0;           // 0 = hold, 1 = toggle
        bool indicator = true;
        // Defensive: on peek start the real DT charge is released as one
        // teleport burst before the enemy sees us (breaks lag comp).
        bool defensive = false;
        // Legacy config field retained for old files. Both values now use
        // fresh per-command prediction; cached-angle forced shots are removed.
        int  second_shot = 0;
    };

    // ----------------------------------------------------------
    //  Viewmodel changer settings (viewmodel_fov / viewmodel_offset_x/y/z / viewmodel_recoil)
    // ----------------------------------------------------------
    struct viewmodel_t
    {
        bool  enabled  = false;
        float fov      = 60.f;   // matches viewmodel_fov default
        float offset_x = 1.0f;
        float offset_y = 1.0f;
        float offset_z = -1.0f;
        float recoil   = 1.0f;
    };

    // ----------------------------------------------------------
    //  Anti-aim settings
    // ----------------------------------------------------------
    struct antiaim_t
    {
        bool  enabled    = false;
        // Zero-based to match ImGui::Combo indices exactly:
        // 0=backward, 1=left, 2=right, 3=jitter, 4=spin, 5=backward jitter.
        int   yaw_mode   = 0;
        int   pitch_mode = 0;   // 0=down, 1=up, 2=zero
        float spin_speed = 20.f;

        // Freestand uses the already confirmed IEngineTrace path to compare
        // cover on each shoulder while this bind is active.
        bool  freestand_on_key = false;
        int   freestand_key = VK_XBUTTON2;
        int   freestand_key_mode = 0; // 0 = hold, 1 = toggle
        bool  freestand_disable_jitter = true; // keep the head still behind cover
        bool  freestand_flip = false;          // swap the side if the head goes the wrong way

        // Base yaw: 0 = relative to camera, 1 = at targets (closest enemy to crosshair)
        int   base_yaw = 0;

        // Manual anti-aim: press a key to lock the yaw left / right / back,
        // press the same key again to release. Overrides yaw mode + freestand.
        bool  manual_enabled   = false;
        int   manual_left_key  = VK_LEFT;
        int   manual_right_key = VK_RIGHT;
        int   manual_back_key  = VK_DOWN;
        bool  manual_disable_jitter = true;
        bool  manual_indicator = true;

        // ------------------------------------------------------
        //  Jitter — user-configurable yaw oscillation applied ON
        //  TOP of the resolved base angle (any yaw mode, or the
        //  freestand shoulder). Phase counting uses applied send
        //  ticks, not tick_count, so the server observes a strict
        //  alternation pattern even under fakelag choking.
        // ------------------------------------------------------
        bool  jitter_enabled = false;
        int   jitter_mode   = 0;    // 0 = offset flip, 1 = random, 2 = sway
        float jitter_range  = 30.f; // degrees around the base yaw (1..180)
        int   jitter_speed  = 2;    // applied send ticks per jitter phase
    };

    // ----------------------------------------------------------
    //  Custom grenade trajectory prediction
    // ----------------------------------------------------------
    struct grenade_prediction_t
    {
        bool enabled = false;
        bool damage_indicator = true; // show the best visible HE damage estimate
        bool warning = true;          // predict thrown (enemy/any) grenades in flight
        bool timers = true;           // show countdown ring + time on each warning icon
    };

    // ----------------------------------------------------------
    //  Animated ClanTag settings
    // ----------------------------------------------------------
    struct clantag_t
    {
        bool  enabled = false;
        char  text[32] = "project";
        float speed_ms = 350.f;
    };

    // ----------------------------------------------------------
    //  KillSay settings
    // ----------------------------------------------------------
    struct killsay_t
    {
        bool  enabled = false;
        char  message[128] = "ez";
        float cooldown_ms = 800.f;   // min gap between messages
    };

    // ----------------------------------------------------------
    //  Voice Kill settings
    // ----------------------------------------------------------
    struct voice_kill_t
    {
        // 0 = selected clip, 1 = random library clip, 2 = next clip in order.
        bool enabled = false; // voice clip after a local kill
        int  mode = 0;
        char selected[128] = "mellstroy-raduet.wav";

        // Independent library for player_death events where the local player
        // is the victim. Disabled by default to preserve existing behavior.
        bool death_enabled = false;
        int  death_mode = 0;
        char death_selected[128] = "";
    };

    // ----------------------------------------------------------
    //  Scope crosshair (drawn while scoped with "Remove scope overlay")
    // ----------------------------------------------------------
    struct crosshair_t
    {
        float       scope_thickness = 1.f;   // px
        float       scope_length    = 0.f;   // 0 = full-screen lines, else px per side
        float       scope_gap       = 0.f;   // px center gap (centered style only)
        bool        scope_dot       = true;
        float       scope_dot_size  = 2.f;   // px square edge
        bool        scope_outline   = true;  // 1px black outline behind every part
        bool        scope_t_style   = false; // removes the top line (centered style)
        box_color_t scope_color     { 255, 255, 255, 255 };
    };

    // ----------------------------------------------------------
    //  Gameplay misc settings
    // ----------------------------------------------------------
    struct misc_t
    {
        int         menu_key = VK_INSERT; // keyboard key, independent of feature binds
        float       menu_width = 720.f, menu_height = 606.f;
        std::string menu_box_heights;    // manually resized groupboxes
        std::string menu_layout;         // custom groupbox order (drag & drop), "" = auto
        int         menu_gamma  = 0;     // 0 = auto-detect, 1 = normal, 2 = gamma fix (linearize menu colors)
        bool        bhop        = true;
        bool        autostrafe  = true;  // air strafe assist (bhop movement)
        float       autostrafe_smoothing = 30.f;    // 0..100
        bool        autostrafe_avoid_walls = true;
        bool        no_flash    = false;
        bool        remove_scope_overlay = false;
        float       scope_fov   = 0.f;   // 0 = native zoom FOV, else scope FOV override while scoped
        bool        thirdperson = false;
        bool        thirdperson_on_key = false;
        int         thirdperson_key = VK_MBUTTON; // Mouse3 / middle mouse
        int         thirdperson_key_mode = 0; // 0 = hold, 1 = toggle
        bool        auto_peek = false;
        int         auto_peek_key = VK_XBUTTON1;
        int         auto_peek_mode = 0; // 0 = hold, 1 = toggle
        box_color_t auto_peek_color{ 255, 32, 48, 200 };
        float       auto_peek_max_distance = 200.f; // never walk back farther than this (units)
        antiaim_t   antiaim;
        fakelag_t   fakelag;
        fake_duck_t fake_duck;
        fake_ping_t fake_ping;
        doubletap_t doubletap;
        crosshair_t crosshair;
        viewmodel_t          viewmodel;
        grenade_prediction_t grenade_prediction;
        clantag_t            clantag;
        killsay_t            killsay;
        voice_kill_t         voice_kill;
    };

    // ----------------------------------------------------------
    //  Aimbot settings
    // ----------------------------------------------------------
    struct aimbot_t
    {
        bool  enabled = false;
        bool  on_key  = true;
        int   key     = 0x02;   // VK_RBUTTON (Right Click)
        int   key_mode = 0;     // Binds::Mode
        int   bone    = 0;      // 0=head, 1=neck, 2=chest, 3=stomach
        int   bone_index = 0;   // 0=auto head detection, >0=manual bone index
        float fov     = 5.0f;
        bool  silent  = true;
        // --- rage mode ---
        bool  rage           = false;
        bool  auto_fire      = false;
        bool  auto_wall      = false;
        int   min_damage     = 20;
        float wall_min_dmg   = 10.f;
        int   hitgroup_mask = HITGROUP_BIT_HEAD | HITGROUP_BIT_CHEST | HITGROUP_BIT_STOMACH; // which HITGROUP_* to aim at (multiple allowed)
        // 0 = off, 1/2 = Supremacy pending-stop. Both enabled values use the
        // same exact 34%-of-weapon-max-speed accuracy gate.
        int   auto_stop        = 1;
        bool  auto_stop_on_key = false;
        int   auto_stop_key    = VK_LSHIFT;
        int   auto_stop_key_mode = 0;
        float auto_stop_slide_speed = 85.f; // legacy persisted field; unused
        float multipoint_scale = 100.f; // hitbox radius scale for multipoint aimpoints
        float stomach_scale    = 100.f; // separate scale for stomach (Pandora)
        bool  multipoint_head    = true;
        bool  multipoint_chest   = true;
        bool  multipoint_stomach = true;
        bool  multipoint_legs    = true;
        bool  multipoint_feet    = true;
        float hit_chance       = 80.f;  // minimum hit chance % to fire
        bool  auto_scope       = true;  // scope in (IN_ATTACK2) only on zoomable weapons while waiting for hit chance
        // Auto-revolver: starts an R8 charge only for a selected valid target
        // and holds it for the weapon's primary cycle before one release tick.
        bool  auto_revolver    = false;
        // Partial cock: produces the R8 wind-up click but always releases before
        // the primary cycle ends, so this mode intentionally never fires.
        bool  auto_revolver_partial_cock = false;

        // Uses the existing game-thread simulation-time history. The rewind
        // request is capped at 12 ticks (187.5 ms) below the observed server
        // 0.2-second unlag acceptance window; no new entity rewrites occur.
        bool  backtrack        = true;
        int   backtrack_ticks  = 12;
        // Test-only: Rage may select only an historical record, never a live
        // snapshot. Disabled by default; intended solely to verify rewind ticks.
        bool  backtrack_only_test = false;

        // New rage policy. Legacy global fields above are retained for old
        // configs and legit mode; RunRage resolves an active weapon profile.
        rage_profiles_t weapon_profiles;
    };

    // ----------------------------------------------------------
    //  Triggerbot settings
    // ----------------------------------------------------------
    struct triggerbot_t
    {
        bool  enabled   = false;
        bool  on_key    = false;
        int   key       = 0x06; // VK_XBUTTON2 (Mouse5)
        int   key_mode  = 0;
        int   delay_ms  = 0;    // shot delay in ms
        bool  smoke_check = false;
        bool  flash_check = false;
        int   hitgroup   = 0;   // 0=any, 1=head, 2=chest, 3=stomach
    };

    // ----------------------------------------------------------
    //  Aggregate visual settings
    // ----------------------------------------------------------
    struct visuals_t
    {
        esp_t     esp;
        chams_t   chams;
        glow_t    glow;
        dlights_t dlights;
        others_t  others;
    };

    // ----------------------------------------------------------
    //  Top-level settings
    // ----------------------------------------------------------
    struct settings_t
    {
        visuals_t            visuals;
        misc_t               misc;
        aimbot_t             aimbot;
        triggerbot_t         triggerbot;
        std::string          config_name;
        std::vector<std::string> config_list{ "default" };
        int                  config_id = 0;
    };

} // namespace cfg

// ------------------------------------------------------------------
//  config_wrapper_t — save / load / refresh
// ------------------------------------------------------------------
struct config_wrapper_t
{
    cfg::settings_t settings;

    void reset(); // restore defaults in memory; never delete or overwrite config files
    void save( const std::string& name );
    void load( const std::string& name );
    void remove( const std::string& name );
    void refresh();
};

// Global config instance (defined in Config.cpp)
inline config_wrapper_t g_config;

// ------------------------------------------------------------------
//  sdk_t — unload flag shared between menu and EntryPoint
// ------------------------------------------------------------------
struct sdk_t
{
    bool unhook = false;
};

inline sdk_t g_sdk;
