<?xml version="1.0"?>
<xsl:stylesheet version="1.0"
xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
<xsl:template match="/registry">
  <xsl:for-each select="commands/command">
    <xsl:variable name="surface" select="param[type = 'VkSurfaceKHR'][not(contains(text(), '*'))]"/>
    <xsl:variable name="surfaceInfo" select="param[type = 'VkPhysicalDeviceSurfaceInfo2KHR']"/>
    <xsl:variable name="dev" select="param[type = 'VkPhysicalDevice']"/>
    <xsl:if test="($surface/text() != '' or $surfaceInfo/text() != '') and $dev/text() != ''">
VK_LAYER_EXPORT <xsl:value-of select="proto/type"/> VKAPI_CALL PrimusVK_<xsl:value-of select="substring(proto/name,3)"/>(
<xsl:for-each select="param">
  <xsl:text>    </xsl:text><xsl:value-of select="."/>
  <xsl:if test="./following-sibling::param/text() != ''">,<xsl:text>
</xsl:text></xsl:if>
</xsl:for-each>) {
  VkPhysicalDevice phy = instance_info[GetKey(<xsl:value-of select="$dev/name"/>)].display;
  return instance_dispatch[GetKey(phy)].<xsl:value-of select="substring(proto/name,3)"/>(phy<xsl:for-each select="param[type != 'VkPhysicalDevice']">, <xsl:value-of select="name"/></xsl:for-each>);
}	    
    </xsl:if>
  </xsl:for-each>
</xsl:template>
</xsl:stylesheet> 
