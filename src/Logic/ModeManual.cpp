#include "ModeManual.h"

ModeManual::ModeManual(Target* t1, Target* t2, Target* t3) : GameMode(t1, t2, t3) {
}

void ModeManual::start() {
    // Manual mode doesn't need specific start logic
}

void ModeManual::stop() {
    _t1->stop();
    _t2->stop();
    _t3->stop();
}

void ModeManual::update() {
    // Manual mode is reactive, but targets need to update their state
    _t1->update();
    _t2->update();
    _t3->update();
}

void ModeManual::handleInput(int targetId, String cmd) {
    // targetId 0 is the broadcast address used by the UI's global buttons.
    if (targetId == 0) {
        if (cmd == "stop") stop();
        else if (cmd == "reset") { _t1->reset(); _t2->reset(); _t3->reset(); }
        return;
    }

    Target* target = nullptr;
    if (targetId == 1) target = _t1;
    else if (targetId == 2) target = _t2;
    else if (targetId == 3) target = _t3;

    if (target) {
        if (cmd == "show") target->show();
        else if (cmd == "hide") target->hide();
        else if (cmd == "stop") target->stop();
        else if (cmd == "reset") target->reset();
    }
}
