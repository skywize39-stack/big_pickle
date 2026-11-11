#pragma once
// ============================================================
//  WeaponIcons.hpp — CS2 gun icon glyph lookup for the ESP.
//
//  Glyphs come from the embedded CS2GunIcons.ttf. Each weapon's
//  icon lives at an ASCII codepoint keyed exactly as below, so
//  drawing the returned letter in that font renders the weapon.
// ============================================================
#include <map>
#include <string>

namespace weapons
{
    // Localiser maps a designer classname (with or without the
    // leading "weapon_" prefix) to its icon glyph.
    inline std::map<std::string, const wchar_t*> gunIcons = {
        { "weapon_p90", L"P" },
        { "weapon_mp9", L"O" },
        // The CS2GunIcons.ttf has no MP5SD glyph; show MP9's icon (O).
        { "weapon_mp5sd", L"O" },
        // In the CS2GunIcons layout 'M' is the PP-Bizon; the M4 rifle glyph is
        // 'S' (shared with weapon_m4a1). Using anything else for the M4A4 makes
        // it render the Bizon icon, so it must map to 'S'.
        { "weapon_m4a4", L"S" },
        { "weapon_knife", L"]" },
        { "weapon_knife_ct", L"]" },
        { "weapon_knife_t", L"]" },
        { "weapon_deagle", L"A" },
        { "weapon_elite", L"B" },
        { "weapon_fiveseven", L"C" },
        { "weapon_glock", L"D" },
        { "weapon_revolver", L"J" },
        { "weapon_hkp2000", L"E" },
        { "weapon_p250", L"F" },
        { "weapon_usp_silencer", L"G" },
        { "weapon_tec9", L"H" },
        { "weapon_cz75a", L"I" },
        { "weapon_mac10", L"K" },
        { "weapon_ump45", L"L" },
        { "weapon_bizon", L"M" },
        { "weapon_mp7", L"N" },
        { "weapon_galilar", L"Q" },
        { "weapon_famas", L"R" },
        { "weapon_m4a1_silencer", L"T" },
        { "weapon_m4a1", L"S" },
        { "weapon_aug", L"U" },
        { "weapon_sg556", L"V" },
        { "weapon_sg553", L"V" },
        // Player's CSSO build reports the SG rifle as "ssg553".
        { "weapon_ssg553", L"V" },
        { "weapon_sg552", L"V" },
        { "weapon_ak47", L"W" },
        { "weapon_g3sg1", L"X" },
        { "weapon_scar20", L"Y" },
        { "weapon_awp", L"Z" },
        { "weapon_ssg08", L"a" },
        { "weapon_xm1014", L"b" },
        { "weapon_sawedoff", L"c" },
        { "weapon_mag7", L"d" },
        { "weapon_nova", L"e" },
        { "weapon_negev", L"f" },
        { "weapon_m249", L"g" },
        { "weapon_taser", L"h" },
        { "weapon_flashbang", L"i" },
        { "weapon_hegrenade", L"j" },
        { "weapon_smokegrenade", L"k" },
        { "weapon_molotov", L"l" },
        { "weapon_decoy", L"m" },
        { "weapon_incgrenade", L"n" },
        // CSSO builds may report the CT incendiary as "weapon_incendiary".
        { "weapon_incendiary", L"n" },
        { "weapon_c4", L"o" },
        { "weapon_bayonet", L"]" },
        { "weapon_knife_survival_bowie", L"]" },
        { "weapon_knife_butterfly", L"]" },
        { "weapon_knife_canis", L"]" },
        { "weapon_knife_cord", L"]" },
        { "weapon_knife_css", L"]" },
        { "weapon_knife_falchion", L"]" },
        { "weapon_knife_flip", L"]" },
        { "weapon_knife_gut", L"]" },
        { "weapon_knife_karambit", L"]" },
        { "weapon_knife_twinblade", L"]" },
        { "weapon_knife_kukri", L"]" },
        { "weapon_knife_m9_bayonet", L"]" },
        { "weapon_knife_outdoor", L"]" },
        { "weapon_knife_push", L"]" },
        { "weapon_knife_skeleton", L"]" },
        { "weapon_knife_stiletto", L"]" },
        { "weapon_knife_tactical", L"]" },
        { "weapon_knife_widowmaker", L"]" },
        { "weapon_knife_ursus", L"]" }
    };

    // Normalises a designer classname (tolerates both "weapon_p90" and the bare
    // "p90" form) and returns its icon glyph, or 0 when the weapon has no
    // dedicated glyph in CS2GunIcons.ttf. CSSO builds expose class names with
    // trailing suffixes ("weapon_glock18", "mp5sd" is also used) that do not
    // appear verbatim in the table, so when an exact match fails the longest
    // known weapon whose name is a strict prefix of the input is used instead
    // (e.g. "glock18" -> "glock", "m4a4" -> "m4a1").
    inline wchar_t get( const char* designerName )
    {
        if ( !designerName || !designerName[0] )
            return 0;

        std::string name = designerName;
        const std::string prefix = "weapon_";
        if ( name.compare( 0, prefix.size(), prefix ) == 0 )
            name.erase( 0, prefix.size() );
        if ( name.empty() )
            return 0;

        // 1) Exact match.
        const auto it = gunIcons.find( prefix + name );
        if ( it != gunIcons.end() )
            return *( it->second );

        // 2) Longest known weapon name that prefixes `name` (>=3 chars so short
        //    keys like "c4" never grab unrelated weapons accidentally).
        wchar_t best = 0;
        size_t  bestLen = 0;
        for ( const auto& kv : gunIcons )
        {
            const std::string& key = kv.first;
            if ( key.compare( 0, prefix.size(), prefix ) != 0 )
                continue;
            const std::string stripped = key.substr( prefix.size() );
            if ( stripped.size() < 3 || stripped.size() > name.size() )
                continue;
            if ( name.compare( 0, stripped.size(), stripped ) == 0 &&
                 stripped.size() > bestLen )
            {
                bestLen = stripped.size();
                best = *( kv.second );
            }
        }
        return best;
    }

    inline wchar_t get( const std::string& designerName )
    {
        return get( designerName.c_str() );
    }
}