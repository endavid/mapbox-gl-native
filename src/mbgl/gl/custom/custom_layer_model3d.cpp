#include <mbgl/gl/custom/custom_layer_model3d.hpp>
#include <iostream>

using namespace mbgl;
using namespace mbgl::style;
using namespace mbgl::platform;

namespace {

template<typename T, size_t N>
std::ostream& operator<<(std::ostream& os, const std::array<T, N>& arr) {
    os << "[ ";
    for(const auto& element : arr) {
        os << element << ", ";
    }
    os << "]";
    return os;
}

static const GLchar* vertexShaderSource = R"MBGL_SHADER(
attribute vec2 a_pos;
void main() {
    gl_Position = vec4(a_pos, 1, 1);
}
)MBGL_SHADER";

static const GLchar* fragmentShaderSource = R"MBGL_SHADER(
void main() {
    gl_FragColor = vec4(0, 0.5, 0, 0.5);
}
)MBGL_SHADER";

} // anonymous

Model3DLayer::Model3DLayer(const std::vector<ModelDescriptor>& modelList)
{
  for (const auto& m : modelList) {
    std::cout << m.id << ": position " << m.position << ", scale " << m.scale << ", " << m.url << std::endl;
  }
}

void Model3DLayer::initialize() {
  program = MBGL_CHECK_ERROR(glCreateProgram());
  vertexShader = MBGL_CHECK_ERROR(glCreateShader(GL_VERTEX_SHADER));
  fragmentShader = MBGL_CHECK_ERROR(glCreateShader(GL_FRAGMENT_SHADER));

  MBGL_CHECK_ERROR(glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr));
  MBGL_CHECK_ERROR(glCompileShader(vertexShader));
  MBGL_CHECK_ERROR(glAttachShader(program, vertexShader));
  MBGL_CHECK_ERROR(glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr));
  MBGL_CHECK_ERROR(glCompileShader(fragmentShader));
  MBGL_CHECK_ERROR(glAttachShader(program, fragmentShader));
  MBGL_CHECK_ERROR(glLinkProgram(program));
  a_pos = MBGL_CHECK_ERROR(glGetAttribLocation(program, "a_pos"));

  GLfloat triangle[] = { 0, 0.5, 0.5, -0.5, -0.5, -0.5 };
  MBGL_CHECK_ERROR(glGenBuffers(1, &buffer));
  MBGL_CHECK_ERROR(glBindBuffer(GL_ARRAY_BUFFER, buffer));
  MBGL_CHECK_ERROR(glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(GLfloat), triangle, GL_STATIC_DRAW));
}
  
void Model3DLayer::render(const 
mbgl::style::CustomLayerRenderParameters& param) {
  MBGL_CHECK_ERROR(glUseProgram(program));
  MBGL_CHECK_ERROR(glBindBuffer(GL_ARRAY_BUFFER, buffer));
  MBGL_CHECK_ERROR(glEnableVertexAttribArray(a_pos));
  MBGL_CHECK_ERROR(glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
  MBGL_CHECK_ERROR(glDrawArrays(GL_TRIANGLE_STRIP, 0, 3));
}

void Model3DLayer::contextLost() {

}

void Model3DLayer::deinitialize() {
  if (program) {
        MBGL_CHECK_ERROR(glDeleteBuffers(1, &buffer));
        MBGL_CHECK_ERROR(glDetachShader(program, vertexShader));
        MBGL_CHECK_ERROR(glDetachShader(program, fragmentShader));
        MBGL_CHECK_ERROR(glDeleteShader(vertexShader));
        MBGL_CHECK_ERROR(glDeleteShader(fragmentShader));
        MBGL_CHECK_ERROR(glDeleteProgram(program));
    }
}
