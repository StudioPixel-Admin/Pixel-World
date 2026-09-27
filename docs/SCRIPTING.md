# C# gameplay scripting

Pixel World renders, streams terrain and simulates collisions in C++. The native
`ScriptHost` starts .NET through `hostfxr`, loads `PixelWorld.Scripts.dll`, and calls
the C# `WorldScript.Update` function once per gameplay tick. The game reports
whether C# is active. If the assembly or runtime is missing, a native copy of the
same rules keeps the game playable and reports the reason in its status.

The integration follows the official [.NET native hosting API](https://learn.microsoft.com/dotnet/core/tutorials/netcore-hosting).
There is no C# process launched per frame. After initialization, the native host
calls one cached unmanaged function pointer with a 40-byte state structure.

## Build and run

Install the .NET 8 SDK to build scripts; only a compatible .NET runtime is needed
on players' machines. The project targets .NET 8 and allows a newer major runtime
when .NET 8 is absent. The normal CMake game build compiles the script project if
the `dotnet` executable is available. To build and test it independently:

```powershell
dotnet build scripts/PixelWorld.Scripts.csproj -c Release
dotnet run --project scripts/tests/PixelWorld.ScriptTests.csproj -c Release
```

Deploy these files together in the script directory supplied to `ScriptHost.load`:

- `PixelWorld.Scripts.dll`
- `PixelWorld.Scripts.runtimeconfig.json`
- `PixelWorld.Scripts.deps.json`

The native host searches `DOTNET_ROOT`, the architecture-specific root variable,
and standard installation directories for `hostfxr`. It sorts installed host
versions numerically. On Windows the default location is `Program Files/dotnet`;
nonstandard installations can set `DOTNET_ROOT`. Native Linux builds link `dl`.

Scripts remain loaded for the lifetime of the process. After editing a script,
rebuild and restart the game. Hot reload is not implemented.

## State contract

`engine/include/pixel/ScriptHost.hpp` and `scripts/WorldScript.cs` declare the same
sequential, four-byte-aligned structure. Fields appear in this exact order:

| Field | Native/managed type | Meaning |
| --- | --- | --- |
| deltaTime / DeltaTime | float | Simulation step in seconds; clamped to 0–0.25. |
| timeOfDay / TimeOfDay | float | Hours in [0,24); one day takes 20 real minutes. |
| health / Health | float | Health in [0,100]. |
| stamina / Stamina | float | Stamina in [0,100]. |
| hunger / Hunger | float | Food reserve in [0,100]; zero causes starvation. |
| temperature / Temperature | float | Ambient temperature in degrees Celsius. |
| sprinting / Sprinting | int32 / int | Nonzero when requesting sprint. |
| underwater / Underwater | int32 / int | Nonzero when submerged. |
| movementSpeed / MovementSpeed | float | Requested movement speed in world units/second. |
| weather / Weather | int32 / int | 0 clear, 1 overcast, 2 rain. |

Native code owns the structure. C# must never retain its pointer or add managed
references such as strings or arrays to it. The boundary checks the structure's
size before reading memory. `Update` catches managed exceptions and returns an
error; the C++ host then restores the original input and uses native rules.

## Game rules and extension

`WorldScript.Step` handles the day clock, sprint stamina, movement-speed policy,
hunger, health regeneration, starvation and underwater health loss. Weather is a
simple deterministic daily schedule derived from the saved clock: overcast from
04:00 to 07:00, rain from 14:00 to 16:00, and clear otherwise. Temperature follows
the clock and weather. It is ambient state, not a complete temperature-survival
simulation. Underwater damage currently starts immediately; a separate oxygen
reserve can be added when the gameplay needs it.

Add rules to `Step` using the existing state. Keep persistent decisions in saved
state rather than static counters, wall-clock time, or an untracked random-number
generator. Additional persisted fields require coordinated updates to the C++
and C# structures, native size/offset assertions, the size checks, save/load, and
the native fallback. Use integer flags instead of native/C# `bool`, whose ABI
representation can differ.

Mirror supported rules in `nativeStep` inside `ScriptHost.cpp`; this keeps saves
playable on a machine where the runtime is unavailable. Avoid heap allocations
inside `Step` so scripting does not add garbage-collection pauses to gameplay.

## Verification

The C# checks exercise the unmanaged entry pointer, reject a wrong structure size
and null pointer, and verify stamina, survival, midnight wrap and deterministic
restoration. `tests/script_host_tests.cpp` loads the actual managed assembly from
C++, runs 5,000 ticks through `hostfxr`, compares results with the native fallback,
and checks missing-script and invalid-state behavior. That test requires a
compatible .NET runtime and receives the compiled script directory as its sole
argument.
