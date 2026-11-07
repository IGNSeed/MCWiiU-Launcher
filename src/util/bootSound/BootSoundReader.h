#pragma once
#include "Cafe/Filesystem/fsc.h"

class BootSoundReader
{
  public:
	BootSoundReader() = delete;
	BootSoundReader(FSCVirtualFile* bootsndFile, sint32 blockSize);

	sint16* getSamples();

  private:
	FSCVirtualFile* bootsndFile{};
	sint32 blockSize{};
	bool valid = false;

	uint32be muteBits{};
	uint32be loopPoint{};
	std::vector<sint16> buffer{};
	std::vector<sint16be> bufferBE{};
};
