#include "GVerbWidget.hpp"

#include "../include/SongRoll/SongRollModule.hpp"
#include "../include/SongRoll/SongRollWidget.hpp"

using namespace SongRoll;

// Specify the Module and ModuleWidget subclass, human-readable
// author name for categorization per plugin, module slug (should never
// change), human-readable module name, and any number of tags
// (found in `include/tags.hpp`) separated by commas.
Model *modelSongRollModule = Model::create<SongRollModule, SongRollWidget>("rcm", "rcm-songroll", "Song Roll", SEQUENCER_TAG);
