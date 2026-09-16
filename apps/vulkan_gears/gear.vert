#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(push_constant) uniform Transform {
    mat4 mvp;
    mat4 model;
} transform;

layout(location = 0) out vec3 normal;
layout(location = 1) out vec3 color;

void main() {
    gl_Position = transform.mvp * vec4(inPosition, 1.0);
    normal = normalize(mat3(transform.model) * inNormal);
    color = inColor;
}
