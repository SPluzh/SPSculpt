#version 300 es
layout(location = 0) in vec3 aVertex;
layout(location = 1) in vec3 aNormal;
layout(location = 3) in vec3 aMaterial;

uniform mat4 uMV;
uniform mat4 uMVP;
uniform mat3 uN;
uniform mat4 uEM;
uniform mat3 uEN;

out vec3 vVertex;
out vec3 vNormal;

void main() {
    float masking = aMaterial.z;
    vec3 localNormal = mix(aNormal, uEN * aNormal, masking);
    vNormal = normalize(uN * localNormal);
    
    vec4 vertex4 = vec4(aVertex, 1.0);
    vertex4 = mix(vertex4, uEM * vertex4, masking);
    vVertex = vec3(uMV * vertex4);
    
    gl_Position = uMVP * vertex4;
}
