# The app's entry points (setup/loop) live in src/main.cpp, built by comet_app; arduino-esp32
# provides app_main (CONFIG_AUTOSTART_ARDUINO). This component only anchors the project.
idf_component_register(SRCS "comet_main.c" PRIV_REQUIRES comet_app)
