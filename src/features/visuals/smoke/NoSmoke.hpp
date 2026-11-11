#pragma once

// No Smoke without MaterialSystem: hooks the two C_ParticleSmokeGrenade
// functions that produce the visible smoke (client.dll, reversed RVAs).
namespace NoSmoke
{
    bool Init();
    void Shutdown();
    // Called every frame (FeatureDispatcher::OnUpdate): clears the fog overlay alpha.
    void Update();
}
