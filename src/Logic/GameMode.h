#pragma once
#include <Arduino.h>
#include "../Hardware/Target.h"

class GameMode {
public:
    GameMode(Target* t1, Target* t2, Target* t3) : _t1(t1), _t2(t2), _t3(t3) {}
    virtual ~GameMode() {}
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void update() = 0;
    virtual void handleInput(int targetId, String cmd) = 0;

protected:
    Target* _t1;
    Target* _t2;
    Target* _t3;
};
