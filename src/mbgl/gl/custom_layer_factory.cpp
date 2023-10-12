#include <mbgl/gl/custom_layer.hpp>
#include <mbgl/gl/custom_layer_factory.hpp>
#include <mbgl/gl/custom_layer_impl.hpp>
#include <mbgl/gl/render_custom_layer.hpp>
#include <mbgl/style/conversion_impl.hpp>
#include <mbgl/gl/custom/custom_layer_model3d.hpp>
#include <iostream>


namespace mbgl {

vec3 vec3OrZero(const style::conversion::Convertible& v, const std::string& member) {
  vec3 p({0, 0, 0});
  auto value = objectMember(v, member.c_str());
  if (!value) {
      return p;
  }
  if (!isArray(*value)) {
    return p;
  }
  if (arrayLength(*value) != 3) {
    return p;
  }
  for (std::size_t i = 0; i <3; ++i) {
    const auto& m = arrayMember(*value, i);
    optional<double> d = toDouble(m);
    p[i] = d.value_or(0);
  }
  return p;
}

std::string memberOrDefault(const style::conversion::Convertible& v, const std::string& member, const std::string& defo) {
  auto value = objectMember(v, member.c_str());
  if (!value) {
      return defo;
  }
  optional<std::string> s = toString(*value);
  if (!s) {
      return defo;
  }
  return s.value_or(defo);
}

std::vector<style::ModelDescriptor> toModelDescriptions(const style::conversion::Convertible& v) {
  std::vector<style::ModelDescriptor> models;
  if (!isArray(v)) {
    std::cerr << "'models' should be an array" << std::endl;
    return models;
  }
  if (arrayLength(v) == 0) {
    std::cerr << "There should be at least one model" << std::endl;
    return models;
  }
  for (std::size_t i = 0; i < arrayLength(v); ++i) {
    const auto& m = arrayMember(v, i);
    if (!isObject(m)) {
      std::cerr << "model " << i << " should be an object" << std::endl;
      continue;
    }
    auto id = memberOrDefault(m, "id", "");
    auto position = vec3OrZero(m, "position");
    auto scale = vec3OrZero(m, "scale");
    auto url = memberOrDefault(m, "url", "");
    models.push_back({id, position, scale, url});
  }
  return models;
}

const style::LayerTypeInfo* CustomLayerFactory::getTypeInfo() const noexcept {
    return style::CustomLayer::Impl::staticTypeInfo();
}

std::unique_ptr<style::Layer> CustomLayerFactory::createLayer(const std::string& id, const style::conversion::Convertible& value) noexcept {
  auto name = memberOrDefault(value, "customLayer", "");
  if (name == "model3d") {
    auto modelsValue = objectMember(value, "models");
    if (!modelsValue) {
      std::cerr << "There are no models" << std::endl;
      return nullptr;
    }
    auto models = toModelDescriptions(*modelsValue);
    if (models.size() == 0) {
      return nullptr;
    }
    return std::make_unique<style::CustomLayer>(
      id,
      std::make_unique<style::Model3DLayer>(models));
  }
  std::cerr << "Unknown custom layer: " << name << std::endl;
  return nullptr;
}

std::unique_ptr<RenderLayer> CustomLayerFactory::createRenderLayer(Immutable<style::Layer::Impl> impl) noexcept {
    return std::make_unique<RenderCustomLayer>(staticImmutableCast<style::CustomLayer::Impl>(impl));
}

} // namespace mbgl
