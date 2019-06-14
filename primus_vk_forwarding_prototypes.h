
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceSupportKHR(phy, queueFamilyIndex, surface, pSupported);
}	    
    
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceCapabilitiesKHR(phy, surface, pSurfaceCapabilities);
}	    
    
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, pSurfaceFormatCount, pSurfaceFormats);
}	    
    
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, pPresentModeCount, pPresentModes);
}	    
    
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceCapabilities2EXT(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilities2EXT* pSurfaceCapabilities) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceCapabilities2EXT(phy, surface, pSurfaceCapabilities);
}	    
    
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDevicePresentRectanglesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pRectCount,
    VkRect2D* pRects) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDevicePresentRectanglesKHR(phy, surface, pRectCount, pRects);
}	    
    
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceCapabilities2KHR(phy, pSurfaceInfo, pSurfaceCapabilities);
}	    
    
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormat2KHR* pSurfaceFormats) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceFormats2KHR(phy, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
}	    
    
