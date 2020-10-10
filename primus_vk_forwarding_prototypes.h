VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceCapabilitiesKHR(phy, surface, pSurfaceCapabilities);
}	    
    VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  auto result = instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, pSurfaceFormatCount, pSurfaceFormats);
  TRACE("Querying surface formats (base) returned: " << result);
  if( result == VK_SUCCESS && pSurfaceFormats) {
    TRACE("Querying surface formats: " << *pSurfaceFormatCount);
    for(uint32_t i = 0; i < *pSurfaceFormatCount; i++){
      TRACE("Result: " << pSurfaceFormats[i].format << ";" << pSurfaceFormats[i].colorSpace);
    }
  }
  return result;
}	    
    VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, pPresentModeCount, pPresentModes);
}	    
    VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceCapabilities2EXT(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilities2EXT* pSurfaceCapabilities) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceCapabilities2EXT(phy, surface, pSurfaceCapabilities);
}	    
    VkResult VKAPI_CALL PrimusVK_GetPhysicalDevicePresentRectanglesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pRectCount,
    VkRect2D* pRects) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDevicePresentRectanglesKHR(phy, surface, pRectCount, pRects);
}	    
    VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceCapabilities2KHR(phy, pSurfaceInfo, pSurfaceCapabilities);
}	    
    VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormat2KHR* pSurfaceFormats) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  auto result = instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceFormats2KHR(phy, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
  TRACE("Querying surface formats returned: " << result);
  if( result == VK_SUCCESS && pSurfaceFormats) {
    TRACE("Querying surface formats: " << *pSurfaceFormatCount);
    for(uint32_t i = 0; i < *pSurfaceFormatCount; i++){
      TRACE("Result: " << pSurfaceFormats[i].surfaceFormat.format << ";" << pSurfaceFormats[i].surfaceFormat.colorSpace);
    }
  }
  return result;
}	    
    
