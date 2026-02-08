#pragma once
#include "GameMode.h"

class ModeManual : public GameMode {
public:
    ModeManual(Target* t1, Target* t2, Target* t3);
    void start() override;
    void stop() override;
    void update() override;
    void handleInput(int targetId, String cmd) override;
};
