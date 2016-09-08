#pragma once

#include "communication/bolt/v1/states/state.hpp"

namespace bolt
{

class Handshake : public State
{
public:
    Handshake();
    State* run(Session& session) override;
};

}
