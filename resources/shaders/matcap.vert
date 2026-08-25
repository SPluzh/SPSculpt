#version 300 es
layout(location = 0) in vec3 aVertex;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec3 aMaterial;
layout(location = 4) in vec2 aTexCoord;
layout(location = 5) in uint aFaceGroup;
layout(location = 6) in vec4 aTangent;

uniform mat4 uMV;
uniform mat4 uMVP;
uniform mat3 uN;
uniform mat4 uEM;
uniform mat3 uEN;

out vec3 vVertex;
out vec3 vNormal;
out vec3 vColor;
out float vMasking;
out vec3 vVertexPres;
flat out uint vFaceGroup;
out vec2 vTexCoord;
out vec3 vTangent;
out vec3 vBitangent;

void main() {
    vColor = aColor;
    vMasking = aMaterial.z;
    vFaceGroup = aFaceGroup;
    vNormal = mix(aNormal, uEN * aNormal, vMasking);
    vNormal = normalize(uN * vNormal);
    vTexCoord = aTexCoord;

    vec3 tIn = aTangent.xyz;
    if (dot(tIn, tIn) < 0.001) {
        vec3 c1 = cross(vNormal, vec3(0.0, 0.0, 1.0));
        vec3 c2 = cross(vNormal, vec3(0.0, 1.0, 0.0));
        tIn = (length(c1) > length(c2)) ? c1 : c2;
    }
    vec3 T = normalize(uN * tIn);
    float hand = (aTangent.w == 0.0) ? 1.0 : aTangent.w;
    vec3 B = cross(vNormal, T) * hand;
    vTangent = T;
    vBitangent = B;

    vec4 vertex4 = vec4(aVertex, 1.0);
    vertex4 = mix(vertex4, uEM * vertex4, vMasking);
    vVertex = vec3(uMV * vertex4);
    vVertexPres = vVertex / max(1.0, abs(uMV[3][2]));
    gl_Position = uMVP * vertex4;
}

