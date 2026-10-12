/* Compiled into Beetle VB's sources only (a forced include - see CMakeLists.txt):
 * its libretro entry points become beetle_retro_*, so the core's own retro_*
 * (libretro_core.cpp) can wrap them - color the frames, add the options. */
#ifndef VBOYCOLOR_BEETLE_RENAME_H
#define VBOYCOLOR_BEETLE_RENAME_H
#define retro_set_environment beetle_retro_set_environment
#define retro_set_video_refresh beetle_retro_set_video_refresh
#define retro_set_audio_sample beetle_retro_set_audio_sample
#define retro_set_audio_sample_batch beetle_retro_set_audio_sample_batch
#define retro_set_input_poll beetle_retro_set_input_poll
#define retro_set_input_state beetle_retro_set_input_state
#define retro_init beetle_retro_init
#define retro_deinit beetle_retro_deinit
#define retro_api_version beetle_retro_api_version
#define retro_get_system_info beetle_retro_get_system_info
#define retro_get_system_av_info beetle_retro_get_system_av_info
#define retro_set_controller_port_device beetle_retro_set_controller_port_device
#define retro_reset beetle_retro_reset
#define retro_run beetle_retro_run
#define retro_serialize_size beetle_retro_serialize_size
#define retro_serialize beetle_retro_serialize
#define retro_unserialize beetle_retro_unserialize
#define retro_cheat_reset beetle_retro_cheat_reset
#define retro_cheat_set beetle_retro_cheat_set
#define retro_load_game beetle_retro_load_game
#define retro_load_game_special beetle_retro_load_game_special
#define retro_unload_game beetle_retro_unload_game
#define retro_get_region beetle_retro_get_region
#define retro_get_memory_data beetle_retro_get_memory_data
#define retro_get_memory_size beetle_retro_get_memory_size
#endif
