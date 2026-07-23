#ifndef VULKAN_INIT_H
#define VULKAN_INIT_H

#include "graph_core/vulkan_core.h"

FeatureCheckStatus check_gpu_features(VkPhysicalDevice gpu, const struct EngineRequestedFeatures *requested);
bool create_instance(VkInstance *out_instance);
bool create_wayland_surface(struct Application *app);
bool select_gpu_and_verify(struct Application *app, const struct EngineRequestedFeatures *req_features);
bool create_device(struct Application *app, const struct EngineRequestedFeatures *req_features);

bool application_init(struct Application *app, const struct EngineRequestedFeatures *req_features);

#endif // VULKAN_INIT_H
