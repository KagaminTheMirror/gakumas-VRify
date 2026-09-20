#pragma once
namespace gakumas::vr::camera {
    // Unity thread only; desktop Localify camera state remains independent.
    inline int& FollowActorIndex() { static int index = 0; return index; }
}
