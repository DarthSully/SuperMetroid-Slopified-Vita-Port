// Lightweight CRT look: scanlines + a coarse RGB aperture-grille mask +
// a soft vignette. Deliberately no curvature/distortion -- keeps every
// output pixel address identical to the input, so it stays cheap and safe
// (no sampling outside [0,1], no divide-by-zero). Single pass; see
// scanlines.glsl for the preset-format notes.

#if defined(VERTEX)

in vec2 VertexCoord;
in vec2 TexCoord;
out vec2 vTexCoord;
uniform mat4 MVPMatrix;

void main() {
  gl_Position = MVPMatrix * vec4(VertexCoord, 0.0, 1.0);
  vTexCoord = TexCoord;
}

#elif defined(FRAGMENT)

#pragma parameter CRT_SCANLINE_STRENGTH "CRT Scanline Strength" 0.78 0.0 1.0 0.02
#pragma parameter CRT_MASK_STRENGTH "CRT Mask Strength" 0.25 0.0 1.0 0.05
#pragma parameter CRT_VIGNETTE_STRENGTH "CRT Vignette Strength" 0.35 0.0 1.0 0.05

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D Texture;
uniform float CRT_SCANLINE_STRENGTH;
uniform float CRT_MASK_STRENGTH;
uniform float CRT_VIGNETTE_STRENGTH;

void main() {
  vec4 color = texture(Texture, vTexCoord);

  // One dark output row in every two.
  float row = floor(gl_FragCoord.y);
  float scan = mix(1.0, CRT_SCANLINE_STRENGTH, mod(row, 2.0));

  // Coarse RGB aperture-grille mask across output columns.
  float col = mod(floor(gl_FragCoord.x), 3.0);
  vec3 mask;
  if (col < 1.0)
    mask = vec3(1.0, 1.0 - CRT_MASK_STRENGTH, 1.0 - CRT_MASK_STRENGTH);
  else if (col < 2.0)
    mask = vec3(1.0 - CRT_MASK_STRENGTH, 1.0, 1.0 - CRT_MASK_STRENGTH);
  else
    mask = vec3(1.0 - CRT_MASK_STRENGTH, 1.0 - CRT_MASK_STRENGTH, 1.0);

  // Soft vignette toward the edges.
  vec2 uv = vTexCoord * 2.0 - 1.0;
  float vig = 1.0 - CRT_VIGNETTE_STRENGTH * dot(uv, uv) * 0.5;

  FragColor = vec4(color.rgb * scan * mask * vig, color.a);
}

#endif
