<?xml version="1.0"?>
<xsl:stylesheet version="1.0"
xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
<xsl:template match="/registry">
  <xsl:for-each select="commands/command">
    <xsl:variable name="surface" select="param[type = 'VkSurfaceKHR'][not(contains(text(), '*'))]"/>
    <xsl:variable name="surfaceInfo" select="param[type = 'VkPhysicalDeviceSurfaceInfo2KHR']"/>
    <xsl:variable name="dev" select="param[type = 'VkPhysicalDevice']"/>
    <xsl:if test="($surface/text() != '' or $surfaceInfo/text() != '') and $dev/text() != ''">
      FORWARD(<xsl:value-of select="substring(proto/name,3)"/>);
    </xsl:if>
  </xsl:for-each>	
</xsl:template>
</xsl:stylesheet> 
