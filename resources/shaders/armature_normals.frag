#version 300 es
precision highp float;
in vec3 vVertex;
in vec3 vVertexPres;
in vec3 vNormal;
out vec4 fragColor;
void main() {
  float lenN = length(vNormal);
  vec3 N = (lenN > 1e-4 && !isnan(lenN)) ? normalize(vNormal) : vec3(0.0, 0.0, 1.0);
  vec3 crossN = cross(dFdy(vVertex), dFdx(vVertex));
  float lenCross = length(crossN);
  vec3 flatNormal = lenCross > 1e-10 ? -(crossN / lenCross) : N;
  vec3 normal = dot(N, flatNormal) < 0.0 ? -N : N;
  fragColor = vec4(normal * 0.5 + 0.5, 1.0);
}
