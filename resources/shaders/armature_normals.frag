#version 300 es
precision highp float;
in vec3 vVertex;
in vec3 vVertexPres;
in vec3 vNormal;
out vec4 fragColor;
void main() {
  vec3 N = normalize(vNormal);
  vec3 flatNormal = -normalize(cross(dFdy(vVertex), dFdx(vVertex)));
  vec3 normal = dot(N, flatNormal) < 0.0 ? -N : N;
  fragColor = vec4(normal * 0.5 + 0.5, 1.0);
}
