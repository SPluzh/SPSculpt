#version 300 es
precision highp float;

layout(location = 0) in vec3 aVertex;
layout(location = 3) in vec3 aMaterial;

uniform mat4 uLightMVP;
uniform mat4 uEM;

void main() {
    vec4 v = vec4(aVertex, 1.0);
    v = mix(v, uEM * v, aMaterial.z);
    gl_Position = uLightMVP * v;
}
