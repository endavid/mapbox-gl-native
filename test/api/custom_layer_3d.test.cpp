#include <mbgl/test/util.hpp>

#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/gl/custom_layer.hpp>
#include <mbgl/gl/defines.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/map/transform.hpp>
#include <mbgl/platform/gl_functions.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/style/layers/fill_layer.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/util/camera.hpp>
#include <mbgl/util/io.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/run_loop.hpp>

using namespace mbgl;
using namespace mbgl::style;
using namespace mbgl::platform;


// Note that custom layers need to draw geometry with a z value of 1 to take advantage of
// depth-based fragment culling.
static const GLchar* vertexShaderSource = R"MBGL_SHADER(
attribute vec3 position;
attribute vec3 normal;
uniform mat4 modelMatrix;
uniform mat4 projectionMatrix;
varying vec4 vertexColor;
void main() {
    vec4 worldPos = modelMatrix * vec4(position, 1.0);
    vec4 projected = projectionMatrix * worldPos;
    gl_Position = projected;
    //gl_Position = vec4(worldPos.xy, 1, 1);
    vertexColor = vec4(0.5 * normal + 0.5, 1);
}
)MBGL_SHADER";

static const GLchar* fragmentShaderSource = R"MBGL_SHADER(
varying vec4 vertexColor;
void main() {
    gl_FragColor = vec4(vertexColor.rgb, 0.8);
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

class Test3DLayer : public mbgl::style::CustomLayerHost {
public:
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
        GLfloat vertexData[] = {
          -0.5, -0.5, 0.5, 0, 0, 1.0,
          -0.5, 0.5, 0.5, 0, 0, 1.0,
          0.5, -0.5, 0.5, 0, 0, 1.0,
          0.5, 0.5, 0.5, 0, 0, 1.0,
          -0.5, -0.5, -0.5, -1.0, 0, 0,
          -0.5, -0.5, 0.5, -1.0, 0, 0,
          -0.5, 0.5, -0.5, -1.0, 0, 0,
          -0.5, 0.5, 0.5, -1.0, 0, 0,
          0.5, -0.5, -0.5, 1.0, 0, 0,
          0.5, -0.5, 0.5, 1.0, 0, 0,
          0.5, 0.5, -0.5, 1.0, 0, 0,
          0.5, 0.5, 0.5, 1.0, 0, 0,
          -0.5, 0.5, -0.5, 0, 1.0, 0,
          -0.5, 0.5, 0.5, 0, 1.0, 0,
          0.5, 0.5, -0.5, 0, 1.0, 0,
          0.5, 0.5, 0.5, 0, 1.0, 0
        };
        GLuint faces[] = {0, 1, 2, 1, 2, 3, 4, 5, 6, 5, 6, 7, 8, 9, 10, 9, 10, 11, 12, 13, 14, 13, 14, 15};
        GLsizei indexCount = sizeof(faces) / sizeof(GLuint);
        // convert the double precision matrix to GLfloats
        // it's called projection, but I think it's the ProjView (P*V) matrix, because
        // the last column appears translated: [-521467, 347073, 67456.5, 67178.1]
        GLfloat pmatrix[16];
        for (int i = 0; i < 16; i++) {
          pmatrix[i] = static_cast<GLfloat>(param.projectionMatrix[i]);
        }
        dumpMatrix("projectionMatrix", pmatrix);
        GLfloat scale[] {1, 1, 10};
        GLfloat mmatrix[] = {
          scale[0], 0, 0, 0,
          0, scale[1], 0, 0,
          0, 0, scale[2], 0,
          37.8, -122.5, 0, 1
        };
        dumpMatrix("model matrix", mmatrix);
        MBGL_CHECK_ERROR(glUseProgram(program));
        MBGL_CHECK_ERROR(glEnable(GL_DEPTH_TEST));
        MBGL_CHECK_ERROR(glDisable(GL_CULL_FACE)); // for the time being, render back faces as well
        MBGL_CHECK_ERROR(glDisable(GL_STENCIL_TEST));
        MBGL_CHECK_ERROR(glDisable(GL_BLEND));
        MBGL_CHECK_ERROR(glUniformMatrix4fv(u_projectionMatrix, 1, false, pmatrix));
        MBGL_CHECK_ERROR(glUniformMatrix4fv(u_modelMatrix, 1, false, mmatrix));
        // and here we could loop if we had more meshes
        MBGL_CHECK_ERROR(glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer));
        std::cout << "vertexDataSize: " << sizeof(vertexData) << ", indexBuffer: " << sizeof(faces) << ", indexCount: " << indexCount << std::endl;
        MBGL_CHECK_ERROR(glBufferData(GL_ARRAY_BUFFER, sizeof(vertexData), vertexData, GL_STATIC_DRAW));
        MBGL_CHECK_ERROR(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer));
        MBGL_CHECK_ERROR(glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(faces), faces, GL_STATIC_DRAW));
        // unsure why I have to setup the attributes after binding the buffers...
        // if I set them before binding, nothing is drawn...
        MBGL_CHECK_ERROR(glEnableVertexAttribArray(a_position));
        MBGL_CHECK_ERROR(glEnableVertexAttribArray(a_normal));
        MBGL_CHECK_ERROR(glVertexAttribPointer(a_position, 3, GL_FLOAT, GL_FALSE, stride, nullptr));
        MBGL_CHECK_ERROR(glVertexAttribPointer(a_normal, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(GLfloat))));
        // draw
        MBGL_CHECK_ERROR(glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr));
        debugDepthBuffer();
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

    HeadlessFrontend frontend { 1 };
    auto size = frontend.getSize();
    // 256x256
    std::cout << "size: " << size.width << "x" << size.height << std::endl;
    Map map(frontend, MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(size),
            ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
    map.getStyle().loadJSON(util::read_file("test/fixtures/api/water.json"));
    LatLng ll(37.8, -122.5);
    auto cam = CameraOptions().withCenter(ll).withZoom(10.0).withPitch(30).withBearing(30);

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
    std::cout << "cartesian: " << cartesian[0] << ", " << cartesian[1] << ", " << cartesian[2] << std::endl;
    //auto merca = util::toMercator(ll, 10.0);
    //std::cout << "mercator: " << merca[0] << ", " << merca[1] << ", " << merca[2] << std::endl;
    //  0.159722, 0.382893, 3.20185e-07

    map.getStyle().addLayer(std::make_unique<CustomLayer>(
        "custom",
        std::make_unique<Test3DLayer>()));

    auto layer = std::make_unique<FillLayer>("landcover", "mapbox");
    layer->setSourceLayer("landcover");
    layer->setFillColor(Color{ 1.0, 1.0, 0.0, 1.0 });
    map.getStyle().addLayer(std::move(layer));

    test::checkImage("test/fixtures/custom_layer/3d", frontend.render(map).image, 0.0006, 0.1);
}
