idf_component_register(
  SRCS "axp_prot.cpp" "src/XPowersLibInterface.cpp"
  INCLUDE_DIRS "."
  PRIV_INCLUDE_DIRS "src"
  PRIV_REQUIRES i2c_bsp driver
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_XPOWERS_ESP_IDF_NEW_API)
