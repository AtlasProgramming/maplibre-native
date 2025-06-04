#include "plugin_layer_bucket.hpp"
#include <iostream>

using namespace mbgl;

PluginLayerBucket::PluginLayerBucket(const BucketParameters& parameters,
                             const std::vector<Immutable<style::LayerProperties>>& layers)
    : mode(parameters.mode) {
        std::cout << "Plugin Layer Bucket Created\n";

//    for (const auto& layer : layers) {
//        paintPropertyBinders.emplace(
//            std::piecewise_construct,
//            std::forward_as_tuple(layer->baseImpl->id),
//            std::forward_as_tuple(getEvaluated<HeatmapLayerProperties>(layer), parameters.tileID.overscaledZ));
//    }
}

PluginLayerBucket::~PluginLayerBucket() {
   // sharedVertices->release();
}

void PluginLayerBucket::addFeature(const GeometryTileFeature& feature,
                               const GeometryCollection& geometry,
                               const ImagePositions&,
                               const PatternLayerMap&,
                               std::size_t featureIndex,
                                   const CanonicalTileID& canonical) {
    
    std::cout << "Plugin Layer Bucket\n";
    
    
}

void PluginLayerBucket::upload([[maybe_unused]] gfx::UploadPass& uploadPass) {
    uploaded = true;
}

bool PluginLayerBucket::hasData() const {
    return false;
   // return !segments.empty();
}

float PluginLayerBucket::getQueryRadius(const RenderLayer& layer) const {
//    (void)layer;
    return 0;
}
