#include "version.h"

const char* version::GetGitSha()
{
    return "ed0cd72f2b-dirty";
}

const char* version::GetGitBranch()
{
    return "cardian-player-gambits";
}

const char* version::GetGitDate()
{
    return "Sat Oct 3 20:30:06 2026";
}

const char* version::GetGitCommitSubject()
{
    return "Player gambits, step B: his marked rows run the tactician, as a cardian's";
}

const char* version::GetVersionString()
{
    return "cardian-player-gambits (ed0cd72f2b-dirty)";
}
