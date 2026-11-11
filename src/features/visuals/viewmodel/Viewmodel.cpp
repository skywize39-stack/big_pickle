#include "features/visuals/viewmodel/Viewmodel.hpp"
#include "sdk/ConVars.hpp"
#include "config/Config.hpp"

// ============================================================
//  Viewmodel changer — viewmodel_fov / viewmodel_offset_x/y/z / viewmodel_recoil
//
//  The matching CSSO client.dll registers its own change callbacks that clamp
//  FOV to 54..68 and offsets to roughly -2..2.5 *after* ConVar::SetFloat.
//  ClearBounds only disables the vstdlib registered min/max; it cannot defeat
//  that client callback. The client getter at 0x1804C5E90 verifies the actual
//  stored format: uint32(float) XOR uint32(convar-root-address) at +0x54.
//  Therefore write only the final requested value to that confirmed format,
//  after the normal SetFloat path has fired all callbacks. No guessed offsets
//  are used; +0x38 parent resolution and +0x54 XOR storage were read directly
//  from the supplied client.dll.
//
//  IMPORTANT: SetFloat must NOT be called every frame. Each actual write
//  runs ChangeStringValue and callback dispatch. The post-callback encrypted
//  write is likewise done only when the requested value changes or the feature
//  toggles, avoiding viewmodel rebuild hitches.
// ============================================================
static SDK::IConVar* ResolveConVarRoot( SDK::IConVar* convar )
{
    uintptr_t current = reinterpret_cast<uintptr_t>( convar );
    // client.dll helper 0x1804C5E90 follows this same +0x38 parent link until
    // self-parented. Bound the walk defensively in case a foreign object is
    // returned by an unexpected interface implementation.
    __try
    {
        for ( int depth = 0; current && depth < 8; ++depth )
        {
            const uintptr_t parent = *reinterpret_cast<uintptr_t*>( current + 0x38 );
            if ( !parent || parent == current )
                break;
            current = parent;
        }
    }
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
        return nullptr;
    }
    return reinterpret_cast<SDK::IConVar*>( current );
}

static void SetUnlockedViewmodelFloat( SDK::IConVar* convar, float value, bool force )
{
    auto* root = ResolveConVarRoot( convar );
    if ( !root )
        return;

    // Preserve the normal setter/callback notification first. The client
    // Viewmodel callback may clamp it; overwrite only the confirmed encrypted
    // float afterwards, exactly as the client getter decodes it.
    root->ClearBounds();
    root->SetFloat( value, force );

    uint32_t valueBits = 0;
    memcpy( &valueBits, &value, sizeof( valueBits ) );
    const uint32_t encrypted = valueBits ^ static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>( root ) );
    memcpy( reinterpret_cast<void*>( reinterpret_cast<uintptr_t>( root ) + 0x54 ),
            &encrypted, sizeof( encrypted ) );
}

void Viewmodel::Update()
{
    auto& vm = g_config.settings.misc.viewmodel;

    static bool sLastEnabled = false;

    auto* pCvar = SDK::ICvar::Get();
    if ( !pCvar )
        return;

    struct ViewmodelVar
    {
        const char* name;
        float       defaultValue;
        float       lastRequested;
    };
    static ViewmodelVar vars[] = {
        { "viewmodel_fov",        60.f, 60.f },
        { "viewmodel_offset_x",    1.f,  1.f },
        { "viewmodel_offset_y",    1.f,  1.f },
        { "viewmodel_offset_z",   -1.f, -1.f },
        { "viewmodel_recoil",      1.f,  1.f },
    };

    float* settings[] = { &vm.fov, &vm.offset_x, &vm.offset_y, &vm.offset_z, &vm.recoil };

    if ( vm.enabled || sLastEnabled )
    {
        const bool toggle = ( vm.enabled != sLastEnabled );
        for ( int i = 0; i < 5; ++i )
        {
            const float request = vm.enabled ? *settings[ i ] : vars[ i ].defaultValue;
            if ( !toggle && request == vars[ i ].lastRequested )
                continue;

            vars[ i ].lastRequested = request;
            if ( auto* convar = pCvar->FindConVar( vars[ i ].name ) )
                SetUnlockedViewmodelFloat( convar, request, toggle );
        }
    }

    sLastEnabled = vm.enabled;
}

