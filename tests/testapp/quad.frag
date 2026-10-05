#version 450
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform Frame {
    vec4 stamp_rect;
    vec4 stamp_color;
    vec4 badge_rect;
    vec4 badge_tint;
} frame;
layout(set = 0, binding = 1) uniform sampler2D tex;

layout(push_constant) uniform Draw {
    int mode;
} draw;

void main()
{
    if (draw.mode == 0)
        out_color = frame.stamp_color;
    else if (draw.mode == 1)
        out_color = texture(tex, in_uv);
    else
        out_color = texture(tex, in_uv) * frame.badge_tint;
}
