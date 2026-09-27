#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inMaterial;
layout(push_constant) uniform Scene {
    mat4 viewProjection;
    vec4 cameraTime;
    vec4 sunFog;
    vec4 forwardAspect;
    vec4 rightTan;
} scene;
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 normal;
layout(location = 2) out vec3 tint;
layout(location = 3) flat out int material;
void main() {
    worldPosition = inPosition;
    normal = inNormal;
    tint = inColor;
    material = int(inMaterial + 0.5);
    gl_Position = scene.viewProjection * vec4(inPosition, 1.0);
}
