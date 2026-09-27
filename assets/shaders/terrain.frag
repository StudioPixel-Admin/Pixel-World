#version 450
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 tint;
layout(location = 3) flat in int material;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform Scene {
    mat4 viewProjection;
    vec4 cameraTime;
    vec4 sunFog;
    vec4 forwardAspect;
    vec4 rightTan;
} scene;

float hash(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
float noise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i), hash(i + vec3(1,0,0)), f.x),
                   mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), f.x),
                   mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), f.x), f.y), f.z);
}
vec3 skyColor(vec3 ray, float daylight) {
    float h = pow(clamp(ray.y, 0.0, 1.0), 0.55);
    vec3 horizon = mix(vec3(0.018,0.024,0.047), vec3(0.66,0.78,0.81), daylight);
    vec3 zenith = mix(vec3(0.004,0.009,0.027), vec3(0.14,0.37,0.56), daylight);
    float dusk = (1.0 - smoothstep(0.0, 0.38, abs(scene.sunFog.y))) * daylight;
    horizon = mix(horizon, vec3(0.86,0.49,0.28), dusk * 0.42);
    return mix(horizon, zenith, h);
}
void main() {
    vec3 n = normalize(normal);
    // Volumetric material coordinates cross chunk boundaries without UV seams.
    // Fine grain fades with distance to avoid shimmer on economical resolutions.
    float distanceToCamera = length(worldPosition - scene.cameraTime.xyz);
    float broad = noise(worldPosition * 0.24);
    float grain = mix(noise(worldPosition * 3.6), 0.5, smoothstep(18.0, 70.0, distanceToCamera));
    vec3 base;
    if (material == 0) {
        base = mix(vec3(0.15,0.235,0.073), vec3(0.34,0.40,0.14), broad);
        base = mix(vec3(0.285,0.225,0.155), base, smoothstep(0.40,0.78,n.y));
    } else if (material == 1) {
        float layers = smoothstep(0.43,0.56, fract(worldPosition.y * 0.32 + broad * 0.26));
        base = mix(vec3(0.29,0.30,0.28), vec3(0.45,0.445,0.39), broad) * mix(0.86,1.03,layers);
    } else if (material == 2) {
        base = mix(vec3(0.53,0.43,0.26), vec3(0.71,0.62,0.41), broad);
    } else if (material == 3) {
        base = mix(vec3(0.71,0.79,0.80), vec3(0.91,0.92,0.85), broad);
    } else if (material == 4) {
        float bark = noise(vec3(worldPosition.x * 3.0,worldPosition.y * 0.22,worldPosition.z * 3.0));
        base = mix(vec3(0.17,0.10,0.054),vec3(0.32,0.22,0.12),bark);
    } else if (material == 5) {
        base = mix(vec3(0.08,0.17,0.065),vec3(0.26,0.34,0.10),broad);
    } else {
        base = mix(vec3(0.07,0.25,0.29), vec3(0.17,0.37,0.36), broad);
    }
    base *= (0.89 + grain * 0.22) * tint;
    float daylight = smoothstep(-0.18, 0.22, scene.sunFog.y);
    vec3 lightDirection = normalize(scene.sunFog.xyz);
    float diffuse = max(dot(n, lightDirection), 0.0);
    float hemisphere = mix(0.43, 1.0, clamp(n.y * 0.5 + 0.5,0.0,1.0));
    vec3 ambient = mix(vec3(0.105,0.14,0.22),vec3(0.42,0.49,0.50),daylight) * hemisphere;
    vec3 sunlight = mix(vec3(1.0,0.66,0.39),vec3(1.0,0.95,0.79),smoothstep(0.0,0.6,scene.sunFog.y));
    vec3 color = base * (ambient + sunlight * diffuse * daylight * 0.82);
    vec3 ray = normalize(worldPosition - scene.cameraTime.xyz);
    if (material == 6) {
        float fresnel = pow(1.0 - max(dot(n,-ray),0.0), 3.0);
        color = mix(color, skyColor(reflect(ray,n),daylight), 0.33 + fresnel * 0.45);
        float sparkle = pow(max(dot(reflect(-lightDirection,n),-ray),0.0),80.0);
        color += sunlight * sparkle * daylight * 0.5;
    }
    float fog = 1.0 - exp(-pow(distanceToCamera / max(scene.sunFog.w * 0.74,1.0),2.6));
    color = mix(color, skyColor(ray,daylight), clamp(fog,0.0,1.0));
    outColor = vec4(color,1.0);
}
