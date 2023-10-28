#pragma once

#include <mbgl/gl/custom_layer.hpp>
#include <mbgl/gl/defines.hpp>
#include <mbgl/platform/gl_functions.hpp>
#include <mbgl/util/vectors.hpp>
#include <map>

using namespace mbgl::platform;

namespace mbgl {
namespace style {

class Model3D;

struct ModelDescriptor {
  std::string id;
  vec3 position;
  vec3 scale;
  std::string url;
};

class Model3DLayer : public mbgl::style::CustomLayerHost {
public:
  Model3DLayer(const std::vector<ModelDescriptor>& modelList);
  void initialize() override;
  void render(const mbgl::style::CustomLayerRenderParameters& param) override;
  void contextLost() override;
  void deinitialize() override;
private:
  std::shared_ptr<Model3D> getCachedModel(const std::string& url);

  std::vector<ModelDescriptor> modelList;
  std::map<std::string, std::shared_ptr<Model3D> > models;
  GLuint program = 0;
  GLuint vertexShader = 0;
  GLuint fragmentShader = 0;
  GLuint vertexBuffer = 0;
  GLuint indexBuffer = 0;
  GLuint a_position = 0;
  GLuint a_normal = 0;
  GLsizei stride = 0;
  GLint u_mvpMatrix = 0;
};


}
}