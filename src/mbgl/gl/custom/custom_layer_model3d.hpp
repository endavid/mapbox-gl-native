#pragma once

#include <mbgl/gl/custom_layer.hpp>
#include <mbgl/gl/defines.hpp>
#include <mbgl/platform/gl_functions.hpp>

using namespace mbgl::platform;

namespace mbgl {
namespace style {
class Model3DLayer : public mbgl::style::CustomLayerHost {
public:
  Model3DLayer();
  void initialize() override;
  void render(const mbgl::style::CustomLayerRenderParameters& param) override;
  void contextLost() override;
  void deinitialize() override;
private:
    GLuint program = 0;
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    GLuint buffer = 0;
    GLuint a_pos = 0;
};


}
}