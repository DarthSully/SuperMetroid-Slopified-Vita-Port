// Simple horizontal-scanline preset. Single pass; see glsl_shader.c for the
// preset/parameter parser this file is compiled against (GLSL 330 core --
// the profile src/opengl.c already requires via
// SDL_GL_CONTEXT_PROFILE_CORE). No #version line: the loader prepends
// "#version 330\n" (core, since no profile qualifier defaults to core).

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

#pragma parameter SCANLINE_STRENGTH "Scanline Strength" 0.72 0.0 1.0 0.02
#pragma parameter SCANLINE_WIDTH "Scanline Width (px)" 2.0 1.0 4.0 1.0

in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D Texture;
uniform float SCANLINE_STRENGTH;
uniform float SCANLINE_WIDTH;

void main() {
  vec4 color = texture(Texture, vTexCoord);
  float row = floor(gl_FragCoord.y / SCANLINE_WIDTH);
  float dark = mix(1.0, SCANLINE_STRENGTH, mod(row, 2.0));
  FragColor = vec4(color.rgb * dark, color.a);
}

#endif
