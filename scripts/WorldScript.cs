using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace PixelWorld.Scripts;

// An explicit four-byte layout matches pixel::ScriptState on every supported
// architecture. Never add references/bools/strings without redesigning the ABI.
[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct ScriptState
{
    public float DeltaTime;
    public float TimeOfDay;
    public float Health;
    public float Stamina;
    public float Hunger;
    public float Temperature;
    public int Sprinting;
    public int Underwater;
    public float MovementSpeed;
    public int Weather;
}

public static class WorldScript
{
    // Native C++ resolves this method through hostfxr exactly once. There is no
    // reflection, marshalling, allocation or subprocess work in the tick loop.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static unsafe int Update(ScriptState* state, int size)
    {
        if (state == null || size != sizeof(ScriptState)) return -1;
        try
        {
            Step(ref *state);
            return 0;
        }
        catch
        {
            // Managed exceptions cannot cross an unmanaged call boundary safely.
            // The native host reports this failure and switches to its fallback.
            return -2;
        }
    }

    // Add game rules here, then rebuild scripts and restart the game. Keep all
    // persistent state in ScriptState so save/load never loses hidden counters.
    // Physics and rendering stay native; this function owns gameplay decisions.
    public static void Step(ref ScriptState state)
    {
        float dt = Math.Clamp(FiniteOr(state.DeltaTime, 0), 0f, 0.25f);
        state.TimeOfDay = (FiniteOr(state.TimeOfDay, 9) + dt * 0.02f) % 24f;
        if (state.TimeOfDay < 0) state.TimeOfDay += 24;
        state.Stamina = Math.Clamp(FiniteOr(state.Stamina, 100), 0f, 100f);
        state.Hunger = Math.Clamp(FiniteOr(state.Hunger, 100), 0f, 100f);
        state.Health = Math.Clamp(FiniteOr(state.Health, 100), 0f, 100f);

        bool sprint = state.Sprinting != 0 && state.Underwater == 0;
        // Holding sprint while exhausted does not repeatedly recover and drain.
        state.Stamina = Math.Clamp(state.Stamina + (sprint ? -18f : 13f) * dt, 0f, 100f);
        state.MovementSpeed = state.Underwater != 0 ? 3f :
            sprint && state.Stamina >= 1 ? 8f : state.Stamina < 1 ? 3.5f : 5f;
        state.Hunger = Math.Max(0f, state.Hunger - dt * (sprint ? 0.065f : 0.035f));

        // A 20-minute day. Weather is derived from saved time rather than an
        // unsaved random generator, giving exact weather continuity after load.
        state.Weather = state.TimeOfDay >= 14 && state.TimeOfDay < 16 ? 2 :
            state.TimeOfDay >= 4 && state.TimeOfDay < 7 ? 1 : 0;
        const float tau = 6.2831853071795864769f;
        state.Temperature = 14 + 8 * MathF.Sin((state.TimeOfDay - 6) * (tau / 24)) -
                            (state.Weather == 2 ? 4f : 0f);
        float damage = (state.Hunger <= 0 ? 0.9f : 0f) + (state.Underwater != 0 ? 4f : 0f);
        float regeneration = state.Hunger > 60 && state.Underwater == 0 ? 0.6f : 0f;
        state.Health = Math.Clamp(state.Health + (regeneration - damage) * dt, 0f, 100f);
    }

    private static float FiniteOr(float value, float fallback) => float.IsFinite(value) ? value : fallback;
}
