#include <mbgl/test/util.hpp>

#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/gl/custom_layer.hpp>
#include <mbgl/gl/defines.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/platform/gl_functions.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/style/layers/fill_layer.hpp>
#include <mbgl/style/style.hpp>
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
uniform mat4 Pmatrix;
varying vec4 vertexColor;
void main() {
    gl_Position = vec4(position.xy, 1, 1);
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
        stride = 6 * sizeof(GLfloat); // position + normal
        GLuint buffers[2];
        MBGL_CHECK_ERROR(glGenBuffers(2, buffers));
        vertexBuffer = buffers[0];
        indexBuffer = buffers[1];
    }

    void render(const mbgl::style::CustomLayerRenderParameters&) override {
        GLfloat vertexData[] = {
          -0.5, -0.5, 0.5, 0, 0, 1.0,
          -0.5, 0.5, 0.5, 0, 0, 1.0,
          0.5, -0.5, 0.5, 0, 0, 1.0,
          0.5, 0.5, 0.5, 0, 0, 1.0
        };
        GLuint faces[] = {0, 1, 2, 1, 2, 3};
        GLsizei indexCount = sizeof(faces) / sizeof(GLuint);
        MBGL_CHECK_ERROR(glUseProgram(program));
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
};

TEST(CustomLayer, Object) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }

    util::RunLoop loop;

    HeadlessFrontend frontend { 1 };
    Map map(frontend, MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
            ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
    map.getStyle().loadJSON(util::read_file("test/fixtures/api/water.json"));
    map.jumpTo(CameraOptions().withCenter(LatLng { 37.8, -122.5 }).withZoom(10.0));
    map.getStyle().addLayer(std::make_unique<CustomLayer>(
        "custom",
        std::make_unique<Test3DLayer>()));

    auto layer = std::make_unique<FillLayer>("landcover", "mapbox");
    layer->setSourceLayer("landcover");
    layer->setFillColor(Color{ 1.0, 1.0, 0.0, 1.0 });
    map.getStyle().addLayer(std::move(layer));

    test::checkImage("test/fixtures/custom_layer/3d", frontend.render(map).image, 0.0006, 0.1);
}
