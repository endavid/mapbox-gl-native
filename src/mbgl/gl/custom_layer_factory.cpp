#include <mbgl/gl/custom_layer.hpp>
#include <mbgl/gl/custom_layer_factory.hpp>
#include <mbgl/gl/custom_layer_impl.hpp>
#include <mbgl/gl/render_custom_layer.hpp>
#include <mbgl/style/conversion_impl.hpp>
#include <mbgl/gl/custom/custom_layer_model3d.hpp>
#include <iostream>

namespace mbgl {

const style::LayerTypeInfo* CustomLayerFactory::getTypeInfo() const noexcept {
    return style::CustomLayer::Impl::staticTypeInfo();
}

std::unique_ptr<style::Layer> CustomLayerFactory::createLayer(const std::string& id, const style::conversion::Convertible& value) noexcept {
    auto layerValue = objectMember(value, "customLayer");
    if (!layerValue) {
        return nullptr;
    }
    optional<std::string> layer = toString(*layerValue);
    if (!layer) {
        return nullptr;
    }
    auto name = layer.value_or("");
    if (name == "model3d") {
      return std::make_unique<style::CustomLayer>(
        id,
        std::make_unique<style::Model3DLayer>());
    }
    std::cerr << "Unknown custom layer: " << name << std::endl;
    return nullptr;
}

std::unique_ptr<RenderLayer> CustomLayerFactory::createRenderLayer(Immutable<style::Layer::Impl> impl) noexcept {
    return std::make_unique<RenderCustomLayer>(staticImmutableCast<style::CustomLayer::Impl>(impl));
}

} // namespace mbgl
