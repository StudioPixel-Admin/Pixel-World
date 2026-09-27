using System.Runtime.InteropServices;
using PixelWorld.Scripts;

static void Check(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static ScriptState NewState() => new()
{
    DeltaTime = 0.25f, TimeOfDay = 9, Health = 80, Stamina = 100, Hunger = 100
};

Check(Marshal.SizeOf<ScriptState>() == 40, "Native ABI must remain 40 bytes.");
Check(Marshal.OffsetOf<ScriptState>(nameof(ScriptState.MovementSpeed)).ToInt32() == 32,
      "Native ABI field offsets must match.");

// Exercise the actual unmanaged entry point, including its ABI size guard.
unsafe
{
    delegate* unmanaged[Cdecl]<ScriptState*, int, int> update = &WorldScript.Update;
    ScriptState state = NewState();
    Check(update(&state, 40) == 0 && state.TimeOfDay > 9, "Unmanaged C# update must execute.");
    float before = state.TimeOfDay;
    Check(update(&state, 39) == -1 && state.TimeOfDay == before, "Reject incompatible native layout.");
    Check(update(null, 40) == -1, "Reject a null native pointer.");
}

var sprint = NewState();
sprint.Sprinting = 1;
for (int i = 0; i < 24; ++i) WorldScript.Step(ref sprint);
Check(sprint.Stamina == 0 && sprint.MovementSpeed < 5, "Sprinting must exhaust stamina.");
sprint.Sprinting = 0;
for (int i = 0; i < 32; ++i) WorldScript.Step(ref sprint);
Check(sprint.Stamina == 100 && sprint.MovementSpeed == 5, "Rest must restore stamina.");

var drowning = NewState();
drowning.Underwater = 1;
WorldScript.Step(ref drowning);
Check(drowning.Health < 80 && drowning.MovementSpeed == 3, "Underwater movement and damage must apply.");
var starving = NewState();
starving.Hunger = 0;
WorldScript.Step(ref starving);
Check(starving.Health < 80, "Starvation must cause damage.");

var midnight = NewState();
midnight.TimeOfDay = 23.999f;
WorldScript.Step(ref midnight);
Check(midnight.TimeOfDay >= 0 && midnight.TimeOfDay < 1, "Time must wrap at midnight.");
var rain = NewState();
rain.TimeOfDay = 14.5f;
WorldScript.Step(ref rain);
Check(rain.Weather == 2, "Afternoon weather must resume from saved time.");
var restored = rain;
WorldScript.Step(ref rain);
WorldScript.Step(ref restored);
Check(rain.TimeOfDay == restored.TimeOfDay && rain.Weather == restored.Weather &&
      rain.Stamina == restored.Stamina && rain.Health == restored.Health,
      "The same saved state must reproduce the same tick.");

var malformed = NewState();
malformed.DeltaTime = float.PositiveInfinity;
malformed.TimeOfDay = float.NaN;
malformed.Health = -100;
WorldScript.Step(ref malformed);
Check(float.IsFinite(malformed.TimeOfDay) && malformed.Health >= 0 && malformed.Health <= 100,
      "Malformed state must stay bounded and finite.");

Console.WriteLine("C# scripting checks passed: ABI, unmanaged entry, stamina, survival, clock, deterministic restore.");
