#include <mbgl/gl/custom/custom_layer_model3d.hpp>
#include <mbgl/storage/file_source.hpp>
#include <mbgl/storage/http_file_source.hpp>
#include <mbgl/storage/main_resource_loader.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/util/projection.hpp>
#include <mbgl/util/run_loop.hpp>
#include <mbgl/util/io.hpp>
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

double mercatorXfromLng(double lng) {
    return (180.0 + lng) / 360.0;
}

double mercatorYfromLat(double lat) {
    return (180.0 - (180.0 / M_PI * std::log(std::tan(M_PI_4 + lat * M_PI / 360.0)))) / 360.0;
}

vec3 toMercator(const LatLng& location, double altitudeMeters, double zoom) {
    const double pixelsPerMeter = 1.0 / Projection::getMetersPerPixelAtLatitude(location.latitude(), 0.0);
    const double worldSize = Projection::worldSize(std::pow(2.0, zoom));

    return {{mercatorXfromLng(location.longitude()),
             mercatorYfromLat(location.latitude()),
             altitudeMeters * pixelsPerMeter / worldSize}};
}

vec4 multiply(const std::array<double, 16>& m, const vec3& p) {
  // I couldn't find the operation in matrix.hpp ...
  return vec4({
    p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12],
    p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
    p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14],
    p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15]
  });
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
: modelList(std::move(modelList))
{
  for (const auto& m : modelList) {
    std::string url(m.url);
    if (url.compare(0, 7, "file://") == 0) {
      url.erase(0, 7);
      std::string json = util::read_file(url);
      std::cout << json << std::endl;
    } else {
      std::cerr << "Reading models from the network is still not supported: " << url << std::endl;
    }
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
  double worldSize = Projection::worldSize(std::pow(2, param.zoom));
  MBGL_CHECK_ERROR(glUseProgram(program));
  MBGL_CHECK_ERROR(glBindBuffer(GL_ARRAY_BUFFER, buffer));
  MBGL_CHECK_ERROR(glEnableVertexAttribArray(a_pos));
  MBGL_CHECK_ERROR(glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
  MBGL_CHECK_ERROR(glDrawArrays(GL_TRIANGLE_STRIP, 0, 3));
  for (const auto& m : modelList) {
    LatLng ll(m.position[0], m.position[1]);
    double altitude = m.position[2];
    vec3 p = toMercator(ll, altitude, param.zoom);
    p[0] *= worldSize;
    p[1] *= worldSize;
    p[2] *= worldSize;
    vec4 pm = multiply(param.projectionMatrix, p);
    pm[0] /= pm[3];
    pm[1] /= pm[3];
    pm[2] /= pm[3];
    pm[3] = 1.0;
    if (abs(pm[0]) > 2.0 || abs(pm[1]) > 2.0) {
      // Very simple culling test for now, without considering the bounding box of the model.
      // Don't render if center is outside clipping area.
      continue;
    }
    std::cout << pm << std::endl;
  }
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
