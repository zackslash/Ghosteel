// Kitty Graphics Protocol — textured quad shader
// Vertex format: pos(2) + texcoord(2) = 4 floats per vertex
// Blending: GL_ONE, GL_ONE_MINUS_SRC_ALPHA (premultiplied alpha)

//! vertex
attribute vec2 position;
attribute vec2 texcoord;
uniform mat4 u_matrix;
varying vec2 v_texcoord;
void main() {
    gl_Position = u_matrix * vec4(position, 0.0, 1.0);
    v_texcoord = texcoord;
}

//! fragment
precision mediump float;
varying vec2 v_texcoord;
uniform sampler2D u_image;
void main() {
    vec4 color = texture2D(u_image, v_texcoord);
    // Premultiply alpha to match renderer's blend mode (GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
    gl_FragColor = vec4(color.rgb * color.a, color.a);
}
