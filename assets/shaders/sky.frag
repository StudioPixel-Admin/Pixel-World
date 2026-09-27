#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform Scene {
    mat4 viewProjection;
    vec4 cameraTime;
    vec4 sunFog;
    vec4 forwardAspect;
    vec4 rightTan;
} scene;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
void main() {
    vec3 forward = scene.forwardAspect.xyz, right = scene.rightTan.xyz;
    vec3 up = normalize(cross(right,forward));
    vec3 ray = normalize(forward + (right * uv.x * scene.forwardAspect.w - up * uv.y) * scene.rightTan.w);
    vec3 sun = normalize(scene.sunFog.xyz);
    float daylight = smoothstep(-0.18,0.22,sun.y);
    float h = pow(clamp(ray.y,0.0,1.0),0.55);
    vec3 horizon = mix(vec3(0.018,0.024,0.047), vec3(0.66,0.78,0.81),daylight);
    vec3 zenith = mix(vec3(0.004,0.009,0.027), vec3(0.14,0.37,0.56),daylight);
    float dusk = (1.0-smoothstep(0.0,0.38,abs(sun.y))) * daylight;
    horizon = mix(horizon,vec3(0.86,0.49,0.28),dusk*0.42);
    vec3 color = mix(horizon,zenith,h);
    float sunDot = max(dot(ray,sun),0.0);
    color += vec3(1.0,0.73,0.37) * pow(sunDot,24.0) * 0.13 * daylight;
    color = mix(color,vec3(1.0,0.93,0.71),smoothstep(0.99958,0.99984,sunDot) * daylight);
    // Two inexpensive octave layers make broad clouds with no texture downloads.
    if (ray.y > 0.02) {
        vec2 p = ray.xz / max(ray.y,0.05) * 1.6 + vec2(scene.cameraTime.w * 9.0,0);
        float clouds = noise(p) * 0.66 + noise(p * 2.7) * 0.34;
        float cover = smoothstep(0.54,0.77,clouds) * smoothstep(0.02,0.2,ray.y);
        vec3 cloudColor = mix(vec3(0.10,0.13,0.20),vec3(0.91,0.91,0.85),daylight);
        color = mix(color,cloudColor,cover*0.76);
        float star = step(0.9987,hash(floor(ray.xz / (ray.y+0.4) * 650.0)));
        color += star * (1.0-daylight) * smoothstep(0.0,0.3,ray.y) * 0.5;
    }
    outColor = vec4(color,1.0);
}
