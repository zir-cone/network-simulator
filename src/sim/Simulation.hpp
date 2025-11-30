#pragma once
#include "Network.hpp"

class Simulation 
{
public:
    explicit Simulation(Network& net)
        : network_(net) {}
    void step(double dt) 
    {
        currentTime_ += dt;
        // let devices think
        for (auto& dev : network_.devices()) {
            dev->tick(currentTime_);
        }
        // move packets along links
        network_.updatePackets(dt);
    }
    double time() const { return currentTime_; }
private:
    Network& network_;
    double currentTime_ = 0.0;
};
