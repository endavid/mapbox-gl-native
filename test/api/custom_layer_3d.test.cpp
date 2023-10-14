#include <mbgl/test/util.hpp>

#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/gl/custom_layer.hpp>
#include <mbgl/gl/defines.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/map/transform.hpp>
#include <mbgl/platform/gl_functions.hpp>
#include <mbgl/storage/file_source.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/style/layers/fill_layer.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/util/camera.hpp>
#include <mbgl/util/io.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/run_loop.hpp>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

using namespace mbgl;
using namespace mbgl::style;
using namespace mbgl::platform;

namespace {
// Note that custom layers need to draw geometry with a z value of 1 to take advantage of
// depth-based fragment culling.
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

// Not using any mbgl-specific stuff (other than a basic error-checking macro) in the
// layer implementation because it is intended to reflect how someone using custom layers
// might actually write their own implementation.

void dumpMatrix(const std::string& name, const GLfloat* m) {
  std::cout << name << ": " << std::endl;
  std::cout << m[0] << ", " << m[1] << ", " << m[2] << ", " << m[3] << std::endl;
  std::cout << m[4] << ", " << m[5] << ", " << m[6] << ", " << m[7] << std::endl;
  std::cout << m[8] << ", " << m[9] << ", " << m[10] << ", " << m[11] << std::endl;
  std::cout << m[12] << ", " << m[13] << ", " << m[14] << ", " << m[15] << std::endl;
}

void debugDepthBuffer() {
  GLfloat depthBuffer[256 * 256];
  MBGL_CHECK_ERROR(glReadPixels(0, 0, 256, 256, GL_DEPTH_COMPONENT, GL_FLOAT, depthBuffer));
  float sum = 0;
  for (int i = 0; i < 256 * 256; i++) sum += depthBuffer[i];
  sum /= 256 * 256;
  std::cout << "depth buffer avg: " << sum << std::endl;
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

} // anonymous namespace

template<typename T, size_t N>
std::ostream& operator<<(std::ostream& os, const std::array<T, N>& arr) {
    os << "[ ";
    for(const auto& element : arr) {
        os << element << ", ";
    }
    os << "]";
    return os;
}

template<typename T>
std::unique_ptr<T[]> make_unique_array(std::initializer_list<T> values) {
    auto result = std::make_unique<T[]>(values.size());
    std::copy(values.begin(), values.end(), result.get());
    return result;
}

class Model3D {
  public:
  Model3D(const std::string& json)
  : name("")
  , position({0, 0, 0})
  , scale({0, 0, 0})
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
    const auto& pos = doc["position"].GetArray();
    const auto& s = doc["scale"].GetArray();
    const auto& dataArrays = doc["dataArrays"].GetObject();
    const auto& positionArray = dataArrays["position"].GetArray();
    const auto& normalArray = dataArrays["normal"].GetArray();
    const auto& meshes = doc["meshes"].GetArray();
    // assuming there's only one mesh
    const auto& indices = meshes[0]["indices"].GetArray();
    if (positionArray.Size() != normalArray.Size()) {
      throw std::runtime_error("There should be as many normals as vertex positions -- the data should be interleaved.");
    }
    for (size_t i = 0; i < 3; i++) {
      position[i] = pos[i].GetDouble();
      scale[i] = s[i].GetDouble();
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
  const vec3& getScale() const {
    return scale;
  }
  const vec3& getPosition() const {
    return position;
  }
  private:
  std::string name;
  vec3 position;
  vec3 scale;
  GLsizei vertexByteCount;
  GLsizei indexByteCount;
  GLsizei indexCount;
  std::unique_ptr<GLfloat[]> vertexData;
  std::unique_ptr<GLuint[]> faces;
};

typedef std::vector<std::shared_ptr<Model3D> > ModelList;

class Test3DLayer : public mbgl::style::CustomLayerHost {
public:
    Test3DLayer(const ModelList& modelList)
    : modelList(modelList)
    {
    }
    void initialize() override {
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

    void render(const mbgl::style::CustomLayerRenderParameters& param) override {
        // convert the double precision matrix to GLfloats
        // it's called projection, but I think it's the ProjView (P*V) matrix, because
        // the last column appears translated: [-521467, 347073, 67456.5, 67178.1]
        GLfloat pmatrix[16];
        for (int i = 0; i < 16; i++) {
          pmatrix[i] = static_cast<GLfloat>(param.projectionMatrix[i]);
        }
        dumpMatrix("projectionMatrix", pmatrix);
        double worldSize = Projection::worldSize(std::pow(2, param.zoom));
        std::cout << "worldSize: " << worldSize << std::endl;
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
        //demoProjectionView();
        for (const auto& model : modelList) {
          std::cout << "rendering " << model->getName() << "..." << std::endl;
          auto scale = model->getScale();
          auto position = model->getPosition();
          LatLng ll(position[0], position[1]);
          double altitude = position[2];
          double mpp = Projection::getMetersPerPixelAtLatitude(ll.latitude(), param.zoom);
          double meterInMercatorUnits = 1.0 / mpp;
          vec3 s({scale[0] * meterInMercatorUnits, scale[1] * meterInMercatorUnits, scale[2]});
          vec3 p = toMercator(ll, altitude, param.zoom);
          GLfloat modelMatrix[] = {
            static_cast<GLfloat>(s[0]), 0, 0, 0,
            0, static_cast<GLfloat>(s[1]), 0, 0,
            0, 0, static_cast<GLfloat>(s[2]), 0,
            static_cast<GLfloat>(p[0] * worldSize),
            static_cast<GLfloat>(p[1] * worldSize),
            static_cast<GLfloat>(p[2] * worldSize),
            1
          };
          dumpMatrix("model matrix", modelMatrix);
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
        debugDepthBuffer();
    }

    void demoProjectionView() {
      // P = [4.181371346338361, 0, 0, 0; 0, 2.7875808975589074, 0, 0; 0, 0, -1.0004000800160033, -1; 0, 0, -0.20004000800160032, 0]
      // V = [0.8290375725550417, -0.12579103321735163, 0.5448608255822355, 0; -5.551115123125783e-17, 0.9743700647852352, 0.22495105434386506, 0; -0.5591929034707469, -0.1864928760369351, 0.8077893932798501, 0; -8.881784197001252e-16, 0.30077041386817926, -10.193602756540082, 1]
      GLfloat pv[] = {
        3.4665e+00,  -3.5065e-01, -5.4508e-01,  -5.4486e-01,
        -2.3211e-16,  2.7161e+00,  -2.2504e-01,  -2.2495e-01,
        -2.3382e+00,  -5.1986e-01,  -8.0811e-01,  -8.0779e-01,
        -3.7138e-15,   8.3842e-01,   9.9976e+00,   1.0194e+01
      };
      MBGL_CHECK_ERROR(glUniformMatrix4fv(u_projectionMatrix, 1, false, pv));
    }

    void contextLost() override {}

    void deinitialize() override {
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

    const ModelList& modelList;
    GLuint program = 0;
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    GLuint vertexBuffer = 0;
    GLuint indexBuffer = 0;
    GLuint a_position = 0;
    GLuint a_normal = 0;
    GLsizei stride = 0;
    GLint u_projectionMatrix = 0;
    GLint u_modelMatrix = 0;
};

void AlmostEqual(double a, double b, double epsilon) {
  ASSERT_LE(std::abs(a - b), epsilon);
}

void AlmostEqual(const LatLng& a, const LatLng& b, double epsilon) {
  AlmostEqual(a.latitude(), b.latitude(), epsilon);
  AlmostEqual(a.longitude(), b.longitude(), epsilon);
}

TEST(CustomLayer, Object) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }

    util::RunLoop loop;

    ModelList modelList;
    modelList.push_back(std::make_shared<Model3D>(util::read_file("test/fixtures/resources/cube_endavid.json")));
    ASSERT_EQ("cube", modelList[0]->getName());

    HeadlessFrontend frontend { 1 };
    auto size = frontend.getSize();
    // 256x256
    std::cout << "size: " << size.width << "x" << size.height << std::endl;
    Map map(frontend, MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(size),
            ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
    map.getStyle().loadJSON(util::read_file("test/fixtures/api/water.json"));
    LatLng ll(37.8, -122.5);
    double zoom = 10;
    auto cam = CameraOptions().withCenter(ll).withZoom(zoom).withPitch(30).withBearing(30);

    // Understanding transforms
    Transform transform;
    transform.resize({1, 1});
    transform.jumpTo(cam);
    AlmostEqual(ll, transform.getLatLng(), 0.1);

    map.jumpTo(cam);

    // understanding conversions
    auto sc = map.pixelForLatLng(ll);
    ASSERT_EQ(size.width / 2, static_cast<uint32_t>(sc.x));
    ASSERT_EQ(size.height / 2, static_cast<uint32_t>(sc.y));
    auto mid_ll = map.latLngForPixel({static_cast<double>(size.width / 2), static_cast<double>(size.height / 2)});
    AlmostEqual(ll, mid_ll, 0.1);
    
    std::array<float, 3> spherical{{ 2, 37.8, -122.5 }};
    Position position(spherical);
    auto cartesian = position.getCartesian();
    std::cout << "cartesian: " << cartesian << std::endl;
    auto merca = toMercator(ll, 0.0, zoom);
    std::cout << merca << std::endl;

    map.getStyle().addLayer(std::make_unique<CustomLayer>(
        "custom",
        std::make_unique<Test3DLayer>(modelList)));

    test::checkImage("test/fixtures/custom_layer/3d", frontend.render(map).image, 0.0006, 0.1);
}
