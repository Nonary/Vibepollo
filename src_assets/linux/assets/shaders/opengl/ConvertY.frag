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

in vec2 tex;
layout(location = 0) out float color;

float output_lut_coordinate(float value)
{
	float size = float(textureSize(output_lut, 0).x);
	return (clamp(value, 0.0, 1.0) * (size - 1.0) + 0.5) / size;
}

void main()
{
	vec3 rgb = texture(image, tex).rgb;
	rgb = vec3(
		texture(output_lut, vec2(output_lut_coordinate(rgb.r), 0.5)).r,
		texture(output_lut, vec2(output_lut_coordinate(rgb.g), 0.5)).g,
		texture(output_lut, vec2(output_lut_coordinate(rgb.b), 0.5)).b
	);
	float y = dot(color_vec_y.xyz, rgb) + color_vec_y.w;

	color = y * range_y.x + range_y.y;
}
