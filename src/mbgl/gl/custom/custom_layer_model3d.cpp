#include <mbgl/gl/custom/custom_layer_model3d.hpp>
#include <mbgl/storage/file_source.hpp>
#include <mbgl/storage/http_file_source.hpp>
#include <mbgl/storage/main_resource_loader.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/util/projection.hpp>
#include <mbgl/util/run_loop.hpp>
#include <mbgl/util/io.hpp>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
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

std::string readResource(const std::string& url) {
  if (url.compare(0, 7, "file://") == 0) {
    std::string filename(url);
    filename.erase(0, 7);
    return util::read_file(filename);
  }
  std::cerr << "Reading models from the network is still not supported: " << url << std::endl;
  return "";
}

static const GLchar* vertexShaderSource = R"MBGL_SHADER(
attribute vec3 position;
attribute vec3 normal;
uniform mat4 modelMatrix;
uniform mat4 projectionMatrix;
varying vec4 vertexColor;
void main() {
    // Y is up in my models, but Z is up in mapbox -> xzy
    vec4 worldPos = modelMatrix * vec4(position.xzy, 1.0);
    vec4 projected = projectionMatrix * worldPos;
    gl_Position = projected;
    //gl_Position = vec4(worldPos.xy, 1, 1);
    vertexColor = vec4(0.5 * normal + 0.5, 1);
}
)MBGL_SHADER";

static const GLchar* fragmentShaderSource = R"MBGL_SHADER(
varying vec4 vertexColor;
void main() {
    gl_FragColor = vec4(vertexColor.rgb, 1.0);
}
)MBGL_SHADER";

} // anonymous

namespace mbgl {
namespace style {

class Model3D {
  public:
  Model3D(const std::string& json)
  : name("")
  , vertexByteCount(0)
  , indexByteCount(0)
  , indexCount(0)
  , vertexData(nullptr)
  , faces(nullptr)
  {
    rapidjson::GenericDocument<rapidjson::UTF8<>, rapidjson::CrtAllocator> doc;
    doc.Parse<0>(json.c_str());
    if (doc.HasParseError()) {
       throw std::runtime_error("Error parsing model file");
    }
    name = doc["name"].GetString();
    const auto& dataArrays = doc["dataArrays"].GetObject();
    const auto& positionArray = dataArrays["position"].GetArray();
    const auto& normalArray = dataArrays["normal"].GetArray();
    const auto& meshes = doc["meshes"].GetArray();
    // assuming there's only one mesh
    const auto& indices = meshes[0]["indices"].GetArray();
    if (positionArray.Size() != normalArray.Size()) {
      throw std::runtime_error("There should be as many normals as vertex positions -- the data should be interleaved.");
    }
    size_t vertexCount = positionArray.Size() / 3;
    vertexData = std::make_unique<GLfloat[]>(6 * vertexCount);
    vertexByteCount = sizeof(GLfloat) * 6 * vertexCount;
    // interleave the data
    for (size_t i = 0; i < vertexCount; i++) {
      vertexData[6 * i] = positionArray[3 * i].GetFloat();
      vertexData[6 * i + 1] = positionArray[3 * i + 1].GetFloat();
      vertexData[6 * i + 2] = positionArray[3 * i + 2].GetFloat();
      vertexData[6 * i + 3] = normalArray[3 * i].GetFloat();
      vertexData[6 * i + 4] = normalArray[3 * i + 1].GetFloat();
      vertexData[6 * i + 5] = normalArray[3 * i + 2].GetFloat();
    }
    indexCount = indices.Size();
    indexByteCount = indexCount * sizeof(GLuint);
    faces = std::make_unique<GLuint[]>(indices.Size());
    for (size_t i = 0; i < indices.Size(); i++) {
      faces[i] = indices[i].GetUint();
    }
  }
  const std::string& getName() const {
    return name;
  }
  GLsizei getVertexByteCount() const {
    return vertexByteCount;
  }
  GLsizei getIndexByteCount() const {
    return indexByteCount;
  }
  GLsizei getIndexCount() const {
    return indexCount;
  }
  const GLfloat* getVertexData() const {
    return vertexData.get();
  }
  const GLuint* getFaces() const {
    return faces.get();
  }
  private:
  std::string name;
  GLsizei vertexByteCount;
  GLsizei indexByteCount;
  GLsizei indexCount;
  std::unique_ptr<GLfloat[]> vertexData;
  std::unique_ptr<GLuint[]> faces;
};
}} // mbgl::style


Model3DLayer::Model3DLayer(const std::vector<ModelDescriptor>& modelList)
: modelList(std::move(modelList))
, models()
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
  a_position = MBGL_CHECK_ERROR(glGetAttribLocation(program, "position"));
  a_normal = MBGL_CHECK_ERROR(glGetAttribLocation(program, "normal"));
  u_projectionMatrix = MBGL_CHECK_ERROR(glGetUniformLocation(program, "projectionMatrix"));
  u_modelMatrix = MBGL_CHECK_ERROR(glGetUniformLocation(program, "modelMatrix"));
  stride = 6 * sizeof(GLfloat); // position + normal
  GLuint buffers[2];
  MBGL_CHECK_ERROR(glGenBuffers(2, buffers));
  vertexBuffer = buffers[0];
  indexBuffer = buffers[1];
}

std::shared_ptr<Model3D> Model3DLayer::getCachedModel(const std::string& url) {
  auto it = models.find(url);
  if (it == models.end()) {
    // the first time the model becomes visible, we load it
    std::string json = readResource(url);
    if (json.empty()) {
      std::cerr << "Empty JSON: " << url << std::endl;
      return nullptr;
    }
    auto itm = models.emplace(url, std::make_shared<Model3D>(json));
    if (itm.second) {
      return itm.first->second;
    }
    std::cerr << "Insertion failed: " << url << std::endl;
    return nullptr;
  }
  // the model was read already
  return it->second;
}
  
void Model3DLayer::render(const 
mbgl::style::CustomLayerRenderParameters& param) {
  GLfloat pmatrix[16];
  for (int i = 0; i < 16; i++) {
    pmatrix[i] = static_cast<GLfloat>(param.projectionMatrix[i]);
  }
  double worldSize = Projection::worldSize(std::pow(2, param.zoom));
  MBGL_CHECK_ERROR(glUseProgram(program));
  MBGL_CHECK_ERROR(glEnable(GL_DEPTH_TEST));
  MBGL_CHECK_ERROR(glDepthMask(true));
  MBGL_CHECK_ERROR(glDepthFunc(GL_LESS));
  MBGL_CHECK_ERROR(glEnable(GL_CULL_FACE));
  // CCW for xzy order (CW if it were xyz)
  MBGL_CHECK_ERROR(glFrontFace(GL_CCW));
  MBGL_CHECK_ERROR(glDisable(GL_STENCIL_TEST));
  MBGL_CHECK_ERROR(glDisable(GL_BLEND));
  MBGL_CHECK_ERROR(glUniformMatrix4fv(u_projectionMatrix, 1, false, pmatrix));
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
    auto model = getCachedModel(m.url);
    if (model == nullptr) {
      continue;
    }
    double mpp = Projection::getMetersPerPixelAtLatitude(ll.latitude(), param.zoom);
    double meterInMercatorUnits = 1.0 / mpp;
    vec3 s({m.scale[0] * meterInMercatorUnits, m.scale[1] * meterInMercatorUnits, m.scale[2] * meterInMercatorUnits});
    GLfloat modelMatrix[] = {
      static_cast<GLfloat>(s[0]), 0, 0, 0,
      0, static_cast<GLfloat>(s[1]), 0, 0,
      0, 0, static_cast<GLfloat>(s[2]), 0,
      static_cast<GLfloat>(p[0]),
      static_cast<GLfloat>(p[1]),
      static_cast<GLfloat>(p[2]),
      1
    };
    MBGL_CHECK_ERROR(glUniformMatrix4fv(u_modelMatrix, 1, false, modelMatrix));
    MBGL_CHECK_ERROR(glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer));
    MBGL_CHECK_ERROR(glBufferData(GL_ARRAY_BUFFER, model->getVertexByteCount(), model->getVertexData(), GL_STATIC_DRAW));
    MBGL_CHECK_ERROR(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer));
    MBGL_CHECK_ERROR(glBufferData(GL_ELEMENT_ARRAY_BUFFER, model->getIndexByteCount(), model->getFaces(), GL_STATIC_DRAW));
    // unsure why I have to setup the attributes after binding the buffers...
    // if I set them before binding, nothing is drawn...
    MBGL_CHECK_ERROR(glEnableVertexAttribArray(a_position));
    MBGL_CHECK_ERROR(glEnableVertexAttribArray(a_normal));
    MBGL_CHECK_ERROR(glVertexAttribPointer(a_position, 3, GL_FLOAT, GL_FALSE, stride, nullptr));
    MBGL_CHECK_ERROR(glVertexAttribPointer(a_normal, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(GLfloat))));
      // draw
    MBGL_CHECK_ERROR(glDrawElements(GL_TRIANGLES, model->getIndexCount(), GL_UNSIGNED_INT, nullptr));
  }
}

void Model3DLayer::contextLost() {

}

void Model3DLayer::deinitialize() {
  if (program) {
    MBGL_CHECK_ERROR(glDeleteBuffers(1, &vertexBuffer));
    MBGL_CHECK_ERROR(glDeleteBuffers(1, &indexBuffer));
    MBGL_CHECK_ERROR(glDetachShader(program, vertexShader));
    MBGL_CHECK_ERROR(glDetachShader(program, fragmentShader));
    MBGL_CHECK_ERROR(glDeleteShader(vertexShader));
    MBGL_CHECK_ERROR(glDeleteShader(fragmentShader));
    MBGL_CHECK_ERROR(glDeleteProgram(program));
    }
}
