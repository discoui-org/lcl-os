#version 450

layout(location = 0) in vec3 normal;
layout(location = 1) in vec3 color;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 light = normalize(vec3(-0.35, 0.55, 0.78));
    float diffuse = max(dot(normalize(normal), light), 0.0);
    float lighting = 0.24 + 0.76 * diffuse;
    outColor = vec4(color * lighting, 1.0);
}
