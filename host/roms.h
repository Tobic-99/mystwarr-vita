// Loading the Mystic Warriors ROM set (MAME "mystwarr" file names) into the board, video and sound.
#pragma once
#include <string>

bool load_roms(const std::string& dir);   // false (with a message) if a file is missing
