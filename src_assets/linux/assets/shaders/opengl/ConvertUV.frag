#version 300 es

#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D image;
uniform sampler2D output_lut;

layout(shared) uniform ColorMatrix {
  vec4 color_vec_y;
  vec4 color_vec_u;
  vec4 color_vec_v;
  vec2 range_y;
  vec2 range_uv;
};

in vec3 uuv;
layout(location = 0) out vec2 color;

float output_lut_coordinate(float value)
{
  float size = float(textureSize(output_lut, 0).x);
  return (clamp(value, 0.0, 1.0) * (size - 1.0) + 0.5) / size;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
void main() {
  vec3 rgb_left  = texture(image, uuv.xz).rgb;
  vec3 rgb_right = texture(image, uuv.yz).rgb;
  rgb_left = vec3(
    texture(output_lut, vec2(output_lut_coordinate(rgb_left.r), 0.5)).r,
    texture(output_lut, vec2(output_lut_coordinate(rgb_left.g), 0.5)).g,
    texture(output_lut, vec2(output_lut_coordinate(rgb_left.b), 0.5)).b
  );
  rgb_right = vec3(
    texture(output_lut, vec2(output_lut_coordinate(rgb_right.r), 0.5)).r,
    texture(output_lut, vec2(output_lut_coordinate(rgb_right.g), 0.5)).g,
    texture(output_lut, vec2(output_lut_coordinate(rgb_right.b), 0.5)).b
  );
  vec3 rgb       = (rgb_left + rgb_right) * 0.5;

  float u = dot(color_vec_u.xyz, rgb) + color_vec_u.w;
  float v = dot(color_vec_v.xyz, rgb) + color_vec_v.w;

  u = u * range_uv.x + range_uv.y;
  v = v * range_uv.x + range_uv.y;

  color = vec2(u, v);
}
