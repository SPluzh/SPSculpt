#version 300 es
precision highp float;
uniform sampler2D uTexture0;
uniform vec3 uColor;
uniform float uSelected;
in vec3 vVertex;
in vec3 vVertexPres;
in vec3 vNormal;
out vec4 fragColor;
vec3 sRGBToLinear(vec3 color) { return pow(color, vec3(2.2)); }
vec4 encodeRGBM(const in vec3 col) {
  vec4 rgbm;
  vec3 color = col / 5.0;
  rgbm.a = clamp(max(max(color.r, color.g), max(color.b, 1e-6)), 0.0, 1.0);
  rgbm.a = ceil(rgbm.a * 255.0) / 255.0;
  rgbm.rgb = color / rgbm.a;
  return rgbm;
}

void main() {
  // Calculate flat normal using screen-space derivatives
  // Note: cross(dFdy, dFdx) provides the correctly inverted normal needed for the matcap mapping
  vec3 normal = -normalize(cross(dFdy(vVertex), dFdx(vVertex)));
  vec3 nm_z = normalize(vVertexPres);
  vec3 nm_x = vec3(-nm_z.z, 0.0, nm_z.x);
  vec3 nm_y = cross(nm_x, nm_z);
  vec2 texCoord = 0.5 + 0.5 * vec2(dot(normal, nm_x), dot(normal, nm_y));
  vec3 matcapColor = sRGBToLinear(texture(uTexture0, texCoord).rgb);
  vec3 baseColor = matcapColor * sRGBToLinear(uColor);
  if (uSelected > 0.5) {
    baseColor = mix(baseColor, vec3(1.0, 1.0, 0.0), 0.3) + vec3(0.2, 0.2, 0.0);
  }
  fragColor = encodeRGBM(baseColor);
}
