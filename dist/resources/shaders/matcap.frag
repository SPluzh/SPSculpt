#version 300 es
precision highp float;

uniform sampler2D uTexture0;
uniform vec3 uAlbedo;
uniform float uAlpha;
uniform int uUseTexture;
uniform bool uShowPolyGroups;

in vec3 vVertex;
in vec3 vVertexPres;
in vec3 vNormal;
in vec3 vColor;
in float vMasking;
flat in uint vFaceGroup;

out vec4 fragColor;

#include "common.glsl"

vec3 groupColor(uint gid) {
    if (gid == 0u) return vec3(0.72, 0.52, 0.45);
    uint n = (gid * 1664525u + 1013904223u);
    float r = float(n & 0xFFu) / 255.0;
    float g = float((n >> 8u) & 0xFFu) / 255.0;
    float b = float((n >> 16u) & 0xFFu) / 255.0;
    return mix(vec3(r, g, b), vec3(1.0), 0.15);
}

void main() {
    vec3 normal = getNormal();
    vec3 color;
    vec3 baseColor = (uAlbedo.r >= 0.0) ? uAlbedo : ((vColor.r > 0.0 || vColor.g > 0.0 || vColor.b > 0.0) ? vColor : vec3(0.72, 0.52, 0.45));
    if (uShowPolyGroups) {
        baseColor = groupColor(vFaceGroup);
    }
    if (uUseTexture == 1) {
        vec3 nm_z = normalize(vVertexPres);
        vec3 nm_x = vec3(-nm_z.z, 0.0, nm_z.x);
        vec3 nm_y = cross(nm_x, nm_z);
        vec2 texCoord = 0.5 + 0.5 * vec2(dot(normal, nm_x), dot(normal, nm_y));
        vec3 matcapColor = texture(uTexture0, texCoord).rgb;
        if (uShowPolyGroups) {
            color = mix(matcapColor * baseColor, baseColor, 0.5);
        } else {
            color = matcapColor * baseColor;
        }
        color = sRGBToLinear(color);
    } else {
        vec3 r = reflect(normalize(vVertex), normal);
        float m = 2.0 * sqrt(r.x*r.x + r.y*r.y + (r.z+1.0)*(r.z+1.0));
        float diffuse = max(dot(normal, vec3(0.5, 0.8, 1.0)), 0.0);
        vec3 lightColor = vec3(0.9, 0.85, 0.8) * diffuse + vec3(0.18, 0.18, 0.22);
        color = baseColor * lightColor;
        float spec = pow(max(dot(r, vec3(0.5, 0.8, 1.0)), 0.0), 16.0);
        color += vec3(0.15) * spec;
        color = sRGBToLinear(color);
    }
    fragColor = encodeFragColor(color, uAlpha);
}
