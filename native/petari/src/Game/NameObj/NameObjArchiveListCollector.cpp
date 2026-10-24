#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Util/StringUtil.hpp"
#include "Game/Player/NebulaCoop.hpp"
#include <cstring>

NameObjArchiveListCollector::NameObjArchiveListCollector() : mArchiveNum() {
}

void NameObjArchiveListCollector::addArchive(const char* pName) {
    MR::copyString(mArchive[mArchiveNum], pName, ARRAY_SIZE(*mArchive));

    mArchiveNum++;
#ifdef TARGET_PC
    // Nebula's co-op: a character's power-up archive (BeeMario, IceLuigi...) brings the other character's.
    if (NebulaCoop::isAvailable()) {
        static const char* const sPowerUps[] = {"Hopper", "Bee", "Teresa", "Ice", "Invincible", "Bone"};
        for (u32 i = 0; i < ARRAY_SIZE(sPowerUps); i++) {
            const u32 length = strlen(sPowerUps[i]);
            if (strncmp(pName, sPowerUps[i], length) != 0) {
                continue;
            }
            const char* pCharacter = pName + length;
            const char* pPartner = strncmp(pCharacter, "Mario", 5) == 0 ? "Luigi" : strncmp(pCharacter, "Luigi", 5) == 0 ? "Mario" : nullptr;
            if (pPartner == nullptr || (pCharacter[5] != '\0' && pCharacter[5] != '.')) {
                return;
            }
            if (mArchiveNum >= ARRAY_SIZE(mArchive) || length + strlen(pCharacter) >= ARRAY_SIZE(*mArchive)) {
                return;
            }
            char partner[ARRAY_SIZE(*mArchive)];
            strcpy(partner, sPowerUps[i]);
            strcat(partner, pPartner);
            strcat(partner, pCharacter + 5);
            for (s32 j = 0; j < mArchiveNum; j++) {
                if (strcmp(mArchive[j], partner) == 0) {
                    return;
                }
            }
            MR::copyString(mArchive[mArchiveNum], partner, ARRAY_SIZE(*mArchive));
            mArchiveNum++;
            return;
        }
    }
#endif
}

const char* NameObjArchiveListCollector::getArchive(s32 index) const {
    return mArchive[index];
}
