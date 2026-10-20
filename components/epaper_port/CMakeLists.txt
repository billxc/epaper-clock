idf_component_register(
  SRCS "epaper_port.c" "frame_codec.c"
  PRIV_REQUIRES driver axpPower
  INCLUDE_DIRS "./")
