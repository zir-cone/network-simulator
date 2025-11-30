#pragma once
#include "Device.hpp"
#include <iostream>
#include <random>

class IoTDevice : public Device
{
public:
    IoTDevice(int id, NetworkScope scope)
        : Device(id, scope),
        rng_(std::random_device{}()),
        distInterval_(0.5, 2.0)
    {
        scheduleNextSend(0.0);
    }
    void tick(double now) override 
    {
        if (now >= nextSendTime_) {
            // just log a packet was sent
            Packet pkt{};
            pkt.id          = nextPacketId_++;
            pkt.srcNodeId   = id_;
            pkt.dstNodeId   = -1; // unused
            pkt.sizeBytes   = 128;
            pkt.createdAt   = now;

            std::cout << "IoTDevice " << id_
                      << "] sending packet " << pkt.id
                      << " at t=" << now << "s\n";
            // eventually pass into the simulation
            scheduleNextSend(now);
        }
    }
    void onPacketReceived(const Packet& pkt) override
    {
        std::cout << "[IoTDevice " << id_ << "] received packet "
                  << pkt.id << " from " << pkt.srcNodeId << "\n";
    }
private:
    void scheduleNextSend(double now)
    {
        double interval = distInterval_(rng_);
        nextSendTime_ = now + interval;
    }
    double nextSendTime_ = 0.0;
    std::uint64_t nextPacketId_ = 0;

    std::mt19937 rng_;
    std::uniform_real_distribution<double> distInterval_;

};